// Reading files straight out of the APK by "<apk>/<entry>" path.
//
// Mono's assembly loader is handed a root directory of
// ".../base.apk/assets/bin/Data/Managed" and then just opens files under it. On a
// device that path is not a directory at all, so every open fails and Mono prints
// "The assembly mscorlib.dll was not found" and calls exit(1).
//
// Rather than teach every file thunk about zip layout, an entry is materialised
// into an anonymous in-memory file and its descriptor handed back. read, lseek,
// mmap, fstat and close then all work on it unchanged.

#include "apk_reader.h"

#include <android/log.h>
#include <atomic>
#include <chrono>
#include <fcntl.h>
#include <linux/memfd.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <zlib.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define LOG_TAG "CMR_ApkReader"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

struct ZipEntry {
    off_t data_offset = 0;   // where the entry's bytes start, past the local header
    uint32_t compressed = 0;
    uint32_t uncompressed = 0;
    uint16_t method = 0;     // 0 stored, 8 deflate
};

struct ZipIndex {
    int fd = -1;
    std::unordered_map<std::string, ZipEntry> entries;
    // Every directory prefix implied by an entry name. A zip stores no directory
    // records, so without this "assets/bin/Data/Managed" looks like it is missing.
    std::unordered_set<std::string> directories;
};

std::mutex g_mutex;
std::unordered_map<std::string, std::unique_ptr<ZipIndex>> g_indexes;
std::string g_primary_apk;

struct ReadDiagnostics {
    std::atomic<uint64_t> files{0};
    std::atomic<uint64_t> bytes{0};
    std::atomic<uint64_t> stored_files{0};
    std::atomic<uint64_t> inflated_files{0};
    std::atomic<uint64_t> kernel_copies{0};
    std::atomic<uint64_t> kernel_copy_fallbacks{0};
    std::atomic<uint64_t> materialize_us{0};
    std::atomic<uint64_t> first_open_us{0};
    std::atomic<uint64_t> sound_files{0};
    std::atomic<uint64_t> sound_bytes{0};
    std::atomic<uint64_t> sound_materialize_us{0};
    std::atomic<uint64_t> shader_files{0};
    std::atomic<uint64_t> shader_materialize_us{0};
};

ReadDiagnostics g_read_diagnostics;

uint64_t ElapsedMicroseconds(std::chrono::steady_clock::time_point started) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started).count());
}

uint64_t SteadyMicroseconds() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool VerboseAssetTraceEnabled() {
    // Keep the detailed file list available for one-off startup/shader audits,
    // while making the normal path emit only aggregate checkpoints. The value
    // is intentionally sampled once so a property lookup is not added per file.
    static const bool enabled = [] {
        char value[PROP_VALUE_MAX] = {};
        if (__system_property_get("debug.warband.asset_trace", value) <= 0) return false;
        return strcmp(value, "1") == 0 || strcmp(value, "true") == 0 ||
               strcmp(value, "yes") == 0 || strcmp(value, "on") == 0;
    }();
    return enabled;
}

