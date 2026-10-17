#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace warband::input {

struct MapTouchConfig {
    float touch_slop = 8.0f;
    float edge_dead_zone_fraction = 0.055f;
    float bottom_control_exclusion_fraction = 0.12f;
    float pinch_slop = 12.0f;
    float rotation_slop_radians = 0.055f;
    float tilt_slop = 24.0f;
    float density = 1.0f;
    float surface_width = 0.0f;
    float surface_height = 0.0f;
    int64_t double_tap_timeout_ns = 320000000LL;
};

struct MapTouchPoint {
    int32_t pointer_id = -1;
    float x = 0.0f;
    float y = 0.0f;
};

struct MapTouchOutput {
    // Once true, the current event is exposed to the guest as ACTION_CANCEL.
    // The recognizer keeps ownership until the final pointer is released.
    bool cancel_guest_stream = false;
    float pan_dx_pixels = 0.0f;
    float pan_dy_pixels = 0.0f;
    // Navigation convention: positive zooms in, negative zooms out.
    float zoom_log_delta = 0.0f;
    // Signed angular delta made by the two-finger chord.
    float rotation_delta_radians = 0.0f;
    // Average vertical travel of a parallel two-finger chord since the
    // previous frame. Positive is a downward drag (towards top-down view),
    // negative is an upward drag (towards the inclined 3D view).
    float tilt_delta_pixels = 0.0f;
    bool recenter = false;
    // A short, unclaimed tap on map content. It deliberately does not make
    // operator bool true because the guest normally receives this tap.
    bool content_tap = false;

    explicit operator bool() const {
        return cancel_guest_stream || pan_dx_pixels != 0.0f ||
               pan_dy_pixels != 0.0f || zoom_log_delta != 0.0f ||
               rotation_delta_radians != 0.0f || tilt_delta_pixels != 0.0f ||
               recenter;
    }
};

class MapTouchRecognizer {
public:
    void Configure(const MapTouchConfig& config) {
        config_ = config;
        Cancel();
    }

    void Cancel() {
        ResetCurrent();
        ForgetTapHistory();
    }

    void ForgetTapHistory() {
        last_tap_valid_ = false;
        last_tap_time_ns_ = 0;
    }

    MapTouchOutput Begin(int32_t pointer_id, float x, float y,
                         int64_t event_time_ns = 0) {
        ResetCurrent();
        if (pointer_id < 0 || !FinitePoint(x, y)) return {};

        primary_id_ = pointer_id;
        primary_x_ = down_x_ = x;
        primary_y_ = down_y_ = y;
        down_time_ns_ = event_time_ns;
        current_event_time_ns_ = event_time_ns;

        if (IsCompassPoint(x, y)) {
            mode_ = Mode::Compass;
            ClaimGuestStream();
            return {.cancel_guest_stream = true};
        }
        if (IsEdgePoint(x, y)) {
            mode_ = Mode::EdgeSuppressed;
            ClaimGuestStream();
            return {.cancel_guest_stream = true};
        }
        if (IsDoubleTap(x, y, event_time_ns)) {
            mode_ = Mode::DoubleTapCandidate;
            last_tap_valid_ = false;
            ClaimGuestStream();
            return {.cancel_guest_stream = true};
        }

        mode_ = Mode::TapCandidate;
        return {};
    }

    void UpdatePointer(int32_t pointer_id, float x, float y) {
        if (!FinitePoint(x, y)) return;
        if (pointer_id == primary_id_) {
            primary_x_ = x;
            primary_y_ = y;
        } else if (pointer_id == secondary_id_) {
            secondary_x_ = x;
            secondary_y_ = y;
        }
    }

