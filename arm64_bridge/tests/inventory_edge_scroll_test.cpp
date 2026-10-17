#include "inventory_edge_scroll.h"

#include <cmath>
#include <cstdio>
#include <limits>

using warband::input::DecideInventoryEdgeScroll;
using warband::input::InventoryEdgeScrollGateState;
using warband::input::ShouldApplyInventoryEdgeScroll;
using warband::input::TouchTargetRect;

namespace {

struct TestState {
    int checks = 0;
    int failures = 0;

    void Check(bool condition, const char* expression, int line) {
        ++checks;
        if (!condition) {
            ++failures;
            std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
        }
    }
};

#define CHECK(state, expression) \
    (state).Check(static_cast<bool>(expression), #expression, __LINE__)

void TestBottomAndTopEdges(TestState& state) {
    const TouchTargetRect track{1880.0f, 80.0f, 1910.0f, 920.0f};
    const auto bottom = DecideInventoryEdgeScroll(
        &track, 1u, 1600.0f, 900.0f, 2424.0f, 1080.0f, 1.0f / 60.0f);
    CHECK(state, bottom);
    CHECK(state, bottom.scrollbar_index == 0u);
    CHECK(state, bottom.value_range_delta < 0.0f);

    const auto top = DecideInventoryEdgeScroll(
        &track, 1u, 1600.0f, 95.0f, 2424.0f, 1080.0f, 1.0f / 60.0f);
    CHECK(state, top);
    CHECK(state, top.value_range_delta > 0.0f);
}

void TestMiddleAndUnrelatedPaneAreIgnored(TestState& state) {
    const TouchTargetRect track{1880.0f, 80.0f, 1910.0f, 920.0f};
    CHECK(state, !DecideInventoryEdgeScroll(
        &track, 1u, 1600.0f, 500.0f, 2424.0f, 1080.0f, 1.0f / 60.0f));
    CHECK(state, !DecideInventoryEdgeScroll(
        &track, 1u, 200.0f, 900.0f, 2424.0f, 1080.0f, 1.0f / 60.0f));
    CHECK(state, !DecideInventoryEdgeScroll(
        &track, 1u, 1950.0f, 900.0f, 2424.0f, 1080.0f, 1.0f / 60.0f));
}

void TestOneSixthActivationZone(TestState& state) {
    const TouchTargetRect track{1880.0f, 80.0f, 1910.0f, 920.0f};
    // Height is 840, so each activation band is exactly 140 pixels:
    // [80, 220) at the top and (780, 920] at the bottom.
    const auto top_inside = DecideInventoryEdgeScroll(
        &track, 1u, 1600.0f, 210.0f, 2424.0f, 1080.0f, 1.0f / 60.0f);
    CHECK(state, top_inside);
    CHECK(state, top_inside.value_range_delta > 0.0f);
    CHECK(state, !DecideInventoryEdgeScroll(
        &track, 1u, 1600.0f, 221.0f, 2424.0f, 1080.0f,
        1.0f / 60.0f));

    const auto bottom_inside = DecideInventoryEdgeScroll(
        &track, 1u, 1600.0f, 790.0f, 2424.0f, 1080.0f, 1.0f / 60.0f);
    CHECK(state, bottom_inside);
    CHECK(state, bottom_inside.value_range_delta < 0.0f);
    CHECK(state, !DecideInventoryEdgeScroll(
        &track, 1u, 1600.0f, 779.0f, 2424.0f, 1080.0f,
        1.0f / 60.0f));
}

void TestSmallVerticalOvershootKeepsScrolling(TestState& state) {
    const TouchTargetRect track{1880.0f, 80.0f, 1910.0f, 920.0f};
    const auto above = DecideInventoryEdgeScroll(
        &track, 1u, 1600.0f, 35.0f, 2424.0f, 1080.0f, 1.0f / 60.0f);
    CHECK(state, above);
    CHECK(state, above.value_range_delta > 0.0f);

    const auto below = DecideInventoryEdgeScroll(
        &track, 1u, 1600.0f, 970.0f, 2424.0f, 1080.0f, 1.0f / 60.0f);
    CHECK(state, below);
    CHECK(state, below.value_range_delta < 0.0f);

    CHECK(state, !DecideInventoryEdgeScroll(
        &track, 1u, 1600.0f, 1075.0f, 2424.0f, 1080.0f, 1.0f / 60.0f));
}

void TestPickupInsideTopBandNeedsExitAndReentry(TestState& state) {
    const TouchTargetRect track{1880.0f, 80.0f, 1910.0f, 920.0f};
    InventoryEdgeScrollGateState gate{};
    auto decision = DecideInventoryEdgeScroll(
        &track, 1u, 1600.0f, 170.0f, 2424.0f, 1080.0f,
        1.0f / 60.0f);
    CHECK(state, decision);
    CHECK(state, !ShouldApplyInventoryEdgeScroll(&gate, decision));
    CHECK(state, !ShouldApplyInventoryEdgeScroll(&gate, decision));

    decision = DecideInventoryEdgeScroll(
        &track, 1u, 1600.0f, 130.0f, 2424.0f, 1080.0f,
        1.0f / 60.0f);
    CHECK(state, decision);
    // Moving farther into the same upper sixth must never arm the gate.
    CHECK(state, !ShouldApplyInventoryEdgeScroll(&gate, decision));
}

void TestPickupInsideBottomBandNeedsExitAndReentry(TestState& state) {
    const TouchTargetRect track{1880.0f, 80.0f, 1910.0f, 920.0f};
    InventoryEdgeScrollGateState gate{};
    auto decision = DecideInventoryEdgeScroll(
        &track, 1u, 1600.0f, 830.0f, 2424.0f, 1080.0f,
        1.0f / 60.0f);
    CHECK(state, decision);
    CHECK(state, !ShouldApplyInventoryEdgeScroll(&gate, decision));

    decision = DecideInventoryEdgeScroll(
        &track, 1u, 1600.0f, 900.0f, 2424.0f, 1080.0f,
        1.0f / 60.0f);
    CHECK(state, decision);
    CHECK(state, !ShouldApplyInventoryEdgeScroll(&gate, decision));
}

void TestLeavingStartingBandArmsReentry(TestState& state) {
    const TouchTargetRect track{1880.0f, 80.0f, 1910.0f, 920.0f};
    InventoryEdgeScrollGateState gate{};
    auto decision = DecideInventoryEdgeScroll(
        &track, 1u, 1600.0f, 170.0f, 2424.0f, 1080.0f,
        1.0f / 60.0f);
    CHECK(state, !ShouldApplyInventoryEdgeScroll(&gate, decision));

    decision = DecideInventoryEdgeScroll(
        &track, 1u, 1600.0f, 400.0f, 2424.0f, 1080.0f,
        1.0f / 60.0f);
    CHECK(state, !decision);
    CHECK(state, !ShouldApplyInventoryEdgeScroll(&gate, decision));

    decision = DecideInventoryEdgeScroll(
        &track, 1u, 1600.0f, 210.0f, 2424.0f, 1080.0f,
        1.0f / 60.0f);
    CHECK(state, decision);
    CHECK(state, ShouldApplyInventoryEdgeScroll(&gate, decision));
}

void TestMiddlePickupArmsExactLowerOneSixth(TestState& state) {
    const TouchTargetRect track{1880.0f, 80.0f, 1910.0f, 920.0f};
    InventoryEdgeScrollGateState gate{};
    auto decision = DecideInventoryEdgeScroll(
        &track, 1u, 1600.0f, 500.0f, 2424.0f, 1080.0f,
        1.0f / 60.0f);
    CHECK(state, !decision);
    CHECK(state, !ShouldApplyInventoryEdgeScroll(&gate, decision));

    // y=790 is inside the live lower one-sixth (780..920) even though it is
    // above the old screen-relative 75% broad-phase boundary (810).
    decision = DecideInventoryEdgeScroll(
        &track, 1u, 1600.0f, 790.0f, 2424.0f, 1080.0f,
        1.0f / 60.0f);
    CHECK(state, decision);
    CHECK(state, decision.value_range_delta < 0.0f);
    CHECK(state, ShouldApplyInventoryEdgeScroll(&gate, decision));
}

void TestNearestRightHandPaneWins(TestState& state) {
    const TouchTargetRect tracks[] = {
        {800.0f, 80.0f, 830.0f, 920.0f},
        {1880.0f, 80.0f, 1910.0f, 920.0f},
    };
    const auto decision = DecideInventoryEdgeScroll(
        tracks, 2u, 1500.0f, 900.0f, 2424.0f, 1080.0f, 1.0f / 60.0f);
    CHECK(state, decision);
    CHECK(state, decision.scrollbar_index == 1u);
}

void TestRejectsInvalidInput(TestState& state) {
    const TouchTargetRect track{1880.0f, 80.0f, 1910.0f, 920.0f};
    CHECK(state, !DecideInventoryEdgeScroll(
        nullptr, 1u, 1600.0f, 900.0f, 2424.0f, 1080.0f, 0.016f));
    CHECK(state, !DecideInventoryEdgeScroll(
        &track, 1u, 1600.0f, 900.0f, 2424.0f, 1080.0f, 0.0f));
    CHECK(state, !DecideInventoryEdgeScroll(
        &track, 1u, std::numeric_limits<float>::quiet_NaN(), 900.0f,
        2424.0f, 1080.0f, 0.016f));
}

}  // namespace

int main() {
    TestState state;
    TestBottomAndTopEdges(state);
    TestMiddleAndUnrelatedPaneAreIgnored(state);
    TestOneSixthActivationZone(state);
    TestSmallVerticalOvershootKeepsScrolling(state);
    TestPickupInsideTopBandNeedsExitAndReentry(state);
    TestPickupInsideBottomBandNeedsExitAndReentry(state);
    TestLeavingStartingBandArmsReentry(state);
    TestMiddlePickupArmsExactLowerOneSixth(state);
    TestNearestRightHandPaneWins(state);
    TestRejectsInvalidInput(state);
    if (state.failures != 0) {
        std::fprintf(stderr, "%d/%d checks failed\n",
                     state.failures, state.checks);
        return 1;
    }
    std::printf("%d checks passed\n", state.checks);
    return 0;
}