void RecordRead(const ZipEntry& entry, const std::string& entry_name, uint64_t started_us,
                uint64_t elapsed_us, bool kernel_copy, bool kernel_copy_fallback) {
    uint64_t first_open_us = g_read_diagnostics.first_open_us.load(std::memory_order_relaxed);
    while ((first_open_us == 0 || started_us < first_open_us) &&
           !g_read_diagnostics.first_open_us.compare_exchange_weak(
               first_open_us, started_us, std::memory_order_relaxed)) {
    }
    g_read_diagnostics.bytes.fetch_add(entry.uncompressed, std::memory_order_relaxed);
    g_read_diagnostics.materialize_us.fetch_add(elapsed_us, std::memory_order_relaxed);
    if (entry.method == 0) {
        g_read_diagnostics.stored_files.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_read_diagnostics.inflated_files.fetch_add(1, std::memory_order_relaxed);
    }
    if (kernel_copy) {
        g_read_diagnostics.kernel_copies.fetch_add(1, std::memory_order_relaxed);
    }
    if (kernel_copy_fallback) {
        g_read_diagnostics.kernel_copy_fallbacks.fetch_add(1, std::memory_order_relaxed);
    }
    if (entry_name.compare(0, sizeof("assets/Sounds/") - 1, "assets/Sounds/") == 0) {
        g_read_diagnostics.sound_files.fetch_add(1, std::memory_order_relaxed);
        g_read_diagnostics.sound_bytes.fetch_add(entry.uncompressed, std::memory_order_relaxed);
        g_read_diagnostics.sound_materialize_us.fetch_add(elapsed_us, std::memory_order_relaxed);
    }
    if (entry_name.compare(0, sizeof("assets/GLShadersOptimized/") - 1,
                           "assets/GLShadersOptimized/") == 0) {
        g_read_diagnostics.shader_files.fetch_add(1, std::memory_order_relaxed);
        g_read_diagnostics.shader_materialize_us.fetch_add(elapsed_us,
                                                            std::memory_order_relaxed);
    }

    const uint64_t files =
        g_read_diagnostics.files.fetch_add(1, std::memory_order_relaxed) + 1;
    // Per-entry logging made a normal title-screen load emit roughly 1,750
    // synchronous logd writes. Keep enough checkpoints to diagnose throughput
    // without turning logging itself into part of the cold-start path.
    const bool title_checkpoint =
        entry_name == "assets/music/mount_and_blade_title_screen.ogg";
    if (files == 1 || (files % 128) == 0 || title_checkpoint) {
        first_open_us =
            g_read_diagnostics.first_open_us.load(std::memory_order_relaxed);
        LOGI("asset I/O progress: files=%llu bytes=%llu stored=%llu inflated=%llu "
             "kernel_copy=%llu fallback=%llu materialize_ms=%llu wall_ms=%llu "
             "sound_f/b/ms=%llu/%llu/%llu shader_f/ms=%llu/%llu checkpoint=%s",
             static_cast<unsigned long long>(files),
             static_cast<unsigned long long>(
                 g_read_diagnostics.bytes.load(std::memory_order_relaxed)),
             static_cast<unsigned long long>(
                 g_read_diagnostics.stored_files.load(std::memory_order_relaxed)),
             static_cast<unsigned long long>(
                 g_read_diagnostics.inflated_files.load(std::memory_order_relaxed)),
             static_cast<unsigned long long>(
                 g_read_diagnostics.kernel_copies.load(std::memory_order_relaxed)),
             static_cast<unsigned long long>(
                 g_read_diagnostics.kernel_copy_fallbacks.load(std::memory_order_relaxed)),
             static_cast<unsigned long long>(
                 g_read_diagnostics.materialize_us.load(std::memory_order_relaxed) / 1000),
              static_cast<unsigned long long>(
                  first_open_us ? (SteadyMicroseconds() - first_open_us) / 1000 : 0),
              static_cast<unsigned long long>(
                  g_read_diagnostics.sound_files.load(std::memory_order_relaxed)),
              static_cast<unsigned long long>(
                  g_read_diagnostics.sound_bytes.load(std::memory_order_relaxed)),
              static_cast<unsigned long long>(
                  g_read_diagnostics.sound_materialize_us.load(std::memory_order_relaxed) / 1000),
              static_cast<unsigned long long>(
                  g_read_diagnostics.shader_files.load(std::memory_order_relaxed)),
              static_cast<unsigned long long>(
                  g_read_diagnostics.shader_materialize_us.load(std::memory_order_relaxed) / 1000),
              title_checkpoint ? "title_music" : "periodic");
    }
}

template <typename T>
T Read(const unsigned char* p) {
    T v;
    memcpy(&v, p, sizeof(T));
    return v;
}

