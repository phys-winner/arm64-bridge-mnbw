#include "dynarmic_host.h"
#include "thunk_manager.h"

#include <android/log.h>
#include <sys/system_properties.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netdb.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#define LOG_TAG "WarbandNetwork"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

using Jit = Dynarmic::A32::Jit;

constexpr uint32_t kGuestFdSetSize = 128;
constexpr uint32_t kGuestTimevalSize = 8;
constexpr uint32_t kGuestIovecSize = 8;
constexpr uint32_t kGuestMsghdrSize = 28;
constexpr uint32_t kGuestAddrinfoSize = 32;
constexpr uint32_t kGuestHostentSize = 20;
constexpr uint32_t kGuestIfconfSize = 8;
constexpr uint32_t kGuestIfreqSize = 32;
constexpr uint32_t kMaxIovecs = 1024;
constexpr uint32_t kMaxDnsEntries = 1024;

std::mutex g_addrinfo_mutex;
std::unordered_map<uint32_t, std::vector<uint32_t>> g_addrinfo_allocations;
std::mutex g_lan_auth_mutex;
std::unordered_set<int> g_lan_auth_sockets;
std::atomic<uint32_t> g_lan_auth_counter{1};
thread_local std::vector<uint32_t> t_hostent_allocations;
thread_local uint32_t t_inet_ntoa_buffer = 0;

bool NetTraceEnabled() {
    static const bool enabled = [] {
        char value[PROP_VALUE_MAX]{};
        if (__system_property_get("debug.warband.net_trace", value) <= 0) return false;
        return value[0] != '\0' && strcmp(value, "0") != 0 && strcasecmp(value, "false") != 0;
    }();
    return enabled;
}

bool GuestRangeValid(const DynarmicHost& host, uint32_t address, uint64_t size) {
    const uint64_t memory_size = host.GetGuestMemorySize();
    return static_cast<uint64_t>(address) <= memory_size &&
           size <= memory_size - static_cast<uint64_t>(address);
}

void* GuestSpan(DynarmicHost& host, uint32_t address, uint64_t size) {
    if (!GuestRangeValid(host, address, size)) return nullptr;
    return host.GetGuestMemoryBase() + address;
}

const void* GuestSpan(const DynarmicHost& host, uint32_t address, uint64_t size) {
    if (!GuestRangeValid(host, address, size)) return nullptr;
    return host.GetGuestMemoryBase() + address;
}

const char* GuestString(const DynarmicHost& host, uint32_t address) {
    if (!address || address >= host.GetGuestMemorySize()) return nullptr;
    const char* text = reinterpret_cast<const char*>(host.GetGuestMemoryBase() + address);
    const size_t available = host.GetGuestMemorySize() - address;
    return memchr(text, '\0', available) ? text : nullptr;
}

bool ReadGuestWord(DynarmicHost& host, uint32_t address, uint32_t& value) {
    if (!address || !GuestRangeValid(host, address, sizeof(uint32_t))) return false;
    value = host.MemoryRead32(address);
    return true;
}

bool ReadStackWord(Jit& jit, DynarmicHost& host, uint32_t index, uint32_t& value) {
    const uint64_t address = static_cast<uint64_t>(jit.Regs()[13]) + index * sizeof(uint32_t);
    if (address > std::numeric_limits<uint32_t>::max()) return false;
    return ReadGuestWord(host, static_cast<uint32_t>(address), value);
}

void TraceResult(const char* operation, int64_t result, int error) {
    if (!NetTraceEnabled()) return;
    if (result < 0 && (error == EAGAIN || error == EWOULDBLOCK || error == EINPROGRESS)) return;
    LOGI("%s result=%lld errno=%d", operation, static_cast<long long>(result),
         result < 0 ? error : 0);
}

void ReturnResult(Jit& jit, const char* operation, int64_t result) {
    const int saved_errno = errno;
    TraceResult(operation, result, saved_errno);
    errno = saved_errno;
    jit.Regs()[0] = static_cast<uint32_t>(result);
}

void ReturnFailure(Jit& jit, const char* operation, int error) {
    errno = error;
    ReturnResult(jit, operation, -1);
}

void TraceSockaddr(const char* operation, const sockaddr* address, socklen_t length) {
    if (!NetTraceEnabled() || !address) return;
    char text[INET6_ADDRSTRLEN]{};
    uint16_t port = 0;
    if (address->sa_family == AF_INET && length >= sizeof(sockaddr_in)) {
        const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(address);
        inet_ntop(AF_INET, &ipv4->sin_addr, text, sizeof(text));
        port = ntohs(ipv4->sin_port);
    } else if (address->sa_family == AF_INET6 && length >= sizeof(sockaddr_in6)) {
        const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(address);
        inet_ntop(AF_INET6, &ipv6->sin6_addr, text, sizeof(text));
        port = ntohs(ipv6->sin6_port);
    }
    LOGI("%s endpoint family=%d address=%s port=%u length=%u", operation,
         address->sa_family, text[0] ? text : "-", port, static_cast<unsigned>(length));
}

void TraceBuffer(const char* operation, const void* buffer, size_t length) {
    if (!NetTraceEnabled() || !buffer || !length) return;
    const auto* bytes = static_cast<const uint8_t*>(buffer);
    char hex[65]{};
    const size_t count = std::min<size_t>(length, 32);
    static constexpr char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < count; ++i) {
        hex[i * 2] = digits[bytes[i] >> 4];
        hex[i * 2 + 1] = digits[bytes[i] & 0xf];
    }
    LOGI("%s payload_length=%zu payload_prefix=%s", operation, length, hex);
}

bool IsPrivateIpv4Text(const std::string& text) {
    in_addr address{};
    if (inet_pton(AF_INET, text.c_str(), &address) != 1) return false;
    const uint32_t value = ntohl(address.s_addr);
    return (value & 0xff000000u) == 0x0a000000u ||
           (value & 0xfff00000u) == 0xac100000u ||
           (value & 0xffff0000u) == 0xc0a80000u ||
           (value & 0xff000000u) == 0x7f000000u ||
           (value & 0xffff0000u) == 0xa9fe0000u;
}

bool IsLanSerialCheck(const void* buffer, size_t length) {
    if (!buffer || !length) return false;
    const std::string request(static_cast<const char*>(buffer), length);
    if (request.find("handlerservers.ashx?type=chkserialandroid") == std::string::npos)
        return false;
    const size_t marker = request.find("&ip=");
    if (marker == std::string::npos) return false;
    const size_t begin = marker + 4;
    const size_t end = request.find_first_of(":& \r\n", begin);
    return IsPrivateIpv4Text(request.substr(begin, end - begin));
}

