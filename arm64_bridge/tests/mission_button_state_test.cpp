#include "mission_button_state.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

using namespace warband::input::mission_buttons;

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

class GuestMemory final {
public:
    static constexpr uint32_t kMission = 0x100u;
    static constexpr uint32_t kBlocks = 0x1000u;
    static constexpr uint32_t kAgents = 0x2000u;
    static constexpr uint32_t kItems = 0x50000u;

    GuestMemory() : words_(0x100000u / sizeof(uint32_t), 0u) {
        view_ = GuestMemoryView{this, Contains, Read32};
    }

    const GuestMemoryView& view() const { return view_; }

    void Write32(uint32_t address, uint32_t value) {
        if (!Contains(this, address, sizeof(uint32_t))) return;
        words_[address / sizeof(uint32_t)] = value;
    }

    void Write8(uint32_t address, uint8_t value) {
        const uint32_t aligned = address & ~3u;
        if (!Contains(this, aligned, sizeof(uint32_t))) return;
        const uint32_t shift = (address - aligned) * 8u;
        uint32_t word = Read32(this, aligned);
        word = (word & ~(0xFFu << shift)) | (static_cast<uint32_t>(value) << shift);
        Write32(aligned, word);
    }

    static bool Contains(void* context, uint64_t address, uint64_t bytes) {
        const auto* memory = static_cast<const GuestMemory*>(context);
        const uint64_t size = memory->words_.size() * sizeof(uint32_t);
        return address <= size && bytes <= size - address;
    }

    static uint32_t Read32(void* context, uint32_t address) {
        const auto* memory = static_cast<const GuestMemory*>(context);
        return memory->words_[address / sizeof(uint32_t)];
    }

    uint32_t Agent(uint32_t id) const {
        return kAgents + id * kAgentStride;
    }

    uint32_t ItemKind(uint32_t id) const {
        return kItems + id * kItemStride;
    }

    void SetItem(uint32_t inventory_slot, int32_t item_id, uint32_t flags,
                 uint32_t state = 0u, uint32_t max_ammo = 0u) {
        const uint32_t player = Agent(0u);
        Write32(player + kAgentInventoryItemOffset +
                    inventory_slot * kAgentInventoryStride,
                static_cast<uint32_t>(item_id));
        Write32(player + kAgentInventoryStateOffset +
                    inventory_slot * kAgentInventoryStride,
                state);
        if (item_id >= 0) {
            const uint32_t kind = ItemKind(static_cast<uint32_t>(item_id));
            Write32(kind + kItemTypeFlagsOffset, flags);
            Write32(kind + kItemMaxAmmoOffset, max_ammo);
        }
    }

    void InitializeMission(uint32_t count) {
        Write32(kMission + kMissionAgentCountOffset, count);
        Write32(kMission + kMissionAgentsBlocksOffset, kBlocks);
        Write32(kBlocks, kAgents);
        Write32(kMission + kMissionPlayerAgentOffset, 0u);
        const uint32_t player = Agent(0u);
        Write32(player + kAgentActiveOffset, 1u);
        Write32(player + kAgentTypeOffset, 1u);
        Write32(player + kAgentMissionIdOffset, 0u);
        Write32(player + kAgentHorseAgentIdOffset, 0xFFFFFFFFu);
        Write32(player + kAgentRelationActiveOffset, 1u);
        Write32(player + kAgentRelationGroupOffset, 0u);
        Write32(player + kAgentWieldedItemOffset, 0xFFFFFFFFu);
        Write32(player + kAgentWieldedItemOffset + 4u, 0xFFFFFFFFu);
        for (uint32_t slot = 0; slot < kAgentInventoryCount; ++slot) {
            Write32(player + kAgentInventoryItemOffset +
                    kAgentInventoryStride * slot, 0xFFFFFFFFu);
        }
    }

    void InitializeAlly() {
        const uint32_t ally = Agent(1u);
        Write32(ally + kAgentActiveOffset, 1u);
        Write32(ally + kAgentTypeOffset, 1u);
        Write32(ally + kAgentMissionIdOffset, 1u);
        Write32(ally + kAgentRelationActiveOffset, 1u);
        Write32(ally + kAgentRelationGroupOffset, 0u);
        Write32(kMission + kMissionTeamRelationOffset, 1u);
    }

private:
    std::vector<uint32_t> words_;
    GuestMemoryView view_{};
};

void TestContextActionGate(TestState& state) {
    CHECK(state, !ContextActionAvailable(-1, 1u));
    CHECK(state, !ContextActionAvailable(0, 0u));
    CHECK(state, ContextActionAvailable(0, 1u));
    CHECK(state, ContextActionAvailable(10, 1u));
}

void TestMountedStateUsesHorseAgentId(TestState& state) {
    GuestMemory memory;
    memory.InitializeMission(1u);
    const uint32_t player = memory.Agent(0u);
    CHECK(state, !PlayerMounted(memory.view(), player));
    memory.Write32(player + kAgentHorseAgentIdOffset, 7u);
    CHECK(state, PlayerMounted(memory.view(), player));
    memory.Write32(player + kAgentHorseAgentIdOffset, 0xFFFFFFFEu);
    CHECK(state, !PlayerMounted(memory.view(), player));
}

