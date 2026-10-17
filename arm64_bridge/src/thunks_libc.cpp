#include "thunk_manager.h"
#include "dynarmic_host.h"
#include "thunk_types.h"
#include "elf_loader.h"
#include "apk_reader.h"
#include "game_data_vfs.h"
#include "guest_stdio_helpers.h"
#include "guest_signals.h"
#include "shader_path_guard.h"
#include <unistd.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <ctime>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <linux/memfd.h>
#include <fcntl.h>
#include <unistd.h>
#include <android/log.h>
#include <sys/system_properties.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <thread>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <string>
#include <sstream>
#include <vector>
#include <algorithm>

#define LOG_TAG "CMR_LibcThunks"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

extern "C" FILE *funopen(const void *cookie,
                         int (*readfn)(void *, char *, int),
                         int (*writefn)(void *, const char *, int),
                         fpos_t (*seekfn)(void *, fpos_t, int),
                         int (*closefn)(void *));

struct ApkEntry {
    char name[128];
    off_t data_offset;
    off_t file_size;
};

static ApkEntry g_apk_entries[128];
static int g_num_apk_entries = 0;
static char g_apk_indexed_path[1024] = {0};
static int g_apk_indexed_fd = -1;

// Guest CPU discovery must describe the emulated ARMv7 ISA, consistently with
// getauxval below, not the arm64 kernel's incompatible "fp asimd" feature names.
static constexpr char kGuestCpuInfo[] =
    "Processor\t: ARMv7 Processor (Dynarmic)\n"
    "CPU architecture: 7\n"
    "Features\t: swp half thumb fastmult vfp edsp neon vfpv3 tls vfpv4 idiva idivt vfpd32\n";

static int OpenGuestCpuInfo() {
    const int memory_fd = static_cast<int>(syscall(
        __NR_memfd_create, "arm32-cpuinfo", MFD_CLOEXEC | MFD_ALLOW_SEALING));
    if (memory_fd < 0) return -1;
    const ssize_t size = sizeof(kGuestCpuInfo) - 1;
    const ssize_t written = write(memory_fd, kGuestCpuInfo, size);
    if (written != size) {
        const int error = written < 0 ? errno : EIO;
        close(memory_fd);
        errno = error;
        return -1;
    }
    const int seals = F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL;
    if (fcntl(memory_fd, F_ADD_SEALS, seals) < 0) {
        const int seal_error = errno;
        close(memory_fd);
        errno = seal_error;
        return -1;
    }
    // Android 17 SELinux denies reopening a deleted memfd through /proc/self/fd.
    // The original sealed descriptor is already a seekable, read-only stream.
    if (lseek(memory_fd, 0, SEEK_SET) < 0) {
        const int seek_error = errno;
        close(memory_fd);
        errno = seek_error;
        return -1;
    }
    LOGI("guest cpuinfo fd=%d: ARMv7 VFP/NEON (emulated ISA)", memory_fd);
    return memory_fd;
}

static bool GuestCpuInfoSelfCheck() {
    const int fd = OpenGuestCpuInfo();
    char text[sizeof(kGuestCpuInfo)] = {};
    const int flags = fd >= 0 ? fcntl(fd, F_GETFL) : -1;
    const int seals = fd >= 0 ? fcntl(fd, F_GET_SEALS) : -1;
    const int access_mode = flags & O_ACCMODE;
    const bool sealed_readable = access_mode == O_RDONLY ||
        (access_mode == O_RDWR && (seals & F_SEAL_WRITE) != 0);
    const bool ok = fd >= 0 && sealed_readable &&
        read(fd, text, sizeof(text)) == sizeof(kGuestCpuInfo) - 1 &&
        memcmp(text, kGuestCpuInfo, sizeof(kGuestCpuInfo) - 1) == 0 &&
        read(fd, text, 1) == 0;
    if (fd >= 0) close(fd);
    if (ok) LOGI("GuestCpuInfoSelfCheck OK (ARMv7, sealed, EOF)");
    else LOGE("GuestCpuInfoSelfCheck FAILED (fd=%d flags=0x%x seals=0x%x errno=%d)",
              fd, flags, seals, errno);
    return ok;
}

// --- Guest synchronisation objects -----------------------------------------
//
// Real host primitives behind the guest's pthread calls. ARM32 bionic's
// pthread_mutex_t and pthread_cond_t are four bytes, too small to hold a host
// pointer, so the guest address is the key instead. That also makes
// PTHREAD_MUTEX_INITIALIZER work: a statically initialised mutex never calls
// pthread_mutex_init, and looking it up on first lock creates it anyway.
//
// Stubbing these out is what used to hang the boot: Unity's NativeActivity glue
// sits in `while (!running) pthread_cond_wait(...)` waiting for the thread it
// just created, and a no-op wait turns that into a spin that never ends.

namespace {

struct GuestSem {
    std::mutex m;
    std::condition_variable cv;
    int count = 0;
};

// A guest condition variable carries a counter alongside the host one.
//
// pthread_cond_wait here cannot be a plain blocking wait: a thread parked in it
// must still be able to run a signal handler, so it wakes every 20 ms to check.
// That reintroduces a lost wakeup, because condition_variable_any::wait_for
// releases its internal lock before re-taking the caller's mutex, and a notify
// landing in that window reaches nobody. The counter closes it: the signal side
// bumps it, and the waiter compares it against the value it started with, so a
// notification that arrived while it was between waits is still seen.
struct GuestCond {
    // Deliberately not a condition_variable_any bound to the guest's own mutex: that
    // form re-acquires the guest mutex before it returns, and that re-acquisition
    // cannot be interrupted to run a signal handler. See GuestCondWaitUntil.
    std::mutex m;
    std::condition_variable cv;
    std::atomic<uint64_t> generation{0};
};

struct GuestThreadHandle {
    std::mutex m;
    std::condition_variable cv;
    bool finished = false;
    uint32_t retval = 0;
    std::atomic<int> kernel_tid{0};
};

std::mutex g_sync_registry;
std::map<uint32_t, std::unique_ptr<std::recursive_timed_mutex>> g_guest_mutexes;
std::map<uint32_t, std::unique_ptr<GuestCond>> g_guest_conds;
std::map<uint32_t, std::unique_ptr<GuestSem>> g_guest_sems;
std::unordered_map<uint32_t, std::shared_ptr<GuestThreadHandle>> g_guest_threads;
std::condition_variable g_guard_cv;
std::unordered_set<uint32_t> g_guards_in_progress;

}  // namespace

int (*GuestKernelTidLookup)(uint32_t guest_tid) = nullptr;

namespace {

// A sleep the guest asked for, in slices, so a signal that arrives during it is run
// before the sleep is over. On the device the kernel cuts the sleep short and the
// handler runs; a thread whose whole life is "sleep a little, poll" otherwise never
// spends long enough in guest code for the Jit halt to reach it, and Boehm's
// stop-the-world then waits for an acknowledgement that cannot come.
void GuestSleepInterruptible(DynarmicHost& host, int64_t usec) {
    constexpr int64_t kSlice = 20000;  // 20 ms
    while (usec > 0) {
        int64_t chunk = usec < kSlice ? usec : kSlice;
        std::this_thread::sleep_for(std::chrono::microseconds(chunk));
        usec -= chunk;
        if (GuestSignalPending()) GuestSignalDeliverPending(host);
    }
}

}  // namespace

// Guest synchronisation objects are keyed by the guest address they live at, and
// that key only stays meaningful while the object does. The heap now recycles
// blocks, so a freed Semaphore's address comes back as some other object - and it
void GuestSyncForget(uint32_t addr, uint32_t size) {
    if (!size) return;
    std::lock_guard<std::mutex> lock(g_sync_registry);
    auto purge = [addr, size](auto& table) {
        auto it = table.lower_bound(addr);
        while (it != table.end() && it->first < addr + size) it = table.erase(it);
    };
    purge(g_guest_mutexes);
    purge(g_guest_conds);
    purge(g_guest_sems);
}

namespace {

std::recursive_timed_mutex& GuestMutexFor(uint32_t addr) {
    std::lock_guard<std::mutex> lock(g_sync_registry);
    auto& slot = g_guest_mutexes[addr];
    if (!slot) slot = std::make_unique<std::recursive_timed_mutex>();
    return *slot;
}

GuestCond& GuestCondFor(uint32_t addr) {
    std::lock_guard<std::mutex> lock(g_sync_registry);
    auto& slot = g_guest_conds[addr];
    if (!slot) slot = std::make_unique<GuestCond>();
    return *slot;
}

// Waiting for a guest mutex has to stay interruptible. Boehm stops the world by
// signalling every thread and waiting for one acknowledgement from each, so a thread
// queued behind a mutex whose owner is already stopped has to be able to run its
// handler right there - that is what the kernel does on the device. A blocking
// lock() leaves the collector one acknowledgement short, forever, with the allocator
// lock held.
void GuestMutexLockInterruptible(std::recursive_timed_mutex& mutex, DynarmicHost& host) {
    while (!mutex.try_lock_for(std::chrono::milliseconds(20))) {
        if (GuestSignalPending()) GuestSignalDeliverPending(host);
    }
}

// pthread_cond_wait, with both of its blocking points interruptible.
//
// The guest mutex is dropped for the whole wait and re-taken through the helper
// above, rather than by a condition_variable_any bound to it: that binding hides a
// second, uninterruptible acquisition inside wait_for, and the thread that holds the
// mutex at that moment is very often one the collector has just stopped. That is what
// used to leave five threads with an undelivered suspend signal and the stop-the-world
// stuck in sem_wait.
//
// Waiting in 20 ms slices rather than on the deadline directly is what makes room for
// the handler. `generation` is re-read each slice, so a notification that arrived
// while this thread was between slices is still seen. Returns false on timeout.
bool GuestCondWaitUntil(GuestCond& cond, std::recursive_timed_mutex& mutex, DynarmicHost& host,
                        std::chrono::steady_clock::time_point deadline) {
    const uint64_t started_at = cond.generation.load();
    mutex.unlock();
    bool signalled = false;
    for (;;) {
        if (cond.generation.load() != started_at) {
            signalled = true;
            break;
        }
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;
        auto left = deadline - now;
        auto slice = std::chrono::milliseconds(20);
        std::chrono::nanoseconds wait = (left < slice) ? left : std::chrono::nanoseconds(slice);
        {
            std::unique_lock<std::mutex> lock(cond.m);
            cond.cv.wait_for(lock, wait, [&] { return cond.generation.load() != started_at; });
        }
        if (GuestSignalPending()) GuestSignalDeliverPending(host);
    }
    GuestMutexLockInterruptible(mutex, host);
    return signalled;
}

void GuestCondNotify(GuestCond& cond, bool all) {
    {
        // Bumped under the same lock the waiter checks it under, so a notification
        // cannot slip into the gap between the waiter's test and its wait.
        std::lock_guard<std::mutex> lock(cond.m);
        cond.generation.fetch_add(1);
    }
    if (all) {
        cond.cv.notify_all();
    } else {
        cond.cv.notify_one();
    }
}

GuestSem& GuestSemFor(uint32_t addr) {
    std::lock_guard<std::mutex> lock(g_sync_registry);
    auto& slot = g_guest_sems[addr];
    if (!slot) slot = std::make_unique<GuestSem>();
    return *slot;
}

// dlopen handles: kDlHandleBase means "global", anything above indexes g_dl_handles.
constexpr uint32_t kDlHandleBase = 0x50005000;
std::vector<std::string> g_dl_handles;
thread_local std::string t_dl_error;
thread_local uint32_t t_dl_error_guest = 0;

std::atomic<uint32_t> g_next_guest_tid{0x1001};
thread_local uint32_t t_guest_tid = 0;
// pthread_t as the guest sees it -> kernel tid, for threads that minted their own id
// rather than being created through the pthread_create thunk.
std::mutex g_self_ids_mutex;
std::map<uint32_t, int> g_self_ids;

uint32_t GuestSelfId() {
    if (t_guest_tid == 0) {
        t_guest_tid = g_next_guest_tid.fetch_add(1);
        std::lock_guard<std::mutex> lock(g_self_ids_mutex);
        g_self_ids[t_guest_tid] = gettid();
    }
    return t_guest_tid;
}

// Absolute CLOCK_REALTIME deadline -> relative wait, which is what the C++
// condition variables take.
std::chrono::nanoseconds UntilGuestDeadline(DynarmicHost& host, uint32_t abstime_ptr) {
    if (!abstime_ptr) return std::chrono::nanoseconds::zero();
    int64_t sec = static_cast<int32_t>(host.MemoryRead32(abstime_ptr));
    int64_t nsec = static_cast<int32_t>(host.MemoryRead32(abstime_ptr + 4));
    struct timespec now {};
    clock_gettime(CLOCK_REALTIME, &now);
    int64_t delta = (sec - now.tv_sec) * 1000000000LL + (nsec - now.tv_nsec);
    return std::chrono::nanoseconds(delta < 0 ? 0 : delta);
}

}  // namespace

struct ApkCookie {
    off_t base_offset;
    off_t file_size;
    off_t current_offset;
};

static int ApkRead(void* cookie, char* buf, int size) {
    ApkCookie* c = static_cast<ApkCookie*>(cookie);
    if (size <= 0 || !c || g_apk_indexed_fd < 0) return 0;

    if (c->current_offset + size > c->file_size) {
        size = static_cast<int>(c->file_size - c->current_offset);
    }
    if (size <= 0) return 0;

    ssize_t n = pread(g_apk_indexed_fd, buf, size, c->base_offset + c->current_offset);
    if (n > 0) {
        c->current_offset += n;
        return static_cast<int>(n);
    }
    LOGI("ApkRead: size=%d, offset=%lld, n=%zd", size, static_cast<long long>(c->current_offset), n);
    return 0;
}

static fpos_t ApkSeek(void* cookie, fpos_t offset, int whence) {
    ApkCookie* c = static_cast<ApkCookie*>(cookie);
    if (!c) return -1;
    off_t target = 0;

    if (whence == SEEK_SET) target = offset;
    else if (whence == SEEK_CUR) target = c->current_offset + offset;
    else if (whence == SEEK_END) target = c->file_size + offset;
    else return -1;

    if (target < 0) return -1;
    if (target > c->file_size) target = c->file_size;
    c->current_offset = target;
    return target;
}

static int ApkClose(void* cookie) {
    ApkCookie* c = static_cast<ApkCookie*>(cookie);
    if (c) {
        free(c);
    }
    return 0;
}

extern "C" int IndexApk(const char* apk_path) {
    if (!apk_path) return -1;
    strncpy(g_apk_indexed_path, apk_path, sizeof(g_apk_indexed_path) - 1);
    if (g_apk_indexed_fd >= 0) close(g_apk_indexed_fd);
    g_apk_indexed_fd = open(g_apk_indexed_path, O_RDONLY);
    if (g_apk_indexed_fd < 0) {
        LOGE("IndexApk: Failed to open APK: %s (errno %d)", g_apk_indexed_path, errno);
        return -1;
    }

    off_t file_len = lseek(g_apk_indexed_fd, 0, SEEK_END);
    if (file_len < 22) return -1;

    off_t search_len = file_len < 65536 + 22 ? file_len : 65536 + 22;
    off_t search_start = file_len - search_len;
    unsigned char* buf = static_cast<unsigned char*>(malloc(search_len));
    if (!buf) return -1;

    pread(g_apk_indexed_fd, buf, search_len, search_start);
    off_t eocd_offset = -1;
    for (ssize_t i = search_len - 22; i >= 0; --i) {
        if (buf[i] == 0x50 && buf[i+1] == 0x4b && buf[i+2] == 0x05 && buf[i+3] == 0x06) {
            eocd_offset = search_start + i;
            break;
        }
    }
    free(buf);

    if (eocd_offset < 0) {
        LOGE("IndexApk: EOCD not found in APK!");
        return -1;
    }

    unsigned char eocd[22];
    pread(g_apk_indexed_fd, eocd, 22, eocd_offset);
    uint16_t num_entries = *reinterpret_cast<uint16_t*>(eocd + 10);
    uint32_t cd_size = *reinterpret_cast<uint32_t*>(eocd + 12);
    uint32_t cd_offset = *reinterpret_cast<uint32_t*>(eocd + 16);

    unsigned char* cd = static_cast<unsigned char*>(malloc(cd_size));
    if (!cd) return -1;
    pread(g_apk_indexed_fd, cd, cd_size, cd_offset);

    g_num_apk_entries = 0;
    uint32_t pos = 0;
    for (int i = 0; i < num_entries && pos + 46 <= cd_size; ++i) {
        if (*reinterpret_cast<uint32_t*>(cd + pos) != 0x02014b50) break;
        uint16_t method = *reinterpret_cast<uint16_t*>(cd + pos + 10);
        uint32_t usize = *reinterpret_cast<uint32_t*>(cd + pos + 24);
        uint16_t fn_len = *reinterpret_cast<uint16_t*>(cd + pos + 28);
        uint16_t extra_len = *reinterpret_cast<uint16_t*>(cd + pos + 30);
        uint16_t comm_len = *reinterpret_cast<uint16_t*>(cd + pos + 32);
        uint32_t local_hdr_offset = *reinterpret_cast<uint32_t*>(cd + pos + 42);

        char fn[256] = {0};
        int cpy = fn_len < 255 ? fn_len : 255;
        memcpy(fn, cd + pos + 46, cpy);

        if (strstr(fn, "FMOD") != nullptr || strstr(fn, ".fsb") != nullptr || strstr(fn, ".fev") != nullptr || strstr(fn, "assets/data/audio") != nullptr) {
            unsigned char local_hdr[30];
            pread(g_apk_indexed_fd, local_hdr, 30, local_hdr_offset);
            uint16_t l_fn_len = *reinterpret_cast<uint16_t*>(local_hdr + 26);
            uint16_t l_extra_len = *reinterpret_cast<uint16_t*>(local_hdr + 28);
            off_t raw_data_offset = local_hdr_offset + 30 + l_fn_len + l_extra_len;

            if (g_num_apk_entries < 128) {
                strncpy(g_apk_entries[g_num_apk_entries].name, fn, 127);
                g_apk_entries[g_num_apk_entries].data_offset = raw_data_offset;
                g_apk_entries[g_num_apk_entries].file_size = usize;
                LOGI("IndexApk entry [%d]: %s (method=%d, size=%u, offset=%lld)",
                     g_num_apk_entries, fn, method, usize, static_cast<long long>(raw_data_offset));
                g_num_apk_entries++;
            }
        }
        pos += 46 + fn_len + extra_len + comm_len;
    }
    free(cd);
    LOGI("IndexApk: Indexed %d audio entries from APK: %s", g_num_apk_entries, apk_path);
    return 0;
}

