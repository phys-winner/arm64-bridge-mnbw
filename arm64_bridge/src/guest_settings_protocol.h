#ifndef WARBAND_GUEST_SETTINGS_PROTOCOL_H
#define WARBAND_GUEST_SETTINGS_PROTOCOL_H

/*
 * Fixed-address protocol shared by the freestanding ARM32 settings payload
 * and the AArch64 host bridge. Keep the C section freestanding: the payload
 * is linked without a C runtime.
 *
 * v2: gyro aiming defaults to off, the touch look-sensitivity multiplier was
 * removed (the native mouse-sensitivity option is the single look control),
 * and the payload gained button opacity, an FPS limit choice, a left-handed
 * mode and a latched layout-edit request consumed by the next safe mission.
 *
 * v3: the in-game Additional tab exposes the renderer's native HDR, automatic
 * exposure, depth-of-field and shadow-map controls.  HDR brightness is a bridge
 * uniform because the Tegra renderer never authored an equivalent setting.
 * The extension floats and widget pointers live in reserved[] so every v2
 * synchronization offset remains stable.
 *
 * v4: touch haptics are user-configurable and the HDR-dependent rows reflow
 * instead of leaving holes.  The new checkbox and reflow-only widget pointers
 * live in reserved[]; all previously published offsets remain unchanged.
 *
 * v5: world-map pan sensitivity is a persisted host/guest float.  Its values
 * and widget pointers use explicit reserved[] slots so the v3 graphics slots
 * remain byte-for-byte stable.
 *
 * v7: FXAA is a persisted checkbox (bit 13). Float slot 7 now carries the
 * renderer's plant-shadow mode; the Additional-tab checkbox maps enabled to
 * 2 and disabled to 0. The binary layout and every offset remain unchanged.
 *
 * v8: combat report text has a persisted scale slider. Its float values and
 * widget pointers use reserved[31..34]; the state remains exactly 0x400 bytes.
 */

typedef unsigned int gst_u32;
typedef int gst_i32;

#define GST_STATE_VA                 0x0278E400u
#define GST_STATE_BYTES              0x00000400u
#define GST_STATE_END_VA             (GST_STATE_VA + GST_STATE_BYTES)
#define GST_MAGIC                    0x42545347u /* little-endian bytes: GSTB */
#define GST_ABI_VERSION              8u
#define GST_CUSTOM_TAB_ID            14u
#define GST_OPTIONS_WINDOW_ID        13u
#define GST_CONTROLS_WINDOW_ID       15u
#define GST_QUICK_BATTLE_WINDOW_ID   99u
#define GST_MAX_SAVED_CHILDREN       64u

#define GST_BOOL_GYRO_ENABLED        (1u << 0)
#define GST_BOOL_GYRO_INVERT_Y       (1u << 1)
#define GST_BOOL_MENU_SWIPE          (1u << 2)
/* Bit 3: was shoulder-badge taps (removed); now gyroscope ranged-only. */
#define GST_BOOL_GYRO_RANGED_ONLY    (1u << 3)
#define GST_BOOL_LEFT_HANDED         (1u << 4)
#define GST_BOOL_LAYOUT_EDIT_REQUEST (1u << 5) /* guest -> host pending request */
#define GST_BOOL_CAMERA_MOUSE        (1u << 6)
#define GST_BOOL_ALWAYS_RUN          (1u << 7)
#define GST_BOOL_HDR_ENABLED         (1u << 8)
#define GST_BOOL_DOF_ENABLED         (1u << 9)
#define GST_BOOL_AUTO_EXPOSURE       (1u << 10)
#define GST_BOOL_HAPTIC_FEEDBACK     (1u << 11)
#define GST_BOOL_MENU_ASPECT         (1u << 12)
#define GST_BOOL_FXAA_ENABLED        (1u << 13)
#define GST_BOOL_MASK_ALL            0x00003FFFu
/* Layout edit is guest-only; the retired DOF bit is never host-writable. */
#define GST_BOOL_HOST_WRITABLE_MASK  0x00003D9Fu

/* Mission state is supplied by the host because cur_mission may be stale. */
#define GST_MISSION_STATUS_UNKNOWN   0u
#define GST_MISSION_STATUS_INACTIVE  1u
#define GST_MISSION_STATUS_ACTIVE    2u

enum gst_status {
    GST_STATUS_EMPTY = 0u,
    GST_STATUS_BUILDING = 1u,
    GST_STATUS_READY = 2u,
    GST_STATUS_CUSTOM_ACTIVE = 3u,
    GST_STATUS_ERROR = 0x80000000u,
};

