#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

#include "touch_target_geometry.h"

namespace warband::input {

struct InventoryScrollbarSnapshot {
    TouchTargetRect track{};
    float relative_value = 0.0f;
    bool valid = false;
};

inline bool CaptureInventoryScrollbarSnapshot(
        const TouchTargetRect& track, float value, float minimum,
        float maximum, InventoryScrollbarSnapshot* snapshot) {
    if (!snapshot || !ValidTouchTargetRect(track) ||
        !std::isfinite(value) || !std::isfinite(minimum) ||
        !std::isfinite(maximum) || !(maximum > minimum)) {
        return false;
    }
    snapshot->track = track;
    snapshot->relative_value = std::clamp(
        (value - minimum) / (maximum - minimum), 0.0f, 1.0f);
    snapshot->valid = true;
    return true;
}

inline bool RestoreInventoryScrollbarValue(
        const InventoryScrollbarSnapshot& snapshot, float minimum,
        float maximum, float* value_out) {
    if (!value_out || !snapshot.valid ||
        !std::isfinite(snapshot.relative_value) ||
        !std::isfinite(minimum) || !std::isfinite(maximum) ||
        !(maximum > minimum)) {
        return false;
    }
    *value_out = minimum + std::clamp(
        snapshot.relative_value, 0.0f, 1.0f) * (maximum - minimum);
    return std::isfinite(*value_out);
}

inline float InventoryScrollTrackMatchScore(
        const TouchTargetRect& snapshot, const TouchTargetRect& candidate,
        float surface_width, float surface_height) {
    if (!ValidTouchTargetRect(snapshot) || !ValidTouchTargetRect(candidate) ||
        !std::isfinite(surface_width) || !std::isfinite(surface_height) ||
        surface_width <= 0.0f || surface_height <= 0.0f) {
        return std::numeric_limits<float>::infinity();
    }

    const float snapshot_width = snapshot.right - snapshot.left;
    const float snapshot_height = snapshot.bottom - snapshot.top;
    const float candidate_width = candidate.right - candidate.left;
    const float candidate_height = candidate.bottom - candidate.top;
    const float snapshot_x = (snapshot.left + snapshot.right) * 0.5f;
    const float snapshot_y = (snapshot.top + snapshot.bottom) * 0.5f;
    const float candidate_x = (candidate.left + candidate.right) * 0.5f;
    const float candidate_y = (candidate.top + candidate.bottom) * 0.5f;
    const float dx = std::fabs(snapshot_x - candidate_x);
    const float dy = std::fabs(snapshot_y - candidate_y);
    const float horizontal_limit = std::max(32.0f, surface_width * 0.08f);
    const float vertical_limit = std::max(32.0f, surface_height * 0.12f);
    const float height_ratio = candidate_height / snapshot_height;
    const float width_ratio = candidate_width / snapshot_width;
    if (dx > horizontal_limit || dy > vertical_limit ||
        height_ratio < 0.60f || height_ratio > 1.67f ||
        width_ratio < 0.35f || width_ratio > 2.85f) {
        return std::numeric_limits<float>::infinity();
    }

    return dx / horizontal_limit + dy / vertical_limit +
           std::fabs(candidate_height - snapshot_height) /
               std::max(snapshot_height, 1.0f) +
           0.25f * std::fabs(candidate_width - snapshot_width) /
               std::max(snapshot_width, 1.0f);
}

inline std::size_t FindInventoryScrollTrackMatch(
        const InventoryScrollbarSnapshot& snapshot,
        const TouchTargetRect* candidates, std::size_t count,
        const bool* claimed, float surface_width, float surface_height) {
    constexpr std::size_t kNone = std::numeric_limits<std::size_t>::max();
    if (!snapshot.valid || !candidates || count == 0u) return kNone;

    std::size_t best = kNone;
    float best_score = std::numeric_limits<float>::infinity();
    for (std::size_t index = 0; index < count; ++index) {
        if (claimed && claimed[index]) continue;
        const float score = InventoryScrollTrackMatchScore(
            snapshot.track, candidates[index], surface_width, surface_height);
        if (score < best_score) {
            best = index;
            best_score = score;
        }
    }
    return best;
}

}  // namespace warband::input
