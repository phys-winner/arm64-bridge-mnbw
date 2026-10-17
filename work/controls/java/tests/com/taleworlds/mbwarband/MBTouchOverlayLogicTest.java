package com.taleworlds.mbwarband;

import java.lang.reflect.Method;
import java.lang.reflect.Modifier;

public final class MBTouchOverlayLogicTest {
    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    private static void checkClose(float actual, float expected, String message) {
        if (Math.abs(actual - expected) > 0.0001f) {
            throw new AssertionError(message + ": expected " + expected + ", got " + actual);
        }
    }

    private static void testLookOwnership() {
        MBTouchOverlay.PointerBook pointers = new MBTouchOverlay.PointerBook();
        pointers.begin(8, MBTouchOverlay.ROLE_LOOK, 50.0f, 60.0f, 1L);
        check(pointers.lookOwner() == 8, "first dedicated look pointer should own look");

        pointers.begin(7, MBTouchOverlay.ROLE_BLOCK, 45.0f, 55.0f, 2L);
        check(pointers.lookOwner() == 7,
                "block drag must immediately preempt passive look for directional blocking");

        pointers.begin(3, MBTouchOverlay.ROLE_ATTACK, 10.0f, 20.0f, 3L);
        check(pointers.lookOwner() == 3, "attack drag must immediately preempt block/look");
        check(!pointers.directionalActionStarted(3),
                "attack trigger must wait for a direction gesture");
        pointers.markDirectionalActionStarted(3, 25L);
        check(pointers.directionalActionStarted(3),
                "directional trigger state must be tracked per pointer");
        check(pointers.directionalActionStartedAt(3) == 25L,
                "directional trigger timestamp must support a minimum pulse");

        pointers.begin(4, MBTouchOverlay.ROLE_ATTACK, 30.0f, 40.0f, 4L);
        check(pointers.lookOwner() == 3, "second attack pointer must not steal look");

        pointers.update(4, 35.0f, 45.0f);
        pointers.end(3);
        check(!pointers.directionalActionStarted(3),
                "released pointer must clear directional trigger state");
        check(pointers.lookOwner() == 4, "remaining attack pointer should take over look");
        checkClose(pointers.lastX[4], 35.0f, "takeover must retain the latest X origin");
        checkClose(pointers.lastY[4], 45.0f, "takeover must retain the latest Y origin");
        pointers.end(4);
        check(pointers.lookOwner() == 7, "block drag should resume after attack release");
        pointers.end(7);
        check(pointers.lookOwner() == 8, "resting look pointer should resume after block release");
        pointers.end(8);
        check(pointers.lookOwner() == -1, "look owner should clear after final look release");

        pointers.begin(32, MBTouchOverlay.ROLE_LOOK, 0.0f, 0.0f, 0L);
        check(pointers.lookOwner() == -1, "out-of-range Android pointer id must be ignored");
    }

    private static void testMoveOwnership() {
        MBTouchOverlay.PointerBook pointers = new MBTouchOverlay.PointerBook();
        pointers.begin(1, MBTouchOverlay.ROLE_MOVE, 10.0f, 20.0f, 1L);
        pointers.begin(2, MBTouchOverlay.ROLE_MOVE, 30.0f, 40.0f, 2L);
        check(pointers.moveOwner() == 1, "second movement finger must not steal movement");
        check(pointers.ownsMove(1), "first movement finger must remain owner");
        pointers.update(2, 35.0f, 45.0f);
        pointers.end(1);
        check(pointers.moveOwner() == 2, "remaining movement finger must take over");
        checkClose(pointers.lastX[2], 35.0f, "movement takeover retains latest X");
        checkClose(pointers.lastY[2], 45.0f, "movement takeover retains latest Y");
        pointers.end(2);
        check(pointers.moveOwner() == -1, "movement owner clears after final release");
    }

