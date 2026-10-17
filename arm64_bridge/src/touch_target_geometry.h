#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace warband::input {

struct TouchTargetRect {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
};

struct TouchTargetSelection {
    static constexpr std::size_t kNone = std::numeric_limits<std::size_t>::max();

    std::size_t index = kNone;
    float snap_x = 0.0f;
    float snap_y = 0.0f;
    bool point_was_inside = false;

    explicit operator bool() const { return index != kNone; }
};

inline bool ValidTouchTargetRect(const TouchTargetRect& rect) {
    return std::isfinite(rect.left) && std::isfinite(rect.top) &&
           std::isfinite(rect.right) && std::isfinite(rect.bottom) &&
           rect.right > rect.left && rect.bottom > rect.top;
}

inline bool TouchTargetContains(const TouchTargetRect& rect, float x, float y) {
    return ValidTouchTargetRect(rect) && std::isfinite(x) && std::isfinite(y) &&
           x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
}

// Enlarged targets may geometrically meet when the original desktop UI packs
// rows more tightly than a finger.  Selection is nevertheless exclusive: an
// original hit always wins, otherwise the nearest visible centre owns the
// point, with source order as a deterministic boundary tie-break.
inline TouchTargetSelection SelectExpandedTouchTarget(
        const TouchTargetRect* targets, std::size_t count, float x, float y,
        float padding_x, float padding_y, float surface_width,
        float surface_height, const TouchTargetRect* clips = nullptr) {
    TouchTargetSelection best{};
    if (!targets || count == 0u || !std::isfinite(x) || !std::isfinite(y) ||
        !std::isfinite(padding_x) || !std::isfinite(padding_y) ||
        !std::isfinite(surface_width) || !std::isfinite(surface_height) ||
        padding_x < 0.0f || padding_y < 0.0f || surface_width <= 0.0f ||
        surface_height <= 0.0f || x < 0.0f || x >= surface_width || y < 0.0f ||
        y >= surface_height) {
        return best;
    }

    bool best_inside = false;
    float best_distance_squared = std::numeric_limits<float>::infinity();
    for (std::size_t index = 0; index < count; ++index) {
        const TouchTargetRect& target = targets[index];
        if (!ValidTouchTargetRect(target)) continue;
        if (clips && !TouchTargetContains(clips[index], x, y)) continue;

        const float expanded_left = std::max(
            clips ? clips[index].left : 0.0f, target.left - padding_x);
        const float expanded_top = std::max(
            clips ? clips[index].top : 0.0f, target.top - padding_y);
        const float expanded_right = std::min(
            clips ? clips[index].right : surface_width, target.right + padding_x);
        const float expanded_bottom = std::min(
            clips ? clips[index].bottom : surface_height, target.bottom + padding_y);
        if (x < expanded_left || x >= expanded_right || y < expanded_top ||
            y >= expanded_bottom) {
            continue;
        }

        const bool inside = TouchTargetContains(target, x, y);
        const float centre_x = target.left + (target.right - target.left) * 0.5f;
        const float centre_y = target.top + (target.bottom - target.top) * 0.5f;
        const float dx = x - centre_x;
        const float dy = y - centre_y;
        const float distance_squared = dx * dx + dy * dy;
        if (!best || (inside && !best_inside) ||
            (inside == best_inside && distance_squared < best_distance_squared)) {
            best.index = index;
            best.snap_x = centre_x;
            best.snap_y = centre_y;
            best.point_was_inside = inside;
            best_inside = inside;
            best_distance_squared = distance_squared;
        }
    }
    return best;
}

// Bottom navigation buttons in the original 4:3 UI are visually small and
// sit directly above a map-sensitive surface.  Their touch target is doubled
// upward only: the visible rectangle is unchanged, neighbouring buttons keep
// their original horizontal ownership, and an original hit still wins over an
// expanded neighbour.
inline TouchTargetSelection SelectUpwardDoubledTouchTarget(
        const TouchTargetRect* targets, std::size_t count, float x, float y,
        float surface_width, float surface_height,
        const TouchTargetRect* clips = nullptr) {
    TouchTargetSelection best{};
    if (!targets || count == 0u || !std::isfinite(x) || !std::isfinite(y) ||
        !std::isfinite(surface_width) || !std::isfinite(surface_height) ||
        surface_width <= 0.0f || surface_height <= 0.0f || x < 0.0f ||
        x >= surface_width || y < 0.0f || y >= surface_height) {
        return best;
    }

    bool best_inside = false;
    float best_distance_squared = std::numeric_limits<float>::infinity();
    for (std::size_t index = 0; index < count; ++index) {
        const TouchTargetRect& target = targets[index];
        if (!ValidTouchTargetRect(target)) continue;
        if (clips && !TouchTargetContains(clips[index], x, y)) continue;

        const float height = target.bottom - target.top;
        const float expanded_top = std::max(
            clips ? clips[index].top : 0.0f, target.top - height);
        const float expanded_bottom = std::min(
            clips ? clips[index].bottom : surface_height, target.bottom);
        if (x < target.left || x >= target.right || y < expanded_top ||
            y >= expanded_bottom) {
            continue;
        }

        const bool inside = TouchTargetContains(target, x, y);
        const float centre_x = target.left + (target.right - target.left) * 0.5f;
        const float centre_y = target.top + height * 0.5f;
        const float dx = x - centre_x;
        const float dy = y - centre_y;
        const float distance_squared = dx * dx + dy * dy;
        if (!best || (inside && !best_inside) ||
            (inside == best_inside && distance_squared < best_distance_squared)) {
            best.index = index;
            best.snap_x = centre_x;
            best.snap_y = centre_y;
            best.point_was_inside = inside;
            best_inside = inside;
            best_distance_squared = distance_squared;
        }
    }
    return best;
}

