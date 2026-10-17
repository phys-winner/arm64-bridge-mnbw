#pragma once

#include <algorithm>
#include <cstdint>
#include <cmath>

namespace warband::input {

// Most UI interception is disabled while a mission is active so it cannot
// steal tactical input.  Conversation_window is the one deliberate exception:
// dialogs shown inside a castle keep the mission alive, and their long answer
// list otherwise receives the stock Android DOWN/UP path that rebuilds the
// list at row zero.
inline bool ShouldInspectUiScrollbars(bool gameplay_state_known,
                                      bool gameplay_active,
                                      bool conversation_window) {
    return gameplay_state_known &&
           (!gameplay_active || conversation_window);
}

// Conversation answers are wide, shallow buttons in the lower half of the
// screen.  Keep this predicate independent of guest addresses so the JNI
// bridge can safely distinguish them from unrelated mission buttons while the
// castle scene remains active behind the dialog.
inline bool IsConversationResponseGeometry(float width, float height,
                                             float centre_y,
                                             float surface_width,
                                             float surface_height) {
    return std::isfinite(width) && std::isfinite(height) &&
           std::isfinite(centre_y) && std::isfinite(surface_width) &&
           std::isfinite(surface_height) && surface_width > 0.0f &&
           surface_height > 0.0f && width >= surface_width * 0.35f &&
           width <= surface_width * 0.80f && height >= 3.0f &&
           height <= surface_height * 0.12f &&
           centre_y >= surface_height * 0.50f &&
           centre_y <= surface_height * 0.99f;
}

// A content drag is opposite to a scrollbar-thumb drag: moving the finger up
// must move the thumb down.  Mirror the pointer around its DOWN position and
// reuse the same clamped scrollbar model for both gestures.
inline float MirroredContentPointerY(float pointer_start_y, float pointer_y) {
    return pointer_start_y + (pointer_start_y - pointer_y);
}

// A captured desktop scrollbar must not receive the Android DOWN/UP stream:
// several game windows rebuild their rows on those events and reset the
// scrollbar to its first item.  The bridge instead keeps the value at DOWN and
// applies the finger displacement directly to the live normalized range.
struct ScrollbarDragModel {
    float pointer_start_y = 0.0f;
    float track_top = 0.0f;
    float track_bottom = 0.0f;
    float value_start = 0.0f;
    float minimum = 0.0f;
    float maximum = 0.0f;
};

// Dragging list content is not the same operation as dragging the scrollbar
// thumb.  A content gesture must move the document one screen pixel for every
// finger pixel; mapping it across the short thumb track makes long lists jump
// several rows at once.  Store the scrollable document extent in screen pixels
// so this policy does not depend on the guest's normalized/aspect-scaled widget
// coordinate system.
struct ScrollableContentDragModel {
    float pointer_start_y = 0.0f;
    float content_extent_pixels = 0.0f;
    float value_start = 0.0f;
    float minimum = 0.0f;
    float maximum = 0.0f;
};

// MotionEvent UP can carry a slightly different coordinate from the last
// MOVE sample that was actually rendered under the finger.  Re-applying the
// drag model from UP therefore makes a released list jump by one or two rows.
// Commit document motion only from MOVE; release merely ends the gesture and
// retains the last visible value.
inline bool ShouldApplyScrollableContentDragSample(bool is_move,
                                                    bool tap_eligible) {
    return is_move && !tap_eligible;
}

inline bool EstimateContentExtentPixels(float row_pitch_pixels,
                                        uint32_t visible_row_count,
                                        uint32_t total_row_count,
                                        float* extent_out) {
    if (!extent_out || !std::isfinite(row_pitch_pixels) ||
        !(row_pitch_pixels > 0.0f) || visible_row_count == 0u ||
        total_row_count <= visible_row_count) {
        return false;
    }
    const float extent = row_pitch_pixels *
        static_cast<float>(total_row_count - visible_row_count);
    if (!std::isfinite(extent) || !(extent > 0.0f)) return false;
    *extent_out = extent;
    return true;
}

inline bool ValidScrollableContentDragModel(
        const ScrollableContentDragModel& model) {
    return std::isfinite(model.pointer_start_y) &&
           std::isfinite(model.content_extent_pixels) &&
           std::isfinite(model.value_start) &&
           std::isfinite(model.minimum) &&
           std::isfinite(model.maximum) &&
           model.content_extent_pixels > 0.0f &&
           model.maximum > model.minimum &&
           model.value_start >= model.minimum &&
           model.value_start <= model.maximum;
}

inline bool ScrollbarValueForContentPointer(
        const ScrollableContentDragModel& model, float pointer_y,
        float* value_out) {
    if (!value_out || !ValidScrollableContentDragModel(model) ||
        !std::isfinite(pointer_y)) {
        return false;
    }
    const float value_range = model.maximum - model.minimum;
    const float pixel_denominator = model.content_extent_pixels;
    if (!std::isfinite(pixel_denominator) || !(pixel_denominator > 0.0f)) {
        return false;
    }
    const float content_delta =
        (pointer_y - model.pointer_start_y) / pixel_denominator;
    *value_out = std::clamp(
        model.value_start + content_delta * value_range,
        model.minimum, model.maximum);
    return std::isfinite(*value_out);
}

inline bool ValidScrollbarDragModel(const ScrollbarDragModel& model) {
    return std::isfinite(model.pointer_start_y) &&
           std::isfinite(model.track_top) &&
           std::isfinite(model.track_bottom) &&
           std::isfinite(model.value_start) &&
           std::isfinite(model.minimum) &&
           std::isfinite(model.maximum) &&
           model.track_bottom > model.track_top &&
           model.maximum > model.minimum &&
           model.value_start >= model.minimum &&
           model.value_start <= model.maximum;
}

inline bool ScrollbarValueForPointer(const ScrollbarDragModel& model,
                                     float pointer_y, float* value_out) {
    if (!value_out || !ValidScrollbarDragModel(model) ||
        !std::isfinite(pointer_y)) {
        return false;
    }
    const float track_height = model.track_bottom - model.track_top;
    if (!(track_height > 0.0f)) return false;
    const float value_range = model.maximum - model.minimum;
    // Guest widget coordinates are bottom-origin: the top of a vertical
    // scrollbar is maximum and dragging a finger down must reduce its value.
    const float delta = (pointer_y - model.pointer_start_y) / track_height;
    *value_out = std::clamp(model.value_start - delta * value_range,
                            model.minimum, model.maximum);
    return std::isfinite(*value_out);
}

}  // namespace warband::input