    private static void testNativeUiPointerOwnership() {
        MBTouchOverlay.PointerBook pointers = new MBTouchOverlay.PointerBook();
        pointers.begin(3, MBTouchOverlay.ROLE_NATIVE_UI, 1800.0f, 100.0f, 1L);
        check(pointers.lookOwner() == -1 && pointers.moveOwner() == -1,
                "a UI touch must not start camera look or movement");
        pointers.begin(4, MBTouchOverlay.ROLE_MOVE, 100.0f, 700.0f, 2L);
        pointers.begin(5, MBTouchOverlay.ROLE_LOOK, 1300.0f, 600.0f, 3L);
        pointers.update(3, 900.0f, 800.0f);
        check(pointers.role(3) == MBTouchOverlay.ROLE_NATIVE_UI,
                "dragging out of UI bounds must retain UI ownership");
        check(pointers.moveOwner() == 4 && pointers.lookOwner() == 5,
                "gameplay fingers outside UI must remain independent");
        pointers.end(5);
        pointers.end(4);
        check(pointers.lookOwner() == -1 && pointers.moveOwner() == -1,
                "a held UI finger must not become a replacement gameplay owner");
        pointers.end(3);
        check(pointers.role(3) == MBTouchOverlay.ROLE_NONE,
                "releasing UI must clear its pointer role");
        pointers.begin(7, MBTouchOverlay.ROLE_NATIVE_UI, 1700.0f, 150.0f, 4L);
        pointers.reset();
        check(pointers.role(7) == MBTouchOverlay.ROLE_NONE,
                "cancellation must clear UI pointer ownership");
    }

