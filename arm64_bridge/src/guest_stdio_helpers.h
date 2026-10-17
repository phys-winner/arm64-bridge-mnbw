#pragma once

#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>

namespace warband::guest_stdio {

// Guest FILE pointers are 32-bit facade addresses. Resolve them through the
// bridge-owned map before forwarding a character write to host stdio.
template <typename FileMap, typename Mutex>
int PutCharacter(FileMap& files, Mutex& file_mutex, uint32_t guest_file, int character) {
    std::lock_guard<Mutex> lock(file_mutex);
    const auto it = files.find(guest_file);
    if (it == files.end() || !it->second) {
        errno = EBADF;
        return EOF;
    }
    return std::fputc(character, it->second);
}

// Formatted guest output is materialized in host memory before it reaches this
// helper. Preserve fprintf's byte-count contract while resolving the guest's
// 32-bit FILE facade through the bridge registry.
template <typename FileMap, typename Mutex>
int WriteBytes(FileMap& files, Mutex& file_mutex, uint32_t guest_file,
               const char* bytes, std::size_t size) {
    std::lock_guard<Mutex> lock(file_mutex);
    const auto it = files.find(guest_file);
    if (it == files.end() || !it->second) {
        errno = EBADF;
        return EOF;
    }
    if (size > static_cast<std::size_t>(INT_MAX)) {
        errno = EOVERFLOW;
        return EOF;
    }
    if (size == 0) return 0;
    if (!bytes) {
        errno = EINVAL;
        return EOF;
    }
    const std::size_t written = std::fwrite(bytes, 1, size, it->second);
    return written == size ? static_cast<int>(written) : EOF;
}

template <typename FileMap, typename Mutex>
int PutString(FileMap& files, Mutex& file_mutex, uint32_t guest_file, const char* text) {
    std::lock_guard<Mutex> lock(file_mutex);
    const auto it = files.find(guest_file);
    if (it == files.end() || !it->second) {
        errno = EBADF;
        return EOF;
    }
    if (!text) {
        errno = EINVAL;
        return EOF;
    }
    return std::fputs(text, it->second);
}

}  // namespace warband::guest_stdio
