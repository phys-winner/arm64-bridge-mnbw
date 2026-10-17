#include "guest_settings_protocol.h"

#include <cmath>
#include <cstdio>
#include <limits>

namespace {

struct TestState {
    int checks = 0;
    int failures = 0;
};

#define CHECK(state, condition)                                                   \
    do {                                                                          \
        ++(state).checks;                                                         \
        if (!(condition)) {                                                       \
            ++(state).failures;                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,       \
                         #condition);                                             \
        }                                                                         \
    } while (false)

void TestLayout(TestState& state) {
    CHECK(state, sizeof(guest_settings_state) == 0x400u);
    CHECK(state, GST_OFFSETOF(guest_settings_state, host_write_seq) == 0x24u);
    CHECK(state, GST_OFFSETOF(guest_settings_state, guest_write_seq) == 0x2Cu);
    CHECK(state, GST_OFFSETOF(guest_settings_state, host_values) == 0x78u);
    CHECK(state, GST_OFFSETOF(guest_settings_state, guest_values) == 0x94u);
    CHECK(state, GST_OFFSETOF(guest_settings_state, host_mission_status) == 0x3FCu);
    CHECK(state, GST_STATE_END_VA == 0x0278E800u);
    CHECK(state, GST_BOOL_CAMERA_MOUSE == 0x40u);
    CHECK(state, GST_BOOL_ALWAYS_RUN == 0x80u);
    CHECK(state, GST_BOOL_HDR_ENABLED == 0x100u);
    CHECK(state, GST_BOOL_DOF_ENABLED == 0x200u);
    CHECK(state, GST_BOOL_AUTO_EXPOSURE == 0x400u);
    CHECK(state, GST_BOOL_HAPTIC_FEEDBACK == 0x800u);
    CHECK(state, GST_BOOL_FXAA_ENABLED == 0x2000u);
    CHECK(state, GST_BOOL_MASK_ALL == 0x3FFFu);
    CHECK(state, GST_BOOL_HOST_WRITABLE_MASK == 0x3D9Fu);
    CHECK(state, gst_layout_edit_can_open_quick_battle(
                     GST_MISSION_STATUS_UNKNOWN, 1u) == 0);
    CHECK(state, gst_layout_edit_can_open_quick_battle(
                     GST_MISSION_STATUS_INACTIVE, 0u) == 0);
    CHECK(state, gst_layout_edit_can_open_quick_battle(
                     GST_MISSION_STATUS_INACTIVE, 1u) != 0);
    CHECK(state, gst_layout_edit_can_open_quick_battle(
                     GST_MISSION_STATUS_ACTIVE, 1u) == 0);
    CHECK(state, (GST_BOOL_HOST_WRITABLE_MASK & GST_BOOL_CAMERA_MOUSE) == 0u);
    CHECK(state, (GST_BOOL_HOST_WRITABLE_MASK & GST_BOOL_ALWAYS_RUN) != 0u);
    CHECK(state, (GST_BOOL_HOST_WRITABLE_MASK & GST_BOOL_LAYOUT_EDIT_REQUEST) == 0u);
    CHECK(state, (GST_BOOL_HOST_WRITABLE_MASK & GST_BOOL_HDR_ENABLED) != 0u);
    CHECK(state, (GST_BOOL_HOST_WRITABLE_MASK & GST_BOOL_DOF_ENABLED) == 0u);
    CHECK(state, (GST_BOOL_HOST_WRITABLE_MASK & GST_BOOL_FXAA_ENABLED) != 0u);
    CHECK(state, (warband::guest_settings::kDefaultBoolMask &
                  GST_BOOL_CAMERA_MOUSE) == 0u);
    CHECK(state, (warband::guest_settings::kDefaultBoolMask &
                  GST_BOOL_ALWAYS_RUN) != 0u);
    CHECK(state, (warband::guest_settings::kDefaultBoolMask &
                  GST_BOOL_HAPTIC_FEEDBACK) != 0u);
    CHECK(state, (warband::guest_settings::kDefaultBoolMask &
                  GST_BOOL_FXAA_ENABLED) == 0u);
}

void TestHostBounding(TestState& state) {
    using namespace warband::guest_settings;
    Values input{};
    input.bool_mask = 0xFFFFFFFFu;
    input.floats = {
        -10.0f,                                    /* move -> min */
        10.0f,                                     /* gyro sensitivity -> max */
        std::numeric_limits<float>::quiet_NaN(),   /* deadzone -> default */
        std::numeric_limits<float>::infinity(),    /* scale -> default */
        1.2f,                                      /* opacity -> max */
        2.6f,                                      /* fps -> untouched */
        0.1f,                                      /* HDR brightness -> min */
        9.0f,                                      /* shadow mode -> max */
    };
    const Values bounded = BoundedHostValues(input);
    /* Request bits (layout edit) must never be settable from the host. */
    CHECK(state, bounded.bool_mask == GST_BOOL_HOST_WRITABLE_MASK);
    CHECK(state, bounded.floats[0] == 0.5f);
    CHECK(state, bounded.floats[1] == 4.0f);
    CHECK(state, bounded.floats[2] == 0.012f);
    CHECK(state, bounded.floats[3] == 1.0f);
    CHECK(state, bounded.floats[4] == 1.0f);
    CHECK(state, bounded.floats[5] == 2.6f);
    CHECK(state, bounded.floats[6] == 0.5f);
    CHECK(state, bounded.floats[7] == 2.0f);
    CHECK(state, (bounded.bool_mask & GST_BOOL_CAMERA_MOUSE) == 0u);
    CHECK(state, (bounded.bool_mask & GST_BOOL_HDR_ENABLED) != 0u);
    CHECK(state, (bounded.bool_mask & GST_BOOL_DOF_ENABLED) == 0u);
    CHECK(state, (bounded.bool_mask & GST_BOOL_FXAA_ENABLED) != 0u);
    CHECK(state, IsValidGuestValues(bounded));
    CHECK(state, FpsLimitChoice(2.6f) == 3);
    CHECK(state, FpsLimitValue(FpsLimitChoice(2.6f)) == 0);
    CHECK(state, FpsLimitValue(FpsLimitChoice(2.4f)) == 90);
    CHECK(state, FpsLimitValue(3) == 0);
    CHECK(state, ShadowModeChoice(0.4f) == 0);
    CHECK(state, ShadowModeChoice(1.5f) == 2);
}

void TestGuestValidationIsAtomic(TestState& state) {
    using namespace warband::guest_settings;
    Values valid{};
    CHECK(state, IsValidGuestValues(valid));
    Values bad_mask = valid;
    bad_mask.bool_mask |= 1u << 31;
    CHECK(state, !IsValidGuestValues(bad_mask));
    Values bad_nan = valid;
    bad_nan.floats[GST_FLOAT_GYRO_SENSITIVITY] =
        std::numeric_limits<float>::quiet_NaN();
    CHECK(state, !IsValidGuestValues(bad_nan));
    Values bad_range = valid;
    bad_range.floats[GST_FLOAT_GYRO_DEADZONE] = 0.061f;
    CHECK(state, !IsValidGuestValues(bad_range));
    Values bad_opacity = valid;
    bad_opacity.floats[GST_FLOAT_BUTTON_OPACITY] = 1.01f;
    CHECK(state, !IsValidGuestValues(bad_opacity));
}

void TestSequences(TestState& state) {
    using namespace warband::guest_settings;
    CHECK(state, !IsStablePublishedSequence(0u, 0u));
    CHECK(state, !IsStablePublishedSequence(3u, 3u));
    CHECK(state, !IsStablePublishedSequence(2u, 4u));
    CHECK(state, IsStablePublishedSequence(4u, 4u));
    CHECK(state, NextOddWriteSequence(2u) == 3u);
    CHECK(state, NextOddWriteSequence(3u) == 3u);
    CHECK(state, PublishedSequence(3u) == 4u);
    CHECK(state, PublishedSequence(0xFFFFFFFFu) == 0u);
}

}  // namespace

int main() {
    TestState state;
    TestLayout(state);
    TestHostBounding(state);
    TestGuestValidationIsAtomic(state);
    TestSequences(state);
    std::printf("guest_settings_protocol_test: %s (%d checks)\n",
                state.failures ? "FAIL" : "PASS", state.checks);
    return state.failures ? 1 : 0;
}