    private static void testGuestSettingsSyncContract() {
        MBTouchOverlay.GuestSettingsSync.Values bounded =
                MBTouchOverlay.GuestSettingsSync.boundedHostValues(
                        true, false, true, true, true, true, true,
                        true, false, true, true, true, true,
                        -8.0f, Float.NaN, Float.POSITIVE_INFINITY, 1.2f,
                        4.0f, 2.5f, 0.1f, 9.0f, -2.0f, 8.0f);
        check(bounded.boolMask() == 0x3D9D, "guest settings bool-mask mapping");
        check(bounded.hdrEnabled && !bounded.dofEnabled,
                "HDR/DOF host proposals must retain their independent values");
        check(bounded.hapticFeedback, "touch haptics should retain the host preference");
        check(!bounded.cameraMouse,
                "legacy Mouse preference must be ignored in favor of Right stick");
        check(bounded.alwaysRun, "always-run host preference should be retained");
        checkClose(bounded.moveSensitivity, 0.5f, "move lower bound");
        checkClose(bounded.gyroSensitivity, 1.25f, "non-finite gyro default");
        checkClose(bounded.gyroDeadzone, 0.012f, "non-finite deadzone default");
        checkClose(bounded.buttonScale, 1.2f, "in-range button scale");
        checkClose(bounded.buttonOpacity, 1.0f, "opacity upper bound");
        checkClose(bounded.fpsLimit, 2.5f, "in-range fps slider position");
        checkClose(bounded.hdrBrightness, 0.5f, "HDR brightness lower bound");
        checkClose(bounded.shadowMode, 2.0f, "shadow mode upper bound");
        checkClose(bounded.mapPanSensitivity, 0.35f, "map pan lower bound");
        checkClose(bounded.reportTextScale, 1.5f, "report scale upper bound");
        checkClose(MBTouchOverlay.GuestSettingsSync.boundHostFloat(9, -4.0f),
                0.5f, "report scale lower bound");
        checkClose(MBTouchOverlay.GuestSettingsSync.boundHostFloat(9, Float.NaN),
                1.0f, "non-finite report scale default");
        check(MBTouchOverlay.GuestSettingsSync.fpsLimitValue(
                        MBTouchOverlay.GuestSettingsSync.fpsLimitChoice(2.4f)) == 90,
                "fps choice snapping");
        check(MBTouchOverlay.GuestSettingsSync.fpsLimitValue(3) == 0,
                "uncapped fps choice maps to zero");

        int[] packet = {
                0x1DEA,
                Float.floatToRawIntBits(1.1f),
                Float.floatToRawIntBits(2.2f),
                Float.floatToRawIntBits(0.02f),
                Float.floatToRawIntBits(0.9f),
                Float.floatToRawIntBits(0.5f),
                Float.floatToRawIntBits(3.0f),
                Float.floatToRawIntBits(0.75f),
                Float.floatToRawIntBits(1.0f),
                Float.floatToRawIntBits(0.7f),
                Float.floatToRawIntBits(1.3f),
        };
        MBTouchOverlay.GuestSettingsSync.Values decoded =
                MBTouchOverlay.GuestSettingsSync.decodeGuestPacket(packet);
        check(decoded != null, "valid guest packet rejected");
        check(!decoded.gyroEnabled && decoded.gyroInvertY
                        && !decoded.menuSwipe && decoded.gyroRangedOnly
                        && !decoded.leftHanded && !decoded.cameraMouse
                        && decoded.alwaysRun
                        && decoded.layoutEditRequested
                        && decoded.hdrEnabled && !decoded.dofEnabled
                        && decoded.autoExposure && decoded.hapticFeedback
                        && decoded.menuAspect && !decoded.fxaaEnabled,
                "guest packet bool decoding");
        checkClose(decoded.floatAt(0), 1.1f, "guest move decoding");
        checkClose(decoded.floatAt(3), 0.9f, "guest scale decoding");
        checkClose(decoded.floatAt(4), 0.5f, "guest opacity decoding");
        checkClose(decoded.floatAt(5), 3.0f, "guest fps decoding");
        checkClose(decoded.floatAt(6), 0.75f, "guest HDR brightness decoding");
        checkClose(decoded.floatAt(7), 1.0f, "guest shadow mode decoding");
        checkClose(decoded.floatAt(8), 0.7f, "guest map pan sensitivity decoding");
        checkClose(decoded.floatAt(9), 1.3f, "guest report scale decoding");
        check(MBTouchOverlay.GuestSettingsSync.FLOAT_COUNT == 10
                        && MBTouchOverlay.GuestSettingsSync.PACKET_WORDS == 11,
                "report scale packet contract");

        int[] badMask = packet.clone();
        badMask[0] |= 0x4000;
        check(MBTouchOverlay.GuestSettingsSync.decodeGuestPacket(badMask) == null,
                "unknown bool bits must reject the whole snapshot");
        int[] badFloat = packet.clone();
        badFloat[3] = Float.floatToRawIntBits(Float.NaN);
        check(MBTouchOverlay.GuestSettingsSync.decodeGuestPacket(badFloat) == null,
                "NaN must reject the whole snapshot");
        int[] outOfRange = packet.clone();
        outOfRange[3] = Float.floatToRawIntBits(0.061f);
        check(MBTouchOverlay.GuestSettingsSync.decodeGuestPacket(outOfRange) == null,
                "out-of-range value must reject the whole snapshot");
        int[] invalidReportScale = packet.clone();
        invalidReportScale[10] = Float.floatToRawIntBits(1.51f);
        check(MBTouchOverlay.GuestSettingsSync.decodeGuestPacket(invalidReportScale) == null,
                "out-of-range report scale must reject the whole snapshot");
        check(MBTouchOverlay.GuestSettingsSync.decodeGuestPacket(new int[5]) == null,
                "truncated packet must fail closed");

        check(MBTouchOverlay.GuestSettingsSync.pollAction(
                        false, false, false, true) ==
                        MBTouchOverlay.GuestSettingsSync.POLL_NONE,
                "unstable window id must not spend the final poll");
        check(MBTouchOverlay.GuestSettingsSync.pollAction(
                        true, true, false, false) ==
                        MBTouchOverlay.GuestSettingsSync.POLL_ACTIVE,
                "Options id 13 must poll");
        check(MBTouchOverlay.GuestSettingsSync.pollAction(
                        true, true, false, true) ==
                        MBTouchOverlay.GuestSettingsSync.POLL_ACTIVE,
                "reopened Options must supersede an armed final poll");
        check(MBTouchOverlay.GuestSettingsSync.pollAction(
                        true, false, true, false) ==
                        MBTouchOverlay.GuestSettingsSync.POLL_ARM_FINAL,
                "first known close must defer the final poll for the frame thread");
        check(MBTouchOverlay.GuestSettingsSync.pollAction(
                        true, false, false, true) ==
                        MBTouchOverlay.GuestSettingsSync.POLL_FINAL,
                "armed close must issue exactly one final poll");
        check(MBTouchOverlay.GuestSettingsSync.pollAction(
                        true, false, false, false) ==
                        MBTouchOverlay.GuestSettingsSync.POLL_NONE,
                "closed Options must not continue polling");

        check("mb_gyro_enabled".equals(
                        MBTouchOverlay.GuestSettingsSync.KEY_GYRO_ENABLED),
                "gyro preference key contract");
        check("mb_gyro_invert_y".equals(
                        MBTouchOverlay.GuestSettingsSync.KEY_GYRO_INVERT_Y),
                "gyro inversion preference key contract");
        check("mb_menu_swipe_navigation".equals(
                        MBTouchOverlay.GuestSettingsSync.KEY_MENU_SWIPE),
                "menu swipe preference key contract");
        check("mb_gyro_ranged_only".equals(
                        MBTouchOverlay.GuestSettingsSync.KEY_GYRO_RANGED_ONLY),
                "gyro ranged-only preference key contract");
        check("mb_move_sensitivity".equals(
                        MBTouchOverlay.GuestSettingsSync.KEY_MOVE_SENSITIVITY),
                "move sensitivity preference key contract");
        check("mb_left_handed".equals(
                        MBTouchOverlay.GuestSettingsSync.KEY_LEFT_HANDED),
                "left-handed preference key contract");
        check("mb_always_run".equals(
                        MBTouchOverlay.GuestSettingsSync.KEY_ALWAYS_RUN),
                "always-run preference key contract");
        check("mb_button_opacity".equals(
                        MBTouchOverlay.GuestSettingsSync.KEY_BUTTON_OPACITY),
                "button opacity preference key contract");
        check("mb_fps_limit".equals(
                        MBTouchOverlay.GuestSettingsSync.KEY_FPS_LIMIT),
                "fps limit preference key contract");
        check("mb_postfx_hdr".equals(
                        MBTouchOverlay.GuestSettingsSync.KEY_HDR_ENABLED),
                "HDR preference key contract");
        check("mb_hdr_brightness".equals(
                        MBTouchOverlay.GuestSettingsSync.KEY_HDR_BRIGHTNESS),
                "HDR brightness preference key contract");
        check("mb_shadow_rendering_mode".equals(
                        MBTouchOverlay.GuestSettingsSync.KEY_SHADOW_MODE),
                "shadow mode preference key contract");
        check("mb_fxaa_enabled".equals(
                        MBTouchOverlay.GuestSettingsSync.KEY_FXAA_ENABLED),
                "FXAA preference key contract");
        check("mb_map_pan_sensitivity".equals(
                        MBTouchOverlay.GuestSettingsSync.KEY_MAP_PAN_SENSITIVITY),
                "map pan sensitivity preference key contract");
        check("mb_report_text_scale".equals(
                        MBTouchOverlay.GuestSettingsSync.KEY_REPORT_TEXT_SCALE),
                "report scale preference key contract");
        check("mb_touch_haptics".equals(
                        MBTouchOverlay.GuestSettingsSync.KEY_HAPTIC_FEEDBACK),
                "touch haptics preference key contract");
        check("mb_gyro_sensitivity".equals(
                        MBTouchOverlay.GuestSettingsSync.KEY_GYRO_SENSITIVITY),
                "gyro sensitivity preference key contract");
        check("mb_gyro_deadzone".equals(
                        MBTouchOverlay.GuestSettingsSync.KEY_GYRO_DEADZONE),
                "gyro deadzone preference key contract");
        check("mb_button_scale".equals(
                        MBTouchOverlay.GuestSettingsSync.KEY_BUTTON_SCALE),
                "button scale preference key contract");
    }

