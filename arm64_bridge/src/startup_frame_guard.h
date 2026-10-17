#pragma once

#include <cstddef>
#include <cstdint>

namespace warband::startup_frame_guard {

// The legacy quick-battle window submits one or more transient HUD buffers
// immediately after a synchronous scene load.  Their diffuse texture is the
// 2048x1024 font atlas, but their dynamic vertex/style state still describes
// the previous window.  Modern buffer queues can expose those intermediate
// frames; the original Tegra path did not.  The atlas has a very distinctive
// chroma payload (large pure-green regions), so inspect a tiny strip of the
// completed back buffer and withhold only frames carrying that signature.
struct SampleClassification {
    bool valid = false;
    bool dark = false;
    bool font_atlas_artifact = false;
    std::size_t sampled_pixels = 0;
    std::size_t artifact_pixels = 0;
};

inline SampleClassification ClassifyRgbaStrip(const std::uint8_t* rgba,
                                               std::size_t pixel_count) {
    SampleClassification result{};
    if (!rgba || pixel_count == 0) return result;

    std::uint64_t luma_sum = 0;
    std::size_t artifact_pixels = 0;
    for (std::size_t pixel = 0; pixel < pixel_count; ++pixel) {
        const std::uint8_t red = rgba[pixel * 4 + 0];
        const std::uint8_t green = rgba[pixel * 4 + 1];
        const std::uint8_t blue = rgba[pixel * 4 + 2];
        luma_sum += static_cast<std::uint64_t>(red) * 54u +
                    static_cast<std::uint64_t>(green) * 183u +
                    static_cast<std::uint64_t>(blue) * 19u;

        // This deliberately requires both a ratio and an absolute separation.
        // Ordinary grass and green team labels stay well below the aggregate
        // threshold, while a full font atlas covers 15-35% of the sample.
        if (green >= 70u &&
            static_cast<unsigned>(green) >= static_cast<unsigned>(red) * 2u &&
            static_cast<unsigned>(green) >= static_cast<unsigned>(blue) * 2u &&
            static_cast<unsigned>(green) >= static_cast<unsigned>(red) + 30u &&
            static_cast<unsigned>(green) >= static_cast<unsigned>(blue) + 30u) {
            ++artifact_pixels;
        }
    }

    result.valid = true;
    result.sampled_pixels = pixel_count;
    result.artifact_pixels = artifact_pixels;
    // Weighted luma above is scaled by 256.  An average below eight is a
    // transition/clear frame, not a useful first scene frame.
    result.dark = luma_sum < static_cast<std::uint64_t>(pixel_count) * 8u * 256u;
    result.font_atlas_artifact = artifact_pixels * 20u >= pixel_count;
    return result;
}

struct State {
    bool armed = false;
    bool saw_artifact = false;
    unsigned suppressed_frames = 0;
    unsigned consecutive_clean_frames = 0;
};

enum class Decision {
    kPresent,
    kSuppress,
};

inline void Arm(State& state) {
    state = State{};
    state.armed = true;
}

inline Decision Decide(State& state, const SampleClassification& sample,
                       unsigned maximum_suppressed_frames = 180u) {
    if (!state.armed || !sample.valid) {
        state.armed = false;
        return Decision::kPresent;
    }
    if (state.suppressed_frames >= maximum_suppressed_frames) {
        state.armed = false;
        return Decision::kPresent;
    }

    if (sample.dark) {
        ++state.suppressed_frames;
        state.consecutive_clean_frames = 0;
        return Decision::kSuppress;
    }
    if (sample.font_atlas_artifact) {
        state.saw_artifact = true;
        ++state.suppressed_frames;
        state.consecutive_clean_frames = 0;
        return Decision::kSuppress;
    }

    if (state.saw_artifact && state.consecutive_clean_frames++ == 0u) {
        ++state.suppressed_frames;
        return Decision::kSuppress;
    }

    state.armed = false;
    return Decision::kPresent;
}

}  // namespace warband::startup_frame_guard
