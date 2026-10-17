#include "guest_settings_protocol.h"

/* Fixed link-time layout. Runtime addresses are load_bias + these values. */
#define GST_ACTIVATE_ENTRY_VA            0x003CB080u

#define VA_RGL_STRING_CTOR_CSTR          0x0020DDF0u
#define VA_RGL_STRING_DTOR               0x0020DF1Cu
#define VA_CREATE_TEXT                   0x00131D24u
#define VA_WIDGET_HIDE                   0x00135394u
#define VA_WIDGET_UNHIDE                 0x001353A4u
#define VA_WIDGET_DISABLE                0x001353B4u
#define VA_WIDGET_UNDISABLE              0x001353C4u
#define VA_TAB_ADD_ITEM                  0x0021E538u
#define VA_CREATE_CHECK_BOX              0x001331F8u
#define VA_CREATE_VERTICAL_SCROLLABLE    0x002A83C0u
#define VA_CREATE_SLIDER_HOR             0x002A91B4u
#define VA_CREATE_COMBO_LABEL            0x001320F0u
#define VA_COMBO_ADD_ITEM                0x0013ACF8u
#define VA_COMBO_SET_VALUE_I             0x0013AB68u
#define VA_UPDATE_SCROLLABLE_BOARD       0x002A9690u
#define VA_CHECK_BOX_SET_VALUE_I         0x00138370u
#define VA_WIDGET_SET_VALUE_F            0x00133DC0u
#define VA_SET_ACTIVE_WIDGET_HIGHLIGHT   0x00214E94u
#define VA_RGL_WINDOW_MANAGER            0x003E6188u

#define VA_LANGUAGE_LITERAL              0x002564CCu
#define VA_LANGUAGE_GOT_BASE             0x003D7128u
#define EXPECTED_EN_LANGUAGE_WORD        0xFFFCC508u

#define OPTIONS_ROOT_OFFSET              0x01B8u
#define OPTIONS_TAB_OFFSET               0x01BCu
#define OPTIONS_LOGO_OFFSET              0x01C0u
#define OPTIONS_DONE_OFFSET              0x0264u
#define CONTROLS_TAB_OFFSET              0x01BCu
#define CONTAINER_CHILD_BEGIN_OFFSET     0x035Cu
#define CONTAINER_CHILD_END_OFFSET       0x0360u
#define TAB_PANEL_OFFSET                 0x036Cu
#define TAB_ITEMS_BEGIN_OFFSET           0x0370u
#define TAB_ITEMS_END_OFFSET             0x0374u
#define TAB_ACTIVE_ID_OFFSET             0x037Cu
#define TAB_REQUESTED_ID_OFFSET          0x0380u
#define VERTICAL_CONTENT_OFFSET          0x0748u
#define WIDGET_FLAGS_OFFSET              0x01BCu
#define WIDGET_POSITION_Y_OFFSET         0x01A0u
#define WIDGET_LOCAL_POSITION_Y_OFFSET   0x01A8u
#define WIDGET_VALUE_I_OFFSET            0x02B0u
#define WIDGET_VALUE_F_OFFSET            0x02BCu
#define SLIDER_MIN_OFFSET                0x02C0u
#define SLIDER_MAX_OFFSET                0x02C4u
#define TAB_ITEM_ID_OFFSET               0x02D0u
#define VTABLE_SET_VALUE_I_OFFSET        0x0044u

#define WIDGET_HIDDEN_BIT                0x1u
#define WIDGET_DISABLED_BIT              0x2u
#define WIDGET_INACTIVE_BITS             0x3u
#define RGL_STRING_BYTES                 144u

#define GST_PENDING_CUSTOM_MAGIC         0x444E4550u /* little-endian PEND */
#define GST_NAV_EVENT_PREVIOUS_A         102u
#define GST_NAV_EVENT_NEXT_A             103u
#define GST_NAV_EVENT_NEXT_B             104u
#define GST_NAV_EVENT_PREVIOUS_B         105u
#define GST_NAV_REPLAY                   0u
#define GST_NAV_CONSUME                  1u
#define GST_NAV_ROUTE_CONTROLS           2u

GST_STATIC_ASSERT(GST_OFFSETOF(guest_settings_state, reserved) +
                      GST_RESERVED_PENDING_CUSTOM * sizeof(gst_u32) == 0x2B8u,
                  "pending custom token must stay in reserved[1]");
typedef unsigned char gst_u8;

typedef void (*gst_string_ctor_fn)(void*, const char*);
typedef void (*gst_string_dtor_fn)(void*);
typedef void (*gst_widget_void_fn)(void*);
typedef void (*gst_tab_add_item_fn)(void*, const void*, gst_u32);
typedef void* (*gst_create_text_fn)(void*, const void*, const void*,
                                    const float*, const float*, const float*);
typedef void* (*gst_create_check_box_fn)(void*, const void*, const void*,
                                         const float*, const float*, const float*);
typedef void* (*gst_create_vertical_scrollable_fn)(
    void*, const void*, const float*, const float*, const float*, const float*);
typedef void* (*gst_create_slider_fn)(void*, const void*, const float*,
                                      const float*, const float*);
typedef void* (*gst_create_combo_fn)(void*, const void*, const float*,
                                     const float*, const float*);
typedef void (*gst_combo_add_item_fn)(void*, const void*);
typedef void (*gst_set_i_fn)(void*, gst_u32);
typedef void (*gst_set_f_fn)(void*, float);
typedef void (*gst_set_active_widget_highlight_fn)(void*, void*);

typedef struct gst_string_storage {
    gst_u32 align;
    gst_u8 tail[RGL_STRING_BYTES - sizeof(gst_u32)];
} gst_string_storage;

GST_STATIC_ASSERT(sizeof(gst_string_storage) == RGL_STRING_BYTES,
                  "rglString storage size drift");

static const char str_container[] = "mb_guest_settings";

static const char str_gyro_enabled_check_box[] = "mb_gyro_enabled_check_box";
static const char str_gyro_invert_y_check_box[] = "mb_gyro_invert_y_check_box";
static const char str_menu_swipe_check_box[] = "mb_menu_swipe_check_box";
static const char str_left_handed_check_box[] = "mb_left_handed_check_box";
static const char str_hdr_check_box[] = "mb_hdr_check_box";
static const char str_fxaa_check_box[] = "mb_fxaa_check_box";
static const char str_autoexp_check_box[] = "mb_autoexp_check_box";
static const char str_haptic_check_box[] = "mb_haptic_check_box";
static const char str_menu_aspect_check_box[] = "mb_menu_aspect_check_box";

static const char str_move_sensitivity_text[] = "mb_move_sensitivity_text";
static const char str_gyro_sensitivity_text[] = "mb_gyro_sensitivity_text";
static const char str_gyro_deadzone_text[] = "mb_gyro_deadzone_text";
static const char str_button_scale_text[] = "mb_button_scale_text";
static const char str_button_opacity_text[] = "mb_button_opacity_text";
static const char str_fps_limit_text[] = "mb_fps_limit_text";
static const char str_hdr_brightness_text[] = "mb_hdr_brightness_text";
static const char str_shadow_mode_checkbox[] = "mb_shadow_mode_check_box";
static const char str_map_pan_sensitivity_text[] = "mb_map_pan_sensitivity_text";
static const char str_report_text_scale_text[] = "mb_report_text_scale_text";

static const char str_move_sensitivity_slider[] = "mb_move_sensitivity_slider";
static const char str_gyro_sensitivity_slider[] = "mb_gyro_sensitivity_slider";
static const char str_gyro_deadzone_slider[] = "mb_gyro_deadzone_slider";
static const char str_button_scale_slider[] = "mb_button_scale_slider";
static const char str_button_opacity_slider[] = "mb_button_opacity_slider";
static const char str_hdr_brightness_slider[] = "mb_hdr_brightness_slider";
static const char str_map_pan_sensitivity_slider[] = "mb_map_pan_sensitivity_slider";
static const char str_report_text_scale_slider[] = "mb_report_text_scale_slider";

static const char str_en_tab[] = "Additional";
static const char str_en_gyro_ranged[] =
    "Apply gyroscope to ranged weapons only";
