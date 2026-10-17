#include <stdint.h>
#include <stddef.h>

#define GOT_VA 0x003D7128u
#define STATE_VA 0x0278E000u
#define RGL_KEYS_VA 0x0044FAA8u
#define TOUCH_UPDATE_VA 0x002CA348u
#define SET_ACTIVE_RECT_VA 0x002CA170u
#define GET_TIME_MS_VA 0x001D6FE0u

#define POINTER_COUNT 20
#define POINTER_BASE 0x0C14u
#define POINTER_STRIDE 32u

#define KEY_A_OFFSET 384u
#define KEY_Y_OFFSET 400u
#define KEY_RT_OFFSET 416u
#define KEY_LT_OFFSET 420u
#define KEY_START_OFFSET 432u

#define RECT_ATTACK 3
#define RECT_BLOCK 4
#define RECT_INTERACT 5
#define RECT_PAUSE 6

#define TAP_SLOP_SQ 625.0f
#define TAP_MAX_MS 260
#define DOUBLE_TAP_MS 360

typedef struct {
    int32_t type;
    int32_t owner;
    uint8_t active;
    uint8_t padding[3];
    float min_x;
    float min_y;
    float max_x;
    float max_y;
} TouchRect;

typedef struct {
    int32_t owner;
    int32_t history_index;
    int32_t unused_08;
    int32_t rect_index;
    float start_x;
    float start_y;
    float current_x;
    float current_y;
    float history[8];
} TouchState;

typedef struct {
    uint8_t previous_pressed[POINTER_COUNT];
    uint8_t previous_attack;
    uint8_t previous_block;
    uint8_t previous_interact;
    uint8_t previous_pause;
    uint8_t jump_frames;
    uint8_t padding[3];
    float down_x[POINTER_COUNT];
    float down_y[POINTER_COUNT];
    int32_t down_ms[POINTER_COUNT];
    int32_t last_tap_ms;
    int32_t last_tap_pointer;
} ControlState;

typedef void (*TouchUpdateFn)(void *controller);
typedef void (*SetActiveRectFn)(void *controller, int type, float min_x, float min_y, float max_x, float max_y);
typedef int (*GetTimeMsFn)(void);

void mb_controls_update_impl(void *controller, uintptr_t base);

static inline uintptr_t runtime_va(uintptr_t got, uintptr_t va) {
    return got + va - GOT_VA;
}

static inline uint8_t *key_bytes(uintptr_t got) {
    return (uint8_t *)runtime_va(got, RGL_KEYS_VA);
}

static inline uint8_t pointer_pressed(uint8_t *keys, int pointer) {
    return *(volatile uint8_t *)(keys + POINTER_BASE + (uint32_t)pointer * POINTER_STRIDE);
}

static inline float pointer_x(uint8_t *keys, int pointer) {
    return (float)*(volatile int32_t *)(keys + POINTER_BASE + (uint32_t)pointer * POINTER_STRIDE + 4u);
}

static inline float pointer_y(uint8_t *keys, int pointer) {
    return (float)*(volatile int32_t *)(keys + POINTER_BASE + (uint32_t)pointer * POINTER_STRIDE + 8u);
}

static inline void set_key(uint8_t *keys, uint32_t offset, uint8_t active) {
    *(volatile int32_t *)(keys + offset) = active ? 1 : 0;
}

static TouchRect *find_rect(void *controller, int type) {
    TouchRect *begin = *(TouchRect **)((uint8_t *)controller + 8u);
    TouchRect *end = *(TouchRect **)((uint8_t *)controller + 12u);
    for (TouchRect *rect = begin; rect < end; ++rect) {
        if (rect->type == type) return rect;
    }
    return (TouchRect *)0;
}

static void release_look_pointer(void *controller, TouchRect *rect) {
    if (!rect || !rect->active || rect->owner < 0) return;
    TouchState *look = *(TouchState **)((uint8_t *)controller + 4u);
    if (look && look->owner == rect->owner) {
        look->owner = -1;
        look->rect_index = -1;
        look->start_x = -1.0f;
        look->start_y = -1.0f;
        look->current_x = -1.0f;
        look->current_y = -1.0f;
    }
}

static uint8_t rect_active(void *controller, int type) {
    TouchRect *rect = find_rect(controller, type);
    release_look_pointer(controller, rect);
    return rect && rect->active ? 1u : 0u;
}

__attribute__((visibility("default"), noinline, used))
void mb_controls_update_impl(void *controller, uintptr_t got) {
    SetActiveRectFn set_rect = (SetActiveRectFn)runtime_va(got, SET_ACTIVE_RECT_VA);
    TouchUpdateFn original_update = (TouchUpdateFn)runtime_va(got, TOUCH_UPDATE_VA);
    GetTimeMsFn get_time_ms = (GetTimeMsFn)runtime_va(got, GET_TIME_MS_VA);
    uint8_t *keys = key_bytes(got);
    ControlState *state = (ControlState *)runtime_va(got, STATE_VA);

    set_rect(controller, RECT_ATTACK, 0.82f, 0.68f, 0.98f, 0.96f);
    set_rect(controller, RECT_BLOCK, 0.69f, 0.70f, 0.81f, 0.90f);
    set_rect(controller, RECT_INTERACT, 0.84f, 0.48f, 0.98f, 0.65f);
    set_rect(controller, RECT_PAUSE, 0.92f, 0.04f, 0.99f, 0.16f);

    original_update(controller);

    uint8_t attack = rect_active(controller, RECT_ATTACK);
    uint8_t block = rect_active(controller, RECT_BLOCK);
    uint8_t interact = rect_active(controller, RECT_INTERACT);
    uint8_t pause = rect_active(controller, RECT_PAUSE);

    set_key(keys, KEY_RT_OFFSET, attack);
    set_key(keys, KEY_LT_OFFSET, block);
    set_key(keys, KEY_Y_OFFSET, interact);
    set_key(keys, KEY_START_OFFSET, pause);

    state->previous_attack = attack;
    state->previous_block = block;
    state->previous_interact = interact;
    state->previous_pause = pause;

    int32_t now = get_time_ms();
    float half_width = *(volatile float *)runtime_va(got, 0x003E6C38u) * 0.5f;
    for (int pointer = 0; pointer < POINTER_COUNT; ++pointer) {
        uint8_t pressed = pointer_pressed(keys, pointer) ? 1u : 0u;
        float x = pointer_x(keys, pointer);
        float y = pointer_y(keys, pointer);
        if (pressed && !state->previous_pressed[pointer] && x < half_width) {
            state->down_x[pointer] = x;
            state->down_y[pointer] = y;
            state->down_ms[pointer] = now;
        } else if (!pressed && state->previous_pressed[pointer]) {
            float dx = x - state->down_x[pointer];
            float dy = y - state->down_y[pointer];
            int32_t duration = now - state->down_ms[pointer];
            if (state->down_x[pointer] < half_width && duration >= 0 && duration <= TAP_MAX_MS && dx * dx + dy * dy <= TAP_SLOP_SQ) {
                if (state->last_tap_pointer == pointer && now - state->last_tap_ms <= DOUBLE_TAP_MS) {
                    state->jump_frames = 2;
                    state->last_tap_ms = 0;
                    state->last_tap_pointer = -1;
                } else {
                    state->last_tap_ms = now;
                    state->last_tap_pointer = pointer;
                }
            }
        }
        state->previous_pressed[pointer] = pressed;
    }

    if (state->jump_frames) {
        set_key(keys, KEY_A_OFFSET, 1);
        --state->jump_frames;
    } else {
        set_key(keys, KEY_A_OFFSET, 0);
    }
}