enum gst_error {
    GST_ERROR_NONE = 0u,
    GST_ERROR_MISSING_OPTIONS_TREE = 1u,
    GST_ERROR_ALLOCATION = 2u,
    GST_ERROR_WIDGET_CREATION = 3u,
    GST_ERROR_CHILD_VECTOR = 4u,
    GST_ERROR_CHILD_OVERFLOW = 5u,
};

enum gst_language {
    GST_LANGUAGE_EN = 0u,
    GST_LANGUAGE_RU = 1u,
};

enum gst_float_setting {
    GST_FLOAT_MOVE_SENSITIVITY = 0,
    GST_FLOAT_GYRO_SENSITIVITY = 1,
    GST_FLOAT_GYRO_DEADZONE = 2,
    GST_FLOAT_BUTTON_SCALE = 3,
    GST_FLOAT_BUTTON_OPACITY = 4,
    GST_FLOAT_FPS_LIMIT = 5,
    GST_FLOAT_HDR_BRIGHTNESS = 6,
    GST_FLOAT_SHADOW_MODE = 7,
    GST_FLOAT_MAP_PAN_SENSITIVITY = 8,
    GST_FLOAT_REPORT_TEXT_SCALE = 9,
    GST_FLOAT_COUNT = 10,
};

/* The original six floats retain their v2 offsets.  Extension floats are
 * mirrored in reserved[] under the same sequence lock. */
#define GST_CORE_FLOAT_COUNT                 6u
#define GST_RESERVED_GYRO_ROWS_SHOWN         0u
#define GST_RESERVED_PENDING_CUSTOM          1u
#define GST_RESERVED_GYRO_TEXT_SENS          2u
#define GST_RESERVED_GYRO_TEXT_DEADZONE      3u
#define GST_RESERVED_HDR_CHECKBOX             4u
#define GST_RESERVED_DOF_CHECKBOX             5u
#define GST_RESERVED_AUTOEXP_CHECKBOX         6u
#define GST_RESERVED_HDR_BRIGHTNESS_TEXT      7u
#define GST_RESERVED_HDR_BRIGHTNESS_SLIDER    8u
#define GST_RESERVED_LEGACY_SHADOW_MODE_TEXT  9u
#define GST_RESERVED_SHADOW_MODE_CHECKBOX    10u
#define GST_RESERVED_HDR_ROWS_SHOWN          11u
#define GST_RESERVED_HOST_FLOAT_BASE         16u
#define GST_RESERVED_GUEST_FLOAT_BASE        18u
#define GST_RESERVED_HAPTIC_CHECKBOX          20u
#define GST_RESERVED_BUTTON_OPACITY_TEXT      21u
#define GST_RESERVED_BUTTON_SCALE_TEXT        22u
#define GST_RESERVED_MOVE_SENSITIVITY_TEXT    23u
#define GST_RESERVED_FPS_LIMIT_TEXT           24u
#define GST_RESERVED_MAP_PAN_HOST_FLOAT        25u
#define GST_RESERVED_MAP_PAN_GUEST_FLOAT       26u
#define GST_RESERVED_MAP_PAN_TEXT              27u
#define GST_RESERVED_MAP_PAN_SLIDER            28u
#define GST_RESERVED_MENU_ASPECT_CHECKBOX       29u
#define GST_RESERVED_FXAA_CHECKBOX              30u
#define GST_RESERVED_REPORT_SCALE_HOST_FLOAT    31u
#define GST_RESERVED_REPORT_SCALE_GUEST_FLOAT   32u
#define GST_RESERVED_REPORT_SCALE_TEXT          33u
#define GST_RESERVED_REPORT_SCALE_SLIDER        34u

/* FPS limit slider positions; GST_FPS_UNLIMITED tracks the display rate. */
#define GST_FPS_30  0.0f
#define GST_FPS_60  1.0f
#define GST_FPS_90  2.0f
#define GST_FPS_UNLIMITED 3.0f

