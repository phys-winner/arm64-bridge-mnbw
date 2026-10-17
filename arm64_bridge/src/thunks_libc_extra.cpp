// The rest of libc.
//
// The reference bridge only ever needed what Galaxy on Fire 2 called. Mono brings
// a much wider surface: ctype and string helpers it calls as real functions rather
// than macros, the wide-character family behind System.String, directory walking,
// and a handful of process queries. Everything missing used to land on the default
// stub, which returns zero - so isspace() said "no", strtok() said "no more
// tokens", and Mono's path handling quietly produced empty strings.
//
// Pointer-returning functions are the ones worth reading twice: they must hand
// back a *guest* address, computed as the offset of the host result from the host
// pointer they were given.

#include "thunk_manager.h"
#include "thunk_types.h"
#include "apk_reader.h"
#include "game_data_vfs.h"
#include "guest_signals.h"

#include <algorithm>
#include <vector>
#include <cstring>
#include <android/log.h>
#include <dirent.h>
#include <netdb.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include <cstring>
#include <cwchar>
#include <cwctype>
#include <mutex>
#include <unordered_map>

#include <dynarmic/interface/A32/a32.h>

#include "dynarmic_host.h"
#include "thunk_types.h"

#define LOG_TAG "CMR_LibcExtra"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

using Jit = Dynarmic::A32::Jit;

char* GuestChars(DynarmicHost& host, uint32_t addr) {
    return reinterpret_cast<char*>(GuestToHost(host.GetGuestMemoryBase(), addr));
}

// Turns a host pointer that lives inside a guest buffer back into a guest address.
uint32_t GuestPtr(uint32_t base, const void* start, const void* found) {
    if (!found) return 0;
    return base + static_cast<uint32_t>(static_cast<const char*>(found) -
                                        static_cast<const char*>(start));
}

uint32_t FloatBits(double d) {
    auto f = static_cast<float>(d);
    uint32_t bits;
    memcpy(&bits, &f, sizeof(bits));
    return bits;
}

void SetDouble(Jit& jit, double d) {
    uint64_t bits;
    memcpy(&bits, &d, sizeof(bits));
    jit.Regs()[0] = static_cast<uint32_t>(bits);
    jit.Regs()[1] = static_cast<uint32_t>(bits >> 32);
}

double GetDouble(Jit& jit, int reg) {
    // Doubles arrive 8-byte aligned, so they always start on an even register.
    if (reg & 1) ++reg;
    uint64_t bits = static_cast<uint64_t>(jit.Regs()[reg]) |
                    (static_cast<uint64_t>(jit.Regs()[reg + 1]) << 32);
    double d;
    memcpy(&d, &bits, sizeof(d));
    return d;
}

// DIR* and the strtok cursor are per-call state the guest holds as opaque values.
std::mutex g_dir_mutex;
// The dirent readdir hands back belongs to the DIR and is overwritten by the next
// call, so it is allocated with the DIR rather than per entry - a block per entry
// is a block the guest never frees, and Mono walks its assembly directories.
struct GuestDir {
    DIR* dir = nullptr;
    uint32_t entry = 0;
};
std::unordered_map<uint32_t, GuestDir> g_dirs;
uint32_t g_next_dir = 0x11110000;

}  // namespace