static ApkEntry* FindApkEntry(const char* path) {
    if (!path || g_num_apk_entries == 0) return nullptr;
    const char* slash = strrchr(path, '/');
    const char* base = slash ? slash + 1 : path;

    char preferred[256];
    snprintf(preferred, sizeof(preferred), "assets/data/audio/%s", base);
    for (int i = 0; i < g_num_apk_entries; ++i) {
        if (strcmp(g_apk_entries[i].name, preferred) == 0 || strcmp(g_apk_entries[i].name, path) == 0) {
            return &g_apk_entries[i];
        }
    }

    // 3. Language suffix fallback (e.g. voices.fsb -> voices_eng.fsb)
    std::string base_str(base);
    if (base_str.size() > 4 && base_str.substr(base_str.size() - 4) == ".fsb") {
        std::string stem = base_str.substr(0, base_str.size() - 4);
        std::string eng_variant = "assets/data/audio/" + stem + "_eng.fsb";
        for (int i = 0; i < g_num_apk_entries; ++i) {
            if (g_apk_entries[i].name == eng_variant || strstr(g_apk_entries[i].name, (stem + "_eng").c_str()) != nullptr) {
                return &g_apk_entries[i];
            }
        }
        for (int i = 0; i < g_num_apk_entries; ++i) {
            if (strstr(g_apk_entries[i].name, stem.c_str()) != nullptr) {
                return &g_apk_entries[i];
            }
        }
    }
    return nullptr;
}

struct VirtualFd {
    int real_apk_fd;
    size_t file_size;
    off_t file_offset; // offset within APK
    off_t current_pos; // 0 .. file_size
};
static std::unordered_map<int, VirtualFd> g_vfd_map;
static int g_next_vfd = 1000;
static std::mutex g_vfd_mutex;

static std::unordered_map<uint32_t, FILE*> g_file_map;
// ponytail: serialize FILE operations; use per-stream locks if contention is measured.
static std::mutex g_file_mutex;
static constexpr uint32_t kGuestFileSize = 84;
static constexpr uint32_t kGuestFileFdOffset = 14;
static uint32_t g_stdio_base = 0;

static bool BindGuestFile(DynarmicHost& host, uint32_t handle, FILE* file) {
    if (!handle || !file) { errno = EINVAL; return false; }
    const int fd = fileno(file);
    if (fd > INT16_MAX) { errno = EOVERFLOW; return false; }
    // The guest's static libstdc++ reads _file directly; other FILE fields stay
    // zero and stdio operations go through the registry, not the host layout.
    host.MemoryWrite16(handle + kGuestFileFdOffset, static_cast<uint16_t>(fd));
    std::lock_guard<std::mutex> lock(g_file_mutex);
    g_file_map[handle] = file;
    return true;
}

static int CloseGuestFile(DynarmicHost& host, uint32_t handle) {
    int result;
    int error;
    {
        std::lock_guard<std::mutex> lock(g_file_mutex);
        auto it = g_file_map.find(handle);
        if (it == g_file_map.end()) return EOF;
        result = fclose(it->second);
        error = errno;
        g_file_map.erase(it);
    }
    // __sF is one permanent three-element allocation; its cells are not owned
    // individually. Free dynamic facades outside the FILE lock (heap lock order).
    if (handle < g_stdio_base || handle >= g_stdio_base + 3 * kGuestFileSize)
        host.GuestFree(handle);
    errno = error;
    return result;
}

static bool RegisterGuestStdioStreams(DynarmicHost& host, uint32_t base) {
    g_stdio_base = base;
    return BindGuestFile(host, base, stdin) &&
           BindGuestFile(host, base + kGuestFileSize, stdout) &&
           BindGuestFile(host, base + 2 * kGuestFileSize, stderr);
}

static int GuestFileGetc(uint32_t handle) {
    std::lock_guard<std::mutex> lock(g_file_mutex);
    auto it = g_file_map.find(handle);
    return it != g_file_map.end() ? fgetc(it->second) : EOF;
}

static int GuestFileUngetc(int c, uint32_t handle) {
    std::lock_guard<std::mutex> lock(g_file_mutex);
    auto it = g_file_map.find(handle);
    return it != g_file_map.end() ? ungetc(c, it->second) : EOF;
}

static bool GuestStdioSelfCheck(DynarmicHost& host) {
    int fds[2];
    if (pipe(fds) != 0) {
        LOGE("GuestStdioSelfCheck FAILED: pipe");
        return false;
    }
    const unsigned char bytes[] = {'A', 0xFF};
    const bool written = write(fds[1], bytes, sizeof(bytes)) == sizeof(bytes);
    close(fds[1]);
    FILE* file = fdopen(fds[0], "rb");
    if (!file) {
        close(fds[0]);
        LOGE("GuestStdioSelfCheck FAILED: fdopen");
        return false;
    }
    const uint32_t handle = host.GuestAlloc(kGuestFileSize, 8);
    if (!handle || !BindGuestFile(host, handle, file)) {
        fclose(file);
        if (handle) host.GuestFree(handle);
        LOGE("GuestStdioSelfCheck FAILED: guest FILE facade");
        return false;
    }
    const int mirrored_fd = static_cast<int16_t>(host.MemoryRead16(handle + kGuestFileFdOffset));
    unsigned char first = 0;
    const bool ok = written && mirrored_fd == fds[0] &&
        read(mirrored_fd, &first, 1) == 1 && first == 'A' &&
        GuestFileGetc(handle) == 0xFF && GuestFileGetc(handle) == EOF && feof(file) &&
        GuestFileUngetc('Z', handle) == 'Z' && !feof(file) &&
        GuestFileGetc(handle) == 'Z' && GuestFileGetc(handle) == EOF && feof(file) &&
        GuestFileUngetc(EOF, handle) == EOF && GuestFileGetc(0) == EOF;
    const bool closed = CloseGuestFile(host, handle) == 0;
    if (ok && closed) LOGI("GuestStdioSelfCheck OK (FILE+14, raw read, EOF, pushback)");
    else LOGE("GuestStdioSelfCheck FAILED: FILE/character stream contract");
    return ok && closed;
}

static thread_local std::unordered_map<uint32_t, uint32_t> t_tls_map;
static std::atomic<uint32_t> g_next_tls_key{1};

#undef st_atime
#undef st_mtime
#undef st_ctime
#undef st_atime_nsec
#undef st_mtime_nsec
#undef st_ctime_nsec

// ARM32 bionic's `struct stat` is the stat64 layout, not the old 16-bit-uid
// kernel one: st_mode sits at offset 16 and st_size is a 64-bit field at 48.
// Filling in the legacy layout instead puts st_mode where the guest reads
// __st_ino, so every S_ISDIR test says "not a directory" even though stat
// itself reported success - which is how Mono decided its assembly directory
// did not exist. Written by offset so host struct padding cannot drift.
static constexpr size_t kArm32StatSize = 104;

void WriteArm32Stat(uint8_t* guest_base, uint32_t gaddr, const struct stat& st) {
    if (!gaddr) return;
    uint8_t* p = reinterpret_cast<uint8_t*>(GuestToHost(guest_base, gaddr));
    if (!p) return;
    memset(p, 0, kArm32StatSize);
    auto put32 = [p](size_t off, uint32_t v) { memcpy(p + off, &v, 4); };
    auto put64 = [p](size_t off, uint64_t v) { memcpy(p + off, &v, 8); };

    put64(0,  st.st_dev);
    put32(12, static_cast<uint32_t>(st.st_ino));   // __st_ino, the truncated copy
    put32(16, st.st_mode);
    put32(20, static_cast<uint32_t>(st.st_nlink));
    put32(24, st.st_uid);
    put32(28, st.st_gid);
    put64(32, st.st_rdev);
    put64(48, static_cast<uint64_t>(st.st_size));
    put32(56, static_cast<uint32_t>(st.st_blksize));
    put64(64, static_cast<uint64_t>(st.st_blocks));
    put32(72, static_cast<uint32_t>(st.st_atim.tv_sec));
    put32(76, static_cast<uint32_t>(st.st_atim.tv_nsec));
    put32(80, static_cast<uint32_t>(st.st_mtim.tv_sec));
    put32(84, static_cast<uint32_t>(st.st_mtim.tv_nsec));
    put32(88, static_cast<uint32_t>(st.st_ctim.tv_sec));
    put32(92, static_cast<uint32_t>(st.st_ctim.tv_nsec));
    put64(96, st.st_ino);
}

// --- Guest printf formatter -------------------------------------------------
//
// Mono routes every diagnostic and every constructed path through eglib, and
// eglib builds both with vasprintf/g_strdup_printf. A formatter that drops
// arguments does not merely garble a log line: g_strdup_printf is how the
// runtime assembles the assembly search paths, so a wrong result here reads
// downstream as "mscorlib.dll was not found".
//
// Two rules from AAPCS decide whether the arguments line up at all, and both
// were missing before: a 64-bit variadic argument starts in an even register
// (or eight-byte-aligned on the stack), and a float is promoted to double, so
// it occupies eight bytes and not four. Getting either wrong shifts every
// following argument. Note that on ARM32 `long` is 32 bits - only `ll`, `L`,
// `j` and `q` name a 64-bit argument.
namespace {

// Walks the variadic argument block: core registers first, then the stack.
// Once an argument spills, the remaining registers are dead - that is the
// calling convention, not an approximation.
struct GuestVarArgs {
    Dynarmic::A32::Jit& jit;
    DynarmicHost& host;
    int reg;         // next core register, 4 means "registers exhausted"
    uint32_t stack;  // guest address of the next stack word

    uint32_t Next32() {
        if (reg <= 3) return jit.Regs()[reg++];
        uint32_t v = host.MemoryRead32(stack);
        stack += 4;
        return v;
    }

    uint64_t Next64() {
        if (reg & 1) ++reg;  // 64-bit arguments start on an even register
        if (reg <= 2) {
            uint64_t lo = jit.Regs()[reg];
            uint64_t hi = jit.Regs()[reg + 1];
            reg += 2;
            return lo | (hi << 32);
        }
        reg = 4;
        stack = (stack + 7u) & ~7u;
        uint64_t lo = host.MemoryRead32(stack);
        uint64_t hi = host.MemoryRead32(stack + 4);
        stack += 8;
        return lo | (hi << 32);
    }
};

// Renders one conversion with the host's own printf, so flags, width and
// precision behave exactly as the guest asked for them.
template <typename T>
void AppendFormatted(std::string& out, const std::string& spec, T value) {
    char small[256];
    int n = snprintf(small, sizeof(small), spec.c_str(), value);
    if (n < 0) return;
    if (static_cast<size_t>(n) < sizeof(small)) {
        out.append(small, static_cast<size_t>(n));
        return;
    }
    std::vector<char> big(static_cast<size_t>(n) + 1);
    n = snprintf(big.data(), big.size(), spec.c_str(), value);
    if (n > 0) out.append(big.data(), static_cast<size_t>(n));
}

std::string FormatGuestVarArgs(DynarmicHost& host, const char* fmt, GuestVarArgs& args) {
    if (!fmt) return "";
    std::string out;
    const char* p = fmt;

    while (*p) {
        if (*p != '%') { out += *p++; continue; }
        const char* start = p++;
        if (*p == '%') { out += '%'; ++p; continue; }
        if (!*p) { out += *start; break; }

        std::string spec = "%";
        while (*p == '-' || *p == '+' || *p == ' ' || *p == '#' || *p == '0') spec += *p++;
        if (*p == '*') {
            spec += std::to_string(static_cast<int32_t>(args.Next32()));
            ++p;
        } else {
            while (isdigit(static_cast<unsigned char>(*p))) spec += *p++;
        }
        if (*p == '.') {
            spec += *p++;
            if (*p == '*') {
                spec += std::to_string(static_cast<int32_t>(args.Next32()));
                ++p;
            } else {
                while (isdigit(static_cast<unsigned char>(*p))) spec += *p++;
            }
        }

        bool wide = false;  // names a 64-bit argument
        while (*p == 'l' || *p == 'h' || *p == 'z' || *p == 't' || *p == 'j' ||
               *p == 'L' || *p == 'q') {
            if (*p == 'L' || *p == 'j' || *p == 'q') wide = true;
            if (*p == 'l' && *(p + 1) == 'l') { wide = true; ++p; }
            ++p;
        }
        if (!*p) { out += start; break; }

        char conv = *p++;
        switch (conv) {
        case 'd': case 'i':
            if (wide) AppendFormatted(out, spec + "lld", static_cast<long long>(args.Next64()));
            else      AppendFormatted(out, spec + "d", static_cast<int>(static_cast<int32_t>(args.Next32())));
            break;
        case 'u': case 'o': case 'x': case 'X':
            if (wide) AppendFormatted(out, spec + "ll" + conv, static_cast<unsigned long long>(args.Next64()));
            else      AppendFormatted(out, spec + conv, static_cast<unsigned int>(args.Next32()));
            break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            // Floats are promoted to double by the default argument promotions,
            // so this is always eight bytes even when the guest wrote %f.
            uint64_t raw = args.Next64();
            double d;
            memcpy(&d, &raw, sizeof(d));
            AppendFormatted(out, spec + conv, d);
            break;
        }
        case 'c':
            AppendFormatted(out, spec + "c", static_cast<int>(args.Next32()));
            break;
        case 's': {
            uint32_t gstr = args.Next32();
            const char* str = reinterpret_cast<const char*>(
                GuestToHost(host.GetGuestMemoryBase(), gstr));
            AppendFormatted(out, spec + "s", str ? str : "(null)");
            break;
        }
        case 'p':
            AppendFormatted(out, "0x%08x", args.Next32());
            break;
        case 'n':
            args.Next32();  // deliberately not written back
            break;
        default:
            out.append(start, static_cast<size_t>(p - start));
            break;
        }
    }
    return out;
}

}  // namespace

// Arguments still in registers: `first_arg_reg` is the first variadic one, and
// anything past r3 is on the stack starting at sp.
std::string FormatGuestString(Dynarmic::A32::Jit& jit, DynarmicHost& host, const char* fmt, int first_arg_reg) {
    GuestVarArgs args{jit, host, first_arg_reg, jit.Regs()[13]};
    return FormatGuestVarArgs(host, fmt, args);
}

// A guest va_list on ARM32 is a single cursor into the argument block: every
// argument, including the ones that arrived in registers, has been spilled
// there by va_start.
std::string FormatGuestVaList(Dynarmic::A32::Jit& jit, DynarmicHost& host, const char* fmt, uint32_t va_list_ptr) {
    GuestVarArgs args{jit, host, 4, va_list_ptr};
    return FormatGuestVarArgs(host, fmt, args);
}

// abort() carries no message of its own, so without this the log says only that
// the guest gave up. Frames are raw guest addresses: subtract the image base to
// read them (0x42000000 libmono, 0x44000000 libunity, 0x46000000 the guest audio plugin).
static void GuestFatalBacktrace(Dynarmic::A32::Jit& jit, DynarmicHost& host) {
    uint32_t frames[16];
    int found = GuestBacktrace(host, jit.Regs()[13], jit.Regs()[14], frames, 16);
    std::string line;
    for (int i = 0; i < found; ++i) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%08X ", frames[i]);
        line += buf;
    }
    LOGE("[fatal] tid %d guest stack: %s", gettid(), line.c_str());
}

// Guest logging obeys `log.tag.<TAG>`, the way any Android process does.
// __android_log_is_loggable would say this in one call but only exists from API 30,
// and the bridge targets 26, so the property is read directly - once per tag, since
// this sits in the path guest audio uses to narrate every 16 KiB it reads.
bool GuestLogAllowed(int prio, const char* tag) {
    if (!tag) return true;
    static std::mutex mutex;
    static std::unordered_map<std::string, int> levels;   // tag -> minimum priority
    int minimum;
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = levels.find(tag);
        if (it == levels.end()) {
            char key[160];
            snprintf(key, sizeof(key), "log.tag.%s", tag);
            char value[PROP_VALUE_MAX] = {0};
            int level = ANDROID_LOG_VERBOSE;
            if (__system_property_get(key, value) > 0) {
                switch (value[0]) {
                    case 'v': case 'V': level = ANDROID_LOG_VERBOSE; break;
                    case 'd': case 'D': level = ANDROID_LOG_DEBUG; break;
                    case 'i': case 'I': level = ANDROID_LOG_INFO; break;
                    case 'w': case 'W': level = ANDROID_LOG_WARN; break;
                    case 'e': case 'E': level = ANDROID_LOG_ERROR; break;
                    case 's': case 'S': level = ANDROID_LOG_SILENT; break;
                    default: break;
                }
            }
            it = levels.emplace(tag, level).first;
        }
        minimum = it->second;
    }
    return prio >= minimum;
}

