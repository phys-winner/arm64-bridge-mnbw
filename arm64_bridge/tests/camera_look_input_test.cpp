#include "camera_look_input.h"

#include <cmath>
#include <cstdio>

namespace {

struct TestState {
    int checks = 0;
    int failures = 0;
};

#define CHECK(state, condition)                                                   \
    do {                                                                          \
        ++(state).checks;                                                         \
        if (!(condition)) {                                                       \
            ++(state).failures;                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,       \
                         #condition);                                             \
        }                                                                         \
    } while (false)

bool Near(float left, float right, float epsilon = 1.0e-6f) {
    return std::fabs(left - right) <= epsilon;
}

void TestStickAxisTimeIsFrameRateIndependent(TestState& state) {
    using namespace warband::camera_look;
    constexpr float look_delta = 8.0f;
    constexpr float reference_frame = 1.0f / 60.0f;
    const float reference_contribution =
        NormalizedStickAxis(look_delta, reference_frame) * reference_frame;
    for (const float frame_seconds : {1.0f / 30.0f, 1.0f / 60.0f, 1.0f / 90.0f}) {
        const float contribution =
            NormalizedStickAxis(look_delta, frame_seconds) * frame_seconds;
        CHECK(state, Near(contribution, reference_contribution));
    }
}

void TestStickAxisBoundsAndInvalidFrames(TestState& state) {
    using namespace warband::camera_look;
    CHECK(state, NormalizedStickAxis(10000.0f, 1.0f / 90.0f) == 1.0f);
    CHECK(state, NormalizedStickAxis(-10000.0f, 1.0f / 90.0f) == -1.0f);
    CHECK(state, NormalizedStickAxis(12.0f, 0.0f) == 0.0f);
    CHECK(state, NormalizedStickAxis(12.0f, NAN) == 0.0f);
}

void TestMouseLookUsesSixteenthReferenceStickGain(TestState& state) {
    using namespace warband::camera_look;
    for (const float look_delta : {-60.0f, -1.0f, 0.0f, 1.0f, 60.0f}) {
        const float mouse_delta = ScaledRelativeMouseDelta(look_delta);
        const float stick_axis = NormalizedStickAxis(
            look_delta, kReferenceFrameSeconds);
        CHECK(state, Near(mouse_delta, stick_axis / 16.0f));
    }
    CHECK(state, ScaledRelativeMouseDelta(NAN) == 0.0f);
    CHECK(state, ScaledRelativeMouseDelta(100000.0f) == 1.0f);
}

void TestRelativeMouseQuantizationKeepsFractions(TestState& state) {
    using namespace warband::camera_look;
    float remainder = 0.0f;
    float emitted_total = 0.0f;
    for (int index = 0; index < 3; ++index) {
        const RelativeLookDelta delta = QuantizeRelativeDelta(0.4f, remainder);
        emitted_total += delta.emitted;
        remainder = delta.remainder;
    }
    CHECK(state, emitted_total == 1.0f);
    CHECK(state, Near(remainder, 0.2f));

    const RelativeLookDelta negative = QuantizeRelativeDelta(-1.25f, 0.0f);
    CHECK(state, negative.emitted == -1.0f);
    CHECK(state, Near(negative.remainder, -0.25f));
    const RelativeLookDelta bounded = QuantizeRelativeDelta(1000.0f, 0.0f);
    CHECK(state, bounded.emitted == kMaxRelativeLookPerFrame);
    CHECK(state, bounded.remainder == 0.0f);
}

}  // namespace

int main() {
    TestState state;
    TestStickAxisTimeIsFrameRateIndependent(state);
    TestStickAxisBoundsAndInvalidFrames(state);
    TestMouseLookUsesSixteenthReferenceStickGain(state);
    TestRelativeMouseQuantizationKeepsFractions(state);
    std::printf("camera_look_input_test: %s (%d checks)\n",
                state.failures ? "FAIL" : "PASS", state.checks);
    return state.failures ? 1 : 0;
}
