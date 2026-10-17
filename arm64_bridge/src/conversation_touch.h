#pragma once

#include <cmath>
#include <cstdint>

namespace warband::input {

inline constexpr uint32_t kConversationWindowId = 12u;
inline constexpr uint32_t kInitialWindowId = 4u;
inline constexpr uint32_t kMenuWindowId = 18u;
inline constexpr uint32_t kConversationWindowSlotOffset = 0x34u;
inline constexpr uint32_t kConversationVtableImageOffset = 0x003D4DA8u;
inline constexpr uint32_t kConversationChoiceCountOffset = 0x1320u;
inline constexpr uint32_t kConversationObjectSize = 0x13A0u;
// Conversation_window and Menu_window both embed
// rglWidget_gamepad_controller at +0x140. Its first word is the desktop
// navigation widget; the green hover colour itself is a separate widget flag.
inline constexpr uint32_t kGamepadControllerOffset = 0x140u;
inline constexpr uint32_t kConversationGamepadControllerOffset =
    kGamepadControllerOffset;
// Menu_window::activate/check owns sixteen simple-button choice pointers in
// this exact contiguous range.
inline constexpr uint32_t kMenuChoiceFirstOffset = 0x1F8u;
inline constexpr uint32_t kMenuChoiceLastOffset = 0x234u;
// Initial_window owns the seven original main-menu buttons in the same
// simple-button format.  Highlighting only toggles their native state; each
// button keeps the colour/material supplied by the original UI (yellow).
inline constexpr uint32_t kInitialChoiceFirstOffset = 0x1F8u;
inline constexpr uint32_t kInitialChoiceLastOffset = 0x210u;
inline constexpr uint32_t kMaxContinueChoiceCount = 1u;

// Conversation_window::check performs desktop keyboard/gamepad focus-band
// scrolling immediately after its controller update.  Touch owns the
// scrollbar directly, so that navigation cursor must be suppressed both
// during the gesture and for one cleanup pass after release.
inline constexpr bool ShouldSuppressConversationNavigation(
        bool bridge_owns_conversation_touch, bool cleanup_pending) {
    return bridge_owns_conversation_touch || cleanup_pending;
}

// Keep visual touch feedback independent from the controller cursor that the
// desktop conversation code uses to auto-scroll.  A row stays highlighted
// only while a live response touch owns it; release/scrollbar cleanup passes
// explicitly clear the stock highlight.
inline constexpr uint32_t TouchHighlightWidget(
        bool owns_response_touch, bool release_pending, uint32_t widget) {
    return owns_response_touch && !release_pending ? widget : 0u;
}

// Full-screen Menu_window choices keep their native Android DOWN/UP stream.
// Their bridge state is visual-only and must coexist with menu swipe/tap
// recognition; every other generic UI mode owns its stream exclusively.
inline constexpr bool UiTouchBlocksMenuInput(bool active,
                                             bool visual_highlight_only) {
    return active && !visual_highlight_only;
}

// Continue-only screens can retain one non-selectable slot in the guest
// conversation object even though the answer panel is visually empty.
inline constexpr bool IsContinueChoiceCount(uint32_t choice_count) {
    return choice_count <= kMaxContinueChoiceCount;
}

// Native conv_sc_choices bounds from app/assets/Modules/Native/game_variables.txt:
// x=.01, y=.01, width=.965, height=.27. The game's y origin is bottom-left,
// while Android MotionEvent coordinates start at the top-left.
inline constexpr float kConversationChoiceAreaLeft = 0.01f;
inline constexpr float kConversationChoiceAreaRight = 0.975f;
inline constexpr float kConversationChoiceAreaTop = 0.72f;
inline constexpr float kConversationChoiceAreaBottom = 0.99f;

class ConversationTouchTarget final {
public:
    bool BeginTap(int32_t pointer_id, float x, float y,
                  int32_t surface_width, int32_t surface_height,
                  float touch_slop, bool continue_prompt_visible) {
        Cancel();
        if (!continue_prompt_visible || pointer_id < 0 ||
            surface_width <= 0 || surface_height <= 0 ||
            !std::isfinite(touch_slop) || touch_slop < 1.0f ||
            !InsideChoiceArea(x, y, surface_width, surface_height)) {
            return false;
        }

        tracking_ = true;
        pointer_id_ = pointer_id;
        down_x_ = x;
        down_y_ = y;
        touch_slop_ = touch_slop;
        return true;
    }

    void MoveTap(int32_t pointer_id, float x, float y,
                 int32_t surface_width, int32_t surface_height,
                 bool continue_prompt_visible) {
        if (!tracking_) return;
        if (!continue_prompt_visible || pointer_id != pointer_id_ ||
            !WithinSlop(x, y) ||
            !InsideChoiceArea(x, y, surface_width, surface_height)) {
            Cancel();
        }
    }

    bool EndTap(int32_t pointer_id, float x, float y,
                int32_t surface_width, int32_t surface_height,
                bool continue_prompt_visible) {
        const bool activate = tracking_ && continue_prompt_visible &&
            pointer_id == pointer_id_ && WithinSlop(x, y) &&
            InsideChoiceArea(x, y, surface_width, surface_height);
        Cancel();
        return activate;
    }

    void Cancel() {
        tracking_ = false;
        pointer_id_ = -1;
        down_x_ = 0.0f;
        down_y_ = 0.0f;
        touch_slop_ = 0.0f;
    }

    bool IsTracking() const { return tracking_; }

private:
    static bool InsideChoiceArea(float x, float y,
                                 int32_t surface_width,
                                 int32_t surface_height) {
        if (surface_width <= 0 || surface_height <= 0 ||
            !std::isfinite(x) || !std::isfinite(y)) {
            return false;
        }
        const float x_fraction = x / static_cast<float>(surface_width);
        const float y_fraction = y / static_cast<float>(surface_height);
        return x_fraction >= kConversationChoiceAreaLeft &&
               x_fraction <= kConversationChoiceAreaRight &&
               y_fraction >= kConversationChoiceAreaTop &&
               y_fraction <= kConversationChoiceAreaBottom;
    }

    bool WithinSlop(float x, float y) const {
        if (!std::isfinite(x) || !std::isfinite(y)) return false;
        const double dx = static_cast<double>(x) - static_cast<double>(down_x_);
        const double dy = static_cast<double>(y) - static_cast<double>(down_y_);
        const double slop = static_cast<double>(touch_slop_);
        return dx * dx + dy * dy <= slop * slop;
    }

    bool tracking_ = false;
    int32_t pointer_id_ = -1;
    float down_x_ = 0.0f;
    float down_y_ = 0.0f;
    float touch_slop_ = 0.0f;
};

}  // namespace warband::input
