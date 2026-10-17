#ifndef WARBAND_CAMERA_LOOK_INPUT_H
#define WARBAND_CAMERA_LOOK_INPUT_H

#include <algorithm>
#include <cmath>

namespace warband::camera_look {

inline constexpr float kReferenceFrameSeconds = 1.0f / 60.0f;
inline constexpr float kStickLookScale = 60.0f;
/* Guest mouse counts move the camera much faster than the right-stick path. */
inline constexpr float kRelativeMouseScale = 1.0f / (kStickLookScale * 16.0f);
inline constexpr float kMaxRelativeLookPerFrame = 160.0f;

/*
 * A stick is integrated over frame time by the guest. Scale the injected axis
 * by reference_dt / frame_dt so the same accumulated swipe contributes the
 * same axis-time at 30, 60 or 90 frames per second.
 */
inline float NormalizedStickAxis(float look_delta, float frame_seconds) {
    if (!std::isfinite(look_delta) || !std::isfinite(frame_seconds) ||
        frame_seconds <= 0.0f) {
        return 0.0f;
    }
    const float axis = look_delta / kStickLookScale *
                       (kReferenceFrameSeconds / frame_seconds);
    return std::clamp(axis, -1.0f, 1.0f);
}

inline float ScaledRelativeMouseDelta(float look_delta) {
    if (!std::isfinite(look_delta)) return 0.0f;
    return std::clamp(look_delta * kRelativeMouseScale, -1.0f, 1.0f);
}

struct RelativeLookDelta {
    float emitted;
    float remainder;
};

/*
 * Tactical_window converts its relative mouse deltas to integers. Carry the
 * fractional part across frames so high-density touch input is not discarded.
 */
inline RelativeLookDelta QuantizeRelativeDelta(float look_delta, float remainder) {
    const float safe_delta = std::isfinite(look_delta) ? look_delta : 0.0f;
    const float safe_remainder = std::isfinite(remainder) ? remainder : 0.0f;
    const float total = std::clamp(safe_delta + safe_remainder,
                                   -kMaxRelativeLookPerFrame,
                                   kMaxRelativeLookPerFrame);
    const float emitted = std::trunc(total);
    return {emitted, total - emitted};
}

}  // namespace warband::camera_look

#endif  // WARBAND_CAMERA_LOOK_INPUT_H
