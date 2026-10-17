#include "touch_target_geometry.h"

#include <cmath>
#include <cstdio>
#include <limits>

using warband::input::SelectExpandedTouchTarget;
using warband::input::SelectRightExpandedTouchTarget;
using warband::input::SelectUpwardDoubledTouchTarget;
using warband::input::TouchTargetRect;
using warband::input::WidgetCoordinateTransform;
using warband::input::WidgetRectToScreen;

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

void CheckClose(TestState& state, float actual, float expected,
                float epsilon = 0.001f) {
    CHECK(state, std::fabs(actual - expected) <= epsilon);
}

void TestOriginalHitWinsExpandedNeighbour(TestState& state) {
    const TouchTargetRect targets[] = {
        {90.0f, 90.0f, 110.0f, 110.0f},
        {90.0f, 114.0f, 110.0f, 134.0f},
    };
    const auto selected = SelectExpandedTouchTarget(
        targets, 2u, 100.0f, 115.0f, 16.0f, 16.0f, 200.0f, 200.0f);
    CHECK(state, selected);
    CHECK(state, selected.index == 1u);
    CHECK(state, selected.point_was_inside);
    CheckClose(state, selected.snap_y, 124.0f);
}

void TestPackedTargetsUseExclusiveNearestCentre(TestState& state) {
    const TouchTargetRect targets[] = {
        {90.0f, 90.0f, 110.0f, 106.0f},
        {90.0f, 110.0f, 110.0f, 126.0f},
    };
    const auto upper = SelectExpandedTouchTarget(
        targets, 2u, 100.0f, 107.0f, 20.0f, 20.0f, 200.0f, 200.0f);
    const auto lower = SelectExpandedTouchTarget(
        targets, 2u, 100.0f, 109.0f, 20.0f, 20.0f, 200.0f, 200.0f);
    const auto exact_boundary = SelectExpandedTouchTarget(
        targets, 2u, 100.0f, 108.0f, 20.0f, 20.0f, 200.0f, 200.0f);
    CHECK(state, upper && upper.index == 0u && !upper.point_was_inside);
    CHECK(state, lower && lower.index == 1u && !lower.point_was_inside);
    CHECK(state, exact_boundary && exact_boundary.index == 0u);
}

void TestExpansionIsInvisibleAndSurfaceClipped(TestState& state) {
    const TouchTargetRect target{2.0f, 2.0f, 22.0f, 22.0f};
    const auto expanded = SelectExpandedTouchTarget(
        &target, 1u, 25.0f, 10.0f, 8.0f, 8.0f, 100.0f, 100.0f);
    const auto outside_surface = SelectExpandedTouchTarget(
        &target, 1u, -1.0f, 10.0f, 8.0f, 8.0f, 100.0f, 100.0f);
    CHECK(state, expanded && !expanded.point_was_inside);
    CheckClose(state, expanded.snap_x, 12.0f);
    CheckClose(state, expanded.snap_y, 12.0f);
    CHECK(state, !outside_surface);
    CHECK(state, target.left == 2.0f && target.right == 22.0f);
}

void TestPerTargetClipRejectsCoveredRows(TestState& state) {
    const TouchTargetRect target{90.0f, 90.0f, 110.0f, 110.0f};
    const TouchTargetRect clip{80.0f, 80.0f, 120.0f, 112.0f};
    const auto inside_panel = SelectExpandedTouchTarget(
        &target, 1u, 100.0f, 111.0f, 20.0f, 20.0f,
        200.0f, 200.0f, &clip);
    const auto covered_by_footer = SelectExpandedTouchTarget(
        &target, 1u, 100.0f, 113.0f, 20.0f, 20.0f,
        200.0f, 200.0f, &clip);
    CHECK(state, inside_panel && !inside_panel.point_was_inside);
    CHECK(state, !covered_by_footer);
}

void TestInvalidInputsFailClosed(TestState& state) {
    const TouchTargetRect invalid[] = {
        {20.0f, 10.0f, 10.0f, 20.0f},
        {0.0f, 0.0f, std::numeric_limits<float>::quiet_NaN(), 20.0f},
    };
    CHECK(state, !SelectExpandedTouchTarget(
        invalid, 2u, 10.0f, 10.0f, 8.0f, 8.0f, 100.0f, 100.0f));
    CHECK(state, !SelectExpandedTouchTarget(
        invalid, 2u, std::numeric_limits<float>::quiet_NaN(), 10.0f,
        8.0f, 8.0f, 100.0f, 100.0f));
}