typedef struct guest_settings_state {
    volatile gst_u32 magic;                         /* 0x000 */
    volatile gst_u32 abi_version;                   /* 0x004 */
    volatile gst_u32 struct_size;                   /* 0x008 */
    volatile gst_u32 status;                        /* 0x00C */
    volatile gst_u32 error;                         /* 0x010 */
    volatile gst_u32 language;                      /* 0x014 */
    volatile gst_u32 activation_seq;                /* 0x018 */
    volatile gst_u32 open_seq;                      /* 0x01C */
    volatile gst_u32 close_seq;                     /* 0x020 */
    volatile gst_u32 host_write_seq;                /* 0x024, even when stable */
    volatile gst_u32 applied_host_seq;              /* 0x028 */
    volatile gst_u32 guest_write_seq;               /* 0x02C, even when stable */
    volatile gst_u32 options_window;                /* 0x030 */
    volatile gst_u32 root;                          /* 0x034 */
    volatile gst_u32 tab;                           /* 0x038 */
    volatile gst_u32 custom_container;              /* 0x03C */
    volatile gst_u32 custom_frame;                  /* 0x040 */
    volatile gst_u32 checkboxes[6];                 /* 0x044 */
    volatile gst_u32 sliders[GST_CORE_FLOAT_COUNT]; /* 0x05C */
    volatile gst_u32 host_bool_mask;                /* 0x074 */
    volatile float host_values[GST_CORE_FLOAT_COUNT]; /* 0x078 */
    volatile gst_u32 guest_bool_mask;               /* 0x090 */
    volatile float guest_values[GST_CORE_FLOAT_COUNT]; /* 0x094 */
    volatile gst_u32 saved_child_count;             /* 0x0AC */
    volatile gst_u32 saved_child_overflow;          /* 0x0B0 */
    volatile gst_u32 saved_children[GST_MAX_SAVED_CHILDREN]; /* 0x0B4 */
    volatile gst_u32 saved_flags[GST_MAX_SAVED_CHILDREN];    /* 0x1B4 */
    volatile gst_u32 reserved[82];                  /* 0x2B4..0x3FB */
    volatile gst_u32 host_mission_status;            /* 0x3FC, host-owned */
} guest_settings_state;

#if defined(__cplusplus)
#define GST_STATIC_ASSERT(condition, message) static_assert(condition, message)
#else
#define GST_STATIC_ASSERT(condition, message) _Static_assert(condition, message)
#endif

#define GST_OFFSETOF(type, member) __builtin_offsetof(type, member)
GST_STATIC_ASSERT(sizeof(gst_u32) == 4u, "ARM32 protocol requires 32-bit words");
GST_STATIC_ASSERT(sizeof(float) == 4u, "protocol requires IEEE-sized 32-bit floats");
GST_STATIC_ASSERT(GST_OFFSETOF(guest_settings_state, host_write_seq) == 0x024u,
                  "host sequence offset drift");
GST_STATIC_ASSERT(GST_OFFSETOF(guest_settings_state, guest_write_seq) == 0x02Cu,
                  "guest sequence offset drift");
GST_STATIC_ASSERT(GST_OFFSETOF(guest_settings_state, checkboxes) == 0x044u,
                  "checkbox pointer offset drift");
GST_STATIC_ASSERT(GST_OFFSETOF(guest_settings_state, sliders) == 0x05Cu,
                  "slider pointer offset drift");
GST_STATIC_ASSERT(GST_OFFSETOF(guest_settings_state, host_bool_mask) == 0x074u,
                  "host bool-mask offset drift");
GST_STATIC_ASSERT(GST_OFFSETOF(guest_settings_state, host_values) == 0x078u,
                  "host float-array offset drift");
GST_STATIC_ASSERT(GST_OFFSETOF(guest_settings_state, guest_bool_mask) == 0x090u,
                  "guest bool-mask offset drift");
GST_STATIC_ASSERT(GST_OFFSETOF(guest_settings_state, guest_values) == 0x094u,
                  "guest float-array offset drift");
GST_STATIC_ASSERT(GST_OFFSETOF(guest_settings_state, saved_children) == 0x0B4u,
                  "saved-child offset drift");
GST_STATIC_ASSERT(GST_OFFSETOF(guest_settings_state, saved_flags) == 0x1B4u,
                  "saved-flags offset drift");
GST_STATIC_ASSERT(GST_OFFSETOF(guest_settings_state, reserved) == 0x2B4u,
                  "reserved-field offset drift");
GST_STATIC_ASSERT(GST_OFFSETOF(guest_settings_state, host_mission_status) == 0x3FCu,
                  "host mission-status offset drift");
GST_STATIC_ASSERT(sizeof(guest_settings_state) == GST_STATE_BYTES,
                  "guest settings state must occupy exactly 0x400 bytes");

