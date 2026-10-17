#include "mod_texture_conversion.h"

#include "etc1_encoder.h"
#include "mod_astc_encoder.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
#include <new>
#include <sys/stat.h>
#include <fcntl.h>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <io.h>
#include <process.h>
#else
#include <unistd.h>
#endif

namespace mod_texture {
namespace {

constexpr size_t kDdsHeaderBytes = 128;
constexpr size_t kHeaderBytes = 80;
constexpr size_t kMipBytes = 32;
constexpr uint32_t kMaxMips = 16;
constexpr uint8_t kMagic[8] = {'M', 'B', 'T', 'X', 'C', '0', '0', '1'};
constexpr uint32_t kVersion = 1;
std::mutex g_conversion_mutex;
std::atomic<uint32_t> g_temp_sequence{0};
#ifdef WARBAND_MOD_TEXTURE_TESTS
int64_t g_test_write_budget = -1;
#endif

struct FileInfo {
    uint64_t size = 0;
    uint64_t modified_seconds = 0;
    uint32_t modified_nanoseconds = 0;
};

class File {
public:
    int fd = -1;
    explicit File(int descriptor = -1) : fd(descriptor) {}
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    ~File() { Close(); }
    void Close() {
        if (fd < 0) return;
#ifdef _WIN32
        _close(fd);
#else
        close(fd);
#endif
        fd = -1;
    }
};

int OpenRead(const char* path) {
    if (!path) return -1;
#ifdef _WIN32
    return _wopen(std::filesystem::u8path(path).c_str(), _O_RDONLY | _O_BINARY);
#else
    return open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
#endif
}

bool GetInfo(int fd, FileInfo& info) {
#ifdef _WIN32
    struct _stat64 status{};
    if (_fstat64(fd, &status) != 0 || (status.st_mode & _S_IFREG) == 0 ||
        status.st_size < 0) return false;
    info.modified_nanoseconds = 0;
#else
    struct stat status{};
    if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0)
        return false;
    info.modified_nanoseconds = static_cast<uint32_t>(status.st_mtim.tv_nsec);
#endif
    info.size = static_cast<uint64_t>(status.st_size);
    info.modified_seconds = static_cast<uint64_t>(status.st_mtime);
    return true;
}

bool SameInfo(const FileInfo& a, const FileInfo& b) {
    return a.size == b.size && a.modified_seconds == b.modified_seconds &&
        a.modified_nanoseconds == b.modified_nanoseconds;
}

bool ReadExact(int fd, void* data, size_t size, uint64_t offset) {
    if (offset > static_cast<uint64_t>(INT64_MAX) ||
        size > static_cast<uint64_t>(INT64_MAX) - offset) return false;
    auto* bytes = static_cast<uint8_t*>(data);
    size_t done = 0;
    while (done < size) {
        const size_t chunk = std::min(size - done, size_t{1u << 20u});
#ifdef _WIN32
        if (_lseeki64(fd, static_cast<__int64>(offset + done), SEEK_SET) < 0)
            return false;
        const int count = _read(fd, bytes + done, static_cast<unsigned>(chunk));
#else
        const ssize_t count = pread(fd, bytes + done, chunk,
                                    static_cast<off_t>(offset + done));
#endif
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        done += static_cast<size_t>(count);
    }
    return true;
}

bool WriteExact(int fd, const void* data, size_t size, uint64_t offset) {
#ifdef WARBAND_MOD_TEXTURE_TESTS
    if (g_test_write_budget >= 0) {
        if (static_cast<uint64_t>(g_test_write_budget) < size) { errno = ENOSPC; return false; }
        g_test_write_budget -= static_cast<int64_t>(size);
    }
#endif
    const auto* bytes = static_cast<const uint8_t*>(data);
    size_t done = 0;
    while (done < size) {
        const size_t chunk = std::min(size - done, size_t{1u << 20u});
#ifdef _WIN32
        if (_lseeki64(fd, static_cast<__int64>(offset + done), SEEK_SET) < 0)
            return false;
        const int count = _write(fd, bytes + done, static_cast<unsigned>(chunk));
#else
        const ssize_t count = pwrite(fd, bytes + done, chunk,
                                     static_cast<off_t>(offset + done));
#endif
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        done += static_cast<size_t>(count);
    }
    return true;
}

bool Sync(int fd) {
#ifdef _WIN32
    return _commit(fd) == 0;
#else
    return fsync(fd) == 0;
#endif
}

void Put32(uint8_t* data, uint32_t value) {
    for (size_t i = 0; i < 4; ++i) data[i] = static_cast<uint8_t>(value >> (8u * i));
}

uint64_t Read64(const uint8_t* data) {
    return Read32(data) | (static_cast<uint64_t>(Read32(data + 4)) << 32u);
}

void Put64(uint8_t* data, uint64_t value) {
    Put32(data, static_cast<uint32_t>(value));
    Put32(data + 4, static_cast<uint32_t>(value >> 32u));
}

constexpr std::array<uint32_t, 256> MakeCrcTable() {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t value = i;
        for (int bit = 0; bit < 8; ++bit)
            value = (value >> 1u) ^ ((value & 1u) ? 0xedb88320u : 0u);
        table[i] = value;
    }
    return table;
}
constexpr auto kCrcTable = MakeCrcTable();

