#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace warband::input {

// These are the guest's rglAndroid_input_events pairs.  A zero return from
// PopEncodedEvent means "no event", so real event values are returned + 1.
struct DiscreteBinding {
    uint32_t mask;
    uint8_t down_event;
    uint8_t up_event;
};

constexpr uint32_t kDpadUp = 1u << 0;
constexpr uint32_t kDpadDown = 1u << 1;
constexpr uint32_t kDpadLeft = 1u << 2;
constexpr uint32_t kDpadRight = 1u << 3;
constexpr uint32_t kButtonA = 1u << 4;
constexpr uint32_t kButtonB = 1u << 5;
constexpr uint32_t kButtonX = 1u << 6;
constexpr uint32_t kButtonY = 1u << 7;
constexpr uint32_t kButtonL1 = 1u << 8;
constexpr uint32_t kButtonR1 = 1u << 9;
constexpr uint32_t kThumbLeft = 1u << 10;
constexpr uint32_t kThumbRight = 1u << 11;
constexpr uint32_t kButtonStart = 1u << 12;
constexpr uint32_t kButtonBack = 1u << 13;
// Native Enter key edges used to dismiss the multiplayer welcome presentation.
constexpr uint32_t kEnterConfirm = 1u << 14;

constexpr std::array<DiscreteBinding, 15> kDiscreteBindings = {{
    {kDpadUp, 0u, 1u},
    {kDpadDown, 2u, 3u},
    {kDpadLeft, 4u, 5u},
    {kDpadRight, 6u, 7u},
    {kButtonA, 8u, 9u},
    {kButtonB, 10u, 11u},
    {kButtonX, 12u, 13u},
    {kButtonY, 14u, 15u},
    {kButtonL1, 20u, 21u},
    {kButtonR1, 22u, 23u},
    {kThumbLeft, 24u, 25u},
    {kThumbRight, 26u, 27u},
    {kButtonStart, 28u, 29u},
    {kButtonBack, 30u, 31u},
    {kEnterConfirm, 32u, 33u},
}};

constexpr uint32_t AllDiscreteMask() {
    uint32_t result = 0;
    for (const DiscreteBinding& binding : kDiscreteBindings) result |= binding.mask;
    return result;
}

static_assert(AllDiscreteMask() == ((1u << kDiscreteBindings.size()) - 1u));

class DiscreteArbitrator {
public:
    void SetVirtual(uint32_t mask, bool down) {
        ReplaceSource(virtual_held_, mask, down);
    }

    void SetPhysical(uint32_t mask, bool down) {
        ReplaceSource(physical_held_, mask, down);
    }

    void SetSynthetic(uint32_t mask, bool down) {
        ReplaceSource(synthetic_held_, mask, down);
    }

    void SetPhysicalMask(uint32_t held) {
        const uint32_t before = CombinedHeld();
        physical_held_ = held & AllDiscreteMask();
        QueueCombinedChanges(before, CombinedHeld());
    }

    void ResetVirtual() {
        const uint32_t before = CombinedHeld();
        virtual_held_ = 0;
        QueueCombinedChanges(before, CombinedHeld());
    }

    void ResetPhysical() {
        const uint32_t before = CombinedHeld();
        physical_held_ = 0;
        QueueCombinedChanges(before, CombinedHeld());
    }

    void ResetSynthetic() {
        const uint32_t before = CombinedHeld();
        synthetic_held_ = 0;
        QueueCombinedChanges(before, CombinedHeld());
    }

    void ResetAll() {
        virtual_held_ = 0;
        physical_held_ = 0;
        synthetic_held_ = 0;
        delivered_held_ = 0;
        pending_head_ = 0;
        pending_size_ = 0;
        resync_required_ = false;
        overflow_count_ = 0;
        frame_delivered_mask_ = 0;
    }