    private static void testInputMathAndActionContract() {
        checkClose(MBTouchOverlay.InputMath.clampAxis(1.8f), 1.0f, "positive axis clamp");
        checkClose(MBTouchOverlay.InputMath.clampAxis(-1.8f), -1.0f, "negative axis clamp");
        float[] axes = new float[2];
        MBTouchOverlay.InputMath.movementAxes(0.5f, 0.25f, false, axes);
        checkClose(axes[0], 0.5f, "analog movement retains horizontal magnitude");
        checkClose(axes[1], 0.25f, "analog movement retains non-cardinal angle");
        MBTouchOverlay.InputMath.movementAxes(0.5f, 0.25f, true, axes);
        checkClose(axes[0], 0.8944272f, "always-run normalizes horizontal axis");
        checkClose(axes[1], 0.4472136f, "always-run preserves arbitrary direction");
        MBTouchOverlay.InputMath.movementAxes(0.04f, 0.04f, false, axes);
        checkClose(axes[0], 0.0f, "movement radial deadzone X");
        checkClose(axes[1], 0.0f, "movement radial deadzone Y");
        MBTouchOverlay.InputMath.movementAxes(2.0f, -2.0f, false, axes);
        checkClose(axes[0], 0.7071068f, "analog movement clamps vector magnitude");
        checkClose(axes[1], -0.7071068f, "analog movement clamps without changing angle");
        checkClose(MBTouchOverlay.InputMath.removeDeadzone(0.01f, 0.012f), 0.0f,
                "gyro deadzone");
        checkClose(MBTouchOverlay.InputMath.removeDeadzone(-0.112f, 0.012f), -0.1f,
                "gyro deadzone must preserve sign and remove discontinuity");
        checkClose(MBTouchOverlay.InputMath.touchLookUnits(100.0f, 2.0f, 1.5f),
                75.0f, "touch look must use density-independent accumulated distance");
        checkClose(MBTouchOverlay.InputMath.touchLookUnits(1000.0f, 1.0f, 2.0f),
                160.0f, "single malformed touch sample must be safely bounded");
        check(!MBTouchOverlay.InputMath.directionGestureReady(11.9f, 0.0f, 2.0f),
                "sub-6dp jitter must not select an attack direction");
        check(MBTouchOverlay.InputMath.directionGestureReady(12.0f, 0.0f, 2.0f),
                "6dp drag must select an attack direction");
        check(MBTouchOverlay.InputMath.directionGestureReady(0.0f, -12.1f, 2.0f),
                "direction threshold must be radial and sign independent");
        checkClose(MBTouchOverlay.InputMath.gyroLookUnits(
                0.01f, 0.02f, 0.1f, 1.0f), 1.44f,
                "gyro must integrate deadzoned angular velocity back into a delta");
        checkClose(MBTouchOverlay.InputMath.gyroLookUnits(
                0.1f, 0.2f, 0.0f, 1.0f), 0.0f,
                "stale gyro samples must not create a camera jump");

        check(MBTouchOverlay.ACTION_KICK == 5, "kick must use proven native action 5");
        check(MBTouchOverlay.ACTION_DPAD_UP == 7, "D-pad Up ABI value must remain stable");
        check(MBTouchOverlay.ACTION_OPEN_ORDERS == 16,
                "orders must use the dedicated guest-frame command, not D-pad Up");
        check(MBTouchOverlay.ACTION_DPAD_DOWN == 8,
                "weapon mode must use proven native action 8");
        check(MBTouchOverlay.ACTION_LB == 9, "LB must use native action 9");
        check(MBTouchOverlay.ACTION_RB == 10, "RB must use native action 10");
        check(MBTouchOverlay.ACTION_DROP == 11, "drop must use native action 11");
        check(MBTouchOverlay.ACTION_ZOOM == 12, "zoom must use native action 12");
        check(MBTouchOverlay.ACTION_VIEW == 13, "camera view must use native action 13");
        check(MBTouchOverlay.ACTION_BACK == 14, "retreat/back must use native action 14");
        check(MBTouchOverlay.ACTION_SHEATH == 15, "sheath must use native action 15");
    }

