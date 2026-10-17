#pragma once

#include <atomic>
#include <cmath>

namespace bridge_gl {

inline std::atomic<float> g_hdr_brightness{0.75f};

inline float BoundHdrBrightness(float value) noexcept {
    if (!std::isfinite(value)) return 0.75f;
    if (value < 0.5f) return 0.5f;
    if (value > 1.25f) return 1.25f;
    return value;
}

inline void SetHdrBrightness(float value) noexcept {
    g_hdr_brightness.store(BoundHdrBrightness(value), std::memory_order_relaxed);
}

inline float HdrBrightness() noexcept {
    return g_hdr_brightness.load(std::memory_order_relaxed);
}

}  // namespace bridge_gl
