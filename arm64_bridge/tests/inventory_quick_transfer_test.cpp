#include "../src/inventory_quick_transfer.h"

#include <array>
#include <cstdio>

namespace {

using namespace warband::input;
using Result = InventoryQuickTransferResult;

int checks = 0;
int failures = 0;

void Check(bool condition, const char* expression, int line) {
    ++checks;
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
    }
}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

void TestLootAndTrade() {
    const std::array<int32_t, 3> own{42, -1, -2};
    const std::array<int32_t, 3> other{-2, 55, -1};
    const std::array<int32_t, 10> equipped{1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    for (int mode : {1, 2}) {
        const auto outgoing = PlanInventoryQuickTransfer(mode, 0, 0, 11u, 0u,
                                                         own, other, equipped, true);
        CHECK(outgoing.HasDestination() && outgoing.menu == 1 && outgoing.slot == 2);
        const auto incoming = PlanInventoryQuickTransfer(mode, 1, 1, 11u, 0u,
                                                         own, other, equipped, true);
        CHECK(incoming.HasDestination() && incoming.menu == 0 && incoming.slot == 1);
        const auto unequip = PlanInventoryQuickTransfer(mode, 2, 4, 12u, 0u,
                                                        own, other, equipped, false);
        CHECK(unequip.HasDestination() && unequip.menu == 0 && unequip.slot == 1);
    }
    CHECK(PlanInventoryQuickTransfer(1, 0, 0, 11u, 0u, own, equipped, {}, true)
              .result == Result::NoFreeSlot);
    const auto horse_sale = PlanInventoryQuickTransfer(3, 0, 0, 1u, 0u,
                                                        own, other, equipped, true);
    CHECK(horse_sale.HasDestination() && horse_sale.menu == 1);
    CHECK(PlanInventoryQuickTransfer(3, 0, 0, 12u, 0u, own, other, equipped, true)
              .result == Result::UnsupportedContext);
}

void TestPersonalEquipmentNeverDiscards() {
    const std::array<int32_t, 2> own{42, -1};
    const std::array<int32_t, 2> discard{22, -1};
    std::array<int32_t, 10> equipped;
    equipped.fill(-1);
    equipped[0] = 52;
    const auto sword = PlanInventoryQuickTransfer(0, 0, 0, 2u, 0u,
                                                  own, discard, equipped, true);
    CHECK(sword.HasDestination() && sword.menu == 2 && sword.slot == 1);
    for (uint32_t type = 12; type <= 15; ++type) {
        const auto armor = PlanInventoryQuickTransfer(0, 0, 0, type, 0u,
                                                      own, discard, equipped, true);
        CHECK(armor.HasDestination() && armor.menu == 2 && armor.slot == static_cast<int>(type - 8));
    }
    const auto horse = PlanInventoryQuickTransfer(0, 0, 0, 1u, 0u,
                                                  own, discard, equipped, true);
    CHECK(horse.HasDestination() && horse.menu == 2 && horse.slot == 8);
    equipped[8] = 91;
    CHECK(PlanInventoryQuickTransfer(0, 0, 0, 1u, 0u, own, discard, equipped, true)
              .result == Result::NoFreeSlot);
    CHECK(PlanInventoryQuickTransfer(0, 0, 0, 11u, 0u, own, discard, equipped, true)
              .result == Result::NotEquippable);
    const auto food = PlanInventoryQuickTransfer(0, 0, 0, 11u, 0x80000u,
                                                 own, discard, equipped, true);
    CHECK(food.HasDestination() && food.menu == 2 && food.slot == 9);
    CHECK(PlanInventoryQuickTransfer(0, 0, 0, 2u, 0u, own, discard, equipped, false)
              .result == Result::RequirementsNotMet);
    equipped.fill(1);
    CHECK(PlanInventoryQuickTransfer(0, 0, 0, 2u, 0u, own, discard, equipped, true)
              .result == Result::NoFreeSlot);
    const auto unequip = PlanInventoryQuickTransfer(0, 2, 0, 2u, 0u,
                                                    own, discard, equipped, true);
    CHECK(unequip.HasDestination() && unequip.menu == 0 && unequip.slot == 1);
    const auto reclaim = PlanInventoryQuickTransfer(0, 1, 0, 11u, 0u,
                                                    own, discard, equipped, true);
    CHECK(reclaim.HasDestination() && reclaim.menu == 0 && reclaim.slot == 1);
    CHECK(own[0] == 42 && discard[0] == 22 && equipped[0] == 1);
}

void TestInvalidInputs() {
    const std::array<int32_t, 2> own{42, -1};
    const std::array<int32_t, 2> other{-2, -1};
    const std::array<int32_t, 10> equipped{};
    for (const auto [menu, slot] : std::array<std::array<int, 2>, 7>{{
             {-1, 0}, {3, 0}, {0, -1}, {0, 2}, {0, 1}, {1, 0}, {2, 10}}}) {
        const auto plan = PlanInventoryQuickTransfer(1, menu, slot, 2u, 0u,
                                                      own, other, equipped, true);
        CHECK(!plan.HasDestination() && plan.result == Result::InvalidSource);
    }
    std::array<int32_t, 97> oversized{};
    CHECK(PlanInventoryQuickTransfer(1, 0, 0, 2u, 0u, oversized, other, equipped, true)
              .result == Result::InvalidSource);
    CHECK(PlanInventoryQuickTransfer(4, 0, 0, 2u, 0u, own, other, equipped, true)
              .result == Result::UnsupportedContext);
    CHECK(PlanInventoryQuickTransfer(1, 0, 0, 2u, 0u, own, {}, equipped, true)
              .result == Result::NoFreeSlot);
}

void TestReleasedTapIdentity() {
    const InventoryTapIdentity tap{0x1000u, 2, 87, 1, 3, 42, 7u};
    CHECK(IsInventoryDoubleTap(tap, 1000u, tap, 1300u));
    CHECK(!IsInventoryDoubleTap(tap, 1000u, tap, 1301u));
    CHECK(!IsInventoryDoubleTap(tap, 1000u, tap, 999u));
    CHECK(!IsInventoryDoubleTap({}, 1000u, tap, 1200u));
    auto changed = tap;
    changed.window += 4u;
    CHECK(!IsInventoryDoubleTap(tap, 1000u, changed, 1200u));
    changed = tap; changed.context_troop += 1;
    CHECK(!IsInventoryDoubleTap(tap, 1000u, changed, 1200u));
    changed = tap; changed.mode = 0;
    CHECK(!IsInventoryDoubleTap(tap, 1000u, changed, 1200u));
    changed = tap; changed.slot += 1;
    CHECK(!IsInventoryDoubleTap(tap, 1000u, changed, 1200u));
    changed = tap; changed.item_kind += 1;
    CHECK(!IsInventoryDoubleTap(tap, 1000u, changed, 1200u));
    changed = tap; changed.item_metadata += 1u;
    CHECK(!IsInventoryDoubleTap(tap, 1000u, changed, 1200u));
    changed = tap; changed.item_kind = -1;
    CHECK(!IsInventoryDoubleTap(changed, 1000u, changed, 1200u));
}

}  // namespace

int main() {
    TestLootAndTrade();
    TestPersonalEquipmentNeverDiscards();
    TestInvalidInputs();
    TestReleasedTapIdentity();
    std::printf("inventory quick transfer: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
