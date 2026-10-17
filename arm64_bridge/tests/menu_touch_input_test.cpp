#include "../src/menu_touch_input.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

using warband::input::DiscreteArbitrator;
using warband::input::MenuMoveOutput;
using warband::input::MenuPulseScheduler;
using warband::input::MenuTouchConfig;
using warband::input::MenuTouchRecognizer;
using warband::input::MenuWindowSupportsShoulders;

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

MenuTouchConfig TestConfig() {
    MenuTouchConfig config;
    config.touch_slop = 8.0f;
    config.shoulder_size = 132.0f;
    config.shoulder_tap_slop = 24.0f;
    config.surface_width = 2400.0f;
    config.surface_height = 1080.0f;
    return config;
}

void TestVerticalDragScrolls(TestState& state) {
    MenuTouchRecognizer gesture;
    gesture.Configure(TestConfig());
    gesture.Begin(2, 100.0f, 100.0f, 7u);
    // Wrong pointer is ignored entirely.
    MenuMoveOutput foreign = gesture.Move(9, 100.0f, 200.0f, 7u);
    CHECK(state, !foreign);
    // Within the slop nothing is emitted yet.
    MenuMoveOutput slop = gesture.Move(2, 100.0f, 104.0f, 7u);
    CHECK(state, !slop);
    CHECK(state, !gesture.OwnsVerticalScroll());
    // A vertical drag emits fractional scroll proportional to the movement.
    MenuMoveOutput scroll = gesture.Move(2, 100.0f, 208.0f, 7u);
    CHECK(state, !scroll.swallow);
    CHECK(state, scroll.scroll_dy_fraction > 0.09f && scroll.scroll_dy_fraction < 0.1f);
    CHECK(state, scroll.scroll_anchor_x == 100.0f);
    CHECK(state, scroll.scroll_anchor_y == 100.0f);
    CHECK(state, gesture.OwnsVerticalScroll());
    // Dragging back up emits the opposite sign.
    MenuMoveOutput back = gesture.Move(2, 850.0f, 104.0f, 7u);
    CHECK(state, !back.swallow);
    CHECK(state, back.scroll_dy_fraction < -0.09f && back.scroll_dy_fraction > -0.1f);
    // Even after the live pointer crosses into another pane, the target stays
    // anchored to the pane that owned DOWN.
    CHECK(state, back.scroll_anchor_x == 100.0f);
    CHECK(state, back.scroll_anchor_y == 100.0f);
}

void TestHorizontalDragDoesNotScroll(TestState& state) {
    MenuTouchRecognizer gesture;
    gesture.Configure(TestConfig());
    gesture.Begin(3, 120.0f, 540.0f, 7u);
    MenuMoveOutput horizontal = gesture.Move(3, 800.0f, 544.0f, 7u);
    CHECK(state, !horizontal.swallow);
    CHECK(state, horizontal.scroll_dy_fraction == 0.0f);
    CHECK(state, !gesture.OwnsVerticalScroll());
    gesture.Cancel();
}

void TestWindowChangeSwallowsRelease(TestState& state) {
    MenuTouchRecognizer gesture;
    gesture.Configure(TestConfig());
    gesture.Begin(4, 200.0f, 200.0f, 7u);
    CHECK(state, gesture.MatchesWindow(7u));
    // A release reported on another game screen must be swallowed so the new
    // screen cannot receive the phantom press.
    MenuMoveOutput finish = gesture.Finish(4, 202.0f, 202.0f, 13u);
    CHECK(state, finish.swallow);
    CHECK(state, !gesture.IsTracking());

    // Moves observed on a changed window swallow and stop the drag stream.
    gesture.Begin(5, 200.0f, 200.0f, 7u);
    MenuMoveOutput moved = gesture.Move(5, 210.0f, 400.0f, 13u);
    CHECK(state, moved.swallow);
    CHECK(state, moved.scroll_dy_fraction == 0.0f);
    gesture.Cancel();

    // An unknown (unstable) current window is treated as changed: fail closed.
    gesture.Begin(6, 200.0f, 200.0f, 7u);
    MenuMoveOutput unknown = gesture.Finish(6, 200.0f, 200.0f,
                                            warband::input::kUnknownMenuWindowId);
    CHECK(state, unknown.swallow);
}

void TestShoulderTargets(TestState& state) {
    MenuTouchRecognizer gesture;
    gesture.Configure(TestConfig());
    // Centered 4:3 viewport is x=480..1920.
    gesture.Begin(2, 510.0f, 40.0f, 11u);
    CHECK(state, gesture.IsShoulderTracking());
    // MoveMenuPointerLocked in the bridge evaluates pulses before Cancel, so
    // mirror that order here: evaluate while still tracking.
    gesture.Move(2, 516.0f, 44.0f, 11u);
    MenuTouchRecognizer::DiscretePulse left = gesture.TakeShoulderPulse();
    CHECK(state, left.mask == warband::input::kButtonL1 && left.count == 1u);
    gesture.Cancel();

    gesture.Begin(3, 1880.0f, 35.0f, 13u);
    gesture.Move(3, 1875.0f, 39.0f, 13u);
    MenuTouchRecognizer::DiscretePulse right = gesture.TakeShoulderPulse();
    CHECK(state, right.mask == warband::input::kButtonR1 && right.count == 1u);
    gesture.Cancel();

    gesture.Begin(4, 1200.0f, 40.0f, 29u);
    CHECK(state, !gesture.IsShoulderTracking());
    // A shoulder candidate that slides out of the corner loses eligibility.
    gesture.Begin(5, 520.0f, 40.0f, 31u);
    gesture.Move(5, 580.0f, 40.0f, 31u);
    MenuTouchRecognizer::DiscretePulse cancelled = gesture.TakeShoulderPulse();
    CHECK(state, cancelled.mask == 0u && cancelled.count == 0u);
    gesture.Cancel();
}

