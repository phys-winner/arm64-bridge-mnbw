package com.taleworlds.mbwarband;

public final class MBButtonLayoutPolicyTest {
    private static final float[] DEFAULT_X = {
            .88f, .72f, .90f, .76f, .94f, .78f, .97f, .62f,
            .56f, .55f, .65f, .53f, .61f, .62f, .86f, .95f
    };
    private static final float[] DEFAULT_Y = {
            .75f, .78f, .51f, .56f, .91f, .91f, .68f, .82f,
            .91f, .75f, .55f, .12f, .18f, .38f, .10f, .10f
    };
    private static final float[] RADIUS_DP = {
            42, 34, 30, 26, 28, 24, 24, 24,
            24, 24, 24, 24, 24, 24, 24, 24
    };

    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    private static void checkClose(float actual, float expected, float epsilon, String message) {
        if (Math.abs(actual - expected) > epsilon) {
            throw new AssertionError(message + ": expected " + expected + ", got " + actual);
        }
    }

    private static float[] scaled(float[] values, float scale) {
        float[] result = new float[values.length];
        for (int i = 0; i < values.length; ++i) result[i] = values[i] * scale;
        return result;
    }

    private static void checkPacked(float width, float height,
                                    float safeLeft, float safeTop,
                                    float safeRight, float safeBottom,
                                    float gap, float[] centerX,
                                    float[] centerY, float[] radius) {
        for (int i = 0; i < centerX.length; ++i) {
            check(centerX[i] - radius[i] >= safeLeft - 0.05f
                            && centerX[i] + radius[i] <= width - safeRight + 0.05f
                            && centerY[i] - radius[i] >= safeTop - 0.05f
                            && centerY[i] + radius[i] <= height - safeBottom + 0.05f,
                    "button escaped the safe display rectangle at index " + i);
            for (int j = 0; j < i; ++j) {
                double dx = (double) centerX[i] - centerX[j];
                double dy = (double) centerY[i] - centerY[j];
                double required = (double) radius[i] + radius[j] + gap;
                check(dx * dx + dy * dy + 0.05 >= required * required,
                        "buttons overlap at indices " + j + " and " + i);
            }
        }
    }

    private static void testDefaultDeviceScaleAndReachability() {
        final float width = 2424.0f;
        final float height = 1080.0f;
        final float density = 3.0f;
        float[] rawX = scaled(DEFAULT_X, width);
        float[] rawY = scaled(DEFAULT_Y, height);
        float[] baseRadius = scaled(RADIUS_DP, density);
        float[] centerX = new float[rawX.length];
        float[] centerY = new float[rawY.length];
        float[] radius = new float[rawX.length];
        float scale = MBButtonLayoutPolicy.resolveLayout(width, height,
                0, 0, 0, 0, 1.4f, density * 2.0f, rawX, rawY, baseRadius,
                centerX, centerY, radius);
        checkClose(scale, 1.4f, 0.0001f,
                "default landscape layout should preserve requested 1.4x scale");
        checkPacked(width, height, 0, 0, 0, 0, density * 2.0f,
                centerX, centerY, radius);

        float[] repeatX = new float[rawX.length];
        float[] repeatY = new float[rawY.length];
        float[] repeatRadius = new float[rawX.length];
        float repeatedScale = MBButtonLayoutPolicy.resolveLayout(width, height,
                0, 0, 0, 0, 1.4f, density * 2.0f, rawX, rawY, baseRadius,
                repeatX, repeatY, repeatRadius);
        checkClose(repeatedScale, scale, 0.0f, "packing scale must be deterministic");
        for (int i = 0; i < rawX.length; ++i) {
            check(Float.floatToIntBits(centerX[i]) == Float.floatToIntBits(repeatX[i])
                            && Float.floatToIntBits(centerY[i]) == Float.floatToIntBits(repeatY[i])
                            && Float.floatToIntBits(radius[i]) == Float.floatToIntBits(repeatRadius[i]),
                    "packing coordinates must be deterministic at index " + i);
        }
    }

