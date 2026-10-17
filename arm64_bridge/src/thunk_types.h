#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <functional>
#include <string>

// Helper to convert 32-bit guest pointer to 64-bit host pointer
inline void* GuestToHost(uint8_t* base, uint32_t gaddr) {
    if (gaddr == 0) return nullptr;
    return reinterpret_cast<void*>(base + gaddr);
}

// Helper to convert 64-bit host pointer into 32-bit guest address (if within guest arena)
inline uint32_t HostToGuest(uint8_t* base, void* hptr) {
    if (!hptr) return 0;
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(hptr) - reinterpret_cast<uintptr_t>(base));
}

namespace Dynarmic::A32 { class Jit; }
class DynarmicHost;
struct stat;

// Drops any guest mutex/condition variable/semaphore living inside a range that
// the guest heap is about to reuse. Defined in thunks_libc.cpp.
void GuestSyncForget(uint32_t addr, uint32_t size);

// Starts a detached host thread that is allowed to run guest code.
//
// Not std::thread: that takes whatever default stack the platform hands out, and
// Dynarmic's IR passes recurse over the block they are translating (Value::IsImmediate
// walks Identity chains one frame at a time). How deep that goes is decided by the
// guest's own code, and a big enough Unity method overflowed the *host* stack and
// killed the process inside the optimiser. std::thread cannot ask for a bigger one.
void SpawnHostThread(std::function<void()> body);

// Sends the guest to the exit trap, which ends the RunFunction or CallGuestNested
// that is executing it. Clears the Thumb bit as well as setting PC: the ARM trap
// word read as Thumb is not a trap at all, and execution carries on into memory
// that is not code. Defined in dynarmic_host.cpp.
void GuestJumpToExitTrap(Dynarmic::A32::Jit& jit);

// diag: walks the calling thread's guest stack, keeping only words that have a call
// instruction in front of them. Returns how many it filled in. A raw dump is mostly
// object pointers; this is the difference between "libmono, somewhere" and a name.
int GuestBacktrace(DynarmicHost& host, uint32_t sp, uint32_t lr, uint32_t* out, int max_out);
// Guest logging obeys `log.tag.<TAG>`; see thunks_libc.cpp.
bool GuestLogAllowed(int prio, const char* tag);

// pthread_t as the guest knows it -> kernel tid. Set up by the pthread thunks;
// pthread_kill needs it because the signal table is keyed by gettid(). Returns 0
// when the id belongs to no live guest thread.
extern int (*GuestKernelTidLookup)(uint32_t guest_tid);

// ARM32 bionic's struct stat is the stat64 layout - 104 bytes, st_mode at 16.
// Defined in thunks_libc.cpp so every stat-family thunk agrees on it.
void WriteArm32Stat(uint8_t* guest_base, uint32_t gaddr, const struct stat& st);

// The guest's printf engine, defined in thunks_libc.cpp. `first_arg_reg` is the
// first variadic register; the va_list form takes the cursor va_start produced.
std::string FormatGuestString(Dynarmic::A32::Jit& jit, DynarmicHost& host, const char* fmt, int first_arg_reg);
std::string FormatGuestVaList(Dynarmic::A32::Jit& jit, DynarmicHost& host, const char* fmt, uint32_t va_list_ptr);
