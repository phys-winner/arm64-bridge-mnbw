package com.taleworlds.mbwarband;

/** Keeps every event in a touch gesture on the same side of the input gate. */
final class MBInputStallPolicy {
    static final int ACTION_DOWN = 0;
    static final int ACTION_UP = 1;
    static final int ACTION_CANCEL = 3;

    private boolean consumingGesture;

    boolean shouldConsume(int action, boolean nativeStarted, boolean guestResponsive) {
        if (action == ACTION_DOWN) {
            consumingGesture = nativeStarted && !guestResponsive;
        }
        boolean consume = consumingGesture;
        if (action == ACTION_UP || action == ACTION_CANCEL) {
            consumingGesture = false;
        }
        return consume;
    }

    void reset() {
        consumingGesture = false;
    }
}