static inline int gst_layout_edit_can_open_quick_battle(gst_u32 mission_status,
                                                         gst_u32 game_object) {
    return mission_status == GST_MISSION_STATUS_INACTIVE && game_object != 0u;
}

#if defined(__cplusplus)

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace warband::guest_settings {

/* Gyro aiming ships disabled; swipe, ranged-only gyro and always-run stay on. */
inline constexpr uint32_t kDefaultBoolMask =
    GST_BOOL_MENU_SWIPE | GST_BOOL_GYRO_RANGED_ONLY | GST_BOOL_ALWAYS_RUN |
    GST_BOOL_AUTO_EXPOSURE | GST_BOOL_HAPTIC_FEEDBACK |
    GST_BOOL_MENU_ASPECT;
inline constexpr std::array<float, GST_FLOAT_COUNT> kMinimumValues = {
    0.5f, 0.2f, 0.0f, 0.6f, 0.2f, 0.0f, 0.5f, 0.0f, 0.35f, 0.5f,
};
inline constexpr std::array<float, GST_FLOAT_COUNT> kMaximumValues = {
    1.8f, 4.0f, 0.06f, 1.4f, 1.0f, 3.0f, 1.25f, 2.0f, 1.50f, 1.5f,
};
inline constexpr std::array<float, GST_FLOAT_COUNT> kDefaultValues = {
    1.0f, 1.25f, 0.012f, 1.0f, 1.0f, 3.0f, 0.75f, 2.0f, 0.70f, 1.0f,
};

struct Values {
    uint32_t bool_mask = kDefaultBoolMask;
    std::array<float, GST_FLOAT_COUNT> floats = kDefaultValues;
};

inline float BoundHostFloat(std::size_t index, float value) {
    if (index >= kDefaultValues.size()) return 0.0f;
    if (!std::isfinite(value)) return kDefaultValues[index];
    /* A checked foliage control is the engine's mode 2, not legacy mode 1. */
    if (index == GST_FLOAT_SHADOW_MODE) return value > 0.0f ? 2.0f : 0.0f;
    if (value < kMinimumValues[index]) return kMinimumValues[index];
    if (value > kMaximumValues[index]) return kMaximumValues[index];
    return value;
}

/* Host proposals never raise guest-only request bits. */
inline Values BoundedHostValues(const Values& input) {
    Values output = input;
    output.bool_mask &= GST_BOOL_HOST_WRITABLE_MASK;
    for (std::size_t index = 0; index < output.floats.size(); ++index) {
        output.floats[index] = BoundHostFloat(index, output.floats[index]);
    }
    return output;
}

inline bool IsValidGuestValues(const Values& input) {
    if ((input.bool_mask & ~static_cast<uint32_t>(GST_BOOL_MASK_ALL)) != 0u) return false;
    for (std::size_t index = 0; index < input.floats.size(); ++index) {
        const float value = input.floats[index];
        if (!std::isfinite(value) || value < kMinimumValues[index] ||
            value > kMaximumValues[index]) {
            return false;
        }
    }
    return true;
}

inline bool SameValues(const Values& left, const Values& right) {
    if (left.bool_mask != right.bool_mask) return false;
    for (std::size_t index = 0; index < left.floats.size(); ++index) {
        if (left.floats[index] != right.floats[index]) return false;
    }
    return true;
}

/* FPS limit snapshot: discrete slider position to frames per second. */
inline constexpr int FpsLimitChoice(float position) {
    const int choice = static_cast<int>(position + 0.5f);
    return choice < 0 ? 0 : (choice > 3 ? 3 : choice);
}
inline constexpr int FpsLimitValue(int choice) {
    switch (choice) {
        case 0: return 30;
        case 1: return 60;
        case 2: return 90;
        default: return 0;  /* 0 = uncapped (display rate) */
    }
}

inline constexpr int ShadowModeChoice(float position) {
    const int choice = static_cast<int>(position + 0.5f);
    return choice < 0 ? 0 : (choice > 2 ? 2 : choice);
}

inline constexpr bool IsStablePublishedSequence(uint32_t before, uint32_t after) {
    return before != 0u && before == after && (before & 1u) == 0u;
}

inline constexpr uint32_t NextOddWriteSequence(uint32_t observed) {
    return (observed & ~1u) + 1u;
}

inline constexpr uint32_t PublishedSequence(uint32_t odd) {
    return odd + 1u;
}

}  // namespace warband::guest_settings

#endif  /* __cplusplus */

#endif  /* WARBAND_GUEST_SETTINGS_PROTOCOL_H */