static const char str_en_swipe[] = "Swipe vertically to navigate menus";
static const char str_en_left_handed[] = "Left-handed control layout";
static const char str_en_opacity[] = "Button opacity";
static const char str_en_button_size[] = "Button size";
static const char str_en_move[] = "Movement sensitivity";
static const char str_en_fps[] = "Frame rate limit";
static const char str_en_fps_max[] = "max.";
static const char str_en_gyro[] = "Gyroscope aiming";
static const char str_en_invert[] = "Invert gyroscope vertical axis";
static const char str_en_gyro_sensitivity[] = "Gyroscope sensitivity";
static const char str_en_deadzone[] = "Gyroscope deadzone";
static const char str_en_hdr[] = "HDR";
static const char str_en_fxaa[] = "Edge smoothing (FXAA)";
static const char str_en_autoexp[] = "HDR automatic exposure";
static const char str_en_hdr_brightness[] = "HDR brightness";
static const char str_en_shadow_mode[] = "Foliage shadows";
static const char str_en_haptic[] = "Touch vibration";
static const char str_en_map_pan[] = "Map camera speed";
static const char str_en_menu_aspect[] = "Keep interface proportions (16:9 / 4:3)";
static const char str_en_report_text_scale[] = "Combat report text scale";

static const char str_ru_tab[] = "Дополнительные";
static const char str_ru_gyro_ranged[] =
    "Гироскоп только для дальнобойного оружия";
static const char str_ru_swipe[] = "Листать меню вертикальными свайпами";
static const char str_ru_left_handed[] = "Управление для левшей";
static const char str_ru_opacity[] = "Прозрачность кнопок";
static const char str_ru_button_size[] = "Размер кнопок";
static const char str_ru_move[] = "Чувствительность движения";
static const char str_gyro_ranged_check_box[] = "mb_gyro_ranged_check_box";

static const char str_ru_fps[] = "Лимит кадров";
static const char str_ru_fps_max[] = "макс.";
static const char str_fps_limit_combo[] = "mb_fps_limit_combo";
static const char str_ru_gyro[] = "Прицеливание гироскопом";

static const char str_ru_invert[] = "Инвертировать вертикальную ось гироскопа";
static const char str_ru_gyro_sensitivity[] = "Чувствительность гироскопа";
static const char str_ru_deadzone[] = "Мёртвая зона гироскопа";
static const char str_ru_hdr[] = "HDR";
static const char str_ru_fxaa[] = "Сглаживание краёв (FXAA)";
static const char str_ru_autoexp[] = "Автоэкспозиция HDR";
static const char str_ru_hdr_brightness[] = "Яркость HDR";
static const char str_ru_shadow_mode[] = "Тени листвы";
static const char str_ru_haptic[] = "Вибрация сенсорного управления";
static const char str_ru_map_pan[] = "Скорость камеры карты";
static const char str_ru_menu_aspect[] = "Сохранять пропорции интерфейса (16:9 / 4:3)";
static const char str_ru_report_text_scale[] = "Масштаб боевых сообщений";

static gst_u32 load_word(gst_u32 address) {
    return *(volatile gst_u32*)(address);
}

static void store_word(gst_u32 address, gst_u32 value) {
    *(volatile gst_u32*)(address) = value;
}

static float load_float(gst_u32 address) {
    return *(volatile float*)(address);
}

static void store_float(gst_u32 address, float value) {
    *(volatile float*)(address) = value;
}

static gst_u32 guest_base(gst_u32 anchor_runtime) {
    return anchor_runtime - GST_ACTIVATE_ENTRY_VA;
}

static gst_u32 runtime_va(gst_u32 base, gst_u32 link_va) {
    return base + link_va;
}

static guest_settings_state* state_at(gst_u32 base) {
    return (guest_settings_state*)runtime_va(base, GST_STATE_VA);
}

static const char* runtime_string(gst_u32 base, const char* link_pointer) {
    return (const char*)runtime_va(base, (gst_u32)link_pointer);
}

static volatile float* state_host_float(volatile guest_settings_state* state,
                                        gst_u32 index) {
    if (index < GST_CORE_FLOAT_COUNT) return &state->host_values[index];
    if (index == GST_FLOAT_MAP_PAN_SENSITIVITY) {
        return (volatile float*)&state->reserved[GST_RESERVED_MAP_PAN_HOST_FLOAT];
    }
    if (index == GST_FLOAT_REPORT_TEXT_SCALE) {
        return (volatile float*)&state->reserved[GST_RESERVED_REPORT_SCALE_HOST_FLOAT];
    }
    return (volatile float*)&state->reserved[
        GST_RESERVED_HOST_FLOAT_BASE + index - GST_CORE_FLOAT_COUNT];
}

static volatile float* state_guest_float(volatile guest_settings_state* state,
                                         gst_u32 index) {
    if (index < GST_CORE_FLOAT_COUNT) return &state->guest_values[index];
    if (index == GST_FLOAT_MAP_PAN_SENSITIVITY) {
        return (volatile float*)&state->reserved[GST_RESERVED_MAP_PAN_GUEST_FLOAT];
    }
    if (index == GST_FLOAT_REPORT_TEXT_SCALE) {
        return (volatile float*)&state->reserved[GST_RESERVED_REPORT_SCALE_GUEST_FLOAT];
    }
    return (volatile float*)&state->reserved[
        GST_RESERVED_GUEST_FLOAT_BASE + index - GST_CORE_FLOAT_COUNT];
}

static gst_u32 setting_checkbox(const volatile guest_settings_state* state,
                                gst_u32 bit) {
    if (bit < 5u) return state->checkboxes[bit];
    if (bit == 8u) return state->reserved[GST_RESERVED_HDR_CHECKBOX];
    /* Bit 9 was DOF; its saved value is deliberately ignored. */
    if (bit == 10u) return state->reserved[GST_RESERVED_AUTOEXP_CHECKBOX];
    if (bit == 11u) return state->reserved[GST_RESERVED_HAPTIC_CHECKBOX];
    if (bit == 12u) return state->reserved[GST_RESERVED_MENU_ASPECT_CHECKBOX];
    if (bit == 13u) return state->reserved[GST_RESERVED_FXAA_CHECKBOX];
    return 0u;
}

static gst_u32 setting_slider(const volatile guest_settings_state* state,
                              gst_u32 index) {
    if (index < GST_CORE_FLOAT_COUNT) return state->sliders[index];
    if (index == GST_FLOAT_HDR_BRIGHTNESS) {
        return state->reserved[GST_RESERVED_HDR_BRIGHTNESS_SLIDER];
    }
    if (index == GST_FLOAT_SHADOW_MODE) {
        return state->reserved[GST_RESERVED_SHADOW_MODE_CHECKBOX];
    }
    if (index == GST_FLOAT_MAP_PAN_SENSITIVITY) {
        return state->reserved[GST_RESERVED_MAP_PAN_SLIDER];
    }
    if (index == GST_FLOAT_REPORT_TEXT_SCALE) {
        return state->reserved[GST_RESERVED_REPORT_SCALE_SLIDER];
    }
    return 0u;
}

static void state_defaults(volatile guest_settings_state* state) {
    volatile gst_u32* words = (volatile gst_u32*)state;
    gst_u32 index;
    for (index = 0u; index < GST_STATE_BYTES / sizeof(gst_u32); ++index) {
        words[index] = 0u;
    }
    state->magic = GST_MAGIC;
    state->abi_version = GST_ABI_VERSION;
    state->struct_size = GST_STATE_BYTES;
    state->host_write_seq = 2u;
    state->host_bool_mask = GST_BOOL_MENU_SWIPE |
                            GST_BOOL_GYRO_RANGED_ONLY |
                            GST_BOOL_ALWAYS_RUN |
                            GST_BOOL_AUTO_EXPOSURE |
                            GST_BOOL_HAPTIC_FEEDBACK |
                            GST_BOOL_MENU_ASPECT;
    state->host_values[GST_FLOAT_MOVE_SENSITIVITY] = 1.0f;
    state->host_values[GST_FLOAT_GYRO_SENSITIVITY] = 1.25f;
    state->host_values[GST_FLOAT_GYRO_DEADZONE] = 0.012f;
    state->host_values[GST_FLOAT_BUTTON_SCALE] = 1.0f;
    state->host_values[GST_FLOAT_BUTTON_OPACITY] = 1.0f;
    state->host_values[GST_FLOAT_FPS_LIMIT] = 3.0f;
    *state_host_float(state, GST_FLOAT_HDR_BRIGHTNESS) = 0.75f;
    *state_host_float(state, GST_FLOAT_SHADOW_MODE) = 2.0f;
    *state_host_float(state, GST_FLOAT_MAP_PAN_SENSITIVITY) = 0.70f;
    *state_host_float(state, GST_FLOAT_REPORT_TEXT_SCALE) = 1.0f;
    state->guest_bool_mask = state->host_bool_mask;
    for (index = 0u; index < GST_FLOAT_COUNT; ++index) {
        *state_guest_float(state, index) = *state_host_float(state, index);
    }
}

