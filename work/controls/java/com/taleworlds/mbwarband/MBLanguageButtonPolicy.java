package com.taleworlds.mbwarband;

/**
 * Pure placement policy for the touch-controls entry shown while the native
 * Options screen is open.  The floating selector is intentionally hidden on
 * every other screen: touch settings belong to the game's own settings UI.
 */
public final class MBLanguageButtonPolicy {
    public static final class Placement {
        public final boolean visible;
        public final int x;
        public final int y;
        public final int size;
        public final boolean inSideGutter;

        private Placement(boolean visible, int x, int y, int size, boolean inSideGutter) {
            this.visible = visible;
            this.x = x;
            this.y = y;
            this.size = size;
            this.inSideGutter = inSideGutter;
        }
    }

    private static final Placement HIDDEN = new Placement(false, 0, 0, 0, false);

    private MBLanguageButtonPolicy() {
    }

    public static float sideGutter(int width, int height) {
        if (width <= 0 || height <= 0) return 0.0f;
        float viewportWidth = Math.min((float)width, height * 4.0f / 3.0f);
        return Math.max(0.0f, (width - viewportWidth) * 0.5f);
    }

    /**
     * Resolves a reachable 48 dp selector shown only while the native options
     * window is active. Wide displays use the right letterbox gutter; narrow
     * and 4:3 displays fall back to the safe top-right corner inside the game
     * viewport, immediately below the synthetic RB band.
     */
    public static Placement resolve(boolean attached, boolean nativeReady,
                                    boolean optionsWindowActive,
                                    int width, int height, float density,
                                    int safeInsetTop, int safeInsetRight) {
        if (!attached || !nativeReady || !optionsWindowActive || width <= 0 || height <= 0
                || !(density > 0.0f) || Float.isInfinite(density)) {
            return HIDDEN;
        }
        int size = Math.max(1, Math.round(48.0f * density));
        int margin = Math.max(1, Math.round(12.0f * density));
        int topInset = Math.max(0, safeInsetTop);
        int rightInset = Math.max(0, safeInsetRight);
        if (size + margin * 2 > width - rightInset || size + margin * 2 > height - topInset) {
            return HIDDEN;
        }

        float gutter = sideGutter(width, height);
        boolean useGutter = gutter >= size + margin * 2.0f + rightInset;
        int x;
        if (useGutter) {
            float viewportWidth = Math.min((float)width, height * 4.0f / 3.0f);
            float viewportRight = (width + viewportWidth) * 0.5f;
            float usableGutter = gutter - rightInset;
            x = Math.round(viewportRight + (usableGutter - size) * 0.5f);
        } else {
            x = width - rightInset - margin - size;
        }
        int y = useGutter ? topInset + margin : topInset + size + margin * 2;
        if (y + size + margin > height) return HIDDEN;
        return new Placement(true, x, y, size, useGutter);
    }
}
