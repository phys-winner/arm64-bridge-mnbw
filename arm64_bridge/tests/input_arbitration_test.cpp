#include "../src/input_arbitration.h"
#include "../src/text_input_queue.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <initializer_list>

namespace {

using warband::input::DiscreteArbitrator;

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

void ExpectEvent(TestState& state, DiscreteArbitrator& input, uint32_t event) {
    CHECK(state, input.PopEncodedEvent() == event + 1u);
}

void ExpectDrained(TestState& state, DiscreteArbitrator& input) {
    CHECK(state, input.PopEncodedEvent() == 0u);
}

void TestEveryBindingBothOverlapOrders(TestState& state) {
    for (const auto& binding : warband::input::kDiscreteBindings) {
        DiscreteArbitrator touch_first;
        touch_first.SetVirtual(binding.mask, true);
        ExpectEvent(state, touch_first, binding.down_event);
        touch_first.SetPhysical(binding.mask, true);
        touch_first.SetVirtual(binding.mask, false);
        ExpectDrained(state, touch_first);  // physical still owns it
        touch_first.SetPhysical(binding.mask, false);
        ExpectEvent(state, touch_first, binding.up_event);
        ExpectDrained(state, touch_first);

        DiscreteArbitrator physical_first;
        physical_first.SetPhysical(binding.mask, true);
        ExpectEvent(state, physical_first, binding.down_event);
        physical_first.SetVirtual(binding.mask, true);
        physical_first.SetPhysical(binding.mask, false);
        ExpectDrained(state, physical_first);  // touch still owns it
        physical_first.SetVirtual(binding.mask, false);
        ExpectEvent(state, physical_first, binding.up_event);
        ExpectDrained(state, physical_first);
    }
}

void TestQuickTapAndIdempotence(TestState& state) {
    DiscreteArbitrator input;
    input.SetVirtual(warband::input::kButtonA, true);
    input.SetVirtual(warband::input::kButtonA, true);
    input.SetVirtual(warband::input::kButtonA, false);
    input.SetVirtual(warband::input::kButtonA, false);
    ExpectEvent(state, input, 8u);
    ExpectEvent(state, input, 9u);
    ExpectDrained(state, input);
}

void TestEnterConfirmUsesNativeKeyboardEvents(TestState& state) {
    DiscreteArbitrator input;
    input.SetVirtual(warband::input::kEnterConfirm, true);
    ExpectEvent(state, input, 32u);  // AKEYCODE_ENTER down
    input.SetVirtual(warband::input::kEnterConfirm, false);
    ExpectEvent(state, input, 33u);  // AKEYCODE_ENTER up
    ExpectDrained(state, input);
}

void TestResetPreservesOtherSource(TestState& state) {
    DiscreteArbitrator input;
    input.SetPhysical(warband::input::kButtonB, true);
    input.SetVirtual(warband::input::kButtonB, true);
    ExpectEvent(state, input, 10u);
    input.ResetVirtual();
    ExpectDrained(state, input);
    CHECK(state, input.CombinedHeld() == warband::input::kButtonB);
    input.ResetPhysical();
    ExpectEvent(state, input, 11u);
    ExpectDrained(state, input);
}

void TestIndependentPhysicalAliases(TestState& state) {
    DiscreteArbitrator input;
    // This models SELECT and Android BACK as two independent host keys which
    // are reduced to the same logical physical mask by jni_bridge.cpp.
    uint32_t aliases_down = 0;
    const auto publish = [&] {
        input.SetPhysicalMask(aliases_down ? warband::input::kButtonBack : 0u);
    };
    aliases_down |= 1u;
    publish();
    ExpectEvent(state, input, 30u);
    aliases_down |= 2u;
    publish();
    aliases_down &= ~1u;
    publish();
    ExpectDrained(state, input);
    aliases_down &= ~2u;
    publish();
    ExpectEvent(state, input, 31u);
    ExpectDrained(state, input);
}

void TestOverflowFailsSafeWithoutStuckButtons(TestState& state) {
    DiscreteArbitrator input;
    for (int edge = 0; edge < 65; ++edge) {
        input.SetVirtual(warband::input::kButtonX, (edge & 1) == 0);
    }
    CHECK(state, input.OverflowCount() == 1u);
    CHECK(state, input.ResyncRequired());
    // Final state is down; overflow drops transient edges and reconciles to it.
    ExpectEvent(state, input, 12u);
    ExpectDrained(state, input);
    CHECK(state, input.DeliveredHeld() == warband::input::kButtonX);
    input.SetVirtual(warband::input::kButtonX, false);
    ExpectEvent(state, input, 13u);
    ExpectDrained(state, input);
    CHECK(state, input.DeliveredHeld() == 0u);
}

void TestAllButtonsCanReconcileTogether(TestState& state) {
    DiscreteArbitrator input;
    input.SetPhysicalMask(warband::input::AllDiscreteMask());
    for (const auto& binding : warband::input::kDiscreteBindings) {
        ExpectEvent(state, input, binding.down_event);
    }
    ExpectDrained(state, input);
    CHECK(state, input.DeliveredHeld() == warband::input::AllDiscreteMask());

    input.ResetPhysical();
    for (const auto& binding : warband::input::kDiscreteBindings) {
        ExpectEvent(state, input, binding.up_event);
    }
    ExpectDrained(state, input);
    CHECK(state, input.DeliveredHeld() == 0u);
}

void ExpectFrame(TestState& state, DiscreteArbitrator& input,
                 std::initializer_list<uint32_t> events, uint32_t held) {
    for (uint32_t event : events) CHECK(state, input.PopFrameEncodedEvent() == event + 1u);
    CHECK(state, input.PopFrameEncodedEvent() == 0u);
    CHECK(state, input.DeliveredHeld() == held);
    input.FinishFrame();  // Same boundary as the production post-rglKeys::tick hook.
}

void TestProductionFrameDrain(TestState& state) {
    using namespace warband::input;
    // Every binding survives a complete tap before the next tick, including
    // Back30/31, Enter32/33, controller face buttons, shoulders and the D-pad.
    for (const auto& binding : kDiscreteBindings) {
        DiscreteArbitrator input;
        input.SetVirtual(binding.mask, true);
        input.SetVirtual(binding.mask, false);
        CHECK(state, input.PendingSize() == 2u);
        ExpectFrame(state, input, {binding.down_event}, binding.mask);
        CHECK(state, input.PendingSize() == 1u);
        ExpectFrame(state, input, {binding.up_event}, 0u);
        CHECK(state, input.PendingSize() == 0u);
    }

    DiscreteArbitrator repeat;
    repeat.SetVirtual(kButtonBack, true);
    ExpectFrame(state, repeat, {30u}, kButtonBack);
    repeat.SetVirtual(kButtonBack, false);
    repeat.SetVirtual(kButtonBack, true);
    repeat.SetVirtual(kButtonBack, false);
    ExpectFrame(state, repeat, {31u}, 0u);  // UP cannot erase the next DOWN.
    ExpectFrame(state, repeat, {30u}, kButtonBack);
    ExpectFrame(state, repeat, {31u}, 0u);

    DiscreteArbitrator fifo;
    fifo.SetVirtual(kButtonBack, true);
    fifo.SetVirtual(kButtonA, true);
    fifo.SetVirtual(kButtonBack, false);
    fifo.SetVirtual(kButtonX, true);
    ExpectFrame(state, fifo, {30u, 8u}, kButtonBack | kButtonA);
    CHECK(state, fifo.PendingSize() == 2u);  // X does not bypass the earlier Back UP.
    ExpectFrame(state, fifo, {31u, 12u}, kButtonA | kButtonX);
    fifo.SetVirtual(kButtonA | kButtonX, true);  // Repeated holds create no edges.
    ExpectFrame(state, fifo, {}, kButtonA | kButtonX);
    fifo.ResetVirtual();
    ExpectFrame(state, fifo, {9u, 13u}, 0u);

    DiscreteArbitrator cancelled;
    cancelled.SetVirtual(kButtonBack, true);
    CHECK(state, cancelled.PopFrameEncodedEvent() == 31u);
    cancelled.ResetVirtual();  // Cancel after DOWN delivery, before this tick ends.
    CHECK(state, cancelled.PopFrameEncodedEvent() == 0u);
    CHECK(state, cancelled.DeliveredHeld() == kButtonBack);
    cancelled.FinishFrame();
    ExpectFrame(state, cancelled, {31u}, 0u);
    cancelled.SetVirtual(kButtonBack, true);
    cancelled.SetPhysical(kButtonBack, true);
    ExpectFrame(state, cancelled, {30u}, kButtonBack);
    cancelled.ResetVirtual();
    ExpectFrame(state, cancelled, {}, kButtonBack);  // Cancel cannot release a controller.
    cancelled.ResetPhysical();
    ExpectFrame(state, cancelled, {31u}, 0u);
    cancelled.SetVirtual(kButtonBack, true);
    CHECK(state, cancelled.PopFrameEncodedEvent() == 31u);
    cancelled.ResetAll();  // Bridge lifecycle resets the frame gate as well.
    cancelled.SetVirtual(kButtonBack, true);
    ExpectFrame(state, cancelled, {30u}, kButtonBack);
    cancelled.ResetVirtual();
    ExpectFrame(state, cancelled, {31u}, 0u);

    // The fixed-size FIFO guarantees progress even when each head conflicts
    // with the current frame. Text has its own FIFO and still drains per tick.
    DiscreteArbitrator bounded;
    for (uint32_t edge = 0; edge < 64u; ++edge)
        bounded.SetVirtual(kButtonBack, (edge & 1u) == 0u);
    CHECK(state, bounded.PendingSize() == 64u && bounded.OverflowCount() == 0u);
    bounded.SetVirtual(kButtonBack, false);  // Idempotent release does not fill the FIFO.
    warband::text_input::SessionQueue text;
    const auto token = text.Begin();
    const std::array<uint16_t, 2> characters = {0x0041u, 0x0411u};
    CHECK(state, text.Apply(token, 0, characters, true));
    const std::array<uint32_t, 3> text_expected = {0x0041u, 0x0411u, 0x000Du};
    uint32_t slot = warband::text_input::kPendingSlotEmpty;
    const auto read_slot = [&] { return slot; };
    const auto write_slot = [&](uint32_t value) { slot = value; };
    for (uint32_t frame = 0; frame < 64u; ++frame) {
        CHECK(state, bounded.PopFrameEncodedEvent() == ((frame & 1u) == 0u ? 31u : 32u));
        CHECK(state, bounded.PopFrameEncodedEvent() == 0u);
        CHECK(state, bounded.PendingSize() == 63u - frame);
        if (frame < text_expected.size()) {
            CHECK(state, text.DrainOne(read_slot, write_slot) == warband::text_input::DrainResult::Drained);
            CHECK(state, slot == text_expected[frame]);
            if (frame + 1u < text_expected.size())
                CHECK(state, text.DrainOne(read_slot, write_slot) == warband::text_input::DrainResult::Busy);
            slot = warband::text_input::kPendingSlotEmpty;
        }
        bounded.FinishFrame();
    }
    CHECK(state, bounded.PendingSize() == 0u && bounded.DeliveredHeld() == 0u);
    CHECK(state, text.Empty() && text.ActiveToken() == token);

    DiscreteArbitrator overflow;
    for (uint32_t edge = 0; edge < 65u; ++edge)
        overflow.SetVirtual(kButtonX, (edge & 1u) == 0u);
    CHECK(state, overflow.ResyncRequired());
    CHECK(state, overflow.PopFrameEncodedEvent() == 13u);
    overflow.ResetVirtual();  // Reconcile UP must wait for the DOWN's tick too.
    CHECK(state, overflow.PopFrameEncodedEvent() == 0u);
    CHECK(state, overflow.ResyncRequired());
    overflow.FinishFrame();
    ExpectFrame(state, overflow, {13u}, 0u);
    CHECK(state, !overflow.ResyncRequired());
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    TestState state;
    std::puts("running overlap tests");
    TestEveryBindingBothOverlapOrders(state);
    std::puts("running tap tests");
    TestQuickTapAndIdempotence(state);
    TestEnterConfirmUsesNativeKeyboardEvents(state);
    std::puts("running reset tests");
    TestResetPreservesOtherSource(state);
    std::puts("running alias tests");
    TestIndependentPhysicalAliases(state);
    std::puts("running overflow tests");
    TestOverflowFailsSafeWithoutStuckButtons(state);
    std::puts("running reconciliation tests");
    TestAllButtonsCanReconcileTogether(state);
    std::puts("running production frame drain tests");
    TestProductionFrameDrain(state);
    std::printf("input_arbitration_test: %s (%d checks)\n",
                state.failures ? "FAIL" : "PASS", state.checks);
    return state.failures ? 1 : 0;
}
