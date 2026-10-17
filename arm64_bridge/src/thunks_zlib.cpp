// zlib for the guest.
//
// This one cannot be forwarded argument-for-argument like the rest of libc:
// z_stream is a structure the caller owns and both sides write to, and its
// 32-bit layout is 56 bytes where the host's is 112. Handing the guest's
// address straight to the host zlib would have it parse next_out as avail_in.
//
// So the guest keeps its own 56-byte z_stream and the host keeps a real one
// beside it, keyed by the guest address. Every call copies the six fields that
// describe the current window in, runs the host implementation, and copies the
// results back. The internal state pointer the guest sees is never dereferenced
// by anyone - it only has to be non-zero, because callers test it.
//
// The allocator hooks are deliberately ignored. They allocate for the internal
// state, and that state lives on the host side, so host malloc is the correct
// allocator for it - calling back into the guest would put host structures in
// the guest arena for no reason.

#include "thunk_manager.h"
#include "dynarmic_host.h"
#include "thunk_types.h"

#include <zlib.h>
#include <android/log.h>

#include <cstring>
#include <map>
#include <mutex>

#define LOG_TAG "CMR_Zlib"
#define LOGI(...) ((void)0)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

using Dynarmic::A32::Jit;

namespace {

// Offsets into the guest's 56-byte z_stream.
constexpr uint32_t kNextIn   = 0;
constexpr uint32_t kAvailIn  = 4;
constexpr uint32_t kTotalIn  = 8;
constexpr uint32_t kNextOut  = 12;
constexpr uint32_t kAvailOut = 16;
constexpr uint32_t kTotalOut = 20;
constexpr uint32_t kMsg      = 24;
constexpr uint32_t kState    = 28;
constexpr uint32_t kDataType = 44;
constexpr uint32_t kAdler    = 48;

std::mutex g_mutex;
std::map<uint32_t, z_stream*> g_streams;  // guest z_streamp -> host stream

z_stream* Find(uint32_t guest_stream) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_streams.find(guest_stream);
    return it == g_streams.end() ? nullptr : it->second;
}

z_stream* Create(uint32_t guest_stream) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto& slot = g_streams[guest_stream];
    if (!slot) slot = new z_stream();
    memset(slot, 0, sizeof(z_stream));
    return slot;
}

void Destroy(uint32_t guest_stream) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_streams.find(guest_stream);
    if (it == g_streams.end()) return;
    delete it->second;
    g_streams.erase(it);
}

// Guest pointers are offsets into the arena; a null one has to stay null,
// otherwise zlib would happily read from the arena's base address.
Bytef* Window(DynarmicHost& host, uint32_t gaddr) {
    return gaddr ? reinterpret_cast<Bytef*>(host.GetGuestMemoryBase() + gaddr) : nullptr;
}

void PullFromGuest(DynarmicHost& host, uint32_t gs, z_stream* zs) {
    zs->next_in   = Window(host, host.MemoryRead32(gs + kNextIn));
    zs->avail_in  = host.MemoryRead32(gs + kAvailIn);
    zs->next_out  = Window(host, host.MemoryRead32(gs + kNextOut));
    zs->avail_out = host.MemoryRead32(gs + kAvailOut);
    zs->total_in  = host.MemoryRead32(gs + kTotalIn);
    zs->total_out = host.MemoryRead32(gs + kTotalOut);
}

void PushToGuest(DynarmicHost& host, uint32_t gs, z_stream* zs) {
    uint8_t* base = host.GetGuestMemoryBase();
    auto to_guest = [base](const Bytef* p) -> uint32_t {
        return p ? static_cast<uint32_t>(reinterpret_cast<const uint8_t*>(p) - base) : 0;
    };
    host.MemoryWrite32(gs + kNextIn,   to_guest(zs->next_in));
    host.MemoryWrite32(gs + kAvailIn,  zs->avail_in);
    host.MemoryWrite32(gs + kNextOut,  to_guest(zs->next_out));
    host.MemoryWrite32(gs + kAvailOut, zs->avail_out);
    host.MemoryWrite32(gs + kTotalIn,  static_cast<uint32_t>(zs->total_in));
    host.MemoryWrite32(gs + kTotalOut, static_cast<uint32_t>(zs->total_out));
    host.MemoryWrite32(gs + kDataType, static_cast<uint32_t>(zs->data_type));
    host.MemoryWrite32(gs + kAdler,    static_cast<uint32_t>(zs->adler));
    // The guest only ever prints msg, and pointing it at guest-readable memory
    // it does not own would be worse than saying "no message".
    host.MemoryWrite32(gs + kMsg, 0);
}