// All inspected main-guest scanf callsites use at most four destinations.
// Reject wider/allocating/positional forms instead of letting libc read host varargs.
static int ScanGuestText(const char* text, const char* format, void* const (&outputs)[4],
                         const size_t (&capacity)[4]) {
    std::string native_format;
    size_t destinations = 0;
    const size_t input_size = strlen(text);
    for (const char* p = format; *p;) {
        if (*p != '%') { native_format += *p++; continue; }
        native_format += *p++;
        if (*p == '%') { native_format += *p++; continue; }
        const bool suppressed = *p == '*';
        if (suppressed) native_format += *p++;
        size_t width = 0;
        while (*p >= '0' && *p <= '9') {
            if (width > (SIZE_MAX - 9) / 10) return -2;
            width = width * 10 + (*p - '0');
            native_format += *p++;
        }
        std::string length;
        if (*p && strchr("hljztL", *p)) {
            length += *p++;
            if ((length == "h" || length == "l") && *p == length[0]) length += *p++;
        }
        const char conversion = *p;
        if (!conversion) return -2;
        ++p;
        size_t needed = 0;
        if (strchr("diouxXn", conversion)) {
            if (length == "L") return -2;
            if (length == "l" || length == "z" || length == "t") length.clear();
            if (length == "j") length = "ll";
            needed = length == "ll" ? 8 : length == "h" ? 2 : length == "hh" ? 1 : 4;
        } else if (strchr("aAeEfFgG", conversion)) {
            if (length == "L") length = "l"; // ARM32 long double is double, not AArch64 fp128.
            if (!length.empty() && length != "l") return -2;
            needed = length == "l" ? 8 : 4;
        } else if (conversion == 'c' || conversion == 's' || conversion == '[') {
            if (!length.empty() && length != "l") return -2;
            size_t chars = conversion == 'c' ? (width ? width : 1) : (width ? width : input_size);
            chars = std::min(chars, input_size) + (conversion == 'c' ? 0 : 1);
            if (chars > SIZE_MAX / 4) return -2;
            needed = chars * (length == "l" ? 4 : 1);
        } else {
            return -2; // No pointer-sized or libc-allocated output may escape into guest storage.
        }
        native_format += length;
        native_format += conversion;
        if (conversion == '[') {
            if (*p == '^') native_format += *p++;
            if (*p == ']') native_format += *p++;
            while (*p && *p != ']') native_format += *p++;
            if (*p != ']') return -2;
            native_format += *p++;
        }
        if (!suppressed) {
            if (destinations == 4 || !outputs[destinations] || needed > capacity[destinations]) return -2;
            ++destinations;
        }
    }
    return sscanf(text, native_format.c_str(), outputs[0], outputs[1], outputs[2], outputs[3]);
}

static bool GuestScanfSelfCheck() {
    int decimal = 0;
    unsigned hex = 0;
    long long wide = 0;
    struct { double value; uint64_t guard; } number{0, 0x1122334455667788ULL};
    void* values[4] = {&decimal, &hex, &wide, &number.value};
    const size_t sizes[4] = {sizeof(decimal), sizeof(hex), sizeof(wide), sizeof(number.value)};
    bool ok = ScanGuestText("-17 ff 1234567890123 1.25", "%d %x %lld %Lf", values, sizes) == 4 &&
        decimal == -17 && hex == 255 && wide == 1234567890123LL && number.value == 1.25 &&
        number.guard == 0x1122334455667788ULL;
    ok = ok && ScanGuestText("27", "%ld", values, sizes) == 1 && decimal == 27;
    ok = ok && ScanGuestText("bad", "%d", values, sizes) == 0 && decimal == 27;
    ok = ok && ScanGuestText("1 2 3 4 5", "%d%d%d%d%d", values, sizes) == -2;
    char left[16] = {}, right[16] = {}, last = 0;
    void* strings[4] = {left, right, &last, nullptr};
    const size_t lengths[4] = {sizeof(left), sizeof(right), sizeof(last), 0};
    ok = ok && ScanGuestText("one:two;abc!", "%15[^:]:%15[^;];%*3[a-z]%c", strings, lengths) == 3 &&
        strcmp(left, "one") == 0 && strcmp(right, "two") == 0 && last == '!';
    if (ok) LOGI("GuestScanfSelfCheck OK (integers, fp64, scansets, suppression, bounds)");
    else LOGE("GuestScanfSelfCheck FAILED");
    return ok;
}

namespace {

constexpr uint32_t kEffectXmlRootCopyReturn = 0x001E2AC8u;
constexpr uint32_t kVertexShaderRootCopyReturn = 0x001E2C70u;
constexpr uint32_t kVertexShaderNameAppendReturn = 0x001E2D04u;
constexpr uint32_t kFragmentShaderRootCopyReturn = 0x001E2D54u;
constexpr uint32_t kFragmentShaderNameAppendReturn = 0x001E2DCCu;

uint32_t LibcGuestImageOffset(uint32_t address) {
    return address & 0x00FFFFFFu;
}

bool IsEffectRootCopy(uint32_t caller) {
    return caller == kEffectXmlRootCopyReturn ||
           caller == kVertexShaderRootCopyReturn ||
           caller == kFragmentShaderRootCopyReturn;
}

bool IsEffectTechniqueAppend(uint32_t caller) {
    return caller == kVertexShaderNameAppendReturn ||
           caller == kFragmentShaderNameAppendReturn;
}

const char* RelativeShaderPrefix(uint32_t caller) {
    return caller == kVertexShaderNameAppendReturn
        ? "GLShadersOptimized/vs_" : "GLShadersOptimized/fs_";
}

// Returns true when the constructor-specific strcat has been handled.  The
// guest appends ".glsl" immediately after this call, so every branch reserves
// those five bytes plus the terminating NUL as well.
bool AppendSafeEffectTechnique(uint32_t caller, char* destination,
                               const char* requested) {
    using namespace warband::shader_path_guard;
    if (!IsEffectTechniqueAppend(caller) || !destination || !requested) {
        return false;
    }

    std::size_t destination_bytes = strnlen(destination, kGuestPathCapacity);
    const std::size_t requested_bytes = strnlen(requested, 4096u);
    const bool valid_prefix = destination_bytes >= kOptimizedPrefixBytes &&
                              destination_bytes < kGuestPathCapacity;
    const std::size_t root_bytes = valid_prefix
        ? destination_bytes - kOptimizedPrefixBytes
        : kMaxRootAndTechniqueBytes + 1u;
    Selection selected = SelectTechnique(
        root_bytes, std::string_view(requested, requested_bytes));

    if (!valid_prefix ||
        destination_bytes + selected.name.size() + kGlslSuffixAndNulBytes >
            kGuestPathCapacity) {
        // A broken root must not turn the fallback itself into another
        // overflow. Prefer the real mounted root when it fits; otherwise use a
        // short relative path. The latter can fail to open, but remains memory
        // safe even for a deliberately corrupted installation.
        std::string prefix = GameDataMainRoot();
        if (!prefix.empty()) prefix += '/';
        prefix += RelativeShaderPrefix(caller);
        if (prefix.size() + kFallbackTechnique.size() +
                kGlslSuffixAndNulBytes > kGuestPathCapacity) {
            prefix = RelativeShaderPrefix(caller);
        }
        memcpy(destination, prefix.data(), prefix.size());
        destination[prefix.size()] = '\0';
        destination_bytes = prefix.size();
        selected = {kFallbackTechnique, true};
    }

    memcpy(destination + destination_bytes, selected.name.data(),
           selected.name.size());
    destination[destination_bytes + selected.name.size()] = '\0';
    if (selected.used_fallback) {
        LOGE("unsafe shader technique replaced with '%.*s': caller=0x%08x "
             "root_bytes=%zu requested_bytes=%zu",
             static_cast<int>(kFallbackTechnique.size()),
             kFallbackTechnique.data(), caller, root_bytes, requested_bytes);
    }
    return true;
}

}  // namespace

