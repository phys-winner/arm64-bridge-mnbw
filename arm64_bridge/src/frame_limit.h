#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

namespace warband::frame_limit {

// Host-side frame pacing for the guest render loop.  0 disables the limiter
// (the swap then blocks on vsync at the display refresh rate only).  The
// overlay writes a value selected in the Additional settings tab.
inline std::atomic<int> g_target_fps{0};

inline void SetTargetFps(int fps) {
    g_target_fps.store(fps < 0 ? 0 : fps, std::memory_order_relaxed);
}

inline int TargetFps() {
    return g_target_fps.load(std::memory_order_relaxed);
}

// Called from the eglSwapBuffers thunk on the guest render thread.  Sleeping
// before the swap keeps the guest's per-frame dt small; pacing after vsync
// would quantize every dt to whole display intervals instead.
inline void WaitBeforeSwap() {
    const int fps = TargetFps();
    if (fps <= 0) return;
    using clock = std::chrono::steady_clock;
    static clock::time_point last_swap{};
    const auto now = clock::now();
    if (last_swap.time_since_epoch().count() != 0) {
        const auto interval = std::chrono::microseconds(1000000 / fps);
        const auto deadline = last_swap + interval;
        if (now < deadline) {
            std::this_thread::sleep_for(deadline - now);
        }
    }
    last_swap = clock::now();
}

}  // namespace warband::frame_limit
