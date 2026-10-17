// CP15 for the guest.
//
// Without a coprocessor attached, Dynarmic compiles any MRC/MCR into an
// assertion and the process dies inside EmitCoprocessorException with the single
// word "terminating". The game reached this after about 120 frames, once enough
// managed code had been JIT-compiled: ARM32 bionic reads the thread pointer with
// `mrc p15, 0, Rt, c13, c0, 3`, and Mono's own generated code does the same for
// its thread-local fast paths.
//
// Two groups matter:
//   c13 - the software thread ID registers. Each guest thread gets a small block
//         in the guest arena; TPIDRURO/TPIDRURW read and write it. The value has
//         to be per-thread, so these go through a callback rather than a fixed
//         pointer.
//   c7  - the cache and barrier operations. On ARMv7 these are spelled as real
//         instructions, but code built for ARMv6 still issues them through CP15,
//         and they must not be an exception either.

#include "guest_cp15.h"

#include <android/log.h>

#include <atomic>
#include <mutex>
#include <set>

#include "dynarmic_host.h"

#define LOG_TAG "CMR_Cp15"
#define LOGI(...) ((void)0)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

using Dynarmic::A32::CoprocReg;

// The guest's own view of its thread pointer. bionic keeps a small array here;
// the block is zeroed, which is what a fresh thread would see.
constexpr uint32_t kGuestTlsBlockSize = 256;
thread_local uint32_t t_tls_block = 0;

DynarmicHost* g_host = nullptr;

// One line per distinct instruction, so an unimplemented register is visible
// without drowning the log.
void LogOnce(const char* op, unsigned num, unsigned opc1, CoprocReg CRn, CoprocReg CRm, unsigned opc2) {
    static std::mutex m;
    static std::set<uint64_t> seen;
    uint64_t key = (static_cast<uint64_t>(op[0]) << 40) ^ (static_cast<uint64_t>(num) << 32) ^
                   (opc1 << 24) ^ (static_cast<unsigned>(CRn) << 16) ^
                   (static_cast<unsigned>(CRm) << 8) ^ opc2;
    std::lock_guard<std::mutex> lock(m);
    if (!seen.insert(key).second) return;
    LOGI("%s p%u, %u, c%u, c%u, %u -> no-op", op, num, opc1, static_cast<unsigned>(CRn),
         static_cast<unsigned>(CRm), opc2);
}

uint32_t TlsBlock() {
    if (!t_tls_block && g_host) {
        t_tls_block = g_host->GuestAlloc(kGuestTlsBlockSize, 8);
    }
    return t_tls_block;
}

}  // namespace

GuestCp15::GuestCp15(DynarmicHost& host, unsigned coproc_num) : m_num(coproc_num) {
    g_host = &host;
}

// Every entry point below answers with something harmless rather than
// std::nullopt/std::monostate. Those two mean "compile a coprocessor exception",
// and in this backend that is an assertion that kills the process - not a fault
// the guest could ever see or handle.
std::optional<Dynarmic::A32::Coprocessor::Callback> GuestCp15::CompileInternalOperation(
    bool, unsigned opc1, CoprocReg, CoprocReg CRn, CoprocReg CRm, unsigned opc2) {
    LogOnce("CDP", m_num, opc1, CRn, CRm, opc2);
    return Callback{[](void*, uint32_t, uint32_t) -> uint64_t { return 0; }, std::nullopt};
}