void TestWindowBindingAndShoulderAllowlist(TestState& state) {
    CHECK(state, MenuWindowSupportsShoulders(11u));
    CHECK(state, MenuWindowSupportsShoulders(13u));
    CHECK(state, MenuWindowSupportsShoulders(15u));
    CHECK(state, MenuWindowSupportsShoulders(29u));
    CHECK(state, MenuWindowSupportsShoulders(31u));
    CHECK(state, !MenuWindowSupportsShoulders(7u));
    CHECK(state, !MenuWindowSupportsShoulders(10u));
    CHECK(state, !MenuWindowSupportsShoulders(
        warband::input::kUnknownMenuWindowId));

    MenuTouchRecognizer gesture;
    MenuTouchConfig shoulders_only = TestConfig();
    shoulders_only.swipe_navigation = false;
    gesture.Configure(shoulders_only);

    gesture.Begin(2, 510.0f, 40.0f, 11u);
    CHECK(state, gesture.IsTracking());
    CHECK(state, gesture.WindowId() == 11u);
    CHECK(state, gesture.MatchesWindow(11u));
    CHECK(state, !gesture.MatchesWindow(13u));
    gesture.Cancel();
    CHECK(state, gesture.WindowId() == warband::input::kUnknownMenuWindowId);

    // The same corner is not a hidden synthetic button outside the proven
    // character/options/quick-battle/face-editor windows.
    gesture.Begin(3, 510.0f, 40.0f, 7u);
    CHECK(state, !gesture.IsTracking());
    gesture.Begin(4, 510.0f, 40.0f,
                  warband::input::kUnknownMenuWindowId);
    CHECK(state, !gesture.IsTracking());
}

void TestPulseTimingAndArbitration(TestState& state) {
    DiscreteArbitrator input;
    MenuPulseScheduler pulses;
    pulses.Enqueue({warband::input::kDpadDown, 2u}, 0u);
    pulses.Advance(0u, input);
    CHECK(state, input.PopEncodedEvent() == 3u);  // event 2 + 1
    pulses.Advance(69u, input);
    CHECK(state, input.PopEncodedEvent() == 0u);
    pulses.Advance(70u, input);
    CHECK(state, input.PopEncodedEvent() == 4u);  // event 3 + 1
    pulses.Advance(104u, input);
    CHECK(state, input.PopEncodedEvent() == 0u);
    pulses.Advance(105u, input);
    CHECK(state, input.PopEncodedEvent() == 3u);
    pulses.Advance(175u, input);
    CHECK(state, input.PopEncodedEvent() == 4u);

    input.SetPhysical(warband::input::kButtonA, true);
    CHECK(state, input.PopEncodedEvent() == 9u);  // A down event 8 + 1
    pulses.Enqueue({warband::input::kButtonA, 1u}, 200u);
    pulses.Advance(299u, input);
    CHECK(state, input.PopEncodedEvent() == 0u);
    pulses.Advance(300u, input);
    CHECK(state, input.PopEncodedEvent() == 0u);  // physical source already owns A
    pulses.Advance(370u, input);
    CHECK(state, input.PopEncodedEvent() == 0u);  // synthetic UP cannot release it
    input.SetPhysical(warband::input::kButtonA, false);
    CHECK(state, input.PopEncodedEvent() == 10u); // A up event 9 + 1
}

void TestPulseOverflowFailsClosed(TestState& state) {
    DiscreteArbitrator input;
    MenuPulseScheduler pulses;
    pulses.Enqueue({warband::input::kDpadDown, 300u}, 0u);
    CHECK(state, pulses.PendingSize() == MenuPulseScheduler::kMaxPendingPulses);
    CHECK(state, pulses.PendingSize() == 32u);
    CHECK(state, pulses.DroppedCount() == 268u);
    pulses.Advance(0u, input);
    CHECK(state, input.PopEncodedEvent() == 3u);
    pulses.Reset(input);
    CHECK(state, input.PopEncodedEvent() == 4u);
    CHECK(state, pulses.PendingSize() == 0u && pulses.ActiveMask() == 0u);
    CHECK(state, pulses.DroppedCount() == 268u);
}

}  // namespace

int main() {
    TestState state;
    TestVerticalDragScrolls(state);
    TestHorizontalDragDoesNotScroll(state);
    TestWindowChangeSwallowsRelease(state);
    TestShoulderTargets(state);
    TestWindowBindingAndShoulderAllowlist(state);
    TestPulseTimingAndArbitration(state);
    TestPulseOverflowFailsClosed(state);
    std::printf("menu_touch_input_test: %s (%d checks)\n",
                state.failures ? "FAIL" : "PASS", state.checks);
    return state.failures ? 1 : 0;
}
