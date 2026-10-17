#include "guest_settings_protocol.h"

/*
 * Compile-only AArch64 check for the future bridge side.  It deliberately has
 * no dependency on production bridge headers and performs no guest-memory I/O.
 */
static_assert(GST_STATE_END_VA == 0x0278E800u);
static_assert(GST_OFFSETOF(guest_settings_state, host_mission_status) == 0x3FCu);
static_assert(GST_MISSION_STATUS_UNKNOWN == 0u);
static_assert(GST_MISSION_STATUS_INACTIVE == 1u);
static_assert(GST_MISSION_STATUS_ACTIVE == 2u);
static_assert(GST_BOOL_MASK_ALL == 0x00003FFFu);
static_assert(GST_BOOL_HOST_WRITABLE_MASK == 0x00003D9Fu);
static_assert((GST_BOOL_HOST_WRITABLE_MASK & GST_BOOL_CAMERA_MOUSE) == 0u);
static_assert((GST_BOOL_HOST_WRITABLE_MASK & GST_BOOL_HDR_ENABLED) != 0u);
static_assert((GST_BOOL_HOST_WRITABLE_MASK & GST_BOOL_DOF_ENABLED) == 0u);
static_assert((GST_BOOL_HOST_WRITABLE_MASK & GST_BOOL_FXAA_ENABLED) != 0u);
static_assert((warband::guest_settings::kDefaultBoolMask &
               GST_BOOL_CAMERA_MOUSE) == 0u);
static_assert(GST_FLOAT_COUNT == 10);
static_assert(GST_ABI_VERSION == 8u);
static_assert(GST_RESERVED_MAP_PAN_HOST_FLOAT == 25u);
static_assert(GST_RESERVED_MAP_PAN_GUEST_FLOAT == 26u);
static_assert(GST_OFFSETOF(guest_settings_state, reserved) +
                  GST_RESERVED_REPORT_SCALE_HOST_FLOAT * 4u == 0x330u);
static_assert(GST_OFFSETOF(guest_settings_state, reserved) +
                  GST_RESERVED_REPORT_SCALE_GUEST_FLOAT * 4u == 0x334u);
static_assert(GST_QUICK_BATTLE_WINDOW_ID == 99u);

extern "C" gst_u32 guest_settings_bridge_layout_probe(
    const guest_settings_state* state) {
    return state ? state->abi_version : 0u;
}