static int state_valid(const volatile guest_settings_state* state) {
    return state->magic == GST_MAGIC &&
           state->abi_version == GST_ABI_VERSION &&
           state->struct_size == GST_STATE_BYTES;
}

static float clampf(float value, float minimum, float maximum) {
    if (!(value >= minimum)) return minimum;
    if (value > maximum) return maximum;
    return value;
}

static float to_normalized(gst_u32 index, float value) {
    switch (index) {
        case GST_FLOAT_MOVE_SENSITIVITY:
            return (clampf(value, 0.5f, 1.8f) - 0.5f) * 0.7692307692307692f;
        case GST_FLOAT_GYRO_SENSITIVITY:
            return (clampf(value, 0.2f, 4.0f) - 0.2f) * 0.2631578947368421f;
        case GST_FLOAT_GYRO_DEADZONE:
            return clampf(value, 0.0f, 0.06f) * 16.666666666666668f;
        case GST_FLOAT_BUTTON_SCALE:
            return (clampf(value, 0.6f, 1.4f) - 0.6f) * 1.25f;
        case GST_FLOAT_BUTTON_OPACITY:
            return (clampf(value, 0.2f, 1.0f) - 0.2f) * 1.25f;
        case GST_FLOAT_FPS_LIMIT:
            return clampf(value, 0.0f, 3.0f) * 0.3333333333333333f;
        case GST_FLOAT_HDR_BRIGHTNESS:
            return (clampf(value, 0.5f, 1.25f) - 0.5f) * 1.3333333333333333f;
        case GST_FLOAT_SHADOW_MODE:
            return clampf(value, 0.0f, 2.0f) * 0.5f;
        case GST_FLOAT_MAP_PAN_SENSITIVITY:
            return (clampf(value, 0.35f, 1.50f) - 0.35f) * 0.8695652173913043f;
        case GST_FLOAT_REPORT_TEXT_SCALE:
            return clampf(value, 0.5f, 1.5f) - 0.5f;
        default:
            return 0.0f;
    }
}

static float from_normalized(gst_u32 index, float normalized) {
    const float value = clampf(normalized, 0.0f, 1.0f);
    switch (index) {
        case GST_FLOAT_MOVE_SENSITIVITY: return 0.5f + value * 1.3f;
        case GST_FLOAT_GYRO_SENSITIVITY: return 0.2f + value * 3.8f;
        case GST_FLOAT_GYRO_DEADZONE: return value * 0.06f;
        case GST_FLOAT_BUTTON_SCALE: return 0.6f + value * 0.8f;
        case GST_FLOAT_BUTTON_OPACITY: return 0.2f + value * 0.8f;
        case GST_FLOAT_FPS_LIMIT: {
            /* Snap to the discrete 30/60/90/unlimited positions. */
            const float snapped = (float)(gst_u32)(value * 3.0f + 0.5f);
            return snapped > 3.0f ? 3.0f : snapped;
        }
        case GST_FLOAT_HDR_BRIGHTNESS: return 0.5f + value * 0.75f;
        case GST_FLOAT_SHADOW_MODE: {
            const float snapped = (float)(gst_u32)(value * 2.0f + 0.5f);
            return snapped > 2.0f ? 2.0f : snapped;
        }
        case GST_FLOAT_MAP_PAN_SENSITIVITY: return 0.35f + value * 1.15f;
        case GST_FLOAT_REPORT_TEXT_SCALE: return 0.5f + value;
        default: return 0.0f;
    }
}

static gst_u32 language_is_ru(gst_u32 base) {
    const gst_u32 relative_word = load_word(runtime_va(base, VA_LANGUAGE_LITERAL));
    gst_u32 language_address;
    const volatile gst_u8* language;
    if (relative_word == EXPECTED_EN_LANGUAGE_WORD) return 0u;
    language_address = runtime_va(base, VA_LANGUAGE_GOT_BASE) +
                       (gst_u32)(gst_i32)relative_word;
    language = (const volatile gst_u8*)language_address;
    return language[0] == (gst_u8)'r' && language[1] == (gst_u8)'u' &&
           language[2] == 0u;
}

static gst_u32 label_link_va(gst_u32 russian, gst_u32 index) {
    if (russian) {
        switch (index) {
            case 0u: return (gst_u32)str_ru_tab;
            case 1u: return (gst_u32)str_ru_swipe;
            case 2u: return (gst_u32)str_ru_gyro_ranged;
            case 3u: return (gst_u32)str_ru_left_handed;
            case 4u: return (gst_u32)str_ru_opacity;
            case 5u: return (gst_u32)str_ru_button_size;
            case 6u: return (gst_u32)str_ru_move;
            case 7u: return (gst_u32)str_ru_fps;
            case 9u: return (gst_u32)str_ru_gyro;
            case 10u: return (gst_u32)str_ru_invert;
            case 11u: return (gst_u32)str_ru_gyro_sensitivity;
            case 12u: return (gst_u32)str_ru_deadzone;
            case 13u: return (gst_u32)str_ru_hdr;
            case 14u: return (gst_u32)str_ru_fxaa;
            case 15u: return (gst_u32)str_ru_autoexp;
            case 16u: return (gst_u32)str_ru_hdr_brightness;
            case 17u: return (gst_u32)str_ru_shadow_mode;
            case 18u: return (gst_u32)str_ru_haptic;
            case 19u: return (gst_u32)str_ru_map_pan;
            case 20u: return (gst_u32)str_ru_menu_aspect;
            case 21u: return (gst_u32)str_ru_report_text_scale;
            default: return (gst_u32)str_ru_deadzone;
        }
    }
    switch (index) {
        case 0u: return (gst_u32)str_en_tab;
        case 1u: return (gst_u32)str_en_swipe;
        case 2u: return (gst_u32)str_en_gyro_ranged;
        case 3u: return (gst_u32)str_en_left_handed;
        case 4u: return (gst_u32)str_en_opacity;
        case 5u: return (gst_u32)str_en_button_size;
        case 6u: return (gst_u32)str_en_move;
        case 7u: return (gst_u32)str_en_fps;
        case 9u: return (gst_u32)str_en_gyro;
        case 10u: return (gst_u32)str_en_invert;
        case 11u: return (gst_u32)str_en_gyro_sensitivity;
        case 12u: return (gst_u32)str_en_deadzone;
        case 13u: return (gst_u32)str_en_hdr;
        case 14u: return (gst_u32)str_en_fxaa;
        case 15u: return (gst_u32)str_en_autoexp;
        case 16u: return (gst_u32)str_en_hdr_brightness;
        case 17u: return (gst_u32)str_en_shadow_mode;
        case 18u: return (gst_u32)str_en_haptic;
        case 19u: return (gst_u32)str_en_map_pan;
        case 20u: return (gst_u32)str_en_menu_aspect;
        case 21u: return (gst_u32)str_en_report_text_scale;
        default: return (gst_u32)str_en_deadzone;
    }
}

static void string_construct(gst_u32 base, gst_string_storage* object,
                             const char* bytes) {
    ((gst_string_ctor_fn)runtime_va(base, VA_RGL_STRING_CTOR_CSTR))(object, bytes);
}

static void string_destroy(gst_u32 base, gst_string_storage* object) {
    ((gst_string_dtor_fn)runtime_va(base, VA_RGL_STRING_DTOR))(object);
}

static void add_custom_tab_header(gst_u32 base, gst_u32 tab, gst_u32 russian) {
    gst_string_storage label;
    string_construct(base, &label,
                     (const char*)runtime_va(base, label_link_va(russian, 0u)));
    ((gst_tab_add_item_fn)runtime_va(base, VA_TAB_ADD_ITEM))(
        (void*)tab, &label, GST_CUSTOM_TAB_ID);
    string_destroy(base, &label);
}

static void set_widget_inactive(gst_u32 base, gst_u32 widget, int inactive) {
    if (!widget) return;
    if (inactive) {
        ((gst_widget_void_fn)runtime_va(base, VA_WIDGET_HIDE))((void*)widget);
        ((gst_widget_void_fn)runtime_va(base, VA_WIDGET_DISABLE))((void*)widget);
    } else {
        ((gst_widget_void_fn)runtime_va(base, VA_WIDGET_UNHIDE))((void*)widget);
        ((gst_widget_void_fn)runtime_va(base, VA_WIDGET_UNDISABLE))((void*)widget);
    }
}