Dynarmic::A32::Coprocessor::CallbackOrAccessOneWord GuestCp15::CompileSendOneWord(
    bool two, unsigned opc1, CoprocReg CRn, CoprocReg CRm, unsigned opc2) {
    // The MCR2 encoding reaches the same registers; refusing it here was enough
    // to abort the process once the game had run a couple of hundred frames.
    if (two) {
        LogOnce("MCR2", m_num, opc1, CRn, CRm, opc2);
        return Callback{[](void*, uint32_t, uint32_t) -> uint64_t { return 0; }, std::nullopt};
    }

    // Anything that is not CP15 is reported once and then ignored. On real
    // hardware an absent coprocessor traps as undefined, so the guest would not
    // be issuing this unless the register exists there - and an exception here
    // is not an exception the guest can see, it is an assertion that kills the
    // process from inside the emitter.
    if (m_num != 15) {
        LOGE("MCR p%u, %u, c%u, c%u, %u ignored", m_num, opc1, static_cast<unsigned>(CRn),
             static_cast<unsigned>(CRm), opc2);
        return Callback{[](void*, uint32_t, uint32_t) -> uint64_t { return 0; }, std::nullopt};
    }

    // c7: cache maintenance and the ARMv6 spellings of the barriers. The guest
    // arena is ordinary memory shared between host threads, so a data barrier is
    // a real barrier; cache maintenance has nothing to maintain.
    if (CRn == CoprocReg::C7) {
        return Callback{
            [](void*, uint32_t, uint32_t) -> uint64_t {
                std::atomic_thread_fence(std::memory_order_seq_cst);
                return 0;
            },
            std::nullopt};
    }

    // c13, opc2 = 2: TPIDRURW, the writable thread pointer.
    if (CRn == CoprocReg::C13 && opc1 == 0 && CRm == CoprocReg::C0 && opc2 == 2) {
        return Callback{
            [](void*, uint32_t value, uint32_t) -> uint64_t {
                t_tls_block = value;
                return 0;
            },
            std::nullopt};
    }

    LOGE("unhandled MCR p15, %u, c%u, c%u, %u - ignored", opc1, static_cast<unsigned>(CRn),
         static_cast<unsigned>(CRm), opc2);
    return Callback{[](void*, uint32_t, uint32_t) -> uint64_t { return 0; }, std::nullopt};
}

Dynarmic::A32::Coprocessor::CallbackOrAccessTwoWords GuestCp15::CompileSendTwoWords(bool, unsigned opc,
                                                                                   CoprocReg CRm) {
    LogOnce("MCRR", m_num, opc, CoprocReg::C0, CRm, 0);
    return Callback{[](void*, uint32_t, uint32_t) -> uint64_t { return 0; }, std::nullopt};
}

Dynarmic::A32::Coprocessor::CallbackOrAccessOneWord GuestCp15::CompileGetOneWord(
    bool two, unsigned opc1, CoprocReg CRn, CoprocReg CRm, unsigned opc2) {
    if (two) {
        LogOnce("MRC2", m_num, opc1, CRn, CRm, opc2);
        return Callback{[](void*, uint32_t, uint32_t) -> uint64_t { return 0; }, std::nullopt};
    }

    if (m_num != 15) {
        LOGE("MRC p%u, %u, c%u, c%u, %u ignored, reading zero", m_num, opc1,
             static_cast<unsigned>(CRn), static_cast<unsigned>(CRm), opc2);
        return Callback{[](void*, uint32_t, uint32_t) -> uint64_t { return 0; }, std::nullopt};
    }

    // c13, opc2 = 2 (TPIDRURW) and 3 (TPIDRURO) both answer with this thread's
    // block. bionic reads the read-only one from __get_tls().
    if (CRn == CoprocReg::C13 && opc1 == 0 && CRm == CoprocReg::C0 && (opc2 == 2 || opc2 == 3)) {
        return Callback{
            [](void*, uint32_t, uint32_t) -> uint64_t { return TlsBlock(); },
            std::nullopt};
    }

    LOGE("unhandled MRC p15, %u, c%u, c%u, %u - reading zero", opc1, static_cast<unsigned>(CRn),
         static_cast<unsigned>(CRm), opc2);
    return Callback{[](void*, uint32_t, uint32_t) -> uint64_t { return 0; }, std::nullopt};
}

Dynarmic::A32::Coprocessor::CallbackOrAccessTwoWords GuestCp15::CompileGetTwoWords(bool, unsigned opc,
                                                                                  CoprocReg CRm) {
    LogOnce("MRRC", m_num, opc, CoprocReg::C0, CRm, 0);
    return Callback{[](void*, uint32_t, uint32_t) -> uint64_t { return 0; }, std::nullopt};
}

std::optional<Dynarmic::A32::Coprocessor::Callback> GuestCp15::CompileLoadWords(
    bool, bool, CoprocReg CRd, std::optional<std::uint8_t>) {
    LogOnce("LDC", m_num, 0, CRd, CoprocReg::C0, 0);
    return Callback{[](void*, uint32_t, uint32_t) -> uint64_t { return 0; }, std::nullopt};
}

std::optional<Dynarmic::A32::Coprocessor::Callback> GuestCp15::CompileStoreWords(
    bool, bool, CoprocReg CRd, std::optional<std::uint8_t>) {
    LogOnce("STC", m_num, 0, CRd, CoprocReg::C0, 0);
    return Callback{[](void*, uint32_t, uint32_t) -> uint64_t { return 0; }, std::nullopt};
}

uint32_t GuestCp15CurrentTls() {
    return TlsBlock();
}

void GuestCp15SetCurrentTls(uint32_t value) {
    t_tls_block = value;
}
