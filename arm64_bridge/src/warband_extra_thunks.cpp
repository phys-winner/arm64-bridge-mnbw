#include "dynarmic_host.h"
#include "native_activity_bridge.h"
#include "thunk_manager.h"
#include "thunk_types.h"

#include <android/configuration.h>
#include <android/input.h>
#include <android/native_activity.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <mutex>
#include <string>

#include <android/log.h>

#define LOG_TAG "WarbandExtraThunks"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {
using Jit = Dynarmic::A32::Jit;

uint32_t FloatBits(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

void WriteDouble(Jit& jit, double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    jit.Regs()[0] = static_cast<uint32_t>(bits);
    jit.Regs()[1] = static_cast<uint32_t>(bits >> 32);
}

float ReadFloat(Jit& jit, int index) {
    float value;
    uint32_t bits = jit.Regs()[index];
    memcpy(&value, &bits, sizeof(value));
    return value;
}

void WriteFloat(Jit& jit, float value) { jit.Regs()[0] = FloatBits(value); }

char* GuestChars(DynarmicHost& host, uint32_t address) {
    return reinterpret_cast<char*>(GuestToHost(host.GetGuestMemoryBase(), address));
}

uint32_t GuestStringResult(DynarmicHost& host, uint32_t base, const char* start,
                           const char* result) {
    if (!result) return 0;
    return base + static_cast<uint32_t>(result - start);
}

void RegisterConfigurationThunks(ThunkManager& manager) {
    auto config_int = [](int32_t (*fn)(AConfiguration*)) {
        return [fn](Jit& jit, DynarmicHost&) {
            auto* config = static_cast<AConfiguration*>(HostPointerFor(jit.Regs()[0]));
            jit.Regs()[0] = config ? static_cast<uint32_t>(fn(config)) : 0;
        };
    };
    manager.RegisterThunk("AConfiguration_getDensity", config_int(&AConfiguration_getDensity));
    manager.RegisterThunk("AConfiguration_getKeyboard", config_int(&AConfiguration_getKeyboard));
    manager.RegisterThunk("AConfiguration_getKeysHidden", config_int(&AConfiguration_getKeysHidden));
    manager.RegisterThunk("AConfiguration_getMcc", config_int(&AConfiguration_getMcc));
    manager.RegisterThunk("AConfiguration_getMnc", config_int(&AConfiguration_getMnc));
    manager.RegisterThunk("AConfiguration_getNavHidden", config_int(&AConfiguration_getNavHidden));
    manager.RegisterThunk("AConfiguration_getNavigation", config_int(&AConfiguration_getNavigation));
    manager.RegisterThunk("AConfiguration_getOrientation", config_int(&AConfiguration_getOrientation));
    manager.RegisterThunk("AConfiguration_getScreenLong", config_int(&AConfiguration_getScreenLong));
    manager.RegisterThunk("AConfiguration_getScreenSize", config_int(&AConfiguration_getScreenSize));
    manager.RegisterThunk("AConfiguration_getSdkVersion", config_int(&AConfiguration_getSdkVersion));
    manager.RegisterThunk("AConfiguration_getTouchscreen", config_int(&AConfiguration_getTouchscreen));
    manager.RegisterThunk("AConfiguration_getUiModeNight", config_int(&AConfiguration_getUiModeNight));
    manager.RegisterThunk("AConfiguration_getUiModeType", config_int(&AConfiguration_getUiModeType));

    auto config_string = [](void (*fn)(AConfiguration*, char*)) {
        return [fn](Jit& jit, DynarmicHost& host) {
            auto* config = static_cast<AConfiguration*>(HostPointerFor(jit.Regs()[0]));
            uint32_t output = jit.Regs()[1];
            if (config && output) {
                char value[2] = {0, 0};
                fn(config, value);
                host.MemoryWrite8(output, static_cast<uint8_t>(value[0]));
                host.MemoryWrite8(output + 1, static_cast<uint8_t>(value[1]));
            }
            jit.Regs()[0] = 0;
        };
    };
    manager.RegisterThunk("AConfiguration_getCountry", config_string(&AConfiguration_getCountry));
    manager.RegisterThunk("AConfiguration_getLanguage", config_string(&AConfiguration_getLanguage));
}

}  // namespace