uint32_t UpdateCrc(uint32_t crc, const uint8_t* bytes, size_t size) {
    for (size_t i = 0; i < size; ++i)
        crc = kCrcTable[(crc ^ bytes[i]) & 255u] ^ (crc >> 8u);
    return crc;
}

bool FileCrc(int fd, uint64_t size, uint32_t& checksum) {
    std::array<uint8_t, 65536> bytes{};
    uint32_t crc = 0xffffffffu;
    for (uint64_t offset = 0; offset < size;) {
        const size_t count = static_cast<size_t>(std::min<uint64_t>(bytes.size(), size - offset));
        if (!ReadExact(fd, bytes.data(), count, offset)) return false;
        crc = UpdateCrc(crc, bytes.data(), count);
        offset += count;
    }
    checksum = ~crc;
    return true;
}

struct SourceMip {
    uint32_t width;
    uint32_t height;
    uint64_t offset;
    size_t size;
};

struct SourceTexture {
    FileInfo info;
    DxtFormat format = DxtFormat::kNone;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<SourceMip> mips;
};

int ReadSource(int fd, SourceTexture& texture, std::string& error) {
    if (fd < 0 || !GetInfo(fd, texture.info)) {
        error = "Cannot read texture file. Check storage access and try again.";
        return -1;
    }
    std::array<uint8_t, kDdsHeaderBytes> header{};
    if (texture.info.size < 4 || !ReadExact(fd, header.data(), 4, 0) ||
        std::memcmp(header.data(), "DDS ", 4) != 0) return 0;
    if (texture.info.size < header.size() ||
        !ReadExact(fd, header.data(), header.size(), 0) ||
        Read32(header.data() + 4) != 124 || Read32(header.data() + 76) != 32) {
        error = "The DDS header is damaged or incomplete. Reinstall this texture from the original mod.";
        return -1;
    }
    const uint32_t flags = Read32(header.data() + 80);
    const uint8_t* fourcc = header.data() + 84;
    if ((flags & 4u) != 0) {
        if (std::memcmp(fourcc, "DXT1", 4) == 0) texture.format = DxtFormat::kDxt1;
        else if (std::memcmp(fourcc, "DXT3", 4) == 0) texture.format = DxtFormat::kDxt3;
        else if (std::memcmp(fourcc, "DXT5", 4) == 0) texture.format = DxtFormat::kDxt5;
        else if (std::memcmp(fourcc, "DXT", 3) == 0 || std::memcmp(fourcc, "DX10", 4) == 0) {
            error = "Unsupported DDS compression. Export this texture as DXT1, DXT3, DXT5 or BGRA8 DDS.";
            return -1;
        }
    } else if ((flags & 0x40u) != 0 && Read32(header.data() + 88) == 32u &&
               Read32(header.data() + 92) == 0x00ff0000u &&
               Read32(header.data() + 96) == 0x0000ff00u &&
               Read32(header.data() + 100) == 0x000000ffu) {
        const uint32_t alpha_mask = Read32(header.data() + 104);
        if (alpha_mask == 0xff000000u) texture.format = DxtFormat::kBgra8;
        else if (alpha_mask == 0u) texture.format = DxtFormat::kBgrx8;
    }
    if (texture.format == DxtFormat::kNone) return 0;
    texture.width = Read32(header.data() + 16);
    texture.height = Read32(header.data() + 12);
    const uint32_t mip_count = std::max(1u, Read32(header.data() + 28));
    const uint32_t caps2 = Read32(header.data() + 112);
    const uint32_t depth = Read32(header.data() + 24);
    // Three supplied Pendor DXT1 files carry DDSCAPS2_VOLUME with depth one.
    // Their byte-exact mip chains are ordinary 2D data. Admit only that case;
    // real volume/cubemap layouts remain unsupported and fail closed.
    const bool one_slice_volume = caps2 == 0x00200000u && depth == 1u &&
        (Read32(header.data() + 8) & 0x00800000u) != 0u;
    if (!texture.width || !texture.height || texture.width > 16384u ||
        texture.height > 16384u || mip_count > kMaxMips ||
        (caps2 != 0u && !one_slice_volume) || depth > 1u) {
        error = "Invalid DDS dimensions, mip count, cubemap or volume texture. Re-export this texture as a 2D DDS.";
        return -1;
    }
    uint32_t width = texture.width, height = texture.height;
    uint64_t offset = kDdsHeaderBytes;
    for (uint32_t level = 0; level < mip_count; ++level) {
        const size_t size = DxtBytes(texture.format, width, height);
        if (size == 0 || offset > texture.info.size || size > texture.info.size - offset) {
            error = "The DDS mip data is incomplete. Reinstall this texture from the original mod.";
            return -1;
        }
        texture.mips.push_back({width, height, offset, size});
        offset += size;
        if (width == 1 && height == 1 && level + 1 != mip_count) {
            error = "The DDS contains more mip levels than its dimensions permit.";
            return -1;
        }
        width = std::max(1u, width / 2u);
        height = std::max(1u, height / 2u);
    }
    if (one_slice_volume && offset != texture.info.size) {
        error = "A single-slice DDS has unexpected extra data; volume textures are unsupported.";
        return -1;
    }
    return 1;
}