    private static void testGameLanguageContract() {
        String[] expected = {
                "cns", "cnt", "cz", "de", "en", "es",
                "fr", "hu", "ja", "pl", "ru", "tr"
        };
        check(MBTouchOverlay.GameLanguage.CODES.length == expected.length,
                "language whitelist length");
        check(MBTouchOverlay.GameLanguage.DISPLAY_NAMES.length == expected.length,
                "every language code needs a display name");
        for (int index = 0; index < expected.length; ++index) {
            String code = expected[index];
            check(code.equals(MBTouchOverlay.GameLanguage.CODES[index]),
                    "language whitelist order at " + index);
            check(MBTouchOverlay.GameLanguage.isSupported(code),
                    "whitelisted language rejected: " + code);
            check(code.equals(MBTouchOverlay.GameLanguage.normalize(code)),
                    "whitelisted language changed: " + code);
            check(MBTouchOverlay.GameLanguage.indexOf(code) == index,
                    "language index mismatch: " + code);
            check(code.equals(MBTouchOverlay.GameLanguage.codeAt(index)),
                    "language reverse lookup mismatch: " + code);
            check(MBTouchOverlay.GameLanguage.DISPLAY_NAMES[index].length() > code.length(),
                    "language needs a human-readable display name: " + code);
        }
        check("PREFERENCE".equals(MBTouchOverlay.GameLanguage.PREFERENCES_NAME),
                "SharedPreferences file contract");
        check("mb_game_language".equals(MBTouchOverlay.GameLanguage.PREFERENCE_KEY),
                "SharedPreferences language key contract");
        check("en".equals(MBTouchOverlay.GameLanguage.DEFAULT_CODE), "default language contract");
        for (String invalid : new String[] {null, "", "EN", " en", "cs", "zh"}) {
            check(!MBTouchOverlay.GameLanguage.isSupported(invalid),
                    "non-whitelisted language accepted: " + invalid);
            check("en".equals(MBTouchOverlay.GameLanguage.normalize(invalid)),
                    "invalid language must deterministically fall back to English");
        }
        check("en".equals(MBTouchOverlay.GameLanguage.codeAt(-1)),
                "negative picker index must fall back to English");
        check("en".equals(MBTouchOverlay.GameLanguage.codeAt(expected.length)),
                "oversized picker index must fall back to English");
    }