void ThunkManager::RegisterWarbandExtraThunks() {
    RegisterConfigurationThunks(*this);
    RegisterThunk("nv_android_init", [](Jit& jit, DynarmicHost&) { jit.Regs()[0] = 0; });

    RegisterThunk("ANativeActivity_finish", [](Jit& jit, DynarmicHost&) {
        LOGE("guest requested ANativeActivity_finish");
        jit.Regs()[0] = 0;
    });

    static uint8_t storage_manager_token;
    RegisterThunk("AStorageManager_new", [](Jit& jit, DynarmicHost&) {
        jit.Regs()[0] = GuestHandleFor(&storage_manager_token);
    });
    RegisterThunk("AStorageManager_delete", [](Jit& jit, DynarmicHost&) { jit.Regs()[0] = 0; });
    RegisterThunk("AStorageManager_mountObb", [](Jit& jit, DynarmicHost&) { jit.Regs()[0] = 0; });
    RegisterThunk("AStorageManager_unmountObb", [](Jit& jit, DynarmicHost&) { jit.Regs()[0] = 0; });
    RegisterThunk("AStorageManager_isObbMounted", [](Jit& jit, DynarmicHost&) {
        jit.Regs()[0] = 0;
    });
    RegisterThunk("AStorageManager_getMountedObbPath", [](Jit& jit, DynarmicHost&) {
        jit.Regs()[0] = 0;
    });

    RegisterThunk("alarm", [](Jit& jit, DynarmicHost&) { jit.Regs()[0] = 0; });
    RegisterThunk("getpwuid", [](Jit& jit, DynarmicHost&) { jit.Regs()[0] = 0; });
    RegisterThunk("basename", [](Jit& jit, DynarmicHost& host) {
        const uint32_t base = jit.Regs()[0];
        const char* text = GuestChars(host, base);
        if (!text) { jit.Regs()[0] = 0; return; }
        const char* slash = strrchr(text, '/');
        jit.Regs()[0] = GuestStringResult(host, base, text, slash ? slash + 1 : text);
    });
    RegisterThunk("difftime", [](Jit& jit, DynarmicHost&) {
        const double result = static_cast<double>(static_cast<int32_t>(jit.Regs()[0])) -
                              static_cast<double>(static_cast<int32_t>(jit.Regs()[1]));
        WriteDouble(jit, result);
    });
    RegisterThunk("gmtime_r", [](Jit& jit, DynarmicHost& host) {
        const time_t value = static_cast<time_t>(static_cast<int32_t>(host.MemoryRead32(jit.Regs()[0])));
        struct tm result{};
        uint32_t output = jit.Regs()[1];
        if (!output || !gmtime_r(&value, &result)) {
            jit.Regs()[0] = 0;
            return;
        }
        const int values[] = {result.tm_sec, result.tm_min, result.tm_hour, result.tm_mday,
                              result.tm_mon, result.tm_year, result.tm_wday, result.tm_yday,
                              result.tm_isdst};
        for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
            host.MemoryWrite32(output + i * 4, static_cast<uint32_t>(values[i]));
        }
        host.MemoryWrite32(output + 36, static_cast<uint32_t>(result.tm_gmtoff));
        host.MemoryWrite32(output + 40, 0);
        jit.Regs()[0] = output;
    });
    RegisterThunk("exp2f", [](Jit& jit, DynarmicHost&) { WriteFloat(jit, exp2f(ReadFloat(jit, 0))); });
    RegisterThunk("memrchr", [](Jit& jit, DynarmicHost& host) {
        const uint32_t base = jit.Regs()[0];
        const void* start = GuestToHost(host.GetGuestMemoryBase(), base);
        const void* result = start ? memrchr(start, static_cast<int>(jit.Regs()[1]), jit.Regs()[2]) : nullptr;
        jit.Regs()[0] = result ? base + static_cast<uint32_t>(static_cast<const uint8_t*>(result) -
                                                               static_cast<const uint8_t*>(start)) : 0;
    });
    RegisterThunk("realpath", [](Jit& jit, DynarmicHost& host) {
        const char* path = GuestChars(host, jit.Regs()[0]);
        uint32_t output = jit.Regs()[1];
        char resolved[PATH_MAX];
        if (!path || !realpath(path, resolved)) { jit.Regs()[0] = 0; return; }
        if (!output) output = host.GuestAlloc(strlen(resolved) + 1, 1);
        if (output) {
            strcpy(GuestChars(host, output), resolved);
            jit.Regs()[0] = output;
        } else {
            jit.Regs()[0] = 0;
        }
    });
    RegisterThunk("strerror_r", [](Jit& jit, DynarmicHost& host) {
        char* output = GuestChars(host, jit.Regs()[1]);
        size_t size = jit.Regs()[2];
        const char* error = strerror(static_cast<int>(jit.Regs()[0]));
        if (!output || !size) { jit.Regs()[0] = static_cast<uint32_t>(EINVAL); return; }
        strncpy(output, error ? error : "", size - 1);
        output[size - 1] = '\0';
        jit.Regs()[0] = 0;
    });
    RegisterThunk("strlcat", [](Jit& jit, DynarmicHost& host) {
        char* dst = GuestChars(host, jit.Regs()[0]);
        const char* src = GuestChars(host, jit.Regs()[1]);
        const size_t size = jit.Regs()[2];
        if (!dst || !src) { jit.Regs()[0] = 0; return; }
        const size_t dst_len = strnlen(dst, size);
        const size_t src_len = strlen(src);
        if (dst_len < size) {
            const size_t copy = std::min(src_len, size - dst_len - 1);
            memcpy(dst + dst_len, src, copy);
            dst[dst_len + copy] = '\0';
        }
        jit.Regs()[0] = static_cast<uint32_t>(dst_len + src_len);
    });
    RegisterThunk("strtoll", [](Jit& jit, DynarmicHost& host) {
        const uint32_t base = jit.Regs()[0];
        const char* text = GuestChars(host, base);
        const uint32_t end_out = jit.Regs()[1];
        char* end = nullptr;
        const long long value = text ? strtoll(text, &end, static_cast<int>(jit.Regs()[2])) : 0;
        if (end_out && text && end) host.MemoryWrite32(end_out, GuestStringResult(host, base, text, end));
        const uint64_t bits = static_cast<uint64_t>(value);
        jit.Regs()[0] = static_cast<uint32_t>(bits);
        jit.Regs()[1] = static_cast<uint32_t>(bits >> 32);
    });
    RegisterThunk("wcsftime", [](Jit& jit, DynarmicHost& host) {
        auto* output = reinterpret_cast<wchar_t*>(GuestChars(host, jit.Regs()[0]));
        const wchar_t* format = reinterpret_cast<const wchar_t*>(GuestChars(host, jit.Regs()[2]));
        auto* time_value = reinterpret_cast<const struct tm*>(GuestChars(host, jit.Regs()[3]));
        jit.Regs()[0] = (output && format && time_value)
            ? static_cast<uint32_t>(wcsftime(output, jit.Regs()[1], format, time_value)) : 0;
    });
    RegisterThunk("wcsxfrm", [](Jit& jit, DynarmicHost& host) {
        auto* output = reinterpret_cast<wchar_t*>(GuestChars(host, jit.Regs()[0]));
        const auto* input = reinterpret_cast<const wchar_t*>(GuestChars(host, jit.Regs()[1]));
        jit.Regs()[0] = (output && input)
            ? static_cast<uint32_t>(wcsxfrm(output, input, jit.Regs()[2])) : 0;
    });
    RegisterThunk("getwc", [](Jit& jit, DynarmicHost&) {
        jit.Regs()[0] = static_cast<uint32_t>(WEOF);
    });
    RegisterThunk("putwc", [](Jit& jit, DynarmicHost&) {
        jit.Regs()[0] = static_cast<uint32_t>(WEOF);
    });
    RegisterThunk("ungetwc", [](Jit& jit, DynarmicHost&) {
        jit.Regs()[0] = static_cast<uint32_t>(WEOF);
    });
}