void TestDropRequiresAnActuallyWieldedItem(TestState& state) {
    GuestMemory memory;
    memory.InitializeMission(1u);
    const uint32_t player = memory.Agent(0u);
    CHECK(state, !HasWieldedItem(memory.view(), player));

    memory.Write32(player + kAgentWieldedItemOffset, 2u);
    CHECK(state, !HasWieldedItem(memory.view(), player));
    memory.Write32(player + kAgentInventoryItemOffset + 2u * kAgentInventoryStride,
                   14u);
    CHECK(state, HasWieldedItem(memory.view(), player));

    memory.Write32(player + kAgentInventoryItemOffset + 2u * kAgentInventoryStride,
                   0xFFFFFFFFu);
    memory.Write32(player + kAgentWieldedItemOffset, 16u);
    CHECK(state, !HasWieldedItem(memory.view(), player));
}

void TestOrderTargetUsesLiveFriendRelation(TestState& state) {
    GuestMemory memory;
    memory.InitializeMission(1u);
    CHECK(state, !HasFriendlyOrderTarget(memory.view(), GuestMemory::kMission,
                                         memory.Agent(0u), 0));

    memory.InitializeMission(2u);
    memory.InitializeAlly();
    CHECK(state, HasFriendlyOrderTarget(memory.view(), GuestMemory::kMission,
                                        memory.Agent(0u), 0));

    memory.Write32(GuestMemory::kMission + kMissionTeamRelationOffset, 0xFFFFFFFFu);
    CHECK(state, !HasFriendlyOrderTarget(memory.view(), GuestMemory::kMission,
                                         memory.Agent(0u), 0));

    memory.Write32(GuestMemory::kMission + kMissionTeamRelationOffset, 1u);
    memory.Write32(memory.Agent(1u) + kAgentActiveOffset, 0u);
    CHECK(state, !HasFriendlyOrderTarget(memory.view(), GuestMemory::kMission,
                                         memory.Agent(0u), 0));

    memory.Write32(GuestMemory::kMission + kMissionAgentCountOffset,
                   kMaxScannedMissionAgents + 1u);
    CHECK(state, !HasFriendlyOrderTarget(memory.view(), GuestMemory::kMission,
                                         memory.Agent(0u), 0));
}

void TestWeaponCycleRequiresAChangedSelection(TestState& state) {
    GuestMemory memory;
    memory.InitializeMission(1u);
    const uint32_t player = memory.Agent(0u);
    constexpr uint32_t kOneHandedWeapon = 1u | kTwoHandedWieldFlag;
    memory.SetItem(0u, 10, kOneHandedWeapon);
    memory.Write32(player + kAgentWieldedItemOffset, 0u);
    CHECK(state, !CanCycleWeapon(memory.view(), player, GuestMemory::kItems, 0));

    memory.SetItem(1u, 11, kOneHandedWeapon);
    CHECK(state, CanCycleWeapon(memory.view(), player, GuestMemory::kItems, 0));

    memory.SetItem(1u, 10, kOneHandedWeapon);
    CHECK(state, !CanCycleWeapon(memory.view(), player, GuestMemory::kItems, 0));

    memory.SetItem(0u, 10, kOneHandedWeapon);
    memory.SetItem(1u, 12, kItemShieldType, 1u);
    CHECK(state, CanCycleWeapon(memory.view(), player, GuestMemory::kItems, 1));
}

void TestShieldWieldSelectionUsesOriginalMainHand(TestState& state) {
    GuestMemory memory;
    memory.InitializeMission(1u);
    const uint32_t player = memory.Agent(0u);
    memory.SetItem(0u, 12, kItemShieldType, 1u);
    memory.Write32(player + kAgentOffhandRestrictionOffset, 1u);

    // With an empty original main hand, equip_next_weapon still asks the
    // wield-index helper to evaluate the candidate as an offhand item.  A
    // non-empty main hand applies the offhand restriction and rejects it.
    CHECK(state, CandidateWieldIndex(memory.view(), player, GuestMemory::kItems,
                                     0, false) == 1);
    CHECK(state, CandidateWieldIndex(memory.view(), player, GuestMemory::kItems,
                                     0, true) == -1);
}

void TestStateMask(TestState& state) {
    const uint32_t mask = BuildStateMask(
        true, false, true, false, true, false, true);
    CHECK(state, (mask & kContextAction) != 0u);
    CHECK(state, (mask & kDropItem) == 0u);
    CHECK(state, (mask & kIssueOrders) != 0u);
    CHECK(state, (mask & kCycleWeapon) == 0u);
    CHECK(state, (mask & kCycleShield) != 0u);
    CHECK(state, (mask & kSheathWeapon) == 0u);
    CHECK(state, (mask & kPlayerMounted) != 0u);
}

}  // namespace

int main() {
    TestState state;
    TestContextActionGate(state);
    TestMountedStateUsesHorseAgentId(state);
    TestDropRequiresAnActuallyWieldedItem(state);
    TestOrderTargetUsesLiveFriendRelation(state);
    TestWeaponCycleRequiresAChangedSelection(state);
    TestShieldWieldSelectionUsesOriginalMainHand(state);
    TestStateMask(state);
    std::printf("mission_button_state_test: %s (%d checks)\n",
                state.failures ? "FAIL" : "PASS", state.checks);
    return state.failures ? 1 : 0;
}
