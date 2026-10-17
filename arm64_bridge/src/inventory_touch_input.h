#pragma once

#include <cstdint>

namespace warband::input {

// Item gestures are claimed on DOWN only when both the concrete occupied slot
// and its parent menu were resolved.  Empty-space gestures remain completely
// unowned so the menu scroller can react on the first MOVE without a hold.
constexpr bool ShouldClaimInventoryItemGesture(
    bool point_resolved, uint32_t slot, uint32_t menu) {
    return point_resolved && slot != 0u && menu != 0u;
}

constexpr bool ShouldStartInventoryDrag(
    bool item_gesture_owned, bool drag_started, bool moved_past_slop) {
    return item_gesture_owned && !drag_started && moved_past_slop;
}

constexpr bool ShouldShowInventoryTapDetails(
    bool item_gesture_owned, bool drag_started, bool long_press_shown,
    bool released) {
    return item_gesture_owned && !drag_started && !long_press_shown && released;
}

enum class InventoryDragPhaseAction {
    Wait,
    DispatchRelease,
    AbortLostGuestDrag,
};

enum class InventoryReleaseAction {
    WaitForReadableState,
    Complete,
    RestoreSource,
};

// Once a destination event has been queued, only a readable, cleared C34 may
// complete bridge cleanup.  A readable active C34 always requires a verified
// source restore; it must never be converted into a timeout cancellation.
constexpr InventoryReleaseAction DecideInventoryReleaseAction(
    bool guest_state_readable, bool guest_drag_active) {
    if (!guest_state_readable) {
        return InventoryReleaseAction::WaitForReadableState;
    }
    return guest_drag_active ? InventoryReleaseAction::RestoreSource
                             : InventoryReleaseAction::Complete;
}

constexpr bool InventoryRecoveryInFlight(
    bool release_event_queued, bool restore_pending,
    bool source_event_queued, bool release_pending) {
    (void)release_pending;
    // Live touch dragging queues the source before the finger lifts.  From
    // that moment until C34 is positively observed clear, lifecycle code must
    // preserve ownership even when release_pending is still false.
    return release_event_queued || restore_pending || source_event_queued;
}

// A queued live source event is lifecycle-sensitive, but its owning Android
// gesture must keep reaching the inventory observer so last_x/last_y and the
// eventual UP are recorded.  Only post-release recovery (or an orphaned
// recovery with no tracked gesture) quarantines later raw touchscreen input.
constexpr bool ShouldQuarantineInventoryRawStream(
    bool recovery_in_flight, bool tracking, bool release_pending,
    bool release_event_queued, bool restore_pending) {
    return recovery_in_flight &&
           (!tracking || release_pending || release_event_queued ||
            restore_pending);
}

// DOWN-time hit testing now decides ownership immediately.  Keep this guard for
// recovery and for fail-closed transitional states, while ordinary empty-space
// scrolling reaches the menu recognizer with inventory_tracking=false.
constexpr bool ShouldDeferInventoryMenuScroll(
    bool inventory_tracking, bool inventory_drag_started) {
    return inventory_tracking && !inventory_drag_started;
}

constexpr bool ShouldPreserveInventoryRecovery(
    bool recovery_in_flight, bool source_awaiting_consumption,
    bool guest_state_readable, bool guest_drag_active) {
    if (!recovery_in_flight) return false;
    // C34 is still zero between queueing the source bits and the next stock
    // inventory pass.  Clearing bridge state in that gap would leave those
    // bits able to pick the item up without any owner on the following frame.
    if (source_awaiting_consumption) return true;
    return !guest_state_readable || guest_drag_active;
}

// The frame hook queues the source immediately before Inventory_window::check,
// so the guest exposes its active-drag field after that frame completes. Once
// that field has been observed, losing it before our release means the guest
// already completed or cancelled the drag and a destination event would start
// a second drag instead of finishing the first one.
constexpr InventoryDragPhaseAction DecideInventoryDragPhase(
    bool release_pending, bool source_just_queued, bool guest_drag_seen,
    bool guest_state_readable, bool guest_drag_active) {
    if (guest_drag_seen && guest_state_readable && !guest_drag_active) {
        return InventoryDragPhaseAction::AbortLostGuestDrag;
    }
    if (!release_pending || source_just_queued || !guest_drag_seen ||
        !guest_state_readable || !guest_drag_active) {
        return InventoryDragPhaseAction::Wait;
    }
    return InventoryDragPhaseAction::DispatchRelease;
}

struct InventoryDropPlan {
    uint32_t menu = 0;
    uint32_t slot = 0;
    bool restores_source = false;

    // Every concrete slot is gated by its parent menu.  Menu-only targets such
    // as Discard are valid, but a slot-only plan must fail closed.
    constexpr bool HasDispatch() const { return menu != 0; }
};

// A concrete destination slot is always authoritative.  Its owning menu is
// queued as the stock loop gate.  A different menu without a slot represents
// a menu-level target such as Discard.  Releasing over empty space in the
// source menu (or outside every menu) restores the item to its source slot.
constexpr InventoryDropPlan PlanInventoryDrop(
    uint32_t source_menu, uint32_t source_slot, uint32_t target_menu,
    uint32_t target_slot) {
    if (target_slot != 0) {
        return {.menu = target_menu, .slot = target_slot};
    }
    if (target_menu != 0 && target_menu != source_menu) {
        return {.menu = target_menu};
    }
    return {
        .menu = source_menu,
        .slot = source_slot,
        .restores_source = true,
    };
}

}  // namespace warband::input
