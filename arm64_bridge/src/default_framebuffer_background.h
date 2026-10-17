#pragma once

#include <cstdint>

namespace warband::default_framebuffer_background {

inline constexpr bool IsEligibleSurface(int width, int height) noexcept {
    if (width <= 0 || height <= 0) return false;
    // Limit the compatibility paint to 2:1-and-wider landscape surfaces.  It
    // covers the reported 2400x1080 class without touching ordinary 16:9.
    return static_cast<std::int64_t>(width) >=
           static_cast<std::int64_t>(height) * 2;
}

inline constexpr bool ShouldReplaceColourClear(int framebuffer,
                                               int surface_width,
                                               int surface_height) noexcept {
    return framebuffer == 0 &&
           IsEligibleSurface(surface_width, surface_height);
}

inline constexpr bool ShouldClearBeforeDefaultDraw(
        bool first_draw, int framebuffer, int surface_width,
        int surface_height) noexcept {
    return first_draw && framebuffer == 0 &&
           IsEligibleSurface(surface_width, surface_height);
}

}  // namespace warband::default_framebuffer_background

namespace bridge_gl {

// Replaces an ultrawide default-framebuffer colour clear with a deterministic
// background clear. Returns true only when the caller must omit
// GL_COLOR_BUFFER_BIT from that guest clear. Every colour clear is replaced:
// the legacy renderer can issue a second clear after its frame prologue.
bool ReplaceDefaultFramebufferColourClear() noexcept;
bool ClearDefaultFramebufferBeforeDraw(int framebuffer) noexcept;
void NotifyDefaultFramebufferFramePresented() noexcept;

}  // namespace bridge_gl