static void clear_active_widget_highlight(gst_u32 base) {
    ((gst_set_active_widget_highlight_fn)runtime_va(
        base, VA_SET_ACTIVE_WIDGET_HIGHLIGHT))(
            (void*)runtime_va(base, VA_RGL_WINDOW_MANAGER), (void*)0);
}

static void set_tab_active(gst_u32 tab, gst_u32 active_id, int set_requested) {
    gst_u32 begin;
    gst_u32 end;
    gst_u32 slot;
    if (!tab) return;
    store_word(tab + TAB_ACTIVE_ID_OFFSET, active_id);
    if (set_requested) store_word(tab + TAB_REQUESTED_ID_OFFSET, active_id);
    begin = load_word(tab + TAB_ITEMS_BEGIN_OFFSET);
    end = load_word(tab + TAB_ITEMS_END_OFFSET);
    if (!begin || end < begin || (end - begin) > 8u * sizeof(gst_u32) ||
        ((end - begin) & 3u) != 0u) {
        return;
    }
    for (slot = begin; slot < end; slot += sizeof(gst_u32)) {
        const gst_u32 item = load_word(slot);
        gst_u32 vtable;
        gst_u32 setter;
        if (!item) continue;
        vtable = load_word(item);
        if (!vtable) continue;
        setter = load_word(vtable + VTABLE_SET_VALUE_I_OFFSET);
        if (setter) {
            ((gst_set_i_fn)setter)((void*)item,
                                  load_word(item + TAB_ITEM_ID_OFFSET) == active_id);
        }
    }
}

static gst_u32 create_checkbox(gst_u32 base, gst_u32 parent,
                               const char* internal_name, const char* visible_text,
                               float x, float y, float scale) {
    gst_string_storage name_string;
    gst_string_storage visible_string;
    void* result;
    string_construct(base, &name_string, internal_name);
    string_construct(base, &visible_string, visible_text);
    result = ((gst_create_check_box_fn)runtime_va(base, VA_CREATE_CHECK_BOX))(
        (void*)parent, &name_string, &visible_string, &x, &y, &scale);
    string_destroy(base, &visible_string);
    string_destroy(base, &name_string);
    return (gst_u32)result;
}

static gst_u32 create_text(gst_u32 base, gst_u32 parent,
                           const char* internal_name, const char* visible_text,
                           float x, float y, float scale) {
    gst_string_storage name_string;
    gst_string_storage visible_string;
    void* result;
    string_construct(base, &name_string, internal_name);
    string_construct(base, &visible_string, visible_text);
    result = ((gst_create_text_fn)runtime_va(base, VA_CREATE_TEXT))(
        (void*)parent, &name_string, &visible_string, &x, &y, &scale);
    string_destroy(base, &visible_string);
    string_destroy(base, &name_string);
    return (gst_u32)result;
}

static gst_u32 create_slider(gst_u32 base, gst_u32 parent,
                             const char* internal_name,
                             float x, float y, float scale) {
    gst_string_storage name_string;
    void* result;
    string_construct(base, &name_string, internal_name);
    result = ((gst_create_slider_fn)runtime_va(base, VA_CREATE_SLIDER_HOR))(
        (void*)parent, &name_string, &x, &y, &scale);
    string_destroy(base, &name_string);
    return (gst_u32)result;
}

static gst_u32 create_fps_combo(gst_u32 base, gst_u32 parent,
                                const char* internal_name,
                                const char* max_choice_text,
                                float x, float y, float scale) {
    static const char* const numeric_choices[] = {"30", "60", "90"};
    gst_string_storage name_string;
    gst_string_storage choice_string;
    void* result;
    string_construct(base, &name_string, internal_name);
    result = ((gst_create_combo_fn)runtime_va(base, VA_CREATE_COMBO_LABEL))(
        (void*)parent, &name_string, &x, &y, &scale);
    string_destroy(base, &name_string);
    if (!result) return 0u;
    /* -fno-pic code: even the pointer ARRAY lives at a link address, so it
     * must be reached through the load bias before its entries are usable. */
    const char* const* choices = (const char* const*)runtime_va(
        base, (gst_u32)numeric_choices);
    for (gst_u32 index = 0u; index < 3u; ++index) {
        string_construct(base, &choice_string, runtime_string(base, choices[index]));
        ((gst_combo_add_item_fn)runtime_va(base, VA_COMBO_ADD_ITEM))(
            result, &choice_string);
        string_destroy(base, &choice_string);
    }
    string_construct(base, &choice_string, max_choice_text);
    ((gst_combo_add_item_fn)runtime_va(base, VA_COMBO_ADD_ITEM))(
        result, &choice_string);
    string_destroy(base, &choice_string);
    return (gst_u32)result;
}

static void set_widget_y(gst_u32 widget, float y) {
    if (widget) {
        /* Factories store the authored child coordinate at +0x1A8.  The
         * parent then derives the global coordinate at +0x1A0.  Updating only
         * +0x1A0 is overwritten/ignored by the vertical board and leaves the
         * original hole in its child layout.  Move both representations by
         * the same delta, exactly as rglContainer_widget::update_child does. */
        const float old_local = load_float(
            widget + WIDGET_LOCAL_POSITION_Y_OFFSET);
        const float old_global = load_float(widget + WIDGET_POSITION_Y_OFFSET);
        const float delta = y - old_local;
        store_float(widget + WIDGET_LOCAL_POSITION_Y_OFFSET, y);
        store_float(widget + WIDGET_POSITION_Y_OFFSET, old_global + delta);
    }
}

static int read_host_seed(volatile guest_settings_state* state,
                          gst_u32* bool_mask, float values[GST_FLOAT_COUNT],
                          gst_u32* stable_seq) {
    gst_u32 before;
    gst_u32 after;
    gst_u32 index;
    before = state->host_write_seq;
    if ((before & 1u) != 0u || before == state->applied_host_seq) return 0;
    *bool_mask = state->host_bool_mask & GST_BOOL_HOST_WRITABLE_MASK;
    for (index = 0u; index < GST_FLOAT_COUNT; ++index) {
        values[index] = *state_host_float(state, index);
    }
    __asm__ volatile("" ::: "memory");
    after = state->host_write_seq;
    if (after != before || (after & 1u) != 0u) return 0;
    *stable_seq = after;
    return 1;
}

static void apply_values(gst_u32 base, volatile guest_settings_state* state) {
    gst_u32 bool_mask;
    gst_u32 stable_seq;
    gst_u32 index;
    float values[GST_FLOAT_COUNT];
    if (!read_host_seed(state, &bool_mask, values, &stable_seq)) return;
    for (index = 0u; index <= 13u; ++index) {
        const gst_u32 widget = setting_checkbox(state, index);
        if (widget) {
            ((gst_set_i_fn)runtime_va(base, VA_CHECK_BOX_SET_VALUE_I))(
                (void*)widget, (bool_mask >> index) & 1u);
        }
    }
    for (index = 0u; index < GST_FLOAT_COUNT; ++index) {
        const gst_u32 widget = setting_slider(state, index);
        if (!widget) continue;
        if (index == GST_FLOAT_SHADOW_MODE) {
            ((gst_set_i_fn)runtime_va(base, VA_CHECK_BOX_SET_VALUE_I))(
                (void*)widget, values[index] > 0.0f ? 1u : 0u);
            continue;
        }
        if (index == GST_FLOAT_FPS_LIMIT) {
            /* FPS positions render as the native arrow combo control. */
            const float snapped = from_normalized(index, to_normalized(index, values[index]));
            ((gst_set_i_fn)runtime_va(base, VA_COMBO_SET_VALUE_I))(
                (void*)widget, (gst_u32)(snapped + 0.5f));
            continue;
        }
        store_float(widget + SLIDER_MIN_OFFSET, 0.0f);
        store_float(widget + SLIDER_MAX_OFFSET, 1.0f);
        ((gst_set_f_fn)runtime_va(base, VA_WIDGET_SET_VALUE_F))(
            (void*)widget, to_normalized(index, values[index]));
    }
    state->applied_host_seq = stable_seq;
}

