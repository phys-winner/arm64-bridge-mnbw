#include "../src/inventory_touch_input.h"

#include <cstdio>

namespace {

using warband::input::DecideInventoryDragPhase;
using warband::input::DecideInventoryReleaseAction;
using warband::input::InventoryDragPhaseAction;
using warband::input::InventoryReleaseAction;
using warband::input::InventoryRecoveryInFlight;
using warband::input::ShouldPreserveInventoryRecovery;
using warband::input::ShouldQuarantineInventoryRawStream;
using warband::input::ShouldDeferInventoryMenuScroll;
using warband::input::ShouldClaimInventoryItemGesture;
using warband::input::ShouldShowInventoryTapDetails;
using warband::input::ShouldStartInventoryDrag;
using warband::input::PlanInventoryDrop;

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

void TestGuestPhaseHandshake(TestState& state) {
    CHECK(state, DecideInventoryDragPhase(false, true, false, true, false) ==
                     InventoryDragPhaseAction::Wait);
    CHECK(state, DecideInventoryDragPhase(true, true, false, true, false) ==
                     InventoryDragPhaseAction::Wait);
    CHECK(state, DecideInventoryDragPhase(true, false, false, true, false) ==
                     InventoryDragPhaseAction::Wait);
    CHECK(state, DecideInventoryDragPhase(true, false, true, false, false) ==
                     InventoryDragPhaseAction::Wait);
    CHECK(state, DecideInventoryDragPhase(false, false, true, true, true) ==
                     InventoryDragPhaseAction::Wait);
    CHECK(state, DecideInventoryDragPhase(true, false, true, true, true) ==
                     InventoryDragPhaseAction::DispatchRelease);
    CHECK(state, DecideInventoryDragPhase(false, false, true, true, false) ==
                     InventoryDragPhaseAction::AbortLostGuestDrag);
    CHECK(state, DecideInventoryDragPhase(true, false, true, true, false) ==
                     InventoryDragPhaseAction::AbortLostGuestDrag);
}

void TestConcreteSlotUsesOwningMenuGate(TestState& state) {
    const auto plan = PlanInventoryDrop(10u, 20u, 30u, 40u);
    CHECK(state, plan.HasDispatch());
    CHECK(state, plan.menu == 30u);
    CHECK(state, plan.slot == 40u);
    CHECK(state, !plan.restores_source);

    const auto same_slot = PlanInventoryDrop(10u, 20u, 10u, 20u);
    CHECK(state, same_slot.menu == 10u);
    CHECK(state, same_slot.slot == 20u);
    CHECK(state, !same_slot.restores_source);
}

void TestReleaseNeverAbandonsHeldItem(TestState& state) {
    CHECK(state, DecideInventoryReleaseAction(false, false) ==
                     InventoryReleaseAction::WaitForReadableState);
    CHECK(state, DecideInventoryReleaseAction(true, false) ==
                     InventoryReleaseAction::Complete);
    CHECK(state, DecideInventoryReleaseAction(true, true) ==
                     InventoryReleaseAction::RestoreSource);

    CHECK(state, !InventoryRecoveryInFlight(false, false, false, false));
    CHECK(state, InventoryRecoveryInFlight(true, false, false, false));
    CHECK(state, InventoryRecoveryInFlight(false, true, false, false));
    CHECK(state, InventoryRecoveryInFlight(false, false, true, true));
    CHECK(state, InventoryRecoveryInFlight(false, false, true, false));

    CHECK(state, ShouldPreserveInventoryRecovery(true, true, true, false));
    CHECK(state, !ShouldPreserveInventoryRecovery(true, false, true, false));
    CHECK(state, ShouldPreserveInventoryRecovery(true, false, true, true));
    CHECK(state, ShouldPreserveInventoryRecovery(true, false, false, false));
    CHECK(state, !ShouldPreserveInventoryRecovery(false, true, false, true));

    CHECK(state, !ShouldQuarantineInventoryRawStream(
                     true, true, false, false, false));
    CHECK(state, ShouldQuarantineInventoryRawStream(
                     true, true, true, false, false));
    CHECK(state, ShouldQuarantineInventoryRawStream(
                     true, true, false, true, false));
    CHECK(state, ShouldQuarantineInventoryRawStream(
                     true, true, false, false, true));
    CHECK(state, ShouldQuarantineInventoryRawStream(
                     true, false, false, false, false));
    CHECK(state, !ShouldQuarantineInventoryRawStream(
                     false, false, true, true, true));
}

void TestMenuOnlyTargetAndSafeRestore(TestState& state) {
    const auto discard = PlanInventoryDrop(10u, 20u, 30u, 0u);
    CHECK(state, discard.menu == 30u);
    CHECK(state, discard.slot == 0u);
    CHECK(state, !discard.restores_source);

    const auto source_gap = PlanInventoryDrop(10u, 20u, 10u, 0u);
    CHECK(state, source_gap.menu == 10u);
    CHECK(state, source_gap.slot == 20u);
    CHECK(state, source_gap.restores_source);

    const auto outside = PlanInventoryDrop(10u, 20u, 0u, 0u);
    CHECK(state, outside.menu == 10u);
    CHECK(state, outside.slot == 20u);
    CHECK(state, outside.restores_source);

    const warband::input::InventoryDropPlan malformed_slot_only{
        .menu = 0u,
        .slot = 20u,
    };
    CHECK(state, !malformed_slot_only.HasDispatch());
}

void TestInventoryMenuScrollArbitration(TestState& state) {
    CHECK(state, !ShouldDeferInventoryMenuScroll(false, false));
    CHECK(state, !ShouldDeferInventoryMenuScroll(false, true));
    CHECK(state, ShouldDeferInventoryMenuScroll(true, false));
    CHECK(state, !ShouldDeferInventoryMenuScroll(true, true));
}

void TestImmediateInventoryGestureClassification(TestState& state) {
    CHECK(state, ShouldClaimInventoryItemGesture(true, 10u, 20u));
    CHECK(state, !ShouldClaimInventoryItemGesture(false, 10u, 20u));
    CHECK(state, !ShouldClaimInventoryItemGesture(true, 0u, 20u));
    CHECK(state, !ShouldClaimInventoryItemGesture(true, 10u, 0u));

    CHECK(state, ShouldStartInventoryDrag(true, false, true));
    CHECK(state, !ShouldStartInventoryDrag(true, false, false));
    CHECK(state, !ShouldStartInventoryDrag(true, true, true));
    CHECK(state, !ShouldStartInventoryDrag(false, false, true));

    CHECK(state, ShouldShowInventoryTapDetails(true, false, false, true));
    CHECK(state, !ShouldShowInventoryTapDetails(true, true, false, true));
    CHECK(state, !ShouldShowInventoryTapDetails(true, false, true, true));
    CHECK(state, !ShouldShowInventoryTapDetails(true, false, false, false));
    CHECK(state, !ShouldShowInventoryTapDetails(false, false, false, true));
}

}  // namespace

int main() {
    TestState state;
    TestGuestPhaseHandshake(state);
    TestReleaseNeverAbandonsHeldItem(state);
    TestConcreteSlotUsesOwningMenuGate(state);
    TestMenuOnlyTargetAndSafeRestore(state);
    TestInventoryMenuScrollArbitration(state);
    TestImmediateInventoryGestureClassification(state);
    std::printf("inventory_touch_input_test: %s (%d checks)\n",
                state.failures ? "FAIL" : "PASS", state.checks);
    return state.failures ? 1 : 0;
}
