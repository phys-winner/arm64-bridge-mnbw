package com.taleworlds.mbwarband;

public final class MBBattleTouchPolicyTest {
    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    private static void checkClose(float actual, float expected, String message) {
        if (Math.abs(actual - expected) > 0.001f) {
            throw new AssertionError(message + ": expected " + expected + ", got " + actual);
        }
    }

    private static void testContextPromptHitbox() {
        final float width = 2400.0f;
        final float height = 1080.0f;
        check(MBBattleTouchPolicy.hitsContextPrompt(width * 0.5f, height * 0.77f,
                width, height), "native center prompt should be tappable");
        check(MBBattleTouchPolicy.hitsContextPrompt(width * 0.25f, height * 0.68f,
                width, height), "prompt boundary should be inclusive");
        check(!MBBattleTouchPolicy.hitsContextPrompt(width * 0.24f, height * 0.77f,
                width, height), "tap outside prompt width must miss");
        check(!MBBattleTouchPolicy.hitsContextPrompt(width * 0.5f, height * 0.87f,
                width, height), "tap outside prompt height must miss");
        check(!MBBattleTouchPolicy.hitsContextPrompt(0.0f, 0.0f, 0.0f, height),
                "zero-sized surface must miss");
        check(!MBBattleTouchPolicy.hitsContextPrompt(Float.NaN, 10.0f, width, height),
                "malformed coordinates must miss");
        check(!MBBattleTouchPolicy.contextPromptOwnsTap(true,
                width * 0.5f, height * 0.77f, width, height),
                "a relocated overlay button must own its prompt-area touch");
        check(MBBattleTouchPolicy.contextPromptOwnsTap(false,
                width * 0.5f, height * 0.77f, width, height),
                "an unobstructed context prompt must remain tappable");
        check(!MBBattleTouchPolicy.mayForwardNativeOrder(true, false, false),
                "orders must not consume an overlay button release");
        check(MBBattleTouchPolicy.mayForwardNativeOrder(false, false, false),
                "a stationary non-button tap may reach native orders");
        check(!MBBattleTouchPolicy.mayForwardNativeOrder(false, false, true),
                "a drag must not activate a native order row");
        check(MBBattleTouchPolicy.shouldCaptureMissionOverlayTouch(true, true),
                "tactical controls capture touches during an active mission");
        check(!MBBattleTouchPolicy.shouldCaptureMissionOverlayTouch(false, true),
                "native mission windows receive input through the activity window");
        check(!MBBattleTouchPolicy.shouldCaptureMissionOverlayTouch(true, false),
                "touch controls do not capture input without an active mission session");
        check(MBBattleTouchPolicy.shouldCaptureMissionOverlayTouch(true, true,
                        MBBattleTouchPolicy.MULTIPLAYER_INFO_PRESENTATION_ID),
                "tactical information windows keep their UI pointer route");
        check(!MBBattleTouchPolicy.shouldCaptureMissionOverlayTouch(false, true,
                        MBBattleTouchPolicy.MULTIPLAYER_INFO_PRESENTATION_ID),
                "an information presentation must not capture another native window's touches");
        check(!MBBattleTouchPolicy.shouldCaptureMissionOverlayTouch(false, true, 5),
                "non-tactical windows retain their native touch route");
        check(MBBattleTouchPolicy.shouldConfirmMultiplayerWelcomeTap(
                        MBBattleTouchPolicy.MULTIPLAYER_INFO_PRESENTATION_ID),
                "system Back confirms the multiplayer information presentation");
        check(!MBBattleTouchPolicy.shouldConfirmMultiplayerWelcomeTap(5),
                "other presentations must not receive the welcome dismiss action");
        check(MBBattleTouchPolicy.shouldCaptureMissionOverlayTouch(true, true, 5),
                "interactive presentations retain gameplay controls outside their bounds");
        check(MBBattleTouchPolicy.shouldCaptureMissionOverlayTouch(true, true,
                        MBBattleTouchPolicy.MULTIPLAYER_POLL_PRESENTATION_ID),
                "a live poll must leave tactical controls active");
        check(MBBattleTouchPolicy.shouldCaptureMissionOverlayTouch(true, true, -1),
                "tactical touch controls remain active when no presentation is open");
        check(MBBattleTouchPolicy.shouldCaptureMissionOverlayTouch(true, true, 51),
                "the supplied minimap HUD must retain tactical controls");
        check(MBBattleTouchPolicy.shouldCaptureMissionOverlayTouch(true, true, 200),
                "a module's unknown presentation index must not disable tactical controls");
        check(!MBBattleTouchPolicy.shouldCaptureMissionOverlayTouch(true, false, 51),
                "a presentation cannot revive an ended mission session");
    }

    private static void testRadialSectors() {
        final float centerX = 120.0f;
        final float centerY = 100.0f;
        final float radius = 70.0f;
        final float[] point = new float[2];
        for (int index = 0; index < 8; ++index) {
            MBBattleTouchPolicy.radialActionCenter(centerX, centerY, radius, index, point);
            check(MBBattleTouchPolicy.radialActionIndex(point[0], point[1],
                    centerX, centerY, 40.0f, 100.0f) == index,
                    "each radial action center should map to its stable sector " + index);
        }
        check(MBBattleTouchPolicy.radialActionIndex(centerX, centerY,
                centerX, centerY, 40.0f, 100.0f) == -1,
                "radial hub is not an action");
        check(MBBattleTouchPolicy.radialActionIndex(centerX, centerY - 101.0f,
                centerX, centerY, 40.0f, 100.0f) == -1,
                "touch beyond the wheel must miss");
        check(MBBattleTouchPolicy.radialActionIndex(Float.POSITIVE_INFINITY, centerY,
                centerX, centerY, 40.0f, 100.0f) == -1,
                "malformed radial coordinate must miss");

        MBBattleTouchPolicy.radialActionCenter(20.0f, 30.0f, 15.0f, 2, point);
        checkClose(point[0], 35.0f, "right sector x");
        checkClose(point[1], 30.0f, "right sector y");
    }

    private static void testRadialMenuStaysOnSurface() {
        float[] center = new float[2];
        MBBattleTouchPolicy.clampRadialMenuCenter(5.0f, 6.0f,
                320.0f, 200.0f, 90.0f, center);
        checkClose(center[0], 90.0f, "left edge clamp");
        checkClose(center[1], 90.0f, "top edge clamp");
        MBBattleTouchPolicy.clampRadialMenuCenter(315.0f, 195.0f,
                320.0f, 200.0f, 90.0f, center);
        checkClose(center[0], 230.0f, "right edge clamp");
        checkClose(center[1], 110.0f, "bottom edge clamp");
        MBBattleTouchPolicy.clampRadialMenuCenter(5.0f, 6.0f,
                100.0f, 80.0f, 60.0f, center);
        checkClose(center[0], 50.0f, "too-small surface horizontal fallback");
        checkClose(center[1], 40.0f, "too-small surface vertical fallback");
        MBBattleTouchPolicy.clampRadialMenuCenter(Float.NaN, 6.0f,
                320.0f, 200.0f, 90.0f, center);
        checkClose(center[0], 0.0f, "malformed center fallback x");
        checkClose(center[1], 0.0f, "malformed center fallback y");
    }

    public static void main(String[] args) {
        check(MBBattleTouchPolicy.contextActionHoldMillis() >= 1000L,
                "context actions must exceed the guest's one-second dismount hold");
        testContextPromptHitbox();
        testRadialSectors();
        testRadialMenuStaysOnSurface();
        System.out.println("MBBattleTouchPolicyTest: PASS");
    }
}
