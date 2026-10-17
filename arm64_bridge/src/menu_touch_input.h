#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "input_arbitration.h"

namespace warband::input {

inline constexpr uint32_t kUnknownMenuWindowId =
    std::numeric_limits<uint32_t>::max();

// The guest only draws the LB/RB page badges on these proven windows.  Keep
// shoulder synthesis fail-closed elsewhere so an invisible corner target
// cannot activate unrelated widget actions.
inline constexpr bool MenuWindowSupportsShoulders(uint32_t window_id) {
    return window_id == 11u || window_id == 13u || window_id == 15u ||
           window_id == 29u || window_id == 31u;
}

struct MenuTouchConfig {
    bool swipe_navigation = true;
    bool tap_confirm = false;
    bool shoulder_buttons = true;
    float touch_slop = 8.0f;
    float step_distance = 32.0f;
    float shoulder_size = 48.0f;
    float shoulder_tap_slop = 12.0f;
    float surface_width = 0.0f;
    float surface_height = 0.0f;
};

// Result of one recognizer step.  `swallow` marks a touch stream whose
// DOWN/UP pair would straddle a game-screen change: the guest must never see
// that release, otherwise it presses whatever widget now sits under the
// finger.  `scroll_dy_fraction` is the raw vertical drag distance in screen
// fractions, applied by the guest-thread scroll walker.
struct MenuMoveOutput {
    bool swallow = false;
    float scroll_dy_fraction = 0.0f;
    // Keep the scroll target bound to the pane where the gesture started.
    // The finger may leave a narrow viewport during a long drag, but that
    // must not retarget or stall the already-owned scroll gesture.
    float scroll_anchor_x = 0.0f;
    float scroll_anchor_y = 0.0f;

    explicit operator bool() const { return swallow || scroll_dy_fraction != 0.0f; }
};

class MenuTouchRecognizer {
public:
    void Configure(const MenuTouchConfig& config) {
        config_ = config;
        Cancel();
    }

    void Cancel() {
        pointer_id_ = -1;
        window_id_ = kUnknownMenuWindowId;
        direction_ = Direction::Pending;
        shoulder_mask_ = 0;
        shoulder_eligible_ = false;
    }

    void Begin(int32_t pointer_id, float x, float y, uint32_t window_id) {
        Cancel();
        if (pointer_id < 0 || window_id == kUnknownMenuWindowId || !FinitePoint(x, y)) {
            return;
        }

        if (config_.shoulder_buttons && MenuWindowSupportsShoulders(window_id)) {
            shoulder_mask_ = ShoulderMask(x, y);
            if (shoulder_mask_ != 0) {
                pointer_id_ = pointer_id;
                window_id_ = window_id;
                down_x_ = x;
                down_y_ = y;
                shoulder_eligible_ = true;
                return;
            }
        }
        if (!config_.swipe_navigation && !config_.tap_confirm) return;
        pointer_id_ = pointer_id;
        window_id_ = window_id;
        down_x_ = x;
        down_y_ = y;
        last_y_ = y;
    }

    MenuMoveOutput Move(int32_t pointer_id, float x, float y, uint32_t current_window_id) {
        if (pointer_id != pointer_id_ || !FinitePoint(x, y)) return {};
        if (shoulder_mask_ != 0) {
            if (std::fabs(x - down_x_) > config_.shoulder_tap_slop ||
                std::fabs(y - down_y_) > config_.shoulder_tap_slop) {
                shoulder_eligible_ = false;
            }
            return {};
        }
        // The active game screen changed while the finger is down.  Nothing the
        // guest does with this stream can be trusted anymore.
        if (!MatchesWindow(current_window_id)) {
            direction_ = Direction::Pending;
            return {.swallow = true, .scroll_dy_fraction = 0.0f,
                    .scroll_anchor_x = down_x_, .scroll_anchor_y = down_y_};
        }

        MenuMoveOutput output{};
        if (direction_ == Direction::Pending &&
            std::max(std::fabs(x - down_x_), std::fabs(y - down_y_)) >= config_.touch_slop) {
            // Same diagonal thresholds as the retired pulse recognizer: clearly
            // vertical list drags scroll; clearly horizontal slider/carousel
            // drags are permanently excluded.
            const float abs_x = std::fabs(x - down_x_);
            const float abs_y = std::fabs(y - down_y_);
            if (abs_y >= abs_x * 0.72f) {
                direction_ = Direction::Vertical;
            } else if (abs_x >= abs_y * 1.5f) {
                direction_ = Direction::Horizontal;
            }
        }
        if (direction_ == Direction::Vertical && config_.swipe_navigation) {
            const float height = config_.surface_height;
            if (height > 0.0f) {
                output.scroll_dy_fraction = (y - last_y_) / height;
                output.scroll_anchor_x = down_x_;
                output.scroll_anchor_y = down_y_;
            }
        }
        last_y_ = y;
        return output;
    }