bool PreadAll(int fd, unsigned char* bytes, size_t size, off_t offset) {
    size_t consumed = 0;
    while (consumed < size) {
        const ssize_t result = pread(fd, bytes + consumed, size - consumed,
                                     offset + static_cast<off_t>(consumed));
        if (result > 0) {
            consumed += static_cast<size_t>(result);
        } else if (result < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

// Central directory walk. Local headers are only consulted lazily, at open time,
// because their variable-length fields are what fix the real data offset.
std::unique_ptr<ZipIndex> BuildIndex(const std::string& apk_path) {
    const auto started = std::chrono::steady_clock::now();
    int fd = open(apk_path.c_str(), O_RDONLY);
    if (fd < 0) {
        LOGE("cannot open %s (errno %d)", apk_path.c_str(), errno);
        return nullptr;
    }

    off_t length = lseek(fd, 0, SEEK_END);
    if (length < 22) {
        close(fd);
        return nullptr;
    }

    off_t window = length < 65557 ? length : 65557;
    std::vector<unsigned char> tail(static_cast<size_t>(window));
    if (!PreadAll(fd, tail.data(), tail.size(), length - window)) {
        close(fd);
        return nullptr;
    }

    off_t eocd = -1;
    for (ssize_t i = static_cast<ssize_t>(tail.size()) - 22; i >= 0; --i) {
        if (Read<uint32_t>(&tail[i]) == 0x06054b50) {
            eocd = length - window + i;
            break;
        }
    }
    if (eocd < 0) {
        LOGE("no end-of-central-directory in %s", apk_path.c_str());
        close(fd);
        return nullptr;
    }

    unsigned char header[22];
    if (!PreadAll(fd, header, sizeof(header), eocd)) {
        close(fd);
        return nullptr;
    }
    uint16_t count = Read<uint16_t>(header + 10);
    uint32_t cd_size = Read<uint32_t>(header + 12);
    uint32_t cd_offset = Read<uint32_t>(header + 16);

    std::vector<unsigned char> cd(cd_size);
    if (!PreadAll(fd, cd.data(), cd.size(), cd_offset)) {
        close(fd);
        return nullptr;
    }

    auto index = std::make_unique<ZipIndex>();
    index->fd = fd;

    uint32_t pos = 0;
    for (uint16_t i = 0; i < count && pos + 46 <= cd_size; ++i) {
        if (Read<uint32_t>(&cd[pos]) != 0x02014b50) break;
        uint16_t method = Read<uint16_t>(&cd[pos + 10]);
        uint32_t csize = Read<uint32_t>(&cd[pos + 20]);
        uint32_t usize = Read<uint32_t>(&cd[pos + 24]);
        uint16_t name_len = Read<uint16_t>(&cd[pos + 28]);
        uint16_t extra_len = Read<uint16_t>(&cd[pos + 30]);
        uint16_t comment_len = Read<uint16_t>(&cd[pos + 32]);
        uint32_t local_offset = Read<uint32_t>(&cd[pos + 42]);

        std::string name(reinterpret_cast<const char*>(&cd[pos + 46]), name_len);
        index->entries[name] = {static_cast<off_t>(local_offset), csize, usize, method};
        for (size_t slash = name.find('/'); slash != std::string::npos;
             slash = name.find('/', slash + 1)) {
            index->directories.insert(name.substr(0, slash));
        }
        pos += 46u + name_len + extra_len + comment_len;
    }

    LOGI("indexed %zu entries (%u central-directory bytes) in %llu ms: %s",
         index->entries.size(), cd_size,
         static_cast<unsigned long long>(ElapsedMicroseconds(started) / 1000),
         apk_path.c_str());
    return index;
}

// Splits ".../base.apk/assets/foo" into the archive path and the entry name, or
// resolves a bare relative "assets/foo" against the app's own package.
bool SplitApkPath(const char* path, std::string* apk, std::string* entry) {
    if (!path || !*path) return false;
    if (const char* marker = strstr(path, ".apk/")) {
        *apk = std::string(path, marker + 4 - path);  // include ".apk"
        *entry = marker + 5;
        return !entry->empty();
    }
    if (path[0] != '/' && !g_primary_apk.empty()) {
        *apk = g_primary_apk;
        *entry = path;
        return true;
    }
    return false;
}

ZipIndex* IndexFor(const std::string& apk_path) {
    auto it = g_indexes.find(apk_path);
    if (it != g_indexes.end()) return it->second.get();
    auto index = BuildIndex(apk_path);
    if (!index) return nullptr;
    ZipIndex* raw = index.get();
    g_indexes[apk_path] = std::move(index);
    return raw;
}

// The central directory's offset points at the local header, whose name and extra
// fields have their own lengths - the payload starts past those.
off_t PayloadOffset(int fd, const ZipEntry& entry) {
    unsigned char local[30];
    if (!PreadAll(fd, local, sizeof(local), entry.data_offset)) return -1;
    if (Read<uint32_t>(local) != 0x04034b50) return -1;
    return entry.data_offset + 30 + Read<uint16_t>(local + 26) + Read<uint16_t>(local + 28);
}

int MakeMemoryFile(const char* name) {
    long fd = syscall(__NR_memfd_create, name, MFD_CLOEXEC);
    return fd < 0 ? -1 : static_cast<int>(fd);
}

bool WriteAll(int fd, const unsigned char* bytes, size_t size) {
    size_t written = 0;
    while (written < size) {
        const ssize_t result = write(fd, bytes + written, size - written);
        if (result > 0) {
            written += static_cast<size_t>(result);
        } else if (result < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

// Stored ZIP entries need no userspace staging buffer. sendfile lets the kernel
// populate the anonymous file directly from the APK page cache. Some vendor
// kernels reject regular-file -> memfd copies; the caller transparently falls
// back to the portable pread/write path in that case.
bool CopyStoredInKernel(int source_fd, off_t payload, int destination_fd, size_t size) {
    off_t source_offset = payload;
    size_t copied = 0;
    while (copied < size) {
        const ssize_t result = sendfile(destination_fd, source_fd, &source_offset, size - copied);
        if (result > 0) {
            copied += static_cast<size_t>(result);
        } else if (result < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

}  // namespace

void ApkSetPrimaryPackage(const char* apk_path) {
    if (!apk_path) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_primary_apk = apk_path;
    LOGI("primary package: %s", apk_path);
}

int ApkExtractPrefix(const char* prefix, const char* dest_dir) {
    if (!prefix || !dest_dir) return -1;

    std::vector<std::string> names;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ZipIndex* index = IndexFor(g_primary_apk);
        if (!index) return -1;
        for (const auto& entry : index->entries) {
            if (entry.first.compare(0, strlen(prefix), prefix) == 0 &&
                entry.first.find('/', strlen(prefix)) == std::string::npos) {
                names.push_back(entry.first);
            }
        }
    }

    mkdir(dest_dir, 0755);
    int written = 0;
    for (const std::string& name : names) {
        std::string base = name.substr(name.find_last_of('/') + 1);
        if (base.empty()) continue;
        std::string out_path = std::string(dest_dir) + "/" + base;

        off_t expected = 0;
        std::string full = g_primary_apk + "/" + name;
        if (!ApkStatEntry(full.c_str(), &expected, nullptr)) continue;

        struct stat st {};
        if (stat(out_path.c_str(), &st) == 0 && st.st_size == expected) {
            ++written;
            continue;  // already extracted by an earlier run
        }

        int src = ApkOpenEntry(full.c_str(), nullptr);
        if (src < 0) continue;
        int dst = open(out_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (dst < 0) {
            close(src);
            continue;
        }
        std::vector<unsigned char> buf(64 * 1024);
        ssize_t n;
        bool ok = true;
        while ((n = read(src, buf.data(), buf.size())) > 0) {
            if (write(dst, buf.data(), n) != n) { ok = false; break; }
        }
        close(src);
        close(dst);
        if (ok) ++written;
        else LOGE("failed to extract %s", name.c_str());
    }
    LOGI("%d file(s) under '%s' available in %s", written, prefix, dest_dir);
    return written;
}

int ApkOpenEntry(const char* path, off_t* out_size) {
    std::string apk_path;
    std::string entry_name;
    ZipEntry entry;
    off_t payload = -1;
    int source_fd = -1;
    {
        // Protect only index discovery. Materialising hundreds of MiB while this
        // mutex was held serialized the engine's resource-loader threads.
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!SplitApkPath(path, &apk_path, &entry_name)) return -1;
        ZipIndex* index = IndexFor(apk_path);
        if (!index) return -1;
        auto it = index->entries.find(entry_name);
        if (it == index->entries.end()) return -1;
        entry = it->second;
        payload = PayloadOffset(index->fd, entry);
        if (payload < 0) return -1;
        source_fd = dup(index->fd);
    }
    if (source_fd < 0) return -1;

    const auto started = std::chrono::steady_clock::now();
    const uint64_t started_us = SteadyMicroseconds();

    int out = MakeMemoryFile(entry_name.c_str());
    if (out < 0) {
        LOGE("memfd_create failed for %s (errno %d)", entry_name.c_str(), errno);
        close(source_fd);
        return -1;
    }

    bool ok = false;
    bool kernel_copy = false;
    bool kernel_copy_fallback = false;
    if (entry.method == 0) {
        kernel_copy = CopyStoredInKernel(source_fd, payload, out, entry.uncompressed);
        ok = kernel_copy;
        if (!ok) {
            kernel_copy_fallback = true;
            if (ftruncate(out, 0) == 0 && lseek(out, 0, SEEK_SET) == 0) {
                std::vector<unsigned char> raw(entry.uncompressed);
                ok = PreadAll(source_fd, raw.data(), raw.size(), payload) &&
                     WriteAll(out, raw.data(), raw.size());
            }
        }
    } else {
        std::vector<unsigned char> raw(entry.compressed);
        if (!PreadAll(source_fd, raw.data(), raw.size(), payload)) {
            close(source_fd);
            close(out);
            return -1;
        }
        std::vector<unsigned char> plain(entry.uncompressed);
        z_stream zs {};
        // Negative window size: raw deflate, no zlib header, which is what zip stores.
        if (inflateInit2(&zs, -15) == Z_OK) {
            zs.next_in = raw.data();
            zs.avail_in = static_cast<uInt>(raw.size());
            zs.next_out = plain.data();
            zs.avail_out = static_cast<uInt>(plain.size());
            ok = inflate(&zs, Z_FINISH) == Z_STREAM_END;
            inflateEnd(&zs);
        }
        if (ok) ok = WriteAll(out, plain.data(), plain.size());
    }
    close(source_fd);

    if (!ok) {
        LOGE("could not materialise %s", entry_name.c_str());
        close(out);
        return -1;
    }

    lseek(out, 0, SEEK_SET);
    if (out_size) *out_size = entry.uncompressed;
    RecordRead(entry, entry_name, started_us, ElapsedMicroseconds(started), kernel_copy,
               kernel_copy_fallback);
    if (VerboseAssetTraceEnabled()) {
        LOGI("served %s (%u bytes, method %u)", entry_name.c_str(), entry.uncompressed,
             entry.method);
    }
    return out;
}

void ApkForEachEntry(bool (*visit)(const char* name, uint32_t uncompressed, void* ud),
                     void* ud) {
    if (!visit) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    ZipIndex* index = IndexFor(g_primary_apk);
    if (!index) return;
    for (const auto& [name, entry] : index->entries) {
        if (!visit(name.c_str(), entry.uncompressed, ud)) return;
    }
}

bool ApkStatEntry(const char* path, off_t* out_size, bool* out_is_dir) {
    std::string apk_path;
    std::string entry_name;
    if (!SplitApkPath(path, &apk_path, &entry_name)) return false;
    while (!entry_name.empty() && entry_name.back() == '/') entry_name.pop_back();

    std::lock_guard<std::mutex> lock(g_mutex);
    ZipIndex* index = IndexFor(apk_path);
    if (!index) return false;

    auto it = index->entries.find(entry_name);
    if (it != index->entries.end()) {
        if (out_size) *out_size = it->second.uncompressed;
        if (out_is_dir) *out_is_dir = false;
        return true;
    }
    if (index->directories.count(entry_name)) {
        if (out_size) *out_size = 0;
        if (out_is_dir) *out_is_dir = true;
        return true;
    }
    return false;
}
