#include "inventory_scroll_state.h"

#include <cmath>
#include <cstdio>
#include <limits>

namespace {

using warband::input::CaptureInventoryScrollbarSnapshot;
using warband::input::FindInventoryScrollTrackMatch;
using warband::input::InventoryScrollbarSnapshot;
using warband::input::RestoreInventoryScrollbarValue;
using warband::input::TouchTargetRect;

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

void TestRangeIsPreservedAcrossRebuild(TestState& state) {
    InventoryScrollbarSnapshot snapshot{};
    const TouchTargetRect original{1880.0f, 80.0f, 1910.0f, 920.0f};
    CHECK(state, CaptureInventoryScrollbarSnapshot(
        original, 25.0f, 10.0f, 70.0f, &snapshot));
    CHECK(state, snapshot.valid);
    CHECK(state, std::fabs(snapshot.relative_value - 0.25f) < 0.0001f);

    float restored = 0.0f;
    CHECK(state, RestoreInventoryScrollbarValue(
        snapshot, -1.0f, 1.0f, &restored));
    CHECK(state, std::fabs(restored + 0.5f) < 0.0001f);
}

void TestRebuiltColumnsMatchByGeometry(TestState& state) {
    InventoryScrollbarSnapshot right{};
    CHECK(state, CaptureInventoryScrollbarSnapshot(
        {1880.0f, 80.0f, 1910.0f, 920.0f}, 0.35f, 0.0f, 1.0f,
        &right));
    const TouchTargetRect rebuilt[] = {
        {802.0f, 82.0f, 832.0f, 918.0f},
        {1883.0f, 82.0f, 1913.0f, 918.0f},
    };
    bool claimed[] = {false, false};
    CHECK(state, FindInventoryScrollTrackMatch(
        right, rebuilt, 2u, claimed, 2424.0f, 1080.0f) == 1u);
    claimed[1] = true;
    CHECK(state, FindInventoryScrollTrackMatch(
        right, rebuilt, 2u, claimed, 2424.0f, 1080.0f) ==
        std::numeric_limits<std::size_t>::max());
}

void TestInvalidOrUnrelatedTrackIsRejected(TestState& state) {
    InventoryScrollbarSnapshot snapshot{};
    CHECK(state, !CaptureInventoryScrollbarSnapshot(
        {1880.0f, 80.0f, 1910.0f, 920.0f}, 0.0f, 1.0f, 1.0f,
        &snapshot));
    CHECK(state, CaptureInventoryScrollbarSnapshot(
        {1880.0f, 80.0f, 1910.0f, 920.0f}, 0.5f, 0.0f, 1.0f,
        &snapshot));
    const TouchTargetRect unrelated{300.0f, 300.0f, 330.0f, 600.0f};
    CHECK(state, FindInventoryScrollTrackMatch(
        snapshot, &unrelated, 1u, nullptr, 2424.0f, 1080.0f) ==
        std::numeric_limits<std::size_t>::max());
}

}  // namespace

int main() {
    TestState state;
    TestRangeIsPreservedAcrossRebuild(state);
    TestRebuiltColumnsMatchByGeometry(state);
    TestInvalidOrUnrelatedTrackIsRejected(state);
    std::printf("inventory_scroll_state_test: %s (%d checks)\n",
                state.failures ? "FAIL" : "PASS", state.checks);
    return state.failures ? 1 : 0;
}