void ThunkManager::RegisterExtraLibcThunks() {
    // --- ctype -------------------------------------------------------------
    auto ctype_fn = [this](const char* name, int (*fn)(int)) {
        RegisterThunk(name, [fn](Jit& jit, DynarmicHost&) {
            jit.Regs()[0] = static_cast<uint32_t>(fn(static_cast<int>(jit.Regs()[0])));
        });
    };
    ctype_fn("isalnum", &isalnum);
    ctype_fn("isalpha", &isalpha);
    ctype_fn("isspace", &isspace);
    ctype_fn("isupper", &isupper);
    ctype_fn("islower", &islower);
    ctype_fn("isdigit", &isdigit);
    ctype_fn("isxdigit", &isxdigit);
    ctype_fn("ispunct", &ispunct);
    ctype_fn("iscntrl", &iscntrl);
    ctype_fn("isprint", &isprint);
    ctype_fn("isgraph", &isgraph);
    ctype_fn("tolower", &tolower);
    ctype_fn("toupper", &toupper);

    RegisterThunk("towlower", [](Jit& jit, DynarmicHost&) {
        jit.Regs()[0] = static_cast<uint32_t>(towlower(static_cast<wint_t>(jit.Regs()[0])));
    });
    RegisterThunk("towupper", [](Jit& jit, DynarmicHost&) {
        jit.Regs()[0] = static_cast<uint32_t>(towupper(static_cast<wint_t>(jit.Regs()[0])));
    });
    RegisterThunk("iswctype", [](Jit& jit, DynarmicHost&) {
        jit.Regs()[0] = static_cast<uint32_t>(
            iswctype(static_cast<wint_t>(jit.Regs()[0]), static_cast<wctype_t>(jit.Regs()[1])));
    });

    // --- strings that return pointers ---------------------------------------
    RegisterThunk("strpbrk", [](Jit& jit, DynarmicHost& host) {
        uint32_t base = jit.Regs()[0];
        const char* s = GuestChars(host, base);
        const char* accept = GuestChars(host, jit.Regs()[1]);
        jit.Regs()[0] = (s && accept) ? GuestPtr(base, s, strpbrk(s, accept)) : 0;
    });
    RegisterThunk("strcasestr", [](Jit& jit, DynarmicHost& host) {
        uint32_t base = jit.Regs()[0];
        const char* hay = GuestChars(host, base);
        const char* needle = GuestChars(host, jit.Regs()[1]);
        jit.Regs()[0] = (hay && needle) ? GuestPtr(base, hay, strcasestr(hay, needle)) : 0;
    });
    // strtok keeps its cursor in a static; the guest address of the last string is
    // what has to be reconstructed, so the reentrant form does the work.
    RegisterThunk("strtok_r", [](Jit& jit, DynarmicHost& host) {
        uint32_t str_addr = jit.Regs()[0];
        char* str = str_addr ? GuestChars(host, str_addr) : nullptr;
        const char* delim = GuestChars(host, jit.Regs()[1]);
        uint32_t save_addr = jit.Regs()[2];
        if (!delim || !save_addr) { jit.Regs()[0] = 0; return; }

        uint32_t cursor = str_addr ? str_addr : host.MemoryRead32(save_addr);
        char* base = cursor ? GuestChars(host, cursor) : nullptr;
        if (!base) { jit.Regs()[0] = 0; return; }

        // Skip leading delimiters, then terminate the token in place.
        size_t skip = strspn(base, delim);
        base += skip;
        cursor += static_cast<uint32_t>(skip);
        if (!*base) {
            host.MemoryWrite32(save_addr, 0);
            jit.Regs()[0] = 0;
            return;
        }
        size_t len = strcspn(base, delim);
        if (base[len]) {
            base[len] = '\0';
            host.MemoryWrite32(save_addr, cursor + static_cast<uint32_t>(len) + 1);
        } else {
            host.MemoryWrite32(save_addr, 0);
        }
        (void)str;
        jit.Regs()[0] = cursor;
    });
    RegisterThunk("strsep", [](Jit& jit, DynarmicHost& host) {
        uint32_t holder = jit.Regs()[0];
        const char* delim = GuestChars(host, jit.Regs()[1]);
        if (!holder || !delim) { jit.Regs()[0] = 0; return; }
        uint32_t cursor = host.MemoryRead32(holder);
        char* s = cursor ? GuestChars(host, cursor) : nullptr;
        if (!s) { jit.Regs()[0] = 0; return; }
        size_t len = strcspn(s, delim);
        if (s[len]) {
            s[len] = '\0';
            host.MemoryWrite32(holder, cursor + static_cast<uint32_t>(len) + 1);
        } else {
            host.MemoryWrite32(holder, 0);
        }
        jit.Regs()[0] = cursor;
    });

    // --- strings that return counts or copy ---------------------------------
    RegisterThunk("strncat", [](Jit& jit, DynarmicHost& host) {
        char* dst = GuestChars(host, jit.Regs()[0]);
        const char* src = GuestChars(host, jit.Regs()[1]);
        if (dst && src) strncat(dst, src, jit.Regs()[2]);
        // r0 already holds dst, which is the return value.
    });
    RegisterThunk("strlcpy", [](Jit& jit, DynarmicHost& host) {
        char* dst = GuestChars(host, jit.Regs()[0]);
        const char* src = GuestChars(host, jit.Regs()[1]);
        size_t size = jit.Regs()[2];
        if (!src) { jit.Regs()[0] = 0; return; }
        size_t len = strlen(src);
        if (dst && size) {
            size_t n = len < size - 1 ? len : size - 1;
            memcpy(dst, src, n);
            dst[n] = '\0';
        }
        jit.Regs()[0] = static_cast<uint32_t>(len);
    });
    RegisterThunk("strcoll", [](Jit& jit, DynarmicHost& host) {
        const char* a = GuestChars(host, jit.Regs()[0]);
        const char* b = GuestChars(host, jit.Regs()[1]);
        jit.Regs()[0] = static_cast<uint32_t>((a && b) ? strcoll(a, b) : 0);
    });
    RegisterThunk("strxfrm", [](Jit& jit, DynarmicHost& host) {
        char* dst = GuestChars(host, jit.Regs()[0]);
        const char* src = GuestChars(host, jit.Regs()[1]);
        jit.Regs()[0] = static_cast<uint32_t>(src ? strxfrm(dst, src, jit.Regs()[2]) : 0);
    });
    RegisterThunk("atol", [](Jit& jit, DynarmicHost& host) {
        const char* s = GuestChars(host, jit.Regs()[0]);
        jit.Regs()[0] = static_cast<uint32_t>(s ? atol(s) : 0);
    });
    RegisterThunk("strtoull", [](Jit& jit, DynarmicHost& host) {
        const char* s = GuestChars(host, jit.Regs()[0]);
        uint32_t end_out = jit.Regs()[1];
        char* end = nullptr;
        unsigned long long v = s ? strtoull(s, &end, static_cast<int>(jit.Regs()[2])) : 0;
        if (end_out && s) host.MemoryWrite32(end_out, GuestPtr(jit.Regs()[0], s, end));
        jit.Regs()[0] = static_cast<uint32_t>(v);
        jit.Regs()[1] = static_cast<uint32_t>(v >> 32);
    });
    RegisterThunk("strtod", [](Jit& jit, DynarmicHost& host) {
        uint32_t base = jit.Regs()[0];
        const char* s = GuestChars(host, base);
        uint32_t end_out = jit.Regs()[1];
        char* end = nullptr;
        double v = s ? strtod(s, &end) : 0.0;
        if (end_out && s) host.MemoryWrite32(end_out, GuestPtr(base, s, end));
        SetDouble(jit, v);
    });

    // --- wide characters ----------------------------------------------------
    RegisterThunk("wcslen", [](Jit& jit, DynarmicHost& host) {
        auto* s = reinterpret_cast<const wchar_t*>(GuestChars(host, jit.Regs()[0]));
        // The guest's wchar_t is 4 bytes, same as the host's on Android.
        jit.Regs()[0] = static_cast<uint32_t>(s ? wcslen(s) : 0);
    });
    RegisterThunk("wmemcpy", [](Jit& jit, DynarmicHost& host) {
        void* d = GuestChars(host, jit.Regs()[0]);
        const void* s = GuestChars(host, jit.Regs()[1]);
        if (d && s) memcpy(d, s, jit.Regs()[2] * sizeof(wchar_t));
    });
    RegisterThunk("wmemmove", [](Jit& jit, DynarmicHost& host) {
        void* d = GuestChars(host, jit.Regs()[0]);
        const void* s = GuestChars(host, jit.Regs()[1]);
        if (d && s) memmove(d, s, jit.Regs()[2] * sizeof(wchar_t));
    });
    RegisterThunk("wmemset", [](Jit& jit, DynarmicHost& host) {
        auto* d = reinterpret_cast<wchar_t*>(GuestChars(host, jit.Regs()[0]));
        if (d) wmemset(d, static_cast<wchar_t>(jit.Regs()[1]), jit.Regs()[2]);
    });
    RegisterThunk("wmemcmp", [](Jit& jit, DynarmicHost& host) {
        auto* a = reinterpret_cast<const wchar_t*>(GuestChars(host, jit.Regs()[0]));
        auto* b = reinterpret_cast<const wchar_t*>(GuestChars(host, jit.Regs()[1]));
        jit.Regs()[0] = static_cast<uint32_t>((a && b) ? wmemcmp(a, b, jit.Regs()[2]) : 0);
    });
    RegisterThunk("wmemchr", [](Jit& jit, DynarmicHost& host) {
        uint32_t base = jit.Regs()[0];
        auto* s = reinterpret_cast<const wchar_t*>(GuestChars(host, base));
        if (!s) { jit.Regs()[0] = 0; return; }
        const wchar_t* found = wmemchr(s, static_cast<wchar_t>(jit.Regs()[1]), jit.Regs()[2]);
        jit.Regs()[0] = GuestPtr(base, s, found);
    });
    RegisterThunk("wcscoll", [](Jit& jit, DynarmicHost& host) {
        auto* a = reinterpret_cast<const wchar_t*>(GuestChars(host, jit.Regs()[0]));
        auto* b = reinterpret_cast<const wchar_t*>(GuestChars(host, jit.Regs()[1]));
        jit.Regs()[0] = static_cast<uint32_t>((a && b) ? wcscoll(a, b) : 0);
    });
    RegisterThunk("wcstombs", [](Jit& jit, DynarmicHost& host) {
        char* d = GuestChars(host, jit.Regs()[0]);
        auto* s = reinterpret_cast<const wchar_t*>(GuestChars(host, jit.Regs()[1]));
        jit.Regs()[0] = static_cast<uint32_t>(s ? wcstombs(d, s, jit.Regs()[2]) : 0);
    });
    RegisterThunk("wcrtomb", [](Jit& jit, DynarmicHost& host) {
        char* d = GuestChars(host, jit.Regs()[0]);
        mbstate_t state {};
        jit.Regs()[0] = static_cast<uint32_t>(wcrtomb(d, static_cast<wchar_t>(jit.Regs()[1]), &state));
    });
    RegisterThunk("mbrtowc", [](Jit& jit, DynarmicHost& host) {
        auto* d = reinterpret_cast<wchar_t*>(GuestChars(host, jit.Regs()[0]));
        const char* s = GuestChars(host, jit.Regs()[1]);
        mbstate_t state {};
        jit.Regs()[0] = static_cast<uint32_t>(s ? mbrtowc(d, s, jit.Regs()[2], &state) : 0);
    });

    // --- math ---------------------------------------------------------------
    auto double_fn = [this](const char* name, double (*fn)(double)) {
        RegisterThunk(name, [fn](Jit& jit, DynarmicHost&) {
            SetDouble(jit, fn(GetDouble(jit, 0)));
        });
    };
    double_fn("asin", &asin);
    double_fn("acos", &acos);
    double_fn("atan", &atan);
    double_fn("ceil", &ceil);
    double_fn("cosh", &cosh);
    double_fn("sinh", &sinh);
    double_fn("tanh", &tanh);
    double_fn("round", &round);
    double_fn("trunc", &trunc);
    RegisterThunk("fmod", [](Jit& jit, DynarmicHost&) {
        SetDouble(jit, fmod(GetDouble(jit, 0), GetDouble(jit, 2)));
    });
    RegisterThunk("modf", [](Jit& jit, DynarmicHost& host) {
        double integral = 0.0;
        double frac = modf(GetDouble(jit, 0), &integral);
        uint32_t out = jit.Regs()[2];
        if (out) {
            uint64_t bits;
            memcpy(&bits, &integral, sizeof(bits));
            host.MemoryWrite64(out, bits);
        }
        SetDouble(jit, frac);
    });
    RegisterThunk("isnan", [](Jit& jit, DynarmicHost&) {
        jit.Regs()[0] = std::isnan(GetDouble(jit, 0)) ? 1 : 0;
    });
    RegisterThunk("__fpclassifyf", [](Jit& jit, DynarmicHost&) {
        float f;
        uint32_t bits = jit.Regs()[0];
        memcpy(&f, &bits, sizeof(f));
        jit.Regs()[0] = static_cast<uint32_t>(std::fpclassify(f));
    });
    RegisterThunk("div", [](Jit& jit, DynarmicHost&) {
        auto num = static_cast<int32_t>(jit.Regs()[0]);
        auto den = static_cast<int32_t>(jit.Regs()[1]);
        div_t r = den ? div(num, den) : div_t{0, 0};
        jit.Regs()[0] = static_cast<uint32_t>(r.quot);
        jit.Regs()[1] = static_cast<uint32_t>(r.rem);
    });

    // --- directories --------------------------------------------------------
    RegisterThunk("opendir", [](Jit& jit, DynarmicHost& host) {
        const char* path = GuestChars(host, jit.Regs()[0]);
        DIR* dir = path ? opendir(path) : nullptr;
        if (!dir) { jit.Regs()[0] = 0; return; }
        std::lock_guard<std::mutex> lock(g_dir_mutex);
        uint32_t handle = g_next_dir++;
        g_dirs[handle] = GuestDir{dir, 0};
        jit.Regs()[0] = handle;
    });
    // struct dirent as the 32-bit guest sees it: ino(8) off(8) reclen(2) type(1)
    // name[256]. The host's layout matches field for field on Android.
    RegisterThunk("readdir", [](Jit& jit, DynarmicHost& host) {
        constexpr uint32_t kGuestDirentSize = 8 + 8 + 2 + 1 + 256;
        DIR* dir = nullptr;
        uint32_t out = 0;
        {
            std::lock_guard<std::mutex> lock(g_dir_mutex);
            auto it = g_dirs.find(jit.Regs()[0]);
            if (it == g_dirs.end()) { jit.Regs()[0] = 0; return; }
            dir = it->second.dir;
            if (!it->second.entry) it->second.entry = host.GuestAlloc(kGuestDirentSize, 8);
            out = it->second.entry;
        }
        struct dirent* entry = dir ? readdir(dir) : nullptr;
        if (!entry || !out) { jit.Regs()[0] = 0; return; }

        uint8_t* mem = host.GetGuestMemoryBase() + out;
        memset(mem, 0, kGuestDirentSize);
        memcpy(mem, &entry->d_ino, 8);
        memcpy(mem + 8, &entry->d_off, 8);
        uint16_t reclen = kGuestDirentSize;
        memcpy(mem + 16, &reclen, 2);
        mem[18] = entry->d_type;
        strncpy(reinterpret_cast<char*>(mem + 19), entry->d_name, 255);
        jit.Regs()[0] = out;
    });
    RegisterThunk("closedir", [](Jit& jit, DynarmicHost& host) {
        std::lock_guard<std::mutex> lock(g_dir_mutex);
        auto it = g_dirs.find(jit.Regs()[0]);
        if (it != g_dirs.end()) {
            closedir(it->second.dir);
            host.GuestFree(it->second.entry);
            g_dirs.erase(it);
        }
        jit.Regs()[0] = 0;
    });
    RegisterThunk("mkdir", [](Jit& jit, DynarmicHost& host) {
        const char* path = GuestChars(host, jit.Regs()[0]);
        jit.Regs()[0] = static_cast<uint32_t>(path ? mkdir(path, jit.Regs()[1]) : -1);
    });
    RegisterThunk("rmdir", [](Jit& jit, DynarmicHost& host) {
        const char* path = GuestChars(host, jit.Regs()[0]);
        jit.Regs()[0] = static_cast<uint32_t>(path ? rmdir(path) : -1);
    });
    RegisterThunk("chdir", [](Jit& jit, DynarmicHost& host) {
        const char* path = GuestChars(host, jit.Regs()[0]);
        jit.Regs()[0] = static_cast<uint32_t>(path ? chdir(path) : -1);
    });

    // --- process and misc ---------------------------------------------------
    RegisterThunk("getuid", [](Jit& jit, DynarmicHost&) { jit.Regs()[0] = getuid(); });
    RegisterThunk("geteuid", [](Jit& jit, DynarmicHost&) { jit.Regs()[0] = geteuid(); });
    RegisterThunk("getpid", [](Jit& jit, DynarmicHost&) { jit.Regs()[0] = static_cast<uint32_t>(getpid()); });
    RegisterThunk("isatty", [](Jit& jit, DynarmicHost&) { jit.Regs()[0] = 0; });
    RegisterThunk("sleep", [](Jit& jit, DynarmicHost&) {
        jit.Regs()[0] = sleep(jit.Regs()[0]);
    });
    RegisterThunk("dup2", [](Jit& jit, DynarmicHost&) {
        jit.Regs()[0] = static_cast<uint32_t>(dup2(static_cast<int>(jit.Regs()[0]),
                                                   static_cast<int>(jit.Regs()[1])));
    });
    RegisterThunk("pipe", [](Jit& jit, DynarmicHost& host) {
        int fds[2] = {-1, -1};
        int r = pipe(fds);
        if (r == 0 && jit.Regs()[0]) {
            host.MemoryWrite32(jit.Regs()[0], static_cast<uint32_t>(fds[0]));
            host.MemoryWrite32(jit.Regs()[0] + 4, static_cast<uint32_t>(fds[1]));
        }
        jit.Regs()[0] = static_cast<uint32_t>(r);
    });
    RegisterThunk("fsync", [](Jit& jit, DynarmicHost&) {
        jit.Regs()[0] = static_cast<uint32_t>(fsync(static_cast<int>(jit.Regs()[0])));
    });
    RegisterThunk("ftruncate", [](Jit& jit, DynarmicHost&) {
        jit.Regs()[0] = static_cast<uint32_t>(
            ftruncate(static_cast<int>(jit.Regs()[0]), static_cast<off_t>(jit.Regs()[1])));
    });
    RegisterThunk("getdtablesize", [](Jit& jit, DynarmicHost&) { jit.Regs()[0] = 1024; });
    RegisterThunk("uname", [](Jit& jit, DynarmicHost& host) {
        // struct utsname is six 65-byte fields on both sides.
        uint32_t out = jit.Regs()[0];
        struct utsname info {};
        int r = uname(&info);
        if (r == 0 && out) {
            uint8_t* mem = host.GetGuestMemoryBase() + out;
            memset(mem, 0, 65 * 6);
            const char* fields[] = {info.sysname, info.nodename, info.release,
                                    info.version, info.machine, info.domainname};
            for (int i = 0; i < 6; ++i) strncpy(reinterpret_cast<char*>(mem + i * 65), fields[i], 64);
        }
        jit.Regs()[0] = static_cast<uint32_t>(r);
    });
    RegisterThunk("setlocale", [](Jit& jit, DynarmicHost& host) {
        // Only "C" exists here; handing back a stable string is enough for the
        // callers, which just check for non-NULL.
        static uint32_t c_locale = 0;
        if (!c_locale) {
            c_locale = host.GuestAlloc(2, 1);
            memcpy(host.GetGuestMemoryBase() + c_locale, "C", 2);
        }
        jit.Regs()[0] = c_locale;
    });
    RegisterThunk("clock", [](Jit& jit, DynarmicHost&) {
        jit.Regs()[0] = static_cast<uint32_t>(clock());
    });
    RegisterThunk("strftime", [](Jit& jit, DynarmicHost& host) {
        // The guest struct tm matches the host's field order; a straight cast works
        // because every member is a 32-bit int on both sides.
        char* out = GuestChars(host, jit.Regs()[0]);
        const char* fmt = GuestChars(host, jit.Regs()[2]);
        auto* tm_ptr = reinterpret_cast<struct tm*>(GuestChars(host, jit.Regs()[3]));
        jit.Regs()[0] = static_cast<uint32_t>(
            (out && fmt && tm_ptr) ? strftime(out, jit.Regs()[1], fmt, tm_ptr) : 0);
    });

    // --- stdio --------------------------------------------------------------
    RegisterThunk("putchar", [](Jit& jit, DynarmicHost&) {
        jit.Regs()[0] = jit.Regs()[0];  // swallowed; the guest has no console
    });
    RegisterThunk("perror", [](Jit& jit, DynarmicHost& host) {
        const char* s = GuestChars(host, jit.Regs()[0]);
        LOGE("[guest] %s: %s", s ? s : "", strerror(errno));
        jit.Regs()[0] = 0;
    });
    RegisterThunk("__android_log_vprint", [](Jit& jit, DynarmicHost& host) {
        const char* tag = GuestChars(host, jit.Regs()[1]);
        const char* fmt = GuestChars(host, jit.Regs()[2]);
        std::string text = FormatGuestVaList(jit, host, fmt, jit.Regs()[3]);
        if (!GuestLogAllowed(static_cast<int>(jit.Regs()[0]), tag)) { jit.Regs()[0] = 1; return; }
        __android_log_write(static_cast<int>(jit.Regs()[0]), tag ? tag : "guest", text.c_str());
        jit.Regs()[0] = 1;
    });
    RegisterThunk("__android_log_assert", [](Jit& jit, DynarmicHost& host) {
        const char* fmt = GuestChars(host, jit.Regs()[2]);
        LOGE("guest assertion failed: %s", fmt ? fmt : "(null)");
        jit.Regs()[0] = 0;
    });

    // errno has to be per-thread; a shared cell makes one thread's failure look
    // like every other thread's.
    RegisterThunk("__errno", [](Jit& jit, DynarmicHost& host) {
        const int saved_errno = errno;
        static thread_local uint32_t slot = 0;
        if (!slot) slot = host.GuestAlloc(4, 4);
        if (slot) host.MemoryWrite32(slot, static_cast<uint32_t>(saved_errno));
        errno = saved_errno;
        jit.Regs()[0] = slot;
    });
    RegisterThunk("__get_h_errno", [](Jit& jit, DynarmicHost& host) {
        const int saved_h_errno = h_errno;
        static thread_local uint32_t slot = 0;
        if (!slot) slot = host.GuestAlloc(4, 4);
        if (slot) host.MemoryWrite32(slot, static_cast<uint32_t>(saved_h_errno));
        h_errno = saved_h_errno;
        jit.Regs()[0] = slot;
    });

    // --- pthread odds and ends ---------------------------------------------
    auto pthread_ok = [](Jit& jit, DynarmicHost&) { jit.Regs()[0] = 0; };
    for (const char* name : {"pthread_condattr_init", "pthread_condattr_destroy",
                             "pthread_attr_setschedparam", "pthread_attr_setschedpolicy",
                             "pthread_attr_getschedparam", "pthread_attr_getdetachstate",
                             "pthread_setschedparam", "pthread_getschedparam",
                             "pthread_condattr_setclock", "pthread_rwlock_init",
                             "pthread_rwlock_destroy"}) {
        RegisterThunk(name, pthread_ok);
    }
    RegisterThunk("sched_get_priority_max", [](Jit& jit, DynarmicHost&) { jit.Regs()[0] = 99; });
    RegisterThunk("sched_get_priority_min", [](Jit& jit, DynarmicHost&) { jit.Regs()[0] = 1; });
    RegisterThunk("setpriority", pthread_ok);
    RegisterThunk("getpriority", [](Jit& jit, DynarmicHost&) { jit.Regs()[0] = 0; });
    // pthread_exit unwinds the guest thread; returning to the exit trap ends the
    // RunFunction that started it, which is the same thing from the host's side.
    RegisterThunk("pthread_exit", [](Jit& jit, DynarmicHost&) {
        GuestJumpToExitTrap(jit);
    });
    // strtok keeps its cursor in a static, which here has to be a guest address so
    // the returned token pointer stays meaningful to the guest.
    RegisterThunk("strtok", [](Jit& jit, DynarmicHost& host) {
        static thread_local uint32_t cursor = 0;
        uint32_t str_addr = jit.Regs()[0];
        const char* delim = GuestChars(host, jit.Regs()[1]);
        if (!delim) { jit.Regs()[0] = 0; return; }
        if (str_addr) cursor = str_addr;
        if (!cursor) { jit.Regs()[0] = 0; return; }

        char* base = GuestChars(host, cursor);
        size_t skip = strspn(base, delim);
        base += skip;
        cursor += static_cast<uint32_t>(skip);
        if (!*base) { cursor = 0; jit.Regs()[0] = 0; return; }
        size_t len = strcspn(base, delim);
        uint32_t token = cursor;
        if (base[len]) {
            base[len] = '\0';
            cursor += static_cast<uint32_t>(len) + 1;
        } else {
            cursor = 0;
        }
        jit.Regs()[0] = token;
    });

    // Mono canonicalises every assembly path through lstat before it opens
    // anything, so leaving the caller's buffer untouched made it read whatever
    // was on the stack. Entries inside the APK are never symlinks, which is why
    // the fallback can answer the same as stat.
    RegisterThunk("lstat", [](Jit& jit, DynarmicHost& host) {
        const char* path = GuestChars(host, jit.Regs()[0]);
        struct stat st {};
        int r = path ? lstat(path, &st) : -1;
        if (r != 0 && path) {
            off_t size = 0;
            bool is_dir = false;
            if (GameDataStatEntry(path, &size, &is_dir) ||
                ApkStatEntry(path, &size, &is_dir)) {
                memset(&st, 0, sizeof(st));
                st.st_size = size;
                st.st_mode = (is_dir ? (S_IFDIR | 0755) : (S_IFREG | 0644));
                st.st_nlink = 1;
                r = 0;
            }
        }
        if (r == 0) WriteArm32Stat(host.GetGuestMemoryBase(), jit.Regs()[1], st);
        jit.Regs()[0] = static_cast<uint32_t>(r);
    });

    // bsearch(key, base, nmemb, size, compar) - the fifth argument is on the
    // stack. Mono reaches most of its metadata through this: which type owns a
    // field, which methods a type nests, where a custom attribute lives. The
    // stub answered "not found" to all of it.
    //
    // The comparator is guest code called from inside a thunk, which is what
    // CallGuestNested exists for.
    RegisterThunk("bsearch", [](Jit& jit, DynarmicHost& host) {
        uint32_t key = jit.Regs()[0];
        uint32_t base = jit.Regs()[1];
        uint32_t nmemb = jit.Regs()[2];
        uint32_t size = jit.Regs()[3];
        uint32_t compar = host.MemoryRead32(jit.Regs()[13]);
        if (!base || !size || !compar) { jit.Regs()[0] = 0; return; }

        uint32_t lo = 0, hi = nmemb;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            uint32_t elem = base + mid * size;
            int32_t r = static_cast<int32_t>(host.CallGuestNested(compar, {key, elem}));
            if (r == 0) { jit.Regs()[0] = elem; return; }
            if (r < 0) hi = mid; else lo = mid + 1;
        }
        jit.Regs()[0] = 0;
    });

    // qsort(base, nmemb, size, compar). It used to be registered as an empty lambda,
    // which is not "unsorted" but "silently left in insertion order" - and Mono sorts
    // the entries of an IMT slot with it before building the thunk that dispatches
    // interface calls. That thunk is a binary search: on an unsorted list an entry
    // sits in the wrong half, no comparison ever matches it, and the call lands on
    // whatever the thunk takes by default. Which is how `item.gameObject` arrived at
    // a different method entirely once a slot held more than one method.
    //
    // The comparator is guest code, so the sort runs on a snapshot: the elements are
    // copied out, the order is decided by calling the guest with the addresses of the
    // *original* elements (which nothing moves while this runs), and the snapshot is
    // written back in that order. Sorting in place instead would move elements under
    // the comparator's feet.
    RegisterThunk("qsort", [](Jit& jit, DynarmicHost& host) {
        uint32_t base = jit.Regs()[0];
        uint32_t nmemb = jit.Regs()[1];
        uint32_t size = jit.Regs()[2];
        uint32_t compar = jit.Regs()[3];
        if (!base || !compar || !size || nmemb < 2) return;
        // A guard against a bad size/count pair turning into a gigabyte of copying.
        if (static_cast<uint64_t>(nmemb) * size > (64u << 20)) {
            LOGE("qsort(%u elements of %u bytes) is too large to be real", nmemb, size);
            return;
        }

        uint8_t* mem = host.GetGuestMemoryBase();
        std::vector<uint8_t> snapshot(mem + base, mem + base + static_cast<size_t>(nmemb) * size);
        std::vector<uint32_t> order(nmemb);
        for (uint32_t i = 0; i < nmemb; ++i) order[i] = i;

        std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
            int32_t r = static_cast<int32_t>(
                host.CallGuestNested(compar, {base + a * size, base + b * size}));
            return r < 0;
        });

        for (uint32_t i = 0; i < nmemb; ++i)
            memcpy(mem + base + static_cast<size_t>(i) * size,
                   snapshot.data() + static_cast<size_t>(order[i]) * size, size);
    });

    RegisterThunk("__swbuf", [](Jit& jit, DynarmicHost&) { jit.Regs()[0] = jit.Regs()[0]; });
    RegisterThunk("unsetenv", [](Jit& jit, DynarmicHost&) { jit.Regs()[0] = 0; });
    RegisterThunk("writev", [](Jit& jit, DynarmicHost& host) {
        // Each guest iovec is two words; the payload stays in guest memory, so the
        // host iovecs just point into the arena.
        int fd = static_cast<int>(jit.Regs()[0]);
        uint32_t vec = jit.Regs()[1];
        int count = static_cast<int>(jit.Regs()[2]);
        if (count <= 0 || count > 64 || !vec) { jit.Regs()[0] = 0; return; }
        struct iovec host_vec[64];
        for (int i = 0; i < count; ++i) {
            host_vec[i].iov_base = host.GetGuestMemoryBase() + host.MemoryRead32(vec + i * 8);
            host_vec[i].iov_len = host.MemoryRead32(vec + i * 8 + 4);
        }
        jit.Regs()[0] = static_cast<uint32_t>(writev(fd, host_vec, count));
    });
    RegisterThunk("mremap", [](Jit& jit, DynarmicHost&) { jit.Regs()[0] = 0xFFFFFFFF; });
    RegisterThunk("syscall", [](Jit& jit, DynarmicHost&) {
        LOGE("guest called syscall(%u) directly - not bridged", jit.Regs()[0]);
        jit.Regs()[0] = 0xFFFFFFFF;
    });
    // Signals are not delivered into the guest, so a kill aimed at a guest thread
    // would have nowhere to land; reporting success keeps Boehm's stop-the-world
    // handshake from spinning.
    for (const char* name : {"kill", "setitimer"}) {
        RegisterThunk(name, [](Jit& jit, DynarmicHost&) { jit.Regs()[0] = 0; });
    }

    // Boehm's GC_stop_world is a tkill to every collector thread followed by a
    // wait for one acknowledgement each, posted from the signal handler. As a
    // no-op returning success this deadlocked the first collection: the count was
    // right and no acknowledgement ever came.
    auto guest_kill = [](Jit& jit, DynarmicHost&) {
        jit.Regs()[0] = static_cast<uint32_t>(
            GuestSignalSend(static_cast<int>(jit.Regs()[0]), static_cast<int>(jit.Regs()[1])));
    };
    RegisterThunk("tkill", guest_kill);
    RegisterThunk("tgkill", [](Jit& jit, DynarmicHost&) {
        jit.Regs()[0] = static_cast<uint32_t>(
            GuestSignalSend(static_cast<int>(jit.Regs()[1]), static_cast<int>(jit.Regs()[2])));
    });
    // pthread_kill takes a pthread_t, which in this bridge is the id pthread_create
    // handed the creator, not the kernel tid the signal table is keyed by.
    RegisterThunk("pthread_kill", [](Jit& jit, DynarmicHost&) {
        uint32_t guest_tid = jit.Regs()[0];
        int kernel = GuestKernelTidLookup ? GuestKernelTidLookup(guest_tid) : 0;
        int result = GuestSignalSend(kernel ? kernel : static_cast<int>(guest_tid),
                                     static_cast<int>(jit.Regs()[1]));
        if (result != 0) {
            static std::atomic<int> complained{0};
            if (complained.fetch_add(1) < 8) {
                LOGE("pthread_kill(0x%X -> tid %d, %u) failed with %d", guest_tid, kernel,
                     jit.Regs()[1], result);
            }
        }
        jit.Regs()[0] = static_cast<uint32_t>(result);
    });

    // The suspend handler parks here until the restart signal arrives.
    RegisterThunk("sigsuspend", [](Jit& jit, DynarmicHost& host) {
        GuestSignalWait(host);
        jit.Regs()[0] = 0xFFFFFFFF;  // sigsuspend always "fails" with EINTR
    });
    RegisterThunk("sem_timedwait", [](Jit& jit, DynarmicHost&) { jit.Regs()[0] = 0; });

    LOGI("extra libc thunks registered");
}