    // rglKeys::tick snapshots the previous state, then reduces every queued
    // Android event to the final key state.  Two changes of one key in that
    // batch erase its edge. Stop at that FIFO barrier without consuming it.
    uint32_t PopFrameEncodedEvent() {
        uint32_t next_mask = 0;
        if (pending_size_ != 0) {
            next_mask = pending_[pending_head_].mask;
        } else {
            const uint32_t changed = delivered_held_ ^ CombinedHeld();
            for (const DiscreteBinding& binding : kDiscreteBindings) {
                if ((changed & binding.mask) != 0) {
                    next_mask = binding.mask;
                    break;
                }
            }
        }
        if ((next_mask & frame_delivered_mask_) != 0) return 0;
        const uint32_t encoded_event = PopEncodedEvent();
        if (encoded_event != 0) frame_delivered_mask_ |= next_mask;
        return encoded_event;
    }

    // Production calls this only after the guest has completed rglKeys::tick.
    void FinishFrame() { frame_delivered_mask_ = 0; }

    // Returns guest event + 1, or zero when the queue is drained.
    uint32_t PopEncodedEvent() {
        if (pending_size_ != 0) {
            const PendingTransition transition = pending_[pending_head_];
            pending_head_ = (pending_head_ + 1u) % pending_.size();
            --pending_size_;
            SetDelivered(transition.mask, transition.down);
            return static_cast<uint32_t>(transition.event) + 1u;
        }

        const uint32_t changed = delivered_held_ ^ CombinedHeld();
        if (changed != 0) {
            for (const DiscreteBinding& binding : kDiscreteBindings) {
                if ((changed & binding.mask) == 0) continue;
                const bool down = (CombinedHeld() & binding.mask) != 0;
                SetDelivered(binding.mask, down);
                return static_cast<uint32_t>(down ? binding.down_event : binding.up_event) + 1u;
            }
        }

        resync_required_ = false;
        return 0;
    }

    uint32_t CombinedHeld() const {
        return virtual_held_ | physical_held_ | synthetic_held_;
    }

    uint32_t VirtualHeld() const { return virtual_held_; }
    uint32_t PhysicalHeld() const { return physical_held_; }
    uint32_t SyntheticHeld() const { return synthetic_held_; }
    uint32_t DeliveredHeld() const { return delivered_held_; }
    size_t PendingSize() const { return pending_size_; }
    uint32_t OverflowCount() const { return overflow_count_; }
    bool ResyncRequired() const { return resync_required_; }

private:
    struct PendingTransition {
        uint32_t mask;
        uint8_t event;
        bool down;
    };

    static constexpr size_t kPendingCapacity = 64;

    void ReplaceSource(uint32_t& source, uint32_t mask, bool down) {
        mask &= AllDiscreteMask();
        const uint32_t before = CombinedHeld();
        if (down) source |= mask;
        else source &= ~mask;
        QueueCombinedChanges(before, CombinedHeld());
    }

    void QueueCombinedChanges(uint32_t before, uint32_t after) {
        const uint32_t changed = before ^ after;
        if (changed == 0 || resync_required_) return;
        for (const DiscreteBinding& binding : kDiscreteBindings) {
            if ((changed & binding.mask) == 0) continue;
            if (pending_size_ == pending_.size()) {
                ++overflow_count_;
                RequestResync();
                return;
            }
            const bool down = (after & binding.mask) != 0;
            const size_t tail = (pending_head_ + pending_size_) % pending_.size();
            pending_[tail] = {
                binding.mask,
                down ? binding.down_event : binding.up_event,
                down,
            };
            ++pending_size_;
        }
    }

    void SetDelivered(uint32_t mask, bool down) {
        if (down) delivered_held_ |= mask;
        else delivered_held_ &= ~mask;
    }

    void RequestResync() {
        pending_head_ = 0;
        pending_size_ = 0;
        resync_required_ = true;
    }

    uint32_t virtual_held_ = 0;
    uint32_t physical_held_ = 0;
    uint32_t synthetic_held_ = 0;
    uint32_t delivered_held_ = 0;
    uint32_t frame_delivered_mask_ = 0;
    std::array<PendingTransition, kPendingCapacity> pending_{};
    size_t pending_head_ = 0;
    size_t pending_size_ = 0;
    bool resync_required_ = false;
    uint32_t overflow_count_ = 0;
};

}  // namespace warband::input