    private static void testSystemBackPolicy() {
        check(!MBTouchOverlay.SystemBackPolicy.shouldDispatch(
                        false, true, true, -1),
                "system Back must wait until the guest starts");
        check(!MBTouchOverlay.SystemBackPolicy.shouldDispatch(
                        true, false, true, -1),
                "system Back must wait until the guest is ready");
        check(MBTouchOverlay.SystemBackPolicy.shouldDispatch(
                        true, true, true, -1),
                "mission Back must reach the native game action");
        check(MBTouchOverlay.SystemBackPolicy.shouldDispatch(
                        true, true, false, 13),
                "Back in a native submenu must reach the game");
        check(!MBTouchOverlay.SystemBackPolicy.shouldDispatch(
                        true, true, false,
                        MBTouchOverlay.SystemBackPolicy.ROOT_MENU_WINDOW_ID),
                "root-menu Back must not background or close the game");
        check(!MBTouchOverlay.SystemBackPolicy.shouldDispatch(
                        true, true, false, -1),
                "unknown menu state must not background or close the game");
    }

    private static void testMainMenuModButtonPolicy() {
        check("Моды".equals(MBTouchOverlay.MainMenuModButtonPolicy.label(true, 0)),
                "empty Russian module list must not show a redundant zero counter");
        check("Mods".equals(MBTouchOverlay.MainMenuModButtonPolicy.label(false, -1)),
                "empty English module list must fail closed without a counter");
        check("Моды (3)".equals(MBTouchOverlay.MainMenuModButtonPolicy.label(true, 3)),
                "non-empty Russian module list must show its count");
        check("Mods (2)".equals(MBTouchOverlay.MainMenuModButtonPolicy.label(false, 2)),
                "non-empty English module list must show its count");

        MBTouchOverlay.MainMenuModButtonPolicy.Placement realme =
                MBTouchOverlay.MainMenuModButtonPolicy.resolve(2780, 1264, true);
        check(realme.x == 366, "mod row must align with the Realme parchment text area");
        check(realme.y == 980, "mod row must follow Authors on the Realme main menu");
        check(realme.width == 556, "mod row must match the Realme parchment text width");
        check(realme.height == 81, "mod row must match native Realme row height");

        MBTouchOverlay.MainMenuModButtonPolicy.Placement centered =
                MBTouchOverlay.MainMenuModButtonPolicy.resolve(2424, 1080, true);
        check(centered.x == 337, "wide displays must include the centered viewport gutter");
        check(centered.y == 837, "wide-display row Y must scale with game height");
        check(centered.width == 475, "wide-display row width must scale with viewport");
        check(centered.height == 69, "wide-display row height must remain touchable");

        MBTouchOverlay.MainMenuModButtonPolicy.Placement tiny =
                MBTouchOverlay.MainMenuModButtonPolicy.resolve(0, 0, true);
        check(tiny.x >= 0 && tiny.y >= 0 && tiny.width >= 1 && tiny.height >= 1,
                "invalid early decor dimensions must fail safely");
    }

