package com.taleworlds.mbwarband;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ActivityNotFoundException;
import android.content.Context;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.res.ColorStateList;
import android.content.res.AssetManager;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.GradientDrawable;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.SystemClock;
import android.os.storage.StorageManager;
import android.provider.DocumentsContract;
import android.text.InputType;
import android.view.Gravity;
import android.view.DisplayCutout;
import android.view.HapticFeedbackConstants;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.View;
import android.view.ViewConfiguration;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.view.accessibility.AccessibilityEvent;
import android.view.accessibility.AccessibilityManager;
import android.view.accessibility.AccessibilityNodeInfo;
import android.view.accessibility.AccessibilityNodeProvider;
import android.window.OnBackInvokedCallback;
import android.window.OnBackInvokedDispatcher;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.PopupWindow;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.IOException;
import java.util.List;
import java.util.Locale;

public final class MBTouchOverlay extends View implements SensorEventListener {
    /** Release builds intentionally discard diagnostics instead of writing logcat. */
    private static final class ReleaseLog {
        private ReleaseLog() {
        }

        static int i(String tag, String message) {
            return 0;
        }

        static int w(String tag, String message) {
            return 0;
        }

        static int w(String tag, String message, Throwable error) {
            return 0;
        }
    }

    private static final int HOST_ACCESSIBILITY_ID = -1;
    static final int ROLE_NONE = 0;
    static final int ROLE_MOVE = 1;
    static final int ROLE_LOOK = 2;
    static final int ROLE_ATTACK = 3;
    static final int ROLE_BLOCK = 4;
    static final int ROLE_USE = 5;
    static final int ROLE_PAUSE = 6;
    static final int ROLE_KICK = 7;
    static final int ROLE_ORDERS = 8;
    static final int ROLE_WEAPON_MODE = 9;
    static final int ROLE_JUMP = 10;
    static final int ROLE_DROP = 11;
    static final int ROLE_CYCLE_WEAPON = 12;
    static final int ROLE_CYCLE_SHIELD = 13;
    static final int ROLE_ZOOM = 14;
    static final int ROLE_VIEW = 15;
    static final int ROLE_BACK = 16;
    static final int ROLE_SHEATH = 17;
    static final int ROLE_ACTIONS_MENU = 18;
    static final int ROLE_NATIVE_UI = 19;
    static final int ROLE_CHAT = 20;

    private static final int MISSION_STATE_CONTEXT_ACTION = 1;
    private static final int MISSION_STATE_DROP_ITEM = 1 << 1;
    private static final int MISSION_STATE_ISSUE_ORDERS = 1 << 2;
    private static final int MISSION_STATE_CYCLE_WEAPON = 1 << 3;
    private static final int MISSION_STATE_CYCLE_SHIELD = 1 << 4;
    private static final int MISSION_STATE_SHEATH_WEAPON = 1 << 5;
    private static final int MISSION_STATE_PLAYER_MOUNTED = 1 << 6;
    private static final int MISSION_STATE_MULTIPLAYER = 1 << 7;
    private static final long PAUSE_EDIT_HOLD_MILLIS = 650L;
    /**
     * Attack/block direction is sampled by the original game on the trigger's
     * rising edge. Give a stationary hold a short grace period so a directional
     * drag can provide its vector before that edge is emitted.
     */
    private static final long DIRECTIONAL_ACTION_HOLD_MILLIS = 225L;
    private static final long MIN_ACTION_PULSE_MILLIS = 70L;
    private static final long DISMOUNT_HINT_VISIBLE_MILLIS = 8000L;
    private static final String KEY_LAYOUT_HINT_SHOWN = "mb_layout_pause_hint_v2_shown";
    private static final String KEY_RADIAL_HINT_SHOWN = "mb_radial_drag_hint_v2_shown";
    private static final String KEY_DISMOUNT_HINT_SHOWN = "mb_dismount_hint_v4_shown";
    private static final String DIRECTORY_MIME_TYPE = "vnd.android.document/directory";
    private static final String LEGACY_DIRECTORY_MIME_TYPE = "resource/folder";
    private static final String EXTRA_ABSOLUTE_PATH = "org.openintents.extra.ABSOLUTE_PATH";

    /** Game_windows id of the native Options screen (General/Controls tabs). */
    static final int OPTIONS_WINDOW_ID = 13;
    /** Game_windows id of the profile / Host / Join multiplayer landing screen. */
    static final int MULTIPLAYER_PROFILE_WINDOW_ID = 25;

    static final int ACTION_ATTACK = 1;
    static final int ACTION_BLOCK = 2;
    static final int ACTION_USE = 3;
    static final int ACTION_JUMP = 4;
    static final int ACTION_KICK = 5;
    static final int ACTION_PAUSE = 6;
    static final int ACTION_DPAD_UP = 7;
    static final int ACTION_DPAD_DOWN = 8;
    static final int ACTION_LB = 9;
    static final int ACTION_RB = 10;
    static final int ACTION_DROP = 11;
    static final int ACTION_ZOOM = 12;
    static final int ACTION_VIEW = 13;
    static final int ACTION_BACK = 14;
    static final int ACTION_SHEATH = 15;
    /**
     * One-shot bridge command which publishes the stock tactical orders state
     * transition on the guest frame thread. D-pad Up is deliberately not used
     * here: in this mobile build that controller binding opens Quests.
     */
    static final int ACTION_OPEN_ORDERS = 16;
    static final int ACTION_ENTER_CONFIRM = 17;
    static final int ACTION_CHAT = 18;
    private static final int SYSTEM_UI_FLAGS = 5894;
    private static final long SYSTEM_BACK_HOLD_MILLIS = 70L;
    private static final float DEFAULT_GYRO_SENSITIVITY = 1.25f;
    private static final float GYRO_LOOK_UNITS_PER_RADIAN = 180.0f;
    private static final float MAX_LOOK_SAMPLE_UNITS = 160.0f;
    // Painter order is also the deterministic tie-break order for overlapping hit targets.
    private static final int[] BUTTON_ROLES = {
            ROLE_ATTACK, ROLE_BLOCK, ROLE_KICK, ROLE_JUMP,
            ROLE_BACK, ROLE_PAUSE, ROLE_ACTIONS_MENU, ROLE_CHAT
    };
    private static final float[] BUTTON_RADIUS_DP = {
            42.0f, 34.0f, 26.0f, 28.0f, 24.0f, 24.0f, 25.0f, 24.0f
    };
    private static final int[] RADIAL_ACTION_ROLES = {
            ROLE_ORDERS, ROLE_WEAPON_MODE, ROLE_DROP, ROLE_CYCLE_WEAPON,
            ROLE_CYCLE_SHIELD, ROLE_ZOOM, ROLE_VIEW, ROLE_SHEATH
    };

    private static final class SafeInsets {
        static final SafeInsets NONE = new SafeInsets(0, 0, 0, 0);
        final int left;
        final int top;
        final int right;
        final int bottom;

        SafeInsets(int left, int top, int right, int bottom) {
            this.left = Math.max(0, left);
            this.top = Math.max(0, top);
            this.right = Math.max(0, right);
            this.bottom = Math.max(0, bottom);
        }
    }

    /** Pure pointer bookkeeping kept Android-free so ownership can be unit-tested. */
    static final class PointerBook {
        private static final int MAX_POINTERS = 32;
        final int[] roles = new int[MAX_POINTERS];
        final float[] downX = new float[MAX_POINTERS];
        final float[] downY = new float[MAX_POINTERS];
        final float[] lastX = new float[MAX_POINTERS];
        final float[] lastY = new float[MAX_POINTERS];
        final long[] downTime = new long[MAX_POINTERS];
        private final boolean[] directionalActionStarted = new boolean[MAX_POINTERS];
        private final long[] directionalActionStartedAt = new long[MAX_POINTERS];
        private int lookOwner = -1;
        private int moveOwner = -1;

        boolean valid(int id) {
            return id >= 0 && id < roles.length;
        }

        private static boolean canOwnLook(int role) {
            return role == ROLE_LOOK || role == ROLE_ATTACK || role == ROLE_BLOCK;
        }

        private static int lookPriority(int role) {
            // Directional attack/block drags must preempt a passive camera finger. Attack wins
            // a simultaneous tie because releasing it commits the selected strike direction.
            if (role == ROLE_ATTACK) return 3;
            if (role == ROLE_BLOCK) return 2;
            return role == ROLE_LOOK ? 1 : 0;
        }

        int begin(int id, int role, float x, float y, long time) {
            if (!valid(id)) return lookOwner;
            int previousOwner = lookOwner;
            roles[id] = role;
            downX[id] = lastX[id] = x;
            downY[id] = lastY[id] = y;
            downTime[id] = time;
            directionalActionStarted[id] = false;
            directionalActionStartedAt[id] = 0L;
            if (role == ROLE_MOVE && moveOwner < 0) moveOwner = id;
            if (canOwnLook(role) && (lookOwner < 0
                    || lookPriority(role) > lookPriority(roles[lookOwner]))) {
                lookOwner = id;
            }
            return previousOwner;
        }

        void update(int id, float x, float y) {
            if (!valid(id)) return;
            lastX[id] = x;
            lastY[id] = y;
        }

        int role(int id) {
            return valid(id) ? roles[id] : ROLE_NONE;
        }

        int lookOwner() {
            return lookOwner;
        }

        boolean ownsLook(int id) {
            return lookOwner == id;
        }

        int moveOwner() {
            return moveOwner;
        }

        boolean ownsMove(int id) {
            return moveOwner == id;
        }

        boolean directionalActionStarted(int id) {
            return valid(id) && directionalActionStarted[id];
        }

        long directionalActionStartedAt(int id) {
            return valid(id) ? directionalActionStartedAt[id] : 0L;
        }

        void markDirectionalActionStarted(int id, long time) {
            if (!valid(id)) return;
            directionalActionStarted[id] = true;
            directionalActionStartedAt[id] = time;
        }

        int end(int id) {
            if (!valid(id)) return lookOwner;
            roles[id] = ROLE_NONE;
            directionalActionStarted[id] = false;
            directionalActionStartedAt[id] = 0L;
            if (lookOwner == id) chooseReplacementLookOwner();
            if (moveOwner == id) chooseReplacementMoveOwner();
            return lookOwner;
        }

        private void chooseReplacementLookOwner() {
            lookOwner = -1;
            // Directional combat drags are the primary aim gestures. Keep the oldest pointer
            // within a role so additional fingers never steal an active gesture.
            long oldestDownTime = Long.MAX_VALUE;
            for (int id = 0; id < roles.length; ++id) {
                if (roles[id] == ROLE_ATTACK && downTime[id] < oldestDownTime) {
                    lookOwner = id;
                    oldestDownTime = downTime[id];
                }
            }
            if (lookOwner >= 0) return;
            oldestDownTime = Long.MAX_VALUE;
            for (int id = 0; id < roles.length; ++id) {
                if (roles[id] == ROLE_BLOCK && downTime[id] < oldestDownTime) {
                    lookOwner = id;
                    oldestDownTime = downTime[id];
                }
            }
            if (lookOwner >= 0) return;
            oldestDownTime = Long.MAX_VALUE;
            for (int id = 0; id < roles.length; ++id) {
                if (roles[id] == ROLE_LOOK && downTime[id] < oldestDownTime) {
                    lookOwner = id;
                    oldestDownTime = downTime[id];
                }
            }
        }

        private void chooseReplacementMoveOwner() {
            moveOwner = -1;
            long oldestDownTime = Long.MAX_VALUE;
            for (int id = 0; id < roles.length; ++id) {
                if (roles[id] == ROLE_MOVE && downTime[id] < oldestDownTime) {
                    moveOwner = id;
                    oldestDownTime = downTime[id];
                }
            }
        }

        void reset() {
            for (int id = 0; id < roles.length; ++id) {
                roles[id] = ROLE_NONE;
                directionalActionStarted[id] = false;
                directionalActionStartedAt[id] = 0L;
            }
            lookOwner = -1;
            moveOwner = -1;
        }
    }

    static final class InputMath {
        private static final float MOVE_DEADZONE = 0.08f;
        private static final float DIRECTION_GESTURE_SLOP_DP = 6.0f;

        static float clampAxis(float value) {
            return Math.max(-1.0f, Math.min(1.0f, value));
        }

        /** Preserve a continuous stick angle and optionally force a full-speed run. */
        static void movementAxes(float x, float y, boolean alwaysRun, float[] output) {
            if (output == null || output.length < 2) {
                throw new IllegalArgumentException("movement output needs two axes");
            }
            float safeX = finiteClamped(x, 1024.0f);
            float safeY = finiteClamped(y, 1024.0f);
            float length = (float)Math.sqrt(safeX * safeX + safeY * safeY);
            if (!(length >= MOVE_DEADZONE)) {
                output[0] = 0.0f;
                output[1] = 0.0f;
                return;
            }
            float magnitude = alwaysRun ? 1.0f : Math.min(length, 1.0f);
            float scale = magnitude / length;
            output[0] = safeX * scale;
            output[1] = safeY * scale;
        }

        static float removeDeadzone(float value, float deadzone) {
            float magnitude = Math.abs(value);
            if (magnitude <= deadzone) return 0.0f;
            return Math.copySign(magnitude - deadzone, value);
        }

        static float finiteClamped(float value, float limit) {
            if (Float.isNaN(value) || Float.isInfinite(value)) return 0.0f;
            return Math.max(-limit, Math.min(limit, value));
        }

        static float touchLookUnits(float deltaPixels, float density, float sensitivity) {
            float safeDensity = Math.max(0.5f, density);
            return finiteClamped(deltaPixels / safeDensity * sensitivity,
                    MAX_LOOK_SAMPLE_UNITS);
        }

        static float gyroLookUnits(float angularDelta, float deltaSeconds,
                                   float deadzoneRadiansPerSecond, float sensitivity) {
            if (deltaSeconds <= 0.0f || deltaSeconds > 0.12f) return 0.0f;
            float velocity = angularDelta / deltaSeconds;
            float filteredVelocity = removeDeadzone(velocity,
                    Math.max(0.0f, deadzoneRadiansPerSecond));
            return finiteClamped(filteredVelocity * deltaSeconds * sensitivity
                    * GYRO_LOOK_UNITS_PER_RADIAN, MAX_LOOK_SAMPLE_UNITS);
        }

        static boolean directionGestureReady(float deltaX, float deltaY, float density) {
            float safeX = finiteClamped(deltaX, 8192.0f);
            float safeY = finiteClamped(deltaY, 8192.0f);
            float threshold = DIRECTION_GESTURE_SLOP_DP * Math.max(0.5f, density);
            return safeX * safeX + safeY * safeY >= threshold * threshold;
        }
    }

    /** Prevent Android's root-menu Back default from sending the game to Home. */
    static final class SystemBackPolicy {
        static final int ROOT_MENU_WINDOW_ID = 4;

        static boolean shouldDispatch(boolean nativeStarted, boolean nativeReady,
                                      boolean mission, int activeWindowId) {
            if (!nativeStarted || !nativeReady) return false;
            if (mission) return true;
            return activeWindowId > 0 && activeWindowId != ROOT_MENU_WINDOW_ID;
        }
    }

    /** Pure language contract shared by startup, the picker and host-side tests. */
    static final class GameLanguage {
        static final String PREFERENCES_NAME = "PREFERENCE";
        static final String PREFERENCE_KEY = "mb_game_language";
        static final String DEFAULT_CODE = MBTouchStrings.DEFAULT_CODE;
        static final String[] CODES = MBTouchStrings.codes();
        static final String[] DISPLAY_NAMES = MBTouchStrings.displayNames();

        static boolean isSupported(String code) {
            return MBTouchStrings.isSupported(code);
        }

        static String normalize(String code) {
            return MBTouchStrings.normalize(code);
        }

        static int indexOf(String code) {
            return MBTouchStrings.indexOf(code);
        }

        static String codeAt(int index) {
            return MBTouchStrings.codeAt(index);
        }
    }

    /** Pure Java mirror of the fixed ARM32/AArch64 settings protocol. */
    static final class GuestSettingsSync {
        static final int BOOL_GYRO_ENABLED = 1 << 0;
        static final int BOOL_GYRO_INVERT_Y = 1 << 1;
        static final int BOOL_MENU_SWIPE = 1 << 2;
        static final int BOOL_GYRO_RANGED_ONLY = 1 << 3;
        static final int BOOL_LEFT_HANDED = 1 << 4;
        /** Guest-to-host one-shot: the payload never accepts it from the host. */
        static final int BOOL_LAYOUT_EDIT_REQUEST = 1 << 5;
        static final int BOOL_CAMERA_MOUSE = 1 << 6;
        static final int BOOL_ALWAYS_RUN = 1 << 7;
        static final int BOOL_HDR_ENABLED = 1 << 8;
        static final int BOOL_DOF_ENABLED = 1 << 9;
        static final int BOOL_AUTO_EXPOSURE = 1 << 10;
        static final int BOOL_HAPTIC_FEEDBACK = 1 << 11;
        static final int BOOL_MENU_ASPECT = 1 << 12;
        static final int BOOL_FXAA_ENABLED = 1 << 13;
        static final int BOOL_MASK_ALL = 0x3FFF;
        /**
         * Bits the host may propose; the layout-edit request stays guest-owned.
         */
        static final int BOOL_HOST_WRITABLE_MASK = 0x3D9F;
        static final int FLOAT_COUNT = 10;
        static final int PACKET_WORDS = 1 + FLOAT_COUNT;
        static final int POLL_NONE = 0;
        static final int POLL_ACTIVE = 1;
        static final int POLL_ARM_FINAL = 2;
        static final int POLL_FINAL = 3;

        static final String KEY_GYRO_ENABLED = "mb_gyro_enabled";
        static final String KEY_GYRO_INVERT_Y = "mb_gyro_invert_y";
        static final String KEY_MENU_SWIPE = "mb_menu_swipe_navigation";
        static final String KEY_GYRO_RANGED_ONLY = "mb_gyro_ranged_only";
        static final String KEY_LEFT_HANDED = "mb_left_handed";
        static final String KEY_CAMERA_MOUSE = "mb_camera_mouse_method";
        static final String KEY_ALWAYS_RUN = "mb_always_run";
        static final String KEY_MOVE_SENSITIVITY = "mb_move_sensitivity";
        static final String KEY_GYRO_SENSITIVITY = "mb_gyro_sensitivity";
        static final String KEY_GYRO_DEADZONE = "mb_gyro_deadzone";
        static final String KEY_BUTTON_SCALE = "mb_button_scale";
        static final String KEY_BUTTON_OPACITY = "mb_button_opacity";
        static final String KEY_FPS_LIMIT = "mb_fps_limit";
        static final String KEY_HDR_ENABLED = "mb_postfx_hdr";
        static final String KEY_DOF_ENABLED = "mb_postfx_dof";
        static final String KEY_FXAA_ENABLED = "mb_fxaa_enabled";
        static final String KEY_AUTO_EXPOSURE = "mb_postfx_autoexp";
        static final String KEY_HAPTIC_FEEDBACK = "mb_touch_haptics";
        static final String KEY_MENU_ASPECT = "mb_menu_aspect_ratio";
        static final String KEY_HDR_BRIGHTNESS = "mb_hdr_brightness";
        static final String KEY_SHADOW_MODE = "mb_shadow_rendering_mode";
        static final String KEY_MAP_PAN_SENSITIVITY = "mb_map_pan_sensitivity";
        static final String KEY_REPORT_TEXT_SCALE = "mb_report_text_scale";

        /** Move, gyro sensitivity, deadzone, scale, opacity, fps, HDR, shadow, map pan, reports. */
        static final float[] MINIMUM = {
                0.5f, 0.2f, 0.0f, 0.6f, 0.2f, 0.0f, 0.5f, 0.0f, 0.35f, 0.5f};
        static final float[] MAXIMUM = {
                1.8f, 4.0f, 0.06f, 1.4f, 1.0f, 3.0f, 1.25f, 2.0f, 1.50f, 1.5f};
        static final float[] DEFAULT = {
                1.0f, 1.25f, 0.012f, 1.0f, 1.0f, 3.0f, 0.75f, 2.0f, 0.70f, 1.0f};

        /** FPS slider positions: 30, 60, 90 and 3 = display maximum (uncapped). */
        static int fpsLimitChoice(float position) {
            int choice = Math.round(position);
            return Math.max(0, Math.min(3, choice));
        }

        static int fpsLimitValue(int choice) {
            switch (choice) {
                case 0: return 30;
                case 1: return 60;
                case 2: return 90;
                default: return 0;
            }
        }

        static final class Values {
            final boolean gyroEnabled;
            final boolean gyroInvertY;
            final boolean menuSwipe;
            final boolean gyroRangedOnly;
            final boolean leftHanded;
            final boolean cameraMouse;
            final boolean alwaysRun;
            final boolean layoutEditRequested;
            final boolean hdrEnabled;
            final boolean dofEnabled;
            final boolean autoExposure;
            final boolean hapticFeedback;
            final boolean menuAspect;
            final boolean fxaaEnabled;
            final float moveSensitivity;
            final float gyroSensitivity;
            final float gyroDeadzone;
            final float buttonScale;
            final float buttonOpacity;
            final float fpsLimit;
            final float hdrBrightness;
            final float shadowMode;
            final float mapPanSensitivity;
            final float reportTextScale;