    MapTouchOutput PointerDown(int32_t pointer_id, float x, float y,
                               int64_t event_time_ns = 0) {
        if (pointer_id < 0 || !FinitePoint(x, y)) return {};
        if (primary_id_ < 0) return Begin(pointer_id, x, y, event_time_ns);
        if (pointer_id == primary_id_) {
            UpdatePointer(pointer_id, x, y);
            return {};
        }
        if (pointer_id == secondary_id_) return {};
        if (secondary_id_ >= 0) {
            ClaimGuestStream();
            mode_ = Mode::MultiCompleted;
            return {.cancel_guest_stream = true};
        }

        secondary_id_ = pointer_id;
        secondary_x_ = secondary_down_x_ = x;
        secondary_y_ = secondary_down_y_ = y;
        primary_down_x_ = primary_x_;
        primary_down_y_ = primary_y_;
        last_distance_ = initial_distance_ =
            Distance(primary_x_, primary_y_, secondary_x_, secondary_y_);
        last_angle_ = initial_angle_ =
            Angle(primary_x_, primary_y_, secondary_x_, secondary_y_);
        initial_centroid_y_ = last_centroid_y_ =
            (primary_y_ + secondary_y_) * 0.5f;
        mode_ = Mode::TwoFingerPending;
        current_event_time_ns_ = event_time_ns;
        last_tap_valid_ = false;
        ClaimGuestStream();
        return {.cancel_guest_stream = true};
    }

    MapTouchOutput MoveFrame(const MapTouchPoint* points, std::size_t count,
                             int64_t event_time_ns = 0) {
        if (!points || count == 0 || primary_id_ < 0) return {};

        const float old_primary_x = primary_x_;
        const float old_primary_y = primary_y_;
        for (std::size_t index = 0; index < count; ++index) {
            UpdatePointer(points[index].pointer_id, points[index].x, points[index].y);
        }
        current_event_time_ns_ = event_time_ns;

        MapTouchOutput output{};
        switch (mode_) {
            case Mode::TapCandidate: {
                if (Distance(down_x_, down_y_, primary_x_, primary_y_) >= SafeTouchSlop()) {
                    mode_ = Mode::OneFingerPan;
                    ClaimGuestStream();
                    output.cancel_guest_stream = true;
                    output.pan_dx_pixels = primary_x_ - down_x_;
                    output.pan_dy_pixels = primary_y_ - down_y_;
                }
                break;
            }
            case Mode::OneFingerPan:
                output.pan_dx_pixels = primary_x_ - old_primary_x;
                output.pan_dy_pixels = primary_y_ - old_primary_y;
                break;
            case Mode::DoubleTapCandidate: {
                if (Distance(down_x_, down_y_, primary_x_, primary_y_) >= SafeTouchSlop()) {
                    mode_ = Mode::DoubleTapDrag;
                    output.zoom_log_delta = DoubleTapDragZoom(primary_y_ - down_y_);
                }
                break;
            }
            case Mode::DoubleTapDrag:
                output.zoom_log_delta = DoubleTapDragZoom(primary_y_ - old_primary_y);
                break;
            case Mode::TwoFingerPending:
            case Mode::TwoFingerTransform:
            case Mode::TwoFingerTilt:
                output = MoveTwoFinger();
                break;
            default:
                break;
        }
        return output;
    }

    MapTouchOutput PointerUp(int32_t pointer_id, int64_t event_time_ns = 0) {
        if (pointer_id < 0 ||
            (pointer_id != primary_id_ && pointer_id != secondary_id_)) {
            return {};
        }
        current_event_time_ns_ = event_time_ns;
        MapTouchOutput output{};
        if (mode_ == Mode::TwoFingerPending) {
            // A chord released before becoming a transform is a two-finger
            // tap, matching navigation apps' one-step zoom-out gesture.
            output.zoom_log_delta = -kTapZoomStep;
        }

        if (secondary_id_ >= 0) {
            if (pointer_id == primary_id_) {
                primary_id_ = secondary_id_;
                primary_x_ = secondary_x_;
                primary_y_ = secondary_y_;
            }
            secondary_id_ = -1;
            mode_ = Mode::MultiCompleted;
        } else {
            primary_id_ = -1;
        }
        output.cancel_guest_stream = guest_stream_cancelled_;
        return output;
    }

