#ifndef WARBAND_TEXT_INPUT_QUEUE_H
#define WARBAND_TEXT_INPUT_QUEUE_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace warband::text_input {

inline constexpr std::size_t kMaxEditCodePoints = 256;
inline constexpr std::int32_t kMaxDeleteBefore = 256;

// A maximal accepted edit contains 256 backspaces, 256 inserted BMP units,
// and one submit command.  1024 keeps the queue bounded while allowing almost
// two such edits to wait for the guest's one-codepoint-per-frame consumer.
inline constexpr std::size_t kQueueCapacity = 1024;
static_assert(kQueueCapacity >=
              kMaxEditCodePoints + static_cast<std::size_t>(kMaxDeleteBefore) + 1u);

inline constexpr std::uint32_t kBackspaceCodePoint = 0x00000008u;
inline constexpr std::uint32_t kSubmitCodePoint = 0x0000000Du;
inline constexpr std::uint32_t kPendingSlotEmpty = 0xFFFFFFFFu;

constexpr bool IsAllowedUtf16Unit(std::uint16_t unit) noexcept {
    return (unit >= 0x0020u && unit <= 0x007Eu) ||
           (unit >= 0x00A0u && unit <= 0xD7FFu) ||
           (unit >= 0xE000u && unit <= 0xFFFDu);
}

enum class DrainResult {
    Empty,
    Busy,
    Drained,
    WriteNotObserved,
};

// Not internally synchronized.  Production owns every call with g_input_mutex;
// keeping synchronization outside makes the exact protocol host-testable.
class SessionQueue final {
public:
    explicit SessionQueue(std::uint64_t last_token = 0) noexcept
        : last_token_(last_token) {}

    std::uint64_t Begin() noexcept {
        ClearQueue();
        do {
            ++last_token_;  // Unsigned wrap is defined; zero is never issued.
        } while (last_token_ == 0);
        active_token_ = last_token_;
        return active_token_;
    }

    bool Apply(std::uint64_t token, std::int32_t delete_before,
               std::span<const std::uint16_t> inserted, bool submit) noexcept {
        if (token == 0 || token != active_token_ || delete_before < 0 ||
            delete_before > kMaxDeleteBefore || inserted.size() > kMaxEditCodePoints) {
            return false;
        }
        for (const std::uint16_t unit : inserted) {
            if (!IsAllowedUtf16Unit(unit)) return false;
        }

        const std::size_t required = static_cast<std::size_t>(delete_before) +
                                     inserted.size() + (submit ? 1u : 0u);
        if (required > kQueueCapacity - size_) return false;

        for (std::int32_t index = 0; index < delete_before; ++index) {
            Push(kBackspaceCodePoint);
        }
        for (const std::uint16_t unit : inserted) Push(unit);
        if (submit) Push(kSubmitCodePoint);
        return true;
    }

    bool End(std::uint64_t token) noexcept {
        if (token == 0 || token != active_token_) return false;
        active_token_ = 0;
        ClearQueue();
        return true;
    }

    // Exceptional bridge-lifecycle invalidation only.  Normal text-session
    // ownership belongs to Begin()/End(); controls and AInputQueue lifecycle
    // must never call this or they can silently strand a live Java token.
    void InvalidateForBridgeLifecycle() noexcept {
        active_token_ = 0;
        ClearQueue();
        // Preserve last_token_: teardown must invalidate stale callbacks rather
        // than allowing the next session to reuse a recently issued token.
    }

    template <typename ReadSlot, typename WriteSlot>
    DrainResult DrainOne(ReadSlot&& read_slot, WriteSlot&& write_slot) noexcept {
        if (size_ == 0) return DrainResult::Empty;
        if (read_slot() != kPendingSlotEmpty) return DrainResult::Busy;

        const std::uint32_t value = queue_[head_];
        write_slot(value);
        if (read_slot() != value) return DrainResult::WriteNotObserved;

        head_ = (head_ + 1u) % kQueueCapacity;
        --size_;
        return DrainResult::Drained;
    }

    std::uint64_t ActiveToken() const noexcept { return active_token_; }
    std::uint64_t LastToken() const noexcept { return last_token_; }
    std::size_t Size() const noexcept { return size_; }
    bool Empty() const noexcept { return size_ == 0; }

    bool SubmitPending() const noexcept {
        return size_ != 0 && queue_[head_] == kSubmitCodePoint;
    }

    // A live multiplayer chat consumes Done as an Enter key edge rather than
    // CR: the stock textbox releases focus on CR before its chat sender runs.
    bool TakeSubmit() noexcept {
        if (!SubmitPending()) return false;
        head_ = (head_ + 1u) % kQueueCapacity;
        --size_;
        return true;
    }

private:
    void ClearQueue() noexcept {
        head_ = 0;
        size_ = 0;
    }

    void Push(std::uint32_t value) noexcept {
        queue_[(head_ + size_) % kQueueCapacity] = value;
        ++size_;
    }

    std::array<std::uint32_t, kQueueCapacity> queue_{};
    std::size_t head_ = 0;
    std::size_t size_ = 0;
    std::uint64_t last_token_ = 0;
    std::uint64_t active_token_ = 0;
};

}  // namespace warband::text_input

#endif  // WARBAND_TEXT_INPUT_QUEUE_H