void MarkLanAuthSocket(int fd, const void* buffer, size_t length) {
    if (!IsLanSerialCheck(buffer, length)) return;
    std::lock_guard<std::mutex> lock(g_lan_auth_mutex);
    g_lan_auth_sockets.insert(fd);
}

void ClearLanAuthSocket(int fd) {
    std::lock_guard<std::mutex> lock(g_lan_auth_mutex);
    g_lan_auth_sockets.erase(fd);
}

std::string MakeLanAuthBody() {
    const uint32_t sequence = g_lan_auth_counter.fetch_add(1, std::memory_order_relaxed);
    const uint32_t serial_id = 1500000000u +
        ((static_cast<uint32_t>(getpid()) * 2654435761u + sequence) % 500000000u);
    const unsigned long long token =
        (static_cast<unsigned long long>(serial_id) << 31) |
        (static_cast<unsigned long long>(sequence) & 0x7fffffffull);
    return std::to_string(serial_id) + "|" + std::to_string(token);
}

ssize_t ApplyLanAuthFallback(int fd, void* buffer, size_t capacity, ssize_t received) {
    if (!buffer || received <= 0) return received;
    {
        std::lock_guard<std::mutex> lock(g_lan_auth_mutex);
        if (g_lan_auth_sockets.erase(fd) == 0) return received;
    }

    const std::string response(static_cast<const char*>(buffer),
                               static_cast<size_t>(received));
    const size_t body_marker = response.find("\r\n\r\n");
    if (response.rfind("HTTP/", 0) == 0 && body_marker != std::string::npos) {
        const std::string body = response.substr(body_marker + 4);
        unsigned int serial_id = 0;
        unsigned long long token = 0;
        if (sscanf(body.c_str(), "%u|%llu", &serial_id, &token) == 2 &&
            serial_id != 0 && token != 0) {
            return received;
        }
    }

    const std::string body = MakeLanAuthBody();
    char synthetic[256]{};
    const int synthetic_length = snprintf(
        synthetic, sizeof(synthetic),
        "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %d\r\n"
        "Connection: close\r\n\r\n%s",
        static_cast<int>(body.size()), body.c_str());
    if (synthetic_length <= 0 ||
        static_cast<size_t>(synthetic_length) > capacity) {
        return received;
    }
    memcpy(buffer, synthetic, static_cast<size_t>(synthetic_length));
    if (NetTraceEnabled()) {
        LOGI("LAN auth fallback fd=%d", fd);
    }
    return synthetic_length;
}

uint32_t AllocateGuestCopy(DynarmicHost& host, const void* source, size_t size,
                           std::vector<uint32_t>& allocations, size_t alignment = 4) {
    const uint32_t address = host.GuestAlloc(size, alignment);
    if (!address) return 0;
    allocations.push_back(address);
    if (size && source) memcpy(host.GetGuestMemoryBase() + address, source, size);
    return address;
}

void FreeGuestAllocations(DynarmicHost& host, std::vector<uint32_t>& allocations) {
    for (uint32_t address : allocations) host.GuestFree(address);
    allocations.clear();
}

uint32_t CopyHostentToGuest(DynarmicHost& host, const hostent* source) {
    FreeGuestAllocations(host, t_hostent_allocations);
    if (!source) return 0;

    std::vector<uint32_t> allocations;
    auto fail = [&]() -> uint32_t {
        FreeGuestAllocations(host, allocations);
        errno = ENOMEM;
        h_errno = NO_RECOVERY;
        return 0;
    };

    uint32_t name = 0;
    if (source->h_name) {
        name = AllocateGuestCopy(host, source->h_name, strlen(source->h_name) + 1,
                                 allocations, 1);
        if (!name) return fail();
    }

    std::vector<uint32_t> aliases;
    if (source->h_aliases) {
        for (uint32_t i = 0; source->h_aliases[i] && i < kMaxDnsEntries; ++i) {
            const char* alias = source->h_aliases[i];
            const uint32_t copy = AllocateGuestCopy(host, alias, strlen(alias) + 1,
                                                     allocations, 1);
            if (!copy) return fail();
            aliases.push_back(copy);
        }
    }
    aliases.push_back(0);
    const uint32_t alias_list = AllocateGuestCopy(host, aliases.data(),
                                                   aliases.size() * sizeof(uint32_t),
                                                   allocations);
    if (!alias_list) return fail();

    std::vector<uint32_t> addresses;
    if (source->h_addr_list && source->h_length > 0) {
        for (uint32_t i = 0; source->h_addr_list[i] && i < kMaxDnsEntries; ++i) {
            const uint32_t copy = AllocateGuestCopy(host, source->h_addr_list[i],
                                                     static_cast<size_t>(source->h_length),
                                                     allocations);
            if (!copy) return fail();
            addresses.push_back(copy);
        }
    }
    addresses.push_back(0);
    const uint32_t address_list = AllocateGuestCopy(host, addresses.data(),
                                                     addresses.size() * sizeof(uint32_t),
                                                     allocations);
    if (!address_list) return fail();

    const uint32_t output = host.GuestAlloc(kGuestHostentSize, 4);
    if (!output) return fail();
    allocations.push_back(output);
    host.MemoryWrite32(output + 0, name);
    host.MemoryWrite32(output + 4, alias_list);
    host.MemoryWrite32(output + 8, static_cast<uint32_t>(source->h_addrtype));
    host.MemoryWrite32(output + 12, static_cast<uint32_t>(source->h_length));
    host.MemoryWrite32(output + 16, address_list);

    t_hostent_allocations = std::move(allocations);
    return output;
}

struct NativeMessage {
    msghdr header{};
    std::vector<iovec> iovecs;
    uint32_t guest_header = 0;
    socklen_t name_capacity = 0;
};

