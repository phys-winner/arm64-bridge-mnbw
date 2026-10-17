#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

#include "touch_target_geometry.h"

namespace warband::input {

struct InventoryEdgeScrollDecision {
    static constexpr std::size_t kNone =
        std::numeric_limits<std::size_t>::max();

    std::size_t scrollbar_index = kNone;
    // Fraction of the scrollbar's full guest range to add this frame.
    // Vertical guest scrollbars are bottom-origin: positive reveals earlier
    // rows, negative reveals later rows.
    float value_range_delta = 0.0f;

    explicit operator bool() const {
        return scrollbar_index != kNone && value_range_delta != 0.0f;
    }
};

// A pickup that begins in an edge row must not start scrolling immediately:
// that would turn the ordinary act of taking an item into a jump toward the
// end of the list.  Such a gesture is armed only after a frame is observed
// outside every edge band; moving farther inside the starting band is never
// enough.  Pickups that begin in the middle are armed from the outset.
struct InventoryEdgeScrollGateState {
    bool initialized = false;
    bool armed = false;
};

// While an inventory item is held, moving it into the top/bottom edge of its
// pane scrolls that pane.  This replaces the desktop-only two-pointer trick
// and, importantly, keeps the item gesture bridge-owned while rows rebuild.
inline InventoryEdgeScrollDecision DecideInventoryEdgeScroll(
        const TouchTargetRect* scrollbar_tracks, std::size_t count,
        float pointer_x, float pointer_y, float surface_width,
        float surface_height, float frame_seconds) {
    InventoryEdgeScrollDecision best{};
    if (!scrollbar_tracks || count == 0u || !std::isfinite(pointer_x) ||
        !std::isfinite(pointer_y) || !std::isfinite(surface_width) ||
        !std::isfinite(surface_height) || !std::isfinite(frame_seconds) ||
        surface_width <= 0.0f || surface_height <= 0.0f ||
        frame_seconds <= 0.0f || pointer_x < 0.0f ||
        pointer_x >= surface_width || pointer_y < 0.0f ||
        pointer_y >= surface_height) {
        return best;
    }

    float best_horizontal_distance = std::numeric_limits<float>::infinity();
    for (std::size_t index = 0; index < count; ++index) {
        const TouchTargetRect& track = scrollbar_tracks[index];
        if (!ValidTouchTargetRect(track)) {
            continue;
        }

        const float track_height = track.bottom - track.top;
        // An inventory pane sits immediately to the left of its scrollbar.
        // The bound is deliberately relative to the live track height so it
        // follows both phone aspect ratios and modded inventory layouts.
        const float pane_reach = std::clamp(
            track_height * 0.78f, surface_width * 0.16f,
            surface_width * 0.42f);
        const float horizontal_distance = track.left - pointer_x;
        if (horizontal_distance < -std::max(8.0f, track.right - track.left) ||
            horizontal_distance > pane_reach) {
            continue;
        }

        // The requested activation band is exactly the outer one-sixth of the
        // live list viewport.  Do not cap it in pixels: that silently shrinks
        // the band on high-resolution/large-layout devices.
        const float edge_size = track_height / 6.0f;
        // Keep scrolling when the held icon slightly overshoots the visible
        // viewport. A finger at the physical screen edge otherwise falls just
        // outside the scrollbar rectangle and disables the feature precisely
        // where the user expects its strongest response.
        if (pointer_y < track.top - edge_size ||
            pointer_y > track.bottom + edge_size) {
            continue;
        }
        float direction = 0.0f;
        float intensity = 0.0f;
        if (pointer_y < track.top + edge_size) {
            direction = 1.0f;
            intensity = (track.top + edge_size - pointer_y) / edge_size;
        } else if (pointer_y > track.bottom - edge_size) {
            direction = -1.0f;
            intensity = (pointer_y - (track.bottom - edge_size)) / edge_size;
        }
        if (direction == 0.0f) continue;

        // Reach full range in roughly 1.35 seconds at the outer edge. Clamp a
        // stalled frame so resuming the app cannot jump the whole inventory.
        const float safe_seconds = std::clamp(frame_seconds, 0.0f, 0.05f);
        const float delta = direction * std::clamp(intensity, 0.0f, 1.0f) *
                            safe_seconds * 0.74f;
        if (delta == 0.0f ||
            horizontal_distance >= best_horizontal_distance) {
            continue;
        }
        best.scrollbar_index = index;
        best.value_range_delta = delta;
        best_horizontal_distance = horizontal_distance;
    }
    return best;
}

inline bool ShouldApplyInventoryEdgeScroll(
        InventoryEdgeScrollGateState* state,
        const InventoryEdgeScrollDecision& decision) {
    if (!state) return false;

    if (!state->initialized) {
        state->initialized = true;
        state->armed = !decision;
        // Never scroll on the first held frame.  This keeps pickup itself
        // position-preserving even when the source row lies in an edge band.
        return false;
    }

    if (state->armed) return static_cast<bool>(decision);
    if (!decision) {
        // The finger left its starting edge band.  A later re-entry is an
        // explicit request to scroll.
        state->armed = true;
    }
    return false;
}

}  // namespace warband::input
