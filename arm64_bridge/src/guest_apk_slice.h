#pragma once

#include <android/asset_manager.h>
#include <android/log.h>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

struct GuestApkRange {
    off_t offset = 0;
    size_t bias = 0;
    size_t size = 0;
};

inline bool GuestApkMapRange(int64_t apk_size, int64_t offset, int64_t length,
                             long page_size, GuestApkRange& range) {
    constexpr int64_t kMaxGuestFileSize = 64 * 1024 * 1024;
    if (apk_size < 0 || offset < 0 || length <= 0 || length > kMaxGuestFileSize ||
        offset > apk_size || length > apk_size - offset || page_size <= 0) return false;
    const uint64_t page = static_cast<uint64_t>(page_size);
    if (page & (page - 1)) return false;
    const uint64_t aligned = static_cast<uint64_t>(offset) & ~(page - 1);
    const uint64_t bias = static_cast<uint64_t>(offset) - aligned;
    if (aligned > static_cast<uint64_t>(std::numeric_limits<off_t>::max()) ||
        bias > SIZE_MAX || static_cast<uint64_t>(length) > SIZE_MAX - bias) return false;
    range = {static_cast<off_t>(aligned), static_cast<size_t>(bias),
             static_cast<size_t>(bias + static_cast<uint64_t>(length))};
    return true;
}

// Only guest ELF input uses this real APK slice. Ordinary assets keep their VFS.
class GuestApkSlice {
public:
    GuestApkSlice() = default;
    GuestApkSlice(const GuestApkSlice&) = delete;
    GuestApkSlice& operator=(const GuestApkSlice&) = delete;
    ~GuestApkSlice() { Close(); }

    bool Open(AAssetManager* manager, const char* asset) {
        if (m_fd >= 0 || m_mapping != MAP_FAILED) return false;
        m_asset = asset ? asset : "(null)";
        m_apk_size = m_offset = m_length = 0;
        m_range = {};
        if (!manager || !asset) return Fail("asset manager/path", EINVAL);
        AAsset* entry = AAssetManager_open(manager, asset, AASSET_MODE_UNKNOWN);
        if (!entry) return Fail("missing asset", ENOENT);
        off64_t offset = 0, length = 0;
        const int raw_fd = AAsset_openFileDescriptor64(entry, &offset, &length);
        if (raw_fd < 0) {
            AAsset_close(entry);
            return Fail("direct fd unavailable (compressed asset rejected)", EINVAL);
        }
        m_offset = offset;
        m_length = length;
        m_fd = dup(raw_fd);
        const int dup_error = errno;
        const int raw_close = close(raw_fd);
        const int close_error = errno;
        AAsset_close(entry);
        if (m_fd < 0) return Fail("dup", dup_error);
        if (raw_close != 0) return Fail("source fd close", close_error);

        struct stat status{};
        if (fstat(m_fd, &status) != 0) return Fail("fstat", errno);
        if (!S_ISREG(status.st_mode)) return Fail("APK is not a regular file", EINVAL);
        m_apk_size = status.st_size;
        const long page = sysconf(_SC_PAGESIZE);
        if (!GuestApkMapRange(m_apk_size, offset, length, page, m_range))
            return Fail("APK slice/page bounds", EINVAL);
        m_mapping = mmap(nullptr, m_range.size, PROT_READ, MAP_PRIVATE, m_fd, m_range.offset);
        if (m_mapping == MAP_FAILED) return Fail("mmap", errno);
        m_data = static_cast<const uint8_t*>(m_mapping) + m_range.bias;
        __android_log_print(ANDROID_LOG_INFO, "GuestApkSlice",
            "guest mmap: asset=%s dupfd=%d apk_size=%lld offset=%lld size=%lld page=%ld "
            "map_offset=%lld map_base=%p map_size=%zu bias=%zu view=%p",
            m_asset.c_str(), m_fd, static_cast<long long>(m_apk_size),
            static_cast<long long>(m_offset), static_cast<long long>(m_length), page,
            static_cast<long long>(m_range.offset), m_mapping, m_range.size, m_range.bias, m_data);
        return true;
    }

    const uint8_t* Data() const { return m_data; }
    size_t Size() const { return m_data ? static_cast<size_t>(m_length) : 0; }

    bool Close(uint32_t guest_destination = 0) {
        bool ok = true;
        if (m_mapping != MAP_FAILED) {
            const int result = munmap(m_mapping, m_range.size);
            const int error = result ? errno : 0;
            __android_log_print(result ? ANDROID_LOG_ERROR : ANDROID_LOG_INFO, "GuestApkSlice",
                "guest munmap: asset=%s dupfd=%d apk_size=%lld offset=%lld size=%lld "
                "map_base=%p map_size=%zu guest=0x%08x result=%d errno=%d",
                m_asset.c_str(), m_fd, static_cast<long long>(m_apk_size),
                static_cast<long long>(m_offset), static_cast<long long>(m_length),
                m_mapping, m_range.size, guest_destination, result, error);
            ok = result == 0;
            m_mapping = MAP_FAILED;
        }
        m_data = nullptr;
        if (m_fd >= 0) {
            const int result = close(m_fd);
            const int error = result ? errno : 0;
            __android_log_print(result ? ANDROID_LOG_ERROR : ANDROID_LOG_INFO, "GuestApkSlice",
                "guest fd close: asset=%s dupfd=%d result=%d errno=%d",
                m_asset.c_str(), m_fd, result, error);
            ok = ok && result == 0;
            m_fd = -1;
        }
        return ok;
    }

private:
    bool Fail(const char* stage, int error) {
        __android_log_print(ANDROID_LOG_ERROR, "GuestApkSlice",
            "guest slice rejected: asset=%s stage=%s errno=%d dupfd=%d "
            "apk_size=%lld offset=%lld size=%lld",
            m_asset.c_str(), stage, error, m_fd, static_cast<long long>(m_apk_size),
            static_cast<long long>(m_offset), static_cast<long long>(m_length));
        Close();
        return false;
    }

    std::string m_asset;
    int m_fd = -1;
    int64_t m_apk_size = 0, m_offset = 0, m_length = 0;
    GuestApkRange m_range;
    void* m_mapping = MAP_FAILED;
    const uint8_t* m_data = nullptr;
};