size_t StoredBytes(uint32_t format, uint32_t width, uint32_t height) {
    if (format == kStoredRgba8) return static_cast<size_t>(width) * height * 4u;
    const size_t block = format == kStoredAstc6x6 ? 6u : 4u;
    const size_t bytes = format == kStoredEtc1 ? 8u : 16u;
    return ((static_cast<size_t>(width) + block - 1) / block) *
        ((static_cast<size_t>(height) + block - 1) / block) * bytes;
}

bool ReadContainer(int fd, ConvertedTexture& texture,
                   bool verify_payloads, uint32_t* source_crc) {
    FileInfo stored_info;
    std::array<uint8_t, kHeaderBytes> fixed{};
    if (fd < 0 || !GetInfo(fd, stored_info) || stored_info.size < fixed.size() ||
        !ReadExact(fd, fixed.data(), fixed.size(), 0) ||
        std::memcmp(fixed.data(), kMagic, sizeof(kMagic)) != 0 ||
        Read32(fixed.data() + 8) != kVersion) return false;
    const uint32_t mips = Read32(fixed.data() + 40);
    const uint32_t target = Read32(fixed.data() + 28);
    const uint32_t flags = Read32(fixed.data() + 48);
    const uint32_t width = Read32(fixed.data() + 32);
    const uint32_t height = Read32(fixed.data() + 36);
    const auto source_format = static_cast<DxtFormat>(Read32(fixed.data() + 44));
    if (mips == 0 || mips > kMaxMips ||
        Read32(fixed.data() + 12) != kHeaderBytes + mips * kMipBytes ||
        width == 0 || height == 0 || width > 16384u || height > 16384u ||
        (source_format != DxtFormat::kDxt1 && source_format != DxtFormat::kDxt3 &&
         source_format != DxtFormat::kDxt5 && source_format != DxtFormat::kBgra8 &&
         source_format != DxtFormat::kBgrx8) || flags > 1u ||
        (target != kTargetAstc && target != kTargetEtc1) ||
        Read64(fixed.data() + 56) != stored_info.size || Read32(fixed.data() + 76) != 0u)
        return false;
    std::vector<uint8_t> header(kHeaderBytes + mips * kMipBytes);
    if (!ReadExact(fd, header.data(), header.size(), 0)) return false;
    const uint32_t header_crc = Read32(header.data() + 52);
    Put32(header.data() + 52, 0);
    if (ModTextureCrc32(header.data(), header.size()) != header_crc) return false;
    texture.target = static_cast<int>(target);
    texture.source_format = source_format;
    texture.width = width;
    texture.height = height;
    texture.has_alpha = flags != 0;
    texture.mips.clear();
    if (source_crc) *source_crc = Read32(header.data() + 24);
    uint64_t expected_offset = header.size();
    const uint32_t expected_format = target == kTargetAstc
        ? (source_format == DxtFormat::kDxt1 ? kStoredAstc6x6 : kStoredAstc4x4)
        : (texture.has_alpha ? kStoredRgba8 : kStoredEtc1);
    uint32_t mip_width = width, mip_height = height;
    uint64_t minimum_source_size = kDdsHeaderBytes;
    for (size_t level = 0; level < mips; ++level) {
        const uint8_t* record = header.data() + kHeaderBytes + level * kMipBytes;
        ConvertedMip mip{Read32(record), Read32(record + 4), Read32(record + 8),
            Read32(record + 12), Read64(record + 16), Read64(record + 24)};
        if (mip.width != mip_width || mip.height != mip_height ||
            mip.format != expected_format || mip.offset != expected_offset ||
            mip.size != StoredBytes(mip.format, mip.width, mip.height) ||
            mip.offset > stored_info.size || mip.size > stored_info.size - mip.offset)
            return false;
        if (verify_payloads) {
            std::vector<uint8_t> payload(static_cast<size_t>(mip.size));
            if (!ReadExact(fd, payload.data(), payload.size(), mip.offset) ||
                ModTextureCrc32(payload.data(), payload.size()) != mip.checksum) return false;
        }
        texture.mips.push_back(mip);
        expected_offset += mip.size;
        minimum_source_size += DxtBytes(source_format, mip_width, mip_height);
        if (mip_width == 1 && mip_height == 1 && level + 1 != mips) return false;
        mip_width = std::max(1u, mip_width / 2u);
        mip_height = std::max(1u, mip_height / 2u);
    }
    return expected_offset == stored_info.size &&
        Read64(header.data() + 16) >= minimum_source_size;
}