    private static void testDensityAwareDeviceScale() {
        float pixelScale = MBButtonLayoutPolicy.densityAwareScale(
                2424.0f, 1080.0f, 420.0f / 160.0f);
        checkClose(pixelScale, 1.0f, 0.0001f,
                "Pixel 10a must retain the phone-sized controls");

        float tabletScale = MBButtonLayoutPolicy.densityAwareScale(
                2944.0f, 1840.0f, 340.0f / 160.0f);
        check(tabletScale > 1.60f && tabletScale < 1.61f,
                "HA22AHP1 must receive the stronger tablet control boost");
        check(42.0f * (340.0f / 160.0f) * tabletScale
                        > 42.0f * (340.0f / 160.0f),
                "tablet radius must be larger than density-only sizing");

        checkClose(MBButtonLayoutPolicy.densityAwareScale(
                        Float.NaN, 1080.0f, 2.625f),
                1.0f, 0.0f, "malformed display metrics fallback");
        checkClose(MBButtonLayoutPolicy.densityAwareScale(
                        10000.0f, 10000.0f, 1.0f),
                1.65f, 0.0f, "large-display scale cap");
    }

    private static void testLargeScreenReachTransform() {
        final float width = 2944.0f;
        final float factor = MBButtonLayoutPolicy.densityAwareScale(
                width, 1840.0f, 340.0f / 160.0f);
        float attack = MBButtonLayoutPolicy.compactTowardNearestEdge(
                width * 0.92f, width, factor);
        float block = MBButtonLayoutPolicy.compactTowardNearestEdge(
                width * 0.80f, width, factor);
        float actions = MBButtonLayoutPolicy.compactTowardNearestEdge(
                width * 0.13f, width, factor);
        check(attack > width * 0.92f && block > width * 0.80f,
                "right-side combat buttons must move into thumb reach");
        check(actions < width * 0.13f,
                "left-side action button must move into thumb reach");
        check(attack - block < width * (0.92f - 0.80f),
                "tablet combat cluster must become tighter");

        float[] positions = {0.0f, 17.0f, width * 0.13f, width * 0.49f,
                width * 0.5f, width * 0.51f, width * 0.92f, width};
        for (float position : positions) {
            float compacted = MBButtonLayoutPolicy.compactTowardNearestEdge(
                    position, width, factor);
            float restored = MBButtonLayoutPolicy.expandFromNearestEdge(
                    compacted, width, factor);
            checkClose(restored, position, 0.002f,
                    "reach transform must round-trip at " + position);
        }
        checkClose(MBButtonLayoutPolicy.compactTowardNearestEdge(
                        width * 0.37f, width, 1.0f),
                width * 0.37f, 0.001f, "phone reach transform must be identity");
        checkClose(MBButtonLayoutPolicy.compactTowardNearestEdge(
                        Float.NaN, width, factor),
                width * 0.5f, 0.0f, "malformed reach coordinate fallback");
    }

    private static void testInsetsAndMalformedValues() {
        float[] rawX = {5.0f, Float.NaN};
        float[] rawY = {5.0f, Float.POSITIVE_INFINITY};
        float[] baseRadius = {20.0f, 20.0f};
        float[] centerX = new float[rawX.length];
        float[] centerY = new float[rawY.length];
        float[] radius = new float[rawX.length];
        float scale = MBButtonLayoutPolicy.resolveLayout(500, 300,
                30, 20, 40, 25, Float.NaN, 0, rawX, rawY, baseRadius,
                centerX, centerY, radius);
        checkClose(scale, 1.0f, 0.0001f,
                "malformed centres must use safe midpoint fallback");
        checkPacked(500, 300, 30, 20, 40, 25, 0, centerX, centerY, radius);
        checkClose(MBButtonLayoutPolicy.requestedScale(Float.NaN), 1.0f, 0.0f,
                "NaN preference fallback");
        checkClose(MBButtonLayoutPolicy.requestedScale(99.0f), 1.4f, 0.0f,
                "maximum preference clamp");
        checkClose(MBButtonLayoutPolicy.clampCenter(2.0f, 42.0f, 460.0f),
                42.0f, 0.0f, "left inset clamp");
        checkClose(MBButtonLayoutPolicy.clampCenter(Float.NaN, 42.0f, 460.0f),
                251.0f, 0.0f, "malformed centre fallback");
    }