    MapTouchOutput Finish(int32_t pointer_id, int64_t event_time_ns = 0) {
        if (pointer_id != primary_id_ && pointer_id != secondary_id_) return {};
        current_event_time_ns_ = event_time_ns;

        MapTouchOutput output{};
        output.cancel_guest_stream = guest_stream_cancelled_;
        if (mode_ == Mode::TapCandidate) {
            const bool short_tap = down_time_ns_ > 0 && event_time_ns >= down_time_ns_ &&
                event_time_ns - down_time_ns_ <= kContentTapTimeoutNs;
            if (short_tap) {
                output.content_tap = true;
                RememberTap(event_time_ns);
            } else {
                ForgetTapHistory();
            }
        } else if (mode_ == Mode::Compass &&
                   Distance(down_x_, down_y_, primary_x_, primary_y_) <=
                       SafeTouchSlop() * 2.0f) {
            output.recenter = true;
        }

        ResetCurrent();
        return output;
    }

    bool IsTracking() const { return primary_id_ >= 0; }
    int32_t PrimaryId() const { return primary_id_; }
    int32_t SecondaryId() const { return secondary_id_; }
    bool IsPinching() const { return mode_ == Mode::TwoFingerTransform; }
    bool IsEdgeSuppressed() const { return mode_ == Mode::EdgeSuppressed; }
    bool GuestStreamCancelled() const { return guest_stream_cancelled_; }

private:
    enum class Mode : uint8_t {
        Idle = 0,
        TapCandidate,
        EdgeSuppressed,
        Compass,
        OneFingerPan,
        DoubleTapCandidate,
        DoubleTapDrag,
        TwoFingerPending,
        TwoFingerTransform,
        TwoFingerTilt,
        MultiCompleted,
    };

    static constexpr float kTapZoomStep = 0.32f;
    static constexpr float kPi = 3.14159265358979323846f;
    static constexpr int64_t kContentTapTimeoutNs = 500000000LL;

    static bool FinitePoint(float x, float y) {
        return std::isfinite(x) && std::isfinite(y);
    }

    static float Distance(float first_x, float first_y,
                          float second_x, float second_y) {
        return std::hypot(first_x - second_x, first_y - second_y);
    }

    static float Angle(float first_x, float first_y,
                       float second_x, float second_y) {
        return std::atan2(second_y - first_y, second_x - first_x);
    }

    static float WrapAngle(float angle) {
        while (angle > kPi) angle -= 2.0f * kPi;
        while (angle < -kPi) angle += 2.0f * kPi;
        return angle;
    }

    void ClaimGuestStream() { guest_stream_cancelled_ = true; }

    void ResetCurrent() {
        primary_id_ = -1;
        secondary_id_ = -1;
        primary_x_ = primary_y_ = 0.0f;
        secondary_x_ = secondary_y_ = 0.0f;
        down_x_ = down_y_ = 0.0f;
        primary_down_x_ = primary_down_y_ = 0.0f;
        secondary_down_x_ = secondary_down_y_ = 0.0f;
        initial_distance_ = last_distance_ = 0.0f;
        initial_angle_ = last_angle_ = 0.0f;
        initial_centroid_y_ = last_centroid_y_ = 0.0f;
        current_event_time_ns_ = 0;
        down_time_ns_ = 0;
        mode_ = Mode::Idle;
        guest_stream_cancelled_ = false;
    }

    float SafeTouchSlop() const {
        return std::clamp(std::isfinite(config_.touch_slop) ? config_.touch_slop : 8.0f,
                          1.0f, 512.0f);
    }

    float SafePinchSlop() const {
        return std::clamp(std::isfinite(config_.pinch_slop) ? config_.pinch_slop : 12.0f,
                          1.0f, 1024.0f);
    }