bool HasConvertedMagic(int fd) {
    std::array<uint8_t, 4> prefix{};
    return fd >= 0 && ReadExact(fd, prefix.data(), prefix.size(), 0) &&
        std::memcmp(prefix.data(), kMagic, prefix.size()) == 0;
}

std::string IoError(const char* action) {
    if (errno == ENOSPC) return std::string(action) + ": not enough free storage. Free some space and retry; originals are safe.";
    if (errno == EACCES || errno == EPERM || errno == EROFS)
        return std::string(action) + ": storage is not writable. Check the mod folder permissions.";
    return std::string(action) + ": " + std::strerror(errno) + ". The original DDS is unchanged.";
}

// Create a new sibling exclusively. Interrupted jobs leave harmless .tmp files
// which are never mistaken for complete results or selected for upload.
class TemporaryFile {
public:
    File file;
    std::string path;
    explicit TemporaryFile(const std::string& destination) {
        // A killed process cannot run the destructor below. Reclaim only our
        // exact per-texture numeric temp names before allocating a new sibling.
        const std::filesystem::path target(destination);
        const auto directory = target.parent_path();
        const std::string prefix = target.filename().string() + ".mbtex.tmp.";
        std::error_code scan_error;
        for (std::filesystem::directory_iterator it(directory, scan_error), end;
             !scan_error && it != end; it.increment(scan_error)) {
            const std::string name = it->path().filename().string();
            if (name.compare(0, prefix.size(), prefix) != 0) continue;
            const std::string suffix = name.substr(prefix.size());
            const size_t separator = suffix.find('.');
            if (separator == std::string::npos || separator == 0 ||
                separator + 1 == suffix.size() ||
                !std::all_of(suffix.begin(), suffix.begin() + separator,
                             [](char c) { return c >= '0' && c <= '9'; }) ||
                !std::all_of(suffix.begin() + separator + 1, suffix.end(),
                             [](char c) { return c >= '0' && c <= '9'; })) continue;
            std::error_code status_error;
            if (std::filesystem::is_regular_file(it->symlink_status(status_error)) &&
                !status_error) {
                std::error_code remove_error;
                std::filesystem::remove(it->path(), remove_error);
            }
        }
        for (int attempt = 0; attempt < 64; ++attempt) {
#ifdef _WIN32
            const int pid = _getpid();
#else
            const int pid = static_cast<int>(getpid());
#endif
            path = destination + ".mbtex.tmp." + std::to_string(pid) + "." +
                std::to_string(g_temp_sequence.fetch_add(1));
#ifdef _WIN32
            file.fd = _wopen(std::filesystem::u8path(path).c_str(),
                _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
            file.fd = open(path.c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
#endif
            if (file.fd >= 0 || errno != EEXIST) break;
        }
        if (file.fd < 0) path.clear();
    }
    ~TemporaryFile() {
        file.Close();
        if (!path.empty()) {
            std::error_code ignored;
            std::filesystem::remove(std::filesystem::path(path), ignored);
        }
    }
    bool Publish(const std::string& destination) {
        file.Close();
#ifdef _WIN32
        if (!MoveFileExW(std::filesystem::u8path(path).c_str(),
                         std::filesystem::u8path(destination).c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            errno = EIO;
            return false;
        }
#else
        if (rename(path.c_str(), destination.c_str()) != 0) return false;
        const auto parent = std::filesystem::path(destination).parent_path();
        File directory(open(parent.empty() ? "." : parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        // Some external Android storage providers do not implement directory
        // fsync. The file itself was fsynced and atomic rename already succeeded.
        if (directory.fd >= 0) fsync(directory.fd);
#endif
        path.clear();
        return true;
    }
};

bool SourceHasAlpha(int fd, const SourceTexture& source, bool& alpha) {
    alpha = source.format == DxtFormat::kDxt3 || source.format == DxtFormat::kDxt5;
    if (alpha || source.format == DxtFormat::kBgrx8) return true;
    // Examine every mip, including alpha introduced by an unusual DDS exporter.
    // DXT1's unused edge texels can conservatively select RGBA, never lose alpha.
    std::array<uint8_t, 65536> bytes{};
    for (const auto& mip : source.mips) {
        for (size_t consumed = 0; consumed < mip.size;) {
            const size_t count = std::min(bytes.size(), mip.size - consumed);
            if (!ReadExact(fd, bytes.data(), count, mip.offset + consumed)) return false;
            if (source.format == DxtFormat::kBgra8) {
                for (size_t i = 3; i < count; i += 4)
                    if (bytes[i] != 255u) { alpha = true; return true; }
            } else {
                for (size_t i = 0; i < count; i += 8) {
                    if (Read16(bytes.data() + i) <= Read16(bytes.data() + i + 2)) {
                        const uint32_t indices = Read32(bytes.data() + i + 4);
                        if ((indices & (indices >> 1u) & 0x55555555u) != 0u) {
                            alpha = true;
                            return true;
                        }
                    }
                }
            }
            consumed += count;
        }
    }
    return true;
}

std::string Convert(const char* path, int target) {
    if (target != kTargetAstc && target != kTargetEtc1) return "Invalid texture conversion target.";
    File source_file(OpenRead(path));
    if (HasConvertedMagic(source_file.fd)) {
        ConvertedTexture existing;
        if (!ReadContainer(source_file.fd, existing, true, nullptr))
            return "The converted texture is damaged. Restore this texture from the original mod archive.";
        if (existing.target == target) return {};
        return "This texture was converted for another GPU format. Restore it from the original mod archive before converting again.";
    }
    SourceTexture source;
    std::string error;
    const int status = ReadSource(source_file.fd, source, error);
    if (status < 0) return error;
    if (status == 0) return "This file is not a supported DXT/BGRA DDS texture.";
    uint32_t source_crc = 0;
    if (!FileCrc(source_file.fd, source.info.size, source_crc)) return IoError("Cannot verify the original texture");
    const std::string destination(path);
    bool alpha = false;
    if (!SourceHasAlpha(source_file.fd, source, alpha)) return IoError("Cannot read texture transparency");
    const uint32_t format = target == kTargetAstc
        ? (source.format == DxtFormat::kDxt1 ? kStoredAstc6x6 : kStoredAstc4x4)
        : (alpha ? kStoredRgba8 : kStoredEtc1);
    std::vector<uint8_t> header(kHeaderBytes + source.mips.size() * kMipBytes, 0);
    std::memcpy(header.data(), kMagic, sizeof(kMagic));
    Put32(header.data() + 8, kVersion);
    Put32(header.data() + 12, static_cast<uint32_t>(header.size()));
    Put64(header.data() + 16, source.info.size);
    Put32(header.data() + 24, source_crc);
    Put32(header.data() + 28, static_cast<uint32_t>(target));
    Put32(header.data() + 32, source.width);
    Put32(header.data() + 36, source.height);
    Put32(header.data() + 40, static_cast<uint32_t>(source.mips.size()));
    Put32(header.data() + 44, static_cast<uint32_t>(source.format));
    Put32(header.data() + 48, alpha ? 1u : 0u);
    Put64(header.data() + 64, source.info.modified_seconds);
    Put32(header.data() + 72, source.info.modified_nanoseconds);
    TemporaryFile temporary(destination);
    if (temporary.file.fd < 0) return IoError("Cannot create the converted texture");
    if (!WriteExact(temporary.file.fd, header.data(), header.size(), 0)) return IoError("Cannot write the converted texture");
    uint64_t offset = header.size();
    for (size_t level = 0; level < source.mips.size(); ++level) {
        const auto& mip = source.mips[level];
        std::vector<uint8_t> compressed(mip.size), rgba, encoded;
        bool mip_alpha = false;
        if (!ReadExact(source_file.fd, compressed.data(), compressed.size(), mip.offset)) return IoError("Cannot read the original mip");
        if (!DecodeDxt(source.format, compressed.data(), compressed.size(), mip.width, mip.height, rgba, mip_alpha))
            return "Cannot decode a DDS mip. Reinstall this texture from the original mod.";
        if (format == kStoredRgba8) encoded = std::move(rgba);
        else if (format == kStoredEtc1) {
            if (mip_alpha || !bridge_gl::EncodeEtc1(rgba.data(), static_cast<int>(mip.width), static_cast<int>(mip.height), encoded))
                return "ETC1 conversion failed. The original texture and its transparency are unchanged.";
        } else {
            const uint32_t block = format == kStoredAstc6x6 ? 6u : 4u;
            if (!EncodeModTextureAstc(rgba.data(), mip.width, mip.height, block, block, encoded))
                return "ASTC conversion failed. Close other apps to free memory and retry.";
        }
        if (encoded.size() != StoredBytes(format, mip.width, mip.height)) return "The encoder returned an invalid mip size.";
        uint8_t* record = header.data() + kHeaderBytes + level * kMipBytes;
        Put32(record, mip.width);
        Put32(record + 4, mip.height);
        Put32(record + 8, format);
        Put32(record + 12, ModTextureCrc32(encoded.data(), encoded.size()));
        Put64(record + 16, offset);
        Put64(record + 24, encoded.size());
        if (!WriteExact(temporary.file.fd, encoded.data(), encoded.size(), offset)) return IoError("Cannot write the converted mip");
        offset += encoded.size();
    }
    Put64(header.data() + 56, offset);
    Put32(header.data() + 52, ModTextureCrc32(header.data(), header.size()));
    if (!WriteExact(temporary.file.fd, header.data(), header.size(), 0) || !Sync(temporary.file.fd))
        return IoError("Cannot finish the converted texture");
    ConvertedTexture verified;
    uint32_t verified_crc = 0, current_crc = 0;
    FileInfo current_info;
    // Verify from the same temporary FD that will be published; no partially
    // encoded texture ever replaces a previous completed result.
    if (!ReadContainer(temporary.file.fd, verified, true, &verified_crc) || verified_crc != source_crc)
        return "Converted texture verification failed. The original and previous result are unchanged.";
    File current_source(OpenRead(path));
    if (!GetInfo(source_file.fd, current_info) || !SameInfo(source.info, current_info) ||
        current_source.fd < 0 || !GetInfo(current_source.fd, current_info) ||
        !SameInfo(source.info, current_info) ||
        !FileCrc(current_source.fd, source.info.size, current_crc) || current_crc != source_crc)
        return "The source texture changed during conversion. Wait for the mod copy to finish, then retry.";
    source_file.Close();
    current_source.Close();
    if (!temporary.Publish(destination)) return IoError("Cannot publish the converted texture");
    return {};
}

}  // namespace

uint32_t ModTextureCrc32(const void* data, size_t size) {
    return ~UpdateCrc(0xffffffffu, static_cast<const uint8_t*>(data), size);
}

#ifdef WARBAND_MOD_TEXTURE_TESTS
void SetModTextureTestWriteBudget(int64_t bytes) { g_test_write_budget = bytes; }
#endif

int InspectModTexture(const char* path, int target, std::string* output_error) {
    std::string error;
    if (target != kTargetAstc && target != kTargetEtc1) {
        if (output_error) *output_error = "Invalid texture conversion target.";
        return -1;
    }
    try {
        File source_file(OpenRead(path));
        if (HasConvertedMagic(source_file.fd)) {
            ConvertedTexture texture;
            if (!ReadContainer(source_file.fd, texture, true, nullptr)) {
                if (output_error) *output_error = "The converted texture is damaged. Restore this texture from the original mod archive.";
                return -1;
            }
            if (texture.target != target) {
                if (output_error) *output_error = "This texture was converted for another GPU format. Restore this texture from the original mod archive before converting again.";
                return -1;
            }
            return 2;
        }
        SourceTexture source;
        const int status = ReadSource(source_file.fd, source, error);
        if (output_error) *output_error = error;
        return status;
    } catch (const std::bad_alloc&) {
        if (output_error) *output_error = "Not enough memory to verify this texture.";
        return -1;
    } catch (const std::exception& exception) {
        if (output_error) *output_error = exception.what();
        return -1;
    }
}

std::string ConvertModTexture(const char* path, int target) {
    std::lock_guard<std::mutex> lock(g_conversion_mutex);
    try { return Convert(path, target); }
    catch (const std::bad_alloc&) { return "Not enough memory to convert this texture. Close other apps and retry."; }
    catch (const std::exception& exception) { return std::string("Texture conversion failed: ") + exception.what(); }
}

bool ReadConvertedModTexture(const char* path, ConvertedTexture& texture, bool* handled) {
    if (handled) *handled = false;
    try {
        File converted(OpenRead(path));
        if (!HasConvertedMagic(converted.fd)) return false;
        if (handled) *handled = true;
        texture.path = path;
        return ReadContainer(converted.fd, texture, false, nullptr);
    } catch (const std::exception&) { return false; }
}

bool ReadConvertedModMip(const std::string& path, const ConvertedMip& mip,
                         std::vector<uint8_t>& payload) {
    try {
        if (mip.width == 0 || mip.height == 0 || mip.width > 16384u || mip.height > 16384u ||
            mip.size != StoredBytes(mip.format, mip.width, mip.height)) return false;
        File file(OpenRead(path.c_str()));
        if (file.fd < 0) return false;
        payload.resize(static_cast<size_t>(mip.size));
        if (!ReadExact(file.fd, payload.data(), payload.size(), mip.offset) ||
            ModTextureCrc32(payload.data(), payload.size()) != mip.checksum) {
            payload.clear();
            return false;
        }
        return true;
    } catch (const std::exception&) { payload.clear(); return false; }
}

}  // namespace mod_texture
