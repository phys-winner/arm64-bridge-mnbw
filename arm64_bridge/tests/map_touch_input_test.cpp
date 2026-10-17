#include "../src/map_touch_input.h"

#include <array>
#include <cmath>
#include <cstdio>

namespace {

using warband::input::MapTouchConfig;
using warband::input::MapTouchPoint;
using warband::input::MapTouchRecognizer;

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

#define CHECK(state, expression) (state).Check(static_cast<bool>(expression), #expression, __LINE__)

constexpr int64_t kMs = 1000000LL;

MapTouchConfig TestConfig() {
    MapTouchConfig config;
    config.touch_slop = 8.0f;
    config.edge_dead_zone_fraction = 0.05f;
    config.bottom_control_exclusion_fraction = 0.12f;
    config.pinch_slop = 10.0f;
    config.rotation_slop_radians = 0.05f;
    config.tilt_slop = 20.0f;
    config.density = 1.0f;
    config.surface_width = 1000.0f;
    config.surface_height = 500.0f;
    return config;
}

void TestShortTapPassesThrough(TestState& state) {
    MapTouchRecognizer gesture;
    gesture.Configure(TestConfig());
    CHECK(state, !gesture.Begin(1, 500.0f, 250.0f, 1000 * kMs));
    const MapTouchPoint jitter{1, 504.0f, 252.0f};
    CHECK(state, !gesture.MoveFrame(&jitter, 1u, 1020 * kMs));
    const auto finish = gesture.Finish(1, 1060 * kMs);
    CHECK(state, !finish);
    CHECK(state, finish.content_tap);
}

void TestOneFingerPanClaimsAfterSlop(TestState& state) {
    MapTouchRecognizer gesture;
    gesture.Configure(TestConfig());
    gesture.Begin(2, 500.0f, 250.0f, 1000 * kMs);
    const MapTouchPoint first_point{2, 520.0f, 260.0f};
    const auto first = gesture.MoveFrame(&first_point, 1u, 1030 * kMs);
    CHECK(state, first.cancel_guest_stream);
    CHECK(state, std::fabs(first.pan_dx_pixels - 20.0f) < 0.001f);
    CHECK(state, std::fabs(first.pan_dy_pixels - 10.0f) < 0.001f);
    CHECK(state, gesture.GuestStreamCancelled());

    const MapTouchPoint second_point{2, 525.0f, 270.0f};
    const auto second = gesture.MoveFrame(&second_point, 1u, 1040 * kMs);
    CHECK(state, !second.cancel_guest_stream);
    CHECK(state, std::fabs(second.pan_dx_pixels - 5.0f) < 0.001f);
    CHECK(state, std::fabs(second.pan_dy_pixels - 10.0f) < 0.001f);
    CHECK(state, gesture.Finish(2, 1080 * kMs).cancel_guest_stream);
}

void TestEdgeAndBottomStrip(TestState& state) {
    MapTouchRecognizer gesture;
    gesture.Configure(TestConfig());
    CHECK(state, gesture.Begin(3, 4.0f, 250.0f, 1000 * kMs).cancel_guest_stream);
    CHECK(state, gesture.IsEdgeSuppressed());
    CHECK(state, gesture.Finish(3, 1030 * kMs).cancel_guest_stream);

    CHECK(state, !gesture.Begin(4, 4.0f, 490.0f, 2000 * kMs));
    CHECK(state, !gesture.Finish(4, 2040 * kMs));
}

void TestPinchUsesNavigationDirection(TestState& state) {
    MapTouchRecognizer gesture;
    gesture.Configure(TestConfig());
    gesture.Begin(10, 400.0f, 250.0f, 1000 * kMs);
    CHECK(state, gesture.PointerDown(11, 600.0f, 250.0f, 1010 * kMs).cancel_guest_stream);

    const std::array<MapTouchPoint, 2> spread = {{{10, 350.0f, 250.0f},
                                                   {11, 650.0f, 250.0f}}};
    const auto zoom_in = gesture.MoveFrame(spread.data(), spread.size(), 1040 * kMs);
    CHECK(state, zoom_in.zoom_log_delta > 0.0f);
    CHECK(state, gesture.IsPinching());

    const std::array<MapTouchPoint, 2> close = {{{10, 440.0f, 250.0f},
                                                  {11, 560.0f, 250.0f}}};
    const auto zoom_out = gesture.MoveFrame(close.data(), close.size(), 1060 * kMs);
    CHECK(state, zoom_out.zoom_log_delta < 0.0f);
    CHECK(state, gesture.PointerUp(11, 1080 * kMs).cancel_guest_stream);
    CHECK(state, gesture.Finish(10, 1090 * kMs).cancel_guest_stream);
}

void TestRotation(TestState& state) {
    MapTouchRecognizer gesture;
    gesture.Configure(TestConfig());
    gesture.Begin(20, 400.0f, 250.0f, 1000 * kMs);
    gesture.PointerDown(21, 600.0f, 250.0f, 1010 * kMs);
    const std::array<MapTouchPoint, 2> rotate = {{{20, 410.0f, 200.0f},
                                                   {21, 590.0f, 300.0f}}};
    const auto output = gesture.MoveFrame(rotate.data(), rotate.size(), 1040 * kMs);
    CHECK(state, std::fabs(output.rotation_delta_radians) > 0.05f);
    CHECK(state, output.zoom_log_delta != 0.0f);
}

void TestParallelSwipeControlsTiltContinuously(TestState& state) {
    MapTouchRecognizer gesture;
    gesture.Configure(TestConfig());
    gesture.Begin(30, 400.0f, 250.0f, 1000 * kMs);
    gesture.PointerDown(31, 600.0f, 250.0f, 1010 * kMs);
    const std::array<MapTouchPoint, 2> up = {{{30, 402.0f, 215.0f},
                                               {31, 602.0f, 214.0f}}};
    const auto output = gesture.MoveFrame(up.data(), up.size(), 1040 * kMs);
    CHECK(state, output.tilt_delta_pixels < -30.0f);
    CHECK(state, output.zoom_log_delta == 0.0f);
    CHECK(state, output.rotation_delta_radians == 0.0f);
    const std::array<MapTouchPoint, 2> farther_up = {{{30, 402.0f, 195.0f},
                                                       {31, 602.0f, 194.0f}}};
    const auto continued = gesture.MoveFrame(
        farther_up.data(), farther_up.size(), 1060 * kMs);
    CHECK(state, continued.tilt_delta_pixels < -19.0f);
    CHECK(state, continued.tilt_delta_pixels > -21.0f);

    gesture.Cancel();
    gesture.Begin(32, 400.0f, 250.0f, 2000 * kMs);
    gesture.PointerDown(33, 600.0f, 250.0f, 2010 * kMs);
    const std::array<MapTouchPoint, 2> down = {{{32, 398.0f, 286.0f},
                                                 {33, 599.0f, 285.0f}}};
    CHECK(state, gesture.MoveFrame(down.data(), down.size(), 2040 * kMs)
                     .tilt_delta_pixels > 30.0f);

    gesture.Cancel();
    gesture.Begin(34, 400.0f, 260.0f, 3000 * kMs);
    gesture.PointerDown(35, 600.0f, 250.0f, 3010 * kMs);
    const std::array<MapTouchPoint, 2> natural_up = {{{34, 430.0f, 198.0f},
                                                       {35, 627.0f, 194.0f}}};
    const auto tolerant = gesture.MoveFrame(
        natural_up.data(), natural_up.size(), 3040 * kMs);
    CHECK(state, tolerant.tilt_delta_pixels < -50.0f);
    CHECK(state, tolerant.zoom_log_delta == 0.0f);
    CHECK(state, tolerant.rotation_delta_radians == 0.0f);
}

void TestDoubleTapDoesNotStepZoomAndDragStillWorks(TestState& state) {
    MapTouchRecognizer gesture;
    gesture.Configure(TestConfig());
    gesture.Begin(40, 500.0f, 250.0f, 1000 * kMs);
    gesture.Finish(40, 1040 * kMs);
    CHECK(state, gesture.Begin(40, 502.0f, 251.0f, 1200 * kMs).cancel_guest_stream);
    const auto tap = gesture.Finish(40, 1240 * kMs);
    CHECK(state, tap.cancel_guest_stream);
    CHECK(state, tap.zoom_log_delta == 0.0f);

    gesture.Begin(41, 500.0f, 250.0f, 2000 * kMs);
    gesture.Finish(41, 2040 * kMs);
    gesture.Begin(41, 500.0f, 250.0f, 2200 * kMs);
    const MapTouchPoint drag_up{41, 500.0f, 200.0f};
    const auto drag = gesture.MoveFrame(&drag_up, 1u, 2230 * kMs);
    CHECK(state, drag.zoom_log_delta > 0.0f);
    CHECK(state, gesture.Finish(41, 2260 * kMs).cancel_guest_stream);
}

void TestCancelledWaitTapDoesNotArmDoubleTap(TestState& state) {
    MapTouchRecognizer gesture;
    gesture.Configure(TestConfig());
    gesture.Begin(45, 500.0f, 250.0f, 1000 * kMs);
    const auto wait_tap = gesture.Finish(45, 1040 * kMs);
    CHECK(state, wait_tap.content_tap);
    gesture.ForgetTapHistory();
    CHECK(state, !gesture.Begin(45, 502.0f, 251.0f, 1200 * kMs));
    CHECK(state, gesture.Finish(45, 1240 * kMs).content_tap);

    gesture.Begin(46, 500.0f, 250.0f, 2000 * kMs);
    const auto long_hold = gesture.Finish(46, 2600 * kMs);
    CHECK(state, !long_hold.content_tap);
}

void TestTwoFingerTapZoomsOut(TestState& state) {
    MapTouchRecognizer gesture;
    gesture.Configure(TestConfig());
    gesture.Begin(50, 400.0f, 250.0f, 1000 * kMs);
    gesture.PointerDown(51, 600.0f, 250.0f, 1010 * kMs);
    const auto output = gesture.PointerUp(51, 1080 * kMs);
    CHECK(state, output.cancel_guest_stream);
    CHECK(state, output.zoom_log_delta < 0.0f);
    CHECK(state, gesture.Finish(50, 1090 * kMs).cancel_guest_stream);
}

void TestCompassUsesLargePhysicalHitTarget(TestState& state) {
    MapTouchRecognizer gesture;
    auto config = TestConfig();
    config.density = 3.0f;
    gesture.Configure(config);
    // Visual compass center is roughly (170, 441) on this letterboxed surface;
    // the 80 dp target still accepts a point 110 px away from the tiny graphic.
    const auto begin = gesture.Begin(60, 60.0f, 441.0f, 1000 * kMs);
    CHECK(state, begin.cancel_guest_stream);
    const auto finish = gesture.Finish(60, 1050 * kMs);
    CHECK(state, finish.cancel_guest_stream);
    CHECK(state, finish.recenter);
}

void TestMalformedAndThirdPointerFailClosed(TestState& state) {
    MapTouchRecognizer gesture;
    gesture.Configure(TestConfig());
    CHECK(state, !gesture.Begin(-1, 10.0f, 10.0f));
    gesture.Begin(1, 500.0f, 250.0f);
    gesture.PointerDown(2, 450.0f, 250.0f);
    CHECK(state, gesture.PointerDown(3, 550.0f, 250.0f).cancel_guest_stream);
    CHECK(state, gesture.GuestStreamCancelled());
    gesture.Cancel();
    CHECK(state, !gesture.IsTracking());
}

}  // namespace

int main() {
    TestState state;
    TestShortTapPassesThrough(state);
    TestOneFingerPanClaimsAfterSlop(state);
    TestEdgeAndBottomStrip(state);
    TestPinchUsesNavigationDirection(state);
    TestRotation(state);
    TestParallelSwipeControlsTiltContinuously(state);
    TestDoubleTapDoesNotStepZoomAndDragStillWorks(state);
    TestCancelledWaitTapDoesNotArmDoubleTap(state);
    TestTwoFingerTapZoomsOut(state);
    TestCompassUsesLargePhysicalHitTarget(state);
    TestMalformedAndThirdPointerFailClosed(state);
    std::printf("map_touch_input_test: %s (%d checks)\n",
                state.failures ? "FAIL" : "PASS", state.checks);
    return state.failures ? 1 : 0;
}