bool BuildNativeMessage(DynarmicHost& host, uint32_t guest_header, NativeMessage& output) {
    output.iovecs.clear();
    if (!guest_header || !GuestRangeValid(host, guest_header, kGuestMsghdrSize)) {
        errno = EFAULT;
        return false;
    }

    const uint32_t name = host.MemoryRead32(guest_header + 0);
    const uint32_t name_length = host.MemoryRead32(guest_header + 4);
    const uint32_t iovec_array = host.MemoryRead32(guest_header + 8);
    const uint32_t iovec_count = host.MemoryRead32(guest_header + 12);
    const uint32_t control = host.MemoryRead32(guest_header + 16);
    const uint32_t control_length = host.MemoryRead32(guest_header + 20);

    if (control_length != 0) {
        if (NetTraceEnabled()) {
            LOGI("msghdr reject: control=0x%08x control_length=%u", control, control_length);
        }
        errno = EOPNOTSUPP;
        return false;
    }
    if (iovec_count > kMaxIovecs ||
        (iovec_count && (!iovec_array ||
                         !GuestRangeValid(host, iovec_array,
                                          static_cast<uint64_t>(iovec_count) * kGuestIovecSize)))) {
        if (NetTraceEnabled()) {
            LOGI("msghdr reject: iovec_array=0x%08x iovec_count=%u", iovec_array, iovec_count);
        }
        errno = EINVAL;
        return false;
    }
    if (name_length && (!name || !GuestRangeValid(host, name, name_length))) {
        if (NetTraceEnabled()) {
            LOGI("msghdr reject: name=0x%08x name_length=%u", name, name_length);
        }
        errno = EFAULT;
        return false;
    }

    output.iovecs.reserve(iovec_count);
    for (uint32_t i = 0; i < iovec_count; ++i) {
        const uint32_t entry = iovec_array + i * kGuestIovecSize;
        const uint32_t base = host.MemoryRead32(entry + 0);
        const uint32_t length = host.MemoryRead32(entry + 4);
        if (length && (!base || !GuestRangeValid(host, base, length))) {
            if (NetTraceEnabled()) {
                LOGI("msghdr reject: iovec[%u] base=0x%08x length=%u", i, base, length);
            }
            errno = EFAULT;
            return false;
        }
        iovec native{};
        native.iov_base = length ? host.GetGuestMemoryBase() + base : nullptr;
        native.iov_len = length;
        output.iovecs.push_back(native);
    }

    output.guest_header = guest_header;
    output.name_capacity = name_length;
    output.header.msg_name = name_length ? host.GetGuestMemoryBase() + name : nullptr;
    output.header.msg_namelen = name_length;
    output.header.msg_iov = output.iovecs.empty() ? nullptr : output.iovecs.data();
    output.header.msg_iovlen = output.iovecs.size();
    output.header.msg_control = nullptr;
    output.header.msg_controllen = 0;
    output.header.msg_flags = static_cast<int>(host.MemoryRead32(guest_header + 24));
    (void)control;
    return true;
}

void WriteBackMessage(DynarmicHost& host, const NativeMessage& message) {
    host.MemoryWrite32(message.guest_header + 4,
                       static_cast<uint32_t>(message.header.msg_namelen));
    host.MemoryWrite32(message.guest_header + 20,
                       static_cast<uint32_t>(message.header.msg_controllen));
    host.MemoryWrite32(message.guest_header + 24,
                       static_cast<uint32_t>(message.header.msg_flags));
}

void TraceMessageEndpoint(const char* operation, const NativeMessage& message,
                          int64_t result) {
    if (!NetTraceEnabled() || result < 0) return;

    char endpoint[INET6_ADDRSTRLEN]{};
    uint16_t port = 0;
    int family = AF_UNSPEC;
    const socklen_t readable_name = std::min(message.name_capacity,
                                            message.header.msg_namelen);
    if (message.header.msg_name && readable_name >= sizeof(sa_family_t)) {
        const auto* address = static_cast<const sockaddr*>(message.header.msg_name);
        family = address->sa_family;
        if (family == AF_INET && readable_name >= sizeof(sockaddr_in)) {
            const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(address);
            inet_ntop(AF_INET, &ipv4->sin_addr, endpoint, sizeof(endpoint));
            port = ntohs(ipv4->sin_port);
        } else if (family == AF_INET6 &&
                   readable_name >= sizeof(sockaddr_in6)) {
            const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(address);
            inet_ntop(AF_INET6, &ipv6->sin6_addr, endpoint, sizeof(endpoint));
            port = ntohs(ipv6->sin6_port);
        }
    }

    uint8_t bytes[8]{};
    size_t byte_count = 0;
    int64_t remaining = result;
    for (const iovec& part : message.iovecs) {
        if (remaining <= 0 || byte_count == sizeof(bytes)) break;
        const size_t available = std::min<size_t>(
            part.iov_len, static_cast<size_t>(remaining));
        const size_t copy = std::min(available, sizeof(bytes) - byte_count);
        if (copy) memcpy(bytes + byte_count, part.iov_base, copy);
        byte_count += copy;
        remaining -= static_cast<int64_t>(available);
    }

    LOGI("%s endpoint family=%d address=%s port=%u namelen=%u "
         "payload=%02x%02x%02x%02x%02x%02x%02x%02x",
         operation, family, endpoint[0] ? endpoint : "-", port,
         static_cast<unsigned>(message.header.msg_namelen),
         bytes[0], bytes[1], bytes[2], bytes[3],
         bytes[4], bytes[5], bytes[6], bytes[7]);
}

bool CopyGuestFdSetIn(DynarmicHost& host, uint32_t address, fd_set& output) {
    static_assert(sizeof(fd_set) == kGuestFdSetSize,
                  "AArch64 fd_set must remain the Linux 1024-bit set");
    if (!address) {
        FD_ZERO(&output);
        return true;
    }
    const void* source = GuestSpan(host, address, kGuestFdSetSize);
    if (!source) return false;
    memcpy(&output, source, sizeof(output));
    return true;
}

void CopyGuestFdSetOut(DynarmicHost& host, uint32_t address, const fd_set& input) {
    if (address) memcpy(host.GetGuestMemoryBase() + address, &input, sizeof(input));
}

ThunkHandler MakeSocketNameThunk(const char* name,
                                 int (*function)(int, sockaddr*, socklen_t*)) {
    return [name, function](Jit& jit, DynarmicHost& host) {
        const int fd = static_cast<int>(jit.Regs()[0]);
        const uint32_t address = jit.Regs()[1];
        const uint32_t length_pointer = jit.Regs()[2];
        uint32_t guest_length = 0;
        if (!ReadGuestWord(host, length_pointer, guest_length)) {
            ReturnFailure(jit, name, EFAULT);
            return;
        }
        if (guest_length && (!address || !GuestRangeValid(host, address, guest_length))) {
            ReturnFailure(jit, name, EFAULT);
            return;
        }
        socklen_t native_length = static_cast<socklen_t>(guest_length);
        auto* native_address = guest_length
            ? reinterpret_cast<sockaddr*>(host.GetGuestMemoryBase() + address) : nullptr;
        const int result = function(fd, native_address, &native_length);
        const int saved_errno = errno;
        if (result == 0) host.MemoryWrite32(length_pointer, native_length);
        errno = saved_errno;
        ReturnResult(jit, name, result);
    };
}

bool IsIfreqIoctl(unsigned long request) {
    switch (request) {
        case SIOCGIFNAME:
        case SIOCGIFINDEX:
        case SIOCGIFFLAGS:
        case SIOCGIFADDR:
        case SIOCGIFDSTADDR:
        case SIOCGIFBRDADDR:
        case SIOCGIFNETMASK:
        case SIOCGIFMETRIC:
        case SIOCGIFMTU:
        case SIOCGIFHWADDR:
#ifdef SIOCGIFTXQLEN
        case SIOCGIFTXQLEN:
#endif
            return true;
        default:
            return false;
    }
}