    float SafeRotationSlop() const {
        return std::clamp(std::isfinite(config_.rotation_slop_radians)
                              ? config_.rotation_slop_radians
                              : 0.055f,
                          0.01f, 0.5f);
    }

    float SafeTiltSlop() const {
        return std::clamp(std::isfinite(config_.tilt_slop) ? config_.tilt_slop : 24.0f,
                          2.0f, 1024.0f);
    }

    float DoubleTapDragZoom(float delta_y) const {
        const float height = std::max(config_.surface_height, 1.0f);
        const float value = -delta_y * 4.0f / height;
        return std::isfinite(value) ? value : 0.0f;
    }

    bool IsDoubleTap(float x, float y, int64_t event_time_ns) const {
        if (!last_tap_valid_ || event_time_ns <= last_tap_time_ns_) return false;
        const int64_t timeout = std::clamp<int64_t>(config_.double_tap_timeout_ns,
                                                    100000000LL, 1000000000LL);
        return event_time_ns - last_tap_time_ns_ <= timeout &&
               Distance(x, y, last_tap_x_, last_tap_y_) <= SafeTouchSlop() * 3.0f;
    }

    void RememberTap(int64_t event_time_ns) {
        if (event_time_ns <= 0) {
            last_tap_valid_ = false;
            return;
        }
        last_tap_valid_ = true;
        last_tap_time_ns_ = event_time_ns;
        last_tap_x_ = primary_x_;
        last_tap_y_ = primary_y_;
    }

    MapTouchOutput MoveTwoFinger() {
        MapTouchOutput output{};
        if (secondary_id_ < 0 || !(initial_distance_ > 0.0f)) return output;

        const float distance = Distance(primary_x_, primary_y_, secondary_x_, secondary_y_);
        const float angle = Angle(primary_x_, primary_y_, secondary_x_, secondary_y_);
        const float centroid_y = (primary_y_ + secondary_y_) * 0.5f;
        if (!(distance > 0.0f) || !std::isfinite(angle)) return output;

        if (mode_ == Mode::TwoFingerPending) {
            const float distance_delta = std::fabs(distance - initial_distance_);
            const float angle_delta = std::fabs(WrapAngle(angle - initial_angle_));
            const float first_dx = primary_x_ - primary_down_x_;
            const float first_dy = primary_y_ - primary_down_y_;
            const float second_dx = secondary_x_ - secondary_down_x_;
            const float second_dy = secondary_y_ - secondary_down_y_;
            const float average_dy = (first_dy + second_dy) * 0.5f;
            const bool same_vertical_direction = first_dy * second_dy > 0.0f;
            const float vertical_travel = std::min(std::fabs(first_dy),
                                                   std::fabs(second_dy));
            const float horizontal_tolerance = std::max(
                SafeTiltSlop(), vertical_travel * 0.60f);
            const float vertical_match_tolerance = std::max(
                SafeTiltSlop() * 1.5f, vertical_travel * 0.50f);
            const bool parallel_vertical = same_vertical_direction &&
                std::fabs(average_dy) >= SafeTiltSlop() &&
                std::fabs(first_dx) <= horizontal_tolerance &&
                std::fabs(second_dx) <= horizontal_tolerance &&
                std::fabs(first_dy - second_dy) <= vertical_match_tolerance &&
                distance_delta < SafePinchSlop() * 2.5f &&
                angle_delta < SafeRotationSlop() * 2.5f;

            if (parallel_vertical) {
                mode_ = Mode::TwoFingerTilt;
                output.tilt_delta_pixels = centroid_y - initial_centroid_y_;
            } else if (distance_delta >= SafePinchSlop() ||
                       angle_delta >= SafeRotationSlop()) {
                mode_ = Mode::TwoFingerTransform;
            }
        }

        if (mode_ == Mode::TwoFingerTilt) {
            // The classification frame emits the full slop-qualified travel;
            // subsequent frames emit only their incremental movement.
            if (output.tilt_delta_pixels == 0.0f) {
                output.tilt_delta_pixels = centroid_y - last_centroid_y_;
            }
            if (!std::isfinite(output.tilt_delta_pixels)) {
                output.tilt_delta_pixels = 0.0f;
            }
        } else if (mode_ == Mode::TwoFingerTransform) {
            if (last_distance_ > 0.0f) {
                output.zoom_log_delta = std::log(distance / last_distance_);
                if (!std::isfinite(output.zoom_log_delta)) output.zoom_log_delta = 0.0f;
            }
            output.rotation_delta_radians = WrapAngle(angle - last_angle_);
            if (!std::isfinite(output.rotation_delta_radians)) {
                output.rotation_delta_radians = 0.0f;
            }
        }
        last_distance_ = distance;
        last_angle_ = angle;
        last_centroid_y_ = centroid_y;
        return output;
    }