static void capture_values(gst_u32 base, volatile guest_settings_state* state) {
    gst_u32 bool_mask = state->host_bool_mask &
        (GST_BOOL_CAMERA_MOUSE | GST_BOOL_ALWAYS_RUN);
    gst_u32 index;
    gst_u32 sequence = state->guest_write_seq;
    (void)base;
    if ((sequence & 1u) != 0u) ++sequence;
    sequence += 1u;
    state->guest_write_seq = sequence;
    __asm__ volatile("" ::: "memory");
    for (index = 0u; index <= 13u; ++index) {
        const gst_u32 widget = setting_checkbox(state, index);
        if (widget && load_word(widget + WIDGET_VALUE_I_OFFSET) != 0u) {
            bool_mask |= 1u << index;
        }
    }
    state->guest_bool_mask = bool_mask;
    for (index = 0u; index < GST_FLOAT_COUNT; ++index) {
        const gst_u32 widget = setting_slider(state, index);
        if (!widget) continue;
        if (index == GST_FLOAT_SHADOW_MODE) {
            *state_guest_float(state, index) =
                load_word(widget + WIDGET_VALUE_I_OFFSET) != 0u ? 2.0f : 0.0f;
            continue;
        }
        if (index == GST_FLOAT_FPS_LIMIT) {
            const gst_u32 maximum = 3u;
            const gst_u32 raw = load_word(widget + WIDGET_VALUE_I_OFFSET);
            const float choice = (float)(raw > maximum ? maximum : raw);
            *state_guest_float(state, index) = from_normalized(
                index, choice / (float)maximum);
            continue;
        }
        *state_guest_float(state, index) = from_normalized(
            index, load_float(widget + WIDGET_VALUE_F_OFFSET));
    }
    __asm__ volatile("" ::: "memory");
    state->guest_write_seq = sequence + 1u;
}

/* Gyro rows (invert checkbox + sensitivity/deadzone sliders and labels) only
 * exist visually while gyroscope aiming is enabled. */
static void update_gyro_rows_visibility(gst_u32 base,
                                        volatile guest_settings_state* state) {
    const gst_u32 gyro_box = state->checkboxes[0u];
    const int enabled = gyro_box && load_word(gyro_box + WIDGET_VALUE_I_OFFSET) != 0u;
    const gst_u32 sensitivity_text = state->reserved[GST_RESERVED_GYRO_TEXT_SENS];
    const gst_u32 deadzone_text = state->reserved[GST_RESERVED_GYRO_TEXT_DEADZONE];
    const int shown = state->reserved[GST_RESERVED_GYRO_ROWS_SHOWN] != 0u;
    if ((enabled != 0) == (shown != 0)) return;
    set_widget_inactive(base, state->checkboxes[3u], !enabled);
    set_widget_inactive(base, state->checkboxes[1u], !enabled);
    set_widget_inactive(base, sensitivity_text, !enabled);
    set_widget_inactive(base, state->sliders[GST_FLOAT_GYRO_SENSITIVITY], !enabled);
    set_widget_inactive(base, deadzone_text, !enabled);
    set_widget_inactive(base, state->sliders[GST_FLOAT_GYRO_DEADZONE], !enabled);
    state->reserved[GST_RESERVED_GYRO_ROWS_SHOWN] = enabled ? 1u : 0u;
    if (state->custom_container) {
        ((gst_widget_void_fn)runtime_va(base, VA_UPDATE_SCROLLABLE_BOARD))(
            (void*)state->custom_container);
    }
}

/* HDR owns exposure/brightness rows; FXAA stays independently available. */
static void update_hdr_rows_visibility(gst_u32 base,
                                       volatile guest_settings_state* state) {
    const gst_u32 hdr_box = state->reserved[GST_RESERVED_HDR_CHECKBOX];
    const int enabled = hdr_box &&
        load_word(hdr_box + WIDGET_VALUE_I_OFFSET) != 0u;
    const int shown = state->reserved[GST_RESERVED_HDR_ROWS_SHOWN] != 0u;
    if ((enabled != 0) == (shown != 0)) return;
    set_widget_inactive(base, state->reserved[GST_RESERVED_HDR_CHECKBOX], 0);
    set_widget_inactive(base, state->reserved[GST_RESERVED_FXAA_CHECKBOX], 0);
    set_widget_inactive(base, state->reserved[GST_RESERVED_AUTOEXP_CHECKBOX],
                         !enabled);
    set_widget_inactive(base,
                        state->reserved[GST_RESERVED_HDR_BRIGHTNESS_TEXT],
                        !enabled);
    set_widget_inactive(base,
                        state->reserved[GST_RESERVED_HDR_BRIGHTNESS_SLIDER],
                        !enabled);
    const float shift = enabled ? 0.0f : 0.08f;
    set_widget_y(state->reserved[GST_RESERVED_FXAA_CHECKBOX], 0.890f + shift);
    set_widget_y(state->reserved[GST_RESERVED_SHADOW_MODE_CHECKBOX],
                 0.850f + shift);
    set_widget_y(state->checkboxes[2u], 0.810f + shift);
    set_widget_y(state->reserved[GST_RESERVED_HAPTIC_CHECKBOX], 0.770f + shift);
    set_widget_y(state->checkboxes[4u], 0.730f + shift);
    set_widget_y(state->reserved[GST_RESERVED_BUTTON_OPACITY_TEXT], 0.690f + shift);
    set_widget_y(state->sliders[GST_FLOAT_BUTTON_OPACITY], 0.688f + shift);
    set_widget_y(state->reserved[GST_RESERVED_BUTTON_SCALE_TEXT], 0.650f + shift);
    set_widget_y(state->sliders[GST_FLOAT_BUTTON_SCALE], 0.648f + shift);
    set_widget_y(state->reserved[GST_RESERVED_MOVE_SENSITIVITY_TEXT], 0.610f + shift);
    set_widget_y(state->sliders[GST_FLOAT_MOVE_SENSITIVITY], 0.608f + shift);
    set_widget_y(state->reserved[GST_RESERVED_MAP_PAN_TEXT], 0.570f + shift);
    set_widget_y(state->reserved[GST_RESERVED_MAP_PAN_SLIDER], 0.568f + shift);
    set_widget_y(state->reserved[GST_RESERVED_FPS_LIMIT_TEXT], 0.530f + shift);
    set_widget_y(state->sliders[GST_FLOAT_FPS_LIMIT], 0.528f + shift);
    set_widget_y(state->reserved[GST_RESERVED_MENU_ASPECT_CHECKBOX], 0.490f + shift);
    set_widget_y(state->reserved[GST_RESERVED_REPORT_SCALE_TEXT], 0.450f + shift);
    set_widget_y(state->reserved[GST_RESERVED_REPORT_SCALE_SLIDER], 0.448f + shift);
    set_widget_y(state->checkboxes[0u], 0.410f + shift);
    set_widget_y(state->checkboxes[3u], 0.370f + shift);
    set_widget_y(state->checkboxes[1u], 0.330f + shift);
    set_widget_y(state->reserved[GST_RESERVED_GYRO_TEXT_SENS], 0.290f + shift);
    set_widget_y(state->sliders[GST_FLOAT_GYRO_SENSITIVITY], 0.288f + shift);
    set_widget_y(state->reserved[GST_RESERVED_GYRO_TEXT_DEADZONE], 0.250f + shift);
    set_widget_y(state->sliders[GST_FLOAT_GYRO_DEADZONE], 0.248f + shift);
    state->reserved[GST_RESERVED_HDR_ROWS_SHOWN] = enabled ? 1u : 0u;
    if (state->custom_container) {
        ((gst_widget_void_fn)runtime_va(base, VA_UPDATE_SCROLLABLE_BOARD))(
            (void*)state->custom_container);
    }
}