            Values(boolean gyroEnabled, boolean gyroInvertY,
                   boolean menuSwipe, boolean gyroRangedOnly, boolean leftHanded,
                   boolean cameraMouse, boolean alwaysRun, boolean layoutEditRequested,
                   boolean hdrEnabled, boolean dofEnabled, boolean autoExposure,
                   boolean hapticFeedback, boolean menuAspect,
                   boolean fxaaEnabled,
                   float moveSensitivity, float gyroSensitivity,
                   float gyroDeadzone, float buttonScale,
                   float buttonOpacity, float fpsLimit,
                   float hdrBrightness, float shadowMode,
                   float mapPanSensitivity, float reportTextScale) {
                this.gyroEnabled = gyroEnabled;
                this.gyroInvertY = gyroInvertY;
                this.menuSwipe = menuSwipe;
                this.gyroRangedOnly = gyroRangedOnly;
                this.leftHanded = leftHanded;
                this.cameraMouse = cameraMouse;
                this.alwaysRun = alwaysRun;
                this.layoutEditRequested = layoutEditRequested;
                this.hdrEnabled = hdrEnabled;
                this.dofEnabled = dofEnabled;
                this.autoExposure = autoExposure;
                this.hapticFeedback = hapticFeedback;
                this.menuAspect = menuAspect;
                this.fxaaEnabled = fxaaEnabled;
                this.moveSensitivity = moveSensitivity;
                this.gyroSensitivity = gyroSensitivity;
                this.gyroDeadzone = gyroDeadzone;
                this.buttonScale = buttonScale;
                this.buttonOpacity = buttonOpacity;
                this.fpsLimit = fpsLimit;
                this.hdrBrightness = hdrBrightness;
                this.shadowMode = shadowMode;
                this.mapPanSensitivity = mapPanSensitivity;
                this.reportTextScale = reportTextScale;
            }

            int boolMask() {
                int mask = 0;
                if (gyroEnabled) mask |= BOOL_GYRO_ENABLED;
                if (gyroInvertY) mask |= BOOL_GYRO_INVERT_Y;
                if (menuSwipe) mask |= BOOL_MENU_SWIPE;
                if (gyroRangedOnly) mask |= BOOL_GYRO_RANGED_ONLY;
                if (leftHanded) mask |= BOOL_LEFT_HANDED;
                if (cameraMouse) mask |= BOOL_CAMERA_MOUSE;
                if (alwaysRun) mask |= BOOL_ALWAYS_RUN;
                if (hdrEnabled) mask |= BOOL_HDR_ENABLED;
                // DOF is hidden: the stored switch no longer enables it.
                // if (dofEnabled) mask |= BOOL_DOF_ENABLED;
                if (autoExposure) mask |= BOOL_AUTO_EXPOSURE;
                if (hapticFeedback) mask |= BOOL_HAPTIC_FEEDBACK;
                if (menuAspect) mask |= BOOL_MENU_ASPECT;
                if (fxaaEnabled) mask |= BOOL_FXAA_ENABLED;
                return mask & BOOL_HOST_WRITABLE_MASK;
            }

            float floatAt(int index) {
                if (index == 0) return moveSensitivity;
                if (index == 1) return gyroSensitivity;
                if (index == 2) return gyroDeadzone;
                if (index == 3) return buttonScale;
                if (index == 4) return buttonOpacity;
                if (index == 5) return fpsLimit;
                if (index == 6) return hdrBrightness;
                if (index == 7) return shadowMode;
                if (index == 8) return mapPanSensitivity;
                return reportTextScale;
            }
        }

        static float boundHostFloat(int index, float value) {
            if (index < 0 || index >= FLOAT_COUNT) return 0.0f;
            if (Float.isNaN(value) || Float.isInfinite(value)) return DEFAULT[index];
            // Legacy nonzero shadow preferences map to the checked mode 2.
            if (index == 7) return value > 0.0f ? 2.0f : 0.0f;
            return Math.max(MINIMUM[index], Math.min(MAXIMUM[index], value));
        }

        static Values boundedHostValues(boolean gyroEnabled, boolean gyroInvertY,
                                        boolean menuSwipe, boolean gyroRangedOnly,
                                        boolean leftHanded, boolean cameraMouse,
                                        boolean alwaysRun, boolean hdrEnabled,
                                        boolean dofEnabled, boolean autoExposure,
                                        boolean hapticFeedback, boolean menuAspect,
                                        boolean fxaaEnabled,
                                        float moveSensitivity, float gyroSensitivity,
                                        float gyroDeadzone, float buttonScale,
                                        float buttonOpacity, float fpsLimit,
                                        float hdrBrightness, float shadowMode,
                                        float mapPanSensitivity, float reportTextScale) {
            return new Values(
                    gyroEnabled, gyroInvertY, menuSwipe, gyroRangedOnly, leftHanded,
                    false, alwaysRun, false,
                    hdrEnabled, dofEnabled, autoExposure, hapticFeedback,
                    menuAspect, fxaaEnabled,
                    boundHostFloat(0, moveSensitivity),
                    boundHostFloat(1, gyroSensitivity),
                    boundHostFloat(2, gyroDeadzone),
                    boundHostFloat(3, buttonScale),
                    boundHostFloat(4, buttonOpacity),
                    boundHostFloat(5, fpsLimit),
                    boundHostFloat(6, hdrBrightness),
                    boundHostFloat(7, shadowMode),
                    boundHostFloat(8, mapPanSensitivity),
                    boundHostFloat(9, reportTextScale));
        }

        static Values decodeGuestPacket(int[] packet) {
            if (packet == null || packet.length != PACKET_WORDS ||
                    (packet[0] & ~BOOL_MASK_ALL) != 0) {
                return null;
            }
            float[] values = new float[FLOAT_COUNT];
            for (int index = 0; index < FLOAT_COUNT; ++index) {
                float value = Float.intBitsToFloat(packet[index + 1]);
                if (Float.isNaN(value) || Float.isInfinite(value) ||
                        value < MINIMUM[index] || value > MAXIMUM[index]) {
                    return null;
                }
                values[index] = value;
            }
            int mask = packet[0];
            return new Values(
                    (mask & BOOL_GYRO_ENABLED) != 0,
                    (mask & BOOL_GYRO_INVERT_Y) != 0,
                    (mask & BOOL_MENU_SWIPE) != 0,
                    (mask & BOOL_GYRO_RANGED_ONLY) != 0,
                    (mask & BOOL_LEFT_HANDED) != 0,
                    false,
                    (mask & BOOL_ALWAYS_RUN) != 0,
                    (mask & BOOL_LAYOUT_EDIT_REQUEST) != 0,
                    (mask & BOOL_HDR_ENABLED) != 0,
                    false, // Retired DOF bit is ignored even in old guest packets.
                    (mask & BOOL_AUTO_EXPOSURE) != 0,
                    (mask & BOOL_HAPTIC_FEEDBACK) != 0,
                    (mask & BOOL_MENU_ASPECT) != 0,
                    (mask & BOOL_FXAA_ENABLED) != 0,
                    values[0], values[1], values[2], values[3], values[4],
                    values[5], values[6], values[7], values[8], values[9]);
        }

        static int pollAction(boolean windowKnown, boolean optionsActive,
                              boolean optionsWasActive, boolean finalPollPending) {
            if (!windowKnown) return POLL_NONE;
            if (optionsActive) return POLL_ACTIVE;
            if (finalPollPending) return POLL_FINAL;
            return optionsWasActive ? POLL_ARM_FINAL : POLL_NONE;
        }
    }

    private static Activity activeActivity;
    private static View activeDecor;
    private static PopupWindow activePopup;
    private static boolean activePopupReceivesTouches;
    private static PopupWindow tacticalBackPopup;
    private static PopupWindow mapTimePopup;
    private static TextView mapTimeButton;
    private static PopupWindow pollAnswerPopup;
    private static PopupWindow masterServerPopup;
    private static TextView masterServerField;
    private static AlertDialog masterServerDialog;
    private static volatile int activeMissionPresentationId = -1;
    private static long activeMissionPresentationSeenAt;
    private static MBTouchOverlay activeOverlay;
    private static PopupWindow languageButtonPopup;
    private static AlertDialog languageDialog;
    private static PopupWindow modButtonPopup;
    private static MainMenuModButtonPolicy.Placement modButtonPlacement;
    private static AlertDialog modDialog;
    private static TextView modButton;
    private static int languageButtonX = -1;
    private static int languageButtonY = -1;
    private static int languageButtonSize = -1;
    private static boolean nativeStarted;
    private static final MBInputStallPolicy INPUT_STALL_POLICY =
            new MBInputStallPolicy();
    private static boolean guestSettingsOptionsWasActive;
    private static boolean guestSettingsFinalPollPending;
    private static boolean guestSettingsInitialSyncDone;
    private static boolean activeLifecycleResumed;
    private static String gameDataRoot;
    private static boolean gameDataPrepared;
    private static File externalFilesRoot;
    private static MBModManager.Selection activeModSelection;
    private static String pendingModError;
    private static final String KEY_EXTERNAL_STORAGE_MIGRATION_V1 =
            "mb_external_storage_migration_v1";

    /** Keep API-33-only references out of MBTouchOverlay's class metadata on API 28-32. */
    private static final class Api33SystemBack {
        private static OnBackInvokedDispatcher dispatcher;
        private static OnBackInvokedCallback callback;

        static void register(Activity activity) {
            try {
                OnBackInvokedDispatcher nextDispatcher =
                        activity.getOnBackInvokedDispatcher();
                if (nextDispatcher == dispatcher && callback != null) return;
                unregister();
                OnBackInvokedCallback nextCallback = new OnBackInvokedCallback() {
                    @Override public void onBackInvoked() {
                        onSystemBack();
                    }
                };
                nextDispatcher.registerOnBackInvokedCallback(
                        OnBackInvokedDispatcher.PRIORITY_OVERLAY, nextCallback);
                dispatcher = nextDispatcher;
                callback = nextCallback;
                ReleaseLog.i("MBControls", "registered native system Back callback");
            } catch (NoSuchMethodError | RuntimeException error) {
                ReleaseLog.w("MBControls", "could not register native system Back callback", error);
            }
        }

        static void unregister() {
            OnBackInvokedDispatcher oldDispatcher = dispatcher;
            OnBackInvokedCallback oldCallback = callback;
            dispatcher = null;
            callback = null;
            if (oldDispatcher == null || oldCallback == null) return;
            try {
                oldDispatcher.unregisterOnBackInvokedCallback(oldCallback);
            } catch (NoSuchMethodError | RuntimeException error) {
                ReleaseLog.w("MBControls", "could not unregister native system Back callback", error);
            }
        }
    }

    private static final Runnable OVERLAY_POLL = new Runnable() {
        @Override public void run() {
            Activity activity = activeActivity;
            View decor = activeDecor;
            if (activity == null || decor == null || !activeLifecycleResumed) return;
            hideSystemBars(decor);
            if (!nativeStarted) {
                nativeStart();
                nativeStarted = true;
            }
            boolean ready = nativeReady();
            if (ready && !guestSettingsInitialSyncDone) {
                syncNativeGuestSettings(activity, false);
                guestSettingsInitialSyncDone = true;
            }
            if (ready) {
                MBTouchStrings.Text inputToast = MBTouchStrings.inventoryTransferFailure(
                        nativeConsumeInputToast());
                if (inputToast != null) {
                    showTopToast(activity, activity.getSharedPreferences(
                            GameLanguage.PREFERENCES_NAME, Context.MODE_PRIVATE), inputToast);
                }
            }
            boolean mission = ready && nativeMissionActive();
            boolean missionSession = ready && nativeMissionSessionActive();
            int activeWindowId = ready ? nativeActiveMenuWindowId() : -1;
            int activePresentationId = ready && missionSession
                    ? nativeActivePresentationId() : -1;
            int tacticalModal = mission ? nativeTacticalModalState() : 0;
            long presentationPollTime = SystemClock.uptimeMillis();
            if (activePresentationId >= 0) {
                activeMissionPresentationId = activePresentationId;
                activeMissionPresentationSeenAt = presentationPollTime;
            } else if (!missionSession || activeMissionPresentationSeenAt == 0L
                    || presentationPollTime - activeMissionPresentationSeenAt > 500L) {
                activeMissionPresentationId = -1;
                activeMissionPresentationSeenAt = 0L;
            }
            int touchPresentationId = activePresentationId >= 0
                    ? activePresentationId : activeMissionPresentationId;
            boolean tacticalMenu = mission && tacticalModal != 0;
            boolean pollActive = mission && tacticalModal == 0
                    && touchPresentationId == MBBattleTouchPolicy.MULTIPLAYER_POLL_PRESENTATION_ID;
            boolean windowKnown = mission || activeWindowId >= 0;
            boolean optionsWindow = windowKnown && activeWindowId == OPTIONS_WINDOW_ID;
            int settingsPoll = GuestSettingsSync.pollAction(
                    windowKnown, optionsWindow, guestSettingsOptionsWasActive,
                    guestSettingsFinalPollPending);
            if (settingsPoll == GuestSettingsSync.POLL_ACTIVE) {
                syncNativeGuestSettings(activity, false);
                guestSettingsFinalPollPending = false;
            } else if (settingsPoll == GuestSettingsSync.POLL_ARM_FINAL) {
                guestSettingsFinalPollPending = true;
            } else if (settingsPoll == GuestSettingsSync.POLL_FINAL) {
                syncNativeGuestSettings(activity, true);
                guestSettingsFinalPollPending = false;
            }
            if (windowKnown) guestSettingsOptionsWasActive = optionsWindow;
            // A guest commit is applied above, so menu gesture policy sees it
            // during this same UI poll rather than one poll later.
            syncNativeMenuTouchConfiguration(activity, decor);
            boolean captureMissionTouches =
                    MBBattleTouchPolicy.shouldCaptureMissionOverlayTouch(
                            mission, missionSession, touchPresentationId);
            if (missionSession && (activePopup == null || !activePopup.isShowing())) {
                activeOverlay = new MBTouchOverlay(activity);
                activeOverlay.setVisibility(View.VISIBLE);
                activePopup = new PopupWindow(activeOverlay,
                        ViewGroup.LayoutParams.MATCH_PARENT,
                        ViewGroup.LayoutParams.MATCH_PARENT,
                        false);
                activePopup.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
                activePopup.setTouchable(captureMissionTouches);
                activePopupReceivesTouches = captureMissionTouches;
                activePopup.setOutsideTouchable(false);
                activePopup.setClippingEnabled(false);
                activePopup.setInputMethodMode(PopupWindow.INPUT_METHOD_NEEDED);
                activePopup.showAtLocation(decor, Gravity.NO_GRAVITY, 0, 0);
                ReleaseLog.i("MBControls", "overlay popup shown");
            } else if (activePopup != null && activePopup.isShowing()
                    && activePopupReceivesTouches != captureMissionTouches) {
                // Non-tactical mission windows keep NativeActivity's input queue.
                activePopup.setTouchable(captureMissionTouches);
                activePopup.update();
                activePopupReceivesTouches = captureMissionTouches;
                ReleaseLog.i("MBControls", captureMissionTouches
                        ? "overlay touch capture enabled for tactical mission"
                        : "overlay touch capture disabled for native mission window");
            } else if (!missionSession && activePopup != null) {
                activePopup.dismiss();
                activePopup = null;
                activeOverlay = null;
                activePopupReceivesTouches = false;
                nativeReset();
                ReleaseLog.i("MBControls", "overlay popup hidden");
            }
            if (activeOverlay != null) {
                if (!captureMissionTouches && activeOverlay.getVisibility() == View.VISIBLE) {
                    activeOverlay.resetAll();
                }
                activeOverlay.setVisibility(View.VISIBLE);
            }
            updateTacticalBackButton(activity, decor, tacticalMenu);
            updateMapTimeButton(activity, decor, ready && !mission
                    && activeWindowId == 6);
            updatePollAnswerButtons(activity, decor, pollActive);
            updateMasterServerField(activity, decor,
                    (ready && !mission
                            && activeWindowId == MULTIPLAYER_PROFILE_WINDOW_ID)
                    || (masterServerDialog != null && masterServerDialog.isShowing()));
            updateLanguageButton(activity, decor, ready, mission, optionsWindow);
            updateModButton(activity, decor, ready, mission, activeWindowId);
            if (pendingModError != null) {
                Toast.makeText(activity, pendingModError, Toast.LENGTH_LONG).show();
                pendingModError = null;
            }
            if (activity == activeActivity && decor == activeDecor && activeLifecycleResumed) {
                decor.postDelayed(this, 200L);
            }
        }
    };
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint calloutFill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint calloutStroke = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final PointerBook pointers = new PointerBook();
    private final int[] nativeUiKinds = new int[PointerBook.MAX_POINTERS];
    private final int[] heldRoleCounts = new int[ROLE_SHEATH + 1];
    private final Runnable[] directionalActionFallbacks =
            new Runnable[PointerBook.MAX_POINTERS];
    private final SharedPreferences preferences;
    private final float[] buttonRawX = new float[BUTTON_ROLES.length];
    private final float[] buttonRawY = new float[BUTTON_ROLES.length];
    private final float[] buttonBaseRadius = new float[BUTTON_ROLES.length];
    private final float[] buttonCenterX = new float[BUTTON_ROLES.length];
    private final float[] buttonCenterY = new float[BUTTON_ROLES.length];
    private final float[] buttonRadius = new float[BUTTON_ROLES.length];
    private final float[] buttonHitRadius = new float[BUTTON_ROLES.length];
    private final boolean[] buttonHittable = new boolean[BUTTON_ROLES.length];
    private final float[] movementAxes = new float[2];
    /** Buttons the guest reports as currently pointless are never drawn or hit-tested. */
    private final boolean[] buttonVisible = new boolean[BUTTON_ROLES.length];
    private final MissionAccessibilityProvider accessibilityProvider =
            new MissionAccessibilityProvider();

    private float moveX;
    private float moveY;
    private float resolvedButtonScale = 1.0f;
    private boolean buttonLayoutDirty = true;
    private int cachedLayoutWidth = -1;
    private int cachedLayoutHeight = -1;
    private int cachedInsetLeft = -1;
    private int cachedInsetTop = -1;
    private int cachedInsetRight = -1;
    private int cachedInsetBottom = -1;
    private float cachedRequestedScale = Float.NaN;
    private float cachedDensity = Float.NaN;
    private float cachedDensityAwareScale = Float.NaN;
    private boolean attackHeld;
    private boolean blockHeld;
    private boolean gyroTracking;
    private float lastGyroYaw;
    private float lastGyroPitch;
    private long lastGyroTimestampNs;
    private final SensorManager sensorManager;
    private final Sensor rotationVector;
    private final float[] rotationMatrix = new float[9];
    private final float[] remappedRotationMatrix = new float[9];
    private final float[] orientation = new float[3];
    private boolean drawLogged;
    private boolean editMode;
    private boolean disposed;
    private boolean sensorRegistered;
    private int missionButtonStates;
    private long dismountHintVisibleUntil;
    private int promptTapPointerId = -1;
    private int radialPointerId = -1;
    private boolean radialMenuOpen;
    private int radialMenuTargetRole = ROLE_NONE;
    private float radialMenuAnchorX = Float.NaN;
    private float radialMenuAnchorY = Float.NaN;
    private final float[] radialMenuCenter = new float[2];
    private final float[] radialNodeCenter = new float[2];
    private int pausePointerId = -1;
    private boolean pauseLongPressTriggered;
    private int accessibilityFocusedButton = -1;
    private int hoveredButton = -1;
    private final Runnable enterButtonEditMode = new Runnable() {
        @Override public void run() {
            int id = pausePointerId;
            if (disposed || editMode || id < 0 || pointers.role(id) != ROLE_PAUSE
                    || movedBeyondTouchSlop(id, pointers.lastX[id], pointers.lastY[id])) {
                return;
            }
            pauseLongPressTriggered = true;
            performTouchHaptic(HapticFeedbackConstants.LONG_PRESS);
            ReleaseLog.i("MBControls", "button layout edit entered via Pause long-press");
            setEditMode(true);
        }
    };
    private final Runnable releasePause = new Runnable() {
        @Override public void run() {
            if (!disposed) nativeSetAction(ACTION_PAUSE, false);
        }
    };

    private boolean isButtonShown(int index) {
        if (index < 0 || index >= BUTTON_ROLES.length || !buttonVisible[index]) return false;
        int role = BUTTON_ROLES[index];
        return !radialMenuOpen || (role != ROLE_BACK && role != ROLE_ACTIONS_MENU);
    }

