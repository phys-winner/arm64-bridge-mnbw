#pragma once

#include <cstdint>

namespace Dynarmic::A32 {
class Jit;
}
class DynarmicHost;

// Guest signal delivery, for one reason: Boehm stops the world with signals.
//
// GC_stop_world sends tkill(tid, SIGPWR) to every collector-registered thread and
// then blocks on a semaphore until each one acknowledges from its signal handler.
// With tkill stubbed out the acknowledgements never come and the first collection
// hangs the process for good - which is exactly where the port stood.
//
// So a signal here is: remember the handler the guest installed, mark the target
// guest thread, make it stop what it is doing, and run that handler *on that
// thread* with its own stack. The last part is not cosmetic - Boehm records the
// handler's stack pointer as the top of the region it will scan for roots, so the
// handler has to run on the stack it is describing.

// Records what the guest passed to sigaction/signal. Handler 0 and 1 are
// SIG_DFL/SIG_IGN and mean "deliver nothing".
void GuestSignalSetHandler(int signum, uint32_t guest_handler);

// Called once per guest thread, when its Jit is created. `tid` is what the guest's
// own gettid() returned, because that is the value it will hand to tkill.
void GuestSignalRegisterThread(int tid, Dynarmic::A32::Jit* jit);

// A thread running a nested guest call is executing a different Jit, and that is
// the one that has to be stopped for it to notice a signal. Nested Jits are
// registered here as they are created.
void GuestSignalAddThreadJit(Dynarmic::A32::Jit* jit);
void GuestSignalUnregisterThread(int tid);

// tkill/pthread_kill. Returns 0, or ESRCH when the thread is not a guest thread -
// which is the answer Boehm handles correctly (it stops waiting for that one).
int GuestSignalSend(int tid, int signum);

// True when this thread has a signal waiting to be run.
bool GuestSignalPending();

// Runs every pending handler on the calling thread. Safe to call with no signals
// pending. Never call while holding a lock the handler might take.
void GuestSignalDeliverPending(DynarmicHost& host);

// sigsuspend: block until a signal arrives for this thread, run it, return.
void GuestSignalWait(DynarmicHost& host);

