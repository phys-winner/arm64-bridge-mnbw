package com.taleworlds.mbwarband;

/** Android-free hit testing for contextual prompts and the battle action wheel. */
public final class MBBattleTouchPolicy {
    private static final float PROMPT_LEFT = 0.25f;
    private static final float PROMPT_RIGHT = 0.75f;
    private static final float PROMPT_TOP = 0.68f;
    private static final float PROMPT_BOTTOM = 0.86f;
    private static final int RADIAL_ACTION_COUNT = 8;
    static final int MULTIPLAYER_INFO_PRESENTATION_ID = 4;
    static final int MULTIPLAYER_POLL_PRESENTATION_ID = 25;
    // The guest dismount command is a hold action.  Keep Use down beyond the
    // game's one-second unmount threshold; a normal interaction still consumes
    // the initial edge immediately, so the longer release cannot delay it.
    private static final long CONTEXT_ACTION_HOLD_MILLIS = 1500L;

    private MBBattleTouchPolicy() {
    }

    public static boolean hitsContextPrompt(float x, float y, float width, float height) {
        if (!MBButtonLayoutPolicy.finite(x) || !MBButtonLayoutPolicy.finite(y)
                || !MBButtonLayoutPolicy.finite(width) || !MBButtonLayoutPolicy.finite(height)
                || width <= 0.0f || height <= 0.0f) {
            return false;
        }
        float xFraction = x / width;
        float yFraction = y / height;
        return xFraction >= PROMPT_LEFT && xFraction <= PROMPT_RIGHT
                && yFraction >= PROMPT_TOP && yFraction <= PROMPT_BOTTOM;
    }

    /** Overlay buttons always own their visible circle, including the guest prompt area. */
    public static boolean contextPromptOwnsTap(boolean overlayButtonHit,
                                               float x, float y,
                                               float width, float height) {
        return !overlayButtonHit && hitsContextPrompt(x, y, width, height);
    }

    /** Native order rows may consume only touches which began outside overlay buttons. */
    public static boolean mayForwardNativeOrder(boolean startedOnOverlayButton,
                                                boolean cancelled, boolean moved) {
        return !startedOnOverlayButton && !cancelled && !moved;
    }

    /** The touch controls own input only while the tactical mission window is active. */
    public static boolean shouldCaptureMissionOverlayTouch(boolean tacticalMission,
                                                            boolean missionSession) {
        return missionSession && tacticalMission;
    }

    public static boolean shouldCaptureMissionOverlayTouch(boolean tacticalMission,
                                                            boolean missionSession,
                                                            int activePresentationId) {
        // Presentations include HUDs supplied by modules. Their table indices
        // cannot decide whether the entire mission loses its touch controls.
        // Individual UI bounds own their pointers in MBTouchOverlay instead.
        return shouldCaptureMissionOverlayTouch(tacticalMission, missionSession);
    }

    /** Presentation 4 serves both the welcome message and Show Game Rules. */
    public static boolean shouldConfirmMultiplayerWelcomeTap(int activePresentationId) {
        return activePresentationId == MULTIPLAYER_INFO_PRESENTATION_ID;
    }

    public static long contextActionHoldMillis() {
        return CONTEXT_ACTION_HOLD_MILLIS;
    }

    /** Returns a stable top-first sector index, or -1 outside the selection ring. */
    public static int radialActionIndex(float x, float y, float centerX, float centerY,
                                        float innerRadius, float outerRadius) {
        if (!MBButtonLayoutPolicy.finite(x) || !MBButtonLayoutPolicy.finite(y)
                || !MBButtonLayoutPolicy.finite(centerX)
                || !MBButtonLayoutPolicy.finite(centerY)
                || !MBButtonLayoutPolicy.finite(innerRadius)
                || !MBButtonLayoutPolicy.finite(outerRadius)
                || innerRadius < 0.0f || outerRadius <= innerRadius) {
            return -1;
        }
        float dx = x - centerX;
        float dy = y - centerY;
        double distance = Math.sqrt((double)dx * dx + (double)dy * dy);
        if (distance < innerRadius || distance > outerRadius) return -1;
        double angle = Math.atan2(dy, dx) + Math.PI * 0.5;
        double circle = Math.PI * 2.0;
        angle %= circle;
        if (angle < 0.0) angle += circle;
        int index = (int)Math.floor((angle + circle / (RADIAL_ACTION_COUNT * 2.0))
                / (circle / RADIAL_ACTION_COUNT));
        return index & (RADIAL_ACTION_COUNT - 1);
    }

    /** Keeps a radial menu inside the visible surface when its anchor is near an edge. */
    public static void clampRadialMenuCenter(float anchorX, float anchorY,
                                             float width, float height, float extent,
                                             float[] output) {
        if (output == null || output.length < 2) {
            throw new IllegalArgumentException("radial menu center output needs two axes");
        }
        if (!MBButtonLayoutPolicy.finite(anchorX) || !MBButtonLayoutPolicy.finite(anchorY)
                || !MBButtonLayoutPolicy.finite(width) || !MBButtonLayoutPolicy.finite(height)
                || !MBButtonLayoutPolicy.finite(extent)
                || width <= 0.0f || height <= 0.0f || extent < 0.0f) {
            output[0] = 0.0f;
            output[1] = 0.0f;
            return;
        }
        float minX = extent;
        float maxX = width - extent;
        float minY = extent;
        float maxY = height - extent;
        output[0] = minX <= maxX ? Math.max(minX, Math.min(maxX, anchorX)) : width * 0.5f;
        output[1] = minY <= maxY ? Math.max(minY, Math.min(maxY, anchorY)) : height * 0.5f;
    }

    public static void radialActionCenter(float centerX, float centerY, float radius,
                                          int index, float[] output) {
        if (output == null || output.length < 2) {
            throw new IllegalArgumentException("radial center output needs two axes");
        }
        if (!MBButtonLayoutPolicy.finite(centerX) || !MBButtonLayoutPolicy.finite(centerY)
                || !MBButtonLayoutPolicy.finite(radius) || radius < 0.0f
                || index < 0 || index >= RADIAL_ACTION_COUNT) {
            output[0] = 0.0f;
            output[1] = 0.0f;
            return;
        }
        double angle = -Math.PI * 0.5 + index * Math.PI * 2.0 / RADIAL_ACTION_COUNT;
        output[0] = centerX + (float)(Math.cos(angle) * radius);
        output[1] = centerY + (float)(Math.sin(angle) * radius);
    }
}