    bool IsEdgePoint(float x, float y) const {
        if (!(config_.surface_width > 0.0f && config_.surface_height > 0.0f) ||
            !std::isfinite(config_.edge_dead_zone_fraction)) {
            return false;
        }
        const float controls_fraction = std::clamp(
            std::isfinite(config_.bottom_control_exclusion_fraction)
                ? config_.bottom_control_exclusion_fraction
                : 0.12f,
            0.0f, 0.40f);
        if (y >= config_.surface_height * (1.0f - controls_fraction)) return false;
        const float fraction = std::clamp(config_.edge_dead_zone_fraction, 0.0f, 0.25f);
        const float edge_x = config_.surface_width * fraction;
        const float edge_y = config_.surface_height * fraction;
        return x <= edge_x || x >= config_.surface_width - edge_x ||
               y <= edge_y || y >= config_.surface_height - edge_y;
    }

    bool IsCompassPoint(float x, float y) const {
        if (!(config_.surface_width > 0.0f && config_.surface_height > 0.0f)) return false;
        const float viewport_width = std::min(config_.surface_width,
                                              config_.surface_height * (4.0f / 3.0f));
        const float viewport_left = (config_.surface_width - viewport_width) * 0.5f;
        const float compass_x = viewport_left + viewport_width * 0.0125f;
        const float compass_y = config_.surface_height * 0.882f;
        const float density = std::clamp(std::isfinite(config_.density) ? config_.density : 1.0f,
                                         0.5f, 8.0f);
        // At least 80 dp in diameter (~12.7 mm under Android's dp contract),
        // while preserving a usable target on low-density render surfaces.
        const float radius = std::max(40.0f * density, config_.surface_height * 0.050f);
        return Distance(x, y, compass_x, compass_y) <= radius;
    }

    MapTouchConfig config_{};
    Mode mode_ = Mode::Idle;
    int32_t primary_id_ = -1;
    int32_t secondary_id_ = -1;
    float primary_x_ = 0.0f;
    float primary_y_ = 0.0f;
    float secondary_x_ = 0.0f;
    float secondary_y_ = 0.0f;
    float down_x_ = 0.0f;
    float down_y_ = 0.0f;
    float primary_down_x_ = 0.0f;
    float primary_down_y_ = 0.0f;
    float secondary_down_x_ = 0.0f;
    float secondary_down_y_ = 0.0f;
    float initial_distance_ = 0.0f;
    float last_distance_ = 0.0f;
    float initial_angle_ = 0.0f;
    float last_angle_ = 0.0f;
    float initial_centroid_y_ = 0.0f;
    float last_centroid_y_ = 0.0f;
    int64_t current_event_time_ns_ = 0;
    int64_t down_time_ns_ = 0;
    bool guest_stream_cancelled_ = false;

    bool last_tap_valid_ = false;
    int64_t last_tap_time_ns_ = 0;
    float last_tap_x_ = 0.0f;
    float last_tap_y_ = 0.0f;
};

}  // namespace warband::input