int InterfaceIoctl(DynarmicHost& host, int fd, unsigned long request,
                   uint32_t guest_argument) {
    static_assert(offsetof(ifreq, ifr_addr) == IFNAMSIZ,
                  "native ifreq union must follow its interface name");

    if (request == SIOCGIFCONF) {
        if (!guest_argument ||
            !GuestRangeValid(host, guest_argument, kGuestIfconfSize)) {
            errno = EFAULT;
            return -1;
        }

        const int32_t guest_length =
            static_cast<int32_t>(host.MemoryRead32(guest_argument + 0));
        const uint32_t guest_buffer = host.MemoryRead32(guest_argument + 4);
        if (guest_length < 0) {
            errno = EINVAL;
            return -1;
        }
        if (guest_length != 0 &&
            (!guest_buffer ||
             !GuestRangeValid(host, guest_buffer,
                              static_cast<uint32_t>(guest_length)))) {
            errno = EFAULT;
            return -1;
        }

        const size_t guest_capacity =
            static_cast<uint32_t>(guest_length) / kGuestIfreqSize;
        std::vector<ifreq> native_entries(guest_capacity);
        ifconf native_configuration{};
        native_configuration.ifc_len = static_cast<int>(
            native_entries.size() * sizeof(ifreq));
        native_configuration.ifc_buf = native_entries.empty()
            ? nullptr : reinterpret_cast<char*>(native_entries.data());

        const int result = ioctl(fd, request, &native_configuration);
        const int saved_errno = errno;
        if (result == 0) {
            const size_t native_count = native_configuration.ifc_len > 0
                ? static_cast<size_t>(native_configuration.ifc_len) / sizeof(ifreq)
                : 0;
            const size_t copy_count = std::min(native_count, guest_capacity);
            auto* destination = guest_buffer
                ? host.GetGuestMemoryBase() + guest_buffer : nullptr;
            for (size_t i = 0; i < copy_count; ++i) {
                uint8_t* guest_entry = destination + i * kGuestIfreqSize;
                memset(guest_entry, 0, kGuestIfreqSize);
                memcpy(guest_entry, native_entries[i].ifr_name, IFNAMSIZ);
                memcpy(guest_entry + IFNAMSIZ,
                       reinterpret_cast<const uint8_t*>(&native_entries[i]) + IFNAMSIZ,
                       kGuestIfreqSize - IFNAMSIZ);
                if (NetTraceEnabled() &&
                    native_entries[i].ifr_addr.sa_family == AF_INET) {
                    const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(
                        &native_entries[i].ifr_addr);
                    char address[INET_ADDRSTRLEN]{};
                    inet_ntop(AF_INET, &ipv4->sin_addr, address, sizeof(address));
                    LOGI("SIOCGIFCONF entry=%zu name=%s address=%s",
                         i, native_entries[i].ifr_name, address);
                }
            }
            host.MemoryWrite32(guest_argument + 0,
                               static_cast<uint32_t>(copy_count * kGuestIfreqSize));
        }
        errno = saved_errno;
        return result;
    }

    if (!IsIfreqIoctl(request)) {
        errno = ENOTTY;
        return -1;
    }
    if (!guest_argument ||
        !GuestRangeValid(host, guest_argument, kGuestIfreqSize)) {
        errno = EFAULT;
        return -1;
    }

    ifreq native_request{};
    const uint8_t* guest_request = host.GetGuestMemoryBase() + guest_argument;
    memcpy(native_request.ifr_name, guest_request, IFNAMSIZ);
    memcpy(reinterpret_cast<uint8_t*>(&native_request) + IFNAMSIZ,
           guest_request + IFNAMSIZ, kGuestIfreqSize - IFNAMSIZ);

    const int result = ioctl(fd, request, &native_request);
    const int saved_errno = errno;
    if (result == 0) {
        uint8_t* guest_result = host.GetGuestMemoryBase() + guest_argument;
        memcpy(guest_result, native_request.ifr_name, IFNAMSIZ);
        memcpy(guest_result + IFNAMSIZ,
               reinterpret_cast<const uint8_t*>(&native_request) + IFNAMSIZ,
               kGuestIfreqSize - IFNAMSIZ);
    }
    errno = saved_errno;
    return result;
}

}  // namespace

bool TryOfflineLanAuth(DynarmicHost& host, uint32_t url_object, uint32_t response_object) {
    if (!url_object || !response_object ||
        !GuestRangeValid(host, url_object, 16u) ||
        !GuestRangeValid(host, response_object, 16u)) return false;
    const uint32_t url = host.MemoryRead32(url_object + 4u);
    const uint32_t length = host.MemoryRead32(url_object + 12u);
    const uint32_t url_capacity = host.MemoryRead32(url_object + 8u);
    if (!url || length == 0u || length > 4096u || length >= url_capacity ||
        !GuestRangeValid(host, url, static_cast<uint64_t>(length) + 1u)) return false;
    const char* text = reinterpret_cast<const char*>(host.GetGuestMemoryBase() + url);
    if (text[length] != '\0' || memchr(text, '\0', length) != nullptr ||
        !std::string_view(text, length).starts_with(
            "http://127.0.0.1/handlerservers.ashx?type=chkserialandroid&") ||
        !IsLanSerialCheck(text, length)) return false;

    const uint32_t output = host.MemoryRead32(response_object + 4u);
    const uint32_t capacity = host.MemoryRead32(response_object + 8u);
    const uint32_t old_length = host.MemoryRead32(response_object + 12u);
    if (!output || old_length >= capacity || capacity > 1048576u ||
        !GuestRangeValid(host, output, capacity)) return false;
    const std::string body = MakeLanAuthBody();
    if (body.size() >= capacity - old_length) return false;
    memcpy(host.GetGuestMemoryBase() + output + old_length, body.c_str(), body.size() + 1u);
    host.MemoryWrite32(response_object + 12u, old_length + static_cast<uint32_t>(body.size()));
    return true;
}

