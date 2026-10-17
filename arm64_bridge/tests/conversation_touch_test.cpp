#include "conversation_touch.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>

namespace {

using warband::input::ConversationTouchTarget;

static_assert(warband::input::kConversationWindowId == 12u);
static_assert(warband::input::kInitialWindowId == 4u);
static_assert(warband::input::kMenuWindowId == 18u);
static_assert(warband::input::kConversationWindowSlotOffset == 0x34u);
static_assert(warband::input::kConversationVtableImageOffset == 0x003D4DA8u);
static_assert(warband::input::kConversationChoiceCountOffset == 0x1320u);
static_assert(warband::input::kConversationObjectSize == 0x13A0u);
static_assert(warband::input::kConversationGamepadControllerOffset == 0x140u);
static_assert(warband::input::kMenuChoiceFirstOffset == 0x1F8u);
static_assert(warband::input::kMenuChoiceLastOffset == 0x234u);
static_assert(warband::input::kInitialChoiceFirstOffset == 0x1F8u);
static_assert(warband::input::kInitialChoiceLastOffset == 0x210u);

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

void TestChoiceAreaTap(TestState& state) {
    ConversationTouchTarget target;
    constexpr int32_t kWidth = 2400;
    constexpr int32_t kHeight = 1080;

    CHECK(state, target.BeginTap(7, 1200.0f, 900.0f, kWidth, kHeight,
                                 12.0f, true));
    CHECK(state, target.IsTracking());
    target.MoveTap(7, 1204.0f, 904.0f, kWidth, kHeight, true);
    CHECK(state, target.IsTracking());
    CHECK(state, target.EndTap(7, 1204.0f, 904.0f, kWidth, kHeight, true));
    CHECK(state, !target.IsTracking());

    // The stock choice container spans x=.01..975 and the bottom 27% of UI.
    CHECK(state, target.BeginTap(1, 24.0f, 900.0f, kWidth, kHeight, 8.0f, true));
    CHECK(state, target.EndTap(1, 24.0f, 900.0f, kWidth, kHeight, true));
    CHECK(state, target.BeginTap(1, 2300.0f, 900.0f, kWidth, kHeight, 8.0f, true));
    CHECK(state, target.EndTap(1, 2300.0f, 900.0f, kWidth, kHeight, true));
}

void TestGateAndHitbox(TestState& state) {
    ConversationTouchTarget target;
    constexpr int32_t kWidth = 2400;
    constexpr int32_t kHeight = 1080;

    CHECK(state, !target.BeginTap(1, 1200.0f, 900.0f, kWidth, kHeight,
                                  8.0f, false));
    CHECK(state, !target.BeginTap(1, 1200.0f, 600.0f, kWidth, kHeight,
                                  8.0f, true));
    CHECK(state, !target.BeginTap(1, 1200.0f, 1075.0f, kWidth, kHeight,
                                  8.0f, true));
    CHECK(state, !target.BeginTap(1, 6.0f, 900.0f, kWidth, kHeight,
                                  8.0f, true));
    CHECK(state, !target.BeginTap(1, 2390.0f, 900.0f, kWidth, kHeight,
                                  8.0f, true));
    CHECK(state, !target.BeginTap(1, 1200.0f, 900.0f, 0, kHeight,
                                  8.0f, true));
    CHECK(state, !target.BeginTap(1, 1200.0f, 900.0f, kWidth, 0,
                                  8.0f, true));
    CHECK(state, !target.BeginTap(1, std::numeric_limits<float>::quiet_NaN(),
                                  900.0f, kWidth, kHeight, 8.0f, true));
    CHECK(state, !target.BeginTap(1, 1200.0f, 900.0f, kWidth, kHeight,
                                  0.0f, true));
    CHECK(state, !target.BeginTap(-1, 1200.0f, 900.0f, kWidth, kHeight,
                                  8.0f, true));
    CHECK(state, !target.IsTracking());
}

void TestTapCancellation(TestState& state) {
    ConversationTouchTarget target;
    constexpr int32_t kWidth = 2400;
    constexpr int32_t kHeight = 1080;

    CHECK(state, target.BeginTap(2, 1200.0f, 900.0f, kWidth, kHeight,
                                 8.0f, true));
    target.MoveTap(2, 1250.0f, 900.0f, kWidth, kHeight, true);
    CHECK(state, !target.IsTracking());
    CHECK(state, !target.EndTap(2, 1200.0f, 900.0f, kWidth, kHeight, true));

    CHECK(state, target.BeginTap(3, 1200.0f, 900.0f, kWidth, kHeight,
                                 8.0f, true));
    target.MoveTap(4, 1200.0f, 900.0f, kWidth, kHeight, true);
    CHECK(state, !target.IsTracking());

    CHECK(state, target.BeginTap(5, 1200.0f, 900.0f, kWidth, kHeight,
                                 8.0f, true));
    CHECK(state, !target.EndTap(6, 1200.0f, 900.0f, kWidth, kHeight, true));

    CHECK(state, target.BeginTap(7, 1200.0f, 900.0f, kWidth, kHeight,
                                 8.0f, true));
    CHECK(state, !target.EndTap(7, 1200.0f, 900.0f, kWidth, kHeight, false));
    CHECK(state, !target.IsTracking());

    CHECK(state, target.BeginTap(8, 1200.0f, 900.0f, kWidth, kHeight,
                                 8.0f, true));
    target.Cancel();  // Mirrors POINTER_DOWN, POINTER_UP, and ACTION_CANCEL.
    CHECK(state, !target.EndTap(8, 1200.0f, 900.0f, kWidth, kHeight, true));
}

void TestContinueChoiceCountGate(TestState& state) {
    CHECK(state, warband::input::IsContinueChoiceCount(0u));
    CHECK(state, warband::input::IsContinueChoiceCount(1u));
    CHECK(state, !warband::input::IsContinueChoiceCount(2u));
    CHECK(state, !warband::input::IsContinueChoiceCount(
                     std::numeric_limits<uint32_t>::max()));
}

void TestConversationNavigationSuppression(TestState& state) {
    CHECK(state, !warband::input::ShouldSuppressConversationNavigation(
                     false, false));
    CHECK(state, warband::input::ShouldSuppressConversationNavigation(
                     true, false));
    CHECK(state, warband::input::ShouldSuppressConversationNavigation(
                     false, true));
    CHECK(state, warband::input::ShouldSuppressConversationNavigation(
                     true, true));
}

void TestConversationTouchHighlight(TestState& state) {
    constexpr uint32_t kWidget = 0x12345678u;
    CHECK(state, warband::input::TouchHighlightWidget(
                     true, false, kWidget) == kWidget);
    CHECK(state, warband::input::TouchHighlightWidget(
                     false, false, kWidget) == 0u);
    CHECK(state, warband::input::TouchHighlightWidget(
                     true, true, kWidget) == 0u);
    CHECK(state, warband::input::TouchHighlightWidget(
                     true, false, 0u) == 0u);

    CHECK(state, !warband::input::UiTouchBlocksMenuInput(false, false));
    CHECK(state, warband::input::UiTouchBlocksMenuInput(true, false));
    CHECK(state, !warband::input::UiTouchBlocksMenuInput(true, true));
}

}  // namespace

int main() {
    TestState state;
    TestChoiceAreaTap(state);
    TestGateAndHitbox(state);
    TestTapCancellation(state);
    TestContinueChoiceCountGate(state);
    TestConversationNavigationSuppression(state);
    TestConversationTouchHighlight(state);
    std::printf("conversation_touch_test: %s (%d checks)\n",
                state.failures ? "FAIL" : "PASS", state.checks);
    return state.failures ? 1 : 0;
}
