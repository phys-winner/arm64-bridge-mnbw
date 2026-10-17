#pragma once

#include <cmath>
#include <cstdint>
#include <limits>

namespace warband::input::mission_buttons {

inline constexpr uint32_t kContextAction = 1u << 0;
inline constexpr uint32_t kDropItem = 1u << 1;
inline constexpr uint32_t kIssueOrders = 1u << 2;
inline constexpr uint32_t kCycleWeapon = 1u << 3;
inline constexpr uint32_t kCycleShield = 1u << 4;
inline constexpr uint32_t kSheathWeapon = 1u << 5;
inline constexpr uint32_t kPlayerMounted = 1u << 6;

inline constexpr uint32_t kMissionAgentCountOffset = 0x08u;
inline constexpr uint32_t kMissionAgentsBlocksOffset = 0x18u;
inline constexpr uint32_t kMissionPlayerAgentOffset = 0x78u;
inline constexpr uint32_t kMissionTeamRelationOffset = 45636u;
inline constexpr uint32_t kAgentStride = 24240u;
inline constexpr uint32_t kAgentActiveOffset = 0x04u;
inline constexpr uint32_t kAgentRelationGroupOffset = 1088u;
inline constexpr uint32_t kAgentTypeOffset = 40u;
inline constexpr uint32_t kAgentIdentityOffset = 8u;
inline constexpr uint32_t kAgentMissionIdOffset = 276u;
// Agent::set_horse_agent_id_client reads and writes this signed agent id.
// -1 means that the human agent is not mounted.
inline constexpr uint32_t kAgentHorseAgentIdOffset = 0x110u;
inline constexpr uint32_t kAgentRelationActiveOffset = 48u;
inline constexpr uint32_t kAgentWieldedItemOffset = 0x4C0u;
inline constexpr uint32_t kAgentInventoryItemOffset = 1124u;
inline constexpr uint32_t kAgentInventoryStateOffset = 1128u;
inline constexpr uint32_t kAgentInventoryStride = 8u;
inline constexpr uint32_t kAgentInventoryCount = 4u;
inline constexpr uint32_t kAgentWeaponRestrictionOffset = 272u;
inline constexpr uint32_t kAgentOffhandRestrictionOffset = 1232u;
inline constexpr uint32_t kAgentSpecialGroupsFlagOffset = 348u;
inline constexpr uint32_t kAgentSpecialGroupsBeginOffset = 352u;
inline constexpr uint32_t kAgentSpecialGroupsEndOffset = 356u;
inline constexpr uint32_t kAgentSpecialGroupRelationsOffset = 364u;
inline constexpr uint32_t kItemStride = 3120u;
inline constexpr uint32_t kItemTypeFlagsOffset = 3016u;
inline constexpr uint32_t kItemMaxAmmoOffset = 3084u;
inline constexpr uint32_t kItemShieldType = 7u;
inline constexpr uint32_t kItemThrownType = 10u;
inline constexpr uint32_t kTwoHandedWieldFlag = 1u << 22;
inline constexpr uint32_t kAgentBoundWeaponFlag = 1u << 28;
inline constexpr uint32_t kMaxScannedMissionAgents = 4096u;

// The bridge supplies checked guest-memory access so this decision logic can be
// exercised against a small synthetic mission in host-side tests.
struct GuestMemoryView {
    void* context = nullptr;
    bool (*contains)(void*, uint64_t, uint64_t) = nullptr;
    uint32_t (*read32)(void*, uint32_t) = nullptr;
};

inline bool ReadWord(const GuestMemoryView& memory, uint64_t address,
                     uint32_t* value) {
    if (!value || !memory.context || !memory.contains || !memory.read32 ||
        address > std::numeric_limits<uint32_t>::max() ||
        !memory.contains(memory.context, address, sizeof(uint32_t))) {
        return false;
    }
    *value = memory.read32(memory.context, static_cast<uint32_t>(address));
    return true;
}

inline bool ReadByte(const GuestMemoryView& memory, uint64_t address,
                     uint8_t* value) {
    if (!value || address > std::numeric_limits<uint32_t>::max()) return false;
    const uint64_t aligned = address & ~uint64_t{3};
    uint32_t word = 0;
    if (!ReadWord(memory, aligned, &word)) return false;
    const uint32_t shift = static_cast<uint32_t>((address - aligned) * 8u);
    *value = static_cast<uint8_t>((word >> shift) & 0xFFu);
    return true;
}

inline bool ReadMissionAgent(const GuestMemoryView& memory, uint32_t mission,
                             uint32_t agent_id, uint32_t agent_count,
                             uint32_t* agent) {
    if (!agent || agent_id >= agent_count) return false;
    uint32_t blocks = 0;
    if (!ReadWord(memory, static_cast<uint64_t>(mission) +
                          kMissionAgentsBlocksOffset, &blocks) || !blocks) {
        return false;
    }
    const uint64_t block_slot = static_cast<uint64_t>(blocks) +
        static_cast<uint64_t>(agent_id >> 4u) * sizeof(uint32_t);
    uint32_t block = 0;
    if (!ReadWord(memory, block_slot, &block) || !block) return false;
    const uint64_t address = static_cast<uint64_t>(block) +
        static_cast<uint64_t>(agent_id & 0xFu) * kAgentStride;
    if (address > std::numeric_limits<uint32_t>::max() ||
        !memory.contains ||
        !memory.contains(memory.context, address, kAgentStride)) {
        return false;
    }
    *agent = static_cast<uint32_t>(address);
    return true;
}

struct AgentRelationGroup {
    int32_t group = -1;
    int32_t identity = -1;
};

inline bool ResolveRelationGroup(const GuestMemoryView& memory, uint32_t mission,
                                uint32_t agent, uint32_t agent_count,
                                bool is_player,
                                AgentRelationGroup* result) {
    if (!result) return false;
    uint32_t type = 0;
    uint32_t identity = 0;
    if (!ReadWord(memory, static_cast<uint64_t>(agent) + kAgentTypeOffset, &type) ||
        !ReadWord(memory, static_cast<uint64_t>(agent) + kAgentIdentityOffset,
                  &identity)) {
        return false;
    }

    if (type != 1u) {
        uint32_t group = 0;
        if (!ReadWord(memory, static_cast<uint64_t>(agent) +
                              kAgentRelationGroupOffset, &group)) {
            return false;
        }
        result->group = static_cast<int32_t>(group);
        result->identity = static_cast<int32_t>(identity);
        return true;
    }

    uint32_t mission_id_word = 0;
    if (!ReadWord(memory, static_cast<uint64_t>(agent) + kAgentMissionIdOffset,
                  &mission_id_word)) {
        return false;
    }
    const int32_t mission_id = static_cast<int32_t>(mission_id_word);
    // Agent::is_friend accepts player agent zero but treats a candidate agent
    // zero as an invalid comparison target.
    if (mission_id < 0 || (!is_player && mission_id == 0) ||
        static_cast<uint32_t>(mission_id) >= agent_count) {
        return false;
    }

    uint32_t mission_agent = 0;
    uint32_t active = 0;
    if (!ReadMissionAgent(memory, mission, static_cast<uint32_t>(mission_id),
                          agent_count, &mission_agent) ||
        !ReadWord(memory, static_cast<uint64_t>(mission_agent) +
                          kAgentActiveOffset, &active) || !active) {
        return false;
    }
    if (is_player) {
        uint32_t relation_active = 0;
        if (!ReadWord(memory, static_cast<uint64_t>(mission_agent) +
                              kAgentRelationActiveOffset, &relation_active) ||
            relation_active != 1u) {
            return false;
        }
    }

    uint32_t group = 0;
    if (!ReadWord(memory, static_cast<uint64_t>(mission_agent) +
                          kAgentRelationGroupOffset, &group)) {
        return false;
    }
    result->group = static_cast<int32_t>(group);
    result->identity = mission_id;
    return true;
}

inline bool AgentsAreFriends(const GuestMemoryView& memory, uint32_t mission,
                             uint32_t player_agent, uint32_t candidate_agent,
                             uint32_t agent_count) {
    AgentRelationGroup player{};
    AgentRelationGroup candidate{};
    if (!ResolveRelationGroup(memory, mission, player_agent, agent_count,
                              true, &player) ||
        !ResolveRelationGroup(memory, mission, candidate_agent, agent_count,
                              false, &candidate)) {
        return false;
    }

    uint8_t has_special_groups = 0;
    uint32_t groups_begin = 0;
    uint32_t groups_end = 0;
    if (ReadByte(memory, static_cast<uint64_t>(player_agent) +
                         kAgentSpecialGroupsFlagOffset, &has_special_groups) &&
        has_special_groups != 0 &&
        ReadWord(memory, static_cast<uint64_t>(player_agent) +
                         kAgentSpecialGroupsBeginOffset, &groups_begin) &&
        ReadWord(memory, static_cast<uint64_t>(player_agent) +
                         kAgentSpecialGroupsEndOffset, &groups_end) &&
        groups_end > groups_begin && ((groups_end - groups_begin) % 4u) == 0u) {
        const uint32_t group_count = (groups_end - groups_begin) / 4u;
        if (group_count <= kMaxScannedMissionAgents && memory.contains &&
            memory.contains(memory.context, groups_begin,
                            static_cast<uint64_t>(groups_end - groups_begin))) {
            for (uint32_t index = 0; index < group_count; ++index) {
                uint32_t member_id = 0;
                if (!ReadWord(memory, static_cast<uint64_t>(groups_begin) +
                                      4u * index, &member_id)) {
                    return false;
                }
                if (static_cast<int32_t>(member_id) != candidate.identity) continue;
                uint32_t relation_table = 0;
                uint32_t relation = 0;
                if (!ReadWord(memory, static_cast<uint64_t>(player_agent) +
                                      kAgentSpecialGroupRelationsOffset,
                              &relation_table) || !relation_table ||
                    !ReadWord(memory, static_cast<uint64_t>(relation_table) +
                                      4u * index, &relation)) {
                    return false;
                }
                return relation == 1u;
            }
        }
    }

    if (player.group < 0 || player.group > 7 ||
        candidate.group < 0 || candidate.group > 7) {
        return false;
    }
    const uint64_t relation_address = static_cast<uint64_t>(mission) +
        kMissionTeamRelationOffset + 48u * static_cast<uint32_t>(player.group) +
        4u * static_cast<uint32_t>(candidate.group);
    uint32_t relation = 0;
    return ReadWord(memory, relation_address, &relation) && relation == 1u;
}

inline bool HasFriendlyOrderTarget(const GuestMemoryView& memory,
                                   uint32_t mission, uint32_t player_agent,
                                   int32_t player_agent_id) {
    uint32_t agent_count = 0;
    if (player_agent_id < 0 ||
        !ReadWord(memory, static_cast<uint64_t>(mission) +
                          kMissionAgentCountOffset, &agent_count) ||
        agent_count > kMaxScannedMissionAgents ||
        static_cast<uint32_t>(player_agent_id) >= agent_count) {
        return false;
    }

    for (uint32_t id = 0; id < agent_count; ++id) {
        if (id == static_cast<uint32_t>(player_agent_id)) continue;
        uint32_t candidate = 0;
        uint32_t active = 0;
        if (!ReadMissionAgent(memory, mission, id, agent_count, &candidate) ||
            !ReadWord(memory, static_cast<uint64_t>(candidate) +
                              kAgentActiveOffset, &active) || !active) {
            continue;
        }
        if (candidate == player_agent) continue;
        if (AgentsAreFriends(memory, mission, player_agent, candidate,
                             agent_count)) {
            return true;
        }
    }
    return false;
}

inline bool HasWieldedItem(const GuestMemoryView& memory, uint32_t agent) {
    for (uint32_t hand = 0; hand < 2u; ++hand) {
        uint32_t slot_word = 0;
        if (!ReadWord(memory, static_cast<uint64_t>(agent) +
                              kAgentWieldedItemOffset + 4u * hand, &slot_word)) {
            continue;
        }
        const int32_t slot = static_cast<int32_t>(slot_word);
        if (slot < 0 || slot >= 16) continue;
        uint32_t item_id = 0;
        const uint64_t item_id_address = static_cast<uint64_t>(agent) +
            kAgentInventoryItemOffset + kAgentInventoryStride *
            static_cast<uint32_t>(slot);
        if (ReadWord(memory, item_id_address, &item_id) &&
            static_cast<int32_t>(item_id) >= 0) {
            return true;
        }
    }
    return false;
}

inline bool ReadItemKind(const GuestMemoryView& memory, uint32_t item_kinds,
                         int32_t item_id, uint32_t* kind_address,
                         uint32_t* type_flags) {
    if (item_id < 0 || !kind_address || !type_flags) return false;
    const uint64_t kind = static_cast<uint64_t>(item_kinds) +
        static_cast<uint64_t>(static_cast<uint32_t>(item_id)) * kItemStride;
    if (kind > std::numeric_limits<uint32_t>::max() ||
        !ReadWord(memory, kind + kItemTypeFlagsOffset, type_flags)) {
        return false;
    }
    *kind_address = static_cast<uint32_t>(kind);
    return true;
}

inline int32_t CandidateWieldIndex(const GuestMemoryView& memory,
                                   uint32_t agent, uint32_t item_kinds,
                                   int32_t slot, bool main_hand_selected) {
    if (slot < 0 || static_cast<uint32_t>(slot) >= kAgentInventoryCount) return -1;
    const uint64_t item_address = static_cast<uint64_t>(agent) +
        kAgentInventoryItemOffset + kAgentInventoryStride *
        static_cast<uint32_t>(slot);
    uint32_t item_word = 0;
    if (!ReadWord(memory, item_address, &item_word)) return -1;
    const int32_t item_id = static_cast<int32_t>(item_word);
    uint32_t kind_address = 0;
    uint32_t type_flags = 0;
    if (!ReadItemKind(memory, item_kinds, item_id, &kind_address, &type_flags)) {
        return -1;
    }
    const uint32_t type = type_flags & 0xFFu;
    uint32_t restriction_word = 0;
    uint32_t state = 0;
    if (!ReadWord(memory, static_cast<uint64_t>(agent) +
                          kAgentWeaponRestrictionOffset, &restriction_word) ||
        !ReadWord(memory, static_cast<uint64_t>(agent) +
                          kAgentInventoryStateOffset + kAgentInventoryStride *
                          static_cast<uint32_t>(slot), &state)) {
        return -1;
    }
    const int32_t restriction = static_cast<int32_t>(restriction_word);

    if (type == kItemShieldType) {
        uint32_t offhand_restriction = 0;
        if (!ReadWord(memory, static_cast<uint64_t>(agent) +
                              kAgentOffhandRestrictionOffset,
                      &offhand_restriction)) {
            return -1;
        }
        const bool permitted = !main_hand_selected || offhand_restriction == 0u;
        return permitted && (restriction < 0 ||
                             (type_flags & kAgentBoundWeaponFlag) == 0u) &&
                       (state & 0xFFFFu) != 0u
            ? 1 : -1;
    }

    if (restriction >= 0 && (type_flags & kAgentBoundWeaponFlag) != 0u) {
        return -1;
    }
    if (type != kItemThrownType) {
        return (type_flags & kTwoHandedWieldFlag) != 0u ? 0 : -1;
    }

    uint32_t max_ammo_word = 0;
    if (!ReadWord(memory, static_cast<uint64_t>(kind_address) + kItemMaxAmmoOffset,
                  &max_ammo_word)) {
        return -1;
    }
    const int32_t max_ammo = static_cast<int32_t>(max_ammo_word);
    int64_t total = max_ammo > 0 ? max_ammo : 0;
    if (max_ammo > 0 && (state & 0xFF000000u) == 0x2A000000u) {
        const float scaled = static_cast<float>(max_ammo) * 0.13f;
        const float rounded_down = std::floor(scaled);
        int32_t bonus = static_cast<int32_t>(rounded_down);
        if (scaled - rounded_down > 0.5f) ++bonus;
        if (bonus < 1) bonus = 1;
        total += bonus;
    }
    return total - static_cast<int64_t>(state & 0xFFu) >= 1 ? 0 : -1;
}

inline int64_t RemainingAmmo(const GuestMemoryView& memory,
                             uint32_t kind_address, uint32_t item_state) {
    uint32_t max_ammo_word = 0;
    if (!ReadWord(memory, static_cast<uint64_t>(kind_address) + kItemMaxAmmoOffset,
                  &max_ammo_word)) {
        return std::numeric_limits<int64_t>::min();
    }
    const int32_t max_ammo = static_cast<int32_t>(max_ammo_word);
    int64_t total = max_ammo > 0 ? max_ammo : 0;
    if (max_ammo > 0 && (item_state & 0xFF000000u) == 0x2A000000u) {
        const float scaled = static_cast<float>(max_ammo) * 0.13f;
        const float rounded_down = std::floor(scaled);
        int32_t bonus = static_cast<int32_t>(rounded_down);
        if (scaled - rounded_down > 0.5f) ++bonus;
        if (bonus < 1) bonus = 1;
        total += bonus;
    }
    return total - static_cast<int64_t>(item_state & 0xFFu);
}

inline bool SameWeaponStack(const GuestMemoryView& memory, uint32_t agent,
                            uint32_t item_kinds, int32_t selected_slot,
                            int32_t candidate_slot) {
    if (selected_slot < 0 || candidate_slot < 0) return false;
    const uint64_t selected_address = static_cast<uint64_t>(agent) +
        kAgentInventoryItemOffset + kAgentInventoryStride *
        static_cast<uint32_t>(selected_slot);
    const uint64_t candidate_address = static_cast<uint64_t>(agent) +
        kAgentInventoryItemOffset + kAgentInventoryStride *
        static_cast<uint32_t>(candidate_slot);
    uint32_t selected_id_word = 0;
    uint32_t candidate_id_word = 0;
    uint32_t selected_state = 0;
    uint32_t candidate_state = 0;
    if (!ReadWord(memory, selected_address, &selected_id_word) ||
        !ReadWord(memory, candidate_address, &candidate_id_word) ||
        selected_id_word != candidate_id_word ||
        !ReadWord(memory, static_cast<uint64_t>(agent) +
                          kAgentInventoryStateOffset + kAgentInventoryStride *
                          static_cast<uint32_t>(selected_slot), &selected_state) ||
        !ReadWord(memory, static_cast<uint64_t>(agent) +
                          kAgentInventoryStateOffset + kAgentInventoryStride *
                          static_cast<uint32_t>(candidate_slot), &candidate_state)) {
        return false;
    }
    uint32_t kind_address = 0;
    uint32_t type_flags = 0;
    if (!ReadItemKind(memory, item_kinds,
                      static_cast<int32_t>(selected_id_word),
                      &kind_address, &type_flags)) {
        return false;
    }
    if ((type_flags & 0xFFu) == kItemShieldType &&
        (selected_state & 0xFFFFu) != (candidate_state & 0xFFFFu)) {
        return false;
    }
    if ((selected_state & 0xFF000000u) != (candidate_state & 0xFF000000u)) {
        return false;
    }
    if ((type_flags & 0xFFu) == kItemThrownType) return true;
    const int64_t selected_ammo = RemainingAmmo(memory, kind_address, selected_state);
    const int64_t candidate_ammo = RemainingAmmo(memory, kind_address, candidate_state);
    return selected_ammo != std::numeric_limits<int64_t>::min() &&
           candidate_ammo != std::numeric_limits<int64_t>::min() &&
           selected_ammo == candidate_ammo;
}

inline bool CanCycleWeapon(const GuestMemoryView& memory, uint32_t agent,
                           uint32_t item_kinds, int32_t hand) {
    if (hand < 0 || hand > 1) return false;
    uint32_t main_slot_word = 0;
    uint32_t offhand_slot_word = 0;
    uint32_t offhand_restriction = 0;
    if (!ReadWord(memory, static_cast<uint64_t>(agent) +
                          kAgentWieldedItemOffset, &main_slot_word) ||
        !ReadWord(memory, static_cast<uint64_t>(agent) +
                          kAgentWieldedItemOffset + 4u, &offhand_slot_word) ||
        !ReadWord(memory, static_cast<uint64_t>(agent) +
                          kAgentOffhandRestrictionOffset, &offhand_restriction)) {
        return false;
    }
    const int32_t original_main = static_cast<int32_t>(main_slot_word);
    const int32_t original_offhand = static_cast<int32_t>(offhand_slot_word);
    if (original_main < -1 || original_main >=
            static_cast<int32_t>(kAgentInventoryCount) ||
        original_offhand < -1 || original_offhand >=
            static_cast<int32_t>(kAgentInventoryCount)) {
        return false;
    }

    int32_t next_main = original_main;
    int32_t next_offhand = original_offhand;
    int32_t selected_slot = hand == 0 ? next_main : next_offhand;
    bool scan_inventory = original_main < 0 || hand != 1 ||
                          offhand_restriction == 0u;
    if (!scan_inventory) {
        next_main = -1;
        if (original_offhand < 0) {
            scan_inventory = true;
        } else {
            return next_main != original_main;
        }
    }

    if (scan_inventory) {
        int32_t candidate_slot = selected_slot;
        bool found = false;
        for (uint32_t attempt = 0; attempt < kAgentInventoryCount; ++attempt) {
            candidate_slot = (candidate_slot + 1) %
                             static_cast<int32_t>(kAgentInventoryCount);
            if (hand == 0) next_main = candidate_slot;
            else next_offhand = candidate_slot;

            if (selected_slot >= 0 &&
                SameWeaponStack(memory, agent, item_kinds, selected_slot,
                                candidate_slot)) {
                continue;
            }
            // Agent::equip_next_weapon passes the sign of the original main
            // wielded slot to get_weapon_wield_index.  Do not derive this
            // from next_main: while the main hand is empty, a candidate
            // being considered must still be allowed to occupy the offhand.
            const bool main_selected = original_main >= 0;
            if (CandidateWieldIndex(memory, agent, item_kinds, candidate_slot,
                                    main_selected) == hand) {
                found = true;
                break;
            }
        }

        if (hand != 0 && selected_slot >= candidate_slot) {
            next_offhand = -1;
        } else if (!found) {
            if (hand == 0) next_main = selected_slot;
            else next_offhand = selected_slot;
        }
    }

    return next_main != original_main || next_offhand != original_offhand;
}

inline bool ContextActionAvailable(int32_t action_code, uint32_t can_perform) {
    return action_code != -1 && can_perform != 0u;
}

inline bool PlayerMounted(const GuestMemoryView& memory, uint32_t player_agent) {
    uint32_t horse_agent_id = 0;
    return ReadWord(memory, static_cast<uint64_t>(player_agent) +
                            kAgentHorseAgentIdOffset, &horse_agent_id) &&
           static_cast<int32_t>(horse_agent_id) >= 0;
}

inline uint32_t BuildStateMask(bool context_action, bool can_drop,
                               bool can_issue_orders, bool can_cycle_weapon,
                               bool can_cycle_shield, bool can_sheath,
                               bool player_mounted) {
    return (context_action ? kContextAction : 0u) |
           (can_drop ? kDropItem : 0u) |
           (can_issue_orders ? kIssueOrders : 0u) |
           (can_cycle_weapon ? kCycleWeapon : 0u) |
           (can_cycle_shield ? kCycleShield : 0u) |
           (can_sheath ? kSheathWeapon : 0u) |
           (player_mounted ? kPlayerMounted : 0u);
}

}  // namespace warband::input::mission_buttons