void ThunkManager::RegisterLibcThunks() {
    uint8_t* mem = m_host.GetGuestMemoryBase();

    // -------------------------------------------------------------
    // POSIX and math thunks
    // -------------------------------------------------------------

    auto read_double_01 = [](const Dynarmic::A32::Jit& jit) -> double {
        uint64_t raw = (static_cast<uint64_t>(jit.Regs()[1]) << 32) | jit.Regs()[0];
        double val;
        memcpy(&val, &raw, sizeof(double));
        return val;
    };
    auto write_double_01 = [](Dynarmic::A32::Jit& jit, double val) {
        uint64_t raw;
        memcpy(&raw, &val, sizeof(double));
        jit.Regs()[0] = static_cast<uint32_t>(raw & 0xFFFFFFFF);
        jit.Regs()[1] = static_cast<uint32_t>((raw >> 32) & 0xFFFFFFFF);
    };

    RegisterThunk("sin", [=](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        write_double_01(jit, sin(read_double_01(jit)));
    });
    RegisterThunk("cos", [=](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        write_double_01(jit, cos(read_double_01(jit)));
    });
    RegisterThunk("tan", [=](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        write_double_01(jit, tan(read_double_01(jit)));
    });
    RegisterThunk("atan2", [=](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        double y = read_double_01(jit);
        uint64_t raw_x = (static_cast<uint64_t>(jit.Regs()[3]) << 32) | jit.Regs()[2];
        double x;
        memcpy(&x, &raw_x, sizeof(double));
        write_double_01(jit, atan2(y, x));
    });
    RegisterThunk("exp", [=](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        write_double_01(jit, exp(read_double_01(jit)));
    });
    RegisterThunk("log", [=](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        write_double_01(jit, log(read_double_01(jit)));
    });
    RegisterThunk("log10", [=](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        write_double_01(jit, log10(read_double_01(jit)));
    });
    RegisterThunk("log10f", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        float val = *reinterpret_cast<float*>(&jit.Regs()[0]);
        float res = log10f(val);
        jit.Regs()[0] = *reinterpret_cast<uint32_t*>(&res);
    });
    RegisterThunk("pow", [=](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        double x = read_double_01(jit);
        uint64_t raw_y = (static_cast<uint64_t>(jit.Regs()[3]) << 32) | jit.Regs()[2];
        double y;
        memcpy(&y, &raw_y, sizeof(double));
        write_double_01(jit, pow(x, y));
    });
    RegisterThunk("sqrt", [=](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        write_double_01(jit, sqrt(read_double_01(jit)));
    });
    RegisterThunk("floor", [=](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        write_double_01(jit, floor(read_double_01(jit)));
    });
    RegisterThunk("rint", [=](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        write_double_01(jit, rint(read_double_01(jit)));
    });
    RegisterThunk("lrintf", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        float val = *reinterpret_cast<float*>(&jit.Regs()[0]);
        jit.Regs()[0] = static_cast<uint32_t>(lrintf(val));
    });
    RegisterThunk("frexp", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint64_t raw = (static_cast<uint64_t>(jit.Regs()[1]) << 32) | jit.Regs()[0];
        double val;
        memcpy(&val, &raw, sizeof(double));
        uint32_t exp_ptr = jit.Regs()[2];
        int exp_val = 0;
        double res = frexp(val, &exp_val);
        if (exp_ptr) host.MemoryWrite32(exp_ptr, static_cast<uint32_t>(exp_val));
        uint64_t res_raw;
        memcpy(&res_raw, &res, sizeof(double));
        jit.Regs()[0] = static_cast<uint32_t>(res_raw & 0xFFFFFFFF);
        jit.Regs()[1] = static_cast<uint32_t>((res_raw >> 32) & 0xFFFFFFFF);
    });
    RegisterThunk("ldexp", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint64_t raw = (static_cast<uint64_t>(jit.Regs()[1]) << 32) | jit.Regs()[0];
        double val;
        memcpy(&val, &raw, sizeof(double));
        int exp_val = static_cast<int>(jit.Regs()[2]);
        double res = ldexp(val, exp_val);
        uint64_t res_raw;
        memcpy(&res_raw, &res, sizeof(double));
        jit.Regs()[0] = static_cast<uint32_t>(res_raw & 0xFFFFFFFF);
        jit.Regs()[1] = static_cast<uint32_t>((res_raw >> 32) & 0xFFFFFFFF);
    });

    // Memory & Strings
    RegisterThunk("memmove", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t dst = jit.Regs()[0];
        uint32_t src = jit.Regs()[1];
        uint32_t n = jit.Regs()[2];
        void* h_dst = GuestToHost(host.GetGuestMemoryBase(), dst);
        const void* h_src = GuestToHost(host.GetGuestMemoryBase(), src);
        if (h_dst && h_src && n > 0) memmove(h_dst, h_src, n);
        jit.Regs()[0] = dst;
    });
    RegisterThunk("memmem", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t haystack = jit.Regs()[0];
        uint32_t hlen = jit.Regs()[1];
        uint32_t needle = jit.Regs()[2];
        uint32_t nlen = jit.Regs()[3];
        const void* h_hay = GuestToHost(host.GetGuestMemoryBase(), haystack);
        const void* h_nee = GuestToHost(host.GetGuestMemoryBase(), needle);
        if (h_hay && h_nee && hlen >= nlen) {
            const void* found = memmem(h_hay, hlen, h_nee, nlen);
            if (found) {
                uint32_t offset = static_cast<uint32_t>(reinterpret_cast<const uint8_t*>(found) - reinterpret_cast<const uint8_t*>(h_hay));
                jit.Regs()[0] = haystack + offset;
                return;
            }
        }
        jit.Regs()[0] = 0;
    });
    RegisterThunk("strcat", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t dst = jit.Regs()[0];
        uint32_t src = jit.Regs()[1];
        char* h_dst = reinterpret_cast<char*>(GuestToHost(host.GetGuestMemoryBase(), dst));
        const char* h_src = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), src));
        const uint32_t caller = LibcGuestImageOffset(jit.Regs()[14]);
        if (h_dst && h_src &&
            !AppendSafeEffectTechnique(caller, h_dst, h_src)) {
            strcat(h_dst, h_src);
        }
        jit.Regs()[0] = dst;
    });
    RegisterThunk("strncpy", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t dst = jit.Regs()[0];
        uint32_t src = jit.Regs()[1];
        uint32_t n = jit.Regs()[2];
        char* h_dst = reinterpret_cast<char*>(GuestToHost(host.GetGuestMemoryBase(), dst));
        const char* h_src = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), src));
        if (h_dst && h_src && n > 0) strncpy(h_dst, h_src, n);
        jit.Regs()[0] = dst;
    });
    RegisterThunk("strncasecmp", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t s1 = jit.Regs()[0];
        uint32_t s2 = jit.Regs()[1];
        uint32_t n = jit.Regs()[2];
        const char* h_s1 = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), s1));
        const char* h_s2 = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), s2));
        jit.Regs()[0] = (h_s1 && h_s2) ? static_cast<uint32_t>(strncasecmp(h_s1, h_s2, n)) : 0;
    });
    RegisterThunk("strtol", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t nptr = jit.Regs()[0];
        uint32_t endptr = jit.Regs()[1];
        int base = static_cast<int>(jit.Regs()[2]);
        const char* h_nptr = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), nptr));
        if (!h_nptr) { jit.Regs()[0] = 0; return; }
        char* h_end = nullptr;
        long val = strtol(h_nptr, &h_end, base);
        if (endptr && h_end) {
            uint32_t offset = static_cast<uint32_t>(h_end - h_nptr);
            host.MemoryWrite32(endptr, nptr + offset);
        }
        jit.Regs()[0] = static_cast<uint32_t>(val);
    });
    RegisterThunk("puts", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t s = jit.Regs()[0];
        const char* str = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), s));
        if (str) LOGI("[puts] %s", str);
        jit.Regs()[0] = 1;
    });
    RegisterThunk("vsnprintf", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t buf = jit.Regs()[0];
        uint32_t size = jit.Regs()[1];
        char* h_buf = reinterpret_cast<char*>(GuestToHost(host.GetGuestMemoryBase(), buf));
        const char* h_fmt = reinterpret_cast<const char*>(
            GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[2]));
        std::string text = FormatGuestVaList(jit, host, h_fmt, jit.Regs()[3]);
        if (h_buf && size > 0) {
            size_t n = std::min(text.size(), static_cast<size_t>(size) - 1);
            memcpy(h_buf, text.data(), n);
            h_buf[n] = '\0';
        }
        // Like the real vsnprintf, the return value is what would have been
        // written, not what fitted - callers size their buffer from it.
        jit.Regs()[0] = static_cast<uint32_t>(text.size());
    });

    // Time & Threading
    RegisterThunk("gettimeofday", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t tv_ptr = jit.Regs()[0];
        if (tv_ptr) {
            struct timeval tv;
            gettimeofday(&tv, nullptr);
            host.MemoryWrite32(tv_ptr + 0, static_cast<uint32_t>(tv.tv_sec));
            host.MemoryWrite32(tv_ptr + 4, static_cast<uint32_t>(tv.tv_usec));
        }
        jit.Regs()[0] = 0;
    });
    // A no-op usleep turns every "poll, sleep a bit, poll again" loop into a spin
    // at full speed. With a dozen guest threads doing that, the thread that was
    // supposed to make progress never gets scheduled - the game reached its first
    // frames and then burned every core going nowhere.
    // Sleeping in slices keeps the sleep interruptible: on the device a signal cuts
    // it short and the handler runs, and a thread whose whole life is a short sleep
    // and a poll is otherwise unreachable - it never runs long enough in guest code
    // for the Jit halt to catch it, and Boehm's stop-the-world waits for it forever.
    RegisterThunk("usleep", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t usec = jit.Regs()[0];
        if (usec) {
            GuestSleepInterruptible(host, usec);
        } else {
            std::this_thread::yield();
            if (GuestSignalPending()) GuestSignalDeliverPending(host);
        }
        jit.Regs()[0] = 0;
    });
    RegisterThunk("lrand48", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        jit.Regs()[0] = static_cast<uint32_t>(lrand48());
    });
    RegisterThunk("srand48", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        srand48(static_cast<long>(jit.Regs()[0]));
        jit.Regs()[0] = 0;
    });
    RegisterThunk("utime", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        jit.Regs()[0] = 0;
    });
    RegisterThunk("unlink", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        jit.Regs()[0] = 0;
    });
    RegisterThunk("chown", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        jit.Regs()[0] = 0;
    });
    // Semaphores & Pthread
    RegisterThunk("sem_init", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        GuestSem& sem = GuestSemFor(jit.Regs()[0]);
        std::lock_guard<std::mutex> lock(sem.m);
        sem.count = static_cast<int>(jit.Regs()[2]);
        jit.Regs()[0] = 0;
    });
    RegisterThunk("sem_destroy", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        GuestSyncForget(jit.Regs()[0], 4);
        jit.Regs()[0] = 0;
    });
    // A blocked thread still has to be able to run a signal handler, otherwise
    // Boehm's stop-the-world waits forever on a thread that is parked in here.
    // The wait is therefore timed, and the lock is dropped before the handler runs
    // so that a handler touching another guest primitive cannot deadlock on it.
    RegisterThunk("sem_wait", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        GuestSem& sem = GuestSemFor(jit.Regs()[0]);
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(sem.m);
                if (sem.cv.wait_for(lock, std::chrono::milliseconds(20),
                                    [&sem] { return sem.count > 0; })) {
                    --sem.count;
                    break;
                }
            }
            if (GuestSignalPending()) GuestSignalDeliverPending(host);
        }
        jit.Regs()[0] = 0;
    });
    RegisterThunk("sem_trywait", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        GuestSem& sem = GuestSemFor(jit.Regs()[0]);
        std::lock_guard<std::mutex> lock(sem.m);
        if (sem.count > 0) {
            --sem.count;
            jit.Regs()[0] = 0;
        } else {
            jit.Regs()[0] = 0xFFFFFFFF;  // EAGAIN
        }
    });
    RegisterThunk("sem_post", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        GuestSem& sem = GuestSemFor(jit.Regs()[0]);
        {
            std::lock_guard<std::mutex> lock(sem.m);
            ++sem.count;
        }
        sem.cv.notify_one();
        jit.Regs()[0] = 0;
    });
    // Boehm's retrying stop-the-world path polls the acknowledgement semaphore
    // before deciding which threads need another suspend signal. Returning a
    // hard-coded zero here made every successful acknowledgement invisible and
    // is precisely the wrong answer when GC_RETRY_SIGNALS is enabled.
    RegisterThunk("sem_getvalue", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        GuestSem& sem = GuestSemFor(jit.Regs()[0]);
        int value;
        {
            std::lock_guard<std::mutex> lock(sem.m);
            value = sem.count;
        }
        if (jit.Regs()[1]) host.MemoryWrite32(jit.Regs()[1], static_cast<uint32_t>(value));
        jit.Regs()[0] = 0;
    });
    RegisterThunk("pthread_self", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        jit.Regs()[0] = GuestSelfId();
    });
    RegisterThunk("pthread_equal", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        jit.Regs()[0] = (jit.Regs()[0] == jit.Regs()[1]) ? 1 : 0;
    });
    RegisterThunk("pthread_attr_init", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        jit.Regs()[0] = 0;
    });
    RegisterThunk("pthread_attr_destroy", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        jit.Regs()[0] = 0;
    });
    RegisterThunk("pthread_attr_setdetachstate", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        jit.Regs()[0] = 0;
    });
    RegisterThunk("pthread_attr_setstacksize", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        jit.Regs()[0] = 0;
    });
    RegisterThunk("pthread_mutex_init", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t mtx = jit.Regs()[0];
        if (mtx) host.MemoryWrite32(mtx, 0);
        jit.Regs()[0] = 0;
    });
    RegisterThunk("pthread_mutex_destroy", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        jit.Regs()[0] = 0;
    });
    RegisterThunk("pthread_mutexattr_init", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        jit.Regs()[0] = 0;
    });
    RegisterThunk("pthread_mutexattr_settype", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        jit.Regs()[0] = 0;
    });
    // A real host thread with its own Jit and guest stack. The reference bridge only
    // recorded the worker and poked FMOD's Thread::m_running flags through the
    // argument struct to fake it; Unity's argument is a different struct entirely, so
    // that both corrupted it and left the creator waiting forever.
    RegisterThunk("pthread_create", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t thread_out = jit.Regs()[0];
        uint32_t start_func = jit.Regs()[2];
        uint32_t arg = jit.Regs()[3];

        auto handle = std::make_shared<GuestThreadHandle>();
        uint32_t tid = g_next_guest_tid.fetch_add(1);
        {
            std::lock_guard<std::mutex> lock(g_sync_registry);
            g_guest_threads[tid] = handle;
        }
        LOGI("pthread_create: tid=0x%X func=0x%08X arg=0x%08X", tid, start_func, arg);

        SpawnHostThread([&host, handle, tid, start_func, arg] {
            t_guest_tid = tid;
            // pthread_t and the kernel tid are two different id spaces here: the
            // creator is handed this fake id before the thread exists, while the
            // signal table is keyed by gettid(). pthread_kill takes the former.
            handle->kernel_tid.store(gettid());
            uint32_t ret = host.RunFunction(start_func, {arg});
            LOGI("guest thread 0x%X (tid %d) returned", tid, gettid());
            // The kernel tid is about to be handed to some future thread; the
            // signal table must not still point at this one.
            GuestSignalUnregisterThread(gettid());
            {
                std::lock_guard<std::mutex> lock(handle->m);
                handle->finished = true;
                handle->retval = ret;
            }
            handle->cv.notify_all();
        });

        if (thread_out) host.MemoryWrite32(thread_out, tid);
        jit.Regs()[0] = 0;  // Return OK
    });
    // Boehm signals its threads by pthread_t, which in this bridge is the id handed
    // out by pthread_create - not the kernel tid the signal table is keyed by. Without
    // the translation every pthread_kill answered ESRCH, and a collector that believes
    // a thread is gone never waits for it to stop.
    GuestKernelTidLookup = [](uint32_t guest_tid) -> int {
        std::lock_guard<std::mutex> lock(g_sync_registry);
        auto it = g_guest_threads.find(guest_tid);
        if (it != g_guest_threads.end()) {
            int kernel = it->second->kernel_tid.load();
            if (kernel) return kernel;
        }
        std::lock_guard<std::mutex> self_lock(g_self_ids_mutex);
        auto self = g_self_ids.find(guest_tid);
        return self == g_self_ids.end() ? 0 : self->second;
    };
    RegisterThunk("pthread_join", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t tid = jit.Regs()[0];
        uint32_t retval_out = jit.Regs()[1];
        std::shared_ptr<GuestThreadHandle> handle;
        {
            std::lock_guard<std::mutex> lock(g_sync_registry);
            auto it = g_guest_threads.find(tid);
            if (it != g_guest_threads.end()) handle = it->second;
        }
        if (handle) {
            for (;;) {
                std::unique_lock<std::mutex> lock(handle->m);
                if (handle->cv.wait_for(lock, std::chrono::milliseconds(20),
                                        [&handle] { return handle->finished; })) {
                    break;
                }
                lock.unlock();
                if (GuestSignalPending()) GuestSignalDeliverPending(host);
            }
            if (retval_out) host.MemoryWrite32(retval_out, handle->retval);
        }
        jit.Regs()[0] = 0;
    });
    RegisterThunk("pthread_detach", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        jit.Regs()[0] = 0;
    });

    // Android & Runtime
    RegisterThunk("__android_log_write", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        int prio = static_cast<int>(jit.Regs()[0]);
        uint32_t tag_ptr = jit.Regs()[1];
        uint32_t msg_ptr = jit.Regs()[2];
        const char* tag = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), tag_ptr));
        const char* msg = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), msg_ptr));
        if (!GuestLogAllowed(prio, tag)) { jit.Regs()[0] = 1; return; }
        if (tag && msg) __android_log_write(prio, tag, msg);
        jit.Regs()[0] = 1;
    });
    // ponytail: process-lifetime bootstrap does not run guest exit handlers; retain
    // the registrations later if orderly guest unloading becomes a tested requirement.
    RegisterThunk("atexit", [](Dynarmic::A32::Jit& jit, DynarmicHost&) { jit.Regs()[0] = 0; });
    RegisterThunk("__aeabi_atexit", [](Dynarmic::A32::Jit& jit, DynarmicHost&) { jit.Regs()[0] = 0; });
    RegisterThunk("__cxa_guard_acquire", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t guard = jit.Regs()[0];
        if (!guard) { jit.Regs()[0] = 0; return; }
        std::unique_lock<std::mutex> lock(g_sync_registry);
        g_guard_cv.wait(lock, [guard, &host] {
            return host.MemoryRead8(guard) != 0 || !g_guards_in_progress.count(guard);
        });
        if (host.MemoryRead8(guard) != 0) {
            jit.Regs()[0] = 0;
            return;
        }
        g_guards_in_progress.insert(guard);
        jit.Regs()[0] = 1;
    });
    RegisterThunk("__cxa_guard_release", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t guard = jit.Regs()[0];
        {
            std::lock_guard<std::mutex> lock(g_sync_registry);
            if (guard) host.MemoryWrite8(guard, 1);
            g_guards_in_progress.erase(guard);
        }
        g_guard_cv.notify_all();
        jit.Regs()[0] = 0;
    });
    RegisterThunk("__cxa_guard_abort", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        {
            std::lock_guard<std::mutex> lock(g_sync_registry);
            g_guards_in_progress.erase(jit.Regs()[0]);
        }
        g_guard_cv.notify_all();
        jit.Regs()[0] = 0;
    });
    RegisterThunk("__cxa_pure_virtual", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        LOGE("__cxa_pure_virtual called!");
        jit.Regs()[0] = 0;
    });
    RegisterThunk("__cxa_type_match", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) { jit.Regs()[0] = 0; });
    RegisterThunk("__cxa_begin_cleanup", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) { jit.Regs()[0] = 0; });
    RegisterThunk("__cxa_call_unexpected", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) { jit.Regs()[0] = 0; });
    RegisterThunk("__gnu_Unwind_Find_exidx", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) { jit.Regs()[0] = 0; });
    // Handles name a specific loaded image. The old constant handle meant
    // dlsym(libmono_handle, "JNI_OnLoad") resolved against whichever library
    // happened to come last in the search order, which is how libunity's
    // JNI_OnLoad ended up being run three times and libmono's never.
    RegisterThunk("dlopen", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* path = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        std::string soname;
        if (path) {
            soname = path;
            size_t slash = soname.find_last_of('/');
            if (slash != std::string::npos) soname.erase(0, slash + 1);
        }
        uint32_t handle = kDlHandleBase;  // "the whole process", i.e. global search
        // Callers do not all spell a library the same way. DllImport names may omit
        // the lib prefix and .so suffix that the real loader adds back. Matching only
        // the exact string handed the caller a global handle instead, and a global
        // handle finds nothing that lives in that one image.
        if (!soname.empty()) {
            for (const std::string& candidate :
                 {soname, "lib" + soname + ".so", soname + ".so"}) {
                if (!host.GetLibrary(candidate)) continue;
                std::lock_guard<std::mutex> lock(g_sync_registry);
                g_dl_handles.push_back(candidate);
                handle = kDlHandleBase + static_cast<uint32_t>(g_dl_handles.size());
                break;
            }
        }
        // Mono probes `<assembly>.dll.so` before falling back to its JIT. These
        // AOT images are not packaged, so a real dlopen would fail. Returning the
        // global handle made each of the 25 misses look successful and triggered
        // another four global dlsym searches per start. Besides wasting work, a
        // global runtime symbol is not a symbol from the requested AOT module.
        if (handle == kDlHandleBase && soname.size() >= 7 &&
            soname.compare(soname.size() - 7, 7, ".dll.so") == 0) {
            t_dl_error = "guest AOT image not found: " + soname;
            jit.Regs()[0] = 0;
            return;
        }
        LOGI("dlopen('%s', %d) -> 0x%08X", path ? path : "NULL", jit.Regs()[1], handle);
        jit.Regs()[0] = handle;
    });
    RegisterThunk("dlclose", [](Dynarmic::A32::Jit& jit, DynarmicHost&) { jit.Regs()[0] = 0; });
    RegisterThunk("dlerror", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        if (t_dl_error.empty()) {
            jit.Regs()[0] = 0;
            return;
        }
        if (!t_dl_error_guest) t_dl_error_guest = host.GuestAlloc(256, 8);
        if (!t_dl_error_guest) {
            jit.Regs()[0] = 0;
            return;
        }
        char* out = reinterpret_cast<char*>(host.GetGuestMemoryBase() + t_dl_error_guest);
        snprintf(out, 256, "%s", t_dl_error.c_str());
        t_dl_error.clear();
        jit.Regs()[0] = t_dl_error_guest;
    });
    RegisterThunk("dlsym", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t handle = jit.Regs()[0];
        const char* sym = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]));
        if (!sym) { jit.Regs()[0] = 0; return; }

        // A handle that names one image resolves inside it and nowhere else.
        std::string scoped;
        if (handle > kDlHandleBase) {
            std::lock_guard<std::mutex> lock(g_sync_registry);
            uint32_t index = handle - kDlHandleBase - 1;
            if (index < g_dl_handles.size()) scoped = g_dl_handles[index];
        }
        if (!scoped.empty()) {
            ElfLoader* lib = host.GetLibrary(scoped);
            uint32_t addr = lib ? lib->GetSymbolAddress(sym) : 0;
            LOGI("dlsym(%s, '%s') -> 0x%08X", scoped.c_str(), sym, addr);
            jit.Regs()[0] = addr;
            return;
        }

        LOGI("dlsym(global, '%s')", sym);
        auto it = host.GetThunkManager().GetThunkSymbolTable().find(sym);
        if (it != host.GetThunkManager().GetThunkSymbolTable().end()) {
            jit.Regs()[0] = it->second;
            return;
        }
        uint32_t guest = host.FindSymbol(sym);
        jit.Regs()[0] = guest ? guest : host.GetThunkManager().GetOrCreateThunkAddress(sym);
    });

    RegisterThunk("__system_property_get", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* name = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        uint32_t val_ptr = jit.Regs()[1];
        LOGI("__system_property_get('%s')", name ? name : "null");
        if (name && val_ptr) {
            char* out = reinterpret_cast<char*>(GuestToHost(host.GetGuestMemoryBase(), val_ptr));
            if (strcmp(name, "ro.build.version.sdk") == 0) {
                strcpy(out, "28");
                jit.Regs()[0] = 2;
                return;
            }
            strcpy(out, "");
        }
        jit.Regs()[0] = 0;
    });

    // C++ Memory Allocation operators
    RegisterThunk("_Znaj", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        size_t size = jit.Regs()[0];
        jit.Regs()[0] = host.GuestAlloc(size, 8);
    });

    RegisterThunk("_Znwj", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        size_t size = jit.Regs()[0];
        jit.Regs()[0] = host.GuestAlloc(size, 8);
    });

    RegisterThunk("_ZnajRKSt9nothrow_t", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        size_t size = jit.Regs()[0];
        jit.Regs()[0] = host.GuestAlloc(size, 8);
    });

    RegisterThunk("_ZnwjRKSt9nothrow_t", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        size_t size = jit.Regs()[0];
        jit.Regs()[0] = host.GuestAlloc(size, 8);
    });

    RegisterThunk("_ZdaPv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        host.GuestFree(jit.Regs()[0]);
    });

    RegisterThunk("_ZdlPv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        host.GuestFree(jit.Regs()[0]);
    });

    RegisterThunk("_ZNSt8ios_base4InitC1Ev", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {});
    RegisterThunk("_ZNSt8ios_base4InitD1Ev", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {});

    // Memory Allocation inside guest space
    RegisterThunk("malloc", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        size_t size = jit.Regs()[0];
        jit.Regs()[0] = host.GuestAlloc(size, 8);
    });

    RegisterThunk("calloc", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        size_t num = jit.Regs()[0];
        size_t size = jit.Regs()[1];
        uint32_t addr = host.GuestAlloc(num * size, 8);
        if (addr) {
            memset(GuestToHost(host.GetGuestMemoryBase(), addr), 0, num * size);
        }
        jit.Regs()[0] = addr;
    });

    // Copying new_size bytes out of the old block reads past its end whenever the
    // block is growing, which used to be harmless only because nothing was ever
    // reused. The allocator knows the real size, so ask it.
    RegisterThunk("realloc", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t old_addr = jit.Regs()[0];
        size_t new_size = jit.Regs()[1];
        if (!old_addr) { jit.Regs()[0] = host.GuestAlloc(new_size, 8); return; }
        if (new_size == 0) { host.GuestFree(old_addr); jit.Regs()[0] = 0; return; }
        uint32_t old_size = host.GuestAllocSize(old_addr);
        if (old_size >= new_size) { jit.Regs()[0] = old_addr; return; }
        uint32_t new_addr = host.GuestAlloc(new_size, 8);
        if (!new_addr) { jit.Regs()[0] = 0; return; }
        if (!old_size) LOGE("realloc of 0x%08X, which this allocator never handed out", old_addr);
        size_t copy = old_size ? old_size : new_size;
        memcpy(GuestToHost(host.GetGuestMemoryBase(), new_addr),
               GuestToHost(host.GetGuestMemoryBase(), old_addr), copy);
        host.GuestFree(old_addr);
        jit.Regs()[0] = new_addr;
    });

    RegisterThunk("free", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        host.GuestFree(jit.Regs()[0]);
    });

    RegisterThunk("memalign", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        jit.Regs()[0] = host.GuestAlloc(jit.Regs()[1], jit.Regs()[0]);
    });

    // Logging & Formatted output
    RegisterThunk("__android_log_print", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        int prio = static_cast<int>(jit.Regs()[0]);
        const char* tag = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]));
        const char* fmt = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[2]));
        std::string formatted = FormatGuestString(jit, host, fmt, 3);
        // The guest's own logging has to obey `log.tag.<TAG>` like everyone else's:
        // guest audio narrates every 16 KiB it reads, which is several hundred lines while
        // a sound bank loads, and this phone drops a process's log lines once it
        // exceeds a quota - taking the lines that were being read with them.
        // `adb shell setprop log.tag.guest audio S` now silences it at the source.
        if (!GuestLogAllowed(prio, tag)) { jit.Regs()[0] = 0; return; }
        __android_log_print(prio, tag ? tag : "CMR_Guest", "[GUEST] %s", formatted.c_str());
        jit.Regs()[0] = 0;
    });

    RegisterThunk("printf", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* fmt = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        std::string formatted = FormatGuestString(jit, host, fmt, 1);
        LOGI("[printf] %s", formatted.c_str());
        jit.Regs()[0] = formatted.length();
    });

    RegisterThunk("fprintf", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t guest_file = jit.Regs()[0];
        const char* fmt = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]));
        std::string formatted = FormatGuestString(jit, host, fmt, 2);
        LOGI("[fprintf] %s", formatted.c_str());
        jit.Regs()[0] = static_cast<uint32_t>(warband::guest_stdio::WriteBytes(
            g_file_map, g_file_mutex, guest_file, formatted.data(), formatted.size()));
    });

    RegisterThunk("sprintf", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        char* str = reinterpret_cast<char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        const char* fmt = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]));
        std::string formatted = FormatGuestString(jit, host, fmt, 2);
        if (str) {
            strcpy(str, formatted.c_str());
            jit.Regs()[0] = formatted.length();
        } else {
            jit.Regs()[0] = 0;
        }
    });

    RegisterThunk("snprintf", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        char* str = reinterpret_cast<char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        size_t size = jit.Regs()[1];
        const char* fmt = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[2]));
        std::string formatted = FormatGuestString(jit, host, fmt, 3);
        if (str && size > 0) {
            strncpy(str, formatted.c_str(), size - 1);
            str[size - 1] = '\0';
            jit.Regs()[0] = formatted.length();
        } else {
            jit.Regs()[0] = 0;
        }
    });

    if (!GuestScanfSelfCheck()) m_registration_error = true;
    RegisterThunk("sscanf", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const size_t memory_size = host.GetGuestMemorySize();
        auto string_at = [&](uint32_t address, size_t limit) -> const char* {
            if (!address || address >= memory_size) return nullptr;
            const char* text = reinterpret_cast<const char*>(host.GetGuestMemoryBase() + address);
            const size_t available = std::min(limit, memory_size - address);
            return strnlen(text, available) < available ? text : nullptr;
        };
        const char* text = string_at(jit.Regs()[0], 1024 * 1024);
        const char* format = string_at(jit.Regs()[1], 4096);
        if (!text || !format) {
            LOGE("sscanf invalid/unterminated guest string at LR=0x%08X", jit.Regs()[14]);
            jit.Regs()[0] = static_cast<uint32_t>(EOF);
            return;
        }
        void* outputs[4] = {};
        size_t capacity[4] = {};
        for (unsigned i = 0; i < 4; ++i) {
            const uint64_t slot = static_cast<uint64_t>(jit.Regs()[13]) + (i >= 2 ? (i - 2) * 4 : 0);
            const uint32_t address = i < 2 ? jit.Regs()[i + 2]
                : slot + 4 <= memory_size ? host.MemoryRead32(static_cast<uint32_t>(slot)) : 0;
            if (address && address < memory_size) {
                outputs[i] = host.GetGuestMemoryBase() + address;
                capacity[i] = memory_size - address;
            }
        }
        int result = ScanGuestText(text, format, outputs, capacity);
        if (result == -2) {
            LOGE("sscanf unsupported format/output at LR=0x%08X: %.160s", jit.Regs()[14], format);
            result = EOF;
        }
        jit.Regs()[0] = static_cast<uint32_t>(result);
    });

    // eglib builds every string it hands back - log lines, assembly search
    // paths, type names - with g_strdup_printf, which is vasprintf underneath.
    // A stub that reports success without writing *strp leaves the caller
    // holding whatever was on the stack, so Mono ends up probing garbage paths
    // and reporting that mscorlib.dll is missing.
    auto guest_asprintf = [](Dynarmic::A32::Jit& jit, DynarmicHost& host, const std::string& text) {
        uint32_t strp = jit.Regs()[0];
        uint32_t out = host.GuestAlloc(text.size() + 1, 1);
        if (!out || !strp) { jit.Regs()[0] = 0xFFFFFFFF; return; }
        memcpy(host.GetGuestMemoryBase() + out, text.c_str(), text.size() + 1);
        host.MemoryWrite32(strp, out);
        jit.Regs()[0] = static_cast<uint32_t>(text.size());
    };
    RegisterThunk("vasprintf", [guest_asprintf](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* fmt = reinterpret_cast<const char*>(
            GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]));
        guest_asprintf(jit, host, FormatGuestVaList(jit, host, fmt, jit.Regs()[2]));
    });
    RegisterThunk("asprintf", [guest_asprintf](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* fmt = reinterpret_cast<const char*>(
            GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]));
        guest_asprintf(jit, host, FormatGuestString(jit, host, fmt, 2));
    });
    RegisterThunk("vsprintf", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        char* h_buf = reinterpret_cast<char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        const char* fmt = reinterpret_cast<const char*>(
            GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]));
        std::string text = FormatGuestVaList(jit, host, fmt, jit.Regs()[2]);
        if (h_buf) memcpy(h_buf, text.c_str(), text.size() + 1);
        jit.Regs()[0] = static_cast<uint32_t>(text.size());
    });
    auto guest_putc = [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        const int character = static_cast<int32_t>(jit.Regs()[0]);
        const uint32_t guest_file = jit.Regs()[1];
        jit.Regs()[0] = static_cast<uint32_t>(
            warband::guest_stdio::PutCharacter(g_file_map, g_file_mutex, guest_file, character));
    };
    RegisterThunk("putc", guest_putc);
    RegisterThunk("fputc", guest_putc);
    RegisterThunk("fputs", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* text = reinterpret_cast<const char*>(
            GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        jit.Regs()[0] = static_cast<uint32_t>(warband::guest_stdio::PutString(
            g_file_map, g_file_mutex, jit.Regs()[1], text));
    });

    // Math functions
    RegisterThunk("sinf", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        float val = *reinterpret_cast<float*>(&jit.Regs()[0]);
        float res = sinf(val);
        jit.Regs()[0] = *reinterpret_cast<uint32_t*>(&res);
    });

    RegisterThunk("cosf", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        float val = *reinterpret_cast<float*>(&jit.Regs()[0]);
        float res = cosf(val);
        jit.Regs()[0] = *reinterpret_cast<uint32_t*>(&res);
    });

    RegisterThunk("sqrtf", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        float val = *reinterpret_cast<float*>(&jit.Regs()[0]);
        float res = sqrtf(val);
        jit.Regs()[0] = *reinterpret_cast<uint32_t*>(&res);
    });

    RegisterThunk("atanf", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        float val = *reinterpret_cast<float*>(&jit.Regs()[0]);
        float res = atanf(val);
        jit.Regs()[0] = *reinterpret_cast<uint32_t*>(&res);
    });

    RegisterThunk("acosf", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        float val;
        memcpy(&val, &jit.Regs()[0], sizeof(val));
        float res = acosf(val);
        memcpy(&jit.Regs()[0], &res, sizeof(res));
    });
    auto unary_float = [this](const char* name, float (*fn)(float)) {
        RegisterThunk(name, [fn](Dynarmic::A32::Jit& jit, DynarmicHost&) {
            float value;
            memcpy(&value, &jit.Regs()[0], sizeof(value));
            float result = fn(value);
            memcpy(&jit.Regs()[0], &result, sizeof(result));
        });
    };
    unary_float("asinf", asinf);
    unary_float("ceilf", ceilf);
    unary_float("expf", expf);
    unary_float("floorf", floorf);
    unary_float("logf", logf);
    unary_float("tanf", tanf);
    RegisterThunk("atan2f", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        float y;
        float x;
        memcpy(&y, &jit.Regs()[0], sizeof(y));
        memcpy(&x, &jit.Regs()[1], sizeof(x));
        float result = atan2f(y, x);
        memcpy(&jit.Regs()[0], &result, sizeof(result));
    });
    RegisterThunk("fmodf", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        float x;
        float y;
        memcpy(&x, &jit.Regs()[0], sizeof(x));
        memcpy(&y, &jit.Regs()[1], sizeof(y));
        float result = fmodf(x, y);
        memcpy(&jit.Regs()[0], &result, sizeof(result));
    });
    RegisterThunk("modff", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        float value;
        memcpy(&value, &jit.Regs()[0], sizeof(value));
        float integral = 0.0f;
        float result = modff(value, &integral);
        if (jit.Regs()[1]) memcpy(host.GetGuestMemoryBase() + jit.Regs()[1], &integral, sizeof(integral));
        memcpy(&jit.Regs()[0], &result, sizeof(result));
    });

    RegisterThunk("powf", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        float base = *reinterpret_cast<float*>(&jit.Regs()[0]);
        float exp = *reinterpret_cast<float*>(&jit.Regs()[1]);
        float res = powf(base, exp);
        jit.Regs()[0] = *reinterpret_cast<uint32_t*>(&res);
    });

    // String & Memory functions
    RegisterThunk("strcmp", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* s1 = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        const char* s2 = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]));
        jit.Regs()[0] = (s1 && s2) ? strcmp(s1, s2) : 0;
    });

    RegisterThunk("strncmp", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* s1 = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        const char* s2 = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]));
        size_t n = jit.Regs()[2];
        int r = (s1 && s2) ? strncmp(s1, s2, n) : 0;
        jit.Regs()[0] = static_cast<uint32_t>(r);
    });

    RegisterThunk("strcasecmp", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* s1 = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        const char* s2 = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]));
        jit.Regs()[0] = (s1 && s2) ? strcasecmp(s1, s2) : 0;
    });

    RegisterThunk("strlen", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* s = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        size_t n = s ? strlen(s) : 0;
        jit.Regs()[0] = static_cast<uint32_t>(n);
    });

    RegisterThunk("strcpy", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        char* dest = reinterpret_cast<char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        const char* src = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]));
        if (!dest || !src) return;
        const uint32_t caller = LibcGuestImageOffset(jit.Regs()[14]);
        if (IsEffectRootCopy(caller)) {
            const std::size_t source_bytes = strnlen(src, 4096u);
            if (source_bytes >
                warband::shader_path_guard::kMaxRootAndTechniqueBytes) {
                // The constructor performs additional inlined writes after
                // strcpy, so truncation at 255 would still overflow. An empty
                // root keeps those writes in bounds and lets the later shader
                // name guard rebuild a known-safe fallback path.
                dest[0] = '\0';
                LOGE("oversized shader root rejected: caller=0x%08x bytes=%zu",
                     caller, source_bytes);
                return;
            }
            memcpy(dest, src, source_bytes + 1u);
            return;
        }
        strcpy(dest, src);
    });

    RegisterThunk("strdup", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* src = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        if (!src) { jit.Regs()[0] = 0; return; }
        size_t len = strlen(src) + 1;
        uint32_t dest_addr = host.GuestAlloc(len);
        memcpy(GuestToHost(host.GetGuestMemoryBase(), dest_addr), src, len);
        jit.Regs()[0] = dest_addr;
    });

    // strerror hands back storage the caller does not own and never frees, so a
    // fresh block per call is a block per failed open - and Mono probes for files
    // that are not there constantly. One block per errno value, reused.
    RegisterThunk("strerror", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        static std::unordered_map<int, uint32_t> cache;
        int err_no = static_cast<int>(jit.Regs()[0]);
        std::lock_guard<std::mutex> lock(g_sync_registry);
        auto it = cache.find(err_no);
        if (it == cache.end()) {
            const char* err = strerror(err_no);
            size_t len = strlen(err) + 1;
            uint32_t dest = host.GuestAlloc(len);
            if (dest) memcpy(GuestToHost(host.GetGuestMemoryBase(), dest), err, len);
            it = cache.emplace(err_no, dest).first;
        }
        jit.Regs()[0] = it->second;
    });

    RegisterThunk("strtoul", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* nptr = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        int base = static_cast<int>(jit.Regs()[2]);
        jit.Regs()[0] = nptr ? static_cast<uint32_t>(strtoul(nptr, nullptr, base)) : 0;
    });

    RegisterThunk("atoi", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* str = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        jit.Regs()[0] = str ? atoi(str) : 0;
    });

    RegisterThunk("memset", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        void* dst = GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]);
        if (dst) memset(dst, static_cast<int>(jit.Regs()[1]), jit.Regs()[2]);
    });

    RegisterThunk("memcpy", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        void* dest = GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]);
        const void* src = GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]);
        size_t n = jit.Regs()[2];
        if (dest && src) memcpy(dest, src, n);
    });

    RegisterThunk("memcmp", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t left = jit.Regs()[0];
        uint32_t right = jit.Regs()[1];
        uint32_t n = jit.Regs()[2];
        const void* h_left = GuestToHost(host.GetGuestMemoryBase(), left);
        const void* h_right = GuestToHost(host.GetGuestMemoryBase(), right);
        jit.Regs()[0] = (h_left && h_right) ? static_cast<uint32_t>(memcmp(h_left, h_right, n)) : 0;
    });

    // ARM AEABI Helpers
    RegisterThunk("__aeabi_idiv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        int32_t num = static_cast<int32_t>(jit.Regs()[0]);
        int32_t den = static_cast<int32_t>(jit.Regs()[1]);
        jit.Regs()[0] = den ? (num / den) : 0;
    });

    RegisterThunk("__aeabi_uidiv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t num = jit.Regs()[0];
        uint32_t den = jit.Regs()[1];
        jit.Regs()[0] = den ? (num / den) : 0;
    });

    RegisterThunk("__aeabi_idivmod", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        int32_t num = static_cast<int32_t>(jit.Regs()[0]);
        int32_t den = static_cast<int32_t>(jit.Regs()[1]);
        if (den != 0) {
            jit.Regs()[0] = num / den;
            jit.Regs()[1] = num % den;
        } else {
            jit.Regs()[0] = 0;
            jit.Regs()[1] = 0;
        }
    });

    RegisterThunk("__aeabi_uidivmod", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t num = jit.Regs()[0];
        uint32_t den = jit.Regs()[1];
        if (den != 0) {
            jit.Regs()[0] = num / den;
            jit.Regs()[1] = num % den;
        } else {
            jit.Regs()[0] = 0;
            jit.Regs()[1] = 0;
        }
    });

    RegisterThunk("__aeabi_uldivmod", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint64_t num = static_cast<uint64_t>(jit.Regs()[0]) | (static_cast<uint64_t>(jit.Regs()[1]) << 32);
        uint64_t den = static_cast<uint64_t>(jit.Regs()[2]) | (static_cast<uint64_t>(jit.Regs()[3]) << 32);
        if (den != 0) {
            uint64_t q = num / den;
            uint64_t r = num % den;
            jit.Regs()[0] = static_cast<uint32_t>(q & 0xFFFFFFFF);
            jit.Regs()[1] = static_cast<uint32_t>(q >> 32);
            jit.Regs()[2] = static_cast<uint32_t>(r & 0xFFFFFFFF);
            jit.Regs()[3] = static_cast<uint32_t>(r >> 32);
        }
    });

    RegisterThunk("__gnu_ldivmod_helper", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        int64_t num = static_cast<int64_t>(jit.Regs()[0]) | (static_cast<int64_t>(jit.Regs()[1]) << 32);
        int64_t den = static_cast<int64_t>(jit.Regs()[2]) | (static_cast<int64_t>(jit.Regs()[3]) << 32);
        if (den != 0) {
            int64_t q = num / den;
            int64_t r = num % den;
            jit.Regs()[0] = static_cast<uint32_t>(q & 0xFFFFFFFF);
            jit.Regs()[1] = static_cast<uint32_t>(q >> 32);
            jit.Regs()[2] = static_cast<uint32_t>(r & 0xFFFFFFFF);
            jit.Regs()[3] = static_cast<uint32_t>(r >> 32);
        }
    });

    // The AEABI memory helpers are registered further down, next to each other.

    RegisterThunk("__aeabi_f2lz", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        float f = *reinterpret_cast<float*>(&jit.Regs()[0]);
        int64_t i = static_cast<int64_t>(f);
        jit.Regs()[0] = static_cast<uint32_t>(i & 0xFFFFFFFF);
        jit.Regs()[1] = static_cast<uint32_t>(i >> 32);
    });

    RegisterThunk("__aeabi_f2ulz", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        float f = *reinterpret_cast<float*>(&jit.Regs()[0]);
        uint64_t u = static_cast<uint64_t>(f);
        jit.Regs()[0] = static_cast<uint32_t>(u & 0xFFFFFFFF);
        jit.Regs()[1] = static_cast<uint32_t>(u >> 32);
    });

    RegisterThunk("__aeabi_l2f", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        int64_t i = static_cast<int64_t>(jit.Regs()[0]) | (static_cast<int64_t>(jit.Regs()[1]) << 32);
        float f = static_cast<float>(i);
        jit.Regs()[0] = *reinterpret_cast<uint32_t*>(&f);
    });

    RegisterThunk("__aeabi_ul2f", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint64_t u = static_cast<uint64_t>(jit.Regs()[0]) | (static_cast<uint64_t>(jit.Regs()[1]) << 32);
        float f = static_cast<float>(u);
        jit.Regs()[0] = *reinterpret_cast<uint32_t*>(&f);
    });

    RegisterThunk("__aeabi_l2d", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        int64_t i = static_cast<int64_t>(jit.Regs()[0]) | (static_cast<int64_t>(jit.Regs()[1]) << 32);
        double d = static_cast<double>(i);
        uint64_t u = *reinterpret_cast<uint64_t*>(&d);
        jit.Regs()[0] = static_cast<uint32_t>(u & 0xFFFFFFFF);
        jit.Regs()[1] = static_cast<uint32_t>(u >> 32);
    });

    // AEABI memory functions
    auto aeabi_memclr_handler = [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t dest = jit.Regs()[0];
        uint32_t n = jit.Regs()[1];
        if (dest && n) {
            memset(GuestToHost(host.GetGuestMemoryBase(), dest), 0, n);
        }
    };
    RegisterThunk("__aeabi_memclr", aeabi_memclr_handler);
    RegisterThunk("__aeabi_memclr4", aeabi_memclr_handler);
    RegisterThunk("__aeabi_memclr8", aeabi_memclr_handler);

    auto aeabi_memset_handler = [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t dest = jit.Regs()[0];
        uint32_t n = jit.Regs()[1];
        int c = static_cast<int>(jit.Regs()[2]);
        if (dest && n) {
            memset(GuestToHost(host.GetGuestMemoryBase(), dest), c, n);
        }
    };
    RegisterThunk("__aeabi_memset", aeabi_memset_handler);
    RegisterThunk("__aeabi_memset4", aeabi_memset_handler);
    RegisterThunk("__aeabi_memset8", aeabi_memset_handler);

    auto aeabi_memcpy_handler = [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t dest = jit.Regs()[0];
        uint32_t src = jit.Regs()[1];
        uint32_t n = jit.Regs()[2];
        if (dest && src && n) {
            memcpy(GuestToHost(host.GetGuestMemoryBase(), dest),
                   GuestToHost(host.GetGuestMemoryBase(), src), n);
        }
    };
    RegisterThunk("__aeabi_memcpy", aeabi_memcpy_handler);
    RegisterThunk("__aeabi_memcpy4", aeabi_memcpy_handler);
    RegisterThunk("__aeabi_memcpy8", aeabi_memcpy_handler);

    auto aeabi_memmove_handler = [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t dest = jit.Regs()[0];
        uint32_t src = jit.Regs()[1];
        uint32_t n = jit.Regs()[2];
        if (dest && src && n) {
            memmove(GuestToHost(host.GetGuestMemoryBase(), dest),
                    GuestToHost(host.GetGuestMemoryBase(), src), n);
        }
    };
    RegisterThunk("__aeabi_memmove", aeabi_memmove_handler);
    RegisterThunk("__aeabi_memmove4", aeabi_memmove_handler);
    RegisterThunk("__aeabi_memmove8", aeabi_memmove_handler);

    // Pthreads
    // Waiting for a mutex has to stay interruptible. Boehm's stop-the-world reaches a
    // thread that is queued on a lock held by an already-suspended thread; on the
    // device the kernel runs the handler right there, the thread acknowledges and
    // parks, and the lock is resumed after the restart. A plain blocking lock() turns
    // that ordinary sequence into a permanent deadlock: the collector waits for an
    // acknowledgement from a thread that is waiting for a lock nobody will release.
    RegisterThunk("pthread_mutex_lock", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        GuestMutexLockInterruptible(GuestMutexFor(jit.Regs()[0]), host);
        jit.Regs()[0] = 0;
    });
    RegisterThunk("pthread_mutex_trylock", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        bool got = GuestMutexFor(jit.Regs()[0]).try_lock();
        jit.Regs()[0] = got ? 0 : 16;  // EBUSY
    });
    RegisterThunk("pthread_mutex_unlock", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        GuestMutexFor(jit.Regs()[0]).unlock();
        jit.Regs()[0] = 0;
    });
    RegisterThunk("pthread_cond_init", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        GuestCondFor(jit.Regs()[0]);
        jit.Regs()[0] = 0;
    });
    RegisterThunk("pthread_cond_destroy", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        GuestSyncForget(jit.Regs()[0], 4);
        jit.Regs()[0] = 0;
    });
    // ponytail: every guest mutex is recursive, so condition_variable_any releases it
    // once on wait. Correct at lock depth 1, which is all Unity and Mono use here; a
    // depth-2 wait would deadlock and would need a depth-tracking wrapper.
    RegisterThunk("pthread_cond_wait", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        GuestCondWaitUntil(GuestCondFor(jit.Regs()[0]), GuestMutexFor(jit.Regs()[1]), host,
                           std::chrono::steady_clock::time_point::max());
        jit.Regs()[0] = 0;
    });
    RegisterThunk("pthread_cond_timedwait", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        bool signalled = GuestCondWaitUntil(
            GuestCondFor(jit.Regs()[0]), GuestMutexFor(jit.Regs()[1]), host,
            std::chrono::steady_clock::now() + UntilGuestDeadline(host, jit.Regs()[2]));
        jit.Regs()[0] = signalled ? 0 : 110;  // ETIMEDOUT
    });
    RegisterThunk("pthread_cond_signal", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        GuestCondNotify(GuestCondFor(jit.Regs()[0]), false);
        jit.Regs()[0] = 0;
    });
    RegisterThunk("pthread_cond_broadcast", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        GuestCondNotify(GuestCondFor(jit.Regs()[0]), true);
        jit.Regs()[0] = 0;
    });

    // ponytail: the claim is atomic, but a loser returns immediately instead of
    // waiting for the winner's initialiser to finish. Add a per-control condition
    // variable if anything is seen using the initialised state right after the call.
    RegisterThunk("pthread_once", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t control_addr = jit.Regs()[0];
        uint32_t init_routine = jit.Regs()[1];
        uint32_t control_val = 1;
        if (control_addr) {
            std::lock_guard<std::mutex> lock(g_sync_registry);
            control_val = host.MemoryRead32(control_addr);
            host.MemoryWrite32(control_addr, 1);
        }
        if (control_val == 0 && init_routine) {
            jit.Regs()[15] = init_routine & ~1u;
            if (init_routine & 1) {
                jit.SetCpsr(jit.Cpsr() | 0x20);
            } else {
                jit.SetCpsr(jit.Cpsr() & ~0x20);
            }
        }
        jit.Regs()[0] = 0;
    });

    RegisterThunk("pthread_key_create", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t key_ptr = jit.Regs()[0];
        if (key_ptr) host.MemoryWrite32(key_ptr, g_next_tls_key.fetch_add(1));
        jit.Regs()[0] = 0;
    });
    RegisterThunk("pthread_key_delete", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        jit.Regs()[0] = 0;
    });

    // setjmp/longjmp have to save and restore *guest* state, so they get their own
    // layout inside the guest's jmp_buf (which bionic sizes at 64 words on ARM32,
    // far more than the eleven used here). Mono uses them for its exception
    // unwinder, so a stubbed setjmp means every managed throw walks off a cliff.
    //
    // Setting r15 from a thunk works because dynarmic ends the block at the SVC and
    // re-reads PC from the register file afterwards - the trampoline's trailing
    // BX LR is simply never reached.
    auto guest_setjmp = [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t buf = jit.Regs()[0];
        if (buf) {
            for (int i = 0; i < 8; ++i) host.MemoryWrite32(buf + i * 4, jit.Regs()[4 + i]);
            host.MemoryWrite32(buf + 32, jit.Regs()[13]);
            host.MemoryWrite32(buf + 36, jit.Regs()[14]);
            host.MemoryWrite32(buf + 40, jit.Cpsr());
        }
        jit.Regs()[0] = 0;
    };
    auto guest_longjmp = [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t buf = jit.Regs()[0];
        uint32_t value = jit.Regs()[1];
        if (!buf) return;
        for (int i = 0; i < 8; ++i) jit.Regs()[4 + i] = host.MemoryRead32(buf + i * 4);
        jit.Regs()[13] = host.MemoryRead32(buf + 32);
        uint32_t lr = host.MemoryRead32(buf + 36);
        jit.SetCpsr(host.MemoryRead32(buf + 40));
        jit.Regs()[14] = lr;
        jit.Regs()[0] = value ? value : 1;  // longjmp(buf, 0) still returns 1
        jit.Regs()[15] = lr & ~1u;
    };
    for (const char* name : {"setjmp", "_setjmp", "sigsetjmp"}) RegisterThunk(name, guest_setjmp);
    for (const char* name : {"longjmp", "_longjmp", "siglongjmp"}) RegisterThunk(name, guest_longjmp);

    // --- what Mono's start-up needs ----------------------------------------
    //
    // MONO_DEBUG=explicit-null-checks is set on the host side before the guest
    // boots; if getenv is a stub the guest never sees it, keeps its hardware null
    // checks, and the first managed null dereference needs a 32-bit signal frame
    // that does not exist. The result is cached so repeated calls keep returning
    // the same pointer, which is what callers assume.
    RegisterThunk("getenv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        static std::unordered_map<std::string, uint32_t> cache;
        const char* name = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        if (!name) { jit.Regs()[0] = 0; return; }
        std::lock_guard<std::mutex> lock(g_sync_registry);
        auto it = cache.find(name);
        if (it != cache.end()) { jit.Regs()[0] = it->second; return; }
        const char* value = getenv(name);
        uint32_t guest = 0;
        if (value) {
            size_t n = strlen(value) + 1;
            guest = host.GuestAlloc(n, 1);
            if (guest) memcpy(host.GetGuestMemoryBase() + guest, value, n);
        }
        LOGI("getenv('%s') -> %s", name, value ? value : "(unset)");
        cache[name] = guest;
        jit.Regs()[0] = guest;
    });
    RegisterThunk("setenv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) { jit.Regs()[0] = 0; });

    RegisterThunk("sysconf", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        switch (jit.Regs()[0]) {
            case 39:                                                     // _SC_PAGESIZE
            case 40: jit.Regs()[0] = 4096; break;                        // _SC_PAGE_SIZE
            case 96:                                                     // _SC_NPROCESSORS_CONF
            case 97: jit.Regs()[0] = static_cast<uint32_t>(sysconf(_SC_NPROCESSORS_ONLN)); break;
            // Unity builds SystemInfo.systemMemorySize out of these two, and the
            // game reads it back as `IsLowQuality` - with zero here it decided the
            // device had no memory at all and ran every level in low quality.
            // Counted in 4 KiB units, not the host's: a host with 16 KiB pages
            // (Pixel) would otherwise report a quarter of the memory to a guest
            // that has been told its pages are 4 KiB.
            case 98:                                                     // _SC_PHYS_PAGES
            case 99: {                                                   // _SC_AVPHYS_PAGES
                long pages = sysconf(static_cast<int>(jit.Regs()[0]));
                long page_size = sysconf(_SC_PAGESIZE);
                if (pages <= 0 || page_size <= 0) { jit.Regs()[0] = 0; break; }
                uint64_t in_4k = (static_cast<uint64_t>(pages) * static_cast<uint64_t>(page_size)) / 4096;
                jit.Regs()[0] = static_cast<uint32_t>(in_4k > 0xFFFFFFFFull ? 0xFFFFFFFFull : in_4k);
                break;
            }
            default:
                // A silent zero here is a claim about the system, not an absence of
                // one: that is how "no memory" reached the game as a quality setting.
                LOGE("sysconf(%u) is not answered - returning 0", jit.Regs()[0]);
                jit.Regs()[0] = 0;
                break;
        }
    });

    // Mono maps its own JIT code buffers. They have to be guest-addressable, so the
    // mapping comes out of the guest heap rather than from the host mmap.
    // ponytail: no unmapping and no shared-file write-back - the heap is a bump
    // allocator. Revisit if a long session runs the guest arena dry.
    // mmap takes six arguments: r0..r3 hold addr/length/prot/flags and the last two
    // are on the stack. Reading fd out of r4 - a callee-saved register, not an
    // argument - is what produced multi-gigabyte allocation requests.
    auto guest_mmap = [](Dynarmic::A32::Jit& jit, DynarmicHost& host, bool wide_offset) {
        uint32_t requested = jit.Regs()[0];
        uint32_t length = jit.Regs()[1];
        uint32_t prot = jit.Regs()[2];
        uint32_t flags = jit.Regs()[3];
        uint32_t sp = jit.Regs()[13];
        auto fd = static_cast<int32_t>(host.MemoryRead32(sp));
        // mmap64's off64_t is 8-byte aligned, so it starts one slot further along.
        uint64_t offset = wide_offset ? ((sp + 8) & ~7u) == sp + 8
                                            ? host.MemoryRead64(sp + 8)
                                            : host.MemoryRead64((sp + 8 + 7) & ~7u)
                                      : host.MemoryRead32(sp + 4);

        // MAP_FIXED means "this address or nothing", and the caller checks. Boehm
        // hands free heap blocks back to the system by mapping PROT_NONE over them
        // with MAP_FIXED, then takes them back with PROT_READ|WRITE the same way,
        // and each time it compares the result against the address it asked for:
        //   if (result != start_addr) ABORT("mmap(...PROT_NONE...) failed")
        // Ignoring addr and returning a fresh block therefore failed that check,
        // and it failed it *inside the allocator lock*, in the middle of a
        // collection. The arena is one flat mapping, so there is nothing to unmap:
        // the honest answer is the same address back, zero-filled - which is also
        // what Boehm expects to find there, since pages that come back from an
        // unmap read as zero.  ZeroGuestMemory obtains that state by discarding
        // full host pages instead of physically writing through the whole range.
        constexpr uint32_t kMapFixed = 0x10;
        uint32_t addr;
        if ((flags & kMapFixed) && requested &&
            static_cast<uint64_t>(requested) + length <= host.GetGuestMemorySize()) {
            addr = requested;
        } else {
            addr = host.GuestAlloc(length ? length : 4096, 4096);
        }
        if (!addr) {
            LOGE("mmap(%u bytes, fd=%d) exhausted the guest heap", length, fd);
            jit.Regs()[0] = 0xFFFFFFFF;  // MAP_FAILED
            return;
        }
        if (length >= (64u << 20)) {
            LOGI("large guest mmap: requested=0x%08X length=%u prot=0x%X flags=0x%X fd=%d",
                 requested, length, prot, flags, fd);
        }
        host.ZeroGuestMemory(addr, length);
        if (fd >= 0) {
            // File-backed: read it in once. Anything expecting MAP_SHARED writes to
            // reach the file will not get them.
            ssize_t got = pread(fd, host.GetGuestMemoryBase() + addr, length,
                                static_cast<off_t>(offset));
            if (got < 0) LOGE("mmap: pread(fd=%d, %u bytes) failed", fd, length);
        }
        jit.Regs()[0] = addr;
    };
    RegisterThunk("mmap", [guest_mmap](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        guest_mmap(jit, host, false);
    });
    RegisterThunk("mmap64", [guest_mmap](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        guest_mmap(jit, host, true);
    });
    auto ok_zero = [](Dynarmic::A32::Jit& jit, DynarmicHost&) { jit.Regs()[0] = 0; };
    // Mono's code manager and the collector map and unmap pages continuously.
    // While this was a no-op the arena drained at roughly 600 MiB a minute, all
    // of it 4 and 8 KiB mappings. A partial unmap - an address that is not the
    // start of a mapping - has no header to find and is ignored, which leaks
    // exactly as much as the old behaviour did and no more.
    RegisterThunk("munmap", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t addr = jit.Regs()[0];
        uint32_t length = jit.Regs()[1];
        host.ZeroGuestMemory(addr, length);
        host.GuestFree(addr);
        jit.Regs()[0] = 0;
    });
    RegisterThunk("madvise", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        constexpr uint32_t kMadvDontNeed = 4;
        if (jit.Regs()[2] == kMadvDontNeed)
            host.ZeroGuestMemory(jit.Regs()[0], jit.Regs()[1]);
        jit.Regs()[0] = 0;
    });
    // The guest arena is one flat RW mapping, so protection changes are advisory.
    RegisterThunk("mprotect", ok_zero);
    RegisterThunk("__pthread_cleanup_push", ok_zero);
    RegisterThunk("__pthread_cleanup_pop", ok_zero);

    // Pointer-returning string searches have to return a *guest* address, so the
    // host result is turned back into an offset from where the search started.
    auto guest_strchr = [](Dynarmic::A32::Jit& jit, DynarmicHost& host, bool last) {
        uint32_t base = jit.Regs()[0];
        const char* s = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), base));
        if (!s) { jit.Regs()[0] = 0; return; }
        int c = static_cast<int>(jit.Regs()[1]);
        const char* found = last ? strrchr(s, c) : strchr(s, c);
        jit.Regs()[0] = found ? base + static_cast<uint32_t>(found - s) : 0;
    };
    RegisterThunk("strchr", [guest_strchr](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        guest_strchr(jit, host, false);
    });
    RegisterThunk("strrchr", [guest_strchr](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        guest_strchr(jit, host, true);
    });
    RegisterThunk("memchr", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t base = jit.Regs()[0];
        void* p = GuestToHost(host.GetGuestMemoryBase(), base);
        if (!p) { jit.Regs()[0] = 0; return; }
        void* found = memchr(p, static_cast<int>(jit.Regs()[1]), jit.Regs()[2]);
        jit.Regs()[0] = found ? base + static_cast<uint32_t>(static_cast<char*>(found) - static_cast<char*>(p)) : 0;
    });

    // Mono resolves its assembly root through /proc/self/exe and the cwd; without
    // these it gives up and calls exit(1) before a single frame is drawn.
    RegisterThunk("readlink", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint8_t* base = host.GetGuestMemoryBase();
        const char* path = reinterpret_cast<const char*>(GuestToHost(base, jit.Regs()[0]));
        uint32_t buf = jit.Regs()[1];
        size_t size = jit.Regs()[2];
        if (!path || !buf || !size) { jit.Regs()[0] = 0xFFFFFFFF; return; }
        ssize_t n = readlink(path, reinterpret_cast<char*>(base + buf), size);
        jit.Regs()[0] = static_cast<uint32_t>(n);
    });
    RegisterThunk("getcwd", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t buf = jit.Regs()[0];
        size_t size = jit.Regs()[1];
        if (!buf || !size) { jit.Regs()[0] = 0; return; }
        char* out = reinterpret_cast<char*>(host.GetGuestMemoryBase() + buf);
        bool ok = getcwd(out, size) != nullptr;
        jit.Regs()[0] = ok ? buf : 0;
    });
    RegisterThunk("gettid", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        jit.Regs()[0] = static_cast<uint32_t>(gettid());
    });

    auto guest_clock = [](int (*fn)(clockid_t, struct timespec*)) {
        return [fn](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
            struct timespec ts {};
            int r = fn(static_cast<clockid_t>(jit.Regs()[0]), &ts);
            if (jit.Regs()[1]) {
                // 32-bit time_t: the guest's timespec is two words.
                host.MemoryWrite32(jit.Regs()[1], static_cast<uint32_t>(ts.tv_sec));
                host.MemoryWrite32(jit.Regs()[1] + 4, static_cast<uint32_t>(ts.tv_nsec));
            }
            jit.Regs()[0] = static_cast<uint32_t>(r);
        };
    };
    RegisterThunk("clock_gettime", guest_clock(&clock_gettime));
    RegisterThunk("clock_getres", guest_clock(&clock_getres));

    // Signal handling is not bridged: a guest handler would have to run on a
    // synthesised 32-bit frame. Mono only installs these defensively once
    // MONO_DEBUG=explicit-null-checks is in effect, so reporting success is safe.
    auto signal_ok = [](Dynarmic::A32::Jit& jit, DynarmicHost&) { jit.Regs()[0] = 0; };
    for (const char* name : {"sigprocmask", "pthread_sigmask", "sigemptyset",
                             "sigfillset", "sigaddset", "sigdelset", "sigaltstack",
                             "bsd_signal", "sigismember"}) {
        RegisterThunk(name, signal_ok);
    }
    // The handler has to be remembered, not just accepted: Boehm's stop-the-world
    // is a signal round trip, and without the handler address there is nothing to
    // run on the target thread. In ARM32 bionic the handler is the first word of
    // struct sigaction.
    RegisterThunk("sigaction", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        int signum = static_cast<int>(jit.Regs()[0]);
        uint32_t act = jit.Regs()[1];
        if (act) GuestSignalSetHandler(signum, host.MemoryRead32(act));
        jit.Regs()[0] = 0;
    });
    RegisterThunk("signal", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        GuestSignalSetHandler(static_cast<int>(jit.Regs()[0]), jit.Regs()[1]);
        jit.Regs()[0] = 0;
    });

    // Boehm scans each thread's stack conservatively, so it has to be told where the
    // guest stack actually is - not the host one this thread happens to run on.
    RegisterThunk("pthread_getattr_np", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t attr = jit.Regs()[1];
        if (attr) {
            // bionic pthread_attr_t: flags, stack_base, stack_size, guard_size, ...
            uint32_t top = host.GuestStackTop();
            host.MemoryWrite32(attr, 0);
            host.MemoryWrite32(attr + 4, top - DynarmicHost::GUEST_STACK_SIZE);
            host.MemoryWrite32(attr + 8, DynarmicHost::GUEST_STACK_SIZE);
            host.MemoryWrite32(attr + 12, 4096);
        }
        jit.Regs()[0] = 0;
    });
    RegisterThunk("pthread_attr_getstack", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t attr = jit.Regs()[0];
        if (jit.Regs()[1]) host.MemoryWrite32(jit.Regs()[1], host.MemoryRead32(attr + 4));
        if (jit.Regs()[2]) host.MemoryWrite32(jit.Regs()[2], host.MemoryRead32(attr + 8));
        jit.Regs()[0] = 0;
    });
    RegisterThunk("pthread_attr_getstacksize", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        if (jit.Regs()[1]) host.MemoryWrite32(jit.Regs()[1], host.MemoryRead32(jit.Regs()[0] + 8));
        jit.Regs()[0] = 0;
    });
    RegisterThunk("pthread_attr_getguardsize", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        if (jit.Regs()[1]) host.MemoryWrite32(jit.Regs()[1], 4096);
        jit.Regs()[0] = 0;
    });

    // Mono reports why it is giving up through vprintf before calling exit(1), so
    // this is the only place that message is visible.
    auto guest_vprintf = [](Dynarmic::A32::Jit& jit, DynarmicHost& host, int fmt_reg) {
        const char* fmt = reinterpret_cast<const char*>(
            GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[fmt_reg]));
        std::string text = FormatGuestVaList(jit, host, fmt, jit.Regs()[fmt_reg + 1]);
        LOGI("[guest] %s", text.c_str());
        jit.Regs()[0] = static_cast<uint32_t>(text.size());
    };
    RegisterThunk("vprintf", [guest_vprintf](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        guest_vprintf(jit, host, 0);
    });
    RegisterThunk("vfprintf", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t guest_file = jit.Regs()[0];
        const char* fmt = reinterpret_cast<const char*>(
            GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]));
        std::string text = FormatGuestVaList(jit, host, fmt, jit.Regs()[2]);
        LOGI("[guest] %s", text.c_str());
        jit.Regs()[0] = static_cast<uint32_t>(warband::guest_stdio::WriteBytes(
            g_file_map, g_file_mutex, guest_file, text.data(), text.size()));
    });

    RegisterThunk("nanosleep", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t req = jit.Regs()[0];
        if (req) {
            int64_t sec = static_cast<int32_t>(host.MemoryRead32(req));
            int64_t nsec = static_cast<int32_t>(host.MemoryRead32(req + 4));
            int64_t usec = sec * 1000000 + nsec / 1000;
            if (usec <= 0) {
                std::this_thread::yield();
            } else {
                GuestSleepInterruptible(host, usec);
            }
            if (GuestSignalPending()) GuestSignalDeliverPending(host);
        }
        jit.Regs()[0] = 0;
    });
    RegisterThunk("sched_yield", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        sched_yield();
        jit.Regs()[0] = 0;
    });
    RegisterThunk("prctl", [](Dynarmic::A32::Jit& jit, DynarmicHost&) { jit.Regs()[0] = 0; });
    // What the guest is allowed to believe about the CPU.
    //
    // This answered 0 for everything except the page size, which is what
    // AT_HWCAP means "this processor has no VFP, no NEON and no hardware
    // divide". Android's cpu-features library asks here first and falls back to
    // /proc/self/auxv, which a 64-bit process cannot read for a 32-bit view - so
    // Unity, guest audio and Mono all concluded they were on a bare ARMv5 and took
    // their scalar fallback paths. guest audio's mixer then moves audio four floats at
    // a time through memcpy, which is where 1.4 million bridge round trips a
    // second were coming from.
    //
    // Only bits Dynarmic actually implements are claimed. ThumbEE, iWMMXt and
    // Crunch are left out: nothing emits them, and claiming an instruction set
    // the translator does not have turns a slow path into a dead one.
    if (!GuestCpuInfoSelfCheck()) m_registration_error = true;
    constexpr uint32_t kArmHwcap =
        (1u << 0)  |  // SWP
        (1u << 1)  |  // HALF
        (1u << 2)  |  // THUMB
        (1u << 4)  |  // FAST_MULT
        (1u << 6)  |  // VFP
        (1u << 7)  |  // EDSP
        (1u << 12) |  // NEON
        (1u << 13) |  // VFPv3
        (1u << 15) |  // TLS
        (1u << 16) |  // VFPv4
        (1u << 17) |  // IDIVA
        (1u << 18) |  // IDIVT
        (1u << 19);   // VFPD32 - 32 double registers, which Dynarmic has
    RegisterThunk("getauxval", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        switch (jit.Regs()[0]) {
            case 6:  jit.Regs()[0] = 4096; break;        // AT_PAGESZ
            case 16: jit.Regs()[0] = kArmHwcap; break;   // AT_HWCAP
            case 26: jit.Regs()[0] = 0; break;           // AT_HWCAP2: no crypto extensions
            default: jit.Regs()[0] = 0; break;
        }
    });
    RegisterThunk("pthread_mutexattr_destroy", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        jit.Regs()[0] = 0;
    });

    // exit() must not return: the compiler assumed it would not, so the bytes after
    // the call are a jump table, and falling through executes them as code.
    auto guest_exit = [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        LOGE("guest called exit/abort (status %u) at PC 0x%08X", jit.Regs()[0], jit.Regs()[15]);
        GuestFatalBacktrace(jit, host);
        GuestJumpToExitTrap(jit);  // the exit trap RunFunction plants
    };
    RegisterThunk("exit", guest_exit);
    RegisterThunk("_exit", guest_exit);
    RegisterThunk("abort", guest_exit);
    RegisterThunk("__stack_chk_fail", guest_exit);

    RegisterThunk("strstr", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint8_t* base = host.GetGuestMemoryBase();
        const char* hay = reinterpret_cast<const char*>(GuestToHost(base, jit.Regs()[0]));
        const char* needle = reinterpret_cast<const char*>(GuestToHost(base, jit.Regs()[1]));
        if (!hay || !needle) { jit.Regs()[0] = 0; return; }
        const char* found = strstr(hay, needle);
        jit.Regs()[0] = found ? jit.Regs()[0] + static_cast<uint32_t>(found - hay) : 0;
    });
    auto guest_getc = [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        jit.Regs()[0] = static_cast<uint32_t>(GuestFileGetc(jit.Regs()[0]));
    };
    RegisterThunk("fgetc", guest_getc);
    RegisterThunk("getc", guest_getc);
    RegisterThunk("ungetc", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        jit.Regs()[0] = static_cast<uint32_t>(
            GuestFileUngetc(static_cast<int32_t>(jit.Regs()[0]), jit.Regs()[1]));
    });
    if (!GuestStdioSelfCheck(m_host)) m_registration_error = true;

    // Guest FILE facades carry an ARM32 fd field; host stdio still resolves the
    // real FILE through the registry rather than interpreting that guest layout.
    RegisterThunk("fgets", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t buf = jit.Regs()[0];
        int size = static_cast<int>(jit.Regs()[1]);
        std::lock_guard<std::mutex> lock(g_file_mutex);
        auto it = g_file_map.find(jit.Regs()[2]);
        if (!buf || size <= 0 || it == g_file_map.end()) { jit.Regs()[0] = 0; return; }
        char* out = reinterpret_cast<char*>(host.GetGuestMemoryBase() + buf);
        jit.Regs()[0] = fgets(out, size, it->second) ? buf : 0;
    });

    // Bionic's wide-character helpers, used by the C++ locale machinery in the
    // static initialisers. Only the ASCII range ever comes through here.
    RegisterThunk("btowc", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        uint32_t c = jit.Regs()[0];
        jit.Regs()[0] = (c <= 0x7F) ? c : 0xFFFFFFFF;  // WEOF
    });
    RegisterThunk("wctob", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        uint32_t c = jit.Regs()[0];
        jit.Regs()[0] = (c <= 0x7F) ? c : 0xFFFFFFFF;  // EOF
    });
    RegisterThunk("wctype", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* name = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        jit.Regs()[0] = name ? static_cast<uint32_t>(wctype(name)) : 0;
    });

    // Bionic's monotonic-clock condition waits; libmain's start-up handshake uses
    // the first one, so a no-op version turns that wait into a spin.
    RegisterThunk("pthread_cond_timedwait_monotonic_np", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        int64_t sec = static_cast<int32_t>(host.MemoryRead32(jit.Regs()[2]));
        int64_t nsec = static_cast<int32_t>(host.MemoryRead32(jit.Regs()[2] + 4));
        struct timespec now {};
        clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t delta = (sec - now.tv_sec) * 1000000000LL + (nsec - now.tv_nsec);
        bool signalled = GuestCondWaitUntil(
            GuestCondFor(jit.Regs()[0]), GuestMutexFor(jit.Regs()[1]), host,
            std::chrono::steady_clock::now() + std::chrono::nanoseconds(delta < 0 ? 0 : delta));
        jit.Regs()[0] = signalled ? 0 : 110;  // ETIMEDOUT
    });
    RegisterThunk("pthread_cond_timedwait_relative_np", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        int64_t sec = static_cast<int32_t>(host.MemoryRead32(jit.Regs()[2]));
        int64_t nsec = static_cast<int32_t>(host.MemoryRead32(jit.Regs()[2] + 4));
        bool signalled = GuestCondWaitUntil(
            GuestCondFor(jit.Regs()[0]), GuestMutexFor(jit.Regs()[1]), host,
            std::chrono::steady_clock::now() + std::chrono::nanoseconds(sec * 1000000000LL + nsec));
        jit.Regs()[0] = signalled ? 0 : 110;
    });

    RegisterThunk("pthread_setspecific", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        t_tls_map[jit.Regs()[0]] = jit.Regs()[1];
        jit.Regs()[0] = 0;
    });

    RegisterThunk("pthread_getspecific", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        auto it = t_tls_map.find(jit.Regs()[0]);
        jit.Regs()[0] = (it != t_tls_map.end()) ? it->second : 0;
    });

    RegisterThunk("fopen", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* path = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        const char* mode = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]));
        if (!path) { jit.Regs()[0] = 0; return; }
        const uint32_t handle = host.GuestAlloc(kGuestFileSize, 8);
        if (!handle) { errno = ENOMEM; jit.Regs()[0] = 0; return; }

        const bool read_only = !mode || (mode[0] == 'r' && !strchr(mode, '+'));
        FILE* f = nullptr;
        if (strcmp(path, "/proc/cpuinfo") == 0) {
            if (read_only) {
                const int fd = OpenGuestCpuInfo();
                if (fd >= 0) {
                    f = fdopen(fd, "rb");
                    if (!f) close(fd);
                }
            } else {
                errno = EACCES;
            }
        } else {
            f = fopen(path, mode ? mode : "r");
        }
        if (!f && read_only) {
            int apk_fd = GameDataOpenEntry(path, nullptr);
            if (apk_fd < 0) apk_fd = ApkOpenEntry(path, nullptr);
            if (apk_fd >= 0) {
                f = fdopen(apk_fd, "rb");
                if (!f) close(apk_fd);
            }
        }

        if (f && BindGuestFile(host, handle, f)) {
            jit.Regs()[0] = handle;
        } else {
            const int error = errno;
            if (f) fclose(f);
            host.GuestFree(handle);
            errno = error;
            jit.Regs()[0] = 0;
        }
    });

    RegisterThunk("fdopen", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        int fd = static_cast<int>(jit.Regs()[0]);
        const char* mode = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]));
        if (fd < 0 || fd > INT16_MAX || !mode) {
            errno = fd > INT16_MAX ? EOVERFLOW : EINVAL;
            jit.Regs()[0] = 0;
            return;
        }
        // Reserve the guest facade before fdopen takes ownership of the descriptor.
        const uint32_t handle = host.GuestAlloc(kGuestFileSize, 8);
        if (!handle) { errno = ENOMEM; jit.Regs()[0] = 0; return; }
        FILE* f = fdopen(fd, mode);
        if (f && BindGuestFile(host, handle, f)) {
            jit.Regs()[0] = handle;
        } else {
            const int error = errno;
            if (f) fclose(f);
            host.GuestFree(handle);
            errno = error;
            jit.Regs()[0] = 0;
        }
    });

    RegisterThunk("fclose", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        jit.Regs()[0] = static_cast<uint32_t>(CloseGuestFile(host, jit.Regs()[0]));
    });

    RegisterThunk("fread", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        void* ptr = GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]);
        size_t size = jit.Regs()[1];
        size_t count = jit.Regs()[2];
        uint32_t handle = jit.Regs()[3];
        std::lock_guard<std::mutex> lock(g_file_mutex);
        auto it = g_file_map.find(handle);
        size_t res = (it != g_file_map.end() && ptr) ? fread(ptr, size, count, it->second) : 0;
        jit.Regs()[0] = res;
    });

    RegisterThunk("fwrite", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const void* ptr = GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]);
        size_t size = jit.Regs()[1];
        size_t count = jit.Regs()[2];
        uint32_t handle = jit.Regs()[3];
        std::lock_guard<std::mutex> lock(g_file_mutex);
        auto it = g_file_map.find(handle);
        if (it != g_file_map.end() && ptr && (it->second == stdout || it->second == stderr)) {
            LOGE("[guest] %.*s", static_cast<int>(size * count), static_cast<const char*>(ptr));
            jit.Regs()[0] = count;
            return;
        }
        jit.Regs()[0] = (it != g_file_map.end() && ptr) ? fwrite(ptr, size, count, it->second) : 0;
    });

    RegisterThunk("fseek", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t handle = jit.Regs()[0];
        long offset = static_cast<long>(static_cast<int32_t>(jit.Regs()[1]));
        int whence = static_cast<int>(jit.Regs()[2]);
        std::lock_guard<std::mutex> lock(g_file_mutex);
        auto it = g_file_map.find(handle);
        int res = (it != g_file_map.end()) ? fseek(it->second, offset, whence) : -1;
        jit.Regs()[0] = res;
    });

    RegisterThunk("fseeko", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t handle = jit.Regs()[0];
        off_t offset = static_cast<off_t>(static_cast<int32_t>(jit.Regs()[1]));
        int whence = static_cast<int>(jit.Regs()[2]);
        std::lock_guard<std::mutex> lock(g_file_mutex);
        auto it = g_file_map.find(handle);
        int res = (it != g_file_map.end()) ? fseeko(it->second, offset, whence) : -1;
        jit.Regs()[0] = res;
    });

    RegisterThunk("ftell", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t handle = jit.Regs()[0];
        std::lock_guard<std::mutex> lock(g_file_mutex);
        auto it = g_file_map.find(handle);
        jit.Regs()[0] = (it != g_file_map.end()) ? static_cast<uint32_t>(ftell(it->second)) : 0;
    });

    RegisterThunk("ftello", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t handle = jit.Regs()[0];
        std::lock_guard<std::mutex> lock(g_file_mutex);
        auto it = g_file_map.find(handle);
        off_t res = (it != g_file_map.end()) ? ftello(it->second) : -1;
        jit.Regs()[0] = static_cast<uint32_t>(res);
    });

    RegisterThunk("fileno", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t handle = jit.Regs()[0];
        std::lock_guard<std::mutex> lock(g_file_mutex);
        auto it = g_file_map.find(handle);
        jit.Regs()[0] = (it != g_file_map.end()) ? fileno(it->second) : -1;
    });

    RegisterThunk("clearerr", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t handle = jit.Regs()[0];
        std::lock_guard<std::mutex> lock(g_file_mutex);
        auto it = g_file_map.find(handle);
        if (it != g_file_map.end()) clearerr(it->second);
    });

    RegisterThunk("ferror", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t handle = jit.Regs()[0];
        std::lock_guard<std::mutex> lock(g_file_mutex);
        auto it = g_file_map.find(handle);
        jit.Regs()[0] = (it != g_file_map.end()) ? ferror(it->second) : 0;
    });

    RegisterThunk("open", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* path = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        int flags = static_cast<int>(jit.Regs()[1]);
        int mode = static_cast<int>(jit.Regs()[2]);
        if (path && strcmp(path, "/proc/cpuinfo") == 0) {
            if (flags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND)) {
                errno = EACCES;
                jit.Regs()[0] = static_cast<uint32_t>(-1);
            } else {
                jit.Regs()[0] = static_cast<uint32_t>(OpenGuestCpuInfo());
            }
            return;
        }

        // Match fopen's mod-first contract; packaged data is only a read fallback.
        int fd = open(path ? path : "", flags, mode);
        if (fd < 0 && path && (flags & (O_CREAT | O_TRUNC | O_APPEND | O_WRONLY | O_RDWR)) == 0) {
            fd = GameDataOpenEntry(path, nullptr);
            if (fd < 0) fd = ApkOpenEntry(path, nullptr);
        }
        if (fd < 0) LOGI("open('%s') failed (errno %d)", path ? path : "(null)", errno);
        jit.Regs()[0] = (fd >= 0) ? static_cast<uint32_t>(fd) : static_cast<uint32_t>(-1);
    });

    RegisterThunk("close", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        int fd = static_cast<int>(jit.Regs()[0]);
        {
            std::lock_guard<std::mutex> lock(g_vfd_mutex);
            auto it = g_vfd_map.find(fd);
            if (it != g_vfd_map.end()) {
                close(it->second.real_apk_fd);
                g_vfd_map.erase(it);
                jit.Regs()[0] = 0;
                return;
            }
        }
        jit.Regs()[0] = (fd >= 0) ? static_cast<uint32_t>(close(fd)) : static_cast<uint32_t>(-1);
    });

    RegisterThunk("read", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        int fd = static_cast<int>(jit.Regs()[0]);
        void* buf = GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]);
        size_t count = jit.Regs()[2];
        if (!buf) { jit.Regs()[0] = static_cast<uint32_t>(-1); return; }

        {
            std::lock_guard<std::mutex> lock(g_vfd_mutex);
            auto it = g_vfd_map.find(fd);
            if (it != g_vfd_map.end()) {
                VirtualFd& vfd = it->second;
                if (vfd.current_pos >= static_cast<off_t>(vfd.file_size)) {
                    jit.Regs()[0] = 0; // EOF
                    return;
                }
                size_t avail = vfd.file_size - vfd.current_pos;
                size_t to_read = (count < avail) ? count : avail;
                ssize_t nread = pread(vfd.real_apk_fd, buf, to_read, vfd.file_offset + vfd.current_pos);
                if (nread > 0) {
                    vfd.current_pos += nread;
                }
                jit.Regs()[0] = (nread >= 0) ? static_cast<uint32_t>(nread) : static_cast<uint32_t>(-1);
                return;
            }
        }

        ssize_t res = (fd >= 0) ? read(fd, buf, count) : -1;
        jit.Regs()[0] = static_cast<uint32_t>(res);
    });

    RegisterThunk("write", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        int fd = static_cast<int>(jit.Regs()[0]);
        const void* buf = GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]);
        size_t count = jit.Regs()[2];
        // An app's fd 1 and 2 are /dev/null, and that is where Boehm's GC_err_printf
        // and eglib's assertion text were going: the reason for a guest abort() was
        // written out and thrown away every time.
        if (buf && (fd == 1 || fd == 2)) {
            LOGE("[guest] %.*s", static_cast<int>(count), static_cast<const char*>(buf));
            jit.Regs()[0] = static_cast<uint32_t>(count);
            return;
        }
        ssize_t res = (buf && fd >= 0) ? write(fd, buf, count) : -1;
        jit.Regs()[0] = static_cast<uint32_t>(res);
    });

    RegisterThunk("lseek", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        int fd = static_cast<int>(jit.Regs()[0]);
        off_t offset = static_cast<off_t>(static_cast<int32_t>(jit.Regs()[1]));
        int whence = static_cast<int>(jit.Regs()[2]);

        {
            std::lock_guard<std::mutex> lock(g_vfd_mutex);
            auto it = g_vfd_map.find(fd);
            if (it != g_vfd_map.end()) {
                VirtualFd& vfd = it->second;
                off_t new_pos = vfd.current_pos;
                if (whence == SEEK_SET) new_pos = offset;
                else if (whence == SEEK_CUR) new_pos += offset;
                else if (whence == SEEK_END) new_pos = static_cast<off_t>(vfd.file_size) + offset;
                else { jit.Regs()[0] = static_cast<uint32_t>(-1); return; }

                if (new_pos < 0) { jit.Regs()[0] = static_cast<uint32_t>(-1); return; }
                vfd.current_pos = new_pos;
                jit.Regs()[0] = static_cast<uint32_t>(new_pos);
                return;
            }
        }

        off_t res = (fd >= 0) ? lseek(fd, offset, whence) : -1;
        jit.Regs()[0] = static_cast<uint32_t>(res);
    });

    RegisterThunk("lseek64", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        int fd = static_cast<int>(jit.Regs()[0]);
        int64_t offset = static_cast<int64_t>(jit.Regs()[2]) | (static_cast<int64_t>(jit.Regs()[3]) << 32);
        int whence = static_cast<int>(host.MemoryRead32(jit.Regs()[13]));

        {
            std::lock_guard<std::mutex> lock(g_vfd_mutex);
            auto it = g_vfd_map.find(fd);
            if (it != g_vfd_map.end()) {
                VirtualFd& vfd = it->second;
                off_t new_pos = vfd.current_pos;
                if (whence == SEEK_SET) new_pos = offset;
                else if (whence == SEEK_CUR) new_pos += offset;
                else if (whence == SEEK_END) new_pos = static_cast<off_t>(vfd.file_size) + offset;
                else { jit.Regs()[0] = static_cast<uint32_t>(-1); jit.Regs()[1] = static_cast<uint32_t>(-1); return; }

                if (new_pos < 0) { jit.Regs()[0] = static_cast<uint32_t>(-1); jit.Regs()[1] = static_cast<uint32_t>(-1); return; }
                vfd.current_pos = new_pos;
                jit.Regs()[0] = static_cast<uint32_t>(new_pos & 0xFFFFFFFF);
                jit.Regs()[1] = static_cast<uint32_t>(new_pos >> 32);
                return;
            }
        }

        off64_t res = (fd >= 0) ? lseek64(fd, offset, whence) : -1;
        jit.Regs()[0] = static_cast<uint32_t>(res & 0xFFFFFFFF);
        jit.Regs()[1] = static_cast<uint32_t>(res >> 32);
    });

    RegisterThunk("access", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* path = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        int mode = static_cast<int>(jit.Regs()[1]);
        int res = access(path ? path : "", mode);
        if (res != 0 && (GameDataStatEntry(path, nullptr, nullptr) ||
                         ApkStatEntry(path, nullptr, nullptr))) {
            res = 0;
        }
        jit.Regs()[0] = res;
    });

    RegisterThunk("stat", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* path = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        uint32_t buf_gaddr = jit.Regs()[1];
        struct stat st;
        int res = stat(path ? path : "", &st);
        if (res != 0) {
            off_t size = 0;
            bool is_dir = false;
            if (GameDataStatEntry(path, &size, &is_dir) ||
                ApkStatEntry(path, &size, &is_dir)) {
                memset(&st, 0, sizeof(st));
                st.st_size = size;
                st.st_mode = (is_dir ? (S_IFDIR | 0755) : (S_IFREG | 0644));
                st.st_nlink = 1;
                res = 0;
            }
        }
        if (res == 0) {
            WriteArm32Stat(host.GetGuestMemoryBase(), buf_gaddr, st);
        }
        jit.Regs()[0] = res;
    });

    RegisterThunk("feof", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t handle = jit.Regs()[0];
        std::lock_guard<std::mutex> lock(g_file_mutex);
        auto it = g_file_map.find(handle);
        jit.Regs()[0] = (it != g_file_map.end()) ? feof(it->second) : 1;
    });

    RegisterThunk("fflush", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t handle = jit.Regs()[0];
        std::lock_guard<std::mutex> lock(g_file_mutex);
        auto it = g_file_map.find(handle);
        jit.Regs()[0] = (it != g_file_map.end()) ? fflush(it->second) : 0;
    });

    RegisterThunk("setvbuf", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        jit.Regs()[0] = 0;
    });

    RegisterThunk("fstat", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        int fd = static_cast<int>(jit.Regs()[0]);
        uint32_t buf_gaddr = jit.Regs()[1];
        struct stat st;
        int res = -1;

        {
            std::lock_guard<std::mutex> lock(g_vfd_mutex);
            auto it = g_vfd_map.find(fd);
            if (it != g_vfd_map.end()) {
                memset(&st, 0, sizeof(st));
                st.st_size = it->second.file_size;
                st.st_mode = S_IFREG | 0644;
                st.st_nlink = 1;
                res = 0;
            }
        }

        if (res != 0) {
            res = fstat(fd, &st);
        }

        if (res == 0) {
            WriteArm32Stat(host.GetGuestMemoryBase(), buf_gaddr, st);
        }
        jit.Regs()[0] = res;
    });

    RegisterThunk("remove", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* path = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        jit.Regs()[0] = remove(path ? path : "");
    });

    RegisterThunk("rename", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* oldp = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        const char* newp = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]));
        jit.Regs()[0] = rename(oldp ? oldp : "", newp ? newp : "");
    });

    RegisterThunk("chmod", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) { jit.Regs()[0] = 0; });
    RegisterThunk("umask", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) { jit.Regs()[0] = 0; });
    RegisterThunk("mkstemp", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) { jit.Regs()[0] = -1; });

    RegisterThunk("time", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        jit.Regs()[0] = static_cast<uint32_t>(time(nullptr));
    });

    // Like strerror, the result is storage the caller never frees. Real localtime
    // overwrites one static buffer per call; this does the same, per thread, so two
    // threads formatting a timestamp cannot scribble on each other.
    RegisterThunk("localtime", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        static thread_local uint32_t gtm = 0;
        time_t t = static_cast<time_t>(host.MemoryRead32(jit.Regs()[0]));
        struct tm* tm_res = localtime(&t);
        if (!gtm) gtm = host.GuestAlloc(sizeof(struct tm));
        if (tm_res && gtm) {
            memcpy(GuestToHost(host.GetGuestMemoryBase(), gtm), tm_res, sizeof(struct tm));
            jit.Regs()[0] = gtm;
        } else {
            jit.Regs()[0] = 0;
        }
    });

    RegisterThunk("mktime", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        struct tm* tm_ptr = reinterpret_cast<struct tm*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        jit.Regs()[0] = tm_ptr ? static_cast<uint32_t>(mktime(tm_ptr)) : 0;
    });

    RegisterThunk("raise", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) { jit.Regs()[0] = 0; });

    // Runtime guards & hooks.
    // abort() and __stack_chk_fail() are registered above with guest_exit. A second
    // registration here used to shadow them: RegisterThunk hands out a fresh
    // trampoline and overwrites the symbol, so the last one wins, and this one only
    // logged and returned. That is exactly the fall-through exit() was fixed for -
    // and when the aborting thread was inside the Boehm allocator it walked out of
    // the critical section without releasing GC_allocate_ml, leaving every other
    // thread spinning in GC_lock forever.

    RegisterThunk("__assert2", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* file = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        int line = static_cast<int>(jit.Regs()[1]);
        const char* func = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[2]));
        const char* expr = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[3]));
        LOGE("Assertion failed: %s at %s:%d (%s)", expr ? expr : "", file ? file : "", line, func ? func : "");
        GuestFatalBacktrace(jit, host);
        GuestJumpToExitTrap(jit);  // bionic's __assert2 does not return either
    });

    RegisterThunk("__cxa_atexit", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) { jit.Regs()[0] = 0; });
    RegisterThunk("__cxa_finalize", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) { jit.Regs()[0] = 0; });
    RegisterThunk("__aeabi_unwind_cpp_pr0", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {});
    RegisterThunk("__aeabi_unwind_cpp_pr1", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {});
    RegisterThunk("_Unwind_Resume", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {});
    RegisterThunk("_Unwind_RaiseException", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) { jit.Regs()[0] = 0; });
    RegisterThunk("_Unwind_DeleteException", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {});
    RegisterThunk("_Unwind_GetLanguageSpecificData", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) { jit.Regs()[0] = 0; });
    RegisterThunk("_Unwind_GetRegionStart", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) { jit.Regs()[0] = 0; });
    RegisterThunk("_Unwind_VRS_Get", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) { jit.Regs()[0] = 0; });
    RegisterThunk("_Unwind_VRS_Set", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) { jit.Regs()[0] = 0; });
    RegisterThunk("__gnu_unwind_frame", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) { jit.Regs()[0] = 0; });
}