    private static void testLanguageButtonLifecyclePolicy() {
        checkClose(MBLanguageButtonPolicy.sideGutter(2340, 1080),
                450.0f, "Xiaomi centered 4:3 side gutter");
        checkClose(MBLanguageButtonPolicy.sideGutter(2424, 1080),
                492.0f, "Realme centered 4:3 side gutter");
        check(MBLanguageButtonPolicy.resolve(
                        true, true, true, 2340, 1080, 3.0f, 0, 0).visible,
                "attached ready options screen should show touch selector");
        check(!MBLanguageButtonPolicy.resolve(
                        false, true, true, 2340, 1080, 3.0f, 0, 0).visible,
                "detach must hide touch selector");
        check(!MBLanguageButtonPolicy.resolve(
                        true, false, true, 2340, 1080, 3.0f, 0, 0).visible,
                "button must wait until native startup is ready");
        check(!MBLanguageButtonPolicy.resolve(
                        true, true, false, 2340, 1080, 3.0f, 0, 0).visible,
                "screens without the native options window must hide touch selector");
        MBLanguageButtonPolicy.Placement fourByThree = MBLanguageButtonPolicy.resolve(
                true, true, true, 1440, 1080, 2.0f, 0, 0);
        check(fourByThree.visible && !fourByThree.inSideGutter,
                "4:3 options screen must use the reachable safe-corner fallback");
    }

    private static void checkPrivateStaticNative(Method method, String message) {
        int modifiers = method.getModifiers();
        check(Modifier.isPrivate(modifiers) && Modifier.isStatic(modifiers)
                        && Modifier.isNative(modifiers), message);
    }