static int snapshot_and_hide_siblings(volatile guest_settings_state* state) {
    const gst_u32 options_window = state->options_window;
    const gst_u32 root = state->root;
    const gst_u32 tab = state->tab;
    const gst_u32 custom = state->custom_container;
    const gst_u32 logo = options_window ?
        load_word(options_window + OPTIONS_LOGO_OFFSET) : 0u;
    const gst_u32 done = options_window ?
        load_word(options_window + OPTIONS_DONE_OFFSET) : 0u;
    const gst_u32 frame = tab ? load_word(tab + TAB_PANEL_OFFSET) : 0u;
    gst_u32 begin;
    gst_u32 end;
    gst_u32 slot;
    gst_u32 count = 0u;
    if (!root || !tab || !custom) return 0;
    begin = load_word(root + CONTAINER_CHILD_BEGIN_OFFSET);
    end = load_word(root + CONTAINER_CHILD_END_OFFSET);
    if (!begin || end < begin || ((end - begin) & 3u) != 0u ||
        (end - begin) > 256u * sizeof(gst_u32)) {
        state->error = GST_ERROR_CHILD_VECTOR;
        return 0;
    }
    for (slot = begin; slot < end; slot += sizeof(gst_u32)) {
        const gst_u32 child = load_word(slot);
        if (!child || child == tab || child == custom || child == logo ||
            child == done || child == frame) {
            continue;
        }
        if (count == GST_MAX_SAVED_CHILDREN) {
            gst_u32 rollback;
            state->saved_child_overflow = 1u;
            state->error = GST_ERROR_CHILD_OVERFLOW;
            for (rollback = 0u; rollback < count; ++rollback) {
                const gst_u32 saved = state->saved_children[rollback];
                const gst_u32 flags = load_word(saved + WIDGET_FLAGS_OFFSET);
                store_word(saved + WIDGET_FLAGS_OFFSET,
                           (flags & ~WIDGET_INACTIVE_BITS) |
                           (state->saved_flags[rollback] & WIDGET_INACTIVE_BITS));
            }
            state->saved_child_count = 0u;
            return 0;
        }
        state->saved_children[count] = child;
        state->saved_flags[count] = load_word(child + WIDGET_FLAGS_OFFSET);
        store_word(child + WIDGET_FLAGS_OFFSET,
                   state->saved_flags[count] | WIDGET_INACTIVE_BITS);
        ++count;
    }
    state->saved_child_count = count;
    return 1;
}

static void restore_siblings(volatile guest_settings_state* state) {
    gst_u32 index;
    gst_u32 count = state->saved_child_count;
    if (count > GST_MAX_SAVED_CHILDREN) count = GST_MAX_SAVED_CHILDREN;
    for (index = 0u; index < count; ++index) {
        const gst_u32 child = state->saved_children[index];
        if (child) {
            const gst_u32 current = load_word(child + WIDGET_FLAGS_OFFSET);
            store_word(child + WIDGET_FLAGS_OFFSET,
                       (current & ~WIDGET_INACTIVE_BITS) |
                       (state->saved_flags[index] & WIDGET_INACTIVE_BITS));
        }
    }
    state->saved_child_count = 0u;
}

static int show_custom(gst_u32 base, volatile guest_settings_state* state) {
    if (state->status == GST_STATUS_CUSTOM_ACTIVE) return 1;
    if (state->status != GST_STATUS_READY) return 0;
    if (!snapshot_and_hide_siblings(state)) return 0;
    clear_active_widget_highlight(base);
    apply_values(base, state);
    set_widget_inactive(base, state->custom_container, 0);
    set_tab_active(state->tab, GST_CUSTOM_TAB_ID, 1);
    state->status = GST_STATUS_CUSTOM_ACTIVE;
    state->open_seq += 1u;
    capture_values(base, state);
    return 1;
}

static void leave_custom(gst_u32 base, volatile guest_settings_state* state,
                         int returning_to_general) {
    if (state->status != GST_STATUS_CUSTOM_ACTIVE) return;
    capture_values(base, state);
    clear_active_widget_highlight(base);
    set_widget_inactive(base, state->custom_container, 1);
    restore_siblings(state);
    if (returning_to_general) {
        set_tab_active(state->tab, GST_OPTIONS_WINDOW_ID, 1);
    } else {
        /* Preserve requested id for the pristine handler; only repair its old-id source. */
        set_tab_active(state->tab, GST_OPTIONS_WINDOW_ID, 0);
    }
    state->status = GST_STATUS_READY;
    state->close_seq += 1u;
}

void guest_settings_controls_activate_impl(gst_u32 controls_window,
                                           gst_u32 anchor_runtime) {
    const gst_u32 base = guest_base(anchor_runtime);
    const gst_u32 tab = controls_window ?
        load_word(controls_window + CONTROLS_TAB_OFFSET) : 0u;
    if (!tab) return;
    add_custom_tab_header(base, tab, language_is_ru(base));
    set_tab_active(tab, GST_CONTROLS_WINDOW_ID, 1);
}

gst_u32 guest_settings_controls_check_impl(gst_u32 controls_window,
                                           gst_u32 event_code,
                                           gst_u32 tab_event_result,
                                           gst_u32 anchor_runtime) {
    const gst_u32 base = guest_base(anchor_runtime);
    volatile guest_settings_state* state = state_at(base);
    const gst_u32 tab = controls_window ?
        load_word(controls_window + CONTROLS_TAB_OFFSET) : 0u;
    gst_u32 requested;
    if (!tab) return 0u;
    requested = load_word(tab + TAB_REQUESTED_ID_OFFSET);
    if (requested == GST_CUSTOM_TAB_ID ||
        event_code == GST_NAV_EVENT_NEXT_A ||
        event_code == GST_NAV_EVENT_NEXT_B) {
        if (!state_valid(state)) state_defaults(state);
        state->reserved[GST_RESERVED_PENDING_CUSTOM] = GST_PENDING_CUSTOM_MAGIC;
        /* Never let the pristine dispatcher index the real window 14. */
        store_word(tab + TAB_ACTIVE_ID_OFFSET, GST_CONTROLS_WINDOW_ID);
        store_word(tab + TAB_REQUESTED_ID_OFFSET, GST_OPTIONS_WINDOW_ID);
        return 1u;
    }
    return tab_event_result == 1u ? 1u : 0u;
}

gst_u32 guest_settings_options_nav_impl(gst_u32 options_window,
                                        gst_u32 event_code,
                                        gst_u32 anchor_runtime) {
    const gst_u32 base = guest_base(anchor_runtime);
    volatile guest_settings_state* state = state_at(base);
    if (!state_valid(state) || state->options_window != options_window ||
        !state->tab || state->status != GST_STATUS_CUSTOM_ACTIVE) {
        return GST_NAV_REPLAY;
    }
    if (event_code == GST_NAV_EVENT_PREVIOUS_A ||
        event_code == GST_NAV_EVENT_PREVIOUS_B) {
        leave_custom(base, state, 0);
        store_word(state->tab + TAB_REQUESTED_ID_OFFSET, GST_CONTROLS_WINDOW_ID);
        return GST_NAV_ROUTE_CONTROLS;
    }
    if (event_code == GST_NAV_EVENT_NEXT_A ||
        event_code == GST_NAV_EVENT_NEXT_B) {
        return GST_NAV_CONSUME;
    }
    return GST_NAV_REPLAY;
}

