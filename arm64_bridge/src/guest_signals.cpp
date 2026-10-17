#include "guest_signals.h"

#include <android/log.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <cstdio>

#include <dynarmic/interface/A32/a32.h>

#include "dynarmic_host.h"

#define LOG_TAG "CMR_Signals"
#define LOGI(...) ((void)0)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

constexpr int kMaxSignals = 64;
std::atomic<uint32_t> g_handlers[kMaxSignals];

struct SigThread {
    std::mutex m;
    std::condition_variable cv;
    std::deque<int> pending;
    // Mirrors pending.size(). Every thunk return asks "is anything pending?", which is
    // hundreds of thousands of calls a second, and taking a mutex for each of those
    // costs more than the signal machinery itself.
    std::atomic<int> pending_count{0};
    // Every Jit this thread might be executing: its own plus any nested ones.
    std::mutex jits_m;
    std::vector<Dynarmic::A32::Jit*> jits;
};

std::mutex g_table_mutex;
std::map<int, std::shared_ptr<SigThread>> g_threads;  // guest gettid() -> state
thread_local std::shared_ptr<SigThread> t_self;
// t_self retains ownership until thread exit; registration refreshes this alias.
thread_local SigThread* t_self_current = nullptr;
// The kernel blocks a signal while its handler runs. Mirroring that avoids
// recursively re-entering Mono's suspend handler on repeated GC retries.
thread_local uint64_t t_active_signals = 0;

std::shared_ptr<SigThread> Find(int tid) {
    std::lock_guard<std::mutex> lock(g_table_mutex);
    auto it = g_threads.find(tid);
    return it == g_threads.end() ? nullptr : it->second;
}

}  // namespace

void GuestSignalSetHandler(int signum, uint32_t guest_handler) {
    if (signum < 0 || signum >= kMaxSignals) return;
    g_handlers[signum].store(guest_handler);
}

void GuestSignalRegisterThread(int tid, Dynarmic::A32::Jit* jit) {
    auto state = std::make_shared<SigThread>();
    state->jits.push_back(jit);
    {
        std::lock_guard<std::mutex> lock(g_table_mutex);
        // A kernel tid is reused once its thread is gone. Leaving the dead entry
        // in place would route a signal meant for the new thread to a record
        // nobody is watching, and Boehm would wait for an acknowledgement that
        // cannot arrive.
        if (g_threads.count(tid)) LOGI("guest thread tid %d reused; dropping the old record", tid);
        g_threads[tid] = state;
    }
    t_self = std::move(state);
    t_self_current = t_self.get();
}

void GuestSignalUnregisterThread(int tid) {
    std::lock_guard<std::mutex> lock(g_table_mutex);
    g_threads.erase(tid);
}

int GuestSignalSend(int tid, int signum) {
    auto target = Find(tid);
    // ESRCH is not a failure here: GC_suspend_all treats it as "that thread is
    // gone" and stops expecting an acknowledgement from it. Returning success for
    // a thread that will never answer is what deadlocks the collector.
    if (!target) return 3;  // ESRCH
    if (signum < 0 || signum >= kMaxSignals) return 22;  // EINVAL

    {
        std::lock_guard<std::mutex> lock(target->m);
        target->pending.push_back(signum);
        target->pending_count.store(static_cast<int>(target->pending.size()));
    }
    target->cv.notify_all();

    // If the thread is running guest code it will not reach a thunk on its own,
    // so its Jit is stopped and the run loop delivers the signal before re-entering
    // at the same PC. All of the thread's Jits are stopped because it may be inside
    // a nested call - a bsearch comparator, or another signal handler - and halting
    // only the outermost one leaves the signal sitting there undelivered. That is
    // exactly what left Boehm's stop-the-world one acknowledgement short.
    std::lock_guard<std::mutex> lock(target->jits_m);
    for (auto* jit : target->jits) jit->HaltExecution(Dynarmic::HaltReason::UserDefined2);
    return 0;
}

bool GuestSignalPending() {
    auto* state = t_self_current;
    return state && state->pending_count.load(std::memory_order_relaxed) > 0;
}

void GuestSignalDeliverPending(DynarmicHost& host) {
    if (!t_self) return;
    for (;;) {
        int signum;
        {
            std::lock_guard<std::mutex> lock(t_self->m);
            if (t_self->pending.empty()) return;
            signum = t_self->pending.front();
            t_self->pending.pop_front();
            t_self->pending_count.store(static_cast<int>(t_self->pending.size()));
        }
        uint32_t handler = g_handlers[signum].load();
        if (handler <= 1) continue;  // SIG_DFL / SIG_IGN
        uint64_t bit = uint64_t{1} << signum;
        if (t_active_signals & bit) continue;
        // The handler runs on this thread, just below its current frame, so the
        // stack pointer it records for the collector points into the real stack.
        t_active_signals |= bit;
        host.CallGuestNested(handler, {static_cast<uint32_t>(signum)});
        t_active_signals &= ~bit;
    }
}

void GuestSignalWait(DynarmicHost& host) {
    if (!t_self) return;
    // Timed, not indefinite: the signal that would have woken this thread can be
    // delivered on the way out of a thunk instead, which empties the queue before the
    // wait even starts. sigsuspend returning EINTR spuriously is allowed - every
    // caller of it loops on a condition of its own.
    {
        std::unique_lock<std::mutex> lock(t_self->m);
        t_self->cv.wait_for(lock, std::chrono::milliseconds(20),
                            [] { return !t_self->pending.empty(); });
    }
    GuestSignalDeliverPending(host);
}

void GuestSignalAddThreadJit(Dynarmic::A32::Jit* jit) {
    if (!t_self || !jit) return;
    std::lock_guard<std::mutex> lock(t_self->jits_m);
    t_self->jits.push_back(jit);
}