void ThunkManager::RegisterNetworkThunks() {
    RegisterThunk("socket", [](Jit& jit, DynarmicHost&) {
        if (NetTraceEnabled()) {
            LOGI("socket domain=%d type=%d protocol=%d", static_cast<int>(jit.Regs()[0]),
                 static_cast<int>(jit.Regs()[1]), static_cast<int>(jit.Regs()[2]));
        }
        ReturnResult(jit, "socket", socket(static_cast<int>(jit.Regs()[0]),
                                             static_cast<int>(jit.Regs()[1]),
                                             static_cast<int>(jit.Regs()[2])));
    });
    RegisterThunk("bind", [](Jit& jit, DynarmicHost& host) {
        const uint32_t address = jit.Regs()[1];
        const uint32_t length = jit.Regs()[2];
        if (!address || !length || !GuestRangeValid(host, address, length)) {
            ReturnFailure(jit, "bind", EFAULT);
            return;
        }
        TraceSockaddr("bind", reinterpret_cast<const sockaddr*>(
                          host.GetGuestMemoryBase() + address),
                      static_cast<socklen_t>(length));
        ReturnResult(jit, "bind", bind(static_cast<int>(jit.Regs()[0]),
                                        reinterpret_cast<const sockaddr*>(
                                            host.GetGuestMemoryBase() + address),
                                        static_cast<socklen_t>(length)));
    });
    RegisterThunk("listen", [](Jit& jit, DynarmicHost&) {
        ReturnResult(jit, "listen", listen(static_cast<int>(jit.Regs()[0]),
                                            static_cast<int>(jit.Regs()[1])));
    });
    RegisterThunk("accept", [](Jit& jit, DynarmicHost& host) {
        const int fd = static_cast<int>(jit.Regs()[0]);
        const uint32_t address = jit.Regs()[1];
        const uint32_t length_pointer = jit.Regs()[2];
        if (!address && !length_pointer) {
            ReturnResult(jit, "accept", accept(fd, nullptr, nullptr));
            return;
        }
        uint32_t guest_length = 0;
        if (!address || !ReadGuestWord(host, length_pointer, guest_length) ||
            !GuestRangeValid(host, address, guest_length)) {
            ReturnFailure(jit, "accept", EFAULT);
            return;
        }
        socklen_t native_length = static_cast<socklen_t>(guest_length);
        const int result = accept(fd,
                                  reinterpret_cast<sockaddr*>(host.GetGuestMemoryBase() + address),
                                  &native_length);
        const int saved_errno = errno;
        if (result >= 0) host.MemoryWrite32(length_pointer, native_length);
        errno = saved_errno;
        ReturnResult(jit, "accept", result);
    });
    RegisterThunk("connect", [](Jit& jit, DynarmicHost& host) {
        const int fd = static_cast<int>(jit.Regs()[0]);
        const uint32_t address = jit.Regs()[1];
        const uint32_t length = jit.Regs()[2];
        if (!address || !length || !GuestRangeValid(host, address, length)) {
            ReturnFailure(jit, "connect", EFAULT);
            return;
        }
        const auto* native_address = reinterpret_cast<const sockaddr*>(
            host.GetGuestMemoryBase() + address);
        ClearLanAuthSocket(fd);
        TraceSockaddr("connect", native_address, static_cast<socklen_t>(length));
        ReturnResult(jit, "connect", connect(fd, native_address,
                                              static_cast<socklen_t>(length)));
    });
    RegisterThunk("shutdown", [](Jit& jit, DynarmicHost&) {
        ReturnResult(jit, "shutdown", shutdown(static_cast<int>(jit.Regs()[0]),
                                                static_cast<int>(jit.Regs()[1])));
    });
    RegisterThunk("send", [](Jit& jit, DynarmicHost& host) {
        const int fd = static_cast<int>(jit.Regs()[0]);
        const uint32_t buffer = jit.Regs()[1];
        const uint32_t length = jit.Regs()[2];
        if (length && (!buffer || !GuestRangeValid(host, buffer, length))) {
            ReturnFailure(jit, "send", EFAULT);
            return;
        }
        const void* native_buffer = length ? host.GetGuestMemoryBase() + buffer : nullptr;
        MarkLanAuthSocket(fd, native_buffer, length);
        TraceBuffer("send", native_buffer, length);
        ReturnResult(jit, "send", send(fd, native_buffer, length,
                                        static_cast<int>(jit.Regs()[3])));
    });
    RegisterThunk("recv", [](Jit& jit, DynarmicHost& host) {
        const int fd = static_cast<int>(jit.Regs()[0]);
        const uint32_t buffer = jit.Regs()[1];
        const uint32_t length = jit.Regs()[2];
        if (length && (!buffer || !GuestRangeValid(host, buffer, length))) {
            ReturnFailure(jit, "recv", EFAULT);
            return;
        }
        void* native_buffer = length ? host.GetGuestMemoryBase() + buffer : nullptr;
        // MSG_TRUNC on a dgram socket would make recv() report the full
        // datagram size while only `length` bytes were stored; strip it for the
        // same reason as in the recvmsg thunk.
        const int flags = static_cast<int>(jit.Regs()[3]) & ~MSG_TRUNC;
        ssize_t result = recv(fd, native_buffer, length, flags);
        result = ApplyLanAuthFallback(fd, native_buffer, length, result);
        if (result > 0) TraceBuffer("recv", native_buffer, static_cast<size_t>(result));
        ReturnResult(jit, "recv", result);
    });
    RegisterThunk("sendmsg", [](Jit& jit, DynarmicHost& host) {
        thread_local NativeMessage message;
        if (!BuildNativeMessage(host, jit.Regs()[1], message)) {
            ReturnResult(jit, "sendmsg", -1);
            return;
        }
        const ssize_t result = sendmsg(static_cast<int>(jit.Regs()[0]), &message.header,
                                       static_cast<int>(jit.Regs()[2]));
        TraceMessageEndpoint("sendmsg", message, result);
        ReturnResult(jit, "sendmsg", result);
    });
    RegisterThunk("recvmsg", [](Jit& jit, DynarmicHost& host) {
        thread_local NativeMessage message;
        if (!BuildNativeMessage(host, jit.Regs()[1], message)) {
            ReturnResult(jit, "recvmsg", -1);
            return;
        }
        // MSG_TRUNC makes the kernel report the full datagram length even when
        // only iov capacity bytes were stored. Guests that size their buffers
        // from the return value then read past them (libMBExpMobile.so does
        // exactly this in mbnet_protocol_receive_incoming_commands). Strip the
        // flag so the return value always equals the bytes actually delivered.
        const int flags = static_cast<int>(jit.Regs()[2]) & ~MSG_TRUNC;
        const ssize_t result = recvmsg(static_cast<int>(jit.Regs()[0]), &message.header,
                                       flags);
        const int saved_errno = errno;
        if (result >= 0) {
            WriteBackMessage(host, message);
            TraceMessageEndpoint("recvmsg", message, result);
        }
        errno = saved_errno;
        ReturnResult(jit, "recvmsg", result);
    });

    RegisterThunk("select", [](Jit& jit, DynarmicHost& host) {
        const int descriptor_count = static_cast<int>(jit.Regs()[0]);
        if (descriptor_count < 0 || descriptor_count > FD_SETSIZE) {
            ReturnFailure(jit, "select", EINVAL);
            return;
        }
        const uint32_t read_address = jit.Regs()[1];
        const uint32_t write_address = jit.Regs()[2];
        const uint32_t except_address = jit.Regs()[3];
        uint32_t timeout_address = 0;
        if (!ReadStackWord(jit, host, 0, timeout_address)) {
            ReturnFailure(jit, "select", EFAULT);
            return;
        }

        fd_set read_set{};
        fd_set write_set{};
        fd_set except_set{};
        if (!CopyGuestFdSetIn(host, read_address, read_set) ||
            !CopyGuestFdSetIn(host, write_address, write_set) ||
            !CopyGuestFdSetIn(host, except_address, except_set)) {
            ReturnFailure(jit, "select", EFAULT);
            return;
        }

        timeval timeout{};
        timeval* timeout_pointer = nullptr;
        if (timeout_address) {
            if (!GuestRangeValid(host, timeout_address, kGuestTimevalSize)) {
                ReturnFailure(jit, "select", EFAULT);
                return;
            }
            timeout.tv_sec = static_cast<int32_t>(host.MemoryRead32(timeout_address + 0));
            timeout.tv_usec = static_cast<int32_t>(host.MemoryRead32(timeout_address + 4));
            timeout_pointer = &timeout;
        }

        const int result = select(descriptor_count,
                                  read_address ? &read_set : nullptr,
                                  write_address ? &write_set : nullptr,
                                  except_address ? &except_set : nullptr,
                                  timeout_pointer);
        const int saved_errno = errno;
        if (result >= 0) {
            CopyGuestFdSetOut(host, read_address, read_set);
            CopyGuestFdSetOut(host, write_address, write_set);
            CopyGuestFdSetOut(host, except_address, except_set);
            if (timeout_address) {
                const int64_t seconds = std::clamp<int64_t>(timeout.tv_sec,
                    std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max());
                const int64_t microseconds = std::clamp<int64_t>(timeout.tv_usec,
                    std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max());
                host.MemoryWrite32(timeout_address + 0, static_cast<uint32_t>(seconds));
                host.MemoryWrite32(timeout_address + 4, static_cast<uint32_t>(microseconds));
            }
        }
        errno = saved_errno;
        ReturnResult(jit, "select", result);
    });
    RegisterThunk("poll", [](Jit& jit, DynarmicHost& host) {
        static_assert(sizeof(pollfd) == 8, "pollfd ABI must match ARM32 Linux");
        const uint32_t array = jit.Regs()[0];
        const uint32_t count = jit.Regs()[1];
        if (count && (!array || !GuestRangeValid(host, array, static_cast<uint64_t>(count) * 8))) {
            ReturnFailure(jit, "poll", EFAULT);
            return;
        }
        auto* descriptors = count
            ? reinterpret_cast<pollfd*>(host.GetGuestMemoryBase() + array) : nullptr;
        ReturnResult(jit, "poll", poll(descriptors, count, static_cast<int32_t>(jit.Regs()[2])));
    });

    RegisterThunk("getsockname", MakeSocketNameThunk("getsockname", &getsockname));
    RegisterThunk("getpeername", MakeSocketNameThunk("getpeername", &getpeername));

    RegisterThunk("setsockopt", [](Jit& jit, DynarmicHost& host) {
        uint32_t guest_length = 0;
        if (!ReadStackWord(jit, host, 0, guest_length)) {
            ReturnFailure(jit, "setsockopt", EFAULT);
            return;
        }
        const int level = static_cast<int>(jit.Regs()[1]);
        const int option = static_cast<int>(jit.Regs()[2]);
        const uint32_t value = jit.Regs()[3];
        if (guest_length && (!value || !GuestRangeValid(host, value, guest_length))) {
            ReturnFailure(jit, "setsockopt", EFAULT);
            return;
        }
        const void* native_value = guest_length ? host.GetGuestMemoryBase() + value : nullptr;
        socklen_t native_length = static_cast<socklen_t>(guest_length);
        timeval timeout{};
        if (level == SOL_SOCKET && (option == SO_RCVTIMEO || option == SO_SNDTIMEO)) {
            if (guest_length < kGuestTimevalSize) {
                ReturnFailure(jit, "setsockopt", EINVAL);
                return;
            }
            timeout.tv_sec = static_cast<int32_t>(host.MemoryRead32(value + 0));
            timeout.tv_usec = static_cast<int32_t>(host.MemoryRead32(value + 4));
            native_value = &timeout;
            native_length = sizeof(timeout);
        }
        ReturnResult(jit, "setsockopt", setsockopt(static_cast<int>(jit.Regs()[0]), level,
                                                    option, native_value, native_length));
    });
    RegisterThunk("getsockopt", [](Jit& jit, DynarmicHost& host) {
        uint32_t length_pointer = 0;
        if (!ReadStackWord(jit, host, 0, length_pointer)) {
            ReturnFailure(jit, "getsockopt", EFAULT);
            return;
        }
        uint32_t guest_length = 0;
        if (!ReadGuestWord(host, length_pointer, guest_length)) {
            ReturnFailure(jit, "getsockopt", EFAULT);
            return;
        }
        const int level = static_cast<int>(jit.Regs()[1]);
        const int option = static_cast<int>(jit.Regs()[2]);
        const uint32_t value = jit.Regs()[3];
        if (guest_length && (!value || !GuestRangeValid(host, value, guest_length))) {
            ReturnFailure(jit, "getsockopt", EFAULT);
            return;
        }

        void* native_value = guest_length ? host.GetGuestMemoryBase() + value : nullptr;
        socklen_t native_length = static_cast<socklen_t>(guest_length);
        timeval timeout{};
        const bool is_timeout = level == SOL_SOCKET &&
                                (option == SO_RCVTIMEO || option == SO_SNDTIMEO);
        if (is_timeout) {
            if (guest_length < kGuestTimevalSize) {
                ReturnFailure(jit, "getsockopt", EINVAL);
                return;
            }
            native_value = &timeout;
            native_length = sizeof(timeout);
        }
        const int result = getsockopt(static_cast<int>(jit.Regs()[0]), level, option,
                                      native_value, &native_length);
        const int saved_errno = errno;
        if (result == 0) {
            if (NetTraceEnabled() && !is_timeout && native_value && native_length > 0) {
                TraceBuffer("getsockopt value", native_value, native_length);
                LOGI("getsockopt level=%d option=%d length=%u", level, option,
                     static_cast<unsigned>(native_length));
            }
            if (is_timeout) {
                if (timeout.tv_sec < std::numeric_limits<int32_t>::min() ||
                    timeout.tv_sec > std::numeric_limits<int32_t>::max() ||
                    timeout.tv_usec < std::numeric_limits<int32_t>::min() ||
                    timeout.tv_usec > std::numeric_limits<int32_t>::max()) {
                    ReturnFailure(jit, "getsockopt", EOVERFLOW);
                    return;
                }
                host.MemoryWrite32(value + 0, static_cast<uint32_t>(timeout.tv_sec));
                host.MemoryWrite32(value + 4, static_cast<uint32_t>(timeout.tv_usec));
                native_length = kGuestTimevalSize;
            }
            host.MemoryWrite32(length_pointer, native_length);
        }
        errno = saved_errno;
        ReturnResult(jit, "getsockopt", result);
    });

    RegisterThunk("fcntl", [](Jit& jit, DynarmicHost&) {
        const int fd = static_cast<int>(jit.Regs()[0]);
        const int command = static_cast<int>(jit.Regs()[1]);
        const int argument = static_cast<int>(jit.Regs()[2]);
        int result = -1;
        switch (command) {
            case F_GETFD:
            case F_GETFL:
                result = fcntl(fd, command);
                break;
            case F_DUPFD:
#ifdef F_DUPFD_CLOEXEC
            case F_DUPFD_CLOEXEC:
#endif
            case F_SETFD:
            case F_SETFL:
                result = fcntl(fd, command, argument);
                break;
            default:
                errno = ENOTSUP;
                break;
        }
        ReturnResult(jit, "fcntl", result);
    });
    RegisterThunk("ioctl", [](Jit& jit, DynarmicHost& host) {
        const int fd = static_cast<int>(jit.Regs()[0]);
        const unsigned long request = jit.Regs()[1];
        const uint32_t argument = jit.Regs()[2];
        if (request == FIONBIO || request == FIONREAD) {
            if (!argument || !GuestRangeValid(host, argument, sizeof(int))) {
                ReturnFailure(jit, "ioctl", EFAULT);
                return;
            }
            ReturnResult(jit, "ioctl",
                         ioctl(fd, request, host.GetGuestMemoryBase() + argument));
            return;
        }
        const int result = InterfaceIoctl(host, fd, request, argument);
        const int saved_errno = errno;
        if (NetTraceEnabled()) {
            LOGI("ioctl request=0x%lx argument=0x%08x result=%d errno=%d",
                 request, argument, result, result < 0 ? saved_errno : 0);
        }
        errno = saved_errno;
        ReturnResult(jit, "ioctl", result);
    });

    RegisterThunk("inet_addr", [](Jit& jit, DynarmicHost& host) {
        const char* text = GuestString(host, jit.Regs()[0]);
        if (!text) {
            errno = EFAULT;
            jit.Regs()[0] = INADDR_NONE;
            return;
        }
        jit.Regs()[0] = inet_addr(text);
    });
    RegisterThunk("inet_aton", [](Jit& jit, DynarmicHost& host) {
        const char* text = GuestString(host, jit.Regs()[0]);
        const uint32_t output = jit.Regs()[1];
        if (!text || !output || !GuestRangeValid(host, output, sizeof(in_addr))) {
            errno = EFAULT;
            jit.Regs()[0] = 0;
            return;
        }
        in_addr address{};
        const int result = inet_aton(text, &address);
        if (result) host.MemoryWrite32(output, address.s_addr);
        jit.Regs()[0] = static_cast<uint32_t>(result);
    });
    RegisterThunk("inet_ntoa", [](Jit& jit, DynarmicHost& host) {
        in_addr address{};
        address.s_addr = jit.Regs()[0];
        char text[INET_ADDRSTRLEN]{};
        if (!inet_ntop(AF_INET, &address, text, sizeof(text))) {
            jit.Regs()[0] = 0;
            return;
        }
        if (!t_inet_ntoa_buffer) t_inet_ntoa_buffer = host.GuestAlloc(sizeof(text), 1);
        if (!t_inet_ntoa_buffer) {
            errno = ENOMEM;
            jit.Regs()[0] = 0;
            return;
        }
        memcpy(host.GetGuestMemoryBase() + t_inet_ntoa_buffer, text, sizeof(text));
        jit.Regs()[0] = t_inet_ntoa_buffer;
    });
    RegisterThunk("inet_ntop", [](Jit& jit, DynarmicHost& host) {
        const int family = static_cast<int>(jit.Regs()[0]);
        const uint32_t input = jit.Regs()[1];
        const uint32_t output = jit.Regs()[2];
        const uint32_t output_length = jit.Regs()[3];
        const size_t input_length = family == AF_INET ? 4 : (family == AF_INET6 ? 16 : 0);
        if (!input_length) {
            errno = EAFNOSUPPORT;
            jit.Regs()[0] = 0;
            return;
        }
        if (!input || !output || !GuestRangeValid(host, input, input_length) ||
            !GuestRangeValid(host, output, output_length)) {
            errno = EFAULT;
            jit.Regs()[0] = 0;
            return;
        }
        const char* result = inet_ntop(family, host.GetGuestMemoryBase() + input,
                                       reinterpret_cast<char*>(host.GetGuestMemoryBase() + output),
                                       output_length);
        jit.Regs()[0] = result ? output : 0;
    });
    RegisterThunk("inet_pton", [](Jit& jit, DynarmicHost& host) {
        const int family = static_cast<int>(jit.Regs()[0]);
        const char* input = GuestString(host, jit.Regs()[1]);
        const uint32_t output = jit.Regs()[2];
        const size_t output_length = family == AF_INET ? 4 : (family == AF_INET6 ? 16 : 0);
        if (!output_length) {
            ReturnFailure(jit, "inet_pton", EAFNOSUPPORT);
            return;
        }
        if (!input || !output || !GuestRangeValid(host, output, output_length)) {
            ReturnFailure(jit, "inet_pton", EFAULT);
            return;
        }
        ReturnResult(jit, "inet_pton", inet_pton(family, input,
                                                  host.GetGuestMemoryBase() + output));
    });

    RegisterThunk("gethostbyname", [](Jit& jit, DynarmicHost& host) {
        const char* name = GuestString(host, jit.Regs()[0]);
        if (!name) {
            errno = EFAULT;
            h_errno = NO_RECOVERY;
            jit.Regs()[0] = 0;
            return;
        }
        hostent* result = gethostbyname(name);
        jit.Regs()[0] = CopyHostentToGuest(host, result);
        if (NetTraceEnabled()) {
            char address[INET6_ADDRSTRLEN]{};
            if (result && result->h_addr_list && result->h_addr_list[0]) {
                inet_ntop(result->h_addrtype, result->h_addr_list[0], address,
                          sizeof(address));
            }
            LOGI("gethostbyname name=%s result=%s address=%s h_errno=%d",
                 name, result ? "ok" : "null", address[0] ? address : "-",
                 result ? 0 : h_errno);
        }
    });
    RegisterThunk("gethostbyaddr", [](Jit& jit, DynarmicHost& host) {
        const uint32_t address = jit.Regs()[0];
        const uint32_t length = jit.Regs()[1];
        const int type = static_cast<int>(jit.Regs()[2]);
        if (!address || !length || !GuestRangeValid(host, address, length)) {
            errno = EFAULT;
            h_errno = NO_RECOVERY;
            jit.Regs()[0] = 0;
            return;
        }
        hostent* result = gethostbyaddr(host.GetGuestMemoryBase() + address, length, type);
        jit.Regs()[0] = CopyHostentToGuest(host, result);
        if (NetTraceEnabled()) LOGI("gethostbyaddr result=%s h_errno=%d", result ? "ok" : "null",
                                    result ? 0 : h_errno);
    });

    RegisterThunk("getaddrinfo", [](Jit& jit, DynarmicHost& host) {
        const uint32_t node_address = jit.Regs()[0];
        const uint32_t service_address = jit.Regs()[1];
        const uint32_t hints_address = jit.Regs()[2];
        const uint32_t output_address = jit.Regs()[3];
        if (!output_address || !GuestRangeValid(host, output_address, sizeof(uint32_t))) {
            errno = EFAULT;
            jit.Regs()[0] = static_cast<uint32_t>(EAI_SYSTEM);
            return;
        }
        host.MemoryWrite32(output_address, 0);
        const char* node = GuestString(host, node_address);
        const char* service = GuestString(host, service_address);
        if ((node_address && !node) || (service_address && !service)) {
            errno = EFAULT;
            jit.Regs()[0] = static_cast<uint32_t>(EAI_SYSTEM);
            return;
        }
        if (NetTraceEnabled()) {
            LOGI("getaddrinfo query node=%s service=%s", node ? node : "-",
                 service ? service : "-");
        }

        addrinfo hints{};
        const addrinfo* native_hints = nullptr;
        if (hints_address) {
            if (!GuestRangeValid(host, hints_address, kGuestAddrinfoSize)) {
                errno = EFAULT;
                jit.Regs()[0] = static_cast<uint32_t>(EAI_SYSTEM);
                return;
            }
            hints.ai_flags = static_cast<int>(host.MemoryRead32(hints_address + 0));
            hints.ai_family = static_cast<int>(host.MemoryRead32(hints_address + 4));
            hints.ai_socktype = static_cast<int>(host.MemoryRead32(hints_address + 8));
            hints.ai_protocol = static_cast<int>(host.MemoryRead32(hints_address + 12));
            if (host.MemoryRead32(hints_address + 16) ||
                host.MemoryRead32(hints_address + 20) ||
                host.MemoryRead32(hints_address + 24) ||
                host.MemoryRead32(hints_address + 28)) {
#ifdef EAI_BADHINTS
                jit.Regs()[0] = static_cast<uint32_t>(EAI_BADHINTS);
#else
                jit.Regs()[0] = static_cast<uint32_t>(EAI_BADFLAGS);
#endif
                return;
            }
            native_hints = &hints;
        }

        addrinfo* native_result = nullptr;
        int result = getaddrinfo(node, service, native_hints, &native_result);
        const int dns_errno = errno;
        if (result != 0) {
            errno = dns_errno;
            if (NetTraceEnabled()) LOGI("getaddrinfo result=%d errno=%d", result,
                                        result == EAI_SYSTEM ? dns_errno : 0);
            jit.Regs()[0] = static_cast<uint32_t>(result);
            return;
        }

        std::vector<uint32_t> allocations;
        uint32_t first = 0;
        uint32_t previous = 0;
        for (const addrinfo* item = native_result; item; item = item->ai_next) {
            TraceSockaddr("getaddrinfo result", item->ai_addr,
                          static_cast<socklen_t>(item->ai_addrlen));
            const uint32_t guest_item = AllocateGuestCopy(host, nullptr, kGuestAddrinfoSize,
                                                           allocations);
            if (!guest_item) {
                result = EAI_MEMORY;
                break;
            }
            uint32_t guest_name = 0;
            uint32_t guest_address = 0;
            if (item->ai_canonname) {
                guest_name = AllocateGuestCopy(host, item->ai_canonname,
                                               strlen(item->ai_canonname) + 1,
                                               allocations, 1);
                if (!guest_name) {
                    result = EAI_MEMORY;
                    break;
                }
            }
            if (item->ai_addr && item->ai_addrlen) {
                guest_address = AllocateGuestCopy(host, item->ai_addr, item->ai_addrlen,
                                                  allocations);
                if (!guest_address) {
                    result = EAI_MEMORY;
                    break;
                }
            }
            host.MemoryWrite32(guest_item + 0, static_cast<uint32_t>(item->ai_flags));
            host.MemoryWrite32(guest_item + 4, static_cast<uint32_t>(item->ai_family));
            host.MemoryWrite32(guest_item + 8, static_cast<uint32_t>(item->ai_socktype));
            host.MemoryWrite32(guest_item + 12, static_cast<uint32_t>(item->ai_protocol));
            host.MemoryWrite32(guest_item + 16, static_cast<uint32_t>(item->ai_addrlen));
            host.MemoryWrite32(guest_item + 20, guest_name);
            host.MemoryWrite32(guest_item + 24, guest_address);
            host.MemoryWrite32(guest_item + 28, 0);
            if (previous) host.MemoryWrite32(previous + 28, guest_item);
            else first = guest_item;
            previous = guest_item;
        }
        freeaddrinfo(native_result);

        if (result != 0 || !first) {
            FreeGuestAllocations(host, allocations);
            jit.Regs()[0] = static_cast<uint32_t>(result != 0 ? result : EAI_FAIL);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(g_addrinfo_mutex);
            g_addrinfo_allocations.emplace(first, std::move(allocations));
        }
        host.MemoryWrite32(output_address, first);
        if (NetTraceEnabled()) LOGI("getaddrinfo result=0");
        jit.Regs()[0] = 0;
    });
    RegisterThunk("freeaddrinfo", [](Jit& jit, DynarmicHost& host) {
        const uint32_t address = jit.Regs()[0];
        if (!address) return;
        std::vector<uint32_t> allocations;
        {
            std::lock_guard<std::mutex> lock(g_addrinfo_mutex);
            const auto found = g_addrinfo_allocations.find(address);
            if (found == g_addrinfo_allocations.end()) {
                if (NetTraceEnabled()) LOGE("freeaddrinfo ignored unknown guest address 0x%08x", address);
                return;
            }
            allocations = std::move(found->second);
            g_addrinfo_allocations.erase(found);
        }
        FreeGuestAllocations(host, allocations);
    });
}