void guest_settings_activate_impl(gst_u32 options_window, gst_u32 anchor_runtime) {
    const gst_u32 base = guest_base(anchor_runtime);
    volatile guest_settings_state* state = state_at(base);
    gst_u32 root;
    gst_u32 tab;
    gst_u32 outer;
    gst_u32 content;
    gst_u32 frame;
    gst_u32 russian;
    gst_u32 index;
    gst_u32 open_pending;
    gst_string_storage temporary;
    const float outer_x = 0.0f;
    const float outer_y = 0.05f;
    const float outer_w = 0.76f;
    const float outer_h = 0.60f;
    if (!state_valid(state)) state_defaults(state);
    open_pending = state->reserved[GST_RESERVED_PENDING_CUSTOM] ==
                   GST_PENDING_CUSTOM_MAGIC;
    state->reserved[GST_RESERVED_PENDING_CUSTOM] = 0u;
    /* Rows are created visible; the visibility pass reconciles them with the
     * gyroscope checkbox on the first tick. */
    state->reserved[GST_RESERVED_GYRO_ROWS_SHOWN] = 1u;
    state->reserved[GST_RESERVED_HDR_ROWS_SHOWN] = 1u;
    state->reserved[GST_RESERVED_GYRO_TEXT_SENS] = 0u;
    state->reserved[GST_RESERVED_GYRO_TEXT_DEADZONE] = 0u;
    state->reserved[GST_RESERVED_HDR_CHECKBOX] = 0u;
    state->reserved[GST_RESERVED_DOF_CHECKBOX] = 0u;
    state->reserved[GST_RESERVED_FXAA_CHECKBOX] = 0u;
    state->reserved[GST_RESERVED_AUTOEXP_CHECKBOX] = 0u;
    state->reserved[GST_RESERVED_HDR_BRIGHTNESS_TEXT] = 0u;
    state->reserved[GST_RESERVED_HDR_BRIGHTNESS_SLIDER] = 0u;
    state->reserved[GST_RESERVED_LEGACY_SHADOW_MODE_TEXT] = 0u;
    state->reserved[GST_RESERVED_SHADOW_MODE_CHECKBOX] = 0u;
    state->reserved[GST_RESERVED_HAPTIC_CHECKBOX] = 0u;
    state->reserved[GST_RESERVED_BUTTON_OPACITY_TEXT] = 0u;
    state->reserved[GST_RESERVED_BUTTON_SCALE_TEXT] = 0u;
    state->reserved[GST_RESERVED_MOVE_SENSITIVITY_TEXT] = 0u;
    state->reserved[GST_RESERVED_FPS_LIMIT_TEXT] = 0u;
    state->reserved[GST_RESERVED_MAP_PAN_TEXT] = 0u;
    state->reserved[GST_RESERVED_MAP_PAN_SLIDER] = 0u;
    state->reserved[GST_RESERVED_MENU_ASPECT_CHECKBOX] = 0u;
    state->reserved[GST_RESERVED_REPORT_SCALE_TEXT] = 0u;
    state->reserved[GST_RESERVED_REPORT_SCALE_SLIDER] = 0u;
    state->status = GST_STATUS_BUILDING;
    state->error = GST_ERROR_NONE;
    state->activation_seq += 1u;
    state->saved_child_count = 0u;
    state->saved_child_overflow = 0u;
    state->applied_host_seq = state->host_write_seq ^ 0xFFFFFFFFu;
    state->custom_container = 0u;
    state->custom_frame = 0u;
    for (index = 0u; index < 6u; ++index) {
        state->checkboxes[index] = 0u;
    }
    for (index = 0u; index < GST_CORE_FLOAT_COUNT; ++index) {
        state->sliders[index] = 0u;
    }
    state->options_window = options_window;
    root = options_window ? load_word(options_window + OPTIONS_ROOT_OFFSET) : 0u;
    tab = options_window ? load_word(options_window + OPTIONS_TAB_OFFSET) : 0u;
    state->root = root;
    state->tab = tab;
    if (!root || !tab) {
        state->error = GST_ERROR_MISSING_OPTIONS_TREE;
        state->status = GST_STATUS_ERROR;
        return;
    }

    russian = language_is_ru(base);
    state->language = russian ? GST_LANGUAGE_RU : GST_LANGUAGE_EN;
    add_custom_tab_header(base, tab, russian);
    set_tab_active(tab, GST_OPTIONS_WINDOW_ID, 1);

    string_construct(base, &temporary, runtime_string(base, str_container));
    outer = (gst_u32)((gst_create_vertical_scrollable_fn)runtime_va(
        base, VA_CREATE_VERTICAL_SCROLLABLE))(
            (void*)root, &temporary, &outer_x, &outer_y, &outer_w, &outer_h);
    string_destroy(base, &temporary);
    if (!outer) {
        state->error = GST_ERROR_ALLOCATION;
        state->status = GST_STATUS_ERROR;
        return;
    }
    /* The factory owns root attachment; make the returned outer inert immediately. */
    state->custom_container = outer;
    set_widget_inactive(base, outer, 1);
    content = load_word(outer + VERTICAL_CONTENT_OFFSET);
    frame = load_word(tab + TAB_PANEL_OFFSET);
    state->custom_frame = frame;
    if (!content || !frame) {
        state->error = GST_ERROR_WIDGET_CREATION;
        state->status = GST_STATUS_ERROR;
        return;
    }

    /* Rows are created top-to-bottom (descending Y), mirroring the pristine
     * Options rows. Gyro controls follow the general settings; gyro tuning
     * rows only appear while aiming is enabled. */
    /* Preserve the original row order. Exposure and brightness depend on HDR;
     * FXAA is independent and replaces the retired DOF row. */
    state->reserved[GST_RESERVED_HDR_CHECKBOX] = create_checkbox(
        base, content, runtime_string(base, str_hdr_check_box),
        (const char*)runtime_va(base, label_link_va(russian, 13u)),
        0.057f, 1.010f, 0.025f);
    if (!state->reserved[GST_RESERVED_HDR_CHECKBOX]) goto widget_creation_failed;

    state->reserved[GST_RESERVED_AUTOEXP_CHECKBOX] = create_checkbox(
        base, content, runtime_string(base, str_autoexp_check_box),
        (const char*)runtime_va(base, label_link_va(russian, 15u)),
        0.057f, 0.970f, 0.025f);
    if (!state->reserved[GST_RESERVED_AUTOEXP_CHECKBOX]) {
        goto widget_creation_failed;
    }

    state->reserved[GST_RESERVED_HDR_BRIGHTNESS_TEXT] = create_text(
        base, content, runtime_string(base, str_hdr_brightness_text),
        (const char*)runtime_va(base, label_link_va(russian, 16u)),
        0.050f, 0.930f, 0.025f);
    if (!state->reserved[GST_RESERVED_HDR_BRIGHTNESS_TEXT]) {
        goto widget_creation_failed;
    }
    state->reserved[GST_RESERVED_HDR_BRIGHTNESS_SLIDER] = create_slider(
        base, content, runtime_string(base, str_hdr_brightness_slider),
        0.433f, 0.928f, 0.020f);
    if (!state->reserved[GST_RESERVED_HDR_BRIGHTNESS_SLIDER]) {
        goto widget_creation_failed;
    }

    /* DOF creation intentionally removed after visual A/B on the device. */
    state->reserved[GST_RESERVED_FXAA_CHECKBOX] = create_checkbox(
        base, content, runtime_string(base, str_fxaa_check_box),
        (const char*)runtime_va(base, label_link_va(russian, 14u)),
        0.057f, 0.890f, 0.025f);
    if (!state->reserved[GST_RESERVED_FXAA_CHECKBOX]) goto widget_creation_failed;

    state->reserved[GST_RESERVED_SHADOW_MODE_CHECKBOX] = create_checkbox(
        base, content, runtime_string(base, str_shadow_mode_checkbox),
        (const char*)runtime_va(base, label_link_va(russian, 17u)),
        0.057f, 0.850f, 0.025f);
    if (!state->reserved[GST_RESERVED_SHADOW_MODE_CHECKBOX]) {
        goto widget_creation_failed;
    }

    state->checkboxes[2u] = create_checkbox(
        base, content, runtime_string(base, str_menu_swipe_check_box),
        (const char*)runtime_va(base, label_link_va(russian, 1u)),
        0.057f, 0.810f, 0.025f);
    if (!state->checkboxes[2u]) goto widget_creation_failed;

    state->reserved[GST_RESERVED_HAPTIC_CHECKBOX] = create_checkbox(
        base, content, runtime_string(base, str_haptic_check_box),
        (const char*)runtime_va(base, label_link_va(russian, 18u)),
        0.057f, 0.770f, 0.025f);
    if (!state->reserved[GST_RESERVED_HAPTIC_CHECKBOX]) {
        goto widget_creation_failed;
    }

    state->checkboxes[4u] = create_checkbox(
        base, content, runtime_string(base, str_left_handed_check_box),
        (const char*)runtime_va(base, label_link_va(russian, 3u)),
        0.057f, 0.730f, 0.025f);
    if (!state->checkboxes[4u]) goto widget_creation_failed;

    state->reserved[GST_RESERVED_BUTTON_OPACITY_TEXT] = create_text(
            base, content, runtime_string(base, str_button_opacity_text),
            (const char*)runtime_va(base, label_link_va(russian, 4u)),
            0.050f, 0.690f, 0.025f);
    if (!state->reserved[GST_RESERVED_BUTTON_OPACITY_TEXT]) {
        goto widget_creation_failed;
    }
    state->sliders[GST_FLOAT_BUTTON_OPACITY] = create_slider(
        base, content, runtime_string(base, str_button_opacity_slider),
        0.433f, 0.688f, 0.020f);
    if (!state->sliders[GST_FLOAT_BUTTON_OPACITY]) goto widget_creation_failed;

    state->reserved[GST_RESERVED_BUTTON_SCALE_TEXT] = create_text(
            base, content, runtime_string(base, str_button_scale_text),
            (const char*)runtime_va(base, label_link_va(russian, 5u)),
            0.050f, 0.650f, 0.025f);
    if (!state->reserved[GST_RESERVED_BUTTON_SCALE_TEXT]) {
        goto widget_creation_failed;
    }
    state->sliders[GST_FLOAT_BUTTON_SCALE] = create_slider(
        base, content, runtime_string(base, str_button_scale_slider),
        0.433f, 0.648f, 0.020f);
    if (!state->sliders[GST_FLOAT_BUTTON_SCALE]) goto widget_creation_failed;

    state->reserved[GST_RESERVED_MOVE_SENSITIVITY_TEXT] = create_text(
            base, content, runtime_string(base, str_move_sensitivity_text),
            (const char*)runtime_va(base, label_link_va(russian, 6u)),
            0.050f, 0.610f, 0.025f);
    if (!state->reserved[GST_RESERVED_MOVE_SENSITIVITY_TEXT]) {
        goto widget_creation_failed;
    }
    state->sliders[GST_FLOAT_MOVE_SENSITIVITY] = create_slider(
        base, content, runtime_string(base, str_move_sensitivity_slider),
        0.433f, 0.608f, 0.020f);
    if (!state->sliders[GST_FLOAT_MOVE_SENSITIVITY]) goto widget_creation_failed;

    state->reserved[GST_RESERVED_MAP_PAN_TEXT] = create_text(
            base, content, runtime_string(base, str_map_pan_sensitivity_text),
            (const char*)runtime_va(base, label_link_va(russian, 19u)),
            0.050f, 0.570f, 0.025f);
    if (!state->reserved[GST_RESERVED_MAP_PAN_TEXT]) {
        goto widget_creation_failed;
    }
    state->reserved[GST_RESERVED_MAP_PAN_SLIDER] = create_slider(
        base, content, runtime_string(base, str_map_pan_sensitivity_slider),
        0.433f, 0.568f, 0.020f);
    if (!state->reserved[GST_RESERVED_MAP_PAN_SLIDER]) {
        goto widget_creation_failed;
    }

    state->reserved[GST_RESERVED_FPS_LIMIT_TEXT] = create_text(
            base, content, runtime_string(base, str_fps_limit_text),
            (const char*)runtime_va(base, label_link_va(russian, 7u)),
            0.050f, 0.530f, 0.025f);
    if (!state->reserved[GST_RESERVED_FPS_LIMIT_TEXT]) {
        goto widget_creation_failed;
    }
    state->sliders[GST_FLOAT_FPS_LIMIT] = create_fps_combo(
        base, content, runtime_string(base, str_fps_limit_combo),
        (const char*)runtime_va(base, russian ? (gst_u32)str_ru_fps_max
                                              : (gst_u32)str_en_fps_max),
        0.402f, 0.528f, 0.020f);
    if (!state->sliders[GST_FLOAT_FPS_LIMIT]) goto widget_creation_failed;

    state->reserved[GST_RESERVED_MENU_ASPECT_CHECKBOX] = create_checkbox(
        base, content, runtime_string(base, str_menu_aspect_check_box),
        (const char*)runtime_va(base, label_link_va(russian, 20u)),
        0.057f, 0.490f, 0.025f);
    if (!state->reserved[GST_RESERVED_MENU_ASPECT_CHECKBOX]) {
        goto widget_creation_failed;
    }

    state->reserved[GST_RESERVED_REPORT_SCALE_TEXT] = create_text(
        base, content, runtime_string(base, str_report_text_scale_text),
        (const char*)runtime_va(base, label_link_va(russian, 21u)),
        0.050f, 0.450f, 0.025f);
    if (!state->reserved[GST_RESERVED_REPORT_SCALE_TEXT]) goto widget_creation_failed;
    state->reserved[GST_RESERVED_REPORT_SCALE_SLIDER] = create_slider(
        base, content, runtime_string(base, str_report_text_scale_slider),
        0.433f, 0.448f, 0.020f);
    if (!state->reserved[GST_RESERVED_REPORT_SCALE_SLIDER]) goto widget_creation_failed;

    state->checkboxes[0u] = create_checkbox(
        base, content, runtime_string(base, str_gyro_enabled_check_box),
        (const char*)runtime_va(base, label_link_va(russian, 9u)),
        0.057f, 0.410f, 0.025f);
    if (!state->checkboxes[0u]) goto widget_creation_failed;

    state->checkboxes[3u] = create_checkbox(
        base, content, runtime_string(base, str_gyro_ranged_check_box),
        (const char*)runtime_va(base, label_link_va(russian, 2u)),
        0.057f, 0.370f, 0.025f);
    if (!state->checkboxes[3u]) goto widget_creation_failed;
    state->checkboxes[1u] = create_checkbox(
        base, content, runtime_string(base, str_gyro_invert_y_check_box),
        (const char*)runtime_va(base, label_link_va(russian, 10u)),
        0.057f, 0.330f, 0.025f);
    if (!state->checkboxes[1u]) goto widget_creation_failed;

    state->reserved[GST_RESERVED_GYRO_TEXT_SENS] = create_text(
        base, content, runtime_string(base, str_gyro_sensitivity_text),
        (const char*)runtime_va(base, label_link_va(russian, 11u)),
        0.050f, 0.290f, 0.025f);
    if (!state->reserved[GST_RESERVED_GYRO_TEXT_SENS]) goto widget_creation_failed;
    state->sliders[GST_FLOAT_GYRO_SENSITIVITY] = create_slider(
        base, content, runtime_string(base, str_gyro_sensitivity_slider),
        0.433f, 0.288f, 0.020f);
    if (!state->sliders[GST_FLOAT_GYRO_SENSITIVITY]) goto widget_creation_failed;

    state->reserved[GST_RESERVED_GYRO_TEXT_DEADZONE] = create_text(
        base, content, runtime_string(base, str_gyro_deadzone_text),
        (const char*)runtime_va(base, label_link_va(russian, 12u)),
        0.050f, 0.250f, 0.025f);
    if (!state->reserved[GST_RESERVED_GYRO_TEXT_DEADZONE]) goto widget_creation_failed;
    state->sliders[GST_FLOAT_GYRO_DEADZONE] = create_slider(
        base, content, runtime_string(base, str_gyro_deadzone_slider),
        0.433f, 0.248f, 0.020f);
    if (!state->sliders[GST_FLOAT_GYRO_DEADZONE]) goto widget_creation_failed;

    ((gst_widget_void_fn)runtime_va(base, VA_UPDATE_SCROLLABLE_BOARD))(
        (void*)outer);
    state->status = GST_STATUS_READY;
    apply_values(base, state);
    capture_values(base, state);
    update_gyro_rows_visibility(base, state);
    update_hdr_rows_visibility(base, state);
    if (open_pending && !show_custom(base, state)) {
        /* One-shot pending requests fail closed to the real Options window. */
        set_tab_active(tab, GST_OPTIONS_WINDOW_ID, 1);
    }
    return;

widget_creation_failed:
    state->error = GST_ERROR_WIDGET_CREATION;
    state->status = GST_STATUS_ERROR;
}