// Thin vertical scrollbars sit on the right edge of their content pane.  Grow
// only into the free space to their right so the final row/column of content
// never becomes an accidental scrollbar grab.
inline TouchTargetSelection SelectRightExpandedTouchTarget(
        const TouchTargetRect* targets, std::size_t count, float x, float y,
        float padding_right, float surface_width, float surface_height,
        const TouchTargetRect* clips = nullptr) {
    TouchTargetSelection best{};
    if (!targets || count == 0u || !std::isfinite(x) || !std::isfinite(y) ||
        !std::isfinite(padding_right) || padding_right < 0.0f ||
        !std::isfinite(surface_width) || !std::isfinite(surface_height) ||
        surface_width <= 0.0f || surface_height <= 0.0f || x < 0.0f ||
        x >= surface_width || y < 0.0f || y >= surface_height) {
        return best;
    }

    bool best_inside = false;
    float best_distance_squared = std::numeric_limits<float>::infinity();
    for (std::size_t index = 0; index < count; ++index) {
        const TouchTargetRect& target = targets[index];
        if (!ValidTouchTargetRect(target)) continue;
        if (clips && !TouchTargetContains(clips[index], x, y)) continue;
        const float expanded_right = std::min(
            clips ? clips[index].right : surface_width,
            target.right + padding_right);
        if (x < target.left || x >= expanded_right || y < target.top ||
            y >= target.bottom) {
            continue;
        }
        const bool inside = TouchTargetContains(target, x, y);
        const float centre_x = target.left + (target.right - target.left) * 0.5f;
        const float centre_y = target.top + (target.bottom - target.top) * 0.5f;
        const float dx = x - centre_x;
        const float dy = y - centre_y;
        const float distance_squared = dx * dx + dy * dy;
        if (!best || (inside && !best_inside) ||
            (inside == best_inside && distance_squared < best_distance_squared)) {
            best.index = index;
            best.snap_x = centre_x;
            best.snap_y = centre_y;
            best.point_was_inside = inside;
            best_inside = inside;
            best_distance_squared = distance_squared;
        }
    }
    return best;
}

// rglWindow_manager stores a normalized, bottom-origin cursor.  On wide
// surfaces is_over_widget applies an additional centre-preserving aspect
// transform.  Keeping the inverse here makes a native pixel snap land at the
// exact visual centre without changing guest widget geometry.
struct WidgetCoordinateTransform {
    float surface_width = 0.0f;
    float surface_height = 0.0f;
    float coordinate_scale = 0.0f;
    float x_factor = 1.0f;
    float y_factor = 1.0f;

    bool Valid() const {
        return std::isfinite(surface_width) && std::isfinite(surface_height) &&
               std::isfinite(coordinate_scale) && std::isfinite(x_factor) &&
               std::isfinite(y_factor) && surface_width > 0.0f &&
               surface_height > 0.0f && coordinate_scale > 0.0f &&
               x_factor > 0.0f && y_factor > 0.0f;
    }

    float WidgetXFromScreen(float screen_x) const {
        if (!Valid() || !std::isfinite(screen_x)) {
            return std::numeric_limits<float>::quiet_NaN();
        }
        const float cursor_x = screen_x / surface_width;
        return x_factor * (cursor_x - 0.5f) + 0.5f;
    }

    float WidgetYFromScreen(float screen_y) const {
        if (!Valid() || !std::isfinite(screen_y)) {
            return std::numeric_limits<float>::quiet_NaN();
        }
        const float cursor_y = (1.0f - screen_y / surface_height) * coordinate_scale;
        return y_factor * (cursor_y - 0.375f) + 0.375f;
    }

    float ScreenXFromWidget(float widget_x) const {
        if (!Valid() || !std::isfinite(widget_x)) {
            return std::numeric_limits<float>::quiet_NaN();
        }
        const float cursor_x = (widget_x - 0.5f) / x_factor + 0.5f;
        return cursor_x * surface_width;
    }

    float ScreenYFromWidget(float widget_y) const {
        if (!Valid() || !std::isfinite(widget_y)) {
            return std::numeric_limits<float>::quiet_NaN();
        }
        const float cursor_y = (widget_y - 0.375f) / y_factor + 0.375f;
        return (1.0f - cursor_y / coordinate_scale) * surface_height;
    }
};

inline TouchTargetRect WidgetRectToScreen(const WidgetCoordinateTransform& transform,
                                          float left, float bottom, float right,
                                          float top) {
    return {
        transform.ScreenXFromWidget(left),
        transform.ScreenYFromWidget(top),
        transform.ScreenXFromWidget(right),
        transform.ScreenYFromWidget(bottom),
    };
}

}  // namespace warband::input
