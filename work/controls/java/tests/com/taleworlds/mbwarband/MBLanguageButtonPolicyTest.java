package com.taleworlds.mbwarband;

public final class MBLanguageButtonPolicyTest {
    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    private static void checkClose(float actual, float expected, String message) {
        if (Math.abs(actual - expected) > 0.0001f) {
            throw new AssertionError(message + ": expected " + expected + ", got " + actual);
        }
    }

    private static void testWideGutterPlacement() {
        checkClose(MBLanguageButtonPolicy.sideGutter(2340, 1080), 450.0f,
                "Xiaomi centered 4:3 side gutter");
        MBLanguageButtonPolicy.Placement placement = MBLanguageButtonPolicy.resolve(
                true, true, true, 2340, 1080, 3.0f, 0, 0);
        check(placement.visible, "wide options selector hidden");
        check(placement.inSideGutter, "wide options selector must use its empty gutter");
        check(placement.size == 144, "48 dp touch target");
        check(placement.x >= 1890 && placement.x + placement.size <= 2340,
                "wide selector escaped right gutter");
    }

    private static void testNarrowFallbackPlacement() {
        MBLanguageButtonPolicy.Placement fourByThree = MBLanguageButtonPolicy.resolve(
                true, true, true, 1440, 1080, 2.0f, 0, 0);
        check(fourByThree.visible, "4:3 options screen must keep touch selector reachable");
        check(!fourByThree.inSideGutter, "4:3 options screen has no gutter");
        check(fourByThree.x == 1320 && fourByThree.y == 144 && fourByThree.size == 96,
                "4:3 selector must sit below the RB touch target");

        MBLanguageButtonPolicy.Placement nearFourByThree = MBLanguageButtonPolicy.resolve(
                true, true, true, 1500, 1080, 2.0f, 17, 31);
        check(nearFourByThree.visible, "near-4:3 options screen must keep selector reachable");
        check(!nearFourByThree.inSideGutter, "small gutter cannot hold a 48 dp target");
        check(nearFourByThree.x + nearFourByThree.size <= 1500 - 31,
                "right safe inset ignored");
        check(nearFourByThree.y >= 17 + nearFourByThree.size,
                "top safe inset or RB avoidance ignored");
    }

    private static void testLifecycleAndImpossibleBounds() {
        for (boolean[] state : new boolean[][] {
                {false, true, true},
                {true, false, true},
                {true, true, false}
        }) {
            check(!MBLanguageButtonPolicy.resolve(
                    state[0], state[1], state[2], 2340, 1080, 3.0f, 0, 0).visible,
                    "invalid lifecycle state showed selector");
        }
        check(!MBLanguageButtonPolicy.resolve(
                true, true, true, 100, 100, 3.0f, 0, 0).visible,
                "impossibly small window showed selector");
        check(!MBLanguageButtonPolicy.resolve(
                true, true, true, 1440, 1080, Float.NaN, 0, 0).visible,
                "NaN density showed selector");
    }

    public static void main(String[] args) {
        testWideGutterPlacement();
        testNarrowFallbackPlacement();
        testLifecycleAndImpossibleBounds();
        System.out.println("MBLanguageButtonPolicyTest: PASS");
    }
}