    MenuMoveOutput Finish(int32_t pointer_id, float x, float y, uint32_t current_window_id) {
        if (pointer_id != pointer_id_) return {};
        MenuMoveOutput output{};
        if (shoulder_mask_ != 0) {
            Move(pointer_id, x, y, current_window_id);
            if (shoulder_eligible_ && MatchesWindow(current_window_id)) {
                // Shoulder taps stay synthetic discrete buttons; the pulse
                // scheduler owns their press/release timing.
                output.swallow = false;
            }
        } else if (!MatchesWindow(current_window_id)) {
            // Release on a different screen than the press: swallow it so the
            // new screen cannot receive a phantom widget press.
            output.swallow = true;
        }
        Cancel();
        return output;
    }

    int32_t PointerId() const { return pointer_id_; }
    uint32_t WindowId() const { return window_id_; }
    bool MatchesWindow(uint32_t window_id) const {
        return IsTracking() && window_id != kUnknownMenuWindowId && window_id_ == window_id;
    }
    bool IsTracking() const { return pointer_id_ >= 0; }
    bool IsShoulderTracking() const { return shoulder_mask_ != 0; }
    bool OwnsVerticalScroll() const {
        return IsTracking() && direction_ == Direction::Vertical &&
               config_.swipe_navigation;
    }

    // Discrete button press/release for a synthesized shoulder tap, consumed by
    // the caller through the same arbitrator path as physical gamepads.  Only
    // valid while the tap is still eligible and on the original window.
    struct DiscretePulse {
        uint32_t mask = 0;
        uint32_t count = 0;
    };

    DiscretePulse TakeShoulderPulse() {
        DiscretePulse pulse{};
        if (IsShoulderTracking() && shoulder_eligible_) pulse = {shoulder_mask_, 1u};
        shoulder_eligible_ = false;
        return pulse;
    }

private:
    enum class Direction : uint8_t { Pending, Vertical, Horizontal };

    static bool FinitePoint(float x, float y) {
        return std::isfinite(x) && std::isfinite(y);
    }

    uint32_t ShoulderMask(float x, float y) const {
        if (config_.surface_width <= 0.0f || config_.surface_height <= 0.0f ||
            config_.shoulder_size <= 0.0f || y < 0.0f) {
            return 0;
        }
        const float viewport_width = std::min(
            config_.surface_width, config_.surface_height * 4.0f / 3.0f);
        const float viewport_left = (config_.surface_width - viewport_width) * 0.5f;
        const float viewport_right = viewport_left + viewport_width;
        const float size = std::min(config_.shoulder_size,
            std::min(viewport_width * 0.18f, config_.surface_height * 0.18f));
        if (y > size) return 0;
        if (x >= viewport_left && x <= viewport_left + size) return kButtonL1;
        if (x >= viewport_right - size && x <= viewport_right) return kButtonR1;
        return 0;
    }

    MenuTouchConfig config_{};
    int32_t pointer_id_ = -1;
    uint32_t window_id_ = kUnknownMenuWindowId;
    Direction direction_ = Direction::Pending;
    uint32_t shoulder_mask_ = 0;
    bool shoulder_eligible_ = false;
    float down_x_ = 0.0f;
    float down_y_ = 0.0f;
    float last_y_ = 0.0f;
};

class MenuPulseScheduler {
public:
    static constexpr uint64_t kDownDurationMs = 70;
    static constexpr uint64_t kGapDurationMs = 35;
    static constexpr size_t kMaxPendingPulses = 32;

    void Enqueue(const MenuTouchRecognizer::DiscretePulse& output, uint64_t now_ms) {
        if (output.mask == 0 || output.count == 0) return;
        const uint32_t available = static_cast<uint32_t>(queue_.size() - size_);
        const uint32_t accepted = std::min(output.count, available);
        dropped_count_ += output.count - accepted;
        for (uint32_t index = 0; index < accepted; ++index) {
            const size_t tail = (head_ + size_) % queue_.size();
            queue_[tail] = {output.mask, now_ms};
            ++size_;
        }
    }

    void Advance(uint64_t now_ms, DiscreteArbitrator& input) {
        if (active_mask_ != 0) {
            if (now_ms < next_transition_ms_) return;
            input.SetSynthetic(active_mask_, false);
            active_mask_ = 0;
            next_transition_ms_ = now_ms + kGapDurationMs;
            return;
        }
        if (size_ == 0 || now_ms < next_transition_ms_) {
            return;
        }
        const PendingPulse pulse = queue_[head_];
        head_ = (head_ + 1u) % queue_.size();
        --size_;
        active_mask_ = pulse.mask;
        input.SetSynthetic(active_mask_, true);
        next_transition_ms_ = now_ms + kDownDurationMs;
    }

    void Reset(DiscreteArbitrator& input) {
        input.ResetSynthetic();
        active_mask_ = 0;
        head_ = 0;
        size_ = 0;
        next_transition_ms_ = 0;
    }

    size_t PendingSize() const { return size_; }
    uint32_t ActiveMask() const { return active_mask_; }
    uint32_t DroppedCount() const { return dropped_count_; }

private:
    struct PendingPulse {
        uint32_t mask = 0;
        uint64_t ready_at_ms = 0;
    };

    std::array<PendingPulse, kMaxPendingPulses> queue_{};
    size_t head_ = 0;
    size_t size_ = 0;
    uint32_t active_mask_ = 0;
    uint64_t next_transition_ms_ = 0;
    uint32_t dropped_count_ = 0;
};

}  // namespace warband::input
