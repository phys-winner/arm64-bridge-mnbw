#pragma once

#include <cstdint>
#include <span>

namespace warband::input {

// Values are also the native-to-Java input-toast protocol; zero shows no toast.
enum class InventoryQuickTransferResult : uint32_t {
    Move = 0,
    InvalidSource = 1,
    NoFreeSlot = 2,
    NotEquippable = 3,
    RequirementsNotMet = 4,
    UnsupportedContext = 5,
    GuestRejected = 6,
};

struct InventoryQuickTransferPlan {
    int menu = -1;  // check_slot: bags 0/1, personal equipment 2.
    int slot = -1;
    InventoryQuickTransferResult result = InventoryQuickTransferResult::InvalidSource;

    constexpr bool HasDestination() const {
        return result == InventoryQuickTransferResult::Move && menu >= 0 && slot >= 0;
    }
};

constexpr bool InventoryEquipmentSlotAccepts(int slot, uint32_t item_type,
                                             uint64_t item_flags) {
    // Stock check_slot's weapon types are 2..10 and 16..18.
    if (slot >= 0 && slot < 4) {
        return (item_type >= 2u && item_type <= 10u) ||
               (item_type >= 16u && item_type <= 18u);
    }
    if (slot >= 4 && slot <= 7) return item_type == static_cast<uint32_t>(slot + 8);
    if (slot == 8) return item_type == 1u;
    // The legacy food slot accepts goods with itp_food. Use slot 8 for mounts.
    return slot == 9 && item_type == 11u && (item_flags & 0x80000u) != 0u;
}

constexpr InventoryQuickTransferPlan PlanInventoryQuickTransfer(
    int mode, int source_menu, int source_slot, uint32_t item_type,
    uint64_t item_flags, std::span<const int32_t> own_bag,
    std::span<const int32_t> other_bag, std::span<const int32_t> own_equipment,
    bool can_equip) {
    using Result = InventoryQuickTransferResult;
    // Only validated, currently accessible guest Item arrays may be supplied.
    if (own_bag.size() > 96u || other_bag.size() > 96u || own_equipment.size() > 10u) {
        return {};
    }
    const auto source = source_menu == 0 ? own_bag : source_menu == 1 ? other_bag :
                        source_menu == 2 ? own_equipment : std::span<const int32_t>{};
    if (source_slot < 0 || static_cast<size_t>(source_slot) >= source.size() ||
        source[static_cast<size_t>(source_slot)] < 0) {
        return {};
    }
    if (mode < 0 || mode > 3) return {.result = Result::UnsupportedContext};

    const bool equip = mode == 0 && source_menu == 0;
    // Worn items always unequip into the personal bag. In personal mode, bag 1
    // is Discard: its items can be reclaimed, but it is never a destination.
    const int destination_menu = equip ? 2 : source_menu == 2 ? 0 :
                                 mode == 0 ? 0 : 1 - source_menu;
    const auto destination = destination_menu == 0 ? own_bag :
                             destination_menu == 1 ? other_bag : own_equipment;
    if (mode == 3 && destination_menu == 1 && item_type != 1u) {
        return {.result = Result::UnsupportedContext};
    }

    bool compatible_slot = !equip;
    for (size_t index = 0; index < destination.size(); ++index) {
        if (equip && !InventoryEquipmentSlotAccepts(static_cast<int>(index), item_type,
                                                    item_flags)) {
            continue;
        }
        compatible_slot = true;
        if (equip && !can_equip) return {.result = Result::RequirementsNotMet};
        // Other negative values fail closed; only Item.kind == -1 is empty.
        if (destination[index] == -1) {
            return {.menu = destination_menu, .slot = static_cast<int>(index),
                    .result = Result::Move};
        }
    }
    return {.result = compatible_slot ? Result::NoFreeSlot : Result::NotEquippable};
}

struct InventoryTapIdentity {
    uint32_t window = 0;
    int mode = -1;
    int context_troop = -1;
    int menu = -1;
    int slot = -1;
    int32_t item_kind = -1;
    uint32_t item_metadata = 0;

    constexpr bool operator==(const InventoryTapIdentity&) const = default;
};

// Call only for released short item taps. Clear the saved identity on drag,
// long press, extra pointer, window transition, or after consuming a pair.
constexpr bool IsInventoryDoubleTap(const InventoryTapIdentity& previous,
                                    uint64_t previous_ms,
                                    const InventoryTapIdentity& current,
                                    uint64_t current_ms) {
    return current.window != 0u && current.mode >= 0 && current.mode <= 3 &&
           current.menu >= 0 && current.menu <= 2 && current.slot >= 0 &&
           current.item_kind >= 0 && previous == current &&
           current_ms >= previous_ms && current_ms - previous_ms <= 300u;
}

}  // namespace warband::input