void TestBottomButtonDoublesOnlyUpward(TestState& state) {
    const TouchTargetRect targets[] = {
        {20.0f, 160.0f, 70.0f, 180.0f},
        {80.0f, 160.0f, 130.0f, 180.0f},
    };
    const auto above = SelectUpwardDoubledTouchTarget(
        targets, 2u, 45.0f, 145.0f, 200.0f, 200.0f);
    const auto original = SelectUpwardDoubledTouchTarget(
        targets, 2u, 90.0f, 165.0f, 200.0f, 200.0f);
    const auto horizontal_gap = SelectUpwardDoubledTouchTarget(
        targets, 2u, 75.0f, 145.0f, 200.0f, 200.0f);
    const auto below = SelectUpwardDoubledTouchTarget(
        targets, 2u, 45.0f, 185.0f, 200.0f, 200.0f);
    CHECK(state, above && above.index == 0u && !above.point_was_inside);
    CHECK(state, original && original.index == 1u && original.point_was_inside);
    CHECK(state, !horizontal_gap);
    CHECK(state, !below);
    CheckClose(state, above.snap_x, 45.0f);
    CheckClose(state, above.snap_y, 170.0f);
}

void TestBottomButtonExpansionHonoursClip(TestState& state) {
    const TouchTargetRect target{20.0f, 160.0f, 70.0f, 180.0f};
    const TouchTargetRect clip{0.0f, 150.0f, 200.0f, 200.0f};
    CHECK(state, SelectUpwardDoubledTouchTarget(
        &target, 1u, 45.0f, 155.0f, 200.0f, 200.0f, &clip));
    CHECK(state, !SelectUpwardDoubledTouchTarget(
        &target, 1u, 45.0f, 145.0f, 200.0f, 200.0f, &clip));
}

void TestScrollbarExpandsOnlyToTheRight(TestState& state) {
    const TouchTargetRect scrollbar{100.0f, 20.0f, 108.0f, 180.0f};
    const auto right = SelectRightExpandedTouchTarget(
        &scrollbar, 1u, 116.0f, 90.0f, 12.0f, 200.0f, 200.0f);
    const auto content_side = SelectRightExpandedTouchTarget(
        &scrollbar, 1u, 96.0f, 90.0f, 12.0f, 200.0f, 200.0f);
    const auto below = SelectRightExpandedTouchTarget(
        &scrollbar, 1u, 104.0f, 185.0f, 12.0f, 200.0f, 200.0f);
    CHECK(state, right && !right.point_was_inside);
    CHECK(state, !content_side);
    CHECK(state, !below);
    CheckClose(state, right.snap_x, 104.0f);
}

void TestCoordinateTransformRoundTrip(TestState& state) {
    const WidgetCoordinateTransform transform{
        .surface_width = 2400.0f,
        .surface_height = 1080.0f,
        .coordinate_scale = 0.75f,
        .x_factor = 1.25f,
        .y_factor = 1.0f,
    };
    const float screen_x = 1517.0f;
    const float screen_y = 381.0f;
    CheckClose(state, transform.ScreenXFromWidget(transform.WidgetXFromScreen(screen_x)),
               screen_x);
    CheckClose(state, transform.ScreenYFromWidget(transform.WidgetYFromScreen(screen_y)),
               screen_y);

    const TouchTargetRect rect = WidgetRectToScreen(
        transform, 0.62f, 0.45f, 0.645f, 0.475f);
    CHECK(state, rect.right > rect.left);
    CHECK(state, rect.bottom > rect.top);
    CheckClose(state, transform.WidgetXFromScreen(rect.left), 0.62f);
    CheckClose(state, transform.WidgetYFromScreen(rect.top), 0.475f);
}

}  // namespace

int main() {
    TestState state;
    TestOriginalHitWinsExpandedNeighbour(state);
    TestPackedTargetsUseExclusiveNearestCentre(state);
    TestExpansionIsInvisibleAndSurfaceClipped(state);
    TestPerTargetClipRejectsCoveredRows(state);
    TestInvalidInputsFailClosed(state);
    TestBottomButtonDoublesOnlyUpward(state);
    TestBottomButtonExpansionHonoursClip(state);
    TestScrollbarExpandsOnlyToTheRight(state);
    TestCoordinateTransformRoundTrip(state);
    std::printf("touch_target_geometry_test: %d checks, %d failures\n",
                state.checks, state.failures);
    return state.failures == 0 ? 0 : 1;
}