gst_u32 guest_settings_handle_event(gst_u32 options_window, gst_u32 anchor_runtime) {
    const gst_u32 base = guest_base(anchor_runtime);
    volatile guest_settings_state* state = state_at(base);
    const int owns_window = state_valid(state) &&
                            state->options_window == options_window &&
                            state->tab != 0u;
    const gst_u32 tab = owns_window ? state->tab :
        (options_window ? load_word(options_window + OPTIONS_TAB_OFFSET) : 0u);
    gst_u32 requested;
    if (!tab) return 0u;
    requested = load_word(tab + TAB_REQUESTED_ID_OFFSET);
    if (requested == GST_CUSTOM_TAB_ID) {
        if (owns_window && show_custom(base, state)) return 1u;
        /* ID 14 aliases a real window: always consume it and repair to 13. */
        set_tab_active(tab, GST_OPTIONS_WINDOW_ID, 1);
        return 1u;
    }
    if (!owns_window) return 0u;
    if (state->status == GST_STATUS_CUSTOM_ACTIVE) {
        if (requested == GST_OPTIONS_WINDOW_ID) {
            leave_custom(base, state, 1);
            return 1u;
        }
        leave_custom(base, state, 0);
    }
    return 0u;
}

void guest_settings_tick(gst_u32 options_window, gst_u32 anchor_runtime) {
    const gst_u32 base = guest_base(anchor_runtime);
    volatile guest_settings_state* state = state_at(base);
    if (!state_valid(state) || state->options_window != options_window ||
        state->status != GST_STATUS_CUSTOM_ACTIVE) {
        return;
    }
    clear_active_widget_highlight(base);
    apply_values(base, state);
    capture_values(base, state);
    update_gyro_rows_visibility(base, state);
    update_hdr_rows_visibility(base, state);
}