    private static void testNativeSignatures() throws Exception {
        ClassLoader loader = MBTouchOverlayLogicTest.class.getClassLoader();
        Class<?> overlay = Class.forName(
                "com.taleworlds.mbwarband.MBTouchOverlay", false, loader);
        Class<?> assetManager = Class.forName("android.content.res.AssetManager", false, loader);
        Method prepare = overlay.getDeclaredMethod("nativePrepareGameData",
                assetManager, String.class, String.class, String.class,
                String.class, String.class, String.class, String.class);
        check(prepare.getReturnType() == boolean.class,
                "nativePrepareGameData must return boolean");
        checkPrivateStaticNative(prepare, "nativePrepareGameData modifiers changed");

        Method move = overlay.getDeclaredMethod("nativeSetMoveAxes", float.class, float.class);
        check(move.getReturnType() == void.class, "nativeSetMoveAxes return type");
        checkPrivateStaticNative(move, "nativeSetMoveAxes must remain private static native");

        Method responsive = overlay.getDeclaredMethod("nativeGuestFrameResponsive");
        check(responsive.getReturnType() == boolean.class,
                "nativeGuestFrameResponsive return type");
        checkPrivateStaticNative(responsive,
                "nativeGuestFrameResponsive must remain private static native");
        Method inputToast = overlay.getDeclaredMethod("nativeConsumeInputToast");
        check(inputToast.getReturnType() == int.class, "nativeConsumeInputToast return type");
        checkPrivateStaticNative(inputToast,
                "nativeConsumeInputToast must remain private static native");

        Method look = overlay.getDeclaredMethod("nativeAddLookDelta", float.class, float.class);
        check(look.getReturnType() == void.class, "nativeAddLookDelta return type");
        checkPrivateStaticNative(look, "nativeAddLookDelta must remain private static native");

        Method directional = overlay.getDeclaredMethod("nativeBeginDirectionalAction",
                int.class, float.class, float.class);
        check(directional.getReturnType() == void.class,
                "nativeBeginDirectionalAction return type");
        checkPrivateStaticNative(directional,
                "nativeBeginDirectionalAction must remain private static native");

        Method menuConfig = overlay.getDeclaredMethod("nativeConfigureMenuTouch",
                boolean.class, boolean.class, boolean.class, float.class, float.class,
                int.class, int.class);
        check(menuConfig.getReturnType() == void.class,
                "nativeConfigureMenuTouch return type");
        checkPrivateStaticNative(menuConfig,
                "nativeConfigureMenuTouch must remain private static native");

        Method settingsExchange = overlay.getDeclaredMethod("nativeExchangeGuestSettings",
                boolean.class, boolean.class, boolean.class, boolean.class, boolean.class,
                boolean.class, boolean.class, boolean.class, boolean.class, boolean.class,
                boolean.class, boolean.class, boolean.class, boolean.class,
                float.class, float.class, float.class, float.class, float.class,
                float.class, float.class, float.class, float.class, float.class);
        check(settingsExchange.getReturnType() == int[].class,
                "nativeExchangeGuestSettings return type");
        checkPrivateStaticNative(settingsExchange,
                "nativeExchangeGuestSettings must remain private static native");

        Method editMode = overlay.getDeclaredMethod("nativeSetOverlayEditMode",
                boolean.class);
        check(editMode.getReturnType() == void.class,
                "nativeSetOverlayEditMode return type");
        checkPrivateStaticNative(editMode,
                "nativeSetOverlayEditMode must remain private static native");

        Method uiHit = overlay.getDeclaredMethod("nativeTacticalUiHit", float.class, float.class);
        check(uiHit.getReturnType() == int.class, "nativeTacticalUiHit return type");
        checkPrivateStaticNative(uiHit, "nativeTacticalUiHit must remain private static native");
        Method uiTouch = overlay.getDeclaredMethod("nativeTacticalUiTouch",
                int.class, int.class, float.class, float.class, long.class);
        check(uiTouch.getReturnType() == boolean.class, "nativeTacticalUiTouch return type");
        checkPrivateStaticNative(uiTouch,
                "nativeTacticalUiTouch must remain private static native");

        Class<?> motionEvent = Class.forName("android.view.MotionEvent", false, loader);
        boolean obsoleteCallbackPathPresent = true;
        try {
            overlay.getDeclaredMethod("handleUiTouch", motionEvent);
        } catch (NoSuchMethodException expected) {
            obsoleteCallbackPathPresent = false;
        }
        check(!obsoleteCallbackPathPresent,
                "Window.Callback menu-touch runtime path must stay removed");
    }

    public static void main(String[] args) throws Exception {
        testLookOwnership();
        testMoveOwnership();
        testNativeUiPointerOwnership();
        testGuestSettingsSyncContract();
        testInputMathAndActionContract();
        testSystemBackPolicy();
        testMainMenuModButtonPolicy();
        testGameLanguageContract();
        testLanguageButtonLifecyclePolicy();
        testNativeSignatures();
        System.out.println("MBTouchOverlayLogicTest: PASS");
    }
}