    private final class MissionAccessibilityProvider extends AccessibilityNodeProvider {
        @Override public AccessibilityNodeInfo createAccessibilityNodeInfo(int virtualViewId) {
            if (virtualViewId == HOST_ACCESSIBILITY_ID) {
                AccessibilityNodeInfo host = AccessibilityNodeInfo.obtain(MBTouchOverlay.this);
                MBTouchOverlay.this.onInitializeAccessibilityNodeInfo(host);
                host.setClassName(MBTouchOverlay.class.getName());
                host.setPackageName(getContext().getPackageName());
                for (int index = 0; index < BUTTON_ROLES.length; ++index) {
                    if (!isButtonShown(index)) continue;
                    host.addChild(MBTouchOverlay.this, index);
                }
                return host;
            }
            if (virtualViewId < 0 || virtualViewId >= BUTTON_ROLES.length) return null;
            if (!isButtonShown(virtualViewId)) return null;

            resolveButtonLayout();
            String languageCode = storedGameLanguage(preferences);
            int role = BUTTON_ROLES[virtualViewId];
            AccessibilityNodeInfo node = AccessibilityNodeInfo.obtain();
            node.setPackageName(getContext().getPackageName());
            node.setClassName(Button.class.getName());
            node.setSource(MBTouchOverlay.this, virtualViewId);
            node.setParent(MBTouchOverlay.this);
            node.setText(buttonLabel(languageCode, role));
            node.setContentDescription(buttonAccessibilityLabel(languageCode, role));
            node.setClickable(true);
            node.setLongClickable(role == ROLE_PAUSE);
            node.setFocusable(true);
            node.setEnabled(!disposed);

            float radius = buttonHitRadius[virtualViewId];
            int left = Math.max(0, (int)Math.floor(buttonCenterX[virtualViewId] - radius));
            int top = Math.max(0, (int)Math.floor(buttonCenterY[virtualViewId] - radius));
            int right = Math.min(getWidth(), (int)Math.ceil(buttonCenterX[virtualViewId] + radius));
            int bottom = Math.min(getHeight(), (int)Math.ceil(buttonCenterY[virtualViewId] + radius));
            Rect localBounds = new Rect(left, top, Math.max(left, right), Math.max(top, bottom));
            node.setBoundsInParent(localBounds);
            int[] screenLocation = new int[2];
            getLocationOnScreen(screenLocation);
            Rect screenBounds = new Rect(localBounds);
            screenBounds.offset(screenLocation[0], screenLocation[1]);
            node.setBoundsInScreen(screenBounds);
            node.setVisibleToUser(isShown() && !localBounds.isEmpty());

            node.addAction(AccessibilityNodeInfo.ACTION_CLICK);
            if (role == ROLE_PAUSE) node.addAction(AccessibilityNodeInfo.ACTION_LONG_CLICK);
            if (accessibilityFocusedButton == virtualViewId) {
                node.setAccessibilityFocused(true);
                node.addAction(AccessibilityNodeInfo.ACTION_CLEAR_ACCESSIBILITY_FOCUS);
            } else {
                node.addAction(AccessibilityNodeInfo.ACTION_ACCESSIBILITY_FOCUS);
            }
            return node;
        }

        @Override public boolean performAction(int virtualViewId, int action, Bundle arguments) {
            if (virtualViewId < 0 || virtualViewId >= BUTTON_ROLES.length) return false;
            if (action == AccessibilityNodeInfo.ACTION_CLICK) {
                return activateAccessibleButton(virtualViewId, false);
            }
            if (action == AccessibilityNodeInfo.ACTION_LONG_CLICK) {
                return activateAccessibleButton(virtualViewId, true);
            }
            if (action == AccessibilityNodeInfo.ACTION_ACCESSIBILITY_FOCUS) {
                if (accessibilityFocusedButton == virtualViewId) return false;
                int previous = accessibilityFocusedButton;
                accessibilityFocusedButton = virtualViewId;
                if (previous >= 0) sendVirtualAccessibilityEvent(previous,
                        AccessibilityEvent.TYPE_VIEW_ACCESSIBILITY_FOCUS_CLEARED);
                sendVirtualAccessibilityEvent(virtualViewId,
                        AccessibilityEvent.TYPE_VIEW_ACCESSIBILITY_FOCUSED);
                invalidate();
                return true;
            }
            if (action == AccessibilityNodeInfo.ACTION_CLEAR_ACCESSIBILITY_FOCUS) {
                if (accessibilityFocusedButton != virtualViewId) return false;
                accessibilityFocusedButton = -1;
                sendVirtualAccessibilityEvent(virtualViewId,
                        AccessibilityEvent.TYPE_VIEW_ACCESSIBILITY_FOCUS_CLEARED);
                invalidate();
                return true;
            }
            return false;
        }

        @Override public AccessibilityNodeInfo findFocus(int focus) {
            int virtualViewId = focus == AccessibilityNodeInfo.FOCUS_ACCESSIBILITY
                    ? accessibilityFocusedButton : -1;
            return virtualViewId < 0 ? null : createAccessibilityNodeInfo(virtualViewId);
        }
    }

    private boolean activateAccessibleButton(final int index, boolean longClick) {
        if (disposed || index < 0 || index >= BUTTON_ROLES.length || !isButtonShown(index)
                || !nativeReady() || !nativeMissionActive()) {
            return false;
        }
        final int role = BUTTON_ROLES[index];
        if (longClick && role != ROLE_PAUSE) return false;
        if (editMode && !longClick && role == ROLE_BACK) {
            setEditMode(false);
            ReleaseLog.i("MBControls", "button layout edit exited via accessible Back");
            sendVirtualAccessibilityEvent(index, AccessibilityEvent.TYPE_VIEW_CLICKED);
            return true;
        }
        if (role == ROLE_ACTIONS_MENU) {
            if (!showRadialActionsDialog()) return false;
            sendVirtualAccessibilityEvent(index, AccessibilityEvent.TYPE_VIEW_CLICKED);
            return true;
        }
        if (role == ROLE_CHAT) {
            nativeSetAction(ACTION_CHAT, true);
            sendVirtualAccessibilityEvent(index, AccessibilityEvent.TYPE_VIEW_CLICKED);
            return true;
        }
        if (role == ROLE_PAUSE) {
            if (longClick) {
                setEditMode(true);
            } else {
                if (editMode) setEditMode(false);
                else pauseBriefly();
            }
        } else {
            if (longClick || actionForRole(role) == 0) return false;
            pressRole(role, true);
            postDelayed(new Runnable() {
                @Override public void run() {
                    if (!disposed) pressRole(role, false);
                }
            }, 70L);
        }
        sendVirtualAccessibilityEvent(index, longClick
                ? AccessibilityEvent.TYPE_VIEW_LONG_CLICKED
                : AccessibilityEvent.TYPE_VIEW_CLICKED);
        return true;
    }

    private void sendVirtualAccessibilityEvent(int index, int eventType) {
        if (index < 0 || index >= BUTTON_ROLES.length || getParent() == null) return;
        AccessibilityManager manager = (AccessibilityManager)getContext()
                .getSystemService(Context.ACCESSIBILITY_SERVICE);
        if (manager == null || !manager.isEnabled()) return;
        String languageCode = storedGameLanguage(preferences);
        AccessibilityEvent event = AccessibilityEvent.obtain(eventType);
        event.setPackageName(getContext().getPackageName());
        event.setClassName(Button.class.getName());
        event.setEnabled(!disposed);
        event.setContentDescription(buttonAccessibilityLabel(
                languageCode, BUTTON_ROLES[index]));
        event.setSource(this, index);
        getParent().requestSendAccessibilityEvent(this, event);
    }

    private static void hideSystemBars(View decor) {
        decor.setSystemUiVisibility(SYSTEM_UI_FLAGS);
        if (Build.VERSION.SDK_INT >= 30) {
            WindowInsetsController controller = decor.getWindowInsetsController();
            if (controller != null) {
                controller.hide(WindowInsets.Type.systemBars());
                controller.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        }
    }

    private static SafeInsets safeInsets(View view) {
        if (view == null || Build.VERSION.SDK_INT < 23) return SafeInsets.NONE;
        WindowInsets insets = view.getRootWindowInsets();
        if (insets == null) return SafeInsets.NONE;
        int left = Math.max(0, insets.getSystemWindowInsetLeft());
        int top = Math.max(0, insets.getSystemWindowInsetTop());
        int right = Math.max(0, insets.getSystemWindowInsetRight());
        int bottom = Math.max(0, insets.getSystemWindowInsetBottom());
        if (Build.VERSION.SDK_INT >= 28) {
            DisplayCutout cutout = insets.getDisplayCutout();
            if (cutout != null) {
                left = Math.max(left, cutout.getSafeInsetLeft());
                top = Math.max(top, cutout.getSafeInsetTop());
                right = Math.max(right, cutout.getSafeInsetRight());
                bottom = Math.max(bottom, cutout.getSafeInsetBottom());
            }
        }
        if (Build.VERSION.SDK_INT >= 29) {
            android.graphics.Insets gesture = insets.getSystemGestureInsets();
            left = Math.max(left, gesture.left);
            top = Math.max(top, gesture.top);
            right = Math.max(right, gesture.right);
            bottom = Math.max(bottom, gesture.bottom);
        }
        return new SafeInsets(left, top, right, bottom);
    }

    private static String storedGameLanguage(SharedPreferences preferences) {
        // Language selection moved to the system: the framework seeds the
        // process default Locale from the per-app locale (Android 13+) or the
        // device language, and the guest engine applies the mapped code at boot.
        return MBTouchStrings.normalizeBcp47(Locale.getDefault().toLanguageTag());
    }

    private static void dismissLanguageButton() {
        PopupWindow popup = languageButtonPopup;
        languageButtonPopup = null;
        languageButtonX = -1;
        languageButtonY = -1;
        languageButtonSize = -1;
        if (popup != null) popup.dismiss();
    }

    private static void dismissLanguageDialog() {
        AlertDialog dialog = languageDialog;
        languageDialog = null;
        if (dialog != null) dialog.dismiss();
    }

    private static void dismissLanguageUi() {
        dismissLanguageDialog();
        dismissLanguageButton();
    }

    private static void dismissModButton() {
        PopupWindow popup = modButtonPopup;
        modButtonPopup = null;
        modButtonPlacement = null;
        modButton = null;
        if (popup != null) popup.dismiss();
    }

    private static void dismissModDialog() {
        AlertDialog dialog = modDialog;
        modDialog = null;
        if (dialog != null) dialog.dismiss();
    }

    private static void dismissModUi() {
        dismissModDialog();
        dismissModButton();
    }

    private static boolean russianUi(Activity activity) {
        SharedPreferences preferences = activity.getSharedPreferences(
                GameLanguage.PREFERENCES_NAME, Context.MODE_PRIVATE);
        return "ru".equals(storedGameLanguage(preferences));
    }

    private static String modButtonText(Activity activity, int count) {
        return MainMenuModButtonPolicy.label(russianUi(activity), count);
    }

    /** Places the Android-owned mod entry on the next native main-menu row. */
    static final class MainMenuModButtonPolicy {
        private static final float GAME_ASPECT = 17.0f / 9.0f;
        private static final float LEFT_IN_VIEWPORT = 0.071f;
        private static final float LEFT_IN_STRETCHED_SCREEN = 0.028f;
        private static final float TOP_IN_HEIGHT = 0.775f;
        private static final float WIDTH_IN_VIEWPORT = 0.233f;
        private static final float HEIGHT_IN_HEIGHT = 0.064f;

        static final class Placement {
            final int x;
            final int y;
            final int width;
            final int height;

            Placement(int x, int y, int width, int height) {
                this.x = x;
                this.y = y;
                this.width = width;
                this.height = height;
            }
        }

        static String label(boolean russian, int count) {
            String base = russian ? "Моды" : "Mods";
            return count > 0 ? base + " (" + count + ")" : base;
        }

        static Placement resolve(int decorWidth, int decorHeight,
                                 boolean keepInterfaceProportions) {
            int safeWidth = Math.max(1, decorWidth);
            int safeHeight = Math.max(1, decorHeight);
            int viewportWidth = Math.min(safeWidth,
                    Math.round(safeHeight * GAME_ASPECT));
            boolean stretched = !keepInterfaceProportions && viewportWidth < safeWidth;
            int layoutWidth = stretched ? safeWidth : viewportWidth;
            int x = stretched
                    ? Math.round(safeWidth * LEFT_IN_STRETCHED_SCREEN)
                    : (safeWidth - viewportWidth) / 2
                            + Math.round(viewportWidth * LEFT_IN_VIEWPORT);
            int y = Math.round(safeHeight * TOP_IN_HEIGHT);
            int width = Math.max(1, Math.round(layoutWidth * WIDTH_IN_VIEWPORT));
            int height = Math.max(1, Math.round(safeHeight * HEIGHT_IN_HEIGHT));
            return new Placement(x, y, width, height);
        }
    }

    private static void restartForModule(Activity activity) {
        try {
            Intent restarter = new Intent(activity, MBRestartActivity.class);
            restarter.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK
                    | Intent.FLAG_ACTIVITY_CLEAR_TOP);
            activity.startActivity(restarter);
        } catch (RuntimeException exception) {
            Toast.makeText(activity,
                    russianUi(activity) ? "Не удалось перезапустить игру"
                            : "Could not restart the game",
                    Toast.LENGTH_LONG).show();
            return;
        }
        activity.finishAffinity();
        android.os.Process.killProcess(android.os.Process.myPid());
    }

    private static int uiDp(Activity activity, float value) {
        return Math.round(value * activity.getResources().getDisplayMetrics().density);
    }

    private static void pulseMenuAction(final View decor, final int action) {
        nativeSetAction(action, true);
        decor.postDelayed(new Runnable() {
            @Override public void run() {
                if (nativeStarted) nativeSetAction(action, false);
            }
        }, MIN_ACTION_PULSE_MILLIS);
    }

    private static TextView floatingMenuButton(Activity activity, String label) {
        TextView button = new TextView(activity);
        button.setText(label);
        button.setContentDescription(label);
        button.setTextSize(16.0f);
        button.setTextColor(Color.WHITE);
        button.setGravity(Gravity.CENTER);
        button.setPadding(uiDp(activity, 8), 0, uiDp(activity, 8), 0);
        GradientDrawable background = new GradientDrawable();
        background.setColor(Color.argb(205, 28, 29, 33));
        background.setCornerRadius(uiDp(activity, 8));
        background.setStroke(uiDp(activity, 1), Color.rgb(210, 194, 145));
        button.setBackground(background);
        return button;
    }

    private static void dismissTacticalBackButton() {
        PopupWindow popup = tacticalBackPopup;
        tacticalBackPopup = null;
        if (popup != null) popup.dismiss();
    }