// A non-null value the guest can test but must never dereference.
constexpr uint32_t kFakeState = 0x5A11B000;

}  // namespace

void ThunkManager::RegisterZlibThunks() {
    auto init_inflate = [](Jit& jit, DynarmicHost& host, int window_bits) {
        uint32_t gs = jit.Regs()[0];
        if (!gs) { jit.Regs()[0] = static_cast<uint32_t>(Z_STREAM_ERROR); return; }
        z_stream* zs = Create(gs);
        PullFromGuest(host, gs, zs);
        zs->total_in = zs->total_out = 0;
        int r = inflateInit2_(zs, window_bits, ZLIB_VERSION, static_cast<int>(sizeof(z_stream)));
        if (r == Z_OK) {
            host.MemoryWrite32(gs + kState, kFakeState);
            PushToGuest(host, gs, zs);
        } else {
            Destroy(gs);
            LOGE("inflateInit2_(windowBits=%d) failed: %d", window_bits, r);
        }
        jit.Regs()[0] = static_cast<uint32_t>(r);
    };

    // inflateInit_(strm, version, stream_size): the guest's stream_size describes
    // its own 56-byte layout and is meaningless here, so it is not forwarded.
    RegisterThunk("inflateInit_", [init_inflate](Jit& jit, DynarmicHost& host) {
        init_inflate(jit, host, MAX_WBITS);
    });
    RegisterThunk("inflateInit2_", [init_inflate](Jit& jit, DynarmicHost& host) {
        init_inflate(jit, host, static_cast<int32_t>(jit.Regs()[1]));
    });

    RegisterThunk("inflate", [](Jit& jit, DynarmicHost& host) {
        uint32_t gs = jit.Regs()[0];
        z_stream* zs = Find(gs);
        if (!zs) { jit.Regs()[0] = static_cast<uint32_t>(Z_STREAM_ERROR); return; }
        PullFromGuest(host, gs, zs);
        int r = inflate(zs, static_cast<int32_t>(jit.Regs()[1]));
        PushToGuest(host, gs, zs);
        jit.Regs()[0] = static_cast<uint32_t>(r);
    });

    RegisterThunk("inflateReset", [](Jit& jit, DynarmicHost& host) {
        uint32_t gs = jit.Regs()[0];
        z_stream* zs = Find(gs);
        if (!zs) { jit.Regs()[0] = static_cast<uint32_t>(Z_STREAM_ERROR); return; }
        int r = inflateReset(zs);
        PushToGuest(host, gs, zs);
        jit.Regs()[0] = static_cast<uint32_t>(r);
    });

    RegisterThunk("inflateEnd", [](Jit& jit, DynarmicHost& host) {
        uint32_t gs = jit.Regs()[0];
        z_stream* zs = Find(gs);
        if (!zs) { jit.Regs()[0] = static_cast<uint32_t>(Z_STREAM_ERROR); return; }
        int r = inflateEnd(zs);
        Destroy(gs);
        host.MemoryWrite32(gs + kState, 0);
        jit.Regs()[0] = static_cast<uint32_t>(r);
    });

    // deflateInit2_(strm, level, method, windowBits, memLevel, strategy, version,
    // stream_size): only four arguments fit in registers, the rest are on the stack.
    RegisterThunk("deflateInit2_", [](Jit& jit, DynarmicHost& host) {
        uint32_t gs = jit.Regs()[0];
        if (!gs) { jit.Regs()[0] = static_cast<uint32_t>(Z_STREAM_ERROR); return; }
        int level       = static_cast<int32_t>(jit.Regs()[1]);
        int method      = static_cast<int32_t>(jit.Regs()[2]);
        int window_bits = static_cast<int32_t>(jit.Regs()[3]);
        int mem_level   = static_cast<int32_t>(host.MemoryRead32(jit.Regs()[13]));
        int strategy    = static_cast<int32_t>(host.MemoryRead32(jit.Regs()[13] + 4));
        z_stream* zs = Create(gs);
        PullFromGuest(host, gs, zs);
        zs->total_in = zs->total_out = 0;
        int r = deflateInit2_(zs, level, method, window_bits, mem_level, strategy,
                              ZLIB_VERSION, static_cast<int>(sizeof(z_stream)));
        if (r == Z_OK) {
            host.MemoryWrite32(gs + kState, kFakeState);
            PushToGuest(host, gs, zs);
        } else {
            Destroy(gs);
            LOGE("deflateInit2_ failed: %d", r);
        }
        jit.Regs()[0] = static_cast<uint32_t>(r);
    });

    RegisterThunk("deflateInit_", [](Jit& jit, DynarmicHost& host) {
        uint32_t gs = jit.Regs()[0];
        if (!gs) { jit.Regs()[0] = static_cast<uint32_t>(Z_STREAM_ERROR); return; }
        z_stream* zs = Create(gs);
        PullFromGuest(host, gs, zs);
        zs->total_in = zs->total_out = 0;
        int r = deflateInit_(zs, static_cast<int32_t>(jit.Regs()[1]), ZLIB_VERSION,
                             static_cast<int>(sizeof(z_stream)));
        if (r == Z_OK) {
            host.MemoryWrite32(gs + kState, kFakeState);
            PushToGuest(host, gs, zs);
        } else {
            Destroy(gs);
        }
        jit.Regs()[0] = static_cast<uint32_t>(r);
    });

    RegisterThunk("deflate", [](Jit& jit, DynarmicHost& host) {
        uint32_t gs = jit.Regs()[0];
        z_stream* zs = Find(gs);
        if (!zs) { jit.Regs()[0] = static_cast<uint32_t>(Z_STREAM_ERROR); return; }
        PullFromGuest(host, gs, zs);
        int r = deflate(zs, static_cast<int32_t>(jit.Regs()[1]));
        PushToGuest(host, gs, zs);
        jit.Regs()[0] = static_cast<uint32_t>(r);
    });

    RegisterThunk("deflateReset", [](Jit& jit, DynarmicHost& host) {
        uint32_t gs = jit.Regs()[0];
        z_stream* zs = Find(gs);
        if (!zs) { jit.Regs()[0] = static_cast<uint32_t>(Z_STREAM_ERROR); return; }
        int r = deflateReset(zs);
        PushToGuest(host, gs, zs);
        jit.Regs()[0] = static_cast<uint32_t>(r);
    });

    RegisterThunk("deflateEnd", [](Jit& jit, DynarmicHost& host) {
        uint32_t gs = jit.Regs()[0];
        z_stream* zs = Find(gs);
        if (!zs) { jit.Regs()[0] = static_cast<uint32_t>(Z_STREAM_ERROR); return; }
        int r = deflateEnd(zs);
        Destroy(gs);
        host.MemoryWrite32(gs + kState, 0);
        jit.Regs()[0] = static_cast<uint32_t>(r);
    });

    RegisterThunk("deflateBound", [](Jit& jit, DynarmicHost& host) {
        z_stream* zs = Find(jit.Regs()[0]);
        uLong len = jit.Regs()[1];
        jit.Regs()[0] = static_cast<uint32_t>(zs ? deflateBound(zs, len) : len + (len >> 9) + 64);
    });

    // uLong is four bytes on the guest, so the running value is truncated on the
    // way back - which is exactly what the guest's own zlib would have stored.
    RegisterThunk("crc32", [](Jit& jit, DynarmicHost& host) {
        const Bytef* buf = jit.Regs()[1]
            ? reinterpret_cast<const Bytef*>(host.GetGuestMemoryBase() + jit.Regs()[1])
            : nullptr;
        jit.Regs()[0] = static_cast<uint32_t>(crc32(jit.Regs()[0], buf, jit.Regs()[2]));
    });

    RegisterThunk("adler32", [](Jit& jit, DynarmicHost& host) {
        const Bytef* buf = jit.Regs()[1]
            ? reinterpret_cast<const Bytef*>(host.GetGuestMemoryBase() + jit.Regs()[1])
            : nullptr;
        jit.Regs()[0] = static_cast<uint32_t>(adler32(jit.Regs()[0], buf, jit.Regs()[2]));
    });

    RegisterThunk("zlibVersion", [](Jit& jit, DynarmicHost& host) {
        static uint32_t cached = 0;
        if (!cached) {
            size_t n = strlen(ZLIB_VERSION) + 1;
            cached = host.GuestAlloc(n, 1);
            if (cached) memcpy(host.GetGuestMemoryBase() + cached, ZLIB_VERSION, n);
        }
        jit.Regs()[0] = cached;
    });

}
