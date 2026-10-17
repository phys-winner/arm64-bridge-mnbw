#include "startup_frame_guard.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

void Check(bool value, const char* message) {
    if (!value) {
        std::cerr << "startup frame guard test failed: " << message << '\n';
        std::exit(1);
    }
}

}  // namespace

int main() {
    using namespace warband::startup_frame_guard;

    std::vector<std::uint8_t> clean(100u * 4u, 0u);
    for (std::size_t pixel = 0; pixel < 100u; ++pixel) {
        clean[pixel * 4 + 0] = 90u;
        clean[pixel * 4 + 1] = 120u;
        clean[pixel * 4 + 2] = 70u;
        clean[pixel * 4 + 3] = 255u;
    }
    const SampleClassification clean_sample =
        ClassifyRgbaStrip(clean.data(), clean.size() / 4u);
    Check(clean_sample.valid, "clean sample is valid");
    Check(!clean_sample.dark, "ordinary scene is not dark");
    Check(!clean_sample.font_atlas_artifact,
          "ordinary green terrain is not an atlas artifact");

    std::vector<std::uint8_t> atlas = clean;
    for (std::size_t pixel = 0; pixel < 6u; ++pixel) {
        atlas[pixel * 4 + 0] = 4u;
        atlas[pixel * 4 + 1] = 150u;
        atlas[pixel * 4 + 2] = 8u;
    }
    const SampleClassification atlas_sample =
        ClassifyRgbaStrip(atlas.data(), atlas.size() / 4u);
    Check(atlas_sample.font_atlas_artifact,
          "five-percent chroma coverage identifies the atlas");

    std::array<std::uint8_t, 16> black{};
    const SampleClassification dark_sample =
        ClassifyRgbaStrip(black.data(), black.size() / 4u);
    Check(dark_sample.dark, "clear frame is dark");

    State state{};
    Arm(state);
    Check(Decide(state, dark_sample) == Decision::kSuppress,
          "transition clear is withheld");
    Check(Decide(state, atlas_sample) == Decision::kSuppress,
          "atlas frame is withheld");
    Check(Decide(state, clean_sample) == Decision::kSuppress,
          "first clean frame confirms stability");
    Check(Decide(state, clean_sample) == Decision::kPresent,
          "second clean frame is presented");
    Check(!state.armed, "guard disarms after a stable frame");

    Arm(state);
    Check(Decide(state, clean_sample) == Decision::kPresent,
          "a clean transition without an artifact is not delayed");

    Arm(state);
    Check(Decide(state, dark_sample, 1u) == Decision::kSuppress,
          "timeout still withholds its first frame");
    Check(Decide(state, dark_sample, 1u) == Decision::kPresent,
          "timeout cannot freeze presentation");

    std::cout << "startup frame guard tests passed\n";
    return 0;
}