    private static void updateTacticalBackButton(final Activity activity, final View decor,
                                                  boolean show) {
        if (!show || activity != activeActivity) {
            dismissTacticalBackButton();
            return;
        }
        if (tacticalBackPopup != null && tacticalBackPopup.isShowing()) return;
        TextView button = floatingMenuButton(activity,
                russianUi(activity) ? "← Назад" : "← Back");
        button.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View ignored) {
                pulseMenuAction(decor, ACTION_PAUSE);
            }
        });
        PopupWindow popup = new PopupWindow(button, uiDp(activity, 90), uiDp(activity, 42), false);
        popup.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
        popup.setOutsideTouchable(false);
        popup.setClippingEnabled(true);
        tacticalBackPopup = popup;
        popup.showAtLocation(decor, Gravity.TOP | Gravity.LEFT,
                uiDp(activity, 12), uiDp(activity, 76));
    }

    private static void dismissPollAnswerButtons() {
        PopupWindow popup = pollAnswerPopup;
        pollAnswerPopup = null;
        if (popup != null) popup.dismiss();
    }

    private static void updatePollAnswerButtons(final Activity activity, final View decor,
                                                 boolean show) {
        if (!show || activity != activeActivity) {
            dismissPollAnswerButtons();
            return;
        }
        if (pollAnswerPopup != null && pollAnswerPopup.isShowing()) return;
        LinearLayout row = new LinearLayout(activity);
        row.setOrientation(LinearLayout.HORIZONTAL);
        String language = MBTouchStrings.normalizeBcp47(Locale.getDefault().toLanguageTag());
        final TextView yes = floatingMenuButton(activity,
                MBTouchStrings.get(language, MBTouchStrings.Text.POLL_YES));
        final TextView no = floatingMenuButton(activity,
                MBTouchStrings.get(language, MBTouchStrings.Text.POLL_NO));
        yes.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View ignored) {
                // L1 -> guest event 20 -> key 102 -> poll answer 1 (accept).
                pulseMenuAction(decor, ACTION_LB);
            }
        });
        no.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View ignored) {
                // R1 -> guest event 22 -> key 103 -> poll answer 0 (decline).
                pulseMenuAction(decor, ACTION_RB);
            }
        });
        LinearLayout.LayoutParams half = new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.MATCH_PARENT, 1.0f);
        half.setMargins(0, 0, uiDp(activity, 4), 0);
        row.addView(yes, half);
        half = new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.MATCH_PARENT, 1.0f);
        half.setMargins(uiDp(activity, 4), 0, 0, 0);
        row.addView(no, half);
        int width = uiDp(activity, 164);
        PopupWindow popup = new PopupWindow(row, width, uiDp(activity, 42), false);
        popup.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
        popup.setOutsideTouchable(false);
        popup.setClippingEnabled(true);
        pollAnswerPopup = popup;
        popup.showAtLocation(decor, Gravity.TOP | Gravity.LEFT,
                (decor.getWidth() - width) / 2, uiDp(activity, 76));
    }

    private static void dismissMasterServerField() {
        PopupWindow popup = masterServerPopup;
        masterServerPopup = null;
        masterServerField = null;
        if (popup != null) popup.dismiss();
    }

    private static void dismissMasterServerDialog() {
        AlertDialog dialog = masterServerDialog;
        masterServerDialog = null;
        if (dialog != null) dialog.dismiss();
    }

    private static MBMasterServerConfig.Selection currentMasterServerSelection(Activity activity) {
        try {
            return MBMasterServerConfig.readSelection(
                    new File(activity.getExternalFilesDir(null), "rgl_config.txt"));
        } catch (IOException | RuntimeException ignored) {
            return new MBMasterServerConfig.Selection(MBMasterServerConfig.MODE_LAN, "", "");
        }
    }

    private static void applySavedMasterServer(final Activity activity,
                                               final boolean russian) {
        final MBMasterServerConfig.Selection selected =
                currentMasterServerSelection(activity);
        if (!nativeSelectMasterServer(selected.mode, selected.host)) {
            Toast.makeText(activity,
                    russian ? "Не удалось переключить мастер-сервер"
                            : "Could not switch master server",
                    Toast.LENGTH_LONG).show();
            return;
        }
        dismissMasterServerField();
        final View decor = activeDecor;
        if (decor == null) return;
        decor.postDelayed(new Runnable() {
            private int attempts;

            @Override public void run() {
                if (activity != activeActivity || !activeLifecycleResumed) return;
                MBMasterServerConfig.Selection current =
                        currentMasterServerSelection(activity);
                if (!selected.mode.equals(current.mode)
                        || !selected.host.equals(current.host)) return;
                if (nativeMasterServerApplied(selected.mode, selected.host)) {
                    Toast.makeText(activity,
                            russian ? "Мастер-сервер переключён"
                                    : "Master server switched",
                            Toast.LENGTH_SHORT).show();
                } else if (++attempts < 12) {
                    decor.postDelayed(this, 250L);
                } else {
                    Toast.makeText(activity,
                            russian ? "Не удалось применить мастер-сервер"
                                    : "Could not apply master server",
                            Toast.LENGTH_LONG).show();
                }
            }
        }, 250L);
    }

    private static String mapTimeLabel(Activity activity, boolean running) {
        if (russianUi(activity)) return running ? "Ⅱ Пауза" : "▶ Время";
        return running ? "Ⅱ Pause" : "▶ Time";
    }

    private static void dismissMapTimeButton() {
        PopupWindow popup = mapTimePopup;
        mapTimePopup = null;
        mapTimeButton = null;
        if (popup != null) popup.dismiss();
    }

    private static void updateMapTimeButton(final Activity activity,
                                            final View decor, boolean show) {
        if (!show || activity != activeActivity) {
            dismissMapTimeButton();
            return;
        }
        boolean running = nativeMapTimeRunning();
        if (mapTimePopup != null && mapTimePopup.isShowing()
                && mapTimeButton != null) {
            String label = mapTimeLabel(activity, running);
            mapTimeButton.setText(label);
            mapTimeButton.setContentDescription(label);
            return;
        }
        final TextView button = floatingMenuButton(activity,
                mapTimeLabel(activity, running));
        button.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View ignored) {
                boolean nowRunning = nativeToggleMapTime();
                String label = mapTimeLabel(activity, nowRunning);
                button.setText(label);
                button.setContentDescription(label);
            }
        });
        PopupWindow popup = new PopupWindow(button, uiDp(activity, 120),
                uiDp(activity, 42), false);
        popup.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
        popup.setOutsideTouchable(false);
        popup.setClippingEnabled(true);
        mapTimePopup = popup;
        mapTimeButton = button;
        popup.showAtLocation(decor, Gravity.TOP | Gravity.RIGHT,
                uiDp(activity, 12), uiDp(activity, 12));
    }

    private static String masterServerChoiceLabel(boolean russian, String mode, String savedHost) {
        if (MBMasterServerConfig.MODE_LAN.equals(mode)) {
            return russian ? "Только LAN" : "LAN only";
        }
        if (MBMasterServerConfig.MODE_OFFICIAL.equals(mode)) {
            return russian ? "Официальный (не совместим с Android)" : "Official (not Android compatible)";
        }
        if (MBMasterServerConfig.MODE_OREO922.equals(mode)) return MBMasterServerConfig.OREO922_HOST;
        return russian ? "Сохранённый: " + savedHost : "Saved: " + savedHost;
    }

    private static void showAddMasterServerDialog(final Activity activity, final File configFile,
                                                   final boolean russian) {
        final EditText input = new EditText(activity);
        input.setSingleLine(true);
        input.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        input.setHint("master.example.org");
        new AlertDialog.Builder(activity)
                .setTitle(russian ? "Добавить мастер-сервер" : "Add master server")
                .setMessage(russian ? "Введите домен или IP-адрес без http:// и порта"
                        : "Enter a hostname or IP address without http:// or a port")
                .setView(input)
                .setPositiveButton(russian ? "Сохранить" : "Save",
                        new DialogInterface.OnClickListener() {
                    @Override public void onClick(DialogInterface ignored, int which) {
                        try {
                            MBMasterServerConfig.saveCustomHost(configFile,
                                    input.getText().toString());
                        } catch (IOException | RuntimeException error) {
                            Toast.makeText(activity,
                                    russian ? "Неверный адрес или список заполнен"
                                            : "Invalid address or saved list is full",
                                    Toast.LENGTH_LONG).show();
                            return;
                        }
                        applySavedMasterServer(activity, russian);
                    }
                })
                .setNegativeButton(russian ? "Отмена" : "Cancel", null)
                .show();
    }

    private static void showRemoveMasterServerDialog(final Activity activity, final File configFile,
                                                      final boolean russian,
                                                      final String[] savedHosts) {
        new AlertDialog.Builder(activity)
                .setTitle(russian ? "Удалить сохранённый сервер" : "Remove a saved server")
                .setItems(savedHosts, new DialogInterface.OnClickListener() {
                    @Override public void onClick(DialogInterface ignored, int which) {
                        try {
                            MBMasterServerConfig.removeCustomHost(configFile, savedHosts[which]);
                        } catch (IOException | RuntimeException error) {
                            Toast.makeText(activity,
                                    russian ? "Не удалось удалить адрес"
                                            : "Could not remove the address",
                                    Toast.LENGTH_LONG).show();
                            return;
                        }
                        applySavedMasterServer(activity, russian);
                    }
                })
                .setNegativeButton(russian ? "Отмена" : "Cancel", null)
                .show();
    }

    private static void showMasterServerDialog(final Activity activity) {
        if (activity != activeActivity || activity.isFinishing()) return;
        if (masterServerDialog != null && masterServerDialog.isShowing()) return;
        final boolean russian = russianUi(activity);
        final File configFile = new File(activity.getExternalFilesDir(null), "rgl_config.txt");
        final MBMasterServerConfig.Selection selection = currentMasterServerSelection(activity);
        final String[] modes = new String[3 + selection.savedHosts.length];
        final String[] customHosts = new String[modes.length];
        modes[0] = MBMasterServerConfig.MODE_LAN;
        modes[1] = MBMasterServerConfig.MODE_OFFICIAL;
        modes[2] = MBMasterServerConfig.MODE_OREO922;
        for (int index = 0; index < selection.savedHosts.length; ++index) {
            modes[3 + index] = MBMasterServerConfig.MODE_CUSTOM;
            customHosts[3 + index] = selection.savedHosts[index];
        }
        final String[] labels = new String[modes.length];
        int checkedItem = 0;
        for (int index = 0; index < modes.length; ++index) {
            String mode = modes[index];
            String label;
            if (MBMasterServerConfig.MODE_LAN.equals(mode)) {
                label = russian ? "Только LAN" : "LAN only";
            } else if (MBMasterServerConfig.MODE_OFFICIAL.equals(mode)) {
                label = russian ? "Официальный мастер-сервер (не совместим с Android)"
                        : "Official master server (not Android compatible)";
            } else if (MBMasterServerConfig.MODE_OREO922.equals(mode)) {
                label = MBMasterServerConfig.OREO922_HOST;
            } else {
                label = masterServerChoiceLabel(russian, mode, customHosts[index]);
            }
            labels[index] = label;
            if (mode.equals(selection.mode) && (!MBMasterServerConfig.MODE_CUSTOM.equals(mode)
                    || customHosts[index].equals(selection.host))) checkedItem = index;
        }
        AlertDialog.Builder builder = new AlertDialog.Builder(activity)
                .setTitle(russian ? "Мастер-сервер" : "Master server")
                .setSingleChoiceItems(labels, checkedItem, new DialogInterface.OnClickListener() {
                    @Override public void onClick(DialogInterface ignored, int which) {
                        String selectedMode = modes[which];
                        if (selectedMode.equals(selection.mode)
                                && (!MBMasterServerConfig.MODE_CUSTOM.equals(selectedMode)
                                    || customHosts[which].equals(selection.host))) {
                            ignored.dismiss();
                            return;
                        }
                        try {
                            if (MBMasterServerConfig.MODE_CUSTOM.equals(selectedMode)) {
                                MBMasterServerConfig.selectCustomHost(configFile,
                                        customHosts[which]);
                            } else {
                                MBMasterServerConfig.writeMode(configFile, selectedMode);
                            }
                        } catch (IOException | RuntimeException error) {
                            Toast.makeText(activity,
                                    russian ? "Не удалось сохранить выбор" : "Could not save choice",
                                    Toast.LENGTH_LONG).show();
                            return;
                        }
                        ignored.dismiss();
                        applySavedMasterServer(activity, russian);
                    }
                })
                .setPositiveButton(russian ? "Добавить" : "Add", new DialogInterface.OnClickListener() {
                    @Override public void onClick(DialogInterface ignored, int which) {
                        showAddMasterServerDialog(activity, configFile, russian);
                    }
                })
                .setNegativeButton(russian ? "Отмена" : "Cancel", null);
        if (selection.savedHosts.length != 0) {
            builder.setNeutralButton(russian ? "Удалить" : "Remove",
                    new DialogInterface.OnClickListener() {
                @Override public void onClick(DialogInterface ignored, int which) {
                    showRemoveMasterServerDialog(activity, configFile, russian,
                            selection.savedHosts);
                }
            });
        }
        final AlertDialog dialog = builder.create();
        dialog.setOnDismissListener(new DialogInterface.OnDismissListener() {
            @Override public void onDismiss(DialogInterface ignored) {
                if (masterServerDialog == dialog) masterServerDialog = null;
            }
        });
        masterServerDialog = dialog;
        dialog.show();
    }

    private static void updateMasterServerField(final Activity activity, final View decor,
                                                 boolean show) {
        if (!show || activity != activeActivity || externalFilesRoot == null) {
            dismissMasterServerField();
            if (!show) dismissMasterServerDialog();
            return;
        }
        if (masterServerPopup != null && masterServerPopup.isShowing()) return;
        LinearLayout panel = new LinearLayout(activity);
        panel.setOrientation(LinearLayout.VERTICAL);
        TextView label = new TextView(activity);
        label.setText(russianUi(activity) ? "Мастер-сервер" : "Master server");
        label.setTextColor(Color.rgb(42, 31, 21));
        label.setTextSize(15.0f);
        panel.addView(label, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, uiDp(activity, 22)));
        TextView field = new TextView(activity);
        MBMasterServerConfig.Selection selection = currentMasterServerSelection(activity);
        field.setText(masterServerChoiceLabel(russianUi(activity), selection.mode,
                selection.savedHost) + "  ✎");
        field.setContentDescription(russianUi(activity)
                ? "Изменить мастер-сервер" : "Edit master server");
        field.setSingleLine(true);
        field.setEllipsize(android.text.TextUtils.TruncateAt.END);
        field.setTextColor(Color.rgb(30, 24, 20));
        field.setTextSize(16.0f);
        field.setGravity(Gravity.CENTER_VERTICAL);
        field.setPadding(uiDp(activity, 8), 0, uiDp(activity, 8), 0);
        GradientDrawable fieldBackground = new GradientDrawable();
        fieldBackground.setColor(Color.argb(225, 229, 213, 180));
        fieldBackground.setStroke(uiDp(activity, 1), Color.rgb(91, 70, 46));
        fieldBackground.setCornerRadius(uiDp(activity, 4));
        field.setBackground(fieldBackground);
        field.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View ignored) {
                showMasterServerDialog(activity);
            }
        });
        panel.addView(field, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, uiDp(activity, 38)));
        int width = Math.min(uiDp(activity, 190), Math.round(decor.getWidth() * 0.23f));
        int x = Math.round(decor.getWidth() * 0.55f);
        int y = Math.round(decor.getHeight() * 0.69f);
        PopupWindow popup = new PopupWindow(panel, width, uiDp(activity, 60), false);
        popup.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
        popup.setOutsideTouchable(false);
        popup.setClippingEnabled(true);
        masterServerPopup = popup;
        masterServerField = field;
        popup.showAtLocation(decor, Gravity.TOP | Gravity.LEFT, x, y);
    }

    private static Intent modsDirectoryIntent(Activity activity, File directory,
                                              String mimeType) {
        String documentId = MBModManager.externalModulesDocumentId(
                activity.getPackageName());
        Uri documentUri = DocumentsContract.buildDocumentUri(
                "com.android.externalstorage.documents", documentId);
        Intent intent = new Intent(Intent.ACTION_VIEW);
        intent.setDataAndType(documentUri, mimeType);
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK
                | Intent.FLAG_GRANT_READ_URI_PERMISSION
                | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        intent.putExtra(EXTRA_ABSOLUTE_PATH, directory.getAbsolutePath());
        return intent;
    }

    private static boolean launchDirectoryIntent(Activity activity, Intent intent) {
        try {
            // Do not preflight with PackageManager.queryIntentActivities(). On
            // targetSdk 30+ package visibility can hide a perfectly usable
            // DocumentsUI/file-manager activity from queries while Android is
            // still allowed to resolve and launch this implicit intent.
            activity.startActivity(intent);
            return true;
        } catch (ActivityNotFoundException | SecurityException ignored) {
            return false;
        }
    }

    private static void showNoModsFileManagerToast(Activity activity, boolean russian) {
        String relativePath = MBModManager.externalModulesRelativePath(
                activity.getPackageName());
        String message = russian
                ? "Не найден файловый менеджер с доступом к Android/data. "
                        + "Откройте вручную:\n" + relativePath
                        + "\nПримеры: X-plore + Shizuku, Material Files + Shizuku."
                : "No file manager with Android/data access was found. "
                        + "Open this folder manually:\n" + relativePath
                        + "\nExamples: X-plore + Shizuku, Material Files + Shizuku.";
        Toast.makeText(activity, message, Toast.LENGTH_LONG).show();
    }

    private static void openModsFolder(Activity activity, boolean russian) {
        if (externalFilesRoot == null) return;
        File directory = new File(externalFilesRoot, MBModManager.MODULES_DIRECTORY);
        if ((!directory.isDirectory() && !directory.mkdirs()) || !directory.isDirectory()) {
            Toast.makeText(activity,
                    russian ? "Не удалось создать папку модов"
                            : "Could not create the mods folder",
                    Toast.LENGTH_LONG).show();
            return;
        }
        if (launchDirectoryIntent(activity,
                modsDirectoryIntent(activity, directory, DIRECTORY_MIME_TYPE))) {
            return;
        }
        if (launchDirectoryIntent(activity,
                modsDirectoryIntent(activity, directory, LEGACY_DIRECTORY_MIME_TYPE))) {
            return;
        }
        showNoModsFileManagerToast(activity, russian);
    }

    private static void showModDialog(final Activity activity) {
        if (activity != activeActivity || activity.isFinishing() || externalFilesRoot == null) return;
        if (modDialog != null && modDialog.isShowing()) return;
        dismissModDialog();

        final boolean russian = russianUi(activity);
        final String moduleLanguage = storedGameLanguage(activity.getSharedPreferences(
                GameLanguage.PREFERENCES_NAME, Context.MODE_PRIVATE));
        final List<MBModManager.Module> modules = MBModManager.scan(externalFilesRoot);
        final String[] labels = new String[modules.size() + 1];
        labels[0] = russian ? "Native — основная игра" : "Native — base game";
        int selectedIndex = 0;
        String selectedId = activeModSelection == null
                ? MBModManager.BASE_MODULE : activeModSelection.id;
        for (int i = 0; i < modules.size(); ++i) {
            MBModManager.Module module = modules.get(i);
            if (module.valid) {
                labels[i + 1] = module.id;
            } else {
                labels[i + 1] = "⚠ " + module.id + " — "
                        + (russian ? "повреждён: " : "broken: ") + module.error;
            }
            if (module.id.equals(selectedId)) selectedIndex = i + 1;
        }
        final int[] choice = {selectedIndex};
        Button openFolderButton = new Button(activity);
        openFolderButton.setAllCaps(false);
        openFolderButton.setText(russian ? "Открыть папку модов" : "Open mods folder");
        openFolderButton.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View ignored) {
                openModsFolder(activity, russian);
            }
        });
        final AlertDialog dialog = new AlertDialog.Builder(activity)
                .setTitle(russian ? "Менеджер модов" : "Mod manager")
                .setView(openFolderButton)
                .setSingleChoiceItems(labels, selectedIndex,
                        new DialogInterface.OnClickListener() {
                            @Override public void onClick(DialogInterface ignored, int which) {
                                choice[0] = which;
                            }
                        })
                .setPositiveButton(russian ? "Выбрать и перезапустить"
                                : "Select and restart",
                        new DialogInterface.OnClickListener() {
                            @Override public void onClick(DialogInterface ignored, int which) {
                                final String id = choice[0] == 0 ? MBModManager.BASE_MODULE
                                        : modules.get(choice[0] - 1).id;
                                if (choice[0] > 0 && !modules.get(choice[0] - 1).valid) {
                                    Toast.makeText(activity,
                                            russian ? "Этот мод повреждён и не будет запущен"
                                                    : "This mod is broken and will not be started",
                                            Toast.LENGTH_LONG).show();
                                    return;
                                }
                                File moduleDirectory = choice[0] == 0 ? null
                                        : modules.get(choice[0] - 1).directory;
                                // Finish conversion before persisting a module switch. Cancelling
                                // leaves the current game and selected module usable.
                                MBModTextureConverter.prepare(activity, moduleDirectory,
                                        moduleLanguage,
                                        new Runnable() {
                                            @Override public void run() {
                                                if (activity.isFinishing() || activity.isDestroyed()) return;
                                                if (!MBModManager.select(externalFilesRoot, id)) {
                                                    Toast.makeText(activity,
                                                            russian ? "Не удалось сохранить выбор мода"
                                                                    : "Could not save the selected mod",
                                                            Toast.LENGTH_LONG).show();
                                                    return;
                                                }
                                                restartForModule(activity);
                                            }
                                        }, new Runnable() {
                                            @Override public void run() {
                                                if (!activity.isFinishing() && !activity.isDestroyed()) {
                                                    Toast.makeText(activity,
                                                            MBModTextureStrings.get(moduleLanguage,
                                                                    MBModTextureStrings.Text.LAUNCH_CANCELLED),
                                                            Toast.LENGTH_SHORT).show();
                                                }
                                            }
                                        });
                            }
                        })
                .setNeutralButton(russian ? "Обновить список" : "Refresh",
                        new DialogInterface.OnClickListener() {
                            @Override public void onClick(DialogInterface ignored, int which) {
                                modDialog = null;
                                activity.getWindow().getDecorView().post(new Runnable() {
                                    @Override public void run() {
                                        showModDialog(activity);
                                    }
                                });
                            }
                        })
                .setNegativeButton(russian ? "Отмена" : "Cancel", null)
                .create();
        dialog.setOnDismissListener(new DialogInterface.OnDismissListener() {
            @Override public void onDismiss(DialogInterface ignored) {
                if (modDialog == dialog) modDialog = null;
            }
        });
        modDialog = dialog;
        dialog.show();
        if (modButton != null) {
            String label = modButtonText(activity, modules.size());
            modButton.setText(label);
            modButton.setContentDescription(label);
        }
    }

    private static void updateModButton(final Activity activity, View decor,
                                        boolean ready, boolean mission, int activeWindowId) {
        boolean show = ready && !mission
                && activeWindowId == SystemBackPolicy.ROOT_MENU_WINDOW_ID
                && activity == activeActivity && externalFilesRoot != null;
        if (!show) {
            dismissModButton();
            if (mission || activity != activeActivity) dismissModDialog();
            return;
        }
        SharedPreferences preferences = activity.getSharedPreferences(
                GameLanguage.PREFERENCES_NAME, Context.MODE_PRIVATE);
        boolean keepInterfaceProportions = safeBooleanPreference(preferences,
                GuestSettingsSync.KEY_MENU_ASPECT, true);
        MainMenuModButtonPolicy.Placement placement =
                MainMenuModButtonPolicy.resolve(decor.getWidth(), decor.getHeight(),
                        keepInterfaceProportions);
        if (modButtonPopup != null && modButtonPopup.isShowing()) {
            MainMenuModButtonPolicy.Placement previous = modButtonPlacement;
            if (previous == null || previous.x != placement.x || previous.y != placement.y
                    || previous.width != placement.width || previous.height != placement.height) {
                modButtonPopup.update(placement.x, placement.y,
                        placement.width, placement.height);
                modButtonPlacement = placement;
            }
            return;
        }
        int count = MBModManager.scan(externalFilesRoot).size();
        TextView button = new TextView(activity);
        button.setText(modButtonText(activity, count));
        button.setContentDescription(modButtonText(activity, count));
        button.setTextSize(20.0f);
        button.setTypeface(Typeface.create("sans-serif-light", Typeface.NORMAL));
        button.setGravity(Gravity.CENTER);
        button.setIncludeFontPadding(false);
        button.setPadding(0, 0, 0, 0);
        button.setBackgroundColor(Color.TRANSPARENT);
        button.setTextColor(new ColorStateList(
                new int[][] {
                        new int[] {android.R.attr.state_pressed},
                        new int[] {android.R.attr.state_focused},
                        new int[] {}
                },
                new int[] {
                        Color.rgb(245, 203, 31),
                        Color.rgb(245, 203, 31),
                        Color.rgb(34, 24, 16)
                }));
        button.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View ignored) {
                showModDialog(activity);
            }
        });
        PopupWindow popup = new PopupWindow(button, placement.width,
                placement.height, false);
        popup.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
        popup.setTouchable(true);
        popup.setOutsideTouchable(false);
        popup.setClippingEnabled(true);
        popup.setInputMethodMode(PopupWindow.INPUT_METHOD_NOT_NEEDED);
        modButton = button;
        modButtonPopup = popup;
        modButtonPlacement = placement;
        popup.showAtLocation(decor, Gravity.LEFT | Gravity.TOP,
                placement.x, placement.y);
    }

    private static void showLanguageDialog(final Activity activity) {
        if (activity != activeActivity || activity.isFinishing()) return;
        if (languageDialog != null && languageDialog.isShowing()) return;
        dismissLanguageDialog();

        final SharedPreferences preferences = activity.getSharedPreferences(
                GameLanguage.PREFERENCES_NAME, Context.MODE_PRIVATE);
        final String dialogLanguage = storedGameLanguage(preferences);
        final int[] selected = {GameLanguage.indexOf(dialogLanguage)};
        final AlertDialog dialog = new AlertDialog.Builder(activity)
                .setTitle(MBTouchStrings.get(
                        dialogLanguage, MBTouchStrings.Text.LANGUAGE_DIALOG_TITLE))
                .setSingleChoiceItems(GameLanguage.DISPLAY_NAMES, selected[0],
                        new DialogInterface.OnClickListener() {
                            @Override public void onClick(DialogInterface ignored, int which) {
                                selected[0] = which;
                            }
                        })
                .setPositiveButton(MBTouchStrings.get(
                        dialogLanguage, MBTouchStrings.Text.SAVE),
                        new DialogInterface.OnClickListener() {
                    @Override public void onClick(DialogInterface ignored, int which) {
                        String code = GameLanguage.codeAt(selected[0]);
                        boolean saved = preferences.edit()
                                .putString(GameLanguage.PREFERENCE_KEY, code)
                                .commit();
                        String message = MBTouchStrings.get(
                                saved ? code : dialogLanguage,
                                saved
                                        ? MBTouchStrings.Text.LANGUAGE_RESTART_REQUIRED
                                        : MBTouchStrings.Text.LANGUAGE_SAVE_FAILED);
                        Toast.makeText(activity, message, Toast.LENGTH_LONG).show();
                    }
                })
                .setNegativeButton(MBTouchStrings.get(
                        dialogLanguage, MBTouchStrings.Text.CANCEL), null)
                .create();
        dialog.setOnDismissListener(new DialogInterface.OnDismissListener() {
            @Override public void onDismiss(DialogInterface ignored) {
                if (languageDialog == dialog) languageDialog = null;
            }
        });
        languageDialog = dialog;
        dialog.show();
    }

    private static void updateLanguageButton(final Activity activity, View decor,
                                             boolean ready, boolean mission,
                                             boolean optionsWindow) {
        // The floating settings entry is gone for good: the Additional tab in
        // the native Options screen owns every touch setting now. Cleanup only.
        dismissLanguageButton();
        if (mission || activity != activeActivity) {
            dismissLanguageDialog();
        }
    }

    public static void attach(Activity activity) {
        if (activeActivity == activity && activeDecor != null) return;
        if (activeActivity != null) detach(activeActivity);
        registerSystemBack(activity);
        activeActivity = activity;
        activeDecor = activity.getWindow().getDecorView();
        dismissLanguageUi();
        dismissModUi();
        hideSystemBars(activeDecor);
        syncNativeMenuTouchConfiguration(activity, activeDecor);
        nativeStarted = false;
        INPUT_STALL_POLICY.reset();
        guestSettingsOptionsWasActive = false;
        guestSettingsFinalPollPending = false;
        guestSettingsInitialSyncDone = false;
        activeLifecycleResumed = true;
        activeDecor.postDelayed(OVERLAY_POLL, 200L);
    }

    /** Register the API 33+ back callback used by target-SDK 33 and newer apps. */
    private static void registerSystemBack(Activity activity) {
        if (Build.VERSION.SDK_INT < 33) return;
        Api33SystemBack.register(activity);
    }

    private static void unregisterSystemBack() {
        if (Build.VERSION.SDK_INT >= 33) Api33SystemBack.unregister();
    }

    /** Route Android Back through the action used by the current guest window. */
    public static void onSystemBack() {
        Activity activity = activeActivity;
        View decor = activeDecor;
        if (activity == null || decor == null || !activeLifecycleResumed || !nativeStarted) {
            return;
        }

        MBTouchOverlay editingOverlay = activeOverlay;
        if (editingOverlay != null && !editingOverlay.disposed && editingOverlay.editMode) {
            editingOverlay.setEditMode(false);
            ReleaseLog.i("MBControls", "button layout edit exited via system Back");
            return;
        }

        boolean ready = nativeReady();
        boolean mission = ready && nativeMissionActive();
        if (mission && nativeTacticalModalState() != 0) {
            int presentation = nativeActivePresentationId();
            pulseMenuAction(decor,
                    MBBattleTouchPolicy.shouldConfirmMultiplayerWelcomeTap(presentation)
                            ? ACTION_ENTER_CONFIRM : ACTION_PAUSE);
            return;
        }
        int activeWindowId = ready && !mission ? nativeActiveMenuWindowId() : -1;
        if (!SystemBackPolicy.shouldDispatch(
                nativeStarted, ready, mission, activeWindowId)) {
            ReleaseLog.i("MBControls", "system Back consumed on root or unavailable game window");
            return;
        }

        final MBTouchOverlay overlay = mission ? activeOverlay : null;
        if (overlay != null && !overlay.disposed) {
            // Use the overlay's reference counts so a simultaneous touch-held Back
            // button cannot be released by this short system-key pulse.
            overlay.pressRole(ROLE_BACK, true);
            decor.postDelayed(new Runnable() {
                @Override public void run() {
                    if (!overlay.disposed) overlay.pressRole(ROLE_BACK, false);
                    else if (nativeStarted) nativeSetAction(ACTION_BACK, false);
                }
            }, SYSTEM_BACK_HOLD_MILLIS);
            return;
        }

        nativeSetAction(ACTION_BACK, true);
        decor.postDelayed(new Runnable() {
            @Override public void run() {
                if (nativeStarted) nativeSetAction(ACTION_BACK, false);
            }
        }, SYSTEM_BACK_HOLD_MILLIS);
    }

    public static void pause(Activity activity) {
        if (activeActivity != activity) return;
        activeLifecycleResumed = false;
        View decor = activeDecor;
        if (decor != null) decor.removeCallbacks(OVERLAY_POLL);
        nativeConfigureMenuTouch(false, false, false, 1.0f, 1.0f, 0, 0);
        MBTouchOverlay overlay = activeOverlay;
        if (overlay != null && !overlay.disposed) {
            overlay.unregisterSensor();
            overlay.resetAll();
        }
        dismissLanguageUi();
        dismissModUi();
        dismissTacticalBackButton();
        dismissMapTimeButton();
        dismissPollAnswerButtons();
        dismissMasterServerField();
        dismissMasterServerDialog();
    }

    public static void resume(Activity activity) {
        if (activeActivity != activity || activeDecor == null || activity.isFinishing()) return;
        activeLifecycleResumed = true;
        activeDecor.removeCallbacks(OVERLAY_POLL);
        hideSystemBars(activeDecor);
        syncNativeMenuTouchConfiguration(activity, activeDecor);
        MBTouchOverlay overlay = activeOverlay;
        if (overlay != null && !overlay.disposed
                && overlay.getWindowVisibility() == View.VISIBLE) {
            overlay.registerSensor();
        }
        activeDecor.post(OVERLAY_POLL);
    }

    public static void detach(Activity activity) {
        if (activeActivity != activity) return;
        unregisterSystemBack();
        View decor = activeDecor;
        PopupWindow popup = activePopup;
        MBTouchOverlay overlay = activeOverlay;
        boolean started = nativeStarted;
        if (decor != null) decor.removeCallbacks(OVERLAY_POLL);
        nativeConfigureMenuTouch(false, false, false, 1.0f, 1.0f, 0, 0);
        dismissLanguageUi();
        dismissModUi();
        dismissTacticalBackButton();
        dismissMapTimeButton();
        dismissPollAnswerButtons();
        dismissMasterServerField();
        dismissMasterServerDialog();
        activeActivity = null;
        activeDecor = null;
        activePopup = null;
        activePopupReceivesTouches = false;
        activeOverlay = null;
        nativeStarted = false;
        INPUT_STALL_POLICY.reset();
        guestSettingsOptionsWasActive = false;
        guestSettingsFinalPollPending = false;
        guestSettingsInitialSyncDone = false;
        activeLifecycleResumed = false;
        if (overlay != null) overlay.dispose();
        if (popup != null) popup.dismiss();
        if (started) nativeShutdown();
        ReleaseLog.i("MBControls", "overlay detached");
    }

    private static File requireDirectory(File directory, String name) {
        if (!directory.exists() && !directory.mkdirs() && !directory.exists()) {
            throw new IllegalStateException("Unable to create " + name + " game data directory: " + directory);
        }
        if (!directory.isDirectory()) {
            throw new IllegalStateException("Game data path is not a directory (" + name + "): " + directory);
        }
        return directory;
    }

    private static String canonicalRoot(File directory, String name) {
        final String canonical;
        try {
            canonical = directory.getCanonicalPath();
        } catch (IOException exception) {
            throw new IllegalStateException("Unable to canonicalize " + name + " game data root: " + directory, exception);
        }
        String path = canonical;
        while (path.length() > 1 && (path.endsWith("/") || path.endsWith("\\"))) {
            path = path.substring(0, path.length() - 1);
        }
        if (!new File(path).isAbsolute()) {
            throw new IllegalStateException("Game data root is not absolute (" + name + "): " + path);
        }
        if (path.getBytes(java.nio.charset.Charset.forName("UTF-8")).length >= 511) {
            throw new IllegalStateException("Game data root exceeds 510 UTF-8 bytes (" + name + "): " + path);
        }
        return path;
    }

    public static synchronized void prepareGameData(Activity activity) {
        if (gameDataPrepared) return;
        if (activity == null) {
            throw new IllegalStateException("prepareGameData requires an Activity");
        }
        File externalFilesDir = activity.getExternalFilesDir(null);
        if (externalFilesDir == null) {
            throw new IllegalStateException(
                    "App-specific external storage is unavailable; refusing internal save fallback");
        }
        SharedPreferences preferences = activity.getSharedPreferences(
                GameLanguage.PREFERENCES_NAME, Context.MODE_PRIVATE);
        if (!safeBooleanPreference(preferences, KEY_EXTERNAL_STORAGE_MIGRATION_V1, false)) {
            MBStorageMigrator.Result migration = MBStorageMigrator.copyMissing(
                    activity.getFilesDir(), externalFilesDir);
            if (migration.succeeded()) {
                boolean marked = preferences.edit()
                        .putBoolean(KEY_EXTERNAL_STORAGE_MIGRATION_V1, true)
                        .commit();
                ReleaseLog.i("MBControls", "legacy storage migration copied="
                        + migration.copiedFiles + " skipped=" + migration.skippedFiles
                        + " bytes=" + migration.copiedBytes + " marked=" + marked);
            } else {
                ReleaseLog.w("MBControls", "legacy storage migration will retry: "
                        + migration.failure);
            }
        }
        MBRglConfigNormalizer.Result configNormalization =
                MBRglConfigNormalizer.normalizeAll(externalFilesDir);
        ReleaseLog.i("MBControls", "rgl_config normalization changed="
                + configNormalization.normalizedFiles + " unchanged="
                + configNormalization.unchangedFiles + " failed="
                + configNormalization.failedFiles);
        File modsDir = requireDirectory(new File(externalFilesDir, "mods"), "mods");
        String root = canonicalRoot(modsDir, "mods");
        MBModManager.Selection moduleSelection = MBModManager.resolve(externalFilesDir);
        String moduleRoot = moduleSelection.moduleDirectory == null ? ""
                : canonicalRoot(moduleSelection.moduleDirectory, "selected module");
        String saveRoot = canonicalRoot(moduleSelection.saveDirectory, "module saves");
        String language = storedGameLanguage(preferences);
        MBMasterServerConfig.Selection masterServer;
        try {
            masterServer = MBMasterServerConfig.readSelection(
                    new File(externalFilesDir, "rgl_config.txt"));
        } catch (IOException exception) {
            masterServer = new MBMasterServerConfig.Selection(
                    MBMasterServerConfig.MODE_LAN, "", "");
        }
        if (!nativePrepareGameData(activity.getAssets(), root, root, language,
                moduleRoot, saveRoot, masterServer.mode, masterServer.host)) {
            throw new IllegalStateException("Native game data initialization failed");
        }
        externalFilesRoot = externalFilesDir;
        activeModSelection = moduleSelection;
        pendingModError = moduleSelection.fallbackError;
        gameDataRoot = root;
        gameDataPrepared = true;
    }

    public static synchronized String getMainGameDataRoot() {
        if (!gameDataPrepared || gameDataRoot == null) {
            throw new IllegalStateException("Main game data root requested before prepareGameData");
        }
        return gameDataRoot;
    }

    public static synchronized String getPatchGameDataRoot() {
        if (!gameDataPrepared || gameDataRoot == null) {
            throw new IllegalStateException("Patch game data root requested before prepareGameData");
        }
        return gameDataRoot;
    }

    public static String resolveMountedObbPath(StorageManager manager, String path) {
        RuntimeException failure = null;
        for (int attempt = 0; attempt < 10; ++attempt) {
            try {
                String mounted = manager.getMountedObbPath(path);
                if (mounted != null) return mounted;
            } catch (RuntimeException exception) {
                failure = exception;
            }
            SystemClock.sleep(50L);
        }
        ReleaseLog.w("MBControls", "mounted OBB path unavailable: " + path, failure);
        return null;
    }

    public MBTouchOverlay(Context context) {
        super(context);
        for (int index = 0; index < buttonVisible.length; ++index) buttonVisible[index] = true;
        for (int id = 0; id < directionalActionFallbacks.length; ++id) {
            final int pointerId = id;
            directionalActionFallbacks[id] = new Runnable() {
                @Override public void run() {
                    if (disposed || editMode || !pointers.valid(pointerId)
                            || pointers.directionalActionStarted(pointerId)) {
                        return;
                    }
                    int role = pointers.role(pointerId);
                    if (!isDirectionalCombatRole(role)) return;
                    float x = pointers.lastX[pointerId];
                    float y = pointers.lastY[pointerId];
                    boolean hasDirection = directionGestureReady(pointerId, x, y);
                    beginDirectionalCombatAction(pointerId, x, y, hasDirection);
                }
            };
        }
        preferences = context.getSharedPreferences(GameLanguage.PREFERENCES_NAME, Context.MODE_PRIVATE);
        migrateDefaultBattleButtonLayout();
        sensorManager = (SensorManager)context.getSystemService(Context.SENSOR_SERVICE);
        rotationVector = sensorManager == null ? null : sensorManager.getDefaultSensor(Sensor.TYPE_GAME_ROTATION_VECTOR);
        setWillNotDraw(false);
        setFocusable(false);
        setClickable(true);
        setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_YES);
        setBackgroundColor(Color.argb(1, 0, 0, 0));
        setLayerType(View.LAYER_TYPE_SOFTWARE, null);
        setVisibility(View.VISIBLE);
        setAlpha(1.0f);
        setZ(dp(1000.0f));
        ReleaseLog.i("MBControls", "overlay constructed gyro=" + (rotationVector == null ? "none" : rotationVector.getName()));
        fill.setColor(Color.argb(72, 20, 20, 20));
        stroke.setStyle(Paint.Style.STROKE);
        stroke.setStrokeWidth(dp(2.0f));
        stroke.setColor(Color.argb(150, 235, 220, 180));
        text.setTextAlign(Paint.Align.CENTER);
        text.setColor(Color.argb(210, 255, 245, 215));
        text.setTextSize(sp(13.0f));
        calloutFill.setStyle(Paint.Style.FILL);
        calloutFill.setColor(Color.argb(220, 20, 20, 20));
        calloutStroke.setStyle(Paint.Style.STROKE);
        calloutStroke.setStrokeWidth(dp(1.5f));
        calloutStroke.setColor(Color.argb(235, 235, 220, 180));
    }

    private float dp(float value) {
        return value * getResources().getDisplayMetrics().density;
    }

    private float sp(float value) {
        return value * getResources().getDisplayMetrics().scaledDensity;
    }

    private float densityAwareScale() {
        return MBButtonLayoutPolicy.densityAwareScale(
                getWidth(), getHeight(), getResources().getDisplayMetrics().density);
    }

    private float controlDp(float value) {
        return dp(value) * densityAwareScale();
    }

    private void showTopToast(MBTouchStrings.Text message) {
        showTopToast(getContext(), preferences, message);
    }

    private static void showTopToast(Context context, SharedPreferences preferences,
                                     MBTouchStrings.Text message) {
        float density = context.getResources().getDisplayMetrics().density;
        TextView content = new TextView(context);
        content.setText(MBTouchStrings.get(storedGameLanguage(preferences), message));
        content.setTextColor(Color.rgb(255, 245, 215));
        content.setTextSize(16.0f);
        content.setGravity(Gravity.CENTER);
        content.setMaxWidth(Math.max(Math.round(280.0f * density),
                Math.round(context.getResources().getDisplayMetrics().widthPixels * 0.82f)));
        int horizontalPadding = Math.round(18.0f * density);
        int verticalPadding = Math.round(10.0f * density);
        content.setPadding(horizontalPadding, verticalPadding,
                horizontalPadding, verticalPadding);
        GradientDrawable background = new GradientDrawable();
        background.setColor(Color.argb(238, 20, 20, 20));
        background.setCornerRadius(12.0f * density);
        background.setStroke(Math.max(1, Math.round(1.5f * density)),
                Color.rgb(235, 220, 180));
        content.setBackground(background);

        Toast toast = new Toast(context);
        toast.setView(content);
        toast.setDuration(Toast.LENGTH_LONG);
        toast.setGravity(Gravity.TOP | Gravity.CENTER_HORIZONTAL, 0, Math.round(18.0f * density));
        toast.show();
    }

    private void showOneTimeHint(String preferenceKey, MBTouchStrings.Text message) {
        if (safeBooleanPreference(preferences, preferenceKey, false)) return;
        preferences.edit().putBoolean(preferenceKey, true).apply();
        showTopToast(message);
    }

    private void showOneTimeDismountHint() {
        if (dismountHintVisibleUntil != 0L
                || safeBooleanPreference(preferences, KEY_DISMOUNT_HINT_SHOWN, false)) {
            return;
        }
        preferences.edit().putBoolean(KEY_DISMOUNT_HINT_SHOWN, true).apply();
        dismountHintVisibleUntil = SystemClock.uptimeMillis() + DISMOUNT_HINT_VISIBLE_MILLIS;
        announceForAccessibility(MBTouchStrings.get(
                storedGameLanguage(preferences), MBTouchStrings.Text.DISMOUNT_HINT));
        invalidate();
    }

    private static native boolean nativePrepareGameData(AssetManager manager, String mainRoot,
            String patchRoot, String language, String selectedModuleRoot, String saveRoot,
            String masterServerMode, String masterServerHost);
    private static native boolean nativeSelectMasterServer(String mode, String host);
    private static native boolean nativeMasterServerApplied(String mode, String host);
    private static native void nativeStart();
    private static native void nativeShutdown();
    private static native boolean nativeReady();
    private static native boolean nativeGuestFrameResponsive();
    private static native int nativeConsumeInputToast();
    private static native boolean nativeMissionActive();
    private static native boolean nativeMissionSessionActive();
    private static native void nativeConfigureMenuTouch(boolean swipeNavigation,
            boolean tapConfirm, boolean shoulderButtons, float touchSlop, float density,
            int surfaceWidth, int surfaceHeight);
    private static native int[] nativeExchangeGuestSettings(
            boolean gyroEnabled, boolean gyroInvertY,
            boolean menuSwipe, boolean gyroRangedOnly, boolean leftHanded,
            boolean cameraMouse, boolean alwaysRun,
            boolean hdrEnabled, boolean dofEnabled, boolean autoExposure,
            boolean hapticFeedback,
            boolean menuAspect,
            boolean fxaaEnabled,
            boolean finalPoll,
            float moveSensitivity, float gyroSensitivity,
            float gyroDeadzone, float buttonScale, float buttonOpacity,
            float fpsLimit, float hdrBrightness, float shadowMode,
            float mapPanSensitivity, float reportTextScale);
    private static native void nativeSetOverlayEditMode(boolean active);
    private static native boolean nativeRangedWeaponHeld();
    private static native void nativeSetMoveAxes(float leftX, float leftY);
    private static native void nativeAddLookDelta(float yawDelta, float pitchDelta);
    private static native void nativeAddGyroLookDelta(float yawDelta, float pitchDelta);
    private static native void nativeBeginDirectionalAction(
            int action, float yawDelta, float pitchDelta);
    private static native void nativeSetAction(int action, boolean down);
    private static native void nativeReset();
    private static native int nativeActiveMenuWindowId();
    private static native boolean nativeMapTimeRunning();
    private static native boolean nativeToggleMapTime();
    private static native int nativeActivePresentationId();
    private static native int nativeTacticalModalState();
    private static native int nativeMissionButtonStates();
    private static native boolean nativeTapOrders(float x, float y);
    /** 0: gameplay, 1: interactive UI, 2: passive HUD; coordinates are surface pixels. */
    private static native int nativeTacticalUiHit(float x, float y);
    /** Single captured UI finger; actions use Android DOWN/UP/MOVE/CANCEL (0/1/2/3). */
    private static native boolean nativeTacticalUiTouch(
            int action, int pointerId, float x, float y, long eventTime);

    /** Called by MBNativeActivity before NativeActivity can enqueue a touch. */
    public static boolean shouldConsumeStalledTouchEvent(MotionEvent event) {
        if (event == null) return false;
        boolean responsive = true;
        if (nativeStarted) {
            try {
                responsive = nativeGuestFrameResponsive();
            } catch (RuntimeException | UnsatisfiedLinkError ignored) {
                // Keep input working if Java is temporarily paired with an
                // older bridge during development or recovery.
                responsive = true;
            }
        }
        return INPUT_STALL_POLICY.shouldConsume(
                event.getActionMasked(), nativeStarted, responsive);
    }

    private static boolean safeBooleanPreference(SharedPreferences preferences,
                                                 String key, boolean fallback) {
        try {
            return preferences.getBoolean(key, fallback);
        } catch (ClassCastException exception) {
            return fallback;
        }
    }

    private static float safeFloatPreference(SharedPreferences preferences,
                                             String key, float fallback) {
        try {
            float value = preferences.getFloat(key, fallback);
            return Float.isNaN(value) || Float.isInfinite(value) ? fallback : value;
        } catch (ClassCastException exception) {
            return fallback;
        }
    }

    private static GuestSettingsSync.Values readGuestSettingsPreferences(
            SharedPreferences preferences) {
        return GuestSettingsSync.boundedHostValues(
                safeBooleanPreference(preferences,
                        GuestSettingsSync.KEY_GYRO_ENABLED, false),
                safeBooleanPreference(preferences,
                        GuestSettingsSync.KEY_GYRO_INVERT_Y, false),
                safeBooleanPreference(preferences,
                        GuestSettingsSync.KEY_MENU_SWIPE, true),
                safeBooleanPreference(preferences,
                        GuestSettingsSync.KEY_GYRO_RANGED_ONLY, true),
                safeBooleanPreference(preferences,
                        GuestSettingsSync.KEY_LEFT_HANDED, false),
                false,
                safeBooleanPreference(preferences,
                        GuestSettingsSync.KEY_ALWAYS_RUN, true),
                safeBooleanPreference(preferences,
                        GuestSettingsSync.KEY_HDR_ENABLED, false),
                // DOF is hidden; ignore the old persisted value.
                // safeBooleanPreference(preferences,
                //         GuestSettingsSync.KEY_DOF_ENABLED, false),
                false,
                safeBooleanPreference(preferences,
                        GuestSettingsSync.KEY_AUTO_EXPOSURE, true),
                safeBooleanPreference(preferences,
                        GuestSettingsSync.KEY_HAPTIC_FEEDBACK, true),
                safeBooleanPreference(preferences,
                        GuestSettingsSync.KEY_MENU_ASPECT, true),
                safeBooleanPreference(preferences,
                        GuestSettingsSync.KEY_FXAA_ENABLED, false),
                safeFloatPreference(preferences,
                        GuestSettingsSync.KEY_MOVE_SENSITIVITY,
                        GuestSettingsSync.DEFAULT[0]),
                safeFloatPreference(preferences,
                        GuestSettingsSync.KEY_GYRO_SENSITIVITY,
                        GuestSettingsSync.DEFAULT[1]),
                safeFloatPreference(preferences,
                        GuestSettingsSync.KEY_GYRO_DEADZONE,
                        GuestSettingsSync.DEFAULT[2]),
                safeFloatPreference(preferences,
                        GuestSettingsSync.KEY_BUTTON_SCALE,
                        GuestSettingsSync.DEFAULT[3]),
                safeFloatPreference(preferences,
                        GuestSettingsSync.KEY_BUTTON_OPACITY,
                        GuestSettingsSync.DEFAULT[4]),
                safeFloatPreference(preferences,
                        GuestSettingsSync.KEY_FPS_LIMIT,
                        GuestSettingsSync.DEFAULT[5]),
                safeFloatPreference(preferences,
                        GuestSettingsSync.KEY_HDR_BRIGHTNESS,
                        GuestSettingsSync.DEFAULT[6]),
                safeFloatPreference(preferences,
                        GuestSettingsSync.KEY_SHADOW_MODE,
                        GuestSettingsSync.DEFAULT[7]),
                safeFloatPreference(preferences,
                        GuestSettingsSync.KEY_MAP_PAN_SENSITIVITY,
                        GuestSettingsSync.DEFAULT[8]),
                safeFloatPreference(preferences,
                        GuestSettingsSync.KEY_REPORT_TEXT_SCALE,
                        GuestSettingsSync.DEFAULT[9]));
    }

    private static void syncNativeGuestSettings(Activity activity, boolean finalPoll) {
        if (activity == null) return;
        SharedPreferences preferences = activity.getSharedPreferences(
                GameLanguage.PREFERENCES_NAME, Context.MODE_PRIVATE);
        GuestSettingsSync.Values host = readGuestSettingsPreferences(preferences);
        int[] packet = nativeExchangeGuestSettings(
                host.gyroEnabled, host.gyroInvertY,
                host.menuSwipe, host.gyroRangedOnly, host.leftHanded,
                host.cameraMouse, host.alwaysRun,
                host.hdrEnabled, host.dofEnabled, host.autoExposure,
                host.hapticFeedback, host.menuAspect, host.fxaaEnabled,
                finalPoll,
                host.moveSensitivity, host.gyroSensitivity,
                host.gyroDeadzone, host.buttonScale, host.buttonOpacity,
                host.fpsLimit, host.hdrBrightness, host.shadowMode,
                host.mapPanSensitivity, host.reportTextScale);
        GuestSettingsSync.Values guest = GuestSettingsSync.decodeGuestPacket(packet);
        if (guest == null) return;

        SharedPreferences.Editor editor = preferences.edit()
                .putBoolean(GuestSettingsSync.KEY_GYRO_ENABLED, guest.gyroEnabled)
                .putBoolean(GuestSettingsSync.KEY_GYRO_INVERT_Y, guest.gyroInvertY)
                .putBoolean(GuestSettingsSync.KEY_MENU_SWIPE, guest.menuSwipe)
                .putBoolean(GuestSettingsSync.KEY_GYRO_RANGED_ONLY, guest.gyroRangedOnly)
                .putBoolean(GuestSettingsSync.KEY_LEFT_HANDED, guest.leftHanded)
                .putBoolean(GuestSettingsSync.KEY_CAMERA_MOUSE, guest.cameraMouse)
                .putBoolean(GuestSettingsSync.KEY_ALWAYS_RUN, guest.alwaysRun)
                .putBoolean(GuestSettingsSync.KEY_HDR_ENABLED, guest.hdrEnabled)
                .remove(GuestSettingsSync.KEY_DOF_ENABLED)
                .putBoolean(GuestSettingsSync.KEY_AUTO_EXPOSURE, guest.autoExposure)
                .putBoolean(GuestSettingsSync.KEY_HAPTIC_FEEDBACK, guest.hapticFeedback)
                .putBoolean(GuestSettingsSync.KEY_MENU_ASPECT, guest.menuAspect)
                .putBoolean(GuestSettingsSync.KEY_FXAA_ENABLED, guest.fxaaEnabled)
                .putFloat(GuestSettingsSync.KEY_MOVE_SENSITIVITY,
                        guest.moveSensitivity)
                .putFloat(GuestSettingsSync.KEY_GYRO_SENSITIVITY,
                        guest.gyroSensitivity)
                .putFloat(GuestSettingsSync.KEY_GYRO_DEADZONE,
                        guest.gyroDeadzone)
                .putFloat(GuestSettingsSync.KEY_BUTTON_SCALE, guest.buttonScale)
                .putFloat(GuestSettingsSync.KEY_BUTTON_OPACITY, guest.buttonOpacity)
                .putFloat(GuestSettingsSync.KEY_FPS_LIMIT, guest.fpsLimit)
                .putFloat(GuestSettingsSync.KEY_HDR_BRIGHTNESS, guest.hdrBrightness)
                .putFloat(GuestSettingsSync.KEY_SHADOW_MODE, guest.shadowMode)
                .putFloat(GuestSettingsSync.KEY_MAP_PAN_SENSITIVITY,
                        guest.mapPanSensitivity)
                .putFloat(GuestSettingsSync.KEY_REPORT_TEXT_SCALE,
                        guest.reportTextScale);
        editor.apply();

        MBTouchOverlay overlay = activeOverlay;
        if (overlay != null && !overlay.disposed) {
            overlay.buttonLayoutDirty = true;
            overlay.resetGyroTracking();
            if (guest.gyroEnabled) overlay.registerSensor();
            else overlay.unregisterSensor();
            overlay.invalidate();
        }
    }

    private static void syncNativeMenuTouchConfiguration(Activity activity, View surface) {
        if (activity == null) return;
        SharedPreferences preferences = activity.getSharedPreferences(
                GameLanguage.PREFERENCES_NAME, Context.MODE_PRIVATE);
        float density = activity.getResources().getDisplayMetrics().density;
        float touchSlop = ViewConfiguration.get(activity).getScaledTouchSlop();
        nativeConfigureMenuTouch(
                preferences.getBoolean(GuestSettingsSync.KEY_MENU_SWIPE, true),
                preferences.getBoolean("mb_menu_tap_confirm", false),
                preferences.getBoolean(GuestSettingsSync.KEY_GYRO_RANGED_ONLY, true),
                touchSlop, density,
                surface == null ? 0 : surface.getWidth(),
                surface == null ? 0 : surface.getHeight());
    }

    private void submitMovement() {
        nativeSetMoveAxes(moveX, moveY);
    }

    private void submitLookDelta(float yawDelta, float pitchDelta) {
        float safeYaw = InputMath.finiteClamped(yawDelta, MAX_LOOK_SAMPLE_UNITS);
        float safePitch = InputMath.finiteClamped(pitchDelta, MAX_LOOK_SAMPLE_UNITS);
        if (safeYaw != 0.0f || safePitch != 0.0f) {
            nativeAddLookDelta(safeYaw, safePitch);
        }
    }

    /** Gyro deltas take the dedicated channel: the bridge cancels the game's
     * vertical look multiplier so physical rotation maps 1:1 on both axes. */
    private void submitGyroLookDelta(float yawDelta, float pitchDelta) {
        float safeYaw = InputMath.finiteClamped(yawDelta, MAX_LOOK_SAMPLE_UNITS);
        float safePitch = InputMath.finiteClamped(pitchDelta, MAX_LOOK_SAMPLE_UNITS);
        if (safeYaw != 0.0f || safePitch != 0.0f) {
            nativeAddGyroLookDelta(safeYaw, safePitch);
        }
    }

    private float pref(String key, float fallback) {
        final float value;
        try {
            value = preferences.getFloat(key, fallback);
        } catch (ClassCastException exception) {
            ReleaseLog.w("MBControls", "invalid float preference " + key, exception);
            return fallback;
        }
        return MBButtonLayoutPolicy.finite(value) ? value : fallback;
    }

    private static String buttonPreferencePrefix(int role) {
        if (role == ROLE_ATTACK) return "mb_attack";
        if (role == ROLE_BLOCK) return "mb_block";
        if (role == ROLE_KICK) return "mb_kick";
        if (role == ROLE_JUMP) return "mb_jump";
        if (role == ROLE_BACK) return "mb_back";
        if (role == ROLE_PAUSE) return "mb_pause";
        if (role == ROLE_ACTIONS_MENU) return "mb_actions_menu";
        if (role == ROLE_CHAT) return "mb_chat";
        return null;
    }

    private static float buttonDefaultX(int role) {
        if (role == ROLE_ATTACK) return 0.92f;
        if (role == ROLE_BLOCK) return 0.80f;
        if (role == ROLE_KICK) return 0.93f;
        if (role == ROLE_JUMP) return 0.80f;
        if (role == ROLE_BACK) return 0.06f;
        if (role == ROLE_PAUSE) return 0.94f;
        if (role == ROLE_ACTIONS_MENU) return 0.13f;
        if (role == ROLE_CHAT) return 0.06f;
        return 0.5f;
    }

    private static float buttonDefaultY(int role) {
        if (role == ROLE_ATTACK) return 0.78f;
        if (role == ROLE_BLOCK) return 0.82f;
        if (role == ROLE_KICK) return 0.60f;
        if (role == ROLE_JUMP) return 0.63f;
        if (role == ROLE_BACK) return 0.10f;
        if (role == ROLE_PAUSE) return 0.10f;
        if (role == ROLE_ACTIONS_MENU) return 0.32f;
        if (role == ROLE_CHAT) return 0.23f;
        return 0.5f;
    }

    private void migrateDefaultBattleButtonLayout() {
        int version;
        try {
            version = preferences.getInt("mb_battle_layout_version", 0);
        } catch (ClassCastException ignored) {
            version = 0;
        }
        if (version >= 2) return;

        SharedPreferences.Editor editor = preferences.edit();
        migrateDefaultButtonPosition(editor, ROLE_ATTACK, 0.88f, 0.75f);
        migrateDefaultButtonPosition(editor, ROLE_BLOCK, 0.72f, 0.78f);
        migrateDefaultButtonPosition(editor, ROLE_KICK, 0.76f, 0.56f);
        migrateDefaultButtonPosition(editor, ROLE_JUMP, 0.94f, 0.91f);
        migrateDefaultButtonPosition(editor, ROLE_BACK, 0.53f, 0.12f);
        migrateDefaultButtonPosition(editor, ROLE_PAUSE, 0.95f, 0.10f);
        editor.putInt("mb_battle_layout_version", 2).apply();
    }

    private void migrateDefaultButtonPosition(SharedPreferences.Editor editor, int role,
                                             float oldX, float oldY) {
        String prefix = buttonPreferencePrefix(role);
        if (prefix == null) return;
        String xKey = prefix + "_x";
        String yKey = prefix + "_y";
        if (preferences.contains(xKey) && Math.abs(safeFloatPreference(
                preferences, xKey, oldX) - oldX) < 0.001f) {
            editor.putFloat(xKey, buttonDefaultX(role));
        }
        if (preferences.contains(yKey) && Math.abs(safeFloatPreference(
                preferences, yKey, oldY) - oldY) < 0.001f) {
            editor.putFloat(yKey, buttonDefaultY(role));
        }
    }

    private float buttonScale() {
        return MBButtonLayoutPolicy.requestedScale(pref(
                GuestSettingsSync.KEY_BUTTON_SCALE, GuestSettingsSync.DEFAULT[3]));
    }

    private boolean leftHanded() {
        return preferences.getBoolean(GuestSettingsSync.KEY_LEFT_HANDED, false);
    }

    private float buttonOpacity() {
        return pref(
                GuestSettingsSync.KEY_BUTTON_OPACITY, GuestSettingsSync.DEFAULT[4]);
    }

    /** Mirrors one layout coordinate for the left-handed control layout. */
    private float mirrorX(float x) {
        return getWidth() - x;
    }

    private void resolveButtonLayout() {
        View insetSource = activeDecor != null ? activeDecor : this;
        SafeInsets insets = safeInsets(insetSource);
        float requestedScale = buttonScale();
        float density = getResources().getDisplayMetrics().density;
        float largeScreenScale = densityAwareScale();
        if (!buttonLayoutDirty
                && cachedLayoutWidth == getWidth()
                && cachedLayoutHeight == getHeight()
                && cachedInsetLeft == insets.left
                && cachedInsetTop == insets.top
                && cachedInsetRight == insets.right
                && cachedInsetBottom == insets.bottom
                && Float.floatToIntBits(cachedRequestedScale)
                        == Float.floatToIntBits(requestedScale)
                && Float.floatToIntBits(cachedDensity) == Float.floatToIntBits(density)
                && Float.floatToIntBits(cachedDensityAwareScale)
                        == Float.floatToIntBits(largeScreenScale)) {
            return;
        }
        for (int index = 0; index < BUTTON_ROLES.length; ++index) {
            int role = BUTTON_ROLES[index];
            String prefix = buttonPreferencePrefix(role);
            buttonRawX[index] = getWidth() * pref(
                    prefix + "_x", buttonDefaultX(role));
            buttonRawY[index] = getHeight() * pref(
                    prefix + "_y", buttonDefaultY(role));
            buttonRawX[index] = MBButtonLayoutPolicy.compactTowardNearestEdge(
                    buttonRawX[index], getWidth(), largeScreenScale);
            buttonRawY[index] = MBButtonLayoutPolicy.compactTowardNearestEdge(
                    buttonRawY[index], getHeight(), largeScreenScale);
        }

        if (leftHanded()) {
            // Left-handed mode: buttons and the move/look halves swap sides
            // together. Mirroring happens after preference decode and before
            // packing, so stored positions stay in right-handed coordinates.
            for (int index = 0; index < BUTTON_ROLES.length; ++index) {
                buttonRawX[index] = mirrorX(buttonRawX[index]);
            }
        }

        for (int index = 0; index < BUTTON_ROLES.length; ++index) {
            buttonBaseRadius[index] = dp(BUTTON_RADIUS_DP[index]) * largeScreenScale;
        }
        resolvedButtonScale = MBButtonLayoutPolicy.resolveLayout(
                getWidth(), getHeight(), insets.left, insets.top,
                insets.right, insets.bottom, requestedScale,
                dp(2.0f) * largeScreenScale, dp(32.0f) * largeScreenScale,
                buttonRawX, buttonRawY, buttonBaseRadius,
                buttonCenterX, buttonCenterY, buttonRadius);
        for (int index = 0; index < BUTTON_ROLES.length; ++index) {
            // A 64dp minimum keeps relocated actions forgiving on dense phone displays.
            buttonHitRadius[index] = Math.max(
                    buttonRadius[index], dp(32.0f) * largeScreenScale);
        }
        cachedLayoutWidth = getWidth();
        cachedLayoutHeight = getHeight();
        cachedInsetLeft = insets.left;
        cachedInsetTop = insets.top;
        cachedInsetRight = insets.right;
        cachedInsetBottom = insets.bottom;
        cachedRequestedScale = requestedScale;
        cachedDensity = density;
        cachedDensityAwareScale = largeScreenScale;
        buttonLayoutDirty = false;
        ReleaseLog.i("MBControls", "button layout " + getWidth() + "x" + getHeight()
                + " density=" + density + " adaptiveScale=" + largeScreenScale
                + " requestedScale=" + requestedScale + " resolvedScale="
                + resolvedButtonScale + " attackRadiusPx=" + buttonRadius[0]);
    }

    /** Returns true when the report actually changed a button's visibility. */
    private boolean refreshButtonVisibility() {
        int previousStates = missionButtonStates;
        missionButtonStates = 0;
        for (int index = 0; index < buttonVisible.length; ++index) {
            buttonVisible[index] = true;
        }
        buttonVisible[buttonIndexForRole(ROLE_CHAT)] = false;
        if (disposed || !nativeReady() || !nativeMissionActive()) {
            return previousStates != missionButtonStates;
        }
        try {
            missionButtonStates = nativeMissionButtonStates();
        } catch (RuntimeException | UnsatisfiedLinkError ignored) {
            missionButtonStates = 0;
        }
        buttonVisible[buttonIndexForRole(ROLE_CHAT)] =
                missionStateHas(MISSION_STATE_MULTIPLAYER);
        if (missionStateHas(MISSION_STATE_PLAYER_MOUNTED)) {
            showOneTimeDismountHint();
        }
        return previousStates != missionButtonStates;
    }

    private int buttonIndexForRole(int role) {
        for (int index = 0; index < BUTTON_ROLES.length; ++index) {
            if (BUTTON_ROLES[index] == role) return index;
        }
        return -1;
    }

    private int hitButtonIndex(float x, float y) {
        resolveButtonLayout();
        for (int index = 0; index < buttonHittable.length; ++index) {
            buttonHittable[index] = isButtonShown(index);
        }
        return MBButtonLayoutPolicy.hitIndex(
                x, y, buttonCenterX, buttonCenterY, buttonHitRadius, buttonHittable);
    }

    private int chooseRole(float x, float y) {
        int index = hitButtonIndex(x, y);
        if (index >= 0) return BUTTON_ROLES[index];
        final boolean leftHalf = x < getWidth() * 0.5f;
        if (leftHanded()) return leftHalf ? ROLE_LOOK : ROLE_MOVE;
        return leftHalf ? ROLE_MOVE : ROLE_LOOK;
    }

    private boolean missionStateHas(int bit) {
        return (missionButtonStates & bit) != 0;
    }

    private boolean radialActionAvailable(int role) {
        if (role == ROLE_DROP) return missionStateHas(MISSION_STATE_DROP_ITEM);
        if (role == ROLE_ORDERS) return missionStateHas(MISSION_STATE_ISSUE_ORDERS);
        if (role == ROLE_CYCLE_WEAPON) return missionStateHas(MISSION_STATE_CYCLE_WEAPON);
        if (role == ROLE_CYCLE_SHIELD) return missionStateHas(MISSION_STATE_CYCLE_SHIELD);
        if (role == ROLE_SHEATH || role == ROLE_WEAPON_MODE) {
            return missionStateHas(MISSION_STATE_SHEATH_WEAPON);
        }
        return role == ROLE_ZOOM || role == ROLE_VIEW;
    }

    private int radialRoleForIndex(int index) {
        return index >= 0 && index < RADIAL_ACTION_ROLES.length
                ? RADIAL_ACTION_ROLES[index] : ROLE_NONE;
    }

    private int radialMenuButtonIndex() {
        return buttonIndexForRole(ROLE_ACTIONS_MENU);
    }

    private float radialMenuRadius() {
        float surfaceLimit = Math.min(getWidth(), getHeight()) * 0.24f;
        return Math.max(controlDp(48.0f), Math.min(
                controlDp(58.0f) * resolvedButtonScale, surfaceLimit));
    }

    private float radialButtonRadius() {
        return Math.max(controlDp(16.0f), controlDp(18.0f) * resolvedButtonScale);
    }

    private void resolveRadialMenuCenter() {
        int menuIndex = radialMenuButtonIndex();
        if (menuIndex < 0 || getWidth() <= 0 || getHeight() <= 0) {
            radialMenuCenter[0] = getWidth() * 0.5f;
            radialMenuCenter[1] = getHeight() * 0.5f;
            return;
        }
        float anchorX = MBButtonLayoutPolicy.finite(radialMenuAnchorX)
                ? radialMenuAnchorX : buttonCenterX[menuIndex];
        float anchorY = MBButtonLayoutPolicy.finite(radialMenuAnchorY)
                ? radialMenuAnchorY : buttonCenterY[menuIndex];
        float extent = radialMenuRadius() * 1.42f + radialButtonRadius() + controlDp(4.0f);
        MBBattleTouchPolicy.clampRadialMenuCenter(anchorX, anchorY,
                getWidth(), getHeight(), extent, radialMenuCenter);
    }

    private int radialActionIndexAt(float x, float y) {
        if (radialMenuButtonIndex() < 0) return -1;
        resolveRadialMenuCenter();
        float radius = radialMenuRadius();
        return MBBattleTouchPolicy.radialActionIndex(
                x, y, radialMenuCenter[0], radialMenuCenter[1],
                radius * 0.52f, radius * 1.42f);
    }

    private void updateRadialMenuTarget(float x, float y) {
        int previousRole = radialMenuTargetRole;
        int role = radialRoleForIndex(radialActionIndexAt(x, y));
        radialMenuTargetRole = radialActionAvailable(role) ? role : ROLE_NONE;
        if (radialMenuTargetRole != ROLE_NONE && radialMenuTargetRole != previousRole) {
            performTouchHaptic(HapticFeedbackConstants.CLOCK_TICK);
        }
    }

    private void activateMissionActionBriefly(int role) {
        if (role == ROLE_PAUSE) {
            pauseBriefly();
            return;
        }
        if (actionForRole(role) == 0) return;
        pressRole(role, true);
        postDelayed(new Runnable() {
            @Override public void run() {
                if (!disposed) pressRole(role, false);
            }
        }, role == ROLE_USE
                ? MBBattleTouchPolicy.contextActionHoldMillis()
                : SYSTEM_BACK_HOLD_MILLIS);
    }

    private void dismissRadialMenu() {
        radialPointerId = -1;
        radialMenuOpen = false;
        radialMenuTargetRole = ROLE_NONE;
        radialMenuAnchorX = Float.NaN;
        radialMenuAnchorY = Float.NaN;
    }

    private boolean showRadialActionsDialog() {
        refreshButtonVisibility();
        final int availableCount = countAvailableRadialActions();
        if (availableCount == 0) return false;
        final int[] roles = new int[availableCount];
        final CharSequence[] labels = new CharSequence[availableCount];
        String languageCode = storedGameLanguage(preferences);
        int next = 0;
        for (int role : RADIAL_ACTION_ROLES) {
            if (!radialActionAvailable(role)) continue;
            roles[next] = role;
            labels[next] = buttonLabel(languageCode, role);
            ++next;
        }
        new AlertDialog.Builder(getContext())
                .setItems(labels, new DialogInterface.OnClickListener() {
                    @Override public void onClick(DialogInterface dialog, int which) {
                        if (which >= 0 && which < roles.length && !disposed) {
                            activateMissionActionBriefly(roles[which]);
                        }
                    }
                })
                .show();
        return true;
    }

    private int countAvailableRadialActions() {
        int count = 0;
        for (int role : RADIAL_ACTION_ROLES) {
            if (radialActionAvailable(role)) ++count;
        }
        return count;
    }

    private int actionForRole(int role) {
        if (role == ROLE_ATTACK) return ACTION_ATTACK;
        if (role == ROLE_BLOCK) return ACTION_BLOCK;
        if (role == ROLE_USE) return ACTION_USE;
        if (role == ROLE_PAUSE) return ACTION_PAUSE;
        if (role == ROLE_KICK) return ACTION_KICK;
        if (role == ROLE_JUMP) return ACTION_JUMP;
        if (role == ROLE_DROP) return ACTION_DROP;
        if (role == ROLE_CYCLE_WEAPON) return ACTION_RB;
        if (role == ROLE_CYCLE_SHIELD) return ACTION_LB;
        if (role == ROLE_ZOOM) return ACTION_ZOOM;
        if (role == ROLE_VIEW) return ACTION_VIEW;
        if (role == ROLE_BACK) return ACTION_BACK;
        if (role == ROLE_SHEATH) return ACTION_SHEATH;
        if (role == ROLE_ORDERS) return ACTION_OPEN_ORDERS;
        if (role == ROLE_WEAPON_MODE) return ACTION_DPAD_DOWN;
        return 0;
    }

    private static boolean isDirectionalCombatRole(int role) {
        return role == ROLE_ATTACK || role == ROLE_BLOCK;
    }

    private boolean performTouchHaptic(int feedbackConstant) {
        if (!safeBooleanPreference(
                preferences, GuestSettingsSync.KEY_HAPTIC_FEEDBACK, true)) {
            return false;
        }
        return performHapticFeedback(feedbackConstant);
    }

    private boolean directionGestureReady(int id, float x, float y) {
        if (!pointers.valid(id)) return false;
        return InputMath.directionGestureReady(
                x - pointers.downX[id], y - pointers.downY[id],
                getResources().getDisplayMetrics().density);
    }

    /**
     * Atomically queues the right-stick direction and raises the matching
     * trigger. Warband samples attack/block direction on that rising edge, so
     * sending an ordinary trigger-down on ACTION_DOWN permanently selects the
     * default direction before a drag can arrive.
     */
    private boolean beginDirectionalCombatAction(int id, float x, float y,
                                                  boolean useDirection) {
        if (!pointers.valid(id) || pointers.directionalActionStarted(id)) return false;
        int role = pointers.role(id);
        if (!isDirectionalCombatRole(role)) return false;

        pointers.markDirectionalActionStarted(id, SystemClock.uptimeMillis());
        int action = actionForRole(role);
        int previousCount = heldRoleCounts[role]++;
        if (previousCount != 0) return true;

        if (role == ROLE_ATTACK) attackHeld = true;
        else blockHeld = true;
        resetGyroTracking();

        float yaw = 0.0f;
        float pitch = 0.0f;
        if (useDirection) {
            float density = getResources().getDisplayMetrics().density;
            yaw = InputMath.touchLookUnits(x - pointers.downX[id], density, 1.0f);
            pitch = InputMath.touchLookUnits(y - pointers.downY[id], density, 1.0f);
        }
        nativeBeginDirectionalAction(action, yaw, pitch);
        performTouchHaptic(HapticFeedbackConstants.KEYBOARD_TAP);
        ReleaseLog.i("MBControls", "directional action started role=" + role
                + " vector=" + yaw + "," + pitch
                + " source=" + (useDirection ? "drag" : "hold/tap"));
        return true;
    }

    private void releaseDirectionalCombatAction(final int role, long startedAt) {
        long elapsed = Math.max(0L, SystemClock.uptimeMillis() - startedAt);
        long remaining = Math.max(0L, MIN_ACTION_PULSE_MILLIS - elapsed);
        if (remaining == 0L) {
            pressRole(role, false);
            return;
        }
        postDelayed(new Runnable() {
            @Override public void run() {
                if (!disposed) pressRole(role, false);
            }
        }, remaining);
    }

    private void pressRole(int role, boolean down) {
        int action = actionForRole(role);
        if (action == 0 || role < 0 || role >= heldRoleCounts.length) return;
        if (down) {
            int previousCount = heldRoleCounts[role]++;
            if (previousCount != 0) return;
            if (role == ROLE_ATTACK) {
                attackHeld = true;
                resetGyroTracking();
            } else if (role == ROLE_BLOCK) {
                blockHeld = true;
                resetGyroTracking();
            }
            nativeSetAction(action, true);
            return;
        }
        if (heldRoleCounts[role] <= 0) return;
        --heldRoleCounts[role];
        if (heldRoleCounts[role] != 0) return;
        if (role == ROLE_ATTACK) {
            attackHeld = false;
            resetGyroTracking();
        } else if (role == ROLE_BLOCK) {
            blockHeld = false;
            resetGyroTracking();
        }
        nativeSetAction(action, false);
    }

    private void pauseBriefly() {
        removeCallbacks(releasePause);
        nativeSetAction(ACTION_PAUSE, true);
        postDelayed(releasePause, 70L);
    }

    private void resetGyroTracking() {
        gyroTracking = false;
        lastGyroTimestampNs = 0L;
    }

    private void startPointer(MotionEvent event, int index) {
        int id = event.getPointerId(index);
        if (!pointers.valid(id)) return;
        float x = event.getX(index);
        float y = event.getY(index);
        if (!editMode) {
            int uiKind = nativeTacticalUiHit(x, y);
            if (uiKind != 0) {
                pointers.begin(id, ROLE_NATIVE_UI, x, y, event.getEventTime());
                nativeUiKinds[id] = uiKind == 1 && nativeTacticalUiTouch(
                        MotionEvent.ACTION_DOWN, id, x, y, event.getEventTime()) ? 1 : 2;
                return;
            }
        }
        int buttonIndex;
        boolean promptTapCandidate = false;
        if (!editMode) {
            refreshButtonVisibility();
            buttonIndex = hitButtonIndex(x, y);
            promptTapCandidate = MBBattleTouchPolicy.contextPromptOwnsTap(
                    buttonIndex >= 0, x, y, getWidth(), getHeight());
        } else {
            buttonIndex = hitButtonIndex(x, y);
        }
        int role = buttonIndex >= 0 ? BUTTON_ROLES[buttonIndex]
                : (promptTapCandidate ? ROLE_LOOK : chooseRole(x, y));
        if (role == ROLE_ACTIONS_MENU && radialPointerId >= 0) role = ROLE_LOOK;
        pointers.begin(id, role, x, y, SystemClock.uptimeMillis());
        if (promptTapCandidate) promptTapPointerId = id;
        if (!editMode && role == ROLE_ACTIONS_MENU) {
            radialPointerId = id;
            radialMenuAnchorX = x;
            radialMenuAnchorY = y;
            radialMenuOpen = true;
            radialMenuTargetRole = ROLE_NONE;
            resolveRadialMenuCenter();
            ReleaseLog.i("MBControls", "radial menu opened anchor=" + x + "," + y
                    + " center=" + radialMenuCenter[0] + "," + radialMenuCenter[1]);
        } else if (!editMode && role == ROLE_PAUSE) {
            pausePointerId = id;
            pauseLongPressTriggered = false;
            removeCallbacks(enterButtonEditMode);
            postDelayed(enterButtonEditMode, PAUSE_EDIT_HOLD_MILLIS);
        } else if (!editMode && isDirectionalCombatRole(role)) {
            removeCallbacks(directionalActionFallbacks[id]);
            postDelayed(directionalActionFallbacks[id], DIRECTIONAL_ACTION_HOLD_MILLIS);
        } else if (!editMode && role != ROLE_PAUSE && role != ROLE_CHAT) {
            pressRole(role, true);
        }
    }

    /** Long-pressing Pause enters layout editing; tapping Pause while editing exits it. */
    private void setEditMode(boolean active) {
        if (editMode == active) return;
        dismissRadialMenu();
        promptTapPointerId = -1;
        editMode = active;
        try {
            nativeSetOverlayEditMode(active);
        } catch (RuntimeException | UnsatisfiedLinkError ignored) {
            // Bridge without this entry point keeps the old behaviour.
        }
        buttonLayoutDirty = true;
        invalidate();
    }

    private void updateButtonPosition(int role, float x, float y) {
        String prefix = buttonPreferencePrefix(role);
        if (prefix == null) return;
        if (getWidth() <= 0 || getHeight() <= 0) return;
        resolveButtonLayout();
        int index = buttonIndexForRole(role);
        if (index < 0) return;
        SafeInsets insets = safeInsets(activeDecor != null ? activeDecor : this);
        float radius = buttonHitRadius[index];
        float canonicalDisplayX = leftHanded() ? mirrorX(x) : x;
        float safeX = MBButtonLayoutPolicy.clampCenter(canonicalDisplayX,
                insets.left + radius, getWidth() - insets.right - radius);
        float safeY = MBButtonLayoutPolicy.clampCenter(y,
                insets.top + radius, getHeight() - insets.bottom - radius);
        float largeScreenScale = densityAwareScale();
        float storedX = MBButtonLayoutPolicy.expandFromNearestEdge(
                safeX, getWidth(), largeScreenScale);
        float storedY = MBButtonLayoutPolicy.expandFromNearestEdge(
                safeY, getHeight(), largeScreenScale);
        preferences.edit()
                .putFloat(prefix + "_x", storedX / getWidth())
                .putFloat(prefix + "_y", storedY / getHeight())
                .apply();
        buttonLayoutDirty = true;
        invalidate();
    }

    private void movePointerSample(int id, float x, float y, boolean lookOnly,
                                   long eventTime) {
        if (!pointers.valid(id)) return;
        float previousX = pointers.lastX[id];
        float previousY = pointers.lastY[id];
        int role = pointers.role(id);
        if (promptTapPointerId == id && movedBeyondTouchSlop(id, x, y)) {
            promptTapPointerId = -1;
        }
        pointers.update(id, x, y);
        if (role == ROLE_NATIVE_UI) {
            if (nativeUiKinds[id] == 1) {
                nativeTacticalUiTouch(MotionEvent.ACTION_MOVE, id, x, y, eventTime);
            }
            return;
        }
        if (lookOnly && !pointers.ownsLook(id)) return;
        if (role == ROLE_ACTIONS_MENU && id == radialPointerId) {
            if (radialMenuOpen && movedBeyondTouchSlop(id, x, y)) {
                updateRadialMenuTarget(x, y);
            } else {
                radialMenuTargetRole = ROLE_NONE;
            }
            return;
        }
        if (role == ROLE_PAUSE && id == pausePointerId
                && movedBeyondTouchSlop(id, x, y)) {
            removeCallbacks(enterButtonEditMode);
        }
        if (editMode && role >= ROLE_ATTACK) {
            if (!lookOnly) updateButtonPosition(role, x, y);
            return;
        }
        if (!lookOnly && role == ROLE_MOVE && pointers.ownsMove(id)) {
            updateMovement(id);
        } else if (pointers.ownsLook(id)
                && (role == ROLE_LOOK || role == ROLE_ATTACK || role == ROLE_BLOCK)) {
            // Touch look feeds raw pixel units; the native mouse-sensitivity
            // option (General settings) is the single look-speed control.
            float density = getResources().getDisplayMetrics().density;
            if (isDirectionalCombatRole(role)
                    && !pointers.directionalActionStarted(id)) {
                if (directionGestureReady(id, x, y)) {
                    removeCallbacks(directionalActionFallbacks[id]);
                    beginDirectionalCombatAction(id, x, y, true);
                }
                // Do not let pre-trigger samples get consumed by an earlier
                // frame. The cumulative vector is queued with the trigger edge.
                return;
            }
            submitLookDelta(
                    InputMath.touchLookUnits(x - previousX, density, 1.0f),
                    InputMath.touchLookUnits(y - previousY, density, 1.0f));
        }
    }

    private void updateMovement(int id) {
        if (!pointers.valid(id) || !pointers.ownsMove(id)) {
            moveX = 0.0f;
            moveY = 0.0f;
            submitMovement();
            return;
        }
        float radius = Math.max(dp(72.0f), Math.min(getWidth(), getHeight()) * 0.16f);
        float movementSensitivity = pref(
                GuestSettingsSync.KEY_MOVE_SENSITIVITY, GuestSettingsSync.DEFAULT[0]);
        float dx = (pointers.lastX[id] - pointers.downX[id]) / radius * movementSensitivity;
        float dy = (pointers.lastY[id] - pointers.downY[id]) / radius * movementSensitivity;
        InputMath.movementAxes(dx, dy, safeBooleanPreference(preferences,
                GuestSettingsSync.KEY_ALWAYS_RUN, true), movementAxes);
        moveX = movementAxes[0];
        moveY = movementAxes[1];
        submitMovement();
    }

    private void movePointers(MotionEvent event) {
        // Android may batch several samples into one MotionEvent. Feed every historical sample
        // to the relative-look accumulator so distance is never lost between guest frames.
        int lookId = pointers.lookOwner();
        int lookIndex = lookId < 0 ? -1 : event.findPointerIndex(lookId);
        if (lookIndex >= 0) {
            for (int history = 0; history < event.getHistorySize(); ++history) {
                movePointerSample(lookId, event.getHistoricalX(lookIndex, history),
                        event.getHistoricalY(lookIndex, history), true,
                        event.getHistoricalEventTime(history));
            }
        }
        for (int index = 0; index < event.getPointerCount(); ++index) {
            movePointerSample(event.getPointerId(index), event.getX(index), event.getY(index),
                    false, event.getEventTime());
        }
    }

    private boolean movedBeyondTouchSlop(int id, float x, float y) {
        float touchSlop = ViewConfiguration.get(getContext()).getScaledTouchSlop();
        float deltaX = x - pointers.downX[id];
        float deltaY = y - pointers.downY[id];
        return deltaX * deltaX + deltaY * deltaY > touchSlop * touchSlop;
    }

    private void finishPointer(MotionEvent event, int index, boolean cancelled) {
        int id = event.getPointerId(index);
        if (!pointers.valid(id)) return;
        int role = pointers.role(id);
        boolean ownedMove = pointers.ownsMove(id);
        float x = event.getX(index);
        float y = event.getY(index);
        pointers.update(id, x, y);
        if (role == ROLE_NATIVE_UI) {
            if (nativeUiKinds[id] == 1) {
                nativeTacticalUiTouch(cancelled ? MotionEvent.ACTION_CANCEL : MotionEvent.ACTION_UP,
                        id, x, y, event.getEventTime());
            }
            nativeUiKinds[id] = 0;
            pointers.end(id);
            return;
        }
        if (isDirectionalCombatRole(role)) {
            removeCallbacks(directionalActionFallbacks[id]);
        }
        if (id == pausePointerId) removeCallbacks(enterButtonEditMode);
        if (editMode) {
            if (id == pausePointerId && pauseLongPressTriggered) {
                pausePointerId = -1;
                pauseLongPressTriggered = false;
                pointers.end(id);
                if (ownedMove) updateMovement(pointers.moveOwner());
                invalidate();
                return;
            }
            if (promptTapPointerId == id) promptTapPointerId = -1;
            boolean stationaryExitButton = (role == ROLE_PAUSE || role == ROLE_BACK)
                    && !movedBeyondTouchSlop(id, x, y);
            if (!cancelled && stationaryExitButton) {
                setEditMode(false);
                ReleaseLog.i("MBControls", "button layout edit exited via overlay "
                        + (role == ROLE_PAUSE ? "Pause" : "Back"));
            } else if (!cancelled) {
                updateButtonPosition(role, x, y);
            }
            pointers.end(id);
            if (ownedMove) updateMovement(pointers.moveOwner());
            invalidate();
            return;
        }
        if (role == ROLE_ACTIONS_MENU && id == radialPointerId) {
            refreshButtonVisibility();
            boolean moved = movedBeyondTouchSlop(id, x, y);
            int radialIndex = radialMenuOpen && moved && !cancelled
                    ? radialActionIndexAt(x, y) : -1;
            int selectedRole = radialRoleForIndex(radialIndex);
            if (selectedRole != ROLE_NONE && radialActionAvailable(selectedRole)) {
                ReleaseLog.i("MBControls", "radial menu selected role=" + selectedRole);
                activateMissionActionBriefly(selectedRole);
            } else if (!cancelled) {
                ReleaseLog.i("MBControls", "radial menu quick tap/no selection; no action");
                showOneTimeHint(KEY_RADIAL_HINT_SHOWN, MBTouchStrings.Text.RADIAL_MENU_HINT);
            }
            dismissRadialMenu();
            pointers.end(id);
            if (ownedMove) updateMovement(pointers.moveOwner());
            invalidate();
            return;
        }
        // The tactical orders panel belongs to the still-active mission
        // window, so the full-screen overlay remains installed above it.  A
        // stationary tap on one of its native buttons is forwarded to that
        // exact guest widget; drags keep their ordinary move/look semantics.
        if (MBBattleTouchPolicy.mayForwardNativeOrder(
                    buttonIndexForRole(role) >= 0, cancelled,
                    movedBeyondTouchSlop(id, x, y))
                && nativeTapOrders(x, y)) {
            if (promptTapPointerId == id) promptTapPointerId = -1;
            pointers.end(id);
            if (ownedMove) updateMovement(pointers.moveOwner());
            invalidate();
            return;
        }
        boolean promptTap = promptTapPointerId == id && !cancelled
                && !movedBeyondTouchSlop(id, x, y);
        if (promptTapPointerId == id) promptTapPointerId = -1;
        if (promptTap) {
            refreshButtonVisibility();
            activateMissionActionBriefly(ROLE_USE);
        }
        if (role == ROLE_PAUSE) {
            boolean moved = movedBeyondTouchSlop(id, x, y);
            if (!cancelled && !pauseLongPressTriggered && !moved) {
                pauseBriefly();
            }
            if (id == pausePointerId) {
                pausePointerId = -1;
                pauseLongPressTriggered = false;
            }
        } else if (role == ROLE_CHAT) {
            if (!cancelled && !movedBeyondTouchSlop(id, x, y)) {
                nativeSetAction(ACTION_CHAT, true);
            }
        } else if (isDirectionalCombatRole(role)) {
            if (!cancelled && !pointers.directionalActionStarted(id)) {
                beginDirectionalCombatAction(
                        id, x, y, directionGestureReady(id, x, y));
            }
            if (pointers.directionalActionStarted(id)) {
                releaseDirectionalCombatAction(
                        role, pointers.directionalActionStartedAt(id));
            }
        } else {
            pressRole(role, false);
        }
        pointers.end(id);
        if (ownedMove) updateMovement(pointers.moveOwner());
    }

    private void dispose() {
        if (disposed) return;
        if (editMode) setEditMode(false);
        if (accessibilityFocusedButton >= 0) {
            sendVirtualAccessibilityEvent(accessibilityFocusedButton,
                    AccessibilityEvent.TYPE_VIEW_ACCESSIBILITY_FOCUS_CLEARED);
        }
        accessibilityFocusedButton = -1;
        hoveredButton = -1;
        disposed = true;
        removeCallbacks(releasePause);
        removeCallbacks(enterButtonEditMode);
        dismissRadialMenu();
        unregisterSensor();
        resetAll();
    }

    private void resetAll() {
        removeCallbacks(enterButtonEditMode);
        for (Runnable fallback : directionalActionFallbacks) {
            if (fallback != null) removeCallbacks(fallback);
        }
        dismissRadialMenu();
        promptTapPointerId = -1;
        pausePointerId = -1;
        pauseLongPressTriggered = false;
        for (int id = 0; id < pointers.roles.length; ++id) {
            if (nativeUiKinds[id] == 1) {
                nativeTacticalUiTouch(MotionEvent.ACTION_CANCEL, id,
                        pointers.lastX[id], pointers.lastY[id], SystemClock.uptimeMillis());
            }
            nativeUiKinds[id] = 0;
            pressRole(pointers.roles[id], false);
        }
        pointers.reset();
        for (int role = 0; role < heldRoleCounts.length; ++role) heldRoleCounts[role] = 0;
        moveX = moveY = 0.0f;
        attackHeld = false;
        blockHeld = false;
        resetGyroTracking();
        nativeReset();
    }

    @Override public AccessibilityNodeProvider getAccessibilityNodeProvider() {
        return accessibilityProvider;
    }

    @Override public void onInitializeAccessibilityNodeInfo(AccessibilityNodeInfo info) {
        super.onInitializeAccessibilityNodeInfo(info);
        // The host is only a container for the virtual mission buttons.
        info.setClickable(false);
        info.setLongClickable(false);
        info.removeAction(AccessibilityNodeInfo.ACTION_CLICK);
        info.removeAction(AccessibilityNodeInfo.ACTION_LONG_CLICK);
    }

    @Override public boolean dispatchHoverEvent(MotionEvent event) {
        AccessibilityManager manager = (AccessibilityManager)getContext()
                .getSystemService(Context.ACCESSIBILITY_SERVICE);
        if (manager != null && manager.isEnabled() && manager.isTouchExplorationEnabled()) {
            int action = event.getActionMasked();
            int next = (action == MotionEvent.ACTION_HOVER_ENTER
                    || action == MotionEvent.ACTION_HOVER_MOVE)
                    ? hitButtonIndex(event.getX(), event.getY()) : -1;
            if (action == MotionEvent.ACTION_HOVER_ENTER
                    || action == MotionEvent.ACTION_HOVER_MOVE
                    || action == MotionEvent.ACTION_HOVER_EXIT) {
                int previous = hoveredButton;
                if (previous != next) {
                    hoveredButton = next;
                    if (previous >= 0) sendVirtualAccessibilityEvent(
                            previous, AccessibilityEvent.TYPE_VIEW_HOVER_EXIT);
                    if (next >= 0) sendVirtualAccessibilityEvent(
                            next, AccessibilityEvent.TYPE_VIEW_HOVER_ENTER);
                }
                if (previous >= 0 || next >= 0) return true;
            }
        }
        return super.dispatchHoverEvent(event);
    }

    @Override public boolean performClick() {
        super.performClick();
        return true;
    }

    @Override public boolean onTouchEvent(MotionEvent event) {
        if (disposed) return false;
        if (!nativeReady()) {
            resetAll();
            return false;
        }
        boolean tacticalMission = nativeMissionActive();
        boolean missionSession = nativeMissionSessionActive();
        if (!tacticalMission && !missionSession) {
            resetAll();
            return false;
        }
        int action = event.getActionMasked();
        if (!MBBattleTouchPolicy.shouldCaptureMissionOverlayTouch(
                tacticalMission, missionSession)) {
            resetAll();
            return false;
        }
        int index = event.getActionIndex();
        if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
            startPointer(event, index);
        } else if (action == MotionEvent.ACTION_MOVE) {
            movePointers(event);
        } else if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP) {
            finishPointer(event, index, false);
        } else if (action == MotionEvent.ACTION_CANCEL) {
            for (int i = 0; i < event.getPointerCount(); ++i) finishPointer(event, i, true);
            resetAll();
        }
        invalidate();
        return true;
    }

    private float angleDelta(float value, float origin) {
        float delta = value - origin;
        while (delta > Math.PI) delta -= (float)(Math.PI * 2.0);
        while (delta < -Math.PI) delta += (float)(Math.PI * 2.0);
        return delta;
    }

    private float[] screenAlignedRotationMatrix(SensorEvent event) {
        SensorManager.getRotationMatrixFromVector(rotationMatrix, event.values);
        WindowManager windowManager = (WindowManager)getContext().getSystemService(Context.WINDOW_SERVICE);
        int rotation = windowManager == null
                ? Surface.ROTATION_0 : windowManager.getDefaultDisplay().getRotation();
        int axisX = SensorManager.AXIS_X;
        int axisY = SensorManager.AXIS_Y;
        if (rotation == Surface.ROTATION_90) {
            axisX = SensorManager.AXIS_Y;
            axisY = SensorManager.AXIS_MINUS_X;
        } else if (rotation == Surface.ROTATION_180) {
            axisX = SensorManager.AXIS_MINUS_X;
            axisY = SensorManager.AXIS_MINUS_Y;
        } else if (rotation == Surface.ROTATION_270) {
            axisX = SensorManager.AXIS_MINUS_Y;
            axisY = SensorManager.AXIS_X;
        }
        return SensorManager.remapCoordinateSystem(
                rotationMatrix, axisX, axisY, remappedRotationMatrix)
                ? remappedRotationMatrix : rotationMatrix;
    }

    /** True when the wielded weapon qualifies for gyro aiming under the
     * ranged-only rule: the bridge reports the equipped weapon class. */
    private boolean gyroAimContextValid() {
        if (!attackHeld && !blockHeld) return false;
        if (!preferences.getBoolean(GuestSettingsSync.KEY_GYRO_RANGED_ONLY, true)) {
            return true;
        }
        if (!nativeReady()) return false;
        try {
            return nativeRangedWeaponHeld();
        } catch (RuntimeException | UnsatisfiedLinkError ignored) {
            return true; // bridge without this entry point keeps the old behaviour
        }
    }

    @Override public void onSensorChanged(SensorEvent event) {
        if (disposed || event.sensor.getType() != Sensor.TYPE_GAME_ROTATION_VECTOR) return;
        if (!preferences.getBoolean(GuestSettingsSync.KEY_GYRO_ENABLED, false)
                || !gyroAimContextValid()) {
            resetGyroTracking();
            return;
        }
        SensorManager.getOrientation(screenAlignedRotationMatrix(event), orientation);
        float yaw = orientation[0];
        float pitch = orientation[1];
        if (!gyroTracking || lastGyroTimestampNs <= 0L) {
            lastGyroYaw = yaw;
            lastGyroPitch = pitch;
            lastGyroTimestampNs = event.timestamp;
            gyroTracking = true;
            return;
        }
        float deltaSeconds = (event.timestamp - lastGyroTimestampNs) * 1.0e-9f;
        float yawDelta = angleDelta(yaw, lastGyroYaw);
        float pitchDelta = angleDelta(pitch, lastGyroPitch);
        lastGyroYaw = yaw;
        lastGyroPitch = pitch;
        lastGyroTimestampNs = event.timestamp;
        float sensitivity = pref(
                GuestSettingsSync.KEY_GYRO_SENSITIVITY, DEFAULT_GYRO_SENSITIVITY);
        float deadzone = pref(
                GuestSettingsSync.KEY_GYRO_DEADZONE, GuestSettingsSync.DEFAULT[3]);
        float pitchSign = preferences.getBoolean(
                GuestSettingsSync.KEY_GYRO_INVERT_Y, false) ? 1.0f : -1.0f;
        submitGyroLookDelta(
                InputMath.gyroLookUnits(yawDelta, deltaSeconds, deadzone, sensitivity),
                InputMath.gyroLookUnits(pitchDelta * pitchSign, deltaSeconds,
                        deadzone, sensitivity));
    }

    @Override public void onAccuracyChanged(Sensor sensor, int accuracy) {
    }

    private void registerSensor() {
        if (!disposed && !sensorRegistered
                && preferences.getBoolean(GuestSettingsSync.KEY_GYRO_ENABLED, false)
                && sensorManager != null && rotationVector != null) {
            sensorRegistered = sensorManager.registerListener(this, rotationVector, SensorManager.SENSOR_DELAY_GAME);
            if (sensorRegistered) ReleaseLog.i("MBControls", "gyro listener registered");
        }
    }

    private void unregisterSensor() {
        if (sensorRegistered && sensorManager != null) {
            sensorManager.unregisterListener(this);
            ReleaseLog.i("MBControls", "gyro listener unregistered");
        }
        sensorRegistered = false;
        resetGyroTracking();
    }

    @Override protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        if (disposed) return;
        ReleaseLog.i("MBControls", "overlay attached");
        registerSensor();
        post(new Runnable() {
            @Override public void run() {
                if (disposed) return;
                refreshButtonVisibility();
                if (!missionStateHas(MISSION_STATE_PLAYER_MOUNTED)) {
                    showOneTimeHint(KEY_LAYOUT_HINT_SHOWN,
                            MBTouchStrings.Text.LAYOUT_EDIT_HINT);
                }
            }
        });
        postInvalidate();
    }

    @Override protected void onSizeChanged(int width, int height, int oldWidth, int oldHeight) {
        super.onSizeChanged(width, height, oldWidth, oldHeight);
        buttonLayoutDirty = true;
        ReleaseLog.i("MBControls", "overlay size " + width + "x" + height);
        invalidate();
    }

    @Override protected void onWindowVisibilityChanged(int visibility) {
        super.onWindowVisibilityChanged(visibility);
        ReleaseLog.i("MBControls", "overlay visibility " + visibility);
        if (!disposed && visibility == VISIBLE) {
            registerSensor();
        } else if (!disposed) {
            unregisterSensor();
            resetAll();
        }
    }

    @Override protected void onDetachedFromWindow() {
        dispose();
        super.onDetachedFromWindow();
    }

    private void drawButton(Canvas canvas, float x, float y, float radius, String label) {
        drawButton(canvas, x, y, radius, label, false);
    }

    private void drawButton(Canvas canvas, float x, float y, float radius, String label,
                            boolean highlighted) {
        canvas.drawCircle(x, y, radius, fill);
        canvas.drawCircle(x, y, radius, stroke);
        if (highlighted) {
            float originalStrokeWidth = stroke.getStrokeWidth();
            int originalAlpha = stroke.getAlpha();
            Paint.Style originalStyle = stroke.getStyle();
            stroke.setStyle(Paint.Style.STROKE);
            stroke.setStrokeWidth(dp(3.0f));
            stroke.setAlpha(Math.min(255, originalAlpha + 65));
            canvas.drawCircle(x, y, radius + dp(3.0f), stroke);
            stroke.setStrokeWidth(originalStrokeWidth);
            stroke.setAlpha(originalAlpha);
            stroke.setStyle(originalStyle);
        }
        float originalTextSize = text.getTextSize();
        float measuredWidth = text.measureText(label);
        float availableWidth = radius * 1.55f;
        if (measuredWidth > availableWidth && measuredWidth > 0.0f) {
            text.setTextSize(Math.max(sp(8.0f),
                    originalTextSize * availableWidth / measuredWidth));
        }
        Paint.FontMetrics metrics = text.getFontMetrics();
        if (text.measureText(label) > availableWidth
                && label.codePointCount(0, label.length()) > 1) {
            int codePoints = label.codePointCount(0, label.length());
            int split = label.offsetByCodePoints(0, (codePoints + 1) / 2);
            String firstLine = label.substring(0, split);
            String secondLine = label.substring(split);
            float lineAdvance = (metrics.descent - metrics.ascent) * 0.78f;
            float baselineCenter = y - (metrics.ascent + metrics.descent) * 0.5f;
            canvas.drawText(firstLine, x, baselineCenter - lineAdvance * 0.5f, text);
            canvas.drawText(secondLine, x, baselineCenter + lineAdvance * 0.5f, text);
        } else {
            canvas.drawText(label, x, y - (metrics.ascent + metrics.descent) * 0.5f, text);
        }
        text.setTextSize(originalTextSize);
    }

    private void drawRadialActionMenu(Canvas canvas, String languageCode) {
        int menuIndex = radialMenuButtonIndex();
        if (menuIndex < 0) return;
        resolveRadialMenuCenter();
        float centerX = radialMenuCenter[0];
        float centerY = radialMenuCenter[1];
        float ringRadius = radialMenuRadius();
        float nodeRadius = radialButtonRadius();
        float originalStrokeWidth = stroke.getStrokeWidth();
        int originalAlpha = stroke.getAlpha();
        Paint.Style originalStyle = stroke.getStyle();
        stroke.setStyle(Paint.Style.STROKE);
        stroke.setStrokeWidth(dp(1.5f));
        stroke.setAlpha(Math.min(originalAlpha, 125));
        float anchorX = MBButtonLayoutPolicy.finite(radialMenuAnchorX)
                ? radialMenuAnchorX : buttonCenterX[menuIndex];
        float anchorY = MBButtonLayoutPolicy.finite(radialMenuAnchorY)
                ? radialMenuAnchorY : buttonCenterY[menuIndex];
        canvas.drawLine(anchorX, anchorY, centerX, centerY, stroke);
        canvas.drawCircle(centerX, centerY, ringRadius * 1.42f, stroke);
        for (int index = 0; index < RADIAL_ACTION_ROLES.length; ++index) {
            int role = RADIAL_ACTION_ROLES[index];
            if (!radialActionAvailable(role)) continue;
            MBBattleTouchPolicy.radialActionCenter(centerX, centerY, ringRadius,
                    index, radialNodeCenter);
            canvas.drawLine(centerX, centerY,
                    radialNodeCenter[0], radialNodeCenter[1], stroke);
        }
        stroke.setStrokeWidth(originalStrokeWidth);
        stroke.setAlpha(originalAlpha);
        stroke.setStyle(originalStyle);

        for (int index = 0; index < RADIAL_ACTION_ROLES.length; ++index) {
            int role = RADIAL_ACTION_ROLES[index];
            if (!radialActionAvailable(role)) continue;
            MBBattleTouchPolicy.radialActionCenter(centerX, centerY, ringRadius,
                    index, radialNodeCenter);
            drawButton(canvas, radialNodeCenter[0], radialNodeCenter[1], nodeRadius,
                    buttonLabel(languageCode, role), radialMenuTargetRole == role);
        }
        drawRadialSelectionLabel(canvas, languageCode);
    }

    private void drawRadialSelectionLabel(Canvas canvas, String languageCode) {
        if (radialMenuTargetRole == ROLE_NONE) return;
        drawTopCallout(canvas, buttonAccessibilityLabel(languageCode, radialMenuTargetRole));
    }

    private void drawTopCallout(Canvas canvas, String label) {
        SafeInsets insets = safeInsets(activeDecor != null ? activeDecor : this);
        float horizontalPadding = dp(18.0f);
        float verticalPadding = dp(8.0f);
        float availableWidth = Math.max(0.0f,
                getWidth() - insets.left - insets.right - dp(24.0f));
        float originalTextSize = text.getTextSize();
        int originalTextAlpha = text.getAlpha();
        text.setTextSize(sp(18.0f) * Math.min(1.20f, densityAwareScale()));
        float measuredWidth = text.measureText(label);
        if (measuredWidth + horizontalPadding * 2.0f > availableWidth
                && measuredWidth > 0.0f) {
            text.setTextSize(Math.max(sp(12.0f), text.getTextSize()
                    * Math.max(0.0f, availableWidth - horizontalPadding * 2.0f)
                    / measuredWidth));
            measuredWidth = text.measureText(label);
        }
        Paint.FontMetrics metrics = text.getFontMetrics();
        float boxWidth = Math.min(availableWidth, measuredWidth + horizontalPadding * 2.0f);
        float boxHeight = metrics.descent - metrics.ascent + verticalPadding * 2.0f;
        float centerX = insets.left
                + (getWidth() - insets.left - insets.right) * 0.5f;
        float top = insets.top + dp(12.0f);
        float left = centerX - boxWidth * 0.5f;
        float right = centerX + boxWidth * 0.5f;
        float bottom = top + boxHeight;
        float corner = dp(12.0f);
        canvas.drawRoundRect(left, top, right, bottom, corner, corner, calloutFill);
        canvas.drawRoundRect(left, top, right, bottom, corner, corner, calloutStroke);
        text.setAlpha(255);
        canvas.drawText(label, centerX,
                top + verticalPadding - metrics.ascent, text);
        text.setAlpha(originalTextAlpha);
        text.setTextSize(originalTextSize);
    }

    private String buttonLabel(String languageCode, int role) {
        MBTouchStrings.Text key;
        if (role == ROLE_ATTACK) key = MBTouchStrings.Text.ATTACK_LABEL;
        else if (role == ROLE_BLOCK) key = MBTouchStrings.Text.BLOCK_LABEL;
        else if (role == ROLE_USE) key = MBTouchStrings.Text.USE_LABEL;
        else if (role == ROLE_KICK) key = MBTouchStrings.Text.KICK_LABEL;
        else if (role == ROLE_JUMP) key = MBTouchStrings.Text.JUMP_LABEL;
        else if (role == ROLE_DROP) key = MBTouchStrings.Text.DROP_LABEL;
        else if (role == ROLE_CYCLE_WEAPON) key = MBTouchStrings.Text.CYCLE_WEAPON_LABEL;
        else if (role == ROLE_CYCLE_SHIELD) key = MBTouchStrings.Text.CYCLE_SHIELD_LABEL;
        else if (role == ROLE_ZOOM) key = MBTouchStrings.Text.ZOOM_LABEL;
        else if (role == ROLE_VIEW) key = MBTouchStrings.Text.VIEW_LABEL;
        else if (role == ROLE_SHEATH) key = MBTouchStrings.Text.SHEATH_LABEL;
        else if (role == ROLE_BACK) key = MBTouchStrings.Text.BACK_LABEL;
        else if (role == ROLE_ORDERS) key = MBTouchStrings.Text.ORDERS_LABEL;
        else if (role == ROLE_WEAPON_MODE) key = MBTouchStrings.Text.WEAPON_MODE_LABEL;
        else if (role == ROLE_ACTIONS_MENU) key = MBTouchStrings.Text.MORE_ACTIONS_LABEL;
        else if (role == ROLE_CHAT) key = MBTouchStrings.Text.CHAT_LABEL;
        else key = MBTouchStrings.Text.PAUSE_LABEL;
        return MBTouchStrings.get(languageCode, key);
    }

    private String buttonAccessibilityLabel(String languageCode, int role) {
        MBTouchStrings.Text key;
        if (role == ROLE_ATTACK) key = MBTouchStrings.Text.ATTACK_ACCESSIBILITY;
        else if (role == ROLE_BLOCK) key = MBTouchStrings.Text.BLOCK_ACCESSIBILITY;
        else if (role == ROLE_USE) key = MBTouchStrings.Text.USE_ACCESSIBILITY;
        else if (role == ROLE_KICK) key = MBTouchStrings.Text.KICK_ACCESSIBILITY;
        else if (role == ROLE_JUMP) key = MBTouchStrings.Text.JUMP_ACCESSIBILITY;
        else if (role == ROLE_DROP) key = MBTouchStrings.Text.DROP_ACCESSIBILITY;
        else if (role == ROLE_CYCLE_WEAPON) key = MBTouchStrings.Text.CYCLE_WEAPON_ACCESSIBILITY;
        else if (role == ROLE_CYCLE_SHIELD) key = MBTouchStrings.Text.CYCLE_SHIELD_ACCESSIBILITY;
        else if (role == ROLE_ZOOM) key = MBTouchStrings.Text.ZOOM_ACCESSIBILITY;
        else if (role == ROLE_VIEW) key = MBTouchStrings.Text.VIEW_ACCESSIBILITY;
        else if (role == ROLE_SHEATH) key = MBTouchStrings.Text.SHEATH_ACCESSIBILITY;
        else if (role == ROLE_BACK) key = MBTouchStrings.Text.BACK_ACCESSIBILITY;
        else if (role == ROLE_ORDERS) key = MBTouchStrings.Text.ORDERS_ACCESSIBILITY;
        else if (role == ROLE_WEAPON_MODE) key = MBTouchStrings.Text.WEAPON_MODE_ACCESSIBILITY;
        else if (role == ROLE_ACTIONS_MENU) key = MBTouchStrings.Text.MORE_ACTIONS_ACCESSIBILITY;
        else if (role == ROLE_CHAT) key = MBTouchStrings.Text.CHAT_ACCESSIBILITY;
        else key = MBTouchStrings.Text.PAUSE_ACCESSIBILITY;
        return MBTouchStrings.get(languageCode, key);
    }

    /** Applies the configured button opacity to the shared paints. */
    private void applyOpacity() {
        float opacity = Math.max(0.05f, Math.min(1.0f, buttonOpacity()));
        fill.setAlpha(Math.round(72 * opacity));
        stroke.setAlpha(Math.round(150 * opacity));
        text.setAlpha(Math.round(210 * opacity));
    }

    @Override protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        if (disposed) return;
        if (!drawLogged) {
            drawLogged = true;
            ReleaseLog.i("MBControls", "overlay drawing " + getWidth() + "x" + getHeight());
        }
        if (nativeReady() && nativeMissionActive()) {
            String languageCode = storedGameLanguage(preferences);
            resolveButtonLayout();
            refreshButtonVisibility();
            applyOpacity();
            text.setTextSize(Math.max(sp(8.0f), sp(13.0f)
                    * resolvedButtonScale * densityAwareScale()));
            for (int index = 0; index < BUTTON_ROLES.length; ++index) {
                if (!isButtonShown(index)) continue;
                drawButton(canvas, buttonCenterX[index], buttonCenterY[index],
                        buttonRadius[index], buttonLabel(languageCode, BUTTON_ROLES[index]));
            }
            if (radialMenuOpen) drawRadialActionMenu(canvas, languageCode);
            if (editMode) {
                drawTopCallout(canvas, MBTouchStrings.get(
                        languageCode, MBTouchStrings.Text.EDIT_MODE_HINT));
            } else if (!radialMenuOpen
                    && SystemClock.uptimeMillis() < dismountHintVisibleUntil) {
                drawTopCallout(canvas, MBTouchStrings.get(
                        languageCode, MBTouchStrings.Text.DISMOUNT_HINT));
            }
        }
        postInvalidateDelayed(120L);
    }
}