// Undefined *data* imports.
//
// The symbol resolver's fallback mints a code trampoline for anything it cannot
// find, which is right for functions and silently catastrophic for variables: the
// guest then reads the trampoline's "svc #id" instruction as the variable's value.
// libmono imports __page_size that way, so Mono's allocator saw a page size of
// 0xEF0002F2 and asked mmap for four gigabytes.
//
// These are registered before relocation so the resolver finds real storage.
void ThunkManager::RegisterDataSymbols() {
    uint8_t* mem = m_host.GetGuestMemoryBase();

    auto define = [&](const char* name, size_t size, size_t align = 4) -> uint32_t {
        uint32_t addr = m_host.GuestAlloc(size, align);
        if (addr) memset(mem + addr, 0, size);
        RegisterSymbol(name, addr);
        return addr;
    };

    m_host.MemoryWrite32(define("__page_size", 4), 4096);
    // Boehm takes the addresses of these to bound the static data it scans. Real
    // values are not knowable from here; a small empty range is wrong but bounded,
    // where a trampoline address would send the collector into the code region.
    define("__data_start", 4);
    define("data_start", 4);
    m_host.MemoryWrite32(define("__stack_chk_guard", 4), 0xDEADC0DE);
    define("_ZSt7nothrow", 1, 1);  // std::nothrow, an empty tag object

    // environ: a pointer to a NULL-terminated vector. The guest gets an empty one;
    // everything it actually asks for goes through the getenv thunk.
    uint32_t env_vector = m_host.GuestAlloc(4, 4);
    m_host.MemoryWrite32(env_vector, 0);
    m_host.MemoryWrite32(define("environ", 4), env_vector);
    RegisterSymbol("__progname", define("__progname_storage", 4));

    // __sF is bionic's array of three FILE structs; stdin/stdout/stderr are macros
    // for &__sF[0..2]. sizeof(FILE) is 84 on 32-bit, and the guest has that constant
    // compiled in, so the stride has to match exactly.
    uint32_t sF = define("__sF", kGuestFileSize * 3, 8);
    if (!RegisterGuestStdioStreams(m_host, sF)) m_registration_error = true;

    // Legacy BSD ctype: the symbols are pointers to tables indexed by c+1.
    auto define_table = [&](const char* name, const void* data, size_t size) {
        uint32_t table = m_host.GuestAlloc(size, 4);
        memcpy(mem + table, data, size);
        m_host.MemoryWrite32(define(name, 4), table);
    };

    unsigned char ctype[257] = {0};
    short lower_tab[257];
    short upper_tab[257];
    lower_tab[0] = upper_tab[0] = -1;  // the EOF slot
    for (int c = 0; c < 256; ++c) {
        unsigned char flags = 0;
        if (isupper(c)) flags |= 0x01;   // _U
        if (islower(c)) flags |= 0x02;   // _L
        if (isdigit(c)) flags |= 0x04;   // _N
        if (isspace(c)) flags |= 0x08;   // _S
        if (ispunct(c)) flags |= 0x10;   // _P
        if (iscntrl(c)) flags |= 0x20;   // _C
        if (isxdigit(c)) flags |= 0x40;  // _X
        if (c == ' ') flags |= 0x80;     // _B
        ctype[c + 1] = flags;
        lower_tab[c + 1] = static_cast<short>(tolower(c));
        upper_tab[c + 1] = static_cast<short>(toupper(c));
    }
    define_table("_ctype_", ctype, sizeof(ctype));
    define_table("_tolower_tab_", lower_tab, sizeof(lower_tab));
    define_table("_toupper_tab_", upper_tab, sizeof(upper_tab));

    LOGI("guest data symbols defined (page size 4096)");
}