    private static void testScaleFallbackOnlyWhenNeeded() {
        float[] rawX = {35.0f, 215.0f};
        float[] rawY = {50.0f, 50.0f};
        float[] baseRadius = {50.0f, 50.0f};
        float[] centerX = new float[2];
        float[] centerY = new float[2];
        float[] radius = new float[2];
        float scale = MBButtonLayoutPolicy.resolveLayout(250, 100,
                0, 0, 0, 0, 1.4f, 4.0f, rawX, rawY, baseRadius,
                centerX, centerY, radius);
        check(scale >= 0.95f && scale <= 1.0001f,
                "height-constrained layout should use its largest feasible scale");
        checkPacked(250, 100, 0, 0, 0, 0, 4.0f, centerX, centerY, radius);

        float[] impossibleX = {50.0f, 50.0f};
        float[] impossibleY = {50.0f, 50.0f};
        float impossibleScale = MBButtonLayoutPolicy.resolveLayout(100, 100,
                0, 0, 0, 0, 1.4f, 4.0f,
                impossibleX, impossibleY, baseRadius, centerX, centerY, radius);
        checkClose(impossibleScale, MBButtonLayoutPolicy.MIN_SCALE, 0.0f,
                "impossible layout must retain the documented minimum fallback");
    }

    private static void testMinimumHitTargetsArePackedAndReachable() {
        final float width = 2424.0f;
        final float height = 1080.0f;
        final float density = 3.0f;
        final float minimumHitRadius = density * 24.0f;
        final float gap = density * 2.0f;
        float[] rawX = scaled(DEFAULT_X, width);
        float[] rawY = scaled(DEFAULT_Y, height);
        float[] baseRadius = scaled(RADIUS_DP, density);
        float[] centerX = new float[rawX.length];
        float[] centerY = new float[rawY.length];
        float[] visualRadius = new float[rawX.length];
        float scale = MBButtonLayoutPolicy.resolveLayout(width, height,
                96, 24, 96, 24, 0.6f, gap, minimumHitRadius,
                rawX, rawY, baseRadius, centerX, centerY, visualRadius);
        checkClose(scale, 0.6f, 0.0001f, "minimum requested scale must be retained");
        float[] hitRadius = new float[visualRadius.length];
        for (int index = 0; index < hitRadius.length; ++index) {
            hitRadius[index] = Math.max(visualRadius[index], minimumHitRadius);
        }
        checkPacked(width, height, 96, 24, 96, 24, gap,
                centerX, centerY, hitRadius);
    }

    private static void testNearestHitAndPainterTie() {
        float[] x = {100.0f, 120.0f};
        float[] y = {100.0f, 100.0f};
        float[] radius = {50.0f, 25.0f};
        check(MBButtonLayoutPolicy.hitIndex(118.0f, 100.0f, x, y, radius) == 1,
                "nearest normalized centre did not win overlap");
        float[] sameX = {100.0f, 100.0f};
        float[] sameY = {100.0f, 100.0f};
        float[] sameRadius = {25.0f, 25.0f};
        check(MBButtonLayoutPolicy.hitIndex(100.0f, 100.0f,
                sameX, sameY, sameRadius) == 1,
                "later painter must win an exact tie");
        check(MBButtonLayoutPolicy.hitIndex(Float.NaN, 100.0f,
                sameX, sameY, sameRadius) == -1,
                "malformed touch must miss");
        boolean[] enabled = {true, false};
        check(MBButtonLayoutPolicy.hitIndex(118.0f, 100.0f,
                x, y, radius, enabled) == 0,
                "a hidden overlapping button must not steal a visible hit");
        check(MBButtonLayoutPolicy.hitIndex(118.0f, 100.0f,
                x, y, radius, new boolean[] {true}) == -1,
                "a malformed enabled mask must fail closed");
    }

    public static void main(String[] args) {
        testDefaultDeviceScaleAndReachability();
        testDensityAwareDeviceScale();
        testLargeScreenReachTransform();
        testInsetsAndMalformedValues();
        testScaleFallbackOnlyWhenNeeded();
        testMinimumHitTargetsArePackedAndReachable();
        testNearestHitAndPainterTie();
        System.out.println("MBButtonLayoutPolicyTest: PASS");
    }
}