// memcpy is the busiest thunk in the bridge by an order of magnitude: about 1.4
// million calls a second in game, and 99% of them copy between 16 and 31 bytes.
// A copy that small is not worth leaving the guest for - the round trip out of the
// translated block, through the SVC callback and the dispatcher and back, costs
// more than moving the bytes does.
//
// So the memcpy symbol is pointed at ARM32 code placed in guest memory, which
// Dynarmic translates and runs like any other guest code. Anything over 64 bytes
// branches to the original trampoline, where a 64-bit host memcpy earns the trip.
// The branch is `ldr pc, [pc, #-4]`: the trampoline sits at 0x10000 and this code
// lives in the guest heap, far outside the +-32 MB reach of B.
//
// Registers follow AAPCS: r0 dst, r1 src, r2 len, returns dst, r3 and r12 free.
void ThunkManager::InstallFastMemcpy() {
    uint32_t big = GetOrCreateThunkAddress("memcpy");
    if (!big) return;

    static const uint32_t kBody[] = {
        // Nearly every call is exactly 16 bytes - guest audio moves its mix four floats
        // at a time - so that case gets a straight line with no loop and no saved
        // copy of dst, since r0 is never touched.
        0xE3520010,  //  0  cmp   r2, #16
        0x1A000008,  //  1  bne   general          -> 11
        0xE5913000,  //  2  ldr   r3, [r1]
        0xE591C004,  //  3  ldr   r12, [r1, #4]
        0xE5803000,  //  4  str   r3, [r0]
        0xE580C004,  //  5  str   r12, [r0, #4]
        0xE5913008,  //  6  ldr   r3, [r1, #8]
        0xE591C00C,  //  7  ldr   r12, [r1, #12]
        0xE5803008,  //  8  str   r3, [r0, #8]
        0xE580C00C,  //  9  str   r12, [r0, #12]
        0xE12FFF1E,  // 10  bx    lr
        // general: word loop then a byte tail, any alignment (ARMv7 allows
        // unaligned LDR/STR, and Dynarmic translates them as such).
        0xE3520040,  // 11  cmp   r2, #64
        0x8A00000D,  // 12  bhi   big              -> 27
        0xE1A0C000,  // 13  mov   r12, r0          keep dst for the return value
        0xE2522004,  // 14  subs  r2, r2, #4
        0xBA000002,  // 15  blt   tail             -> 19
        0xE4913004,  // 16  ldr   r3, [r1], #4
        0xE4803004,  // 17  str   r3, [r0], #4
        0xEAFFFFFA,  // 18  b     word             -> 14
        0xE2922004,  // 19  adds  r2, r2, #4       undo the last subtract
        0xE2522001,  // 20  subs  r2, r2, #1
        0xBA000002,  // 21  blt   done             -> 25
        0xE4D13001,  // 22  ldrb  r3, [r1], #1
        0xE4C03001,  // 23  strb  r3, [r0], #1
        0xEAFFFFFA,  // 24  b     bytes            -> 20
        0xE1A0000C,  // 25  mov   r0, r12
        0xE12FFF1E,  // 26  bx    lr
        0xE51FF004,  // 27  ldr   pc, [pc, #-4]    the trampoline, lr untouched
        0x00000000,  // 28  <trampoline address>
    };
    constexpr size_t kWords = sizeof(kBody) / sizeof(kBody[0]);

    uint32_t code = m_host.GuestAlloc(kWords * 4, 4);
    if (!code) return;
    for (size_t i = 0; i < kWords; ++i) m_host.MemoryWrite32(code + i * 4, kBody[i]);
    m_host.MemoryWrite32(code + (kWords - 1) * 4, big);
    m_host.InvalidateGuestCode(code, kWords * 4);

    // __aeabi_memcpy has the same arguments and only differs in returning void.
    for (const char* name : {"memcpy", "__aeabi_memcpy", "__aeabi_memcpy4", "__aeabi_memcpy8"}) {
        if (IsRegistered(name)) m_symbol_to_address[name] = code;
    }
    LOGI("memcpy runs in the guest at 0x%08X, over 64 bytes goes to 0x%08X", code, big);
}
