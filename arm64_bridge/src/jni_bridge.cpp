#include <android/log.h>
#include <android/input.h>
#include <android/native_activity.h>
#include <jni.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <sys/stat.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <dynarmic/interface/A32/a32.h>

#include "apk_reader.h"
#include "bridge_input.h"
#include "camera_look_input.h"
#include "conversation_touch.h"
#include "fmod_host_decode.h"
#include "dynarmic_host.h"
#include "elf_loader.h"
#include "fxaa_present.h"
#include "game_data_vfs.h"
#include "guest_settings_protocol.h"
#include "frame_limit.h"
#include "report_text_scale.h"
#include "input_arbitration.h"
#include "inventory_edge_scroll.h"
#include "inventory_quick_transfer.h"
#include "inventory_scroll_state.h"
#include "inventory_touch_input.h"
#include "map_touch_input.h"
#include "menu_touch_input.h"
#include "mission_button_state.h"
#include "mod_texture_conversion.h"
#include "native_activity_bridge.h"
#include "russian_text_processor.h"
#include "renderer_preferences.h"
#include "scrollbar_drag.h"
#include "text_input_queue.h"
#include "thunk_manager.h"
#include "touch_target_geometry.h"

#define LOG_TAG "WarbandBridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

JavaVM* g_jvm = nullptr;
std::unique_ptr<DynarmicHost> g_host;
std::mutex g_boot_mutex;
std::mutex g_input_mutex;
std::string g_apk_path;
std::string g_game_language = "en";
std::string g_guest_save_root;
std::string g_master_server_mode = "lan";
std::string g_master_server_host;
std::string g_pending_master_server_mode;
std::string g_pending_master_server_host;
bool g_master_server_change_pending = false;
uint32_t g_guest_language_string = 0;
uint32_t g_report_scale_verified_base = std::numeric_limits<uint32_t>::max();
std::array<uint32_t, 2> g_report_scale_expected_words =
    warband::report_text::kOriginalScaleWords;

jclass g_activity_class = nullptr;

uint32_t g_mission_slot = 0;
uint32_t g_add_event = 0;
uint32_t g_guest_keys = 0;
uint32_t g_game_screen = 0;
uint32_t g_cur_presentation_container = 0;
uint32_t g_cur_game_slot = 0;
uint32_t g_window_manager = 0;
uint32_t g_set_active_widget_to_highlight = 0;
uint32_t g_widget_add_event = 0;
uint32_t g_is_over_widget = 0;
uint32_t g_widget_open_tooltip = 0;
uint32_t g_container_clear_events = 0;
uint32_t g_inventory_slot_vtable = 0;
uint32_t g_inventory_menu_vtable = 0;
uint32_t g_inventory_slot_handle_event = 0;
uint32_t g_inventory_menu_handle_event = 0;
uint32_t g_inventory_check_slot = 0;
uint32_t g_inventory_refresh_menus = 0;
uint32_t g_inventory_can_equip = 0;
uint32_t g_inventory_item_kinds = 0;
uint32_t g_inventory_item_kind_count = 0;
uint32_t g_inventory_skills = 0;
uint32_t g_pending_input_toast = 0;
uint32_t g_character_window_vtable = 0;
uint32_t g_simple_button_vtable = 0;
uint32_t g_sheath_weapons = 0;
uint32_t g_application_object = 0;
uint32_t g_pending_character_slot = 0;
bool g_pending_welcome_enter_character = false;
warband::text_input::SessionQueue g_text_input_queue;
uint64_t g_text_submit_token = 0;
uint32_t g_text_submit_tactical = 0;
uint32_t g_text_submit_widget = 0;
bool g_gameplay_state_known = false;
bool g_gameplay_active = false;
// Set by the overlay while the on-screen layout editor is active.
bool g_overlay_edit_mode = false;

struct TacticalUiTouch {
    int32_t pointer_id = -1;
    uint32_t container = 0;
    uint32_t root = 0;
    uint32_t focused = 0;
    float down_x = 0.0f, down_y = 0.0f, x = 0.0f, y = 0.0f;
    bool down_pending = false, release_pending = false, cancelled = false;
    bool semantic_consumed = false;
};
TacticalUiTouch g_tactical_ui_touch;

// NativeActivity only acknowledges an AInputQueue event after the guest asks
// to finish it.  Asset-heavy window transitions can keep the guest away from
// its frame hook for several seconds, so expose a lock-free heartbeat to the
// Java Activity.  The Activity can then consume *new* touch gestures while
// the guest is stalled instead of enqueueing an event that Android will time
// out after five seconds.  Never read guest memory from this query: it runs on
// Android's UI thread precisely while the guest may be busy.
std::atomic<uint64_t> g_guest_frame_heartbeat_ms{0};
constexpr uint64_t kGuestFrameResponsiveWindowMs = 1500;

uint32_t g_virtual_held = 0;
uint32_t g_physical_button_held = 0;
uint32_t g_physical_axis_held = 0;
warband::input::DiscreteArbitrator g_discrete_buttons;
warband::input::MapTouchRecognizer g_map_touch;
warband::input::MapTouchConfig g_map_touch_config;
warband::input::MenuTouchRecognizer g_menu_touch;
warband::input::MenuPulseScheduler g_menu_pulses;
// Tactical orders live in window 10, which the generic menu recognizer
// deliberately excludes.  Keep their confirmation pulse out of the generic
// menu scheduler so a stale/non-menu window binding cannot cancel it.
warband::input::MenuPulseScheduler g_orders_pulses;
warband::input::MenuTouchConfig g_menu_touch_config;
warband::input::ConversationTouchTarget g_conversation_touch;
uint32_t g_conversation_touch_window = 0;
uint32_t g_pending_conversation_window = 0;
uint64_t g_pending_conversation_tap_ms = 0;
bool g_menu_touch_configured = false;
bool g_map_touch_configured = false;
bool g_inventory_touch_configured = false;
warband::guest_settings::Values g_guest_settings_host_values{};
uint64_t g_guest_settings_host_generation = 0;
uint64_t g_guest_settings_seeded_generation = 0;
uint64_t g_guest_settings_pending_serial = 0;
uint32_t g_guest_settings_seeded_activation = 0;
uint32_t g_guest_settings_seen_activation = 0;
uint32_t g_guest_settings_seen_open = 0;
uint32_t g_guest_settings_seen_close = 0;
uint32_t g_guest_settings_previous_window = 0;
warband::guest_settings::Values g_guest_settings_last_snapshot{};
warband::guest_settings::Values g_guest_settings_pending_snapshot{};
bool g_guest_settings_host_configured = false;
bool g_guest_settings_state_seen = false;
bool g_guest_settings_seeded = false;
bool g_guest_settings_previous_window_known = false;
bool g_guest_settings_custom_session = false;
bool g_guest_settings_last_snapshot_valid = false;
bool g_guest_settings_pending_valid = false;
bool g_rgl_config_save_pending = false;
uint32_t g_rgl_config_save_attempts = 0;
// The stock mobile defaults leave manual blocking disabled.  Migrate that
// exact untouched tuple once the guest has initialized rgl_config; do not
// overwrite a player's later choice in the original Options window.
bool g_directional_controls_defaults_checked = false;
std::array<const AInputEvent*, 32> g_observed_input_events{};
// Vertical menu drag pending application on the guest frame thread, in screen
// height fractions.  The target point is the gesture's immutable DOWN anchor,
// so a drag remains bound to its original pane even if the finger leaves it.
// Positive drags scroll content up (finger moves down).
float g_menu_scroll_pending_dy = 0.0f;
float g_menu_scroll_pending_x = 0.0f;
float g_menu_scroll_pending_y = 0.0f;
uint32_t g_menu_scroll_pending_window = std::numeric_limits<uint32_t>::max();
bool g_menu_scroll_pending = false;
// Map gesture deltas arrive on Android's input thread and are consumed by the
// guest frame thread while g_input_mutex is held. Zoom remains wheel-shaped
// because that lets the stock camera retain its distance clamps and easing.
int32_t g_map_zoom_pending = 0;
float g_map_zoom_remainder = 0.0f;
float g_map_pan_pending_x = 0.0f;
float g_map_pan_pending_y = 0.0f;
float g_map_rotation_pending = 0.0f;
float g_map_tilt_pending_pixels = 0.0f;
float g_map_pitch_target = std::numeric_limits<float>::quiet_NaN();
uint32_t g_map_pitch_initialized_window = 0;
bool g_map_compass_tap_pending = false;
bool g_map_interactive_wait_active = false;
bool g_map_cancel_wait_pending = false;
// The stock map issues a movement order on DOWN, before a camera drag can be
// distinguished from a tap. Keep the previous order until the gesture is
// classified, then restore it if the finger was navigating the camera.
struct MapRouteSnapshot {
    uint32_t current_game = 0;
    uint32_t party = 0;
    std::array<uint32_t, 8> behavior{};
    std::array<uint32_t, 5> target{};
    std::array<uint32_t, 14> movement{};
};
MapRouteSnapshot g_map_route_snapshot{};
MapRouteSnapshot g_map_route_candidate{};
bool g_map_route_restore_pending = false;
bool g_map_route_commit_pending = false;
bool g_map_time_running = false;
bool g_map_time_run_in_place = false;
struct MapMarkerAnchor {
    uint32_t current_game = 0;
    uint32_t marker = 0;
    std::array<uint32_t, 3> bounds_position{};
    std::array<uint32_t, 3> transform_position{};
};
MapMarkerAnchor g_map_marker_anchor{};
MapMarkerAnchor g_map_marker_candidate{};
enum class MapRecenterPhase : uint8_t { None = 0, RestoreOrientation };
MapRecenterPhase g_map_recenter_phase = MapRecenterPhase::None;

// Inventory item gestures are classified on DOWN. Empty-space swipes go
// directly to menu scrolling, while an occupied slot is bridge-owned until a
// short details tap, long press, or slop-qualified drag completes.
struct InventoryItemContext {
    uint32_t window = 0, game = 0, troop_table = 0;
    int32_t mode = -1, own_troop = -1, other_troop = -1, troop_count = 0;
    uint32_t bag_count = 0, equipment_count = 0;
    bool operator==(const InventoryItemContext&) const = default;
};

struct InventorySourceRef {
    InventoryItemContext context{};
    int menu = -1, index = -1;
    int32_t kind = -1;
    uint32_t metadata = 0;
};

warband::input::InventoryTapIdentity g_inventory_previous_tap{};
uint64_t g_inventory_previous_tap_ms = 0;

struct InventoryTouchState {
    bool tracking = false;
    bool drag_started = false;
    bool long_press_shown = false;
    bool tap_details_pending = false;
    bool quick_transfer_pending = false;
    bool native_events_sanitized = false;
    bool release_pending = false;
    // The final destination event is injected after the guest frame pass;
    // keep state alive for one extra frame so that pass can consume it.
    bool release_event_queued = false;
    // Records whether at least one source-restore event has been queued.  The
    // event may be reasserted until the guest reports that the drag is clear.
    bool restore_event_queued = false;
    // Level-triggered recovery state.  It remains set until the guest proves
    // that C34 is clear; a timeout must never abandon an item already held by
    // the stock inventory engine.
    bool restore_pending = false;
    bool interrupted = false;
    InventorySourceRef source_ref{};
    InventorySourceRef drop_ref{};
    bool source_resolved = false;
    bool source_event_queued = false;
    bool guest_drag_seen = false;
    // Some mobile inventory paths consult the live guest cursor even after a
    // concrete slot event has been queued.  Keep the cursor over a synthetic
    // follow-up target until the guest consumes that event on the next frame.
    bool guest_cursor_pinned = false;
    // Set only after the original point is confirmed to be an item slot.  A
    // scroll that starts over empty space must continue through the guest's
    // normal touch stream, while an owned drag needs that stream cancelled so
    // it cannot synthesize a second click/release underneath our replayed
    // inventory events.
    bool owns_guest_stream = false;
    int32_t pointer_id = -1;
    float down_x = 0.0f;
    float down_y = 0.0f;
    float last_x = 0.0f;
    float last_y = 0.0f;
    uint64_t down_ms = 0;
    uint64_t source_event_ms = 0;
    uint64_t final_event_ms = 0;
    uint64_t restore_status_log_ms = 0;
    uint32_t cursor_trace_frames = 0;
    uint32_t post_gamepad_trace_frames = 0;
    uint32_t source_slot = 0;
    uint32_t source_menu = 0;
    uint32_t saved_cursor_x = 0;
    uint32_t saved_cursor_y = 0;
    // Picking up or dropping an item makes the stock inventory refresh its
    // slot tree and reset newly built scrollbars to their first row. Preserve
    // the visible position of every pane for the lifetime of the owned drag.
    std::array<warband::input::InventoryScrollbarSnapshot, 16u>
        scrollbar_snapshots{};
    size_t scrollbar_snapshot_count = 0u;
    bool scrollbar_snapshots_captured = false;
    warband::input::InventoryEdgeScrollGateState edge_scroll_gate{};
};
InventoryTouchState g_inventory_touch;
// A completed drop clears InventoryTouchState before the next inventory check.
// Keep one post-release controller pass touch-owned so its cached first-cell
// pointer cannot recreate the green highlight after the item is committed.
bool g_inventory_navigation_cleanup_pending = false;
constexpr uint64_t kInventoryLongPressMs = 480u;
constexpr uint64_t kInventoryGuestStartTimeoutMs = 1000u;
constexpr uint64_t kInventoryRestoreRetryMs = 50u;
constexpr float kInventoryDragSlopMultiplier = 1.25f;
constexpr uint32_t kInventoryRootOffset = 0xB00u;
constexpr uint32_t kWidgetVtableOffset = 0x000u;
constexpr uint32_t kWidgetEventOffset = 0x0D8u;
constexpr uint32_t kWidgetPersistentEventOffset = 0x0DCu;
constexpr uint32_t kWidgetFlagsOffset = 0x1BCu;
// rglWidget_base::render selects +0x2A4 (highlight text colour) instead of
// +0x2A0 (normal text colour) when this byte is non-zero and mark_highlight()
// accepts the widget. rglSimple_button::mark_highlight always accepts it.
constexpr uint32_t kWidgetHighlightFlagOffset = 0x1F2u;
constexpr uint32_t kWidgetChildrenBeginOffset = 0x35Cu;
constexpr uint32_t kWidgetChildrenEndOffset = 0x360u;
constexpr uint32_t kWidgetEventDrag = 0x10000u;
constexpr uint32_t kWidgetEventItemOverlay = 0x00002u;
constexpr uint32_t kWidgetEventClearOverlay = 0x00004u;
constexpr uint32_t kWidgetEventClick = 0x00010u;
constexpr uint32_t kWidgetEventDoubleClick = 0x40000u;
constexpr size_t kInventoryWidgetLimit = 512u;
constexpr uint32_t kWindowManagerCursorXOffset = 0x10u;
constexpr uint32_t kWindowManagerCursorYOffset = 0x14u;
constexpr uint32_t kWindowManagerSurfaceWidthOffset = 0xAB0u;
constexpr uint32_t kWindowManagerSurfaceHeightOffset = 0xAB4u;
constexpr uint32_t kWindowManagerCoordinateScaleOffset = 0xA7Cu;
constexpr uint32_t kWindowManagerActiveHighlightOffset = 0xAD4u;

struct InventoryWidgetSet {
    std::array<uint32_t, kInventoryWidgetLimit> slots{};
    std::array<uint32_t, kInventoryWidgetLimit> menus{};
    size_t slot_count = 0;
    size_t menu_count = 0;
    uint32_t root = 0;
};
InventoryWidgetSet g_inventory_widgets;
// Guest event whose action the guest must observe as CANCEL: its DOWN/UP pair
// straddled a game-screen change and a release would press a phantom widget.
const AInputEvent* g_touch_action_override_event = nullptr;
int32_t g_touch_action_override_value = -1;
// A character-sheet tap may begin just outside a tiny visible '+' button.
// Keep one translation for the complete Android stream so DOWN/MOVE/UP retain
// their native deltas while the guest sees the press centred on that button.
struct CharacterTouchRemapState {
    bool active = false;
    int32_t pointer_id = -1;
    float delta_x = 0.0f;
    float delta_y = 0.0f;
    uint32_t character = 0;
    const AInputEvent* ending_event = nullptr;
};
CharacterTouchRemapState g_character_touch_remap;

enum class UiTouchMode : uint8_t {
    None,
    ScrollbarDrag,
    ConversationResponse,
    MenuChoiceHighlight,
    SyntheticBottomButton,
    MapDeadZone,
};

// Generic desktop-widget touch aid. Scrollbars are bridge-owned drags so a
// guest DOWN/UP cannot rebuild a long list and reset it to the first row.
// Bottom buttons are converted to one exact widget event after swallowing the
// raw map-sensitive stream.
struct UiTouchState {
    UiTouchMode mode = UiTouchMode::None;
    bool active = false;
    bool tap_eligible = false;
    int32_t pointer_id = -1;
    float delta_x = 0.0f;
    float delta_y = 0.0f;
    float down_x = 0.0f;
    float down_y = 0.0f;
    float last_move_x = 0.0f;
    float last_move_y = 0.0f;
    float last_scroll_value = 0.0f;
    bool has_scroll_sample = false;
    uint32_t window_id = std::numeric_limits<uint32_t>::max();
    uint32_t window_object = 0;
    uint32_t widget = 0;
    uint32_t scrollbar_widget = 0;
    uint32_t highlight_widget = 0;
    warband::input::TouchTargetRect scrollbar_track{};
    warband::input::ScrollbarDragModel scrollbar_drag{};
    warband::input::ScrollableContentDragModel content_drag{};
    const AInputEvent* ending_event = nullptr;
};
UiTouchState g_ui_touch;

// A touchscreen interaction takes visual navigation ownership away from the
// stock keyboard/gamepad controller until a physical navigation event arrives
// or the window changes. This prevents the controller's centred cursor from
// leaving an unrelated row green after the finger is released.
constexpr size_t kTouchHighlightTrackedWidgetLimit = 512u;
struct TouchWidgetHighlightState {
    bool active = false;
    uint32_t window_id = std::numeric_limits<uint32_t>::max();
    uint32_t window_object = 0;
    uint32_t applied_widget = 0;
    // The stock controller can leave +0x1F2 set on a response other than its
    // current pointer. Keep the complete live choice set so touch ownership
    // can clear every stale green-text flag, not merely the outlined widget.
    std::array<uint32_t, kTouchHighlightTrackedWidgetLimit> choice_widgets{};
    size_t choice_widget_count = 0u;
};
TouchWidgetHighlightState g_touch_widget_highlight;

// Conversation_window rebuilds its response list during the normal guest
// tick and writes the scrollbar back to its first row. Retain only a
// bridge-owned drag result, keyed to the exact window and choice count, then
// reapply it after each tick until the response set changes or is selected.
struct ConversationScrollRetentionState {
    bool active = false;
    uint32_t window_object = 0;
    uint32_t choice_count = 0;
    uint32_t missing_frames = 0;
    warband::input::InventoryScrollbarSnapshot snapshot{};
};
ConversationScrollRetentionState g_conversation_scroll;
// Keep the controller neutral for one post-release frame so its cached
// desktop focus cannot pull a touch-scrolled response list back into its
// keyboard/gamepad focus band.
bool g_conversation_navigation_cleanup_pending = false;
bool g_ui_button_pending = false;
uint32_t g_ui_button_pending_window = std::numeric_limits<uint32_t>::max();
uint32_t g_ui_button_pending_window_object = 0;
uint32_t g_ui_button_pending_widget = 0;
// Tactical order rows are text widgets rather than rglSimple_button objects.
// A Java-overlay tap is therefore resolved against their live rectangles, then
// committed on the guest frame thread as the stock cursor + D-pad Right path.
bool g_orders_tap_pending = false;
uint32_t g_orders_tap_pending_tactical = 0;
uint32_t g_orders_tap_pending_row = 0;
// Guest VAs resolved once per boot for the menu scroll walker.  A widget whose
// vtable get_type slot points exactly at rglScrollable_widget::get_type is
// treated as a scroll container; the scrollbar get_type check hardens the
// scrollbar-pointer load.
uint32_t g_scrollable_get_type_va = 0;
uint32_t g_scrollbar_get_type_va = 0;
float g_virtual_left_x = 0.0f;
float g_virtual_left_y = 0.0f;
float g_physical_left_x = 0.0f;
float g_physical_left_y = 0.0f;
float g_physical_right_x = 0.0f;
float g_physical_right_y = 0.0f;
float g_pending_look_x = 0.0f;
float g_pending_look_y = 0.0f;
float g_relative_look_remainder_x = 0.0f;
float g_relative_look_remainder_y = 0.0f;
bool g_pending_sheath = false;
bool g_pending_open_orders = false;
bool g_pending_open_chat = false;
uint32_t g_synthetic_chat_key_address = 0;
uint32_t g_reported_discrete_overflows = 0;
uint32_t g_reported_menu_pulse_drops = 0;
constexpr uint32_t kHeldRt = 1u << 0;
constexpr uint32_t kHeldLt = 1u << 1;
constexpr uint32_t kKeyRt = 416u;
constexpr uint32_t kKeyLt = 420u;
constexpr uint32_t kAxisLeftX = 0x0EF4u;
constexpr uint32_t kAxisLeftY = 0x0EF6u;
constexpr uint32_t kAxisRightX = 0x0EF8u;
constexpr uint32_t kAxisRightY = 0x0EFAu;
constexpr uint32_t kMouseDeltaX = 0x0EDCu;
constexpr uint32_t kMouseDeltaY = 0x0EE0u;
constexpr uint32_t kMapWheelDelta = 0x0EE4u;
constexpr uint32_t kControllerState = 0x0C00u;
// rglKeys::add_android_input_event stores the live touchscreen flag in the
// high byte of word_4506AC (rgl_keys + 0xC05).  Inventory_window treats an
// active C34 item plus this flag as an implicit drop on whichever slot is
// under the cursor, even without a widget event.  A bridge-owned drag clears
// only that byte before Inventory_window::check and commits through one exact
// 0x10000 destination event instead.
constexpr uint32_t kTouchscreenActiveFlag = 0x0C05u;
constexpr uint32_t kTacticalCameraDeltaX = 0x0EA0u;
constexpr uint32_t kTacticalCameraDeltaY = 0x0EA4u;
constexpr uint32_t kTacticalWindowId = 10u;
// Tactical_window::handle_user_input (guest 0x2CD1E0..0x2CD1F8) opens the
// root orders panel by publishing this four-field transition.  Feeding the
// desktop D-pad binding is not equivalent in the Android build: that binding
// is owned by the Quests window.
constexpr uint32_t kTacticalOrdersRootWidgetOffset = 0x270u;
constexpr uint32_t kTacticalOrdersFirstRowWidgetOffset = 0x274u;
constexpr uint32_t kTacticalOrdersRowWidgetCount = 8u;
constexpr uint32_t kTacticalOrdersCategoryOffset = 0x5B0u;
constexpr uint32_t kTacticalOrdersGroupOffset = 0x5B4u;
constexpr uint32_t kTacticalOrdersDirtyOffset = 0x5B8u;
constexpr uint32_t kTacticalOrdersCursorOffset = 0x5D0u;
constexpr uint32_t kTacticalOrdersRowCountOffset = 0x5D4u;
// init_game_windows stores Inventory_window in game_screen[1 + 7] and
// Map_window in game_screen[1 + 6].  These IDs are stable in the shipped
// guest and are used only to arbitrate bridge-owned gestures.
constexpr uint32_t kInventoryWindowId = 7u;
constexpr uint32_t kMapWindowId = 6u;
// IDA (shipped ARMv7 guest): Map_window::save_state/load_state and
// Map_window::update_map_camera. These are data fields, not code entrypoints;
// every access is additionally range-checked against the live object.
constexpr uint32_t kMapCenterXOffset = 0x454u;
constexpr uint32_t kMapCenterYOffset = 0x458u;
constexpr uint32_t kMapCameraAngleOffset = 0x474u;
constexpr uint32_t kMapCameraPitchOffset = 0x478u;
constexpr uint32_t kMapCameraDistanceOffset = 0x47Cu;
constexpr uint32_t kMapCameraObjectSpan = 0x480u;
// cur_game is an exported pointer slot. Map_window::get_map_camera_input uses
// these fields to turn off following after manual camera input.
constexpr uint32_t kCurrentGamePlayerPartyOffset = 0x6C8u;
constexpr uint32_t kCurrentGameFollowPartyOffset = 0x6D4u;
constexpr uint32_t kCurrentGameFollowEnabledOffset = 0x6D8u;
constexpr uint32_t kCurrentGameCameraStateSpan = 0x6D9u;
// Operation::run arms an interruptible map wait with a positive duration and
// this flag. Map_window::check cancels it by clearing only the duration.
constexpr uint32_t kCurrentGameWaitRemainingOffset = 0x14330u;
constexpr uint32_t kCurrentGameWaitInteractiveOffset = 0x14338u;
// IDA: Map_window::check resets this mode immediately before Game::tick.
// Game::tick accepts 0=paused, 1=party travel, 2=run time in place.
constexpr uint32_t kCurrentGameMapTimeModeOffset = 82436u;
constexpr uint32_t kCurrentGamePartyCountOffset = 82088u;
constexpr uint32_t kCurrentGamePartyChunksOffset = 82076u;
constexpr uint32_t kPlayerPartyPositionOffset = 0x130u;
constexpr uint32_t kPlayerPartyBehaviorOffset = 704u;
constexpr uint32_t kPlayerPartyTargetOffset = 776u;
constexpr uint32_t kPlayerPartyMovementOffset = 21280u;
constexpr uint32_t kPlayerPartySpan = 21336u;
constexpr uint32_t kCharacterWindowId = 11u;
constexpr uint32_t kNotesWindowId = 21u;
constexpr size_t kUiTouchTargetLimit = 512u;
// IDA: Map_window::check reads its bottom action buttons from this contiguous
// pointer range. Notes_window::check stores the seven bottom category buttons
// at +0x1E0..+0x1F8 (followed by Back/Forward at +0x1FC/+0x200). These windows
// are not rglWidget_base roots, so their action buttons are not reachable by a
// generic children walk; keep the exact shipped layout as a validated fallback.
constexpr uint32_t kMapBottomButtonFirstOffset = 0x1C4u;
constexpr uint32_t kMapBottomButtonLastOffset = 0x1E0u;
// Notes_window::check (0x280CC4) chooses between the two independent
// rglScrollable_widget fields at +0x1D8/+0x1DC according to the pointer x
// coordinate.  They are sibling viewports (right index and left article) and
// are not guaranteed to be reachable through the window's generic child tree.
constexpr uint32_t kNotesScrollableFirstOffset = 0x1D8u;
constexpr uint32_t kNotesScrollableLastOffset = 0x1DCu;
constexpr uint32_t kNotesBottomButtonFirstOffset = 0x1E0u;
constexpr uint32_t kNotesBottomButtonLastOffset = 0x1F8u;
// IDA Character_window::activate stores Done at +0x5BC. Its check routine
// invokes the button's 0x10000 event at 0x2BBA88 before changing screens.
constexpr uint32_t kCharacterDoneButtonOffset = 0x5BCu;
constexpr uint32_t kCharacterSkillCountOffset = 0x45Cu;
constexpr uint32_t kCharacterSkillButtonsOffset = 0x30Cu;
constexpr uint32_t kCharacterAttributeButtonsOffset = 0x5E8u;
constexpr uint32_t kCharacterProficiencyButtonsOffset = 0x620u;
constexpr uint32_t kCharacterAttributeBoardOffset = 0x63Cu;
constexpr uint32_t kCharacterSkillBoardOffset = 0x64Cu;
constexpr size_t kCharacterAttributeButtonCount = 4u;
constexpr size_t kCharacterProficiencyButtonCount = 7u;
constexpr size_t kCharacterSkillButtonLimit = 64u;
constexpr size_t kCharacterTouchTargetLimit =
    kCharacterAttributeButtonCount + kCharacterProficiencyButtonCount +
    kCharacterSkillButtonLimit;
constexpr uint32_t kWidgetPositionXOffset = 0x19Cu;
constexpr uint32_t kWidgetPositionYOffset = 0x1A0u;
constexpr uint32_t kWidgetUpperXOffset = 0x1ACu;
constexpr uint32_t kWidgetUpperYOffset = 0x1B0u;
constexpr uint32_t kWidgetLowerXOffset = 0x1B4u;
constexpr uint32_t kWidgetLowerYOffset = 0x1B8u;
constexpr uint32_t kWindowManagerAspectEnabledOffset = 0x4790u;
constexpr uint32_t kWindowManagerAspectAllowedOffset = 0x4791u;
constexpr uint32_t kWindowManagerFourThreeOffset = 0x4792u;
constexpr uint32_t kWindowManagerAspectBlendOffset = 0xD428u;
constexpr float kMapPinchWheelUnitsPerLog = 18.0f;
// Agent::get_weapon_item (0x5CD20) chain: wielded slot at agent+0x4C0(+4*hand),
// wielded item id at agent + slot*8 + 0x464; item type dword at
// item_kinds_array + 3120*id + 3016 (low byte = item type; 8 bow, 9 crossbow,
// 0xA thrown).  item_kinds (0x177B938) holds the pointer to the array.
constexpr uint32_t kItemKindsPtrImageOffset = 0x177B938u;
constexpr uint32_t kItemStride = 3120u;
constexpr uint32_t kItemTypeFlagsOffset = 3016u;
constexpr uint32_t kAgentWieldedSlotOffset = 0x4C0u;
constexpr uint32_t kAgentWieldedItemIdOffset = 0x464u;
constexpr uint32_t kApplicationObjectImageOffset = 0x02785A10u;
constexpr uint32_t kPendingCharacterObjectOffset = 8u;
constexpr uint32_t kNoActiveWindowId = std::numeric_limits<uint32_t>::max();
uint32_t g_menu_window_id = kNoActiveWindowId;
constexpr uint32_t kGameScreenDequeStartCur = 0x8Cu;
constexpr uint32_t kGameScreenDequeStartFirst = 0x90u;
constexpr uint32_t kGameScreenDequeStartLast = 0x94u;
constexpr uint32_t kGameScreenDequeStartNode = 0x98u;
constexpr uint32_t kGameScreenDequeFinishCur = 0x9Cu;
constexpr uint32_t kGameScreenDequeFinishFirst = 0xA0u;
constexpr uint32_t kGameScreenDequeFinishLast = 0xA4u;
constexpr uint32_t kGameScreenDequeFinishNode = 0xA8u;
constexpr uint32_t kDequeIntBlockBytes = 0x200u;
constexpr uint32_t kLanguageLiteralOffset = 0x000002F8u;
constexpr uint32_t kLanguageGotBaseOffset = 0x00180F54u;
constexpr uint32_t kExpectedLanguageLiteral = 0xFFFCC508u;
constexpr uint32_t kRussianTextProcessorOffset = 0x000B28D4u;
constexpr uint32_t kArmBxLr = 0xE12FFF1Eu;
constexpr uint32_t kRglKeysTickOffset = 0x0025E910u;
constexpr uint32_t kFrameMoveKeysTickCallOffset = 0x00258964u;
constexpr uint32_t kDirectLookStubOffset = 0x003CB000u;
constexpr uint32_t kConversationContinueStubOffset = 0x003CA38Cu;
constexpr uint32_t kExpectedKeysTickCall = 0xEB0017E9u;
// Map_window::check calls Game::tick after handling mouse orders. The wrapper
// can therefore commit or discard a pending route before time advances.
constexpr uint32_t kMapGameTickCallOffset = 0x00322D94u;
constexpr uint32_t kMapGameTickOffset = 0x0032AC88u;
constexpr uint32_t kExpectedMapGameTickCall = 0xEB001FBBu;
constexpr uint32_t kMapGameTickStubOffset = 0x003CDF60u;
constexpr uint32_t kConversationContinueCheckOffset = 0x002B23F0u;
constexpr uint32_t kExpectedConversationContinueCheck = 0x0A000003u;
// Inventory_window::check calls the navigation controller before it consumes
// slot events.  Run a tiny post-call bridge hook there: the stock controller
// must first see touchscreen mode so it clears its green keyboard/gamepad
// highlight instead of selecting the first cell and moving the cursor to it.
constexpr uint32_t kInventoryGamepadUpdateOffset = 0x00127690u;
constexpr uint32_t kSetActiveWidgetHighlightOffset = 0x00214E94u;
constexpr uint32_t kInventoryGamepadUpdateCallOffset = 0x002FF8ECu;
constexpr uint32_t kExpectedInventoryGamepadUpdateCall = 0xEBF89F67u;
constexpr uint32_t kInventoryPostGamepadStubOffset = 0x003CA400u;
// Conversation_window::check runs a desktop focus-band auto-scroll directly
// after the same controller update.  Wrap that call and clear only the
// navigation cursor before the focus-band code can consume it.
constexpr uint32_t kConversationGamepadUpdateCallOffset = 0x002B1970u;
constexpr uint32_t kExpectedConversationGamepadUpdateCall = 0xEBF9D746u;
constexpr uint32_t kConversationPostGamepadStubOffset = 0x003CA430u;
// Menu_window::check calls the same controller for Game_windows::Menu (18).
// Route that call through the shared post-update stub so touch highlighting is
// applied after Presentation_container::tick has assigned hover flags.
constexpr uint32_t kMenuGamepadUpdateCallOffset = 0x00291FC4u;
constexpr uint32_t kExpectedMenuGamepadUpdateCall = 0xEBFA55B1u;
// Initial_window::check (Game_windows::Initial, ID 4) uses the same embedded
// controller.  Sharing the post-update hook moves the original main-menu
// highlight state without introducing a bridge-defined colour.
constexpr uint32_t kInitialGamepadUpdateCallOffset = 0x0031A73Cu;
constexpr uint32_t kExpectedInitialGamepadUpdateCall = 0xEBF833D3u;
// Inventory_window::check_inventory_menu normally erases the item overlay on
// every frame while its C34 "held item" flag is set.  That desktop behaviour
// makes touch dragging blind: the genuine source-slot overlay is built and
// then destroyed in the same check.  Changing only the conditional branch to
// an unconditional branch preserves the overlay while held; the stock leave
// event clears it normally again after the drop.
constexpr uint32_t kInventoryHeldOverlayClearBranchOffset = 0x00301DE0u;
constexpr uint32_t kExpectedInventoryHeldOverlayClearBranch = 0x0A000038u;
constexpr uint32_t kPreserveInventoryHeldOverlayBranch = 0xEA000038u;
constexpr uint32_t kFrameInputStubWords = 30u;
constexpr uint32_t kConversationContinueStubWords = 14u;
constexpr uint32_t kInventoryPostGamepadStubWords = 12u;
constexpr uint32_t kConversationPostGamepadStubWords = 12u;
constexpr uint64_t kConversationTapLifetimeMs = 1000u;
constexpr uint32_t kAgentStride = 24240u;
constexpr uint32_t kMissionAgentsBlocks = 0x18u;
constexpr uint32_t kMissionPlayerAgent = 0x78u;
constexpr float kTriggerThreshold = 0.72f;
constexpr float kAxisFallbackEpsilon = 0.001f;
// Java already caps each touch/gyro sample to 160 logical units.  Cap the
// aggregate too, so a stalled frame or a burst of batched sensor samples cannot
// become a camera snap.  Excess is intentionally not replayed as a stick-like tail.
constexpr float kMaxInjectedLookPerFrame = 160.0f;

struct PhysicalDiscreteKeyBinding {
    int32_t key_code;
    uint32_t mask;
};

// SELECT and Android BACK intentionally share the guest Back action, but keep
// independent physical state so releasing either cannot cancel the other.
constexpr std::array<PhysicalDiscreteKeyBinding, 16> kPhysicalDiscreteKeys = {{
    {AKEYCODE_DPAD_UP, warband::input::kDpadUp},
    {AKEYCODE_DPAD_DOWN, warband::input::kDpadDown},
    {AKEYCODE_DPAD_LEFT, warband::input::kDpadLeft},
    {AKEYCODE_DPAD_RIGHT, warband::input::kDpadRight},
    {AKEYCODE_BUTTON_A, warband::input::kButtonA},
    {AKEYCODE_BUTTON_B, warband::input::kButtonB},
    {AKEYCODE_BUTTON_X, warband::input::kButtonX},
    {AKEYCODE_BUTTON_Y, warband::input::kButtonY},
    {AKEYCODE_BUTTON_L1, warband::input::kButtonL1},
    {AKEYCODE_BUTTON_R1, warband::input::kButtonR1},
    {AKEYCODE_BUTTON_THUMBL, warband::input::kThumbLeft},
    {AKEYCODE_BUTTON_THUMBR, warband::input::kThumbRight},
    {AKEYCODE_BUTTON_START, warband::input::kButtonStart},
    {AKEYCODE_BUTTON_MODE, warband::input::kButtonStart},
    {AKEYCODE_BUTTON_SELECT, warband::input::kButtonBack},
    {AKEYCODE_BACK, warband::input::kButtonBack},
}};
// Allocation-free input-callback storage.  256 simultaneous managed key/device
// pairs exceeds fourteen complete gamepads; saturation fails closed and is logged.
std::array<uint64_t, 256> g_physical_keys_down{};
uint32_t g_physical_key_table_overflows = 0;

constexpr std::array<std::string_view, 12> kSupportedGameLanguages = {
    "cns", "cnt", "cz", "de", "en", "es", "fr", "hu", "ja", "pl", "ru", "tr",
};

bool IsSupportedGameLanguage(std::string_view language) {
    return std::find(kSupportedGameLanguages.begin(), kSupportedGameLanguages.end(), language) !=
           kSupportedGameLanguages.end();
}

constexpr int64_t SignedRelativeWord(uint32_t word) {
    return word <= static_cast<uint32_t>(std::numeric_limits<int32_t>::max())
        ? static_cast<int64_t>(word)
        : static_cast<int64_t>(word) - 0x100000000LL;
}

// Pristine libMBExpMobile.so proof: GOT 0x003D7128 + signed 0xFFFCC508 = "en" at 0x003A3630.
static_assert(SignedRelativeWord(kExpectedLanguageLiteral) == -0x00033AF8LL);
static_assert(0x003D7128LL + SignedRelativeWord(kExpectedLanguageLiteral) == 0x003A3630LL);

bool GuestAddressRange(const DynarmicHost& host, uint64_t address, uint64_t bytes) {
    const uint64_t arena_size = host.GetGuestMemorySize();
    return address <= std::numeric_limits<uint32_t>::max() && address <= arena_size &&
           bytes <= arena_size - address;
}

void InvalidateTextInputForBridgeLifecycleLocked(bool clear_guest_addresses) {
    g_text_input_queue.InvalidateForBridgeLifecycle();
    if (g_text_submit_widget != 0) {
        // Withdraw our held source immediately. Keep the widget marker until
        // an already queued DOWN is suppressed, without blocking fresh text.
        g_discrete_buttons.SetSynthetic(warband::input::kEnterConfirm, false);
        g_text_submit_token = 0;
        if (g_discrete_buttons.PendingSize() == 0 &&
            (g_discrete_buttons.DeliveredHeld() & warband::input::kEnterConfirm) == 0) {
            g_text_submit_tactical = 0;
            g_text_submit_widget = 0;
        }
    }
    if (clear_guest_addresses) {
        g_application_object = 0;
        g_pending_character_slot = 0;
    }
}

void InvalidateTextInputForBridgeLifecycle(bool clear_guest_addresses) {
    std::lock_guard<std::mutex> lock(g_input_mutex);
    InvalidateTextInputForBridgeLifecycleLocked(clear_guest_addresses);
}

bool ApplyGuestLanguage(DynarmicHost& host, std::string_view language) {
    if (!IsSupportedGameLanguage(language)) {
        LOGE("language patch rejected unsupported code '%.*s'",
             static_cast<int>(language.size()), language.data());
        return false;
    }

    const uint32_t initialize = host.FindSymbol("_Z16initialize_stuffPv");
    if (!initialize) {
        LOGE("language patch: initialize_stuff symbol not found");
        return false;
    }
    const uint64_t literal64 = static_cast<uint64_t>(initialize) + kLanguageLiteralOffset;
    const uint64_t got_base64 = static_cast<uint64_t>(initialize) + kLanguageGotBaseOffset;
    if (!GuestAddressRange(host, literal64, sizeof(uint32_t)) ||
        !GuestAddressRange(host, got_base64, 1) || (literal64 & 3u) != 0) {
        LOGE("language patch: derived addresses out of bounds init=0x%08X literal=0x%llX got=0x%llX",
             initialize, static_cast<unsigned long long>(literal64),
             static_cast<unsigned long long>(got_base64));
        return false;
    }

    const uint32_t literal = static_cast<uint32_t>(literal64);
    const uint32_t got_base = static_cast<uint32_t>(got_base64);
    const uint32_t original_word = host.MemoryRead32(literal);
    if (original_word != kExpectedLanguageLiteral) {
        LOGE("language patch guard failed at 0x%08X: expected 0x%08X, got 0x%08X",
             literal, kExpectedLanguageLiteral, original_word);
        return false;
    }

    // The guarded original relative word must resolve to the pristine "en\0" string too.
    const int64_t original_string64 = static_cast<int64_t>(got_base) +
                                      SignedRelativeWord(kExpectedLanguageLiteral);
    if (original_string64 < 0 ||
        !GuestAddressRange(host, static_cast<uint64_t>(original_string64), 3)) {
        LOGE("language patch: guarded English string address is out of bounds");
        return false;
    }
    const uint32_t original_string = static_cast<uint32_t>(original_string64);
    const uint8_t* const memory = host.GetGuestMemoryBase();
    if (memory[original_string] != 'e' || memory[original_string + 1] != 'n' ||
        memory[original_string + 2] != 0) {
        LOGE("language patch: guarded literal does not resolve to English");
        return false;
    }

    const size_t language_bytes = language.size() + 1;
    const uint32_t guest_string = host.GuestAlloc(language_bytes, 8);
    if (!guest_string || !GuestAddressRange(host, guest_string, language_bytes)) {
        if (guest_string) host.GuestFree(guest_string);
        LOGE("language patch: permanent guest string allocation failed");
        return false;
    }
    std::memcpy(host.GetGuestMemoryBase() + guest_string, language.data(), language.size());
    host.MemoryWrite8(guest_string + static_cast<uint32_t>(language.size()), 0);

    const int64_t relative = static_cast<int64_t>(guest_string) -
                             static_cast<int64_t>(got_base);
    if (relative < std::numeric_limits<int32_t>::min() ||
        relative > std::numeric_limits<int32_t>::max()) {
        host.GuestFree(guest_string);
        LOGE("language patch: guest string is outside signed-relative reach");
        return false;
    }
    const uint32_t patched_word = static_cast<uint32_t>(relative);
    if (static_cast<int64_t>(got_base) + SignedRelativeWord(patched_word) != guest_string) {
        host.GuestFree(guest_string);
        LOGE("language patch: signed-relative round trip failed");
        return false;
    }

    host.MemoryWrite32(literal, patched_word);
    if (host.MemoryRead32(literal) != patched_word ||
        std::memcmp(host.GetGuestMemoryBase() + guest_string, language.data(), language.size()) != 0 ||
        host.MemoryRead8(guest_string + static_cast<uint32_t>(language.size())) != 0) {
        host.MemoryWrite32(literal, kExpectedLanguageLiteral);
        host.GuestFree(guest_string);
        LOGE("language patch: write verification failed");
        return false;
    }

    // Intentionally never freed: initialize_stuff consumes this pointer after BootGuest returns.
    g_guest_language_string = guest_string;
    LOGI("language patch applied: %.*s string=0x%08X literal=0x%08X word=0x%08X",
         static_cast<int>(language.size()), language.data(), g_guest_language_string,
         literal, patched_word);
    return true;
}

bool InstallRussianTextProcessorHook(DynarmicHost& host, std::string_view language) {
    // The marker dialect only occurs in the official Russian assets.  Leaving
    // every other language on the pristine guest path also makes this hook
    // incapable of changing their text behavior.
    if (language != "ru") return true;

    constexpr const char* kProcessorSymbol =
        "_ZN10Basic_game22subprocess_text_for_ruER9rglString";
    constexpr const char* kCharProcessorSymbol =
        "_ZN10Basic_game22subprocess_text_for_ruEPc";
    constexpr const char* kThunkName = "__warband_bridge_subprocess_text_for_ru";

    ElfLoader* const game = host.GetLibrary("libMBExpMobile.so");
    if (!game) {
        LOGE("Russian text hook: game image not found");
        return false;
    }
    const uint32_t base = game->GetBaseAddress();
    const uint32_t processor = host.FindSymbol(kProcessorSymbol);
    const uint32_t char_processor = host.FindSymbol(kCharProcessorSymbol);
    const uint64_t expected64 = static_cast<uint64_t>(base) + kRussianTextProcessorOffset;
    if (expected64 > std::numeric_limits<uint32_t>::max() ||
        processor != static_cast<uint32_t>(expected64) ||
        char_processor != processor + sizeof(uint32_t) ||
        kRussianTextProcessorOffset + 2u * sizeof(uint32_t) > game->GetImageSize() ||
        !GuestAddressRange(host, processor, 2u * sizeof(uint32_t)) ||
        (processor & 3u) != 0) {
        LOGE("Russian text hook: address guard failed base=0x%08X processor=0x%08X "
             "char=0x%08X size=0x%08X",
             base, processor, char_processor, game->GetImageSize());
        return false;
    }

    // Both exported overloads are four-byte null stubs in the pristine ARM32
    // game.  The second BX LR is intentionally retained: after the injected SVC
    // it is the return instruction for the first overload.
    if (host.MemoryRead32(processor) != kArmBxLr ||
        host.MemoryRead32(char_processor) != kArmBxLr) {
        LOGE("Russian text hook: null-stub guard failed at 0x%08X: %08X %08X",
             processor, host.MemoryRead32(processor), host.MemoryRead32(char_processor));
        return false;
    }

    ThunkManager& thunks = host.GetThunkManager();
    if (thunks.IsRegistered(kThunkName)) {
        LOGE("Russian text hook: duplicate private thunk");
        return false;
    }
    thunks.RegisterThunk(kThunkName, [](Dynarmic::A32::Jit& jit, DynarmicHost& callback_host) {
        const uint32_t object = jit.Regs()[1];
        bool used_fallback = false;
        const auto status = warband::localization::ProcessGuestRussianText(
            std::span<std::uint8_t>(callback_host.GetGuestMemoryBase(),
                                    callback_host.GetGuestMemorySize()),
            object, &used_fallback);
        if (status == warband::localization::GuestRussianTextStatus::InvalidObject ||
            status == warband::localization::GuestRussianTextStatus::InvalidData ||
            status == warband::localization::GuestRussianTextStatus::InsufficientCapacity) {
            static std::atomic<uint32_t> invalid_reports{0};
            const uint32_t report = invalid_reports.fetch_add(1, std::memory_order_relaxed);
            if (report < 8) {
                LOGE("Russian text hook: rejected rglString=0x%08X status=%u",
                     object, static_cast<unsigned>(status));
            }
        } else if (used_fallback) {
            static std::atomic<uint32_t> fallback_reports{0};
            const uint32_t report = fallback_reports.fetch_add(1, std::memory_order_relaxed);
            if (report < 8) {
                LOGI("Russian text hook: markup-free morphology fallback rglString=0x%08X",
                     object);
            }
        }
    });
    if (!thunks.IsValid()) {
        LOGE("Russian text hook: thunk registration failed");
        return false;
    }
    const uint32_t trampoline = thunks.GetOrCreateThunkAddress(kThunkName);
    if (!trampoline || !GuestAddressRange(host, trampoline, 2u * sizeof(uint32_t))) {
        LOGE("Russian text hook: trampoline address is invalid: 0x%08X", trampoline);
        return false;
    }
    const uint32_t svc = host.MemoryRead32(trampoline);
    if ((svc & 0xFF000000u) != 0xEF000000u ||
        host.MemoryRead32(trampoline + sizeof(uint32_t)) != kArmBxLr) {
        LOGE("Russian text hook: private trampoline guard failed at 0x%08X", trampoline);
        return false;
    }

    host.MemoryWrite32(processor, svc);
    host.InvalidateGuestCode(processor, sizeof(uint32_t));
    if (host.MemoryRead32(processor) != svc || host.MemoryRead32(char_processor) != kArmBxLr) {
        host.MemoryWrite32(processor, kArmBxLr);
        host.InvalidateGuestCode(processor, sizeof(uint32_t));
        LOGE("Russian text hook: write verification failed");
        return false;
    }
    LOGI("Russian text hook installed processor=0x%08X thunk=0x%08X svc=0x%08X",
         processor, trampoline, svc);
    return true;
}

std::string ApplicationInfoString(ANativeActivity* activity, const char* field_name) {
    if (!activity || !activity->env || !activity->clazz || !field_name) return {};
    JNIEnv* env = activity->env;
    jclass activity_cls = env->GetObjectClass(activity->clazz);
    jmethodID get_info = activity_cls
        ? env->GetMethodID(activity_cls, "getApplicationInfo",
                           "()Landroid/content/pm/ApplicationInfo;")
        : nullptr;
    jobject info = get_info ? env->CallObjectMethod(activity->clazz, get_info) : nullptr;
    jclass info_cls = info ? env->GetObjectClass(info) : nullptr;
    jfieldID field = info_cls ? env->GetFieldID(info_cls, field_name, "Ljava/lang/String;") : nullptr;
    jstring value = field ? static_cast<jstring>(env->GetObjectField(info, field)) : nullptr;

    std::string result;
    if (value) {
        const char* utf = env->GetStringUTFChars(value, nullptr);
        if (utf) {
            result = utf;
            env->ReleaseStringUTFChars(value, utf);
        }
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    return result;
}

void ResolveInputSymbols() {
    if (!g_host || (g_guest_keys && g_mission_slot && g_add_event && g_game_screen &&
                    g_cur_presentation_container &&
                    g_cur_game_slot &&
                    g_inventory_check_slot && g_inventory_refresh_menus &&
                    g_inventory_can_equip &&
                    g_inventory_item_kinds && g_inventory_item_kind_count &&
                    g_set_active_widget_to_highlight &&
                    g_widget_open_tooltip &&
                    g_character_window_vtable && g_simple_button_vtable &&
                    g_sheath_weapons &&
                    g_pending_character_slot)) return;
    // rgl_keys is a 3856-byte exported object, not a pointer-valued global.
    g_guest_keys = g_host->FindSymbol("rgl_keys");
    g_mission_slot = g_host->FindSymbol("cur_mission");
    // game_screen is the exported Game_screen object itself, not a pointer slot.
    g_game_screen = g_host->FindSymbol("game_screen");
    // Presentation_container::init stores its presentation table index at +0x0C,
    // and the exported slot points to the currently active container.
    g_cur_presentation_container = g_host->FindSymbol("cur_presentation_container");
    g_cur_game_slot = g_host->FindSymbol("cur_game");
    g_window_manager = g_host->FindSymbol("rgl_window_manager");
    g_set_active_widget_to_highlight = g_host->FindSymbol(
        "_ZN17rglWindow_manager30set_active_widget_to_highlightEP14rglWidget_base");
    g_add_event = g_host->FindSymbol(
        "_ZN7rglKeys23add_android_input_eventE23rglAndroid_input_events");
    g_widget_add_event = g_host->FindSymbol(
        "_ZN14rglWidget_base9add_eventE15rglWindow_event");
    g_is_over_widget = g_host->FindSymbol(
        "_ZN17rglWindow_manager14is_over_widgetEPK14rglWidget_baseRK7rglVec2");
    g_widget_open_tooltip = g_host->FindSymbol(
        "_ZN14rglWidget_base12open_tooltipEv");
    g_container_clear_events = g_host->FindSymbol(
        "_ZN19rglContainer_widget12clear_eventsEv");
    g_inventory_check_slot = g_host->FindSymbol(
        "_ZN16Inventory_window10check_slotER4Itemii");
    g_inventory_refresh_menus = g_host->FindSymbol(
        "_ZN16Inventory_window13refresh_menusEv");
    g_inventory_can_equip = g_host->FindSymbol("_ZNK5Troop9can_equipERK4Item");
    g_inventory_item_kinds = g_host->FindSymbol("item_kinds");
    g_inventory_item_kind_count = g_host->FindSymbol("num_item_kinds");
    g_inventory_skills = g_host->FindSymbol("skills");
    g_sheath_weapons = g_host->FindSymbol("_ZN5Agent14sheath_weaponsEv");
    g_application_object = 0;
    g_pending_character_slot = 0;
    if (ElfLoader* game = g_host->GetLibrary("libMBExpMobile.so")) {
        // Exact get_type function addresses admit widgets into the menu scroll
        // walker without any guest-side virtual call.
        g_scrollable_get_type_va = game->GetBaseAddress() + 0x1BD1E4u;
        g_scrollbar_get_type_va = game->GetBaseAddress() + 0x1BDD00u;
        const uint32_t slot_vtable = g_host->FindSymbol("_ZTV24rglInventory_slot_widget");
        const uint32_t menu_vtable = g_host->FindSymbol("_ZTV24rglInventory_menu_widget");
        const uint32_t character_vtable = g_host->FindSymbol("_ZTV16Character_window");
        const uint32_t simple_button_vtable = g_host->FindSymbol("_ZTV16rglSimple_button");
        // C++ objects point eight bytes past the vtable symbol (the two ABI
        // header words precede the first virtual function).
        g_inventory_slot_vtable = slot_vtable ? slot_vtable + 8u : 0u;
        g_inventory_menu_vtable = menu_vtable ? menu_vtable + 8u : 0u;
        g_character_window_vtable = character_vtable ? character_vtable + 8u : 0u;
        g_simple_button_vtable = simple_button_vtable ? simple_button_vtable + 8u : 0u;
        g_inventory_slot_handle_event = g_host->FindSymbol(
            "_ZN24rglInventory_slot_widget12handle_eventE15rglWindow_event");
        g_inventory_menu_handle_event = g_host->FindSymbol(
            "_ZN24rglInventory_menu_widget12handle_eventE15rglWindow_event");

        const uint32_t exported_app = g_host->FindSymbol("app");
        const uint64_t expected_app64 = static_cast<uint64_t>(game->GetBaseAddress()) +
                                        kApplicationObjectImageOffset;
        const uint64_t pending_slot64 = expected_app64 + kPendingCharacterObjectOffset;
        if (expected_app64 <= std::numeric_limits<uint32_t>::max() &&
            pending_slot64 <= std::numeric_limits<uint32_t>::max() &&
            exported_app == static_cast<uint32_t>(expected_app64) &&
            kApplicationObjectImageOffset + kPendingCharacterObjectOffset +
                    sizeof(uint32_t) <= game->GetImageSize() &&
            GuestAddressRange(*g_host, expected_app64,
                              kPendingCharacterObjectOffset + sizeof(uint32_t)) &&
            (pending_slot64 & (alignof(uint32_t) - 1u)) == 0) {
            g_application_object = exported_app;
            g_pending_character_slot = static_cast<uint32_t>(pending_slot64);
        } else {
            LOGE("text input app guard failed: base=0x%08X exported=0x%08X "
                 "expected=0x%llX pending=0x%llX image_size=0x%08X",
                 game->GetBaseAddress(), exported_app,
                 static_cast<unsigned long long>(expected_app64),
                 static_cast<unsigned long long>(pending_slot64), game->GetImageSize());
        }
    }
    LOGI("input symbols: keys=0x%08X mission_slot=0x%08X game_screen=0x%08X "
         "presentation_container=0x%08X "
         "cur_game_slot=0x%08X "
         "window_manager=0x%08X set_highlight=0x%08X add=0x%08X "
         "widget_add=0x%08X hit_test=0x%08X "
         "open_tooltip=0x%08X "
         "slot_vtable=0x%08X menu_vtable=0x%08X char_vtable=0x%08X "
         "simple_button_vtable=0x%08X slot_event=0x%08X menu_event=0x%08X "
         "sheath=0x%08X app=0x%08X pending_char=0x%08X",
         g_guest_keys, g_mission_slot, g_game_screen, g_cur_presentation_container,
         g_cur_game_slot,
         g_window_manager, g_set_active_widget_to_highlight, g_add_event,
          g_widget_add_event, g_is_over_widget, g_widget_open_tooltip,
          g_inventory_slot_vtable,
          g_inventory_menu_vtable, g_character_window_vtable, g_simple_button_vtable,
          g_inventory_slot_handle_event,
          g_inventory_menu_handle_event, g_sheath_weapons,
          g_application_object,
          g_pending_character_slot);
}

struct ActiveWindowSnapshot {
    uint32_t start_cur;
    uint32_t start_first;
    uint32_t start_last;
    uint32_t start_node;
    uint32_t finish_cur;
    uint32_t finish_first;
    uint32_t finish_last;
    uint32_t finish_node;

    bool operator==(const ActiveWindowSnapshot& other) const = default;
};

bool IsWordAligned(uint32_t address) {
    return (address & (alignof(uint32_t) - 1u)) == 0;
}

bool ValidateActiveWindowSnapshot(DynarmicHost& host,
                                  const ActiveWindowSnapshot& snapshot) {
    const std::array<uint32_t, 8> aligned = {
        snapshot.start_cur, snapshot.start_first, snapshot.start_last,
        snapshot.start_node, snapshot.finish_cur, snapshot.finish_first,
        snapshot.finish_last, snapshot.finish_node,
    };
    if (std::any_of(aligned.begin(), aligned.end(),
                    [](uint32_t address) { return !address || !IsWordAligned(address); })) {
        return false;
    }
    if (static_cast<uint64_t>(snapshot.start_first) + kDequeIntBlockBytes !=
            snapshot.start_last ||
        static_cast<uint64_t>(snapshot.finish_first) + kDequeIntBlockBytes !=
            snapshot.finish_last ||
        snapshot.start_cur < snapshot.start_first ||
        snapshot.start_cur >= snapshot.start_last ||
        snapshot.finish_cur < snapshot.finish_first ||
        snapshot.finish_cur >= snapshot.finish_last ||
        !GuestAddressRange(host, snapshot.start_first, kDequeIntBlockBytes) ||
        !GuestAddressRange(host, snapshot.finish_first, kDequeIntBlockBytes) ||
        !GuestAddressRange(host, snapshot.start_node, sizeof(uint32_t)) ||
        !GuestAddressRange(host, snapshot.finish_node, sizeof(uint32_t))) {
        return false;
    }
    if (host.MemoryRead32(snapshot.start_node) != snapshot.start_first ||
        host.MemoryRead32(snapshot.finish_node) != snapshot.finish_first) {
        return false;
    }
    if (snapshot.start_node == snapshot.finish_node) {
        return snapshot.start_first == snapshot.finish_first &&
               snapshot.start_cur <= snapshot.finish_cur;
    }
    return snapshot.start_node < snapshot.finish_node;
}

bool ReadActiveWindowSnapshot(DynarmicHost& host, uint32_t screen,
                              ActiveWindowSnapshot* snapshot) {
    if (!snapshot || !IsWordAligned(screen) ||
        !GuestAddressRange(host, screen, kGameScreenDequeFinishNode + 4u)) {
        return false;
    }
    *snapshot = {
        host.MemoryRead32(screen + kGameScreenDequeStartCur),
        host.MemoryRead32(screen + kGameScreenDequeStartFirst),
        host.MemoryRead32(screen + kGameScreenDequeStartLast),
        host.MemoryRead32(screen + kGameScreenDequeStartNode),
        host.MemoryRead32(screen + kGameScreenDequeFinishCur),
        host.MemoryRead32(screen + kGameScreenDequeFinishFirst),
        host.MemoryRead32(screen + kGameScreenDequeFinishLast),
        host.MemoryRead32(screen + kGameScreenDequeFinishNode),
    };
    return ValidateActiveWindowSnapshot(host, *snapshot);
}

bool ReadStableActiveWindowId(DynarmicHost& host, uint32_t screen, uint32_t* window_id) {
    ActiveWindowSnapshot before{};
    ActiveWindowSnapshot after{};
    if (!window_id || !ReadActiveWindowSnapshot(host, screen, &before)) return false;

    // Empty deque. Comparing both iterator cursor and node makes the check fail closed even
    // if a guest update is observed between individual 32-bit reads.
    if (before.start_cur == before.finish_cur && before.start_node == before.finish_node) {
        if (!ReadActiveWindowSnapshot(host, screen, &after) || !(before == after)) return false;
        *window_id = kNoActiveWindowId;
        return true;
    }

    uint32_t element = 0;
    if (before.finish_cur != before.finish_first) {
        element = before.finish_cur - sizeof(uint32_t);
    } else {
        if (before.finish_node <= before.start_node ||
            before.finish_node < sizeof(uint32_t) ||
            !GuestAddressRange(host, before.finish_node - sizeof(uint32_t), sizeof(uint32_t))) {
            return false;
        }
        const uint32_t previous_block =
            host.MemoryRead32(before.finish_node - sizeof(uint32_t));
        if (!previous_block || !IsWordAligned(previous_block) ||
            !GuestAddressRange(host, previous_block, kDequeIntBlockBytes)) {
            return false;
        }
        element = previous_block + kDequeIntBlockBytes - sizeof(uint32_t);
    }
    if (!GuestAddressRange(host, element, sizeof(uint32_t))) return false;
    const uint32_t candidate_before = host.MemoryRead32(element);

    if (!ReadActiveWindowSnapshot(host, screen, &after) || !(before == after)) return false;
    // set_active_window can replace the value in-place without moving deque iterators.
    const uint32_t candidate_after = host.MemoryRead32(element);
    if (candidate_before != candidate_after) return false;
    ActiveWindowSnapshot final_snapshot{};
    if (!ReadActiveWindowSnapshot(host, screen, &final_snapshot) ||
        !(after == final_snapshot)) {
        return false;
    }
    *window_id = candidate_after;
    return true;
}

struct GuestSettingsHeaderSnapshot {
    uint32_t magic;
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t status;
    uint32_t error;
    uint32_t activation_seq;
    uint32_t open_seq;
    uint32_t close_seq;
    uint32_t options_window;
};

constexpr uint32_t GuestSettingsField(std::size_t offset) {
    return static_cast<uint32_t>(offset);
}

constexpr uint32_t GuestSettingsHostFloatField(std::size_t index) {
    if (index == GST_FLOAT_REPORT_TEXT_SCALE) {
        return GuestSettingsField(GST_OFFSETOF(guest_settings_state, reserved)) +
               GST_RESERVED_REPORT_SCALE_HOST_FLOAT * sizeof(uint32_t);
    }
    if (index == GST_FLOAT_MAP_PAN_SENSITIVITY) {
        return GuestSettingsField(GST_OFFSETOF(guest_settings_state, reserved)) +
               GST_RESERVED_MAP_PAN_HOST_FLOAT * sizeof(uint32_t);
    }
    return index < GST_CORE_FLOAT_COUNT
        ? GuestSettingsField(GST_OFFSETOF(guest_settings_state, host_values)) +
              static_cast<uint32_t>(index * sizeof(uint32_t))
        : GuestSettingsField(GST_OFFSETOF(guest_settings_state, reserved)) +
              static_cast<uint32_t>((GST_RESERVED_HOST_FLOAT_BASE + index -
                                     GST_CORE_FLOAT_COUNT) * sizeof(uint32_t));
}

constexpr uint32_t GuestSettingsGuestFloatField(std::size_t index) {
    if (index == GST_FLOAT_REPORT_TEXT_SCALE) {
        return GuestSettingsField(GST_OFFSETOF(guest_settings_state, reserved)) +
               GST_RESERVED_REPORT_SCALE_GUEST_FLOAT * sizeof(uint32_t);
    }
    if (index == GST_FLOAT_MAP_PAN_SENSITIVITY) {
        return GuestSettingsField(GST_OFFSETOF(guest_settings_state, reserved)) +
               GST_RESERVED_MAP_PAN_GUEST_FLOAT * sizeof(uint32_t);
    }
    return index < GST_CORE_FLOAT_COUNT
        ? GuestSettingsField(GST_OFFSETOF(guest_settings_state, guest_values)) +
              static_cast<uint32_t>(index * sizeof(uint32_t))
        : GuestSettingsField(GST_OFFSETOF(guest_settings_state, reserved)) +
              static_cast<uint32_t>((GST_RESERVED_GUEST_FLOAT_BASE + index -
                                     GST_CORE_FLOAT_COUNT) * sizeof(uint32_t));
}

static_assert(GuestSettingsHostFloatField(GST_FLOAT_HDR_BRIGHTNESS) == 0x2F4u);
static_assert(GuestSettingsGuestFloatField(GST_FLOAT_HDR_BRIGHTNESS) == 0x2FCu);
static_assert(GuestSettingsHostFloatField(GST_FLOAT_MAP_PAN_SENSITIVITY) == 0x318u);
static_assert(GuestSettingsGuestFloatField(GST_FLOAT_MAP_PAN_SENSITIVITY) == 0x31Cu);
static_assert(GuestSettingsHostFloatField(GST_FLOAT_REPORT_TEXT_SCALE) == 0x330u);
static_assert(GuestSettingsGuestFloatField(GST_FLOAT_REPORT_TEXT_SCALE) == 0x334u);

uint32_t GuestSettingsFloatBits(float value) {
    uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

float GuestSettingsFloatFromBits(uint32_t bits) {
    float value = 0.0f;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void RemoveEmptyGuestRglConfig(const std::string& save_root) {
    if (save_root.empty()) return;
    const std::string path = save_root + "/rgl_config.txt";
    struct stat state {};
    if (lstat(path.c_str(), &state) != 0 || !S_ISREG(state.st_mode) ||
        state.st_size != 0) {
        return;
    }
    // Older bridge builds opened the file with "w" but swallowed every
    // fprintf/fputs call, leaving an empty file that permanently suppressed the
    // guest's create-defaults path. Zero bytes contain no user state to migrate.
    if (unlink(path.c_str()) == 0) {
        LOGI("removed empty legacy rgl_config so the guest can recreate it");
    } else {
        LOGE("unable to remove empty legacy rgl_config (errno %d)", errno);
    }
}

bool PersistGuestRglConfigLocked() {
    if (!g_host || !g_host->IsJitRunning() || g_guest_save_root.empty()) return false;

    const uint32_t open_file = g_host->FindSymbol(
        "_ZN10Basic_game25open_personal_common_fileEPcS0_");
    const uint32_t write_config = g_host->FindSymbol(
        "_ZN9rglConfig5writeEP7__sFILE");
    const uint32_t basic_game = g_host->FindSymbol("basic_game");
    const uint32_t rgl_config = g_host->FindSymbol("rgl_config");
    const uint32_t close_file =
        g_host->GetThunkManager().GetOrCreateThunkAddress("fclose");
    if (!open_file || !write_config || !basic_game || !rgl_config || !close_file) {
        LOGE("rgl_config save symbols are incomplete open=0x%08X write=0x%08X "
             "game=0x%08X config=0x%08X close=0x%08X",
             open_file, write_config, basic_game, rgl_config, close_file);
        return false;
    }

    constexpr char kFilename[] = "rgl_config.txt";
    constexpr char kMode[] = "w";
    const uint32_t guest_filename = g_host->GuestAlloc(sizeof(kFilename), 1);
    const uint32_t guest_mode = g_host->GuestAlloc(sizeof(kMode), 1);
    if (!guest_filename || !guest_mode) {
        if (guest_filename) g_host->GuestFree(guest_filename);
        if (guest_mode) g_host->GuestFree(guest_mode);
        LOGE("rgl_config save could not allocate guest strings");
        return false;
    }
    std::memcpy(g_host->GetGuestMemoryBase() + guest_filename,
                kFilename, sizeof(kFilename));
    std::memcpy(g_host->GetGuestMemoryBase() + guest_mode, kMode, sizeof(kMode));

    const uint32_t stream = g_host->CallGuestNested(
        open_file, {basic_game, guest_filename, guest_mode});
    bool saved = false;
    if (stream) {
        g_host->CallGuestNested(write_config, {rgl_config, stream});
        const int32_t close_result = static_cast<int32_t>(
            g_host->CallGuestNested(close_file, {stream}));
        const std::string path = g_guest_save_root + "/rgl_config.txt";
        struct stat state {};
        saved = close_result == 0 && stat(path.c_str(), &state) == 0 &&
                S_ISREG(state.st_mode) && state.st_size > 0;
        if (!saved) {
            LOGE("rgl_config save verification failed close=%d size=%lld errno=%d",
                 close_result,
                 static_cast<long long>(state.st_size), errno);
        }
    } else {
        LOGE("rgl_config save could not open the personal config file");
    }

    g_host->GuestFree(guest_mode);
    g_host->GuestFree(guest_filename);
    if (saved) LOGI("rgl_config persisted after closing Options");
    return saved;
}

void SetFrameLimitFromGuestSettings(
        const warband::guest_settings::Values& values) {
    warband::frame_limit::SetTargetFps(
        warband::guest_settings::FpsLimitValue(
            warband::guest_settings::FpsLimitChoice(
                values.floats[GST_FLOAT_FPS_LIMIT])));
}

void ResetGuestSettingsSyncLocked(bool clear_host_values) {
    if (clear_host_values) {
        g_guest_settings_host_values = {};
        g_guest_settings_host_generation = 0;
        g_guest_settings_host_configured = false;
    }
    g_guest_settings_seeded_generation = 0;
    ++g_guest_settings_pending_serial;
    if (!g_guest_settings_pending_serial) ++g_guest_settings_pending_serial;
    g_guest_settings_seeded_activation = 0;
    g_guest_settings_seen_activation = 0;
    g_guest_settings_seen_open = 0;
    g_guest_settings_seen_close = 0;
    g_guest_settings_previous_window = 0;
    g_guest_settings_last_snapshot = {};
    g_guest_settings_pending_snapshot = {};
    g_guest_settings_state_seen = false;
    g_guest_settings_seeded = false;
    g_guest_settings_previous_window_known = false;
    g_guest_settings_custom_session = false;
    g_guest_settings_last_snapshot_valid = false;
    g_guest_settings_pending_valid = false;
    g_rgl_config_save_pending = false;
    g_rgl_config_save_attempts = 0;
    g_directional_controls_defaults_checked = false;
}

bool ResolveGuestSettingsStateLocked(uint32_t* state_address) {
    if (!state_address || !g_host) return false;
    ElfLoader* game = g_host->GetLibrary("libMBExpMobile.so");
    if (!game || game->GetImageSize() < GST_STATE_END_VA) return false;
    const uint64_t address64 = static_cast<uint64_t>(game->GetBaseAddress()) + GST_STATE_VA;
    if (!GuestAddressRange(*g_host, address64, GST_STATE_BYTES)) return false;
    *state_address = static_cast<uint32_t>(address64);
    return IsWordAligned(*state_address);
}

GuestSettingsHeaderSnapshot ReadGuestSettingsHeaderWords(uint32_t state_address) {
    return {
        g_host->MemoryRead32(state_address +
            GuestSettingsField(GST_OFFSETOF(guest_settings_state, magic))),
        g_host->MemoryRead32(state_address +
            GuestSettingsField(GST_OFFSETOF(guest_settings_state, abi_version))),
        g_host->MemoryRead32(state_address +
            GuestSettingsField(GST_OFFSETOF(guest_settings_state, struct_size))),
        g_host->MemoryRead32(state_address +
            GuestSettingsField(GST_OFFSETOF(guest_settings_state, status))),
        g_host->MemoryRead32(state_address +
            GuestSettingsField(GST_OFFSETOF(guest_settings_state, error))),
        g_host->MemoryRead32(state_address +
            GuestSettingsField(GST_OFFSETOF(guest_settings_state, activation_seq))),
        g_host->MemoryRead32(state_address +
            GuestSettingsField(GST_OFFSETOF(guest_settings_state, open_seq))),
        g_host->MemoryRead32(state_address +
            GuestSettingsField(GST_OFFSETOF(guest_settings_state, close_seq))),
        g_host->MemoryRead32(state_address +
            GuestSettingsField(GST_OFFSETOF(guest_settings_state, options_window))),
    };
}

bool SameGuestSettingsHeader(const GuestSettingsHeaderSnapshot& left,
                             const GuestSettingsHeaderSnapshot& right) {
    return left.magic == right.magic && left.abi_version == right.abi_version &&
           left.struct_size == right.struct_size && left.status == right.status &&
           left.error == right.error && left.activation_seq == right.activation_seq &&
           left.open_seq == right.open_seq && left.close_seq == right.close_seq &&
           left.options_window == right.options_window;
}

bool ReadGuestSettingsHeaderLocked(uint32_t state_address,
                                   GuestSettingsHeaderSnapshot* snapshot) {
    if (!snapshot || !g_host) return false;
    const GuestSettingsHeaderSnapshot before =
        ReadGuestSettingsHeaderWords(state_address);
    std::atomic_signal_fence(std::memory_order_seq_cst);
    const GuestSettingsHeaderSnapshot after =
        ReadGuestSettingsHeaderWords(state_address);
    if (!SameGuestSettingsHeader(before, after) || before.magic != GST_MAGIC ||
        before.abi_version != GST_ABI_VERSION ||
        before.struct_size != GST_STATE_BYTES || before.error != GST_ERROR_NONE ||
        (before.status != GST_STATUS_READY &&
         before.status != GST_STATUS_CUSTOM_ACTIVE) ||
        !before.options_window || !IsWordAligned(before.options_window) ||
        !GuestAddressRange(*g_host, before.options_window, sizeof(uint32_t))) {
        return false;
    }
    *snapshot = before;
    return true;
}

bool ReadGuestSettingsSnapshotLocked(
        uint32_t state_address, warband::guest_settings::Values* values,
        uint32_t* published_sequence) {
    if (!values || !published_sequence || !g_host) return false;
    const uint32_t sequence_offset =
        GuestSettingsField(GST_OFFSETOF(guest_settings_state, guest_write_seq));
    const uint32_t bool_offset =
        GuestSettingsField(GST_OFFSETOF(guest_settings_state, guest_bool_mask));
    const uint32_t before = g_host->MemoryRead32(state_address + sequence_offset);
    if (!before || (before & 1u) != 0u) return false;

    warband::guest_settings::Values candidate{};
    candidate.bool_mask = g_host->MemoryRead32(state_address + bool_offset) &
                          ~GST_BOOL_CAMERA_MOUSE;
    for (std::size_t index = 0; index < candidate.floats.size(); ++index) {
        candidate.floats[index] = GuestSettingsFloatFromBits(g_host->MemoryRead32(
            state_address + GuestSettingsGuestFloatField(index)));
    }
    std::atomic_signal_fence(std::memory_order_seq_cst);
    const uint32_t after = g_host->MemoryRead32(state_address + sequence_offset);
    if (!warband::guest_settings::IsStablePublishedSequence(before, after) ||
        !warband::guest_settings::IsValidGuestValues(candidate)) {
        return false;
    }
    *values = candidate;
    *published_sequence = after;
    return true;
}

bool WriteGuestSettingsHostSeedLocked(
        uint32_t state_address, const warband::guest_settings::Values& values) {
    if (!g_host) return false;
    const uint32_t sequence_offset =
        GuestSettingsField(GST_OFFSETOF(guest_settings_state, host_write_seq));
    const uint32_t bool_offset =
        GuestSettingsField(GST_OFFSETOF(guest_settings_state, host_bool_mask));
    const uint32_t odd = warband::guest_settings::NextOddWriteSequence(
        g_host->MemoryRead32(state_address + sequence_offset));
    const uint32_t published = warband::guest_settings::PublishedSequence(odd);

    g_host->MemoryWrite32(state_address + sequence_offset, odd);
    std::atomic_signal_fence(std::memory_order_seq_cst);
    g_host->MemoryWrite32(state_address + bool_offset, values.bool_mask);
    for (std::size_t index = 0; index < values.floats.size(); ++index) {
        g_host->MemoryWrite32(
            state_address + GuestSettingsHostFloatField(index),
            GuestSettingsFloatBits(values.floats[index]));
    }
    std::atomic_signal_fence(std::memory_order_seq_cst);
    g_host->MemoryWrite32(state_address + sequence_offset, published);

    bool verified = g_host->MemoryRead32(state_address + sequence_offset) == published &&
                    g_host->MemoryRead32(state_address + bool_offset) == values.bool_mask;
    for (std::size_t index = 0; verified && index < values.floats.size(); ++index) {
        verified = g_host->MemoryRead32(
            state_address + GuestSettingsHostFloatField(index)) ==
            GuestSettingsFloatBits(values.floats[index]);
    }
    if (!verified) {
        // Leave the publication odd so the guest can never consume a partial seed.
        g_host->MemoryWrite32(state_address + sequence_offset, odd);
    }
    return verified;
}

void QueueGuestSettingsCommitLocked(const warband::guest_settings::Values& values) {
    if (g_guest_settings_host_configured &&
        warband::guest_settings::SameValues(values, g_guest_settings_host_values)) {
        return;
    }
    g_guest_settings_pending_snapshot = values;
    g_guest_settings_pending_valid = true;
    ++g_guest_settings_pending_serial;
    if (!g_guest_settings_pending_serial) ++g_guest_settings_pending_serial;
}

uint32_t CurrentMissionStatusForSettingsLocked() {
    if (!g_host || !g_mission_slot ||
        !GuestAddressRange(*g_host, g_mission_slot, sizeof(uint32_t))) {
        return GST_MISSION_STATUS_UNKNOWN;
    }

    const uint32_t mission = g_host->MemoryRead32(g_mission_slot);
    if (!mission) return GST_MISSION_STATUS_INACTIVE;

    const uint64_t player_agent_slot = static_cast<uint64_t>(mission) +
                                       kMissionPlayerAgent;
    if (!GuestAddressRange(*g_host, player_agent_slot, sizeof(uint32_t))) {
        // A non-zero cur_mission value outside the guest arena is stale, not a
        // live Mission object. The title menu leaves exactly this condition.
        return GST_MISSION_STATUS_INACTIVE;
    }

    const int32_t player_agent = static_cast<int32_t>(
        g_host->MemoryRead32(static_cast<uint32_t>(player_agent_slot)));
    return player_agent < 0 ? GST_MISSION_STATUS_INACTIVE
                            : GST_MISSION_STATUS_ACTIVE;
}

void MigrateStockDirectionalControlDefaultsLocked() {
    if (g_directional_controls_defaults_checked || !g_host) return;
    ElfLoader* game = g_host->GetLibrary("libMBExpMobile.so");
    if (!game) return;

    // rgl_config is at guest VA 0x451104.  Options_window::activate maps these
    // fields to Show Attack Direction, Control Attack Direction, and Control
    // Block Direction respectively.  The requested joystick choices are 2
    // for attack and 1 for block.
    constexpr uint32_t kDisplayAttackDirectionOffset = 0x45116Cu;
    constexpr uint32_t kAttackDirectionControlOffset = 0x451174u;
    constexpr uint32_t kDefendDirectionControlOffset = 0x451178u;
    const uint32_t base = game->GetBaseAddress();
    const uint64_t display64 = static_cast<uint64_t>(base) +
                               kDisplayAttackDirectionOffset;
    const uint64_t attack64 = static_cast<uint64_t>(base) +
                              kAttackDirectionControlOffset;
    const uint64_t defend64 = static_cast<uint64_t>(base) +
                              kDefendDirectionControlOffset;
    if (!GuestAddressRange(*g_host, display64, sizeof(uint32_t)) ||
        !GuestAddressRange(*g_host, attack64, sizeof(uint32_t)) ||
        !GuestAddressRange(*g_host, defend64, sizeof(uint32_t))) {
        return;
    }

    const uint32_t display = static_cast<uint32_t>(display64);
    const uint32_t attack = static_cast<uint32_t>(attack64);
    const uint32_t defend = static_cast<uint32_t>(defend64);
    const uint32_t display_value = g_host->MemoryRead32(display);
    const uint32_t attack_value = g_host->MemoryRead32(attack);
    const uint32_t defend_value = g_host->MemoryRead32(defend);

    // Values outside these option ranges mean rgl_config has not been
    // initialized yet.  Try again on the next guest frame.
    if (display_value > 1u || attack_value > 3u || defend_value > 2u) return;
    // Likewise, all-zero is the ELF/BSS state before Config_manager installs
    // the game's defaults (stock attack direction is joystick movement = 2).
    if (display_value == 0u && attack_value == 0u && defend_value == 0u) return;

    if (display_value == 0u && attack_value == 2u && defend_value == 0u) {
        g_host->MemoryWrite32(display, 1u);
        g_host->MemoryWrite32(attack, 2u);
        g_host->MemoryWrite32(defend, 1u);
        if (g_host->MemoryRead32(display) == 1u &&
            g_host->MemoryRead32(attack) == 2u &&
            g_host->MemoryRead32(defend) == 1u) {
            LOGI("migrated stock direction controls: show=1 attack=2 block=1");
        } else {
            // A concurrent guest initialization write won the race.  Retry
            // next frame instead of claiming the migration completed.
            return;
        }
    }
    g_directional_controls_defaults_checked = true;
}

void SyncGuestSettingsLocked() {
    if (!g_host || !g_game_screen) return;
    uint32_t active_window = kNoActiveWindowId;
    if (!ReadStableActiveWindowId(*g_host, g_game_screen, &active_window)) return;
    const bool options_active = active_window == GST_OPTIONS_WINDOW_ID;
    const bool options_closed = g_guest_settings_previous_window_known &&
        g_guest_settings_previous_window == GST_OPTIONS_WINDOW_ID && !options_active;
    g_guest_settings_previous_window = active_window;
    g_guest_settings_previous_window_known = true;
    if (options_closed) {
        g_rgl_config_save_pending = true;
        g_rgl_config_save_attempts = 0;
    }
    if (g_rgl_config_save_pending && !options_active) {
        if (PersistGuestRglConfigLocked()) {
            g_rgl_config_save_pending = false;
            g_rgl_config_save_attempts = 0;
        } else if (++g_rgl_config_save_attempts >= 3u) {
            LOGE("rgl_config save abandoned after %u attempts",
                 g_rgl_config_save_attempts);
            g_rgl_config_save_pending = false;
        }
    }
    if (!options_active && !options_closed) return;

    uint32_t state_address = 0;
    GuestSettingsHeaderSnapshot header{};
    if (!ResolveGuestSettingsStateLocked(&state_address) ||
        !ReadGuestSettingsHeaderLocked(state_address, &header)) {
        if (options_closed) {
            g_guest_settings_custom_session = false;
            g_guest_settings_last_snapshot_valid = false;
        }
        return;
    }

    const uint32_t mission_status = CurrentMissionStatusForSettingsLocked();
    const uint32_t mission_status_address = state_address +
        GuestSettingsField(GST_OFFSETOF(guest_settings_state, host_mission_status));
    if (GuestAddressRange(*g_host, mission_status_address, sizeof(uint32_t))) {
        g_host->MemoryWrite32(mission_status_address, mission_status);
    }

    warband::guest_settings::Values snapshot{};
    uint32_t snapshot_sequence = 0;
    const bool snapshot_valid = ReadGuestSettingsSnapshotLocked(
        state_address, &snapshot, &snapshot_sequence);
    (void)snapshot_sequence;

    if (snapshot_valid &&
        (snapshot.bool_mask & GST_BOOL_LAYOUT_EDIT_REQUEST) != 0u) {
        LOGI("layout-edit request: mask=0x%08X window=%u mission_status=%u",
             snapshot.bool_mask, active_window, mission_status);
    }

    if (options_closed) {
        if (g_guest_settings_custom_session) {
            if (snapshot_valid) {
                g_guest_settings_last_snapshot = snapshot;
                g_guest_settings_last_snapshot_valid = true;
            }
            if (g_guest_settings_last_snapshot_valid) {
                QueueGuestSettingsCommitLocked(g_guest_settings_last_snapshot);
            }
        }
        g_guest_settings_custom_session = false;
        g_guest_settings_last_snapshot_valid = false;
        return;
    }

    const bool new_activation = !g_guest_settings_state_seen ||
        header.activation_seq != g_guest_settings_seen_activation;
    if (new_activation) {
        g_guest_settings_state_seen = true;
        g_guest_settings_seen_activation = header.activation_seq;
        g_guest_settings_seen_open = header.open_seq;
        g_guest_settings_seen_close = header.close_seq;
        g_guest_settings_custom_session = header.status == GST_STATUS_CUSTOM_ACTIVE;
        g_guest_settings_last_snapshot_valid = false;
    }

    if (g_guest_settings_host_configured &&
        (!g_guest_settings_seeded ||
         g_guest_settings_seeded_activation != header.activation_seq ||
         g_guest_settings_seeded_generation != g_guest_settings_host_generation)) {
        if (WriteGuestSettingsHostSeedLocked(state_address,
                                             g_guest_settings_host_values)) {
            g_guest_settings_seeded = true;
            g_guest_settings_seeded_activation = header.activation_seq;
            g_guest_settings_seeded_generation = g_guest_settings_host_generation;
        }
    }

    if (header.open_seq != g_guest_settings_seen_open) {
        g_guest_settings_seen_open = header.open_seq;
        g_guest_settings_custom_session = true;
        g_guest_settings_last_snapshot_valid = false;
    } else if (header.status == GST_STATUS_CUSTOM_ACTIVE) {
        g_guest_settings_custom_session = true;
    }
    if (g_guest_settings_custom_session && snapshot_valid) {
        g_guest_settings_last_snapshot = snapshot;
        g_guest_settings_last_snapshot_valid = true;
    }

    if (header.close_seq != g_guest_settings_seen_close) {
        g_guest_settings_seen_close = header.close_seq;
        if (g_guest_settings_custom_session && g_guest_settings_last_snapshot_valid) {
            QueueGuestSettingsCommitLocked(g_guest_settings_last_snapshot);
        }
        g_guest_settings_custom_session = false;
        g_guest_settings_last_snapshot_valid = false;
    }
}

void ApplyReportTextScaleLocked(uint32_t base, float scale) {
    using namespace warband::report_text;
    const uint32_t movw = base + kScaleMovwVa;
    const uint32_t movt = base + kScaleMovtVa;
    if (!GuestAddressRange(*g_host, static_cast<uint64_t>(base) + kScaleMovtVa,
                           sizeof(uint32_t))) return;
    if (g_report_scale_verified_base != base) {
        for (const Instruction& instruction : kContextPreimage) {
            const uint64_t address = static_cast<uint64_t>(base) + instruction.va;
            if (!GuestAddressRange(*g_host, address, sizeof(uint32_t)) ||
                g_host->MemoryRead32(static_cast<uint32_t>(address)) !=
                    instruction.word) {
                return;
            }
        }
        g_report_scale_expected_words = kOriginalScaleWords;
        g_report_scale_verified_base = base;
    }
    if (g_host->MemoryRead32(movw) != g_report_scale_expected_words[0] ||
        g_host->MemoryRead32(movt) != g_report_scale_expected_words[1]) return;
    const auto words = ScaleWords(scale);
    if (words == g_report_scale_expected_words) return;
    g_host->MemoryWrite32(movw, words[0]);
    g_host->MemoryWrite32(movt, words[1]);
    g_host->InvalidateGuestCode(movw, movt - movw + sizeof(uint32_t));
    g_report_scale_expected_words = words;
}

void ApplyRendererGuestSettingsLocked() {
    if (!g_host || !g_guest_settings_host_configured) return;

    const warband::guest_settings::Values* values = &g_guest_settings_host_values;
    if (g_guest_settings_pending_valid) {
        values = &g_guest_settings_pending_snapshot;
    } else if (g_guest_settings_custom_session &&
               g_guest_settings_last_snapshot_valid) {
        values = &g_guest_settings_last_snapshot;
    }

    bridge_gl::SetHdrBrightness(
        values->floats[GST_FLOAT_HDR_BRIGHTNESS]);
    bridge_gl::SetFxaaEnabled(
        (values->bool_mask & GST_BOOL_FXAA_ENABLED) != 0u);

    ElfLoader* game = g_host->GetLibrary("libMBExpMobile.so");
    if (!game) return;
    const uint32_t base = game->GetBaseAddress();

    ApplyReportTextScaleLocked(base, values->floats[GST_FLOAT_REPORT_TEXT_SCALE]);

    // IDA-verified rgl_config fields in libMBExpMobile.so. Keep the shadow
    // map at 4096; the user-facing 0/1/2 control selects the separate plant
    // shadow rendering mode, whose visual effect is much larger.
    constexpr uint32_t kAccurateShadowsVa = 0x451134u;
    constexpr uint32_t kRealisticPlantShadowsVa = 0x451188u;
    constexpr uint32_t kShadowmapQualityVa = 0x451210u;
    constexpr uint32_t kPostFxDofVa = 0x451220u;
    constexpr uint32_t kPostFxHdrVa = 0x451224u;
    constexpr uint32_t kPostFxAutoExposureVa = 0x451228u;
    constexpr uint32_t kAspectControlVa = 0x45124Cu;
    constexpr std::array<uint32_t, 7> kFields = {
        kAccurateShadowsVa, kRealisticPlantShadowsVa, kShadowmapQualityVa,
        kPostFxDofVa, kPostFxHdrVa, kPostFxAutoExposureVa, kAspectControlVa,
    };
    for (uint32_t va : kFields) {
        if (!GuestAddressRange(*g_host, static_cast<uint64_t>(base) + va,
                               sizeof(uint32_t))) {
            return;
        }
    }

    const uint32_t bool_mask = values->bool_mask;
    // clear_all copies rgl_config.enable_aspect_ratio_control to this byte.
    // Mirror a menu change immediately, then let future window resets inherit
    // the saved config value. Leaving the bit clear restores the game's wide,
    // stretched interface and its matching hit tests.
    const uint32_t menu_aspect =
        (bool_mask & GST_BOOL_MENU_ASPECT) != 0u ? 1u : 0u;
    if (g_host->MemoryRead32(base + kAspectControlVa) != menu_aspect) {
        g_host->MemoryWrite32(base + kAspectControlVa, menu_aspect);
        if (g_window_manager && GuestAddressRange(
                *g_host, static_cast<uint64_t>(g_window_manager) +
                             kWindowManagerAspectEnabledOffset,
                sizeof(uint8_t))) {
            g_host->MemoryWrite8(
                g_window_manager + kWindowManagerAspectEnabledOffset,
                static_cast<uint8_t>(menu_aspect));
        }
    }
    // DOF only blurred the frame in device A/B captures. Its checkbox is
    // hidden and a persisted preference must never enable the old path.
    // const uint32_t dof_enabled = (bool_mask & GST_BOOL_DOF_ENABLED) ? 1u : 0u;
    constexpr uint32_t dof_enabled = 0u;
    const uint32_t hdr_enabled =
        (bool_mask & GST_BOOL_HDR_ENABLED) != 0u ? 1u : 0u;
    const uint32_t auto_exposure =
        (bool_mask & GST_BOOL_AUTO_EXPOSURE) != 0u ? 1u : 0u;
    const uint32_t shadow_mode = static_cast<uint32_t>(
        warband::guest_settings::ShadowModeChoice(
            values->floats[GST_FLOAT_SHADOW_MODE]));
    constexpr uint32_t kShadowmapQuality = 2u;
    constexpr uint32_t kDetailedShadows = 1u;
    g_host->MemoryWrite32(base + kShadowmapQualityVa, kShadowmapQuality);
    g_host->MemoryWrite32(base + kAccurateShadowsVa, kDetailedShadows);
    g_host->MemoryWrite32(base + kRealisticPlantShadowsVa, shadow_mode);
    const uint32_t previous_dof = g_host->MemoryRead32(base + kPostFxDofVa);
    const uint32_t previous_hdr = g_host->MemoryRead32(base + kPostFxHdrVa);
    const uint32_t previous_auto_exposure =
        g_host->MemoryRead32(base + kPostFxAutoExposureVa);
    g_host->MemoryWrite32(base + kPostFxDofVa, dof_enabled);
    g_host->MemoryWrite32(base + kPostFxHdrVa, hdr_enabled);
    g_host->MemoryWrite32(base + kPostFxAutoExposureVa, auto_exposure);

    // rglGraphics_context::restore_device normally copies the config dwords
    // into these renderer-owned mode bytes only during a device restore. Live
    // Additional-tab changes must mirror that hand-off and dirty the PostFX
    // graph once; changing rgl_config alone leaves the old graph rendering.
    constexpr uint32_t kHdrModeSelectedVa = 0x1778778u;
    constexpr uint32_t kDofModeSelectedVa = 0x1778779u;
    constexpr uint32_t kRendererVa = 0x3DA000u;
    constexpr uint32_t kRendererPostFxDirtyOffset = 0xAF68u;
    const uint32_t hdr_mode = base + kHdrModeSelectedVa;
    const uint32_t dof_mode = base + kDofModeSelectedVa;
    const uint32_t renderer_dirty = base + kRendererVa +
                                    kRendererPostFxDirtyOffset;
    if (g_host->FindSymbol("rgl_HDR_mode_selected") == hdr_mode &&
        g_host->FindSymbol("rgl_DOF_mode_selected") == dof_mode &&
        g_host->FindSymbol("rgl_renderer") == base + kRendererVa &&
        GuestAddressRange(*g_host, hdr_mode, sizeof(uint8_t)) &&
        GuestAddressRange(*g_host, dof_mode, sizeof(uint8_t)) &&
        GuestAddressRange(*g_host, renderer_dirty, sizeof(uint32_t))) {
        const uint8_t previous_hdr_mode = g_host->MemoryRead8(hdr_mode);
        const uint8_t previous_dof_mode = g_host->MemoryRead8(dof_mode);
        const bool postfx_graph_changed = previous_hdr != hdr_enabled ||
            previous_dof != dof_enabled ||
            previous_auto_exposure != auto_exposure ||
            previous_hdr_mode != static_cast<uint8_t>(hdr_enabled) ||
            previous_dof_mode != static_cast<uint8_t>(dof_enabled);
        g_host->MemoryWrite8(hdr_mode, static_cast<uint8_t>(hdr_enabled));
        g_host->MemoryWrite8(dof_mode, static_cast<uint8_t>(dof_enabled));
        if (postfx_graph_changed && g_host->MemoryRead32(renderer_dirty) == 0u) {
            g_host->MemoryWrite32(renderer_dirty, 2u);
        }
    }

    static uint32_t reported_base = 0u;
    static uint32_t reported_mask = std::numeric_limits<uint32_t>::max();
    static uint32_t reported_quality = std::numeric_limits<uint32_t>::max();
    static float reported_brightness = -1.0f;
    const float brightness = bridge_gl::HdrBrightness();
    const uint32_t renderer_mask = bool_mask &
        (GST_BOOL_HDR_ENABLED | GST_BOOL_AUTO_EXPOSURE);
    if (reported_base != base || reported_mask != renderer_mask ||
        reported_quality != shadow_mode ||
        reported_brightness != brightness) {
        reported_base = base;
        reported_mask = renderer_mask;
        reported_quality = shadow_mode;
        reported_brightness = brightness;
        LOGI("renderer settings hdr=%u dof=%u autoexp=%u hdr_brightness=%.3f "
             "shadow_mode=%u shadow_size=%u accurate_shadows=%u "
             "realistic_plant_shadows=%u",
             hdr_enabled, dof_enabled, auto_exposure,
             brightness, shadow_mode, 1024u << kShadowmapQuality,
             kDetailedShadows, shadow_mode);
    }
}

void ClearPendingConversationTapLocked() {
    g_pending_conversation_window = 0;
    g_pending_conversation_tap_ms = 0;
}

void CancelConversationTouchLocked() {
    g_conversation_touch.Cancel();
    g_conversation_touch_window = 0;
    ClearPendingConversationTapLocked();
}

bool ResolveContinueConversationWindowLocked(uint32_t* conversation_out) {
    if (conversation_out) *conversation_out = 0;
    if (!conversation_out || !g_host || !g_game_screen) return false;
    uint32_t active_window_id = kNoActiveWindowId;
    if (!ReadStableActiveWindowId(*g_host, g_game_screen, &active_window_id) ||
        active_window_id != warband::input::kConversationWindowId) {
        return false;
    }

    // init_game_windows stores Conversation_window in game_screen[1 + 12].
    // The stock activation path creates the continue prompt when this window
    // has no response choices; validate both the active ID and its object.
    const uint64_t slot64 = static_cast<uint64_t>(g_game_screen) +
                            warband::input::kConversationWindowSlotOffset;
    if (!GuestAddressRange(*g_host, slot64, sizeof(uint32_t))) return false;
    const uint32_t slot = static_cast<uint32_t>(slot64);
    const uint32_t conversation = g_host->MemoryRead32(slot);
    if (!conversation || !IsWordAligned(conversation) ||
        !GuestAddressRange(*g_host, conversation,
                           warband::input::kConversationObjectSize)) {
        return false;
    }
    ElfLoader* game = g_host->GetLibrary("libMBExpMobile.so");
    if (!game) return false;
    const uint64_t expected_vtable64 = static_cast<uint64_t>(game->GetBaseAddress()) +
                                       warband::input::kConversationVtableImageOffset;
    if (!GuestAddressRange(*g_host, expected_vtable64, sizeof(uint32_t))) return false;
    const uint32_t expected_vtable = static_cast<uint32_t>(expected_vtable64);
    if (g_host->MemoryRead32(conversation) != expected_vtable) {
        return false;
    }
    const uint64_t choice_count64 = static_cast<uint64_t>(conversation) +
                                    warband::input::kConversationChoiceCountOffset;
    if (!GuestAddressRange(*g_host, choice_count64, sizeof(uint32_t))) return false;
    const uint32_t choice_count_address = static_cast<uint32_t>(choice_count64);
    if (!warband::input::IsContinueChoiceCount(
            g_host->MemoryRead32(choice_count_address))) return false;
    uint32_t final_window_id = kNoActiveWindowId;
    if (g_host->MemoryRead32(slot) != conversation ||
        !ReadStableActiveWindowId(*g_host, g_game_screen, &final_window_id) ||
        final_window_id != warband::input::kConversationWindowId ||
        g_host->MemoryRead32(slot) != conversation ||
        g_host->MemoryRead32(conversation) != expected_vtable ||
        !warband::input::IsContinueChoiceCount(
            g_host->MemoryRead32(choice_count_address))) {
        return false;
    }
    *conversation_out = conversation;
    return true;
}

uint64_t MonotonicMilliseconds() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool ConsumePendingConversationTapLocked(uint32_t guest_conversation) {
    if (!g_pending_conversation_window) return false;
    const uint64_t now_ms = MonotonicMilliseconds();
    if (now_ms < g_pending_conversation_tap_ms ||
        now_ms - g_pending_conversation_tap_ms > kConversationTapLifetimeMs) {
        ClearPendingConversationTapLocked();
        return false;
    }

    uint32_t active_conversation = 0;
    if (guest_conversation != g_pending_conversation_window ||
        !ResolveContinueConversationWindowLocked(&active_conversation) ||
        active_conversation != g_pending_conversation_window) {
        ClearPendingConversationTapLocked();
        return false;
    }

    ClearPendingConversationTapLocked();
    return true;
}

void ExpirePendingConversationTapLocked() {
    if (!g_pending_conversation_window) return;
    const uint64_t now_ms = MonotonicMilliseconds();
    uint32_t active_conversation = 0;
    if (now_ms < g_pending_conversation_tap_ms ||
        now_ms - g_pending_conversation_tap_ms > kConversationTapLifetimeMs ||
        !ResolveContinueConversationWindowLocked(&active_conversation) ||
        active_conversation != g_pending_conversation_window) {
        ClearPendingConversationTapLocked();
    }
}

bool SameMenuTouchConfig(const warband::input::MenuTouchConfig& left,
                         const warband::input::MenuTouchConfig& right) {
    return left.swipe_navigation == right.swipe_navigation &&
           left.tap_confirm == right.tap_confirm &&
           left.shoulder_buttons == right.shoulder_buttons &&
           left.touch_slop == right.touch_slop &&
           left.step_distance == right.step_distance &&
           left.shoulder_size == right.shoulder_size &&
           left.shoulder_tap_slop == right.shoulder_tap_slop &&
           left.surface_width == right.surface_width &&
           left.surface_height == right.surface_height;
}

bool SameMapTouchConfig(const warband::input::MapTouchConfig& left,
                         const warband::input::MapTouchConfig& right) {
    return left.touch_slop == right.touch_slop &&
           left.edge_dead_zone_fraction == right.edge_dead_zone_fraction &&
           left.bottom_control_exclusion_fraction == right.bottom_control_exclusion_fraction &&
           left.pinch_slop == right.pinch_slop &&
           left.rotation_slop_radians == right.rotation_slop_radians &&
           left.tilt_slop == right.tilt_slop &&
           left.density == right.density &&
           left.surface_width == right.surface_width &&
           left.surface_height == right.surface_height &&
           left.double_tap_timeout_ns == right.double_tap_timeout_ns;
}

void SetTouchActionOverrideLocked(
    const AInputEvent* event,
    int32_t action = AMOTION_EVENT_ACTION_CANCEL) {
    if (!event) return;
    g_touch_action_override_event = event;
    g_touch_action_override_value = action;
}

void CancelMapTouchLocked() {
    g_map_touch.Cancel();
    g_map_zoom_pending = 0;
    g_map_zoom_remainder = 0.0f;
    g_map_pan_pending_x = 0.0f;
    g_map_pan_pending_y = 0.0f;
    g_map_rotation_pending = 0.0f;
    g_map_tilt_pending_pixels = 0.0f;
    g_map_pitch_target = std::numeric_limits<float>::quiet_NaN();
    g_map_compass_tap_pending = false;
    g_map_interactive_wait_active = false;
    g_map_cancel_wait_pending = false;
    g_map_recenter_phase = MapRecenterPhase::None;
}

void CaptureMapRouteLocked();

void QueueMapRouteRestoreLocked() {
    if (g_map_route_snapshot.current_game) {
        g_map_route_restore_pending = true;
        g_map_route_commit_pending = false;
    }
}

void RestoreInventoryGuestCursorLocked(uint32_t saved_cursor_x,
                                       uint32_t saved_cursor_y);

void CancelInventoryTouchLocked() {
    if (g_inventory_touch.guest_cursor_pinned) {
        RestoreInventoryGuestCursorLocked(g_inventory_touch.saved_cursor_x,
                                          g_inventory_touch.saved_cursor_y);
    }
    g_inventory_touch = {};
    g_inventory_widgets = {};
}

void ObserveInventoryTouchLocked(const AInputEvent* event, int32_t source);
void ApplyInventoryDragLocked();
bool PreserveInventoryRecoveryLocked(const char* reason);
void MaintainInventoryScrollPositionLocked();

void CancelCharacterTouchLocked() {
    g_character_touch_remap = {};
}

void ObserveCharacterTouchLocked(const AInputEvent* event, int32_t source);

void ClearConversationScrollRetentionLocked();
void MaintainConversationScrollPositionLocked();
void ReleaseTouchWidgetHighlightOwnershipLocked();
void RetireTouchWidgetHighlightIfWindowChangedLocked();

void CancelUiTouchLocked() {
    g_ui_touch = {};
}

void ResetUiTouchLocked() {
    CancelUiTouchLocked();
    ReleaseTouchWidgetHighlightOwnershipLocked();
    ClearConversationScrollRetentionLocked();
    g_ui_button_pending = false;
    g_ui_button_pending_window = kNoActiveWindowId;
    g_ui_button_pending_window_object = 0;
    g_ui_button_pending_widget = 0;
    g_orders_tap_pending = false;
    g_orders_tap_pending_tactical = 0;
    g_orders_tap_pending_row = 0;
    g_orders_pulses.Reset(g_discrete_buttons);
}

void ObserveUiTouchLocked(const AInputEvent* event, int32_t source);
void ApplyUiButtonTapLocked();
void ApplyOrdersTapLocked();
bool UiTouchOwnsWindowLocked(uint32_t window_id);
bool ResolveActiveWindowObjectLocked(uint32_t window_id, uint32_t* window_out);

void CancelMenuInputLocked() {
    g_menu_touch.Cancel();
    g_menu_pulses.Reset(g_discrete_buttons);
    g_menu_window_id = kNoActiveWindowId;
    g_menu_scroll_pending = false;
    g_menu_scroll_pending_dy = 0.0f;
    g_menu_scroll_pending_x = 0.0f;
    g_menu_scroll_pending_y = 0.0f;
    g_menu_scroll_pending_window = kNoActiveWindowId;
}

bool ReadCurrentMenuWindowIdLocked(uint32_t* window_id) {
    if (!window_id || !g_host || !g_game_screen) return false;
    uint32_t candidate = kNoActiveWindowId;
    if (!ReadStableActiveWindowId(*g_host, g_game_screen, &candidate) ||
        candidate == kNoActiveWindowId || candidate == kTacticalWindowId ||
        candidate == kMapWindowId) {
        return false;
    }
    *window_id = candidate;
    return true;
}

bool MenuWindowBindingMatchesLocked() {
    if (g_menu_window_id == kNoActiveWindowId) return false;
    uint32_t active_window_id = kNoActiveWindowId;
    if (!ReadCurrentMenuWindowIdLocked(&active_window_id) ||
        active_window_id != g_menu_window_id) {
        return false;
    }
    return !g_menu_touch.IsTracking() ||
           g_menu_touch.MatchesWindow(active_window_id);
}

void ReleaseIdleMenuWindowBindingLocked() {
    if (!g_menu_touch.IsTracking() && g_menu_pulses.PendingSize() == 0u &&
        g_menu_pulses.ActiveMask() == 0u) {
        g_menu_window_id = kNoActiveWindowId;
    }
}

void ForgetObservedInputEventLocked(const AInputEvent* event) {
    for (const AInputEvent*& observed : g_observed_input_events) {
        if (observed == event) observed = nullptr;
    }
}

bool MarkInputEventObservedLocked(const AInputEvent* event) {
    if (std::find(g_observed_input_events.begin(), g_observed_input_events.end(), event) !=
        g_observed_input_events.end()) {
        return false;
    }
    const auto empty = std::find(g_observed_input_events.begin(),
                                 g_observed_input_events.end(), nullptr);
    if (empty == g_observed_input_events.end()) {
        LOGE("input lifecycle table saturated; event observation skipped");
        return false;
    }
    *empty = event;
    return true;
}

void EnqueueMenuOutputLocked(const warband::input::MenuTouchRecognizer::DiscretePulse& output) {
    if (output.mask != 0 && output.count != 0) {
        g_menu_pulses.Enqueue(output, MonotonicMilliseconds());
    }
}

int32_t MotionPointerIndex(const AInputEvent* event, int32_t pointer_id) {
    const size_t count = AMotionEvent_getPointerCount(event);
    for (size_t index = 0; index < count; ++index) {
        if (AMotionEvent_getPointerId(event, index) == pointer_id) {
            return static_cast<int32_t>(index);
        }
    }
    return -1;
}

float ConversationTouchSlopLocked() {
    if (!g_menu_touch_configured || !std::isfinite(g_menu_touch_config.touch_slop)) {
        return 8.0f;
    }
    return std::clamp(g_menu_touch_config.touch_slop, 1.0f, 512.0f);
}

int32_t ConversationTouchSurfaceWidthLocked() {
    if (!g_menu_touch_configured ||
        !std::isfinite(g_menu_touch_config.surface_width)) {
        return 0;
    }
    return static_cast<int32_t>(
        std::clamp(g_menu_touch_config.surface_width, 0.0f, 32768.0f));
}

int32_t ConversationTouchSurfaceHeightLocked() {
    if (!g_menu_touch_configured ||
        !std::isfinite(g_menu_touch_config.surface_height)) {
        return 0;
    }
    return static_cast<int32_t>(
        std::clamp(g_menu_touch_config.surface_height, 0.0f, 32768.0f));
}

void MoveConversationPointerLocked(const AInputEvent* event, size_t pointer_index) {
    const int32_t pointer_id = AMotionEvent_getPointerId(event, pointer_index);
    const int32_t surface_width = ConversationTouchSurfaceWidthLocked();
    const int32_t surface_height = ConversationTouchSurfaceHeightLocked();
    const size_t history_size = AMotionEvent_getHistorySize(event);
    for (size_t history = 0; history < history_size; ++history) {
        g_conversation_touch.MoveTap(
            pointer_id,
            AMotionEvent_getHistoricalX(event, pointer_index, history),
            AMotionEvent_getHistoricalY(event, pointer_index, history),
            surface_width, surface_height, true);
    }
    g_conversation_touch.MoveTap(pointer_id,
                                 AMotionEvent_getX(event, pointer_index),
                                 AMotionEvent_getY(event, pointer_index),
                                 surface_width, surface_height, true);
}

void ObserveConversationTouchLocked(const AInputEvent* event, int32_t source) {
    if ((source & AINPUT_SOURCE_TOUCHSCREEN) != AINPUT_SOURCE_TOUCHSCREEN) return;
    const int32_t raw_action = AMotionEvent_getAction(event);
    const int32_t action = raw_action & AMOTION_EVENT_ACTION_MASK;
    const size_t pointer_count = AMotionEvent_getPointerCount(event);
    if (action == AMOTION_EVENT_ACTION_DOWN) {
        g_conversation_touch.Cancel();
        g_conversation_touch_window = 0;
        if (pointer_count != 1u) return;
        uint32_t conversation = 0;
        if (!ResolveContinueConversationWindowLocked(&conversation)) return;
        if (g_conversation_touch.BeginTap(
            AMotionEvent_getPointerId(event, 0),
            AMotionEvent_getX(event, 0), AMotionEvent_getY(event, 0),
            ConversationTouchSurfaceWidthLocked(),
            ConversationTouchSurfaceHeightLocked(), ConversationTouchSlopLocked(), true)) {
            g_conversation_touch_window = conversation;
        }
        return;
    }
    if (action == AMOTION_EVENT_ACTION_POINTER_DOWN ||
        action == AMOTION_EVENT_ACTION_POINTER_UP ||
        action == AMOTION_EVENT_ACTION_CANCEL) {
        g_conversation_touch.Cancel();
        g_conversation_touch_window = 0;
        return;
    }
    if (action == AMOTION_EVENT_ACTION_MOVE) {
        if (!g_conversation_touch.IsTracking()) return;
        if (pointer_count != 1u) {
            g_conversation_touch.Cancel();
            g_conversation_touch_window = 0;
            return;
        }
        uint32_t conversation = 0;
        if (!ResolveContinueConversationWindowLocked(&conversation) ||
            conversation != g_conversation_touch_window) {
            g_conversation_touch.Cancel();
            g_conversation_touch_window = 0;
            return;
        }
        MoveConversationPointerLocked(event, 0);
        if (!g_conversation_touch.IsTracking()) g_conversation_touch_window = 0;
        return;
    }
    if (action != AMOTION_EVENT_ACTION_UP) return;
    if (!g_conversation_touch.IsTracking() || pointer_count != 1u) {
        g_conversation_touch.Cancel();
        g_conversation_touch_window = 0;
        return;
    }
    const size_t action_index = static_cast<size_t>(
        (raw_action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
        AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
    if (action_index >= pointer_count) {
        g_conversation_touch.Cancel();
        g_conversation_touch_window = 0;
        return;
    }
    uint32_t conversation = 0;
    if (!ResolveContinueConversationWindowLocked(&conversation) ||
        conversation != g_conversation_touch_window) {
        g_conversation_touch.Cancel();
        g_conversation_touch_window = 0;
        return;
    }
    MoveConversationPointerLocked(event, action_index);
    const bool tapped = g_conversation_touch.EndTap(
        AMotionEvent_getPointerId(event, action_index),
        AMotionEvent_getX(event, action_index),
        AMotionEvent_getY(event, action_index),
        ConversationTouchSurfaceWidthLocked(),
        ConversationTouchSurfaceHeightLocked(), true);
    g_conversation_touch_window = 0;
    if (tapped && !g_pending_conversation_window) {
        g_pending_conversation_window = conversation;
        g_pending_conversation_tap_ms = MonotonicMilliseconds();
    }
}

void MoveMenuPointerLocked(const AInputEvent* event, size_t pointer_index,
                           uint32_t current_window_id) {
    const int32_t pointer_id = AMotionEvent_getPointerId(event, pointer_index);
    const auto remember_scroll_point = [current_window_id](
            const warband::input::MenuMoveOutput& output) {
        if (output.scroll_dy_fraction == 0.0f) return;
        g_menu_scroll_pending_dy += output.scroll_dy_fraction;
        g_menu_scroll_pending_x = output.scroll_anchor_x;
        g_menu_scroll_pending_y = output.scroll_anchor_y;
        g_menu_scroll_pending_window = current_window_id;
        g_menu_scroll_pending = true;
    };
    const size_t history_size = AMotionEvent_getHistorySize(event);
    for (size_t history = 0; history < history_size; ++history) {
        const float x = AMotionEvent_getHistoricalX(event, pointer_index, history);
        const float y = AMotionEvent_getHistoricalY(event, pointer_index, history);
        const warband::input::MenuMoveOutput output = g_menu_touch.Move(
            pointer_id, x, y, current_window_id);
        remember_scroll_point(output);
        if (output.swallow) g_menu_touch.Cancel();
    }
    const float x = AMotionEvent_getX(event, pointer_index);
    const float y = AMotionEvent_getY(event, pointer_index);
    const warband::input::MenuMoveOutput output = g_menu_touch.Move(
        pointer_id, x, y, current_window_id);
    remember_scroll_point(output);
    if (output.swallow) g_menu_touch.Cancel();
}

void QueueMapZoomLocked(float zoom_log_delta) {
    if (!std::isfinite(zoom_log_delta) || zoom_log_delta == 0.0f) return;
    // Both the recognizer and the guest wheel use the navigation convention:
    // spread/up during double-tap-drag are positive (zoom in), while pinch/down and
    // a two-finger tap are negative (zoom out).
    g_map_zoom_remainder += zoom_log_delta * kMapPinchWheelUnitsPerLog;
    if (!std::isfinite(g_map_zoom_remainder)) {
        g_map_zoom_remainder = 0.0f;
        return;
    }
    // A stalled or malicious motion stream must not build an unbounded wheel
    // burst that survives into a later map frame.
    g_map_zoom_remainder = std::clamp(g_map_zoom_remainder, -4096.0f, 4096.0f);
    const float integral = std::trunc(g_map_zoom_remainder);
    if (integral == 0.0f) return;
    g_map_zoom_remainder -= integral;
    const int64_t sum = static_cast<int64_t>(g_map_zoom_pending) +
                        static_cast<int64_t>(integral);
    g_map_zoom_pending = static_cast<int32_t>(std::clamp<int64_t>(sum, -4096, 4096));
}

void QueueMapGestureLocked(const warband::input::MapTouchOutput& output) {
    QueueMapZoomLocked(output.zoom_log_delta);
    if (std::isfinite(output.pan_dx_pixels)) {
        g_map_pan_pending_x = std::clamp(g_map_pan_pending_x + output.pan_dx_pixels,
                                         -32768.0f, 32768.0f);
    }
    if (std::isfinite(output.pan_dy_pixels)) {
        g_map_pan_pending_y = std::clamp(g_map_pan_pending_y + output.pan_dy_pixels,
                                         -32768.0f, 32768.0f);
    }
    if (std::isfinite(output.rotation_delta_radians)) {
        g_map_rotation_pending = std::clamp(
            g_map_rotation_pending + output.rotation_delta_radians,
            -6.28318530718f, 6.28318530718f);
    }
    if (std::isfinite(output.tilt_delta_pixels)) {
        g_map_tilt_pending_pixels = std::clamp(
            g_map_tilt_pending_pixels + output.tilt_delta_pixels,
            -32768.0f, 32768.0f);
    }
    if (output.recenter) {
        g_map_compass_tap_pending = true;
    }
    if (output.pan_dx_pixels != 0.0f || output.pan_dy_pixels != 0.0f ||
        output.rotation_delta_radians != 0.0f ||
        output.tilt_delta_pixels != 0.0f) {
        g_map_compass_tap_pending = false;
        g_map_recenter_phase = MapRecenterPhase::None;
    }
}

void ObserveMapTouchLocked(const AInputEvent* event, int32_t source) {
    if ((source & AINPUT_SOURCE_TOUCHSCREEN) != AINPUT_SOURCE_TOUCHSCREEN) return;
    const bool trace = [] {
        char value[2] = {0, 0};
        __system_property_get("debug.warband.map_trace", value);
        return value[0] == '1';
    }();
    static uint32_t trace_events = 0;
    const uint32_t trace_index = trace_events++;
    if (!g_gameplay_state_known || g_gameplay_active || !g_map_touch_configured) {
        if (trace && trace_index < 96u) {
            LOGI("map event dropped before window state known=%d gameplay=%d configured=%d "
                 "source=0x%X", g_gameplay_state_known, g_gameplay_active,
                 g_map_touch_configured, source);
        }
        CancelMapTouchLocked();
        return;
    }

    uint32_t active_window_id = kNoActiveWindowId;
    if (!g_host || !g_game_screen ||
        !ReadStableActiveWindowId(*g_host, g_game_screen, &active_window_id)) {
        if (trace && trace_index < 96u) LOGI("map event dropped: active window unreadable");
        CancelMapTouchLocked();
        return;
    }
    if (active_window_id != kMapWindowId) {
        if (trace && trace_index < 96u) {
            LOGI("map event ignored window=%u source=0x%X", active_window_id, source);
        }
        if (g_map_touch.IsTracking()) SetTouchActionOverrideLocked(event);
        CancelMapTouchLocked();
        return;
    }
    if (UiTouchOwnsWindowLocked(kMapWindowId)) {
        // The raw stream is either a swallowed bottom dead-zone touch or a
        // synthetic bottom button.  Letting MapTouchRecognizer see it would
        // reintroduce the destination click we intentionally removed.
        CancelMapTouchLocked();
        return;
    }

    const int32_t raw_action = AMotionEvent_getAction(event);
    const int32_t action = raw_action & AMOTION_EVENT_ACTION_MASK;
    const size_t pointer_count = AMotionEvent_getPointerCount(event);
    if (trace && trace_index < 96u &&
        (action != AMOTION_EVENT_ACTION_MOVE || (trace_index % 8u) == 0u)) {
        const float x = pointer_count > 0u ? AMotionEvent_getX(event, 0) : 0.0f;
        const float y = pointer_count > 0u ? AMotionEvent_getY(event, 0) : 0.0f;
        LOGI("map event action=%d raw=0x%X pointers=%zu p0=%.1f,%.1f "
             "surface=%.0fx%.0f tracking=%d cancelled=%d edge=%d",
             action, raw_action, pointer_count, x, y,
             g_map_touch_config.surface_width, g_map_touch_config.surface_height,
             g_map_touch.IsTracking(), g_map_touch.GuestStreamCancelled(),
             g_map_touch.IsEdgeSuppressed());
    }
    if (action == AMOTION_EVENT_ACTION_DOWN) {
        if (pointer_count == 1u) {
            if (g_map_interactive_wait_active) {
                // A wait may begin immediately after an unrelated map tap.
                // Its cancelling tap must never be reclassified as the second
                // half of the one-handed zoom gesture.
                g_map_touch.ForgetTapHistory();
            }
            const auto output = g_map_touch.Begin(
                AMotionEvent_getPointerId(event, 0),
                AMotionEvent_getX(event, 0),
                AMotionEvent_getY(event, 0),
                AMotionEvent_getEventTime(event));
            QueueMapGestureLocked(output);
            if (!output.cancel_guest_stream) CaptureMapRouteLocked();
            if (output.cancel_guest_stream) SetTouchActionOverrideLocked(event);
        }
        return;
    }
    if (action == AMOTION_EVENT_ACTION_POINTER_DOWN) {
        if (!g_map_touch.IsTracking() || pointer_count < 2u) return;
        // Refresh the original pointer before establishing the second point;
        // Android includes both points in this event.
        for (size_t index = 0; index < pointer_count; ++index) {
            g_map_touch.UpdatePointer(AMotionEvent_getPointerId(event, index),
                                      AMotionEvent_getX(event, index),
                                      AMotionEvent_getY(event, index));
        }
        const size_t action_index = static_cast<size_t>(
            (raw_action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
            AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
        if (action_index >= pointer_count) return;
        const auto output = g_map_touch.PointerDown(
            AMotionEvent_getPointerId(event, action_index),
            AMotionEvent_getX(event, action_index),
            AMotionEvent_getY(event, action_index),
            AMotionEvent_getEventTime(event));
        QueueMapGestureLocked(output);
        if (output.cancel_guest_stream) QueueMapRouteRestoreLocked();
        if (output.cancel_guest_stream) SetTouchActionOverrideLocked(event);
        return;
    }
    if (action == AMOTION_EVENT_ACTION_MOVE) {
        if (!g_map_touch.IsTracking() || pointer_count == 0u) return;
        if (pointer_count > 16u) {
            // Do not let an unrepresentable motion frame leak a partial
            // stream to the guest.  Cancel the recognizer and the current
            // Android event so the stock map cannot turn it into a tap.
            SetTouchActionOverrideLocked(event);
            QueueMapRouteRestoreLocked();
            CancelMapTouchLocked();
            return;
        }
        std::array<warband::input::MapTouchPoint, 16> points{};
        for (size_t index = 0; index < pointer_count; ++index) {
            points[index] = {
                AMotionEvent_getPointerId(event, index),
                AMotionEvent_getX(event, index),
                AMotionEvent_getY(event, index),
            };
        }
        const auto output = g_map_touch.MoveFrame(
            points.data(), pointer_count, AMotionEvent_getEventTime(event));
        QueueMapGestureLocked(output);
        if (output.cancel_guest_stream || g_map_touch.GuestStreamCancelled()) {
            QueueMapRouteRestoreLocked();
        }
        // Once an edge swipe or pinch has claimed the stream, every later
        // MOVE must remain CANCEL as well.  Cancelling only the transition
        // frame lets the stock map consume the following coordinates and
        // continue panning underneath the bridge-owned gesture.
        if (output.cancel_guest_stream || g_map_touch.GuestStreamCancelled()) {
            SetTouchActionOverrideLocked(event);
        }
        return;
    }
    if (action == AMOTION_EVENT_ACTION_POINTER_UP) {
        if (!g_map_touch.IsTracking() || pointer_count == 0u) return;
        if (pointer_count > 16u) {
            SetTouchActionOverrideLocked(event);
            QueueMapRouteRestoreLocked();
            CancelMapTouchLocked();
            return;
        }
        std::array<warband::input::MapTouchPoint, 16> points{};
        for (size_t index = 0; index < pointer_count; ++index) {
            points[index] = {
                AMotionEvent_getPointerId(event, index),
                AMotionEvent_getX(event, index),
                AMotionEvent_getY(event, index),
            };
        }
        const auto moved = g_map_touch.MoveFrame(
            points.data(), pointer_count, AMotionEvent_getEventTime(event));
        QueueMapGestureLocked(moved);
        const size_t action_index = static_cast<size_t>(
            (raw_action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
            AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
        if (action_index >= pointer_count) return;
        const auto output = g_map_touch.PointerUp(
            AMotionEvent_getPointerId(event, action_index),
            AMotionEvent_getEventTime(event));
        QueueMapGestureLocked(output);
        if (moved.cancel_guest_stream || output.cancel_guest_stream ||
            g_map_touch.GuestStreamCancelled()) {
            QueueMapRouteRestoreLocked();
        }
        if (moved.cancel_guest_stream || output.cancel_guest_stream ||
            g_map_touch.GuestStreamCancelled()) {
            SetTouchActionOverrideLocked(event);
        }
        return;
    }
    if (action == AMOTION_EVENT_ACTION_UP) {
        if (!g_map_touch.IsTracking() || pointer_count == 0u) {
            CancelMapTouchLocked();
            return;
        }
        const size_t action_index = static_cast<size_t>(
            (raw_action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
            AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
        if (action_index >= pointer_count) {
            CancelMapTouchLocked();
            return;
        }
        const std::array<warband::input::MapTouchPoint, 1> point = {{
            {AMotionEvent_getPointerId(event, action_index),
             AMotionEvent_getX(event, action_index),
             AMotionEvent_getY(event, action_index)}}};
        const auto moved = g_map_touch.MoveFrame(
            point.data(), point.size(), AMotionEvent_getEventTime(event));
        QueueMapGestureLocked(moved);
        const bool guest_stream_was_claimed = g_map_touch.GuestStreamCancelled();
        const auto output = g_map_touch.Finish(
            AMotionEvent_getPointerId(event, action_index),
            AMotionEvent_getEventTime(event));
        QueueMapGestureLocked(output);
        const bool cancel_interactive_wait = output.content_tap &&
            !guest_stream_was_claimed && g_map_interactive_wait_active;
        if (cancel_interactive_wait) {
            // DOWN has already reached the guest. Replacing this final UP with
            // CANCEL makes the first tap stop waiting without also publishing
            // a party destination. The frame thread revalidates and clears the
            // wait duration before touching guest state.
            g_map_cancel_wait_pending = true;
            g_map_interactive_wait_active = false;
            g_map_touch.ForgetTapHistory();
        }
        if (cancel_interactive_wait || guest_stream_was_claimed ||
            moved.cancel_guest_stream || output.cancel_guest_stream ||
            !output.content_tap) {
            QueueMapRouteRestoreLocked();
        } else if (g_map_route_snapshot.current_game) {
            g_map_route_commit_pending = true;
        }
        if (moved.cancel_guest_stream || output.cancel_guest_stream ||
            cancel_interactive_wait) {
            SetTouchActionOverrideLocked(event);
        }
        return;
    }
    if (action == AMOTION_EVENT_ACTION_CANCEL) {
        QueueMapRouteRestoreLocked();
        CancelMapTouchLocked();
    }
}

void ObserveMenuTouchLocked(const AInputEvent* event, int32_t source) {
    if (!g_gameplay_state_known || g_gameplay_active || !g_menu_touch_configured) {
        CancelMenuInputLocked();
        return;
    }
    if ((source & AINPUT_SOURCE_TOUCHSCREEN) != AINPUT_SOURCE_TOUCHSCREEN) {
        return;
    }
    if (!g_menu_touch_config.swipe_navigation && !g_menu_touch_config.tap_confirm &&
        !g_menu_touch_config.shoulder_buttons) {
        CancelMenuInputLocked();
        return;
    }

    const int32_t raw_action = AMotionEvent_getAction(event);
    const int32_t action = raw_action & AMOTION_EVENT_ACTION_MASK;
    const size_t pointer_count = AMotionEvent_getPointerCount(event);
    if (action == AMOTION_EVENT_ACTION_DOWN) {
        CancelMenuInputLocked();
        if (pointer_count == 0) return;
        uint32_t active_window_id = kNoActiveWindowId;
        if (!ReadCurrentMenuWindowIdLocked(&active_window_id)) return;
        g_menu_window_id = active_window_id;
        g_menu_touch.Begin(AMotionEvent_getPointerId(event, 0),
                           AMotionEvent_getX(event, 0), AMotionEvent_getY(event, 0),
                           active_window_id);
        ReleaseIdleMenuWindowBindingLocked();
        return;
    }
    if (action == AMOTION_EVENT_ACTION_POINTER_DOWN) {
        // Menu synthesis is deliberately single-touch.  The raw multi-touch
        // stream remains untouched for guest widgets and sliders.
        CancelMenuInputLocked();
        return;
    }
    if (action == AMOTION_EVENT_ACTION_MOVE) {
        uint32_t current_window_id = warband::input::kUnknownMenuWindowId;
        ReadCurrentMenuWindowIdLocked(&current_window_id);
        const int32_t index = MotionPointerIndex(event, g_menu_touch.PointerId());
        if (index >= 0) {
            MoveMenuPointerLocked(event, static_cast<size_t>(index), current_window_id);
            if (g_menu_touch.OwnsVerticalScroll()) {
                // DOWN may already have reached the stock widget. Once this
                // gesture is proven vertical, CANCEL the raw stream so its UP
                // cannot activate a row and rebuild/reset the list while the
                // bridge-owned scrollbar is moving.
                SetTouchActionOverrideLocked(event);
            }
        }
        else CancelMenuInputLocked();
        return;
    }
    if (action == AMOTION_EVENT_ACTION_UP || action == AMOTION_EVENT_ACTION_POINTER_UP) {
        const size_t action_index = static_cast<size_t>(
            (raw_action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
            AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
        if (action_index >= pointer_count) {
            CancelMenuInputLocked();
            return;
        }
        const int32_t pointer_id = AMotionEvent_getPointerId(event, action_index);
        if (pointer_id != g_menu_touch.PointerId()) {
            CancelMenuInputLocked();
            return;
        }
        uint32_t current_window_id = warband::input::kUnknownMenuWindowId;
        ReadCurrentMenuWindowIdLocked(&current_window_id);
        MoveMenuPointerLocked(event, action_index, current_window_id);
        const bool still_tracking = g_menu_touch.IsTracking();
        const bool owns_scroll = still_tracking && g_menu_touch.OwnsVerticalScroll();
        const bool swallow = still_tracking && !g_menu_touch.MatchesWindow(current_window_id);
        if (still_tracking && g_menu_touch.IsShoulderTracking() &&
            g_menu_touch.MatchesWindow(current_window_id)) {
            EnqueueMenuOutputLocked(g_menu_touch.TakeShoulderPulse());
        }
        g_menu_touch.Cancel();
        if (swallow || owns_scroll) {
            // The press and release happened on different game screens.  The
            // guest observes CANCEL instead of UP, so the release cannot press
            // a widget on the new screen.  Cleared when the event is finished.
            g_touch_action_override_event = event;
            g_touch_action_override_value = AMOTION_EVENT_ACTION_CANCEL;
        }
        ReleaseIdleMenuWindowBindingLocked();
        return;
    }
    if (action == AMOTION_EVENT_ACTION_CANCEL) {
        CancelMenuInputLocked();
    }
}

jboolean RememberGameplayState(bool active) {
    const bool changed = !g_gameplay_state_known || g_gameplay_active != active;
    g_gameplay_state_known = true;
    g_gameplay_active = active;
    if (active || changed) CancelMenuInputLocked();
    return active ? JNI_TRUE : JNI_FALSE;
}

jboolean RememberedGameplayState() {
    return g_gameplay_state_known && g_gameplay_active ? JNI_TRUE : JNI_FALSE;
}

uint32_t GuestKeys() {
    return g_host ? g_guest_keys : 0;
}

bool CurrentMissionIsMultiplayerLocked() {
    if (!g_host || !g_mission_slot ||
        !GuestAddressRange(*g_host, g_mission_slot, sizeof(uint32_t))) return false;
    const uint32_t mission = g_host->MemoryRead32(g_mission_slot);
    return mission != 0u &&
        GuestAddressRange(*g_host, static_cast<uint64_t>(mission) + 0xB909u, 1u) &&
        g_host->MemoryRead8(mission + 0xB909u) == 1u;
}

uint32_t CombinedHeld() {
    return g_virtual_held | g_physical_button_held | g_physical_axis_held;
}

void ApplyHeldKey(uint32_t offset, uint32_t mask) {
    const uint32_t keys = GuestKeys();
    if (!g_host || !keys) return;
    const uint32_t held = CombinedHeld();
    g_host->MemoryWrite32(keys + offset, (held & mask) ? 1u : 0u);
}

void SetVirtualTriggerHeld(uint32_t mask, bool down) {
    if (down) g_virtual_held |= mask;
    else g_virtual_held &= ~mask;
}

// Gyro look arrives on its own channel so the bridge can cancel the game's
// control_mouse_movement_y_scale vertical multiplier: gyroscope rotation is
// physical and must map 1:1 to camera degrees on both axes.
float g_pending_gyro_look_x = 0.0f;
float g_pending_gyro_look_y = 0.0f;

float ClampUnit(float value) {
    return std::clamp(std::isfinite(value) ? value : 0.0f, -1.0f, 1.0f);
}

int16_t AxisValue(float value) {
    return static_cast<int16_t>(ClampUnit(value) * 32767.0f);
}

void WriteAxis(uint32_t keys, uint32_t offset, float value) {
    g_host->MemoryWrite16(keys + offset, static_cast<uint16_t>(AxisValue(value)));
}

void ApplyMergedControllerStateLocked() {
    const uint32_t keys = GuestKeys();
    if (!g_host || !keys) return;
    WriteAxis(keys, kAxisLeftX, g_physical_left_x + g_virtual_left_x);
    WriteAxis(keys, kAxisLeftY, g_physical_left_y + g_virtual_left_y);
    WriteAxis(keys, kAxisRightX, g_physical_right_x);
    WriteAxis(keys, kAxisRightY, g_physical_right_y);
    ApplyHeldKey(kKeyRt, kHeldRt);
    ApplyHeldKey(kKeyLt, kHeldLt);
}

int32_t AddClampedLook(int32_t current, float pending) {
    const float finite = std::clamp(std::isfinite(pending) ? pending : 0.0f,
                                    -kMaxInjectedLookPerFrame, kMaxInjectedLookPerFrame);
    const int64_t sum = static_cast<int64_t>(current) +
                        static_cast<int64_t>(std::lround(finite));
    return static_cast<int32_t>(std::clamp<int64_t>(
        sum, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max()));
}

uint32_t ResolvePlayerAgentLocked() {
    if (!g_host || !g_mission_slot || !g_sheath_weapons ||
        !GuestAddressRange(*g_host, g_mission_slot, sizeof(uint32_t))) {
        return 0;
    }
    const uint32_t mission = g_host->MemoryRead32(g_mission_slot);
    if (!mission || !GuestAddressRange(*g_host, mission + kMissionPlayerAgent,
                                       sizeof(uint32_t))) {
        return 0;
    }
    const int32_t player_id = static_cast<int32_t>(
        g_host->MemoryRead32(mission + kMissionPlayerAgent));
    if (player_id < 0 || player_id > 0xFFFF) return 0;
    if (!GuestAddressRange(*g_host, mission + kMissionAgentsBlocks, sizeof(uint32_t))) return 0;
    const uint32_t blocks = g_host->MemoryRead32(mission + kMissionAgentsBlocks);
    const uint64_t block_slot64 = static_cast<uint64_t>(blocks) +
                                  (static_cast<uint32_t>(player_id) >> 4u) * sizeof(uint32_t);
    if (!blocks || !GuestAddressRange(*g_host, block_slot64, sizeof(uint32_t))) return 0;
    const uint32_t block = g_host->MemoryRead32(static_cast<uint32_t>(block_slot64));
    const uint64_t agent64 = static_cast<uint64_t>(block) +
                             (static_cast<uint32_t>(player_id) & 0xFu) * kAgentStride;
    if (!block || !GuestAddressRange(*g_host, agent64, kAgentStride)) return 0;
    return static_cast<uint32_t>(agent64);
}

uint32_t FloatBits(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

bool ValidGuestPointerLocked(uint32_t pointer) {
    return pointer != 0 && (pointer & 3u) == 0 &&
           GuestAddressRange(*g_host, pointer, sizeof(uint32_t));
}

bool IsGuestContainerWidgetLocked(uint32_t widget) {
    if (!g_host || !g_container_clear_events || !ValidGuestPointerLocked(widget))
        return false;
    const uint32_t vtable = g_host->MemoryRead32(widget);
    // The stock container clear_events implementation itself owns this child
    // vector. Leaf widgets have a shorter allocation and the base method.
    return ValidGuestPointerLocked(vtable) &&
        GuestAddressRange(*g_host, static_cast<uint64_t>(vtable) + 0x58u, 4u) &&
        g_host->MemoryRead32(vtable + 0x58u) == g_container_clear_events &&
        GuestAddressRange(*g_host, static_cast<uint64_t>(widget) + 0x35Cu, 8u);
}

float ReadGuestFloatLocked(uint32_t address) {
    const uint32_t bits = g_host->MemoryRead32(address);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return std::isfinite(value) ? value : 0.0f;
}

bool InventoryWidgetSeen(const std::array<uint32_t, kInventoryWidgetLimit>& visited,
                         size_t count, uint32_t widget) {
    return std::find(visited.begin(), visited.begin() + count, widget) !=
           visited.begin() + count;
}

void CollectInventoryWidgetsLocked(
    uint32_t widget, int depth, InventoryWidgetSet* set,
    std::array<uint32_t, kInventoryWidgetLimit>* visited, size_t* visited_count) {
    if (!set || !visited || !visited_count || depth > 12 ||
        *visited_count >= kInventoryWidgetLimit || !ValidGuestPointerLocked(widget)) {
        return;
    }
    if (InventoryWidgetSeen(*visited, *visited_count, widget)) return;
    (*visited)[(*visited_count)++] = widget;

    const uint32_t flags = g_host->MemoryRead32(widget + kWidgetFlagsOffset);
    if ((flags & 1u) != 0u) return;
    const uint32_t vtable = g_host->MemoryRead32(widget + kWidgetVtableOffset);
    if (!ValidGuestPointerLocked(vtable)) return;
    if (g_inventory_slot_vtable && vtable == g_inventory_slot_vtable &&
        set->slot_count < kInventoryWidgetLimit) {
        set->slots[set->slot_count++] = widget;
    }
    if (g_inventory_menu_vtable && vtable == g_inventory_menu_vtable &&
        set->menu_count < kInventoryWidgetLimit) {
        set->menus[set->menu_count++] = widget;
    }

    if (!IsGuestContainerWidgetLocked(widget)) return;

    const uint32_t children_begin = g_host->MemoryRead32(
        widget + kWidgetChildrenBeginOffset);
    const uint32_t children_end = g_host->MemoryRead32(
        widget + kWidgetChildrenEndOffset);
    if (!ValidGuestPointerLocked(children_begin) ||
        !ValidGuestPointerLocked(children_end) || children_end < children_begin ||
        (children_end - children_begin) > 4u * 4096u) {
        return;
    }
    for (uint32_t cursor = children_begin; cursor < children_end; cursor += 4u) {
        if (!GuestAddressRange(*g_host, cursor, sizeof(uint32_t))) return;
        CollectInventoryWidgetsLocked(g_host->MemoryRead32(cursor), depth + 1,
                                      set, visited, visited_count);
        if (*visited_count >= kInventoryWidgetLimit) return;
    }
}

bool GatherInventoryWidgetsLocked(InventoryWidgetSet* set) {
    if (!set || !g_host || !g_game_screen ||
        !GuestAddressRange(*g_host, g_game_screen, 4u * (kInventoryWindowId + 1u) +
                                      sizeof(uint32_t))) {
        return false;
    }
    const uint32_t inventory_slot =
        g_game_screen + 4u * (kInventoryWindowId + 1u);
    const uint32_t inventory = g_host->MemoryRead32(inventory_slot);
    if (!ValidGuestPointerLocked(inventory) ||
        !GuestAddressRange(*g_host, inventory + kInventoryRootOffset,
                           sizeof(uint32_t))) {
        return false;
    }
    const uint32_t root = g_host->MemoryRead32(inventory + kInventoryRootOffset);
    if (!ValidGuestPointerLocked(root)) return false;
    *set = {};
    set->root = root;
    std::array<uint32_t, kInventoryWidgetLimit> visited{};
    size_t visited_count = 0;
    CollectInventoryWidgetsLocked(root, 0, set, &visited, &visited_count);
    return set->root == root;
}

bool InventoryWidgetUnderCursorLocked(uint32_t widget) {
    if (!g_host || !g_window_manager || !g_is_over_widget ||
        !ValidGuestPointerLocked(g_window_manager) ||
        !GuestAddressRange(*g_host, g_window_manager + 16u, sizeof(float) * 2u) ||
        !ValidGuestPointerLocked(widget)) {
        return false;
    }
    // The guest already stores its transformed cursor at window_manager+16/+20;
    // using the stock hit-test preserves clipping and scroll offsets.
    return g_host->CallGuestNested(
               g_is_over_widget,
               {g_window_manager, widget, g_window_manager + 16u}) != 0u;
}

bool InventorySlotHasItemLocked(uint32_t widget) {
    if (!g_host || !ValidGuestPointerLocked(widget) ||
        !GuestAddressRange(*g_host, widget + 0x1ECu, 3u)) {
        return false;
    }
    // refresh_slot clears these three per-mesh flags for an empty Item and
    // sets them while attaching the item's metadata meshes.  The slot's
    // background is stored separately, so this distinguishes an empty cell
    // from a real item without guessing from the cursor or menu index.
    return g_host->MemoryRead8(widget + 0x1ECu) != 0u ||
           g_host->MemoryRead8(widget + 0x1EDu) != 0u ||
           g_host->MemoryRead8(widget + 0x1EEu) != 0u;
}

bool ReadInventoryWordLocked(uint64_t address, uint32_t* value) {
    if (!value || !g_host || address > std::numeric_limits<uint32_t>::max() ||
        !GuestAddressRange(*g_host, address, sizeof(uint32_t))) return false;
    *value = g_host->MemoryRead32(static_cast<uint32_t>(address));
    return true;
}

bool ReadInventoryItemContextLocked(InventoryItemContext* context) {
    if (!context || !g_host || !g_game_screen || !g_cur_game_slot) return false;
    InventoryItemContext live{};
    uint32_t value = 0;
    if (!ReadInventoryWordLocked(static_cast<uint64_t>(g_game_screen) + 0x20u,
                                  &live.window) ||
        !ValidGuestPointerLocked(live.window) ||
        !ReadInventoryWordLocked(g_cur_game_slot, &live.game) ||
        !ValidGuestPointerLocked(live.game)) return false;
    if (!ReadInventoryWordLocked(static_cast<uint64_t>(live.window) + 4u, &value)) return false;
    live.mode = static_cast<int32_t>(value);
    if (live.mode < 0 || live.mode > 4) return false;
    if (!ReadInventoryWordLocked(static_cast<uint64_t>(live.window) + 0x14u, &value)) return false;
    live.other_troop = static_cast<int32_t>(value);
    if (!ReadInventoryWordLocked(static_cast<uint64_t>(live.game) + 0x6DCu, &value)) return false;
    live.own_troop = static_cast<int32_t>(value);
    if (!ReadInventoryWordLocked(static_cast<uint64_t>(live.game) + 0x140D8u, &value)) return false;
    live.troop_count = static_cast<int32_t>(value);
    if (!ReadInventoryWordLocked(static_cast<uint64_t>(live.game) + 0x140D4u,
                                  &live.troop_table) ||
        !ValidGuestPointerLocked(live.troop_table) || live.troop_count <= 0 ||
        live.own_troop < 0 || live.own_troop >= live.troop_count ||
        !ReadInventoryWordLocked(static_cast<uint64_t>(live.window) + 0xAF8u,
                                  &live.bag_count) ||
        !ReadInventoryWordLocked(static_cast<uint64_t>(live.window) + 0xAFCu,
                                  &live.equipment_count) ||
        live.bag_count == 0u || live.bag_count > 2u || live.equipment_count > 2u) return false;
    *context = live;
    return true;
}

int InventoryBagCapacityLocked(const InventoryItemContext& context, int menu) {
    const int32_t troop = menu == 0 ? context.own_troop : context.other_troop;
    if (troop < 0 || troop >= context.troop_count) return 0;
    uint32_t discard = 0;
    if (context.mode == 0 && ReadInventoryWordLocked(
            static_cast<uint64_t>(context.game) + 0x6E0u, &discard) &&
        discard == static_cast<uint32_t>(troop)) return 9;
    const uint64_t troop_address = static_cast<uint64_t>(context.troop_table) +
        0xFC8u * static_cast<uint64_t>(troop);
    if (!GuestAddressRange(*g_host, troop_address, 0x288u)) return 0;
    if ((g_host->MemoryRead8(static_cast<uint32_t>(troop_address) + 0x248u) & 0x10u) == 0u)
        return 90;
    uint32_t skill_limit = 0;
    if (!g_inventory_skills || !ReadInventoryWordLocked(
            static_cast<uint64_t>(g_inventory_skills) + 0x16E4u,
                                  &skill_limit)) return 0;
    const int skill = std::min<int>(g_host->MemoryRead16(
        static_cast<uint32_t>(troop_address) + 0x286u) & 0xFu,
        static_cast<int32_t>(skill_limit));
    constexpr std::array<int, 11> capacities{30, 36, 42, 48, 54, 60, 66, 72, 78, 84, 96};
    return skill >= 0 && skill <= 10 ? capacities[static_cast<size_t>(skill)] : 96;
}

bool ResolveIndexedInventoryItemLocked(const InventoryItemContext& context,
        int menu, int index, uint32_t* item, uint32_t* widget = nullptr,
        uint32_t* parent = nullptr) {
    if (menu < 0 || menu > 3 || index < 0) return false;
    const bool equipment = menu >= 2;
    const uint32_t pane = static_cast<uint32_t>(equipment ? menu - 2 : menu);
    const int capacity = equipment ? 10 : InventoryBagCapacityLocked(context, menu);
    if (pane >= (equipment ? context.equipment_count : context.bag_count) ||
        index >= capacity) return false;
    const int32_t troop = (menu & 1) == 0 ? context.own_troop : context.other_troop;
    if (troop < 0 || troop >= context.troop_count) return false;
    const uint64_t item_address = static_cast<uint64_t>(context.troop_table) +
        static_cast<uint64_t>(troop) * 0xFC8u + (equipment ? 0xF68u : 0x2D0u) +
        8u * static_cast<uint64_t>(index);
    if (item_address > std::numeric_limits<uint32_t>::max() ||
        !GuestAddressRange(*g_host, item_address, 8u)) return false;
    if (item) *item = static_cast<uint32_t>(item_address);
    if (widget && !ReadInventoryWordLocked(static_cast<uint64_t>(context.window) +
            (equipment ? 0xB04u : 0x1F8u) + (equipment ? 0x28u : 0x180u) * pane +
            4u * static_cast<uint32_t>(index), widget)) return false;
    if (parent && !ReadInventoryWordLocked(static_cast<uint64_t>(context.window) +
            (equipment ? 0x1C8u : 0x1B8u) + 4u * pane, parent)) return false;
    return true;
}

bool InventorySourceContextIsLiveLocked() {
    InventoryItemContext live{};
    return g_inventory_touch.source_ref.context.window != 0u &&
        ReadInventoryItemContextLocked(&live) && live == g_inventory_touch.source_ref.context;
}

bool OwnsActiveInventoryTouchLocked() {
    uint32_t window_id = kNoActiveWindowId;
    return g_inventory_touch.owns_guest_stream && InventorySourceContextIsLiveLocked() &&
        ReadStableActiveWindowId(*g_host, g_game_screen, &window_id) &&
        window_id == kInventoryWindowId;
}

bool CaptureInventorySourceRefLocked(uint32_t slot, uint32_t menu,
                                      InventorySourceRef* source, bool require_item = true) {
    InventoryItemContext context{};
    if (!source || !ReadInventoryItemContextLocked(&context)) return false;
    for (int pane = 0; pane < 4; ++pane) {
        const int capacity = pane < 2 ? InventoryBagCapacityLocked(context, pane) : 10;
        for (int index = 0; index < capacity; ++index) {
            uint32_t item = 0, widget = 0, parent = 0;
            if (!ResolveIndexedInventoryItemLocked(context, pane, index,
                                                   &item, &widget, &parent)) break;
            if (widget != slot || parent != menu) continue;
            uint32_t kind_count = 0;
            const int32_t kind = static_cast<int32_t>(g_host->MemoryRead32(item));
            if (!ReadInventoryWordLocked(g_inventory_item_kind_count, &kind_count) ||
                kind < -1 || (require_item && kind == -1) ||
                (kind >= 0 && static_cast<uint32_t>(kind) >= kind_count)) return false;
            *source = {context, pane, index, kind, g_host->MemoryRead32(item + 4u)};
            return true;
        }
    }
    return false;
}

void ClearInventoryDoubleTapLocked() {
    g_inventory_previous_tap = {};
    g_inventory_previous_tap_ms = 0;
}

bool SetInventoryGuestCursorForTouchLocked(float x, float y,
                                           uint32_t* saved_cursor_x,
                                           uint32_t* saved_cursor_y);
void RestoreInventoryGuestCursorLocked(uint32_t saved_cursor_x,
                                       uint32_t saved_cursor_y);

bool ResolveInventoryPointLocked(float x, float y, InventoryWidgetSet* widgets_out,
                                 uint32_t* slot_out, uint32_t* menu_out) {
    if (!widgets_out || !slot_out || !menu_out) return false;
    InventoryWidgetSet widgets{};
    if (!GatherInventoryWidgetsLocked(&widgets)) return false;
    uint32_t saved_cursor_x = 0;
    uint32_t saved_cursor_y = 0;
    if (!SetInventoryGuestCursorForTouchLocked(
            x, y, &saved_cursor_x, &saved_cursor_y)) {
        return false;
    }

    uint32_t slot = 0;
    uint32_t menu = 0;
    for (size_t index = 0; index < widgets.slot_count; ++index) {
        if (InventorySlotHasItemLocked(widgets.slots[index]) &&
            InventoryWidgetUnderCursorLocked(widgets.slots[index])) {
            slot = widgets.slots[index];
            break;
        }
    }
    for (size_t index = 0; index < widgets.menu_count; ++index) {
        if (InventoryWidgetUnderCursorLocked(widgets.menus[index])) {
            menu = widgets.menus[index];
            break;
        }
    }
    RestoreInventoryGuestCursorLocked(saved_cursor_x, saved_cursor_y);
    *widgets_out = widgets;
    *slot_out = slot;
    *menu_out = menu;
    return slot != 0u;
}

bool SetInventoryGuestCursorForTouchLocked(float x, float y,
                                           uint32_t* saved_cursor_x,
                                           uint32_t* saved_cursor_y) {
    if (!saved_cursor_x || !saved_cursor_y || !g_host || !g_window_manager ||
        !ValidGuestPointerLocked(g_window_manager) ||
        !GuestAddressRange(*g_host,
                           g_window_manager + kWindowManagerCursorXOffset,
                           sizeof(uint32_t) * 2u) ||
        !GuestAddressRange(*g_host,
                           g_window_manager + kWindowManagerSurfaceWidthOffset,
                           sizeof(uint32_t) * 2u)) {
        return false;
    }
    if (!std::isfinite(x) || !std::isfinite(y)) return false;

    // rglWindow_manager::tick converts Android pixels to the normalized
    // coordinates consumed by is_over_widget:
    //   cursor.x = touch.x / surface_width
    //   cursor.y = (1 - touch.y / surface_height) * coordinate_scale
    // Use the current touch point instead of the one-frame-late manager
    // cursor, then restore both words after the hit-test pass.
    const int32_t width = static_cast<int32_t>(
        g_host->MemoryRead32(g_window_manager + kWindowManagerSurfaceWidthOffset));
    const int32_t height = static_cast<int32_t>(
        g_host->MemoryRead32(g_window_manager + kWindowManagerSurfaceHeightOffset));
    if (width <= 0 || height <= 0) return false;
    float scale = ReadGuestFloatLocked(
        g_window_manager + kWindowManagerCoordinateScaleOffset);
    if (!(scale > 0.0f && scale <= 4.0f)) scale = 0.75f;

    *saved_cursor_x = g_host->MemoryRead32(
        g_window_manager + kWindowManagerCursorXOffset);
    *saved_cursor_y = g_host->MemoryRead32(
        g_window_manager + kWindowManagerCursorYOffset);
    const float normalized_x = std::clamp(x / static_cast<float>(width), 0.0f, 1.0f);
    const float normalized_y = std::clamp(
        (1.0f - y / static_cast<float>(height)) * scale, -4.0f, 4.0f);
    g_host->MemoryWrite32(g_window_manager + kWindowManagerCursorXOffset,
                          FloatBits(normalized_x));
    g_host->MemoryWrite32(g_window_manager + kWindowManagerCursorYOffset,
                          FloatBits(normalized_y));
    return true;
}

void RestoreInventoryGuestCursorLocked(uint32_t saved_cursor_x,
                                        uint32_t saved_cursor_y) {
    if (!g_host || !g_window_manager ||
        !GuestAddressRange(*g_host,
                           g_window_manager + kWindowManagerCursorXOffset,
                           sizeof(uint32_t) * 2u)) {
        return;
    }
    g_host->MemoryWrite32(g_window_manager + kWindowManagerCursorXOffset,
                          saved_cursor_x);
    g_host->MemoryWrite32(g_window_manager + kWindowManagerCursorYOffset,
                          saved_cursor_y);
}

bool PinInventoryGuestCursorForTouchLocked(float x, float y) {
    uint32_t displaced_cursor_x = 0;
    uint32_t displaced_cursor_y = 0;
    if (!SetInventoryGuestCursorForTouchLocked(
            x, y, &displaced_cursor_x, &displaced_cursor_y)) {
        return false;
    }
    if (!g_inventory_touch.guest_cursor_pinned) {
        g_inventory_touch.guest_cursor_pinned = true;
        g_inventory_touch.saved_cursor_x = displaced_cursor_x;
        g_inventory_touch.saved_cursor_y = displaced_cursor_y;
    }
    return true;
}

void AddInventoryWidgetEventLocked(uint32_t widget) {
    if (!g_host || !ValidGuestPointerLocked(widget) ||
        !GuestAddressRange(*g_host, widget + kWidgetPersistentEventOffset,
                           sizeof(uint32_t))) {
        return;
    }
    const uint32_t event = g_host->MemoryRead32(widget + kWidgetEventOffset);
    const uint32_t persistent =
        g_host->MemoryRead32(widget + kWidgetPersistentEventOffset);
    // A claimed touch may have left a native click/double-click queued on the
    // same widget.  The inventory window consumes 0x10000 directly; retaining
    // those lower click bits can replay a second operation after our drop.
    const uint32_t click_mask = kWidgetEventClick | kWidgetEventDoubleClick;
    g_host->MemoryWrite32(widget + kWidgetEventOffset,
                          (event & ~click_mask) | kWidgetEventDrag);
    g_host->MemoryWrite32(widget + kWidgetPersistentEventOffset,
                          (persistent & ~click_mask) | kWidgetEventDrag);
}

void ClearInventoryWidgetDragPersistenceLocked(uint32_t widget) {
    if (!g_host || !ValidGuestPointerLocked(widget) ||
        !GuestAddressRange(*g_host, widget + kWidgetPersistentEventOffset,
                           sizeof(uint32_t))) {
        return;
    }
    const uint32_t persistent = g_host->MemoryRead32(
        widget + kWidgetPersistentEventOffset);
    g_host->MemoryWrite32(widget + kWidgetPersistentEventOffset,
                          persistent & ~kWidgetEventDrag);
}

void ClearInventoryWidgetTransientEventsLocked(uint32_t widget) {
    if (!g_host || !ValidGuestPointerLocked(widget) ||
        !GuestAddressRange(*g_host, widget + kWidgetPersistentEventOffset,
                           sizeof(uint32_t))) {
        return;
    }
    const uint32_t transient_mask = kWidgetEventClick |
                                    kWidgetEventDoubleClick |
                                    kWidgetEventDrag;
    const uint32_t event = g_host->MemoryRead32(widget + kWidgetEventOffset);
    const uint32_t persistent =
        g_host->MemoryRead32(widget + kWidgetPersistentEventOffset);
    g_host->MemoryWrite32(widget + kWidgetEventOffset, event & ~transient_mask);
    g_host->MemoryWrite32(widget + kWidgetPersistentEventOffset,
                          persistent & ~transient_mask);
}

void ClearInventoryWidgetOwnedDragEventsLocked(uint32_t widget) {
    if (!g_host || !ValidGuestPointerLocked(widget) ||
        !GuestAddressRange(*g_host, widget + kWidgetPersistentEventOffset,
                           sizeof(uint32_t))) {
        return;
    }
    // Once the source's 0x2 details event has been consumed, raw MOVE hover
    // events must not replace it with another item's 0x2 or erase it with
    // 0x4. Keep the original dragged-item panel stable until the destination
    // event installs the normal post-drop overlay.
    const uint32_t owned_mask = kWidgetEventClick |
                                kWidgetEventDoubleClick |
                                kWidgetEventDrag |
                                kWidgetEventItemOverlay |
                                kWidgetEventClearOverlay;
    const uint32_t event = g_host->MemoryRead32(widget + kWidgetEventOffset);
    const uint32_t persistent =
        g_host->MemoryRead32(widget + kWidgetPersistentEventOffset);
    g_host->MemoryWrite32(widget + kWidgetEventOffset, event & ~owned_mask);
    g_host->MemoryWrite32(widget + kWidgetPersistentEventOffset,
                          persistent & ~owned_mask);
}

void SanitizeInventoryTransientEventsLocked(const InventoryWidgetSet& widgets) {
    for (size_t index = 0; index < widgets.slot_count; ++index) {
        ClearInventoryWidgetTransientEventsLocked(widgets.slots[index]);
    }
    for (size_t index = 0; index < widgets.menu_count; ++index) {
        ClearInventoryWidgetTransientEventsLocked(widgets.menus[index]);
    }
}

void SanitizeInventoryOwnedDragEventsLocked(
        const InventoryWidgetSet& widgets) {
    for (size_t index = 0; index < widgets.slot_count; ++index) {
        ClearInventoryWidgetOwnedDragEventsLocked(widgets.slots[index]);
    }
    for (size_t index = 0; index < widgets.menu_count; ++index) {
        ClearInventoryWidgetOwnedDragEventsLocked(widgets.menus[index]);
    }
}

bool QueueInventoryItemOverlayLocked(
        const InventoryWidgetSet& widgets, uint32_t slot) {
    if (!g_host || !ValidGuestPointerLocked(slot) ||
        !GuestAddressRange(*g_host, slot + kWidgetPersistentEventOffset,
                           sizeof(uint32_t)) ||
        g_host->MemoryRead32(slot + kWidgetVtableOffset) !=
            g_inventory_slot_vtable) {
        return false;
    }
    // Inventory_window::check_inventory_menu handles event 0x2 by calling
    // setup_slot_item_overlay with the real Item, slot position and ownership
    // indices. Event 0x4 clears that overlay. Remove stale hover/leave bits
    // everywhere, then queue one genuine item-details event on this slot.
    const uint32_t overlay_mask =
        kWidgetEventItemOverlay | kWidgetEventClearOverlay;
    for (size_t index = 0; index < widgets.slot_count; ++index) {
        const uint32_t widget = widgets.slots[index];
        if (!ValidGuestPointerLocked(widget) ||
            !GuestAddressRange(*g_host, widget + kWidgetEventOffset,
                               sizeof(uint32_t))) {
            continue;
        }
        const uint32_t event =
            g_host->MemoryRead32(widget + kWidgetEventOffset);
        g_host->MemoryWrite32(widget + kWidgetEventOffset,
                              event & ~overlay_mask);
    }
    const uint32_t event = g_host->MemoryRead32(slot + kWidgetEventOffset);
    g_host->MemoryWrite32(slot + kWidgetEventOffset,
                          (event & ~kWidgetEventClearOverlay) |
                              kWidgetEventItemOverlay);
    return true;
}

bool SendInventoryWidgetDragEventLocked(uint32_t widget) {
    if (!g_host || !ValidGuestPointerLocked(widget)) return false;
    const uint32_t vtable = g_host->MemoryRead32(widget + kWidgetVtableOffset);
    if (vtable != g_inventory_slot_vtable &&
        vtable != g_inventory_menu_vtable) {
        return false;
    }
    // Calling handle_event(16) is not equivalent to queuing the inventory
    // event: its base handler also creates click/double-click bits, while the
    // container variant recursively dispatches to whichever child happens to
    // be under the cursor at call time.  Queue only the exact 0x10000 bit that
    // Inventory_window::check_inventory_menu consumes.
    AddInventoryWidgetEventLocked(widget);
    ClearInventoryWidgetDragPersistenceLocked(widget);
    return true;
}

void LogInventoryWidgetGeometryLocked(const char* label, uint32_t widget) {
    if (!label || !widget || !g_host || !ValidGuestPointerLocked(widget) ||
        !GuestAddressRange(*g_host, widget + 412u, sizeof(uint32_t) * 5u)) {
        return;
    }
    LOGI("inventory widget %s=0x%08X vtable=0x%08X pos=(%.4f,%.4f) "
         "size=(%.4f,%.4f) offset=(%.4f,%.4f) event=0x%X persistent=0x%X",
         label, widget, g_host->MemoryRead32(widget),
         ReadGuestFloatLocked(widget + 412u), ReadGuestFloatLocked(widget + 416u),
         ReadGuestFloatLocked(widget + 428u), ReadGuestFloatLocked(widget + 432u),
         ReadGuestFloatLocked(widget + 436u), ReadGuestFloatLocked(widget + 440u),
         g_host->MemoryRead32(widget + kWidgetEventOffset),
         g_host->MemoryRead32(widget + kWidgetPersistentEventOffset));
}

float InventoryDragSlopLocked() {
    const float slop = g_menu_touch_configured &&
                               std::isfinite(g_menu_touch_config.touch_slop)
                           ? g_menu_touch_config.touch_slop
                           : 8.0f;
    return std::clamp(slop * kInventoryDragSlopMultiplier, 2.0f, 512.0f);
}

void ObserveInventoryTouchLocked(const AInputEvent* event, int32_t source) {
    if (!event || (source & AINPUT_SOURCE_TOUCHSCREEN) != AINPUT_SOURCE_TOUCHSCREEN) {
        return;
    }
    if (g_ui_touch.active && g_ui_touch.window_id == kInventoryWindowId) {
        ClearInventoryDoubleTapLocked();
        if (!PreserveInventoryRecoveryLocked("inventory button owns touch"))
            CancelInventoryTouchLocked();
        return;
    }
    if (!g_gameplay_state_known || g_gameplay_active || !g_inventory_touch_configured) {
        ClearInventoryDoubleTapLocked();
        if (!PreserveInventoryRecoveryLocked("inventory input mode changed"))
            CancelInventoryTouchLocked();
        return;
    }
    uint32_t active_window_id = kNoActiveWindowId;
    if (!g_host || !g_game_screen ||
        !ReadStableActiveWindowId(*g_host, g_game_screen, &active_window_id) ||
        active_window_id != kInventoryWindowId) {
        ClearInventoryDoubleTapLocked();
        if (!PreserveInventoryRecoveryLocked("inventory window changed"))
            CancelInventoryTouchLocked();
        return;
    }

    const int32_t raw_action = AMotionEvent_getAction(event);
    const int32_t action = raw_action & AMOTION_EVENT_ACTION_MASK;
    const size_t pointer_count = AMotionEvent_getPointerCount(event);
    if (action == AMOTION_EVENT_ACTION_DOWN) {
        if (PreserveInventoryRecoveryLocked("new inventory DOWN")) {
            if (OwnsActiveInventoryTouchLocked()) SetTouchActionOverrideLocked(event);
            ClearInventoryDoubleTapLocked();
            return;
        }
        CancelInventoryTouchLocked();
        if (pointer_count != 1u) return;
        const float x = AMotionEvent_getX(event, 0u);
        const float y = AMotionEvent_getY(event, 0u);
        InventoryWidgetSet widgets{};
        uint32_t slot = 0;
        uint32_t menu = 0;
        const bool point_resolved = ResolveInventoryPointLocked(
            x, y, &widgets, &slot, &menu);
        if (!warband::input::ShouldClaimInventoryItemGesture(
                point_resolved, slot, menu)) {
            // No bridge candidate is left behind: ObserveMenuTouchLocked sees
            // this same DOWN and starts empty-space scrolling immediately.
            return;
        }
        InventorySourceRef source_ref{};
        if (!CaptureInventorySourceRefLocked(slot, menu, &source_ref)) return;
        g_inventory_touch.tracking = true;
        g_inventory_touch.owns_guest_stream = true;
        g_inventory_touch.pointer_id = AMotionEvent_getPointerId(event, 0);
        g_inventory_touch.down_x = g_inventory_touch.last_x = x;
        g_inventory_touch.down_y = g_inventory_touch.last_y = y;
        g_inventory_touch.down_ms = MonotonicMilliseconds();
        g_inventory_touch.source_slot = slot;
        g_inventory_touch.source_menu = menu;
        g_inventory_touch.source_ref = source_ref;
        g_inventory_touch.source_resolved = true;
        // The guest never receives a raw item DOWN. This prevents its native
        // click/hover path from moving the item or replaying pickup audio; the
        // bridge later emits exactly one semantic event.
        SetTouchActionOverrideLocked(event, AMOTION_EVENT_ACTION_CANCEL);
        return;
    }
    if (!g_inventory_touch.tracking) return;
    if (action == AMOTION_EVENT_ACTION_POINTER_DOWN ||
        action == AMOTION_EVENT_ACTION_POINTER_UP ||
        action == AMOTION_EVENT_ACTION_CANCEL || pointer_count != 1u) {
        if (g_inventory_touch.owns_guest_stream) {
            SetTouchActionOverrideLocked(event);
        }
        ClearInventoryDoubleTapLocked();
        if (!PreserveInventoryRecoveryLocked("inventory pointer interrupted"))
            CancelInventoryTouchLocked();
        return;
    }
    const int32_t pointer_index = MotionPointerIndex(event, g_inventory_touch.pointer_id);
    if (pointer_index < 0) {
        if (g_inventory_touch.owns_guest_stream) {
            SetTouchActionOverrideLocked(event);
        }
        ClearInventoryDoubleTapLocked();
        if (!PreserveInventoryRecoveryLocked("inventory pointer disappeared"))
            CancelInventoryTouchLocked();
        return;
    }
    const float x = AMotionEvent_getX(event, static_cast<size_t>(pointer_index));
    const float y = AMotionEvent_getY(event, static_cast<size_t>(pointer_index));
    g_inventory_touch.last_x = x;
    g_inventory_touch.last_y = y;
    if (g_inventory_touch.owns_guest_stream) {
        SetTouchActionOverrideLocked(event, AMOTION_EVENT_ACTION_CANCEL);
    }
    if (action == AMOTION_EVENT_ACTION_MOVE) {
        const float dx = x - g_inventory_touch.down_x;
        const float dy = y - g_inventory_touch.down_y;
        const float distance = std::sqrt(dx * dx + dy * dy);
        if (warband::input::ShouldStartInventoryDrag(
                g_inventory_touch.owns_guest_stream,
                g_inventory_touch.drag_started,
                distance >= InventoryDragSlopLocked())) {
            g_inventory_touch.drag_started = true;
            ClearInventoryDoubleTapLocked();
            g_inventory_touch.tap_details_pending = false;
            g_inventory_touch.native_events_sanitized = false;
            // Refresh can rebuild the slot tree between DOWN and the first
            // MOVE. Force the source event to re-resolve a live slot.
            g_inventory_touch.source_resolved = false;
            g_inventory_touch.source_slot = 0u;
            g_inventory_touch.source_menu = 0u;
        }
        return;
    }
    if (action == AMOTION_EVENT_ACTION_UP) {
        if (g_inventory_touch.drag_started ||
            g_inventory_touch.long_press_shown) {
            g_inventory_touch.release_pending = true;
        } else if (warband::input::ShouldShowInventoryTapDetails(
                       g_inventory_touch.owns_guest_stream,
                       g_inventory_touch.drag_started,
                       g_inventory_touch.long_press_shown, true)) {
            const auto& ref = g_inventory_touch.source_ref;
            const warband::input::InventoryTapIdentity identity{
                ref.context.window, ref.context.mode, ref.context.other_troop,
                ref.menu, ref.index, ref.kind, ref.metadata};
            const uint64_t now_ms = MonotonicMilliseconds();
            g_inventory_touch.quick_transfer_pending = warband::input::IsInventoryDoubleTap(
                g_inventory_previous_tap, g_inventory_previous_tap_ms, identity, now_ms);
            if (g_inventory_touch.quick_transfer_pending) {
                ClearInventoryDoubleTapLocked();
            } else {
                g_inventory_previous_tap = identity;
                g_inventory_previous_tap_ms = now_ms;
            }
            g_inventory_touch.tap_details_pending = true;
        }
    }
}

struct InventoryGuestDragSnapshot {
    bool readable = false;
    bool active = false;
    uint32_t object = 0;
    int32_t source = -1;
    int32_t kind = 0;
};

InventoryGuestDragSnapshot ReadInventoryGuestDragLocked() {
    InventoryGuestDragSnapshot snapshot{};
    if (!g_host || !g_game_screen ||
        !GuestAddressRange(*g_host,
                           g_game_screen + 4u * (kInventoryWindowId + 1u),
                           sizeof(uint32_t))) {
        return snapshot;
    }
    snapshot.object = g_host->MemoryRead32(
        g_game_screen + 4u * (kInventoryWindowId + 1u));
    if (g_inventory_touch.source_ref.context.window != 0u &&
        snapshot.object != g_inventory_touch.source_ref.context.window) return snapshot;
    if (!ValidGuestPointerLocked(snapshot.object) ||
        !GuestAddressRange(*g_host, snapshot.object + 0xC34u, 12u)) {
        return snapshot;
    }
    snapshot.readable = true;
    snapshot.active = g_host->MemoryRead32(snapshot.object + 0xC34u) != 0u;
    snapshot.source = static_cast<int32_t>(
        g_host->MemoryRead32(snapshot.object + 0xC38u));
    snapshot.kind = static_cast<int32_t>(
        g_host->MemoryRead32(snapshot.object + 0xC3Cu));
    return snapshot;
}

bool InventoryRecoveryInFlightLocked() {
    return warband::input::InventoryRecoveryInFlight(
        g_inventory_touch.release_event_queued,
        g_inventory_touch.restore_pending,
        g_inventory_touch.source_event_queued,
        g_inventory_touch.release_pending);
}

bool PreserveInventoryRecoveryLocked(const char* reason) {
    const bool recovery_in_flight = InventoryRecoveryInFlightLocked();
    const bool source_awaiting_consumption =
        g_inventory_touch.source_event_queued &&
        !g_inventory_touch.guest_drag_seen &&
        !g_inventory_touch.release_event_queued;
    const InventoryGuestDragSnapshot guest_drag = ReadInventoryGuestDragLocked();
    if (recovery_in_flight && guest_drag.readable && !guest_drag.active &&
        source_awaiting_consumption) {
        // Remove the unconsumed pickup before retiring its owner. Waiting for
        // an inactive window to consume it would quarantine input indefinitely.
        InventoryWidgetSet widgets{};
        if (GatherInventoryWidgetsLocked(&widgets)) {
            SanitizeInventoryOwnedDragEventsLocked(widgets);
            return false;
        }
    }
    if (!warband::input::ShouldPreserveInventoryRecovery(
            recovery_in_flight, source_awaiting_consumption,
            guest_drag.readable, guest_drag.active)) {
        return false;
    }

    g_inventory_touch.interrupted = true;
    g_inventory_touch.release_pending = true;
    g_inventory_touch.restore_pending = true;
    ClearInventoryDoubleTapLocked();
    // If C34 is known active, turn every lifecycle/config interruption into a
    // source-restore request.  If the guest state is temporarily unreadable,
    // retain the existing phase instead of guessing that cleanup is safe.
    if (guest_drag.readable && guest_drag.active &&
        !g_inventory_touch.release_event_queued) {
        g_inventory_touch.release_event_queued = true;
        g_inventory_touch.restore_pending = true;
        g_inventory_touch.restore_event_queued = false;
        g_inventory_touch.final_event_ms = 0u;
    }
    const uint64_t now_ms = MonotonicMilliseconds();
    if (g_inventory_touch.restore_status_log_ms == 0u ||
        now_ms >= g_inventory_touch.restore_status_log_ms +
                      kInventoryGuestStartTimeoutMs) {
        LOGI("inventory recovery preserved reason=%s readable=%d active=%d "
             "source_awaiting=%d",
             reason ? reason : "unknown", guest_drag.readable,
             guest_drag.active, source_awaiting_consumption);
        g_inventory_touch.restore_status_log_ms = now_ms;
    }
    return true;
}

#if defined(WARBAND_BRIDGE_NO_LOGS)
constexpr bool InventoryTraceEnabled() {
    return false;
}

constexpr bool ScrollTraceEnabled() {
    return false;
}
#else
bool InventoryTraceEnabled() {
    char value[2] = {0, 0};
    __system_property_get("debug.warband.inv_trace", value);
    return value[0] == '1';
}

bool ScrollTraceEnabled() {
    char value[2] = {0, 0};
    __system_property_get("debug.warband.scroll_trace", value);
    return value[0] == '1';
}
#endif

bool DispatchInventoryDropLocked(
    const warband::input::InventoryDropPlan& plan) {
    if (!plan.HasDispatch() || !g_host) return false;

    // Validate every component before writing any bit.  A partial slot/menu
    // dispatch is not recoverable: the stock menu is the gate for its child,
    // so accepting just one component can leave target and rollback bits live
    // together on a later frame.
    if (plan.slot) {
        if (!ValidGuestPointerLocked(plan.slot) ||
            !GuestAddressRange(*g_host, plan.slot + kWidgetEventOffset,
                               sizeof(uint32_t) * 2u) ||
            g_host->MemoryRead32(plan.slot + kWidgetVtableOffset) !=
                g_inventory_slot_vtable) {
            return false;
        }
    }
    if (plan.menu) {
        if (!ValidGuestPointerLocked(plan.menu) ||
            !GuestAddressRange(*g_host, plan.menu + kWidgetEventOffset,
                               sizeof(uint32_t) * 2u) ||
            g_host->MemoryRead32(plan.menu + kWidgetVtableOffset) !=
                g_inventory_menu_vtable) {
            return false;
        }
    }

    // Queue both exact event bits in one frame.  With the validation above and
    // the bridge lock held, these writes are effectively atomic to the guest.
    if (plan.slot) SendInventoryWidgetDragEventLocked(plan.slot);
    if (plan.menu) SendInventoryWidgetDragEventLocked(plan.menu);
    return true;
}

bool QueueInventorySourceRestoreLocked(uint64_t now_ms, bool trace,
                                        const char* reason) {
    g_inventory_touch.release_event_queued = true;
    g_inventory_touch.restore_pending = true;

    const bool should_log =
        g_inventory_touch.restore_status_log_ms == 0u ||
        now_ms >= g_inventory_touch.restore_status_log_ms +
                      kInventoryGuestStartTimeoutMs;
    g_inventory_touch.final_event_ms = now_ms;

    const auto& source = g_inventory_touch.source_ref;
    uint32_t source_item = 0;
    const InventoryGuestDragSnapshot guest_drag = ReadInventoryGuestDragLocked();
    if (!g_inventory_check_slot || !g_inventory_refresh_menus ||
        !InventorySourceContextIsLiveLocked() || !guest_drag.readable || !guest_drag.active ||
        !ResolveIndexedInventoryItemLocked(source.context, source.menu, source.index,
                                            &source_item) ||
        static_cast<int32_t>(g_host->MemoryRead32(source_item)) != -1) {
        if (should_log) {
            LOGE("inventory indexed restore waiting reason=%s menu=%d index=%d",
                 reason ? reason : "unknown", source.menu, source.index);
            g_inventory_touch.restore_status_log_ms = now_ms;
        }
        return false;
    }
    // Cancel pending widget bits before the synchronous native restore; a
    // destination queued before an interruption must not pick up a second item.
    InventoryWidgetSet widgets{};
    if (!GatherInventoryWidgetsLocked(&widgets)) return false;
    SanitizeInventoryOwnedDragEventsLocked(widgets);
    g_host->CallGuestNested(g_inventory_check_slot,
        {source.context.window, source_item, static_cast<uint32_t>(source.menu),
         static_cast<uint32_t>(source.index)});
    // An occupied bag target can leave an incompatible displaced item held
    // after dragging out of equipment. Reverse that stock swap, then restore
    // the original equipment item, instead of repeatedly trying the wrong type.
    const auto& drop = g_inventory_touch.drop_ref;
    const auto remaining = ReadInventoryGuestDragLocked();
    uint32_t target_item = 0;
    if (remaining.readable && remaining.active && drop.kind >= 0 &&
        drop.context == source.context && InventorySourceContextIsLiveLocked() &&
        remaining.source == drop.kind &&
        static_cast<uint32_t>(remaining.kind) == drop.metadata &&
        ResolveIndexedInventoryItemLocked(drop.context, drop.menu, drop.index, &target_item) &&
        static_cast<int32_t>(g_host->MemoryRead32(target_item)) == source.kind &&
        g_host->MemoryRead32(target_item + 4u) == source.metadata &&
        static_cast<int32_t>(g_host->MemoryRead32(source_item)) == -1) {
        g_host->CallGuestNested(g_inventory_check_slot,
            {drop.context.window, target_item, static_cast<uint32_t>(drop.menu),
             static_cast<uint32_t>(drop.index)});
        const auto original = ReadInventoryGuestDragLocked();
        if (InventorySourceContextIsLiveLocked() && original.readable && original.active &&
            original.source == source.kind &&
            static_cast<uint32_t>(original.kind) == source.metadata &&
            static_cast<int32_t>(g_host->MemoryRead32(source_item)) == -1) {
            g_host->CallGuestNested(g_inventory_check_slot,
                {source.context.window, source_item, static_cast<uint32_t>(source.menu),
                 static_cast<uint32_t>(source.index)});
            g_pending_input_toast = static_cast<uint32_t>(
                warband::input::InventoryQuickTransferResult::GuestRejected);
        }
    }
    if (InventorySourceContextIsLiveLocked())
        g_host->CallGuestNested(g_inventory_refresh_menus, {source.context.window});
    g_inventory_touch.restore_event_queued = true;
    if (trace && should_log) {
        LOGI("inventory indexed source restore reason=%s menu=%d index=%d active=%d",
             reason ? reason : "unknown", source.menu, source.index,
             ReadInventoryGuestDragLocked().active);
        g_inventory_touch.restore_status_log_ms = now_ms;
    }
    return true;
}

void CompleteInventoryGestureLocked() {
    if (OwnsActiveInventoryTouchLocked()) {
        MaintainInventoryScrollPositionLocked();
        g_inventory_navigation_cleanup_pending = true;
        const uint32_t keys = GuestKeys();
        if (keys && GuestAddressRange(*g_host, keys + kTouchscreenActiveFlag, 1u))
            g_host->MemoryWrite8(keys + kTouchscreenActiveFlag, 1u);
    }
    CancelInventoryTouchLocked();
}

void ApplyInventoryQuickTransferLocked() {
    using Result = warband::input::InventoryQuickTransferResult;
    const InventorySourceRef source = g_inventory_touch.source_ref;
    uint32_t source_item = 0, kind_count = 0, kinds = 0;
    if (!g_inventory_check_slot || !g_inventory_refresh_menus ||
        !InventorySourceContextIsLiveLocked() ||
        !ResolveIndexedInventoryItemLocked(source.context, source.menu, source.index,
                                            &source_item) ||
        static_cast<int32_t>(g_host->MemoryRead32(source_item)) != source.kind ||
        g_host->MemoryRead32(source_item + 4u) != source.metadata ||
        !ReadInventoryWordLocked(g_inventory_item_kind_count, &kind_count) ||
        source.kind < 0 || static_cast<uint32_t>(source.kind) >= kind_count ||
        !ReadInventoryWordLocked(g_inventory_item_kinds, &kinds)) {
        g_pending_input_toast = static_cast<uint32_t>(Result::InvalidSource);
        CancelInventoryTouchLocked();
        return;
    }
    const auto before = ReadInventoryGuestDragLocked();
    if (!before.readable || before.active) {
        g_pending_input_toast = static_cast<uint32_t>(Result::GuestRejected);
        CancelInventoryTouchLocked();
        return;
    }
    const uint64_t flags_address = static_cast<uint64_t>(kinds) +
        static_cast<uint64_t>(source.kind) * 0xC30u + 0xBC8u;
    uint32_t flags_low = 0, flags_high = 0;
    if (!ReadInventoryWordLocked(flags_address, &flags_low) ||
        !ReadInventoryWordLocked(flags_address + 4u, &flags_high)) {
        g_pending_input_toast = static_cast<uint32_t>(Result::InvalidSource);
        CancelInventoryTouchLocked();
        return;
    }
    std::array<int32_t, 96> own_bag{}, other_bag{};
    std::array<int32_t, 10> equipment{};
    own_bag.fill(-2); other_bag.fill(-2); equipment.fill(-2);
    const int own_capacity = InventoryBagCapacityLocked(source.context, 0);
    const int other_capacity = source.context.bag_count > 1u
        ? InventoryBagCapacityLocked(source.context, 1) : 0;
    for (int pane = 0; pane < 3; ++pane) {
        const int capacity = pane == 0 ? own_capacity : pane == 1 ? other_capacity : 10;
        for (int index = 0; index < capacity; ++index) {
            uint32_t item = 0, widget = 0, vtable = 0, widget_flags = 0;
            if (!ResolveIndexedInventoryItemLocked(source.context, pane, index,
                                                    &item, &widget) ||
                !ReadInventoryWordLocked(widget, &vtable) || vtable != g_inventory_slot_vtable ||
                !ReadInventoryWordLocked(static_cast<uint64_t>(widget) + kWidgetFlagsOffset,
                                         &widget_flags) || (widget_flags & 3u) != 0u) continue;
            const int32_t kind = static_cast<int32_t>(g_host->MemoryRead32(item));
            if (pane == 0) own_bag[static_cast<size_t>(index)] = kind;
            else if (pane == 1) other_bag[static_cast<size_t>(index)] = kind;
            else equipment[static_cast<size_t>(index)] = kind;
        }
    }
    bool can_equip = true;
    if (source.context.mode == 0 && source.menu == 0) {
        const uint64_t troop_address = static_cast<uint64_t>(source.context.troop_table) +
            static_cast<uint64_t>(source.context.own_troop) * 0xFC8u;
        if (!GuestAddressRange(*g_host, troop_address, 0x288u)) can_equip = false;
        else if ((g_host->MemoryRead8(static_cast<uint32_t>(troop_address) + 0x248u) & 0x10u) != 0u)
            can_equip = g_inventory_can_equip && g_host->CallGuestNested(
                g_inventory_can_equip, {static_cast<uint32_t>(troop_address), source_item}) == 1u;
    }
    const auto plan = warband::input::PlanInventoryQuickTransfer(
        source.context.mode, source.menu, source.index, flags_low & 0xFFu,
        (static_cast<uint64_t>(flags_high) << 32u) | flags_low,
        std::span<const int32_t>(own_bag.data(), static_cast<size_t>(own_capacity)),
        std::span<const int32_t>(other_bag.data(), static_cast<size_t>(other_capacity)),
        equipment, can_equip);
    if (!plan.HasDestination()) {
        g_pending_input_toast = static_cast<uint32_t>(plan.result);
        CancelInventoryTouchLocked();
        return;
    }
    uint32_t target_item = 0;
    if (!ResolveIndexedInventoryItemLocked(source.context, plan.menu, plan.slot, &target_item) ||
        static_cast<int32_t>(g_host->MemoryRead32(target_item)) != -1) {
        g_pending_input_toast = static_cast<uint32_t>(Result::InvalidSource);
        CancelInventoryTouchLocked();
        return;
    }
    g_inventory_touch.drag_started = true;
    MaintainInventoryScrollPositionLocked();
    InventoryWidgetSet widgets{};
    if (!GatherInventoryWidgetsLocked(&widgets)) {
        g_pending_input_toast = static_cast<uint32_t>(Result::InvalidSource);
        CancelInventoryTouchLocked();
        return;
    }
    SanitizeInventoryOwnedDragEventsLocked(widgets);
    g_host->CallGuestNested(g_inventory_check_slot,
        {source.context.window, source_item, static_cast<uint32_t>(source.menu),
         static_cast<uint32_t>(source.index)});
    g_inventory_touch.source_event_queued = true;
    g_inventory_touch.source_event_ms = MonotonicMilliseconds();
    auto held = ReadInventoryGuestDragLocked();
    g_inventory_touch.guest_drag_seen = held.readable && held.active;
    const bool picked_source = held.readable && held.active &&
        held.source == source.kind && static_cast<uint32_t>(held.kind) == source.metadata;
    if (picked_source && InventorySourceContextIsLiveLocked() &&
        static_cast<int32_t>(g_host->MemoryRead32(target_item)) == -1) {
        g_host->CallGuestNested(g_inventory_check_slot,
            {source.context.window, target_item, static_cast<uint32_t>(plan.menu),
             static_cast<uint32_t>(plan.slot)});
        held = ReadInventoryGuestDragLocked();
    }
    if (!picked_source || !held.readable || held.active)
        g_pending_input_toast = static_cast<uint32_t>(Result::GuestRejected);
    if (held.readable && held.active) {
        g_inventory_touch.interrupted = true;
        g_inventory_touch.release_pending = true;
        QueueInventorySourceRestoreLocked(MonotonicMilliseconds(), InventoryTraceEnabled(),
                                           "double tap rejected");
        held = ReadInventoryGuestDragLocked();
    }
    if (InventorySourceContextIsLiveLocked())
        g_host->CallGuestNested(g_inventory_refresh_menus, {source.context.window});
    if (held.readable && !held.active) CompleteInventoryGestureLocked();
    else {
        g_inventory_touch.interrupted = true;
        g_inventory_touch.restore_pending = true;
        g_inventory_touch.release_pending = true;
    }
}

void MaybeOpenInventoryLongPressLocked(uint64_t now_ms, bool trace) {
    if (!g_inventory_touch.tracking || g_inventory_touch.drag_started ||
        g_inventory_touch.long_press_shown || g_inventory_touch.release_pending ||
        now_ms < g_inventory_touch.down_ms + kInventoryLongPressMs) {
        return;
    }
    const float dx = g_inventory_touch.last_x - g_inventory_touch.down_x;
    const float dy = g_inventory_touch.last_y - g_inventory_touch.down_y;
    if (std::sqrt(dx * dx + dy * dy) >= InventoryDragSlopLocked()) return;

    InventoryWidgetSet widgets{};
    uint32_t slot = 0;
    uint32_t menu = 0;
    if (!ResolveInventoryPointLocked(
            g_inventory_touch.down_x, g_inventory_touch.down_y,
            &widgets, &slot, &menu) || !slot) {
        return;
    }
    if (!PinInventoryGuestCursorForTouchLocked(
            g_inventory_touch.down_x, g_inventory_touch.down_y)) {
        return;
    }

    g_inventory_touch.source_slot = slot;
    g_inventory_touch.source_menu = menu;
    g_inventory_touch.source_resolved = true;
    g_inventory_touch.long_press_shown = true;
    ClearInventoryDoubleTapLocked();
    g_inventory_touch.owns_guest_stream = true;
    SanitizeInventoryTransientEventsLocked(widgets);
    g_inventory_touch.native_events_sanitized = true;
    if (!QueueInventoryItemOverlayLocked(widgets, slot)) {
        CancelInventoryTouchLocked();
        return;
    }
    if (trace) {
        LOGI("inventory long press queued native item details slot=0x%08X "
             "menu=0x%08X", slot, menu);
    }
}

void ApplyInventoryDragLocked() {
    if (!g_inventory_touch.tracking) return;

    uint32_t active_window_id = kNoActiveWindowId;
    if (!g_host || !g_game_screen ||
        !ReadStableActiveWindowId(*g_host, g_game_screen, &active_window_id) ||
        active_window_id != kInventoryWindowId) {
        ClearInventoryDoubleTapLocked();
        if (PreserveInventoryRecoveryLocked("active window unavailable")) {
            const uint64_t now_ms = MonotonicMilliseconds();
            if (g_inventory_touch.final_event_ms == 0u ||
                now_ms >= g_inventory_touch.final_event_ms + kInventoryRestoreRetryMs)
                QueueInventorySourceRestoreLocked(now_ms,
                    InventoryTraceEnabled(), "inventory inactive");
            const auto restored = ReadInventoryGuestDragLocked();
            if (restored.readable && !restored.active)
                CompleteInventoryGestureLocked();
            return;
        }
        CancelInventoryTouchLocked();
        return;
    }

    const bool trace = InventoryTraceEnabled();
    const uint64_t now_ms = MonotonicMilliseconds();
    if (g_inventory_touch.interrupted) {
        if (!PreserveInventoryRecoveryLocked("interrupted gesture")) {
            CompleteInventoryGestureLocked();
            return;
        }
        if (g_inventory_touch.final_event_ms == 0u ||
            now_ms >= g_inventory_touch.final_event_ms + kInventoryRestoreRetryMs)
            QueueInventorySourceRestoreLocked(now_ms, trace, "interrupted gesture");
        const auto restored = ReadInventoryGuestDragLocked();
        if (restored.readable && !restored.active &&
            g_inventory_touch.restore_event_queued) CompleteInventoryGestureLocked();
        return;
    }
    if (!g_inventory_touch.drag_started) {
        if (g_inventory_touch.tap_details_pending) {
            if (g_inventory_touch.quick_transfer_pending) {
                ApplyInventoryQuickTransferLocked();
                return;
            }
            InventoryWidgetSet widgets{};
            uint32_t slot = 0;
            uint32_t menu = 0;
            const bool resolved = ResolveInventoryPointLocked(
                g_inventory_touch.down_x, g_inventory_touch.down_y,
                &widgets, &slot, &menu);
            if (warband::input::ShouldClaimInventoryItemGesture(
                    resolved, slot, menu)) {
                SanitizeInventoryTransientEventsLocked(widgets);
                if (QueueInventoryItemOverlayLocked(widgets, slot) && trace) {
                    LOGI("inventory short tap queued details only slot=0x%08X "
                         "menu=0x%08X", slot, menu);
                }
            }
            CancelInventoryTouchLocked();
            return;
        }
        if (g_inventory_touch.long_press_shown &&
            g_inventory_touch.release_pending) {
            InventoryWidgetSet widgets{};
            if (GatherInventoryWidgetsLocked(&widgets)) {
                SanitizeInventoryTransientEventsLocked(widgets);
            }
            CancelInventoryTouchLocked();
            return;
        }
        MaybeOpenInventoryLongPressLocked(now_ms, trace);
        return;
    }
    InventoryGuestDragSnapshot guest_drag = ReadInventoryGuestDragLocked();

    if (g_inventory_touch.release_event_queued) {
        // The final event was queued after the prior Inventory_window::check
        // and must have been consumed before this frame hook.  A cleared C34
        // is positive proof that the drop completed.
        const auto release_action =
            warband::input::DecideInventoryReleaseAction(
                guest_drag.readable, guest_drag.active);
        if (release_action ==
            warband::input::InventoryReleaseAction::Complete) {
            if (trace) {
                LOGI("inventory release consumed source=%d kind=%d restored=%d",
                     guest_drag.source, guest_drag.kind,
                     g_inventory_touch.restore_event_queued);
            }
            // The next controller update would otherwise run after the owned
            // gesture has been discarded, select its cached first slot and
            // leave that slot green.  Give it one final touch-owned pass; the
            // post-call hook clears both the flag and cached widget pointer.
            CompleteInventoryGestureLocked();
            return;
        }

        if (release_action ==
            warband::input::InventoryReleaseAction::RestoreSource) {
            // A still-active C34 means either an occupied destination left its
            // displaced item on the stock cursor or a rollback has not yet
            // been consumed.  Recovery is level-triggered and idempotent: on
            // every retry we re-resolve an empty source and OR the same bits.
            // Never terminally cancel while the guest proves it holds an item.
            if (!g_inventory_touch.restore_pending) {
                g_inventory_touch.restore_pending = true;
                g_inventory_touch.restore_event_queued = false;
                g_inventory_touch.final_event_ms = 0u;
                if (trace) {
                    LOGI("inventory destination left item active; entering "
                         "source restore source=%d kind=%d",
                         guest_drag.source, guest_drag.kind);
                }
            }
            if (g_inventory_touch.final_event_ms == 0u ||
                now_ms >= g_inventory_touch.final_event_ms +
                              kInventoryRestoreRetryMs) {
                QueueInventorySourceRestoreLocked(
                    now_ms, trace, "active guest item after release");
            }
        }
        return;
    }

    InventoryWidgetSet widgets{};
    bool source_just_queued = false;
    if (!g_inventory_touch.source_resolved) {
        const auto& source = g_inventory_touch.source_ref;
        uint32_t source_item = 0, source_slot = 0, source_menu = 0;
        if (!InventorySourceContextIsLiveLocked() ||
            !ResolveIndexedInventoryItemLocked(source.context, source.menu, source.index,
                                                &source_item, &source_slot, &source_menu) ||
            !source_slot || !source_menu ||
            static_cast<int32_t>(g_host->MemoryRead32(source_item)) != source.kind ||
            g_host->MemoryRead32(source_item + 4u) != source.metadata ||
            !GatherInventoryWidgetsLocked(&widgets)) {
            CancelInventoryTouchLocked();
            return;
        }

        g_inventory_touch.source_slot = source_slot;
        g_inventory_touch.source_menu = source_menu;
        g_inventory_touch.source_resolved = true;
        if (trace) {
            LOGI("inventory source resolved root=0x%08X menu=0x%08X "
                 "slot=0x%08X release=%d",
                 widgets.root, g_inventory_touch.source_menu,
                 g_inventory_touch.source_slot,
                 g_inventory_touch.release_pending);
            LogInventoryWidgetGeometryLocked("source_slot",
                                             g_inventory_touch.source_slot);
            LogInventoryWidgetGeometryLocked("source_menu",
                                             g_inventory_touch.source_menu);
        }
    }

    // The claimed MOVE has already delivered a single CANCEL.  Remove every
    // stale click/drag bit left by the original DOWN before queueing exactly
    // one source event; this is what prevents item-specific mesh stacks from
    // replaying the pickup sound two or three times.
    if (!g_inventory_touch.native_events_sanitized) {
        if (!widgets.root && !GatherInventoryWidgetsLocked(&widgets)) return;
        SanitizeInventoryTransientEventsLocked(widgets);
        g_inventory_touch.native_events_sanitized = true;
    }

    // Every native MOVE over an inventory slot can enqueue handle_event(16),
    // i.e. another stock 0x10000 operation.  Once our one source event is
    // live, scrub those freshly generated bits on every frame before the
    // inventory window consumes them.  This both prevents repeated pickup
    // sounds and stops a second source event from immediately returning C34.
    if (g_inventory_touch.source_event_queued) {
        if (!widgets.root && !GatherInventoryWidgetsLocked(&widgets)) return;
        SanitizeInventoryOwnedDragEventsLocked(widgets);
    }

    // Pick the item up while the finger is still held.  The stock C34 state
    // owns the dragged icon; the source's native overlay event below supplies
    // its information panel. Subsequent frames only move the guest cursor and
    // never replay the source slot event.
    if (!g_inventory_touch.source_event_queued) {
        if (!widgets.root && !GatherInventoryWidgetsLocked(&widgets)) return;
        if (!PinInventoryGuestCursorForTouchLocked(
                g_inventory_touch.down_x, g_inventory_touch.down_y)) {
            return;
        }
        // check_inventory_menu handles event 0x2 before event 0x10000.  Queue
        // both on the still-populated source slot so the native details panel
        // is built from the real Item immediately before check_slot removes it
        // from that cell.  The overlay then follows the same held C34 item as
        // the stock mesh, without replaying the pickup event or its sound.
        if (!QueueInventoryItemOverlayLocked(
                widgets, g_inventory_touch.source_slot)) {
            LOGE("inventory source overlay dispatch failed");
            CancelInventoryTouchLocked();
            return;
        }
        const warband::input::InventoryDropPlan source_press{
            .menu = g_inventory_touch.source_menu,
            .slot = g_inventory_touch.source_slot,
        };
        // The event bits are consumed on the next guest frame, not by this
        // call.  Pin the guest cursor at DOWN across that frame so the mobile
        // hover path and the source event agree on the same item.
        if (!DispatchInventoryDropLocked(source_press)) {
            LOGE("inventory source event dispatch failed");
            CancelInventoryTouchLocked();
            return;
        }
        g_inventory_touch.source_event_queued = true;
        g_inventory_touch.source_event_ms = now_ms;
        source_just_queued = true;
        if (trace) {
            LOGI("inventory live source queued menu=0x%08X slot=0x%08X",
                  source_press.menu, source_press.slot);
        }
    }

    // This frame hook runs before Inventory_window::check.  Keep touchscreen
    // mode asserted through its rglWidget_gamepad_controller::update call so
    // the stock controller clears (rather than creates) the green first-cell
    // highlight and does not move the cursor or list.  A narrow post-call hook
    // clears the flag immediately afterwards, before check_inventory_menu can
    // interpret hover as an implicit drop.  Explicit 0x10000 source and
    // destination events remain the only operations that move an item.
    const uint32_t keys = GuestKeys();
    if (keys && GuestAddressRange(*g_host, keys + kTouchscreenActiveFlag,
                                  sizeof(uint8_t))) {
        const uint8_t touch_flag =
            g_host->MemoryRead8(keys + kTouchscreenActiveFlag);
        if (trace && source_just_queued) {
            LOGI("inventory navigation highlight suppressed touch_flag=%u",
                 touch_flag);
        }
        if (touch_flag != 1u) {
            g_host->MemoryWrite8(keys + kTouchscreenActiveFlag, 1u);
        }
    }

    // Crucially, do not replay the source slot as a per-frame keep-alive.
    // IDA shows that check_slot sees C34=1 and treats that second source event
    // as a destination, immediately returning the item and clearing C34.
    guest_drag = ReadInventoryGuestDragLocked();
    if (guest_drag.readable && guest_drag.active) {
        g_inventory_touch.guest_drag_seen = true;
        if (!g_inventory_touch.release_pending) {
            // This drives the stock held-item mesh and information panel with
            // the live finger position without generating another slot event.
            const bool pinned = PinInventoryGuestCursorForTouchLocked(
                g_inventory_touch.last_x, g_inventory_touch.last_y);
            if (trace && pinned && g_window_manager &&
                (g_inventory_touch.cursor_trace_frames < 8u ||
                 g_inventory_touch.cursor_trace_frames % 30u == 0u)) {
                LOGI("inventory held cursor touch=(%.1f,%.1f) "
                     "guest=(%.4f,%.4f) surface=%ux%u scale=%.4f",
                     g_inventory_touch.last_x, g_inventory_touch.last_y,
                     ReadGuestFloatLocked(
                         g_window_manager + kWindowManagerCursorXOffset),
                     ReadGuestFloatLocked(
                         g_window_manager + kWindowManagerCursorYOffset),
                     g_host->MemoryRead32(
                         g_window_manager + kWindowManagerSurfaceWidthOffset),
                     g_host->MemoryRead32(
                         g_window_manager + kWindowManagerSurfaceHeightOffset),
                     ReadGuestFloatLocked(
                         g_window_manager + kWindowManagerCoordinateScaleOffset));
            }
            ++g_inventory_touch.cursor_trace_frames;
        }
    }

    const auto phase = warband::input::DecideInventoryDragPhase(
        g_inventory_touch.release_pending, source_just_queued,
        g_inventory_touch.guest_drag_seen, guest_drag.readable,
        guest_drag.active);
    if (phase == warband::input::InventoryDragPhaseAction::AbortLostGuestDrag) {
        if (trace) {
            LOGI("inventory guest drag ended before bridge release source=%d kind=%d",
                 guest_drag.source, guest_drag.kind);
        }
        CancelInventoryTouchLocked();
        return;
    }
    if (phase == warband::input::InventoryDragPhaseAction::Wait) {
        if (g_inventory_touch.source_event_queued &&
            !g_inventory_touch.guest_drag_seen &&
            now_ms >= g_inventory_touch.source_event_ms +
                          kInventoryGuestStartTimeoutMs) {
            if (guest_drag.readable && !guest_drag.active) {
                if (trace) {
                    LOGI("inventory completed without an observable intermediate "
                         "drag state");
                }
                CancelInventoryTouchLocked();
            } else if (!guest_drag.readable && trace) {
                LOGI("inventory source acknowledgement waiting for readable "
                     "guest state");
            }
        }
        return;
    }

    // Release is the only time we traverse the destination widgets.  This
    // avoids the expensive 512-widget walk on every held frame and prevents
    // intermediate occupied cells from consuming the item.
    if (!GatherInventoryWidgetsLocked(&widgets)) {
        if (guest_drag.readable && guest_drag.active &&
            now_ms >= g_inventory_touch.source_event_ms +
                          kInventoryGuestStartTimeoutMs) {
            LOGE("inventory destination widget gathering timed out");
            QueueInventorySourceRestoreLocked(
                now_ms, trace, "destination widget gathering timeout");
        }
        return;
    }
    uint32_t displaced_cursor_x = 0;
    uint32_t displaced_cursor_y = 0;
    if (!SetInventoryGuestCursorForTouchLocked(
            g_inventory_touch.last_x, g_inventory_touch.last_y,
            &displaced_cursor_x, &displaced_cursor_y)) {
        if (guest_drag.readable && guest_drag.active &&
            now_ms >= g_inventory_touch.source_event_ms +
                          kInventoryGuestStartTimeoutMs) {
            LOGE("inventory destination cursor preparation timed out");
            QueueInventorySourceRestoreLocked(
                now_ms, trace, "destination cursor preparation timeout");
        }
        return;
    }

    size_t slot_hits = 0;
    size_t menu_hits = 0;
    uint32_t current_slot_hit = 0;
    uint32_t current_menu_hit = 0;
    for (size_t index = 0; index < widgets.slot_count; ++index) {
        const uint32_t widget = widgets.slots[index];
        if (InventoryWidgetUnderCursorLocked(widget)) {
            if (!current_slot_hit) current_slot_hit = widget;
            ++slot_hits;
        }
    }
    for (size_t index = 0; index < widgets.menu_count; ++index) {
        const uint32_t widget = widgets.menus[index];
        if (InventoryWidgetUnderCursorLocked(widget)) {
            if (!current_menu_hit) current_menu_hit = widget;
            ++menu_hits;
        }
    }
    const auto drop = warband::input::PlanInventoryDrop(
        g_inventory_touch.source_menu, g_inventory_touch.source_slot,
        current_menu_hit, current_slot_hit);
    g_inventory_touch.drop_ref = {};
    if (drop.slot != 0u)
        CaptureInventorySourceRefLocked(drop.slot, drop.menu,
                                         &g_inventory_touch.drop_ref, false);
    if (!drop.HasDispatch()) {
        LOGE("inventory release has no safe source or destination widget");
        QueueInventorySourceRestoreLocked(
            now_ms, trace, "release has no safe destination");
        return;
    }
    if (drop.restores_source) {
        // Never dispatch cached source pointers directly.  The stock refresh
        // can rebuild widgets, and an occupied source would exchange the held
        // item again.  The recovery helper re-resolves a live empty source,
        // pins DOWN, and atomically queues its menu+slot pair.
        QueueInventorySourceRestoreLocked(
            now_ms, trace, "release outside a concrete destination");
        return;
    }
    // A real destination remains pinned at the release point.  The exact
    // event and the mobile hover path therefore agree on one slot.
    if (!DispatchInventoryDropLocked(drop)) {
        LOGE("inventory destination dispatch failed");
        QueueInventorySourceRestoreLocked(
            now_ms, trace, "destination dispatch failure");
        return;
    }
    g_inventory_touch.release_event_queued = true;
    g_inventory_touch.restore_event_queued = drop.restores_source;
    g_inventory_touch.restore_pending = drop.restores_source;
    g_inventory_touch.final_event_ms = now_ms;

    if (trace) {
        LOGI("inventory release queued root=0x%08X source_slot=0x%08X "
             "source_menu=0x%08X target_slot=0x%08X target_menu=0x%08X "
             "slot_hits=%zu menu_hits=%zu restore=%d touch=%.1f,%.1f "
             "guest_source=%d guest_kind=%d",
             widgets.root, g_inventory_touch.source_slot,
             g_inventory_touch.source_menu, drop.slot, drop.menu,
             slot_hits, menu_hits, drop.restores_source,
             g_inventory_touch.last_x, g_inventory_touch.last_y,
             guest_drag.source, guest_drag.kind);
        LogInventoryWidgetGeometryLocked("target_slot", current_slot_hit);
        LogInventoryWidgetGeometryLocked("target_menu", current_menu_hit);
    }
}

void ApplyInventoryPostGamepadLocked(uint32_t controller) {
    const bool owns_drag = OwnsActiveInventoryTouchLocked() && g_inventory_touch.tracking &&
        g_inventory_touch.drag_started &&
        g_inventory_touch.owns_guest_stream &&
        g_inventory_touch.source_event_queued;
    const bool cleanup = g_inventory_navigation_cleanup_pending;
    if ((!owns_drag && !cleanup) || !g_host) {
        return;
    }

    uint32_t active_window_id = kNoActiveWindowId;
    if (!g_game_screen ||
        !ReadStableActiveWindowId(*g_host, g_game_screen, &active_window_id) ||
        active_window_id != kInventoryWindowId) {
        const uint32_t keys = GuestKeys();
        if (keys && GuestAddressRange(*g_host,
                                      keys + kTouchscreenActiveFlag,
                                      sizeof(uint8_t))) {
            g_host->MemoryWrite8(keys + kTouchscreenActiveFlag, 0u);
        }
        g_inventory_navigation_cleanup_pending = false;
        return;
    }

    // The original gamepad-controller update has now consumed the temporary
    // touchscreen mode and cleared its active highlight.  Remove that mode
    // before check_inventory_menu reaches the mobile hover-drop branch.
    uint8_t touch_flag = 0u;
    const uint32_t keys = GuestKeys();
    if (keys && GuestAddressRange(*g_host, keys + kTouchscreenActiveFlag,
                                  sizeof(uint8_t))) {
        touch_flag = g_host->MemoryRead8(keys + kTouchscreenActiveFlag);
        if (touch_flag != 0u) {
            g_host->MemoryWrite8(keys + kTouchscreenActiveFlag, 0u);
        }
    }

    // update() stores its current keyboard/gamepad widget in the controller's
    // first word.  A slot event can repopulate this cache after touch began;
    // leaving it alive is what reselects the first visible cell one frame after
    // release.  Touch dragging has no use for that navigation cursor, and a
    // later real gamepad direction rebuilds it through the stock path.
    if (ValidGuestPointerLocked(controller) &&
        GuestAddressRange(*g_host, controller, sizeof(uint32_t))) {
        g_host->MemoryWrite32(controller, 0u);
    }
    g_inventory_navigation_cleanup_pending = false;

    if (!owns_drag) {
        return;
    }

    // update() is the last stock navigation code allowed to touch the cursor.
    // Re-pin it afterwards so the native held mesh is rendered at the live
    // finger.  The first pickup frame intentionally stays on DOWN until the
    // source event has created C34; every held/release frame uses the live tip.
    const InventoryGuestDragSnapshot guest_drag =
        ReadInventoryGuestDragLocked();
    const bool use_live_tip = g_inventory_touch.release_pending ||
        (guest_drag.readable && guest_drag.active);
    const float pointer_x = use_live_tip
        ? g_inventory_touch.last_x : g_inventory_touch.down_x;
    const float pointer_y = use_live_tip
        ? g_inventory_touch.last_y : g_inventory_touch.down_y;
    const bool pinned = PinInventoryGuestCursorForTouchLocked(
        pointer_x, pointer_y);

    if (InventoryTraceEnabled() &&
        (g_inventory_touch.post_gamepad_trace_frames < 8u ||
         g_inventory_touch.post_gamepad_trace_frames % 30u == 0u)) {
        LOGI("inventory post-gamepad guard touch_flag=%u active=%d "
             "release=%d pinned=%d touch=(%.1f,%.1f)",
             touch_flag, guest_drag.readable && guest_drag.active,
             g_inventory_touch.release_pending, pinned, pointer_x, pointer_y);
    }
    ++g_inventory_touch.post_gamepad_trace_frames;
}

bool IsTouchHighlightWindow(uint32_t window_id) {
    return window_id == warband::input::kConversationWindowId ||
           window_id == warband::input::kInitialWindowId ||
           window_id == warband::input::kMenuWindowId;
}

bool ValidTouchHighlightWidgetLocked(uint32_t widget) {
    return g_host && g_simple_button_vtable &&
        ValidGuestPointerLocked(widget) &&
        GuestAddressRange(*g_host, widget,
                          kWidgetHighlightFlagOffset + sizeof(uint8_t)) &&
        GuestAddressRange(*g_host, widget + kWidgetFlagsOffset,
                          sizeof(uint32_t)) &&
        g_host->MemoryRead32(widget + kWidgetVtableOffset) ==
            g_simple_button_vtable &&
        (g_host->MemoryRead32(widget + kWidgetFlagsOffset) & 0x3u) == 0u;
}

void SetTouchHighlightFlagLocked(uint32_t widget, bool highlighted) {
    if (ValidTouchHighlightWidgetLocked(widget)) {
        g_host->MemoryWrite8(widget + kWidgetHighlightFlagOffset,
                             highlighted ? 1u : 0u);
    }
}

void ReleaseTouchWidgetHighlightOwnershipLocked() {
    SetTouchHighlightFlagLocked(g_touch_widget_highlight.applied_widget, false);
    for (size_t index = 0;
         index < g_touch_widget_highlight.choice_widget_count; ++index) {
        SetTouchHighlightFlagLocked(
            g_touch_widget_highlight.choice_widgets[index], false);
    }
    g_touch_widget_highlight = {};
}

void ActivateTouchWidgetHighlightLocked(uint32_t window_id,
                                        uint32_t window_object) {
    if (!IsTouchHighlightWindow(window_id) ||
        !ValidGuestPointerLocked(window_object)) {
        return;
    }
    if (!g_touch_widget_highlight.active ||
        g_touch_widget_highlight.window_id != window_id ||
        g_touch_widget_highlight.window_object != window_object) {
        ReleaseTouchWidgetHighlightOwnershipLocked();
        g_touch_widget_highlight.active = true;
        g_touch_widget_highlight.window_id = window_id;
        g_touch_widget_highlight.window_object = window_object;
    }
}

void RetireTouchWidgetHighlightIfWindowChangedLocked() {
    if (!g_touch_widget_highlight.active || !g_host || !g_game_screen) return;
    uint32_t window_id = kNoActiveWindowId;
    uint32_t window_object = 0u;
    if (ReadStableActiveWindowId(*g_host, g_game_screen, &window_id) &&
        ResolveActiveWindowObjectLocked(window_id, &window_object) &&
        (window_id != g_touch_widget_highlight.window_id ||
         window_object != g_touch_widget_highlight.window_object)) {
        ReleaseTouchWidgetHighlightOwnershipLocked();
    }
}

void ApplyDialogPostGamepadLocked(uint32_t controller) {
    if (!g_host || !g_game_screen) return;

    uint32_t active_window_id = kNoActiveWindowId;
    uint32_t active_window = 0u;
    if (!ReadStableActiveWindowId(*g_host, g_game_screen, &active_window_id) ||
        !IsTouchHighlightWindow(active_window_id) ||
        !ResolveActiveWindowObjectLocked(active_window_id, &active_window)) {
        g_conversation_navigation_cleanup_pending = false;
        return;
    }
    const uint64_t expected_controller64 =
        static_cast<uint64_t>(active_window) +
        warband::input::kGamepadControllerOffset;
    if (expected_controller64 > std::numeric_limits<uint32_t>::max() ||
        controller != static_cast<uint32_t>(expected_controller64) ||
        !ValidGuestPointerLocked(controller) ||
        !GuestAddressRange(*g_host, controller, sizeof(uint32_t))) {
        g_conversation_navigation_cleanup_pending = false;
        return;
    }

    const bool touch_owner = g_touch_widget_highlight.active &&
        g_touch_widget_highlight.window_id == active_window_id &&
        g_touch_widget_highlight.window_object == active_window;
    const bool cleanup = active_window_id ==
            warband::input::kConversationWindowId &&
        g_conversation_navigation_cleanup_pending;
    if (!warband::input::ShouldSuppressConversationNavigation(
            touch_owner, cleanup)) {
        return;
    }

    const bool live_choice_touch = touch_owner && g_ui_touch.active &&
        g_ui_touch.window_id == active_window_id &&
        g_ui_touch.window_object == active_window &&
        g_ui_touch.ending_event == nullptr &&
        ((active_window_id == warband::input::kConversationWindowId &&
          g_ui_touch.mode == UiTouchMode::ConversationResponse) ||
         ((active_window_id == warband::input::kMenuWindowId ||
           active_window_id == warband::input::kInitialWindowId) &&
          g_ui_touch.mode == UiTouchMode::MenuChoiceHighlight));
    uint32_t highlight_widget = warband::input::TouchHighlightWidget(
        live_choice_touch, g_ui_touch.ending_event != nullptr,
        g_ui_touch.highlight_widget);
    if (!ValidTouchHighlightWidgetLocked(highlight_widget)) {
        highlight_widget = 0u;
    }

    // update() has just restored its desktop navigation widget and moved the
    // manager cursor to that row. Remove both of its visual channels: the
    // controller pointer drives focus-band scrolling, while +0x1F2 drives the
    // native selected-text colour independently from the outline material.
    const uint32_t controller_widget = g_host->MemoryRead32(controller);
    g_host->MemoryWrite32(controller, 0u);
    uint32_t manager_widget = 0u;
    if (g_window_manager && ValidGuestPointerLocked(g_window_manager) &&
        GuestAddressRange(*g_host,
                          g_window_manager +
                              kWindowManagerActiveHighlightOffset,
                          sizeof(uint32_t))) {
        manager_widget = g_host->MemoryRead32(
            g_window_manager + kWindowManagerActiveHighlightOffset);
    }
    const std::array<uint32_t, 3> stale_widgets = {
        controller_widget, manager_widget,
        g_touch_widget_highlight.applied_widget};
    for (const uint32_t widget : stale_widgets) {
        if (widget != highlight_widget) {
            SetTouchHighlightFlagLocked(widget, false);
        }
    }
    for (size_t index = 0;
         index < g_touch_widget_highlight.choice_widget_count; ++index) {
        const uint32_t widget =
            g_touch_widget_highlight.choice_widgets[index];
        if (widget != highlight_widget) {
            SetTouchHighlightFlagLocked(widget, false);
        }
    }
    // Menu_window has a fixed array of sixteen choices. Clear it directly as
    // well: the controller may preserve an old +0x1F2 flag before the first
    // input-side tree walk has had a chance to remember every button.
    if (active_window_id == warband::input::kMenuWindowId) {
        for (uint32_t offset = warband::input::kMenuChoiceFirstOffset;
             offset <= warband::input::kMenuChoiceLastOffset;
             offset += sizeof(uint32_t)) {
            const uint64_t field64 =
                static_cast<uint64_t>(active_window) + offset;
            if (field64 > std::numeric_limits<uint32_t>::max() ||
                !GuestAddressRange(*g_host, field64, sizeof(uint32_t))) {
                continue;
            }
            const uint32_t widget =
                g_host->MemoryRead32(static_cast<uint32_t>(field64));
            if (widget != highlight_widget) {
                SetTouchHighlightFlagLocked(widget, false);
            }
        }
    }
    // Initial_window has seven native main-menu choices.  As above, clear
    // stale selection flags directly before applying the live finger target.
    // No colour is written here: the original widget style remains yellow.
    if (active_window_id == warband::input::kInitialWindowId) {
        for (uint32_t offset = warband::input::kInitialChoiceFirstOffset;
             offset <= warband::input::kInitialChoiceLastOffset;
             offset += sizeof(uint32_t)) {
            const uint64_t field64 =
                static_cast<uint64_t>(active_window) + offset;
            if (field64 > std::numeric_limits<uint32_t>::max() ||
                !GuestAddressRange(*g_host, field64, sizeof(uint32_t))) {
                continue;
            }
            const uint32_t widget =
                g_host->MemoryRead32(static_cast<uint32_t>(field64));
            if (widget != highlight_widget) {
                SetTouchHighlightFlagLocked(widget, false);
            }
        }
    }

    if (g_set_active_widget_to_highlight && g_window_manager &&
        ValidGuestPointerLocked(g_window_manager) &&
        GuestAddressRange(*g_host, g_window_manager, sizeof(uint32_t))) {
        g_host->CallGuestNested(g_set_active_widget_to_highlight,
                                {g_window_manager, highlight_widget});
    }
    SetTouchHighlightFlagLocked(highlight_widget, true);
    g_touch_widget_highlight.applied_widget =
        touch_owner ? highlight_widget : 0u;
    if (active_window_id == warband::input::kConversationWindowId) {
        g_conversation_navigation_cleanup_pending = false;
    }

    const bool trace = ScrollTraceEnabled();
    static uint32_t reports = 0u;
    if (trace && reports++ < 160u &&
        (controller_widget != 0u || highlight_widget != 0u)) {
        LOGI("dialog touch highlight window=%u controller=0x%08X "
             "stock=0x%08X native_outline=0x%08X owner=%d live=%d",
             active_window_id, controller, controller_widget,
             highlight_widget, touch_owner, live_choice_touch);
    }
}

struct CharacterTouchTargetSet {
    std::array<warband::input::TouchTargetRect, kCharacterTouchTargetLimit> rects{};
    std::array<warband::input::TouchTargetRect, kCharacterTouchTargetLimit> clips{};
    std::array<uint32_t, kCharacterTouchTargetLimit> widgets{};
    size_t count = 0;
};

bool ResolveCharacterWindowLocked(uint32_t* character_out) {
    if (!character_out || !g_host || !g_game_screen || !g_character_window_vtable) {
        return false;
    }
    uint32_t active_window_id = kNoActiveWindowId;
    if (!ReadStableActiveWindowId(*g_host, g_game_screen, &active_window_id) ||
        active_window_id != kCharacterWindowId) {
        return false;
    }
    const uint64_t slot64 = static_cast<uint64_t>(g_game_screen) +
                            sizeof(uint32_t) * (kCharacterWindowId + 1u);
    if (slot64 > std::numeric_limits<uint32_t>::max() ||
        !GuestAddressRange(*g_host, slot64, sizeof(uint32_t))) {
        return false;
    }
    const uint32_t character = g_host->MemoryRead32(static_cast<uint32_t>(slot64));
    if (!ValidGuestPointerLocked(character) ||
        !GuestAddressRange(*g_host, character, 0x6D0u) ||
        g_host->MemoryRead32(character + kWidgetVtableOffset) !=
            g_character_window_vtable) {
        return false;
    }
    *character_out = character;
    return true;
}

bool ReadCharacterCoordinateTransformLocked(
        warband::input::WidgetCoordinateTransform* transform_out) {
    if (!transform_out || !g_host || !g_window_manager ||
        !ValidGuestPointerLocked(g_window_manager) ||
        !GuestAddressRange(*g_host, g_window_manager,
                           kWindowManagerAspectBlendOffset + sizeof(uint32_t))) {
        return false;
    }
    const int32_t width = static_cast<int32_t>(g_host->MemoryRead32(
        g_window_manager + kWindowManagerSurfaceWidthOffset));
    const int32_t height = static_cast<int32_t>(g_host->MemoryRead32(
        g_window_manager + kWindowManagerSurfaceHeightOffset));
    const float coordinate_scale = ReadGuestFloatLocked(
        g_window_manager + kWindowManagerCoordinateScaleOffset);
    if (width <= 0 || height <= 0 || !std::isfinite(coordinate_scale) ||
        coordinate_scale <= 0.0f || coordinate_scale > 4.0f) {
        return false;
    }

    warband::input::WidgetCoordinateTransform transform{
        .surface_width = static_cast<float>(width),
        .surface_height = static_cast<float>(height),
        .coordinate_scale = coordinate_scale,
        .x_factor = 1.0f,
        .y_factor = 1.0f,
    };
    const bool aspect_enabled =
        g_host->MemoryRead8(g_window_manager + kWindowManagerAspectEnabledOffset) != 0u;
    const bool aspect_allowed =
        g_host->MemoryRead8(g_window_manager + kWindowManagerAspectAllowedOffset) != 0u;
    const float aspect_blend = ReadGuestFloatLocked(
        g_window_manager + kWindowManagerAspectBlendOffset);
    if (aspect_enabled && (aspect_allowed || aspect_blend <= 0.5f)) {
        const bool four_three =
            g_host->MemoryRead8(g_window_manager + kWindowManagerFourThreeOffset) != 0u;
        const float target_aspect = four_three ? 1.3333f : 1.0f;
        const float current_aspect =
            transform.surface_width * coordinate_scale / transform.surface_height;
        if (!std::isfinite(current_aspect) || current_aspect <= 0.0f) return false;
        if (current_aspect >= target_aspect) {
            transform.x_factor = current_aspect / target_aspect;
        } else {
            transform.y_factor = target_aspect / current_aspect;
        }
    }
    if (!transform.Valid() || transform.x_factor > 8.0f ||
        transform.y_factor > 8.0f) {
        return false;
    }
    *transform_out = transform;
    return true;
}

bool ReadVisibleWidgetScreenRectLocked(
        uint32_t widget, const warband::input::WidgetCoordinateTransform& transform,
        uint32_t expected_vtable, warband::input::TouchTargetRect* rect_out,
        uint32_t rejected_flags = 0x3u) {
    if (!rect_out || !g_host || !ValidGuestPointerLocked(widget) ||
        !GuestAddressRange(*g_host, widget, kWidgetFlagsOffset + sizeof(uint32_t)) ||
        (expected_vtable != 0u &&
         g_host->MemoryRead32(widget + kWidgetVtableOffset) != expected_vtable) ||
        (g_host->MemoryRead32(widget + kWidgetFlagsOffset) & rejected_flags) != 0u) {
        return false;
    }
    const float position_x = ReadGuestFloatLocked(widget + kWidgetPositionXOffset);
    const float position_y = ReadGuestFloatLocked(widget + kWidgetPositionYOffset);
    const float left = position_x + ReadGuestFloatLocked(widget + kWidgetLowerXOffset);
    const float right = position_x + ReadGuestFloatLocked(widget + kWidgetUpperXOffset);
    const float bottom = position_y + ReadGuestFloatLocked(widget + kWidgetLowerYOffset);
    const float top = position_y + ReadGuestFloatLocked(widget + kWidgetUpperYOffset);
    const warband::input::TouchTargetRect rect =
        warband::input::WidgetRectToScreen(transform, left, bottom, right, top);
    if (!warband::input::ValidTouchTargetRect(rect)) return false;
    *rect_out = rect;
    return true;
}

bool ReadCharacterGroupClipLocked(
        uint32_t character, uint32_t board_offset,
        const warband::input::WidgetCoordinateTransform& transform,
        warband::input::TouchTargetRect* clip_out) {
    const uint64_t slot64 = static_cast<uint64_t>(character) + board_offset;
    if (!clip_out || slot64 > std::numeric_limits<uint32_t>::max() ||
        !GuestAddressRange(*g_host, slot64, sizeof(uint32_t))) {
        return false;
    }
    return ReadVisibleWidgetScreenRectLocked(
        g_host->MemoryRead32(static_cast<uint32_t>(slot64)), transform, 0u, clip_out);
}

bool AppendCharacterTouchTargetLocked(
        uint32_t widget, const warband::input::WidgetCoordinateTransform& transform,
        const warband::input::TouchTargetRect& clip,
        CharacterTouchTargetSet* targets) {
    if (!targets || targets->count >= targets->rects.size() ||
        !g_simple_button_vtable) {
        return false;
    }
    for (size_t index = 0; index < targets->count; ++index) {
        if (targets->widgets[index] == widget) return false;
    }
    warband::input::TouchTargetRect rect{};
    if (!ReadVisibleWidgetScreenRectLocked(
            widget, transform, g_simple_button_vtable, &rect)) {
        return false;
    }
    const float rect_width = rect.right - rect.left;
    const float rect_height = rect.bottom - rect.top;
    const float centre_x = rect.left + rect_width * 0.5f;
    const float centre_y = rect.top + rect_height * 0.5f;
    if (rect_width < 2.0f || rect_height < 2.0f ||
        rect_width > transform.surface_width * 0.2f ||
        rect_height > transform.surface_height * 0.2f || centre_x < 0.0f ||
        centre_x >= transform.surface_width || centre_y < 0.0f ||
        centre_y >= transform.surface_height ||
        !warband::input::TouchTargetContains(clip, centre_x, centre_y)) {
        return false;
    }
    targets->widgets[targets->count] = widget;
    targets->rects[targets->count] = rect;
    targets->clips[targets->count] = clip;
    ++targets->count;
    return true;
}

void AppendCharacterTouchTargetArrayLocked(
        uint32_t character, uint32_t offset, size_t count,
        const warband::input::WidgetCoordinateTransform& transform,
        const warband::input::TouchTargetRect& clip,
        CharacterTouchTargetSet* targets) {
    const uint64_t begin64 = static_cast<uint64_t>(character) + offset;
    const uint64_t bytes64 = count * sizeof(uint32_t);
    if (!targets || count == 0u || begin64 > std::numeric_limits<uint32_t>::max() ||
        !GuestAddressRange(*g_host, begin64, bytes64)) {
        return;
    }
    const uint32_t begin = static_cast<uint32_t>(begin64);
    for (size_t index = 0; index < count; ++index) {
        AppendCharacterTouchTargetLocked(
            g_host->MemoryRead32(begin + static_cast<uint32_t>(index * sizeof(uint32_t))),
            transform, clip, targets);
    }
}

bool GatherCharacterTouchTargetsLocked(
        uint32_t character,
        const warband::input::WidgetCoordinateTransform& transform,
        CharacterTouchTargetSet* targets) {
    if (!targets || !g_host ||
        !GuestAddressRange(*g_host,
                           static_cast<uint64_t>(character) +
                               kCharacterSkillCountOffset,
                           sizeof(uint32_t))) {
        return false;
    }
    *targets = {};
    warband::input::TouchTargetRect skill_clip{};
    warband::input::TouchTargetRect attribute_clip{};
    const warband::input::TouchTargetRect surface_clip{
        0.0f, 0.0f, transform.surface_width, transform.surface_height};
    const bool has_skill_clip = ReadCharacterGroupClipLocked(
        character, kCharacterSkillBoardOffset, transform, &skill_clip);
    const bool has_attribute_clip = ReadCharacterGroupClipLocked(
        character, kCharacterAttributeBoardOffset, transform, &attribute_clip);
    const uint32_t skill_count = g_host->MemoryRead32(
        character + kCharacterSkillCountOffset);
    if (has_skill_clip && skill_count <= kCharacterSkillButtonLimit) {
        AppendCharacterTouchTargetArrayLocked(
            character, kCharacterSkillButtonsOffset, skill_count, transform,
            skill_clip, targets);
    }
    if (has_attribute_clip) {
        AppendCharacterTouchTargetArrayLocked(
            character, kCharacterAttributeButtonsOffset,
            kCharacterAttributeButtonCount, transform, attribute_clip, targets);
    }
    AppendCharacterTouchTargetArrayLocked(
        character, kCharacterProficiencyButtonsOffset,
        kCharacterProficiencyButtonCount, transform, surface_clip, targets);
    return targets->count != 0u;
}

float CharacterTouchPaddingLocked() {
    const float configured_slop =
        g_menu_touch_configured && std::isfinite(g_menu_touch_config.touch_slop)
            ? g_menu_touch_config.touch_slop
            : 8.0f;
    // One Android touch slop (normally 8 dp) on each side is a moderate gain
    // for the dense desktop layout without turning a whole row into a button.
    return std::clamp(configured_slop, 6.0f, 64.0f);
}

void EndCharacterTouchOnEventLocked(const AInputEvent* event, bool cancel_guest) {
    if (!g_character_touch_remap.active || !event) return;
    if (cancel_guest) SetTouchActionOverrideLocked(event);
    g_character_touch_remap.ending_event = event;
}

void ObserveCharacterTouchLocked(const AInputEvent* event, int32_t source) {
    if (!event ||
        (source & AINPUT_SOURCE_TOUCHSCREEN) != AINPUT_SOURCE_TOUCHSCREEN) {
        return;
    }
    const int32_t raw_action = AMotionEvent_getAction(event);
    const int32_t action = raw_action & AMOTION_EVENT_ACTION_MASK;
    const size_t pointer_count = AMotionEvent_getPointerCount(event);
    if (action == AMOTION_EVENT_ACTION_DOWN) {
        CancelCharacterTouchLocked();
        if (pointer_count != 1u) return;
        uint32_t character = 0;
        warband::input::WidgetCoordinateTransform transform{};
        CharacterTouchTargetSet targets{};
        if (!ResolveCharacterWindowLocked(&character) ||
            !ReadCharacterCoordinateTransformLocked(&transform) ||
            !GatherCharacterTouchTargetsLocked(character, transform, &targets)) {
            return;
        }
        const float x = AMotionEvent_getX(event, 0u);
        const float y = AMotionEvent_getY(event, 0u);
        const float padding = CharacterTouchPaddingLocked();
        const auto selected = warband::input::SelectExpandedTouchTarget(
            targets.rects.data(), targets.count, x, y, padding, padding,
            transform.surface_width, transform.surface_height,
            targets.clips.data());
        if (!selected) return;
        const float delta_x = selected.point_was_inside ? 0.0f : selected.snap_x - x;
        const float delta_y = selected.point_was_inside ? 0.0f : selected.snap_y - y;
        if (!std::isfinite(delta_x) || !std::isfinite(delta_y)) return;
        g_character_touch_remap.active = true;
        g_character_touch_remap.pointer_id = AMotionEvent_getPointerId(event, 0u);
        g_character_touch_remap.delta_x = delta_x;
        g_character_touch_remap.delta_y = delta_y;
        g_character_touch_remap.character = character;
        if (!selected.point_was_inside) {
            LOGI("character touch target expanded widget=0x%08X delta=%.1f,%.1f",
                 targets.widgets[selected.index], delta_x, delta_y);
        }
        return;
    }
    if (!g_character_touch_remap.active) return;

    uint32_t character = 0;
    if (!ResolveCharacterWindowLocked(&character) ||
        character != g_character_touch_remap.character) {
        EndCharacterTouchOnEventLocked(event, true);
        return;
    }
    if (action == AMOTION_EVENT_ACTION_POINTER_DOWN) {
        // A '+' tap is deliberately single-touch.  Cancelling avoids a raw
        // coordinate jump if a second finger appears during the snapped stream.
        EndCharacterTouchOnEventLocked(event, true);
        return;
    }
    if (action == AMOTION_EVENT_ACTION_CANCEL) {
        EndCharacterTouchOnEventLocked(event, false);
        return;
    }
    if (MotionPointerIndex(event, g_character_touch_remap.pointer_id) < 0) {
        EndCharacterTouchOnEventLocked(event, true);
        return;
    }
    if (action == AMOTION_EVENT_ACTION_UP ||
        action == AMOTION_EVENT_ACTION_POINTER_UP) {
        const size_t action_index = static_cast<size_t>(
            (raw_action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
            AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
        if (action_index >= pointer_count ||
            AMotionEvent_getPointerId(event, action_index) ==
                g_character_touch_remap.pointer_id) {
            EndCharacterTouchOnEventLocked(event, action_index >= pointer_count);
        }
    }
}

struct UiTouchTargetSet {
    std::array<warband::input::TouchTargetRect, kUiTouchTargetLimit> rects{};
    std::array<warband::input::TouchTargetRect, kUiTouchTargetLimit> clips{};
    std::array<uint32_t, kUiTouchTargetLimit> widgets{};
    std::array<uint32_t, kUiTouchTargetLimit> scrollables{};
    std::array<float, kUiTouchTargetLimit> content_extents{};
    size_t count = 0;
};

void RememberTouchHighlightChoicesLocked(uint32_t window_id,
                                         uint32_t window,
                                         const UiTouchTargetSet& buttons) {
    if (!g_touch_widget_highlight.active ||
        g_touch_widget_highlight.window_id != window_id ||
        g_touch_widget_highlight.window_object != window) {
        return;
    }

    // Clear widgets dropped by a rebuilt response list before replacing the
    // snapshot. The post-controller hook will immediately re-enable only the
    // row under the live finger.
    for (size_t index = 0;
         index < g_touch_widget_highlight.choice_widget_count; ++index) {
        SetTouchHighlightFlagLocked(
            g_touch_widget_highlight.choice_widgets[index], false);
    }
    g_touch_widget_highlight.choice_widgets.fill(0u);
    g_touch_widget_highlight.choice_widget_count = 0u;

    for (size_t index = 0;
         index < buttons.count &&
         g_touch_widget_highlight.choice_widget_count <
             g_touch_widget_highlight.choice_widgets.size();
         ++index) {
        const uint32_t widget = buttons.widgets[index];
        if (!ValidTouchHighlightWidgetLocked(widget)) continue;
        const auto begin = g_touch_widget_highlight.choice_widgets.begin();
        const auto end = begin +
            g_touch_widget_highlight.choice_widget_count;
        if (std::find(begin, end, widget) != end) continue;
        g_touch_widget_highlight.choice_widgets[
            g_touch_widget_highlight.choice_widget_count++] = widget;
    }
}

bool ResolveActiveWindowObjectLocked(uint32_t window_id, uint32_t* window_out) {
    if (!window_out || !g_host || !g_game_screen ||
        window_id == kNoActiveWindowId || window_id > 63u) {
        return false;
    }
    const uint64_t slot64 = static_cast<uint64_t>(g_game_screen) +
                            sizeof(uint32_t) * (window_id + 1u);
    if (slot64 > std::numeric_limits<uint32_t>::max() ||
        !GuestAddressRange(*g_host, slot64, sizeof(uint32_t))) {
        return false;
    }
    const uint32_t window = g_host->MemoryRead32(static_cast<uint32_t>(slot64));
    if (!ValidGuestPointerLocked(window)) return false;
    *window_out = window;
    return true;
}

bool UiWidgetSeen(const std::array<uint32_t, kUiTouchTargetLimit>& visited,
                  size_t count, uint32_t widget) {
    return std::find(visited.begin(), visited.begin() + count, widget) !=
           visited.begin() + count;
}

bool ReadScrollableContentExtentLocked(uint32_t scrollable,
                                       float* content_extent) {
    if (!content_extent || !ValidGuestPointerLocked(scrollable) ||
        !GuestAddressRange(*g_host, scrollable + 1864u, sizeof(uint32_t)) ||
        !GuestAddressRange(*g_host, scrollable + 432u, sizeof(uint32_t))) {
        return false;
    }
    const uint32_t outer = g_host->MemoryRead32(scrollable + 1864u);
    if (!ValidGuestPointerLocked(outer) ||
        !GuestAddressRange(*g_host, outer + 440u, sizeof(uint32_t))) {
        return false;
    }
    const float content = ReadGuestFloatLocked(outer + 432u) -
                          ReadGuestFloatLocked(outer + 440u);
    const float viewport = ReadGuestFloatLocked(scrollable + 432u);
    const float extent = content - viewport;
    if (!std::isfinite(extent) || !(extent > 1.0e-5f)) return false;
    *content_extent = extent;
    return true;
}

void AppendUiTouchTargetLocked(
        uint32_t widget, const warband::input::WidgetCoordinateTransform& transform,
        const warband::input::TouchTargetRect& clip, UiTouchTargetSet* targets,
        uint32_t scrollable = 0u, float content_extent = 0.0f) {
    if (!targets || targets->count >= targets->rects.size()) return;
    for (size_t index = 0; index < targets->count; ++index) {
        if (targets->widgets[index] != widget) continue;
        if (scrollable && std::isfinite(content_extent) &&
            content_extent > 0.0f) {
            targets->scrollables[index] = scrollable;
            targets->content_extents[index] = content_extent;
        }
        return;
    }
    warband::input::TouchTargetRect rect{};
    if (!ReadVisibleWidgetScreenRectLocked(widget, transform, 0u, &rect)) return;
    targets->widgets[targets->count] = widget;
    targets->rects[targets->count] = rect;
    targets->clips[targets->count] = clip;
    targets->scrollables[targets->count] = scrollable;
    targets->content_extents[targets->count] = content_extent;
    ++targets->count;
}

bool ResolveOrdersRowAtPointLocked(float x, float y,
                                   uint32_t* tactical_out,
                                   uint32_t* row_out) {
    if (!tactical_out || !row_out || !std::isfinite(x) ||
        !std::isfinite(y) || !g_host || !g_game_screen) {
        return false;
    }
    uint32_t active_window = kNoActiveWindowId;
    uint32_t tactical = 0;
    if (!ReadStableActiveWindowId(*g_host, g_game_screen, &active_window) ||
        active_window != kTacticalWindowId ||
        !ResolveActiveWindowObjectLocked(active_window, &tactical) ||
        !GuestAddressRange(*g_host, tactical + kTacticalOrdersRootWidgetOffset,
                           kTacticalOrdersRowCountOffset -
                               kTacticalOrdersRootWidgetOffset +
                               sizeof(uint32_t))) {
        return false;
    }
    const uint32_t root = g_host->MemoryRead32(
        tactical + kTacticalOrdersRootWidgetOffset);
    if (!ValidGuestPointerLocked(root) ||
        !GuestAddressRange(*g_host, root,
                           kWidgetFlagsOffset + sizeof(uint32_t)) ||
        (g_host->MemoryRead32(root + kWidgetFlagsOffset) & 0x3u) != 0u) {
        return false;
    }
    const uint32_t row_count =
        g_host->MemoryRead32(tactical + kTacticalOrdersRowCountOffset);
    if (row_count == 0u || row_count > kTacticalOrdersRowWidgetCount) {
        return false;
    }

    warband::input::WidgetCoordinateTransform transform{};
    if (!ReadCharacterCoordinateTransformLocked(&transform)) return false;
    const warband::input::TouchTargetRect surface_clip{
        0.0f, 0.0f, transform.surface_width, transform.surface_height};
    UiTouchTargetSet targets{};
    std::array<uint32_t, kTacticalOrdersRowWidgetCount> target_rows{};
    for (uint32_t row = 0; row < row_count; ++row) {
        const uint32_t field = tactical + kTacticalOrdersFirstRowWidgetOffset +
                               row * sizeof(uint32_t);
        const uint32_t widget = g_host->MemoryRead32(field);
        if (!ValidGuestPointerLocked(widget) ||
            !GuestAddressRange(*g_host, widget,
                               kWidgetFlagsOffset + sizeof(uint32_t)) ||
            (g_host->MemoryRead32(widget + kWidgetFlagsOffset) & 0x3u) != 0u) {
            continue;
        }
        const size_t before = targets.count;
        AppendUiTouchTargetLocked(widget, transform, surface_clip, &targets);
        if (targets.count != before) {
            target_rows[before] = row;
        }
    }
    if (targets.count == 0u) return false;

    const float padding = std::clamp(
        g_menu_touch_configured ? g_menu_touch_config.touch_slop : 8.0f,
        6.0f, 24.0f);
    const auto selected = warband::input::SelectExpandedTouchTarget(
        targets.rects.data(), targets.count, x, y, padding, padding,
        transform.surface_width, transform.surface_height,
        targets.clips.data());
    if (!selected) return false;
    *tactical_out = tactical;
    *row_out = target_rows[selected.index];
    return true;
}

void AppendUiBottomButtonFieldLocked(
        uint32_t window, uint32_t field_offset,
        const warband::input::WidgetCoordinateTransform& transform,
        const warband::input::TouchTargetRect& surface_clip,
        UiTouchTargetSet* targets) {
    const uint64_t field64 = static_cast<uint64_t>(window) + field_offset;
    if (!targets || !g_simple_button_vtable ||
        field64 > std::numeric_limits<uint32_t>::max() ||
        !GuestAddressRange(*g_host, field64, sizeof(uint32_t))) {
        return;
    }
    const uint32_t widget = g_host->MemoryRead32(static_cast<uint32_t>(field64));
    if (!ValidGuestPointerLocked(widget) ||
        !GuestAddressRange(*g_host, widget,
                           kWidgetFlagsOffset + sizeof(uint32_t)) ||
        g_host->MemoryRead32(widget + kWidgetVtableOffset) !=
            g_simple_button_vtable ||
        (g_host->MemoryRead32(widget + kWidgetFlagsOffset) & 0x3u) != 0u) {
        return;
    }
    warband::input::TouchTargetRect rect{};
    if (!ReadVisibleWidgetScreenRectLocked(
            widget, transform, g_simple_button_vtable, &rect)) {
        return;
    }
    const float width = rect.right - rect.left;
    const float height = rect.bottom - rect.top;
    const float centre_y = rect.top + height * 0.5f;
    if (width < 3.0f || height < 3.0f ||
        width > transform.surface_width * 0.48f ||
        height > transform.surface_height * 0.16f ||
        centre_y < transform.surface_height * 0.72f) {
        return;
    }
    AppendUiTouchTargetLocked(
        widget, transform, surface_clip, targets);
}

void AppendUiMenuChoiceFieldLocked(
        uint32_t window, uint32_t field_offset,
        const warband::input::WidgetCoordinateTransform& transform,
        const warband::input::TouchTargetRect& surface_clip,
        UiTouchTargetSet* targets) {
    const uint64_t field64 = static_cast<uint64_t>(window) + field_offset;
    if (!targets || !g_simple_button_vtable ||
        field64 > std::numeric_limits<uint32_t>::max() ||
        !GuestAddressRange(*g_host, field64, sizeof(uint32_t))) {
        return;
    }
    const uint32_t widget = g_host->MemoryRead32(static_cast<uint32_t>(field64));
    if (!ValidTouchHighlightWidgetLocked(widget)) return;

    warband::input::TouchTargetRect rect{};
    if (!ReadVisibleWidgetScreenRectLocked(
            widget, transform, g_simple_button_vtable, &rect)) {
        return;
    }
    const float width = rect.right - rect.left;
    const float height = rect.bottom - rect.top;
    if (width < 3.0f || height < 3.0f ||
        width > transform.surface_width ||
        height > transform.surface_height * 0.16f) {
        return;
    }
    AppendUiTouchTargetLocked(widget, transform, surface_clip, targets);
}

void AppendUiScrollbarFieldLocked(
        uint32_t window, uint32_t scrollable_field_offset,
        const warband::input::WidgetCoordinateTransform& transform,
        const warband::input::TouchTargetRect& surface_clip,
        UiTouchTargetSet* targets) {
    const uint64_t field64 = static_cast<uint64_t>(window) +
                             scrollable_field_offset;
    if (!targets || !g_scrollable_get_type_va || !g_scrollbar_get_type_va ||
        field64 > std::numeric_limits<uint32_t>::max() ||
        !GuestAddressRange(*g_host, field64, sizeof(uint32_t))) {
        return;
    }
    const uint32_t scrollable = g_host->MemoryRead32(
        static_cast<uint32_t>(field64));
    if (!ValidGuestPointerLocked(scrollable) ||
        !GuestAddressRange(*g_host, scrollable + 1852u, 16u)) {
        return;
    }
    const uint32_t scrollable_vtable = g_host->MemoryRead32(scrollable);
    if (!ValidGuestPointerLocked(scrollable_vtable) ||
        g_host->MemoryRead32(scrollable_vtable + 236u) !=
            g_scrollable_get_type_va ||
        g_host->MemoryRead32(scrollable + 1860u) != 1u) {
        return;
    }
    const uint32_t scrollbar = g_host->MemoryRead32(scrollable + 1852u);
    if (!ValidGuestPointerLocked(scrollbar)) return;
    const uint32_t scrollbar_vtable = g_host->MemoryRead32(scrollbar);
    if (!ValidGuestPointerLocked(scrollbar_vtable) ||
        g_host->MemoryRead32(scrollbar_vtable + 236u) !=
            g_scrollbar_get_type_va) {
        return;
    }
    warband::input::TouchTargetRect rect{};
    if (!ReadVisibleWidgetScreenRectLocked(
            scrollbar, transform, 0u, &rect)) {
        return;
    }
    const float width = rect.right - rect.left;
    const float height = rect.bottom - rect.top;
    if (height < std::max(8.0f, width * 1.5f)) return;
    float content_extent = 0.0f;
    ReadScrollableContentExtentLocked(scrollable, &content_extent);
    AppendUiTouchTargetLocked(
        scrollbar, transform, surface_clip, targets, scrollable,
        content_extent);
}

void CollectUiTouchTargetsLocked(
        uint32_t widget, int depth,
        const warband::input::WidgetCoordinateTransform& transform,
        bool conversation_rows,
        UiTouchTargetSet* bottom_buttons, UiTouchTargetSet* scrollbars,
        std::array<uint32_t, kUiTouchTargetLimit>* visited,
        size_t* visited_count) {
    if (!visited || !visited_count || depth > 14 ||
        *visited_count >= visited->size() || !ValidGuestPointerLocked(widget) ||
        UiWidgetSeen(*visited, *visited_count, widget)) {
        return;
    }
    (*visited)[(*visited_count)++] = widget;
    if (!GuestAddressRange(*g_host, widget,
                           kWidgetFlagsOffset + sizeof(uint32_t)) ||
        (g_host->MemoryRead32(widget + kWidgetFlagsOffset) & 0x3u) != 0u) {
        return;
    }

    const warband::input::TouchTargetRect surface_clip{
        0.0f, 0.0f, transform.surface_width, transform.surface_height};
    const uint32_t vtable = g_host->MemoryRead32(widget + kWidgetVtableOffset);
    if (!ValidGuestPointerLocked(vtable)) return;
    if (bottom_buttons && g_simple_button_vtable &&
        vtable == g_simple_button_vtable) {
        warband::input::TouchTargetRect rect{};
        if (ReadVisibleWidgetScreenRectLocked(
                widget, transform, g_simple_button_vtable, &rect)) {
            const float width = rect.right - rect.left;
            const float height = rect.bottom - rect.top;
            const float centre_y = rect.top + height * 0.5f;
            const bool standard_bottom =
                width >= 3.0f && height >= 3.0f &&
                width <= transform.surface_width * 0.48f &&
                height <= transform.surface_height * 0.16f &&
                centre_y >= transform.surface_height * 0.72f;
            const bool conversation_response = conversation_rows &&
                warband::input::IsConversationResponseGeometry(
                    width, height, centre_y, transform.surface_width,
                    transform.surface_height);
            if (standard_bottom || conversation_response) {
                AppendUiTouchTargetLocked(
                    widget, transform, surface_clip, bottom_buttons);
            }
        }
    }

    if (scrollbars && g_scrollable_get_type_va && g_scrollbar_get_type_va &&
        g_host->MemoryRead32(vtable + 236u) == g_scrollable_get_type_va &&
        GuestAddressRange(*g_host, widget + 1852u, 16u) &&
        g_host->MemoryRead32(widget + 1860u) == 1u) {
        const uint32_t scrollbar = g_host->MemoryRead32(widget + 1852u);
        if (ValidGuestPointerLocked(scrollbar)) {
            const uint32_t scrollbar_vtable = g_host->MemoryRead32(scrollbar);
            warband::input::TouchTargetRect rect{};
            if (ValidGuestPointerLocked(scrollbar_vtable) &&
                g_host->MemoryRead32(scrollbar_vtable + 236u) ==
                    g_scrollbar_get_type_va &&
                ReadVisibleWidgetScreenRectLocked(
                    scrollbar, transform, 0u, &rect)) {
                const float width = rect.right - rect.left;
                const float height = rect.bottom - rect.top;
                if (height >= std::max(8.0f, width * 1.5f)) {
                    float content_extent = 0.0f;
                    ReadScrollableContentExtentLocked(widget, &content_extent);
                    AppendUiTouchTargetLocked(
                        scrollbar, transform, surface_clip, scrollbars,
                        widget, content_extent);
                }
            }
        }
    }

    if (!IsGuestContainerWidgetLocked(widget)) return;

    const uint32_t children_begin = g_host->MemoryRead32(
        widget + kWidgetChildrenBeginOffset);
    const uint32_t children_end = g_host->MemoryRead32(
        widget + kWidgetChildrenEndOffset);
    if (!ValidGuestPointerLocked(children_begin) ||
        !ValidGuestPointerLocked(children_end) || children_end < children_begin ||
        (children_end - children_begin) > 4u * 4096u) {
        return;
    }
    for (uint32_t cursor = children_begin; cursor < children_end; cursor += 4u) {
        if (!GuestAddressRange(*g_host, cursor, sizeof(uint32_t))) return;
        CollectUiTouchTargetsLocked(
            g_host->MemoryRead32(cursor), depth + 1, transform,
            conversation_rows,
            bottom_buttons, scrollbars, visited, visited_count);
        if (*visited_count >= visited->size()) return;
    }
}

bool GatherUiTouchTargetsLocked(
        uint32_t window_id, uint32_t window,
        const warband::input::WidgetCoordinateTransform& transform,
        UiTouchTargetSet* bottom_buttons, UiTouchTargetSet* scrollbars) {
    if (!ValidGuestPointerLocked(window)) return false;
    if (bottom_buttons) *bottom_buttons = {};
    if (scrollbars) *scrollbars = {};
    std::array<uint32_t, kUiTouchTargetLimit> visited{};
    size_t visited_count = 0;
    // Character Done is captured from its exact reverse-engineered field
    // below. Do not collect neighbouring Character buttons into this set.
    UiTouchTargetSet* tree_bottom_buttons =
        (window_id == kCharacterWindowId ||
         window_id == warband::input::kInitialWindowId ||
         window_id == warband::input::kMenuWindowId)
            ? nullptr : bottom_buttons;
    CollectUiTouchTargetsLocked(
        window, 0, transform,
        window_id == warband::input::kConversationWindowId,
        tree_bottom_buttons, scrollbars,
        &visited, &visited_count);
    if (bottom_buttons) {
        const warband::input::TouchTargetRect surface_clip{
            0.0f, 0.0f, transform.surface_width, transform.surface_height};
        uint32_t first_offset = 0u;
        uint32_t last_offset = 0u;
        if (window_id == kMapWindowId) {
            first_offset = kMapBottomButtonFirstOffset;
            last_offset = kMapBottomButtonLastOffset;
        } else if (window_id == kNotesWindowId) {
            first_offset = kNotesBottomButtonFirstOffset;
            last_offset = kNotesBottomButtonLastOffset;
        } else if (window_id == kCharacterWindowId) {
            first_offset = kCharacterDoneButtonOffset;
            last_offset = kCharacterDoneButtonOffset;
        }
        for (uint32_t offset = first_offset;
             first_offset != 0u && offset <= last_offset;
             offset += sizeof(uint32_t)) {
            AppendUiBottomButtonFieldLocked(
                window, offset, transform, surface_clip, bottom_buttons);
        }
        if (window_id == warband::input::kMenuWindowId) {
            for (uint32_t offset = warband::input::kMenuChoiceFirstOffset;
                 offset <= warband::input::kMenuChoiceLastOffset;
                 offset += sizeof(uint32_t)) {
                AppendUiMenuChoiceFieldLocked(
                    window, offset, transform, surface_clip, bottom_buttons);
            }
        }
        if (window_id == warband::input::kInitialWindowId) {
            for (uint32_t offset = warband::input::kInitialChoiceFirstOffset;
                 offset <= warband::input::kInitialChoiceLastOffset;
                 offset += sizeof(uint32_t)) {
                AppendUiMenuChoiceFieldLocked(
                    window, offset, transform, surface_clip, bottom_buttons);
            }
        }
    }
    if (window_id == kNotesWindowId && scrollbars) {
        const warband::input::TouchTargetRect surface_clip{
            0.0f, 0.0f, transform.surface_width, transform.surface_height};
        for (uint32_t offset = kNotesScrollableFirstOffset;
             offset <= kNotesScrollableLastOffset;
             offset += sizeof(uint32_t)) {
            AppendUiScrollbarFieldLocked(
                window, offset, transform, surface_clip, scrollbars);
        }
    }
    if (window_id == kInventoryWindowId &&
        GuestAddressRange(*g_host, window + kInventoryRootOffset,
                          sizeof(uint32_t))) {
        const uint32_t root = g_host->MemoryRead32(
            window + kInventoryRootOffset);
        CollectUiTouchTargetsLocked(
            root, 0, transform, false, bottom_buttons, scrollbars,
            &visited, &visited_count);
    }
    // Several stock windows keep their visible rglScrollable_widget in the
    // window-manager root rather than below the object stored in game_screen.
    // Options and long conversation response lists use this layout.  Mirror
    // the menu-scroll walk and inspect the actual topmost root as well, or a
    // direct scrollbar drag falls through to the guest and its UP rebuilds the
    // list at row zero.
    if (scrollbars && g_window_manager &&
        ValidGuestPointerLocked(g_window_manager) &&
        GuestAddressRange(*g_host, g_window_manager, 2u * sizeof(uint32_t))) {
        const uint32_t roots_begin = g_host->MemoryRead32(g_window_manager);
        const uint32_t roots_end = g_host->MemoryRead32(
            g_window_manager + sizeof(uint32_t));
        if (ValidGuestPointerLocked(roots_begin) &&
            ValidGuestPointerLocked(roots_end) && roots_end > roots_begin &&
            (roots_end - roots_begin) <= 4u * 64u &&
            GuestAddressRange(*g_host, roots_end - sizeof(uint32_t),
                              sizeof(uint32_t))) {
            // The game-screen object and the window-manager root overlap for
            // response rows, but not for the owning scrollable.  A large
            // conversation can exhaust the first walk's bounded visited set
            // before the second walk starts, leaving the visible scrollbar
            // undiscoverable.  Give the topmost UI root its own traversal
            // budget; target insertion already de-duplicates shared widgets.
            std::array<uint32_t, kUiTouchTargetLimit> root_visited{};
            size_t root_visited_count = 0u;
            CollectUiTouchTargetsLocked(
                g_host->MemoryRead32(roots_end - sizeof(uint32_t)), 0,
                transform,
                window_id == warband::input::kConversationWindowId,
                window_id == warband::input::kConversationWindowId
                    ? bottom_buttons : nullptr,
                scrollbars, &root_visited, &root_visited_count);
        }
    }
    return (bottom_buttons && bottom_buttons->count != 0u) ||
           (scrollbars && scrollbars->count != 0u);
}

bool SetTouchChoiceHighlightWidgetLocked(uint32_t window_id,
                                         uint32_t window,
                                         uint32_t widget) {
    if (!IsTouchHighlightWindow(window_id) ||
        !ValidGuestPointerLocked(window) ||
        !g_touch_widget_highlight.active ||
        g_touch_widget_highlight.window_id != window_id ||
        g_touch_widget_highlight.window_object != window) {
        return false;
    }
    if (widget != 0u && !ValidTouchHighlightWidgetLocked(widget)) {
        return false;
    }
    g_ui_touch.highlight_widget = widget;
    if (window_id == warband::input::kConversationWindowId) {
        g_conversation_navigation_cleanup_pending = true;
    }
    return true;
}

bool SelectUiButtonByGuestHitTestLocked(
        const UiTouchTargetSet& buttons, float x, float y,
        size_t* selected_index) {
    if (!selected_index || buttons.count == 0u ||
        !std::isfinite(x) || !std::isfinite(y)) {
        return false;
    }

    uint32_t saved_cursor_x = 0u;
    uint32_t saved_cursor_y = 0u;
    if (!SetInventoryGuestCursorForTouchLocked(
            x, y, &saved_cursor_x, &saved_cursor_y)) {
        return false;
    }

    size_t hit_index = kUiTouchTargetLimit;
    for (size_t index = 0; index < buttons.count; ++index) {
        if (InventoryWidgetUnderCursorLocked(buttons.widgets[index])) {
            hit_index = index;
            break;
        }
    }
    RestoreInventoryGuestCursorLocked(saved_cursor_x, saved_cursor_y);

    const bool trace = ScrollTraceEnabled();
    static uint32_t hit_reports = 0u;
    if (trace && hit_reports++ < 96u) {
        if (hit_index < buttons.count) {
            const auto& rect = buttons.rects[hit_index];
            LOGI("touch highlight hit touch=%.1f,%.1f index=%zu "
                 "widget=0x%08X rect=%.1f,%.1f-%.1f,%.1f buttons=%zu",
                 x, y, hit_index, buttons.widgets[hit_index], rect.left,
                 rect.top, rect.right, rect.bottom, buttons.count);
        } else {
            LOGI("touch highlight miss touch=%.1f,%.1f buttons=%zu",
                 x, y, buttons.count);
        }
    }
    if (hit_index >= buttons.count) return false;
    *selected_index = hit_index;
    return true;
}

bool UpdateTouchChoiceHighlightAtPointLocked(uint32_t window_id,
                                             uint32_t window,
                                             float x, float y) {
    if (!std::isfinite(x) || !std::isfinite(y)) return false;
    warband::input::WidgetCoordinateTransform transform{};
    UiTouchTargetSet buttons{};
    // Supplying the scrollbar set also enables the independent topmost-root
    // traversal.  Long conversations can exhaust the window object's first
    // visitor budget before every live response row is reached.
    UiTouchTargetSet scrollbars{};
    if (!ReadCharacterCoordinateTransformLocked(&transform) ||
        !GatherUiTouchTargetsLocked(
            window_id, window, transform, &buttons,
            window_id == warband::input::kConversationWindowId
                ? &scrollbars : nullptr) || buttons.count == 0u) {
        g_ui_touch.highlight_widget = 0u;
        return false;
    }
    RememberTouchHighlightChoicesLocked(window_id, window, buttons);
    size_t selected_index = 0u;
    if (!SelectUiButtonByGuestHitTestLocked(
            buttons, x, y, &selected_index)) {
        SetTouchChoiceHighlightWidgetLocked(window_id, window, 0u);
        return false;
    }
    return SetTouchChoiceHighlightWidgetLocked(
        window_id, window, buttons.widgets[selected_index]);
}

bool ReadInventoryScrollbarStateLocked(
        uint32_t scrollbar, float* value, float* minimum, float* maximum) {
    if (!value || !minimum || !maximum || !ValidGuestPointerLocked(scrollbar) ||
        !GuestAddressRange(*g_host, scrollbar + 700u,
                           3u * sizeof(uint32_t))) {
        return false;
    }
    const uint32_t vtable =
        g_host->MemoryRead32(scrollbar + kWidgetVtableOffset);
    if (!ValidGuestPointerLocked(vtable) ||
        g_host->MemoryRead32(vtable + 236u) != g_scrollbar_get_type_va) {
        return false;
    }
    *value = ReadGuestFloatLocked(scrollbar + 700u);
    *minimum = ReadGuestFloatLocked(scrollbar + 704u);
    *maximum = ReadGuestFloatLocked(scrollbar + 708u);
    return std::isfinite(*value) && std::isfinite(*minimum) &&
           std::isfinite(*maximum) && *maximum > *minimum;
}

bool ReadConversationChoiceCountLocked(uint32_t conversation,
                                       uint32_t* choice_count) {
    if (!choice_count || !g_host || !ValidGuestPointerLocked(conversation) ||
        !GuestAddressRange(
            *g_host,
            static_cast<uint64_t>(conversation) +
                warband::input::kConversationChoiceCountOffset,
            sizeof(uint32_t))) {
        return false;
    }
    const uint32_t count = g_host->MemoryRead32(
        conversation + warband::input::kConversationChoiceCountOffset);
    if (count > 4096u) return false;
    *choice_count = count;
    return true;
}

void ClearConversationScrollRetentionLocked() {
    g_conversation_scroll = {};
}

void RememberConversationScrollPositionLocked(
        uint32_t conversation,
        const warband::input::TouchTargetRect& track,
        float value, float minimum, float maximum) {
    uint32_t choice_count = 0;
    warband::input::InventoryScrollbarSnapshot snapshot{};
    if (!ReadConversationChoiceCountLocked(conversation, &choice_count) ||
        choice_count <= warband::input::kMaxContinueChoiceCount ||
        !warband::input::CaptureInventoryScrollbarSnapshot(
            track, value, minimum, maximum, &snapshot)) {
        return;
    }
    g_conversation_scroll.active = true;
    g_conversation_scroll.window_object = conversation;
    g_conversation_scroll.choice_count = choice_count;
    g_conversation_scroll.missing_frames = 0u;
    g_conversation_scroll.snapshot = snapshot;
}

void MaintainConversationScrollPositionLocked() {
    if (!g_conversation_scroll.active) return;

    uint32_t window_id = kNoActiveWindowId;
    uint32_t conversation = 0;
    uint32_t choice_count = 0;
    if (!g_host || !g_game_screen ||
        !ReadStableActiveWindowId(*g_host, g_game_screen, &window_id) ||
        window_id != warband::input::kConversationWindowId ||
        !ResolveActiveWindowObjectLocked(window_id, &conversation) ||
        conversation != g_conversation_scroll.window_object ||
        !ReadConversationChoiceCountLocked(conversation, &choice_count) ||
        choice_count != g_conversation_scroll.choice_count) {
        ClearConversationScrollRetentionLocked();
        return;
    }

    warband::input::WidgetCoordinateTransform transform{};
    UiTouchTargetSet scrollbars{};
    if (!ReadCharacterCoordinateTransformLocked(&transform) ||
        !GatherUiTouchTargetsLocked(
            window_id, conversation, transform, nullptr, &scrollbars)) {
        if (++g_conversation_scroll.missing_frames > 4u) {
            ClearConversationScrollRetentionLocked();
        }
        return;
    }
    const size_t match = warband::input::FindInventoryScrollTrackMatch(
        g_conversation_scroll.snapshot, scrollbars.rects.data(),
        scrollbars.count, nullptr, transform.surface_width,
        transform.surface_height);
    if (match == std::numeric_limits<size_t>::max()) {
        if (++g_conversation_scroll.missing_frames > 4u) {
            ClearConversationScrollRetentionLocked();
        }
        return;
    }

    float value = 0.0f;
    float minimum = 0.0f;
    float maximum = 0.0f;
    if (!ReadInventoryScrollbarStateLocked(
            scrollbars.widgets[match], &value, &minimum, &maximum)) {
        if (++g_conversation_scroll.missing_frames > 4u) {
            ClearConversationScrollRetentionLocked();
        }
        return;
    }
    float retained = 0.0f;
    if (!warband::input::RestoreInventoryScrollbarValue(
            g_conversation_scroll.snapshot, minimum, maximum, &retained)) {
        ClearConversationScrollRetentionLocked();
        return;
    }
    g_conversation_scroll.missing_frames = 0u;
    g_conversation_scroll.snapshot.track = scrollbars.rects[match];
    if (FloatBits(value) != FloatBits(retained)) {
        g_host->MemoryWrite32(
            scrollbars.widgets[match] + 700u, FloatBits(retained));
    }
}

void MaintainInventoryScrollPositionLocked() {
    if (!g_inventory_touch.tracking || !g_inventory_touch.drag_started ||
        !InventorySourceContextIsLiveLocked()) return;
    const bool trace = InventoryTraceEnabled();

    uint32_t window_id = kNoActiveWindowId;
    uint32_t window = 0;
    warband::input::WidgetCoordinateTransform transform{};
    if (!g_host || !g_game_screen ||
        !ReadStableActiveWindowId(*g_host, g_game_screen, &window_id) ||
        window_id != kInventoryWindowId ||
        !ResolveActiveWindowObjectLocked(window_id, &window) ||
        !ReadCharacterCoordinateTransformLocked(&transform)) {
        return;
    }

    UiTouchTargetSet scrollbars{};
    GatherUiTouchTargetsLocked(
        window_id, window, transform, nullptr, &scrollbars);

    if (!g_inventory_touch.scrollbar_snapshots_captured) {
        // The first source event is consumed after this frame hook. Capture
        // before it can rebuild the panes; never learn a reset-to-top value
        // from the following frame and then preserve the wrong state.
        if (g_inventory_touch.source_event_queued) return;
        g_inventory_touch.scrollbar_snapshot_count = 0u;
        for (size_t index = 0;
             index < scrollbars.count &&
             g_inventory_touch.scrollbar_snapshot_count <
                 g_inventory_touch.scrollbar_snapshots.size();
             ++index) {
            float value = 0.0f;
            float minimum = 0.0f;
            float maximum = 0.0f;
            if (!ReadInventoryScrollbarStateLocked(
                    scrollbars.widgets[index], &value, &minimum, &maximum)) {
                continue;
            }
            auto& snapshot = g_inventory_touch.scrollbar_snapshots[
                g_inventory_touch.scrollbar_snapshot_count];
            if (warband::input::CaptureInventoryScrollbarSnapshot(
                    scrollbars.rects[index], value, minimum, maximum,
                    &snapshot)) {
                ++g_inventory_touch.scrollbar_snapshot_count;
            }
        }
        g_inventory_touch.scrollbar_snapshots_captured = true;
        if (trace) {
            LOGI("inventory captured %zu pane scroll positions before pickup",
                 g_inventory_touch.scrollbar_snapshot_count);
        }
        return;
    }

    std::array<bool, kUiTouchTargetLimit> claimed{};
    for (size_t snapshot_index = 0;
         snapshot_index < g_inventory_touch.scrollbar_snapshot_count;
         ++snapshot_index) {
        auto& snapshot =
            g_inventory_touch.scrollbar_snapshots[snapshot_index];
        const size_t live_index = warband::input::FindInventoryScrollTrackMatch(
            snapshot, scrollbars.rects.data(), scrollbars.count,
            claimed.data(), transform.surface_width, transform.surface_height);
        if (live_index == std::numeric_limits<size_t>::max()) continue;

        float value = 0.0f;
        float minimum = 0.0f;
        float maximum = 0.0f;
        if (!ReadInventoryScrollbarStateLocked(
                scrollbars.widgets[live_index], &value, &minimum, &maximum)) {
            continue;
        }
        float restored = 0.0f;
        if (!warband::input::RestoreInventoryScrollbarValue(
                snapshot, minimum, maximum, &restored)) {
            continue;
        }
        claimed[live_index] = true;
        snapshot.track = scrollbars.rects[live_index];
        if (FloatBits(value) != FloatBits(restored)) {
            g_host->MemoryWrite32(
                scrollbars.widgets[live_index] + 700u,
                FloatBits(restored));
            if (trace) {
                LOGI("inventory restored pane scroll widget=0x%08X "
                     "value=%.6f reset=%.6f",
                     scrollbars.widgets[live_index], restored, value);
            }
        }
    }
}

void UpdateInventoryScrollSnapshotLocked(
        const warband::input::TouchTargetRect& live_track,
        float value, float minimum, float maximum,
        float surface_width, float surface_height) {
    if (!g_inventory_touch.scrollbar_snapshots_captured ||
        g_inventory_touch.scrollbar_snapshot_count == 0u) {
        return;
    }

    size_t best_index = std::numeric_limits<size_t>::max();
    float best_score = std::numeric_limits<float>::infinity();
    for (size_t index = 0;
         index < g_inventory_touch.scrollbar_snapshot_count; ++index) {
        const float score = warband::input::InventoryScrollTrackMatchScore(
            g_inventory_touch.scrollbar_snapshots[index].track, live_track,
            surface_width, surface_height);
        if (score < best_score) {
            best_score = score;
            best_index = index;
        }
    }
    if (best_index == std::numeric_limits<size_t>::max()) return;

    warband::input::InventoryScrollbarSnapshot updated{};
    if (warband::input::CaptureInventoryScrollbarSnapshot(
            live_track, value, minimum, maximum, &updated)) {
        g_inventory_touch.scrollbar_snapshots[best_index] = updated;
    }
}

float UiTouchSlopLocked() {
    const float configured =
        g_menu_touch_configured && std::isfinite(g_menu_touch_config.touch_slop)
            ? g_menu_touch_config.touch_slop
            : 8.0f;
    return std::clamp(configured, 4.0f, 64.0f);
}

bool EstimateConversationContentExtentPixelsLocked(
        const UiTouchTargetSet& buttons, uint32_t choice_count,
        float surface_width, float surface_height, float* extent_out) {
    if (!extent_out || choice_count == 0u || !std::isfinite(surface_width) ||
        !std::isfinite(surface_height) || surface_width <= 0.0f ||
        surface_height <= 0.0f) {
        return false;
    }
    std::array<float, kUiTouchTargetLimit> centres{};
    size_t centre_count = 0u;
    for (size_t index = 0; index < buttons.count; ++index) {
        const auto& rect = buttons.rects[index];
        const float width = rect.right - rect.left;
        const float height = rect.bottom - rect.top;
        const float centre = rect.top + height * 0.5f;
        if (!warband::input::IsConversationResponseGeometry(
                width, height, centre, surface_width, surface_height)) {
            continue;
        }
        centres[centre_count++] = centre;
    }
    if (centre_count < 2u || choice_count <= centre_count) return false;
    std::sort(centres.begin(), centres.begin() + centre_count);
    std::array<float, kUiTouchTargetLimit> pitches{};
    size_t pitch_count = 0u;
    for (size_t index = 1u; index < centre_count; ++index) {
        const float pitch = centres[index] - centres[index - 1u];
        if (std::isfinite(pitch) && pitch >= 3.0f &&
            pitch <= surface_height * 0.15f) {
            pitches[pitch_count++] = pitch;
        }
    }
    if (pitch_count == 0u) return false;
    std::sort(pitches.begin(), pitches.begin() + pitch_count);
    const float row_pitch = pitches[pitch_count / 2u];
    return warband::input::EstimateContentExtentPixels(
        row_pitch, static_cast<uint32_t>(centre_count), choice_count,
        extent_out);
}

bool UiTouchOwnsWindowLocked(uint32_t window_id) {
    return g_ui_touch.active && g_ui_touch.window_id == window_id &&
           g_ui_touch.mode != UiTouchMode::MenuChoiceHighlight;
}

bool UiTouchBlocksMenuInputLocked() {
    return warband::input::UiTouchBlocksMenuInput(
        g_ui_touch.active,
        g_ui_touch.mode == UiTouchMode::MenuChoiceHighlight);
}

void EndUiTouchOnEventLocked(const AInputEvent* event, bool cancel_guest) {
    if (!g_ui_touch.active || !event) return;
    const bool visual_only =
        g_ui_touch.mode == UiTouchMode::MenuChoiceHighlight;
    if (!visual_only &&
        (cancel_guest || g_ui_touch.mode == UiTouchMode::ScrollbarDrag ||
         g_ui_touch.mode == UiTouchMode::ConversationResponse ||
         g_ui_touch.mode == UiTouchMode::SyntheticBottomButton ||
         g_ui_touch.mode == UiTouchMode::MapDeadZone)) {
        SetTouchActionOverrideLocked(event);
    }
    if (g_ui_touch.window_id == warband::input::kConversationWindowId &&
        (g_ui_touch.mode == UiTouchMode::ConversationResponse ||
         g_ui_touch.mode == UiTouchMode::ScrollbarDrag)) {
        g_conversation_navigation_cleanup_pending = true;
    }
    g_ui_touch.ending_event = event;
}

void BeginSyntheticUiTouchLocked(
        const AInputEvent* event, int32_t pointer_id, float x, float y,
        uint32_t window_id, uint32_t window, uint32_t widget,
        UiTouchMode mode) {
    g_ui_touch.active = true;
    g_ui_touch.tap_eligible = widget != 0u;
    g_ui_touch.pointer_id = pointer_id;
    g_ui_touch.down_x = x;
    g_ui_touch.down_y = y;
    g_ui_touch.window_id = window_id;
    g_ui_touch.window_object = window;
    g_ui_touch.widget = widget;
    g_ui_touch.mode = mode;
    SetTouchActionOverrideLocked(event);
}

void ObserveUiTouchLocked(const AInputEvent* event, int32_t source) {
    if (!event ||
        (source & AINPUT_SOURCE_TOUCHSCREEN) != AINPUT_SOURCE_TOUCHSCREEN) {
        return;
    }
    const int32_t raw_action = AMotionEvent_getAction(event);
    const int32_t action = raw_action & AMOTION_EVENT_ACTION_MASK;
    const size_t pointer_count = AMotionEvent_getPointerCount(event);
    if (action == AMOTION_EVENT_ACTION_DOWN) {
        CancelUiTouchLocked();
        if (pointer_count != 1u || !g_gameplay_state_known ||
            !g_menu_touch_configured) {
            return;
        }
        uint32_t window_id = kNoActiveWindowId;
        uint32_t window = 0;
        warband::input::WidgetCoordinateTransform transform{};
        if (!g_host || !g_game_screen ||
            !ReadStableActiveWindowId(*g_host, g_game_screen, &window_id) ||
            !warband::input::ShouldInspectUiScrollbars(
                g_gameplay_state_known, g_gameplay_active,
                window_id == warband::input::kConversationWindowId) ||
            !ResolveActiveWindowObjectLocked(window_id, &window) ||
            !ReadCharacterCoordinateTransformLocked(&transform)) {
            return;
        }

        UiTouchTargetSet bottom_buttons{};
        UiTouchTargetSet scrollbars{};
        const bool conversation_window =
            window_id == warband::input::kConversationWindowId;
        const bool menu_window =
            window_id == warband::input::kMenuWindowId ||
            window_id == warband::input::kInitialWindowId;
        GatherUiTouchTargetsLocked(
            window_id, window, transform,
            (window_id == kMapWindowId || window_id == kNotesWindowId ||
             window_id == kCharacterWindowId || conversation_window ||
             menu_window)
                ? &bottom_buttons : nullptr,
            (window_id == kMapWindowId || menu_window)
                ? nullptr : &scrollbars);
        const float x = AMotionEvent_getX(event, 0u);
        const float y = AMotionEvent_getY(event, 0u);
        const bool scroll_trace = ScrollTraceEnabled();
        if (scroll_trace && scrollbars.count != 0u) {
            static uint32_t target_reports = 0;
            if (target_reports++ < 24u) {
                LOGI("scrollbar targets window=%u count=%zu touch=%.1f,%.1f",
                     window_id, scrollbars.count, x, y);
                for (size_t index = 0;
                     index < std::min<size_t>(scrollbars.count, 8u); ++index) {
                    const auto& rect = scrollbars.rects[index];
                    LOGI("scrollbar target[%zu]=0x%08X rect=%.1f,%.1f-%.1f,%.1f",
                         index, scrollbars.widgets[index], rect.left, rect.top,
                         rect.right, rect.bottom);
                }
            }
        }
        if (bottom_buttons.count != 0u) {
            const auto selected = (conversation_window || menu_window)
                ? warband::input::SelectExpandedTouchTarget(
                      bottom_buttons.rects.data(), bottom_buttons.count, x, y,
                      0.0f, 0.0f, transform.surface_width,
                      transform.surface_height, bottom_buttons.clips.data())
                : window_id == kCharacterWindowId
                ? warband::input::SelectExpandedTouchTarget(
                      bottom_buttons.rects.data(), bottom_buttons.count, x, y,
                      0.0f, 0.0f, transform.surface_width,
                      transform.surface_height, bottom_buttons.clips.data())
                : warband::input::SelectUpwardDoubledTouchTarget(
                      bottom_buttons.rects.data(), bottom_buttons.count, x, y,
                      transform.surface_width, transform.surface_height,
                      bottom_buttons.clips.data());
            if (selected) {
                if (menu_window) {
                    g_ui_touch.active = true;
                    g_ui_touch.pointer_id =
                        AMotionEvent_getPointerId(event, 0u);
                    g_ui_touch.down_x = x;
                    g_ui_touch.down_y = y;
                    g_ui_touch.last_move_x = x;
                    g_ui_touch.last_move_y = y;
                    g_ui_touch.window_id = window_id;
                    g_ui_touch.window_object = window;
                    size_t hit_index = selected.index;
                    if (SelectUiButtonByGuestHitTestLocked(
                            bottom_buttons, x, y, &hit_index)) {
                        g_ui_touch.widget = bottom_buttons.widgets[hit_index];
                    } else {
                        g_ui_touch.widget =
                            bottom_buttons.widgets[selected.index];
                    }
                    g_ui_touch.mode = UiTouchMode::MenuChoiceHighlight;
                    ActivateTouchWidgetHighlightLocked(window_id, window);
                    RememberTouchHighlightChoicesLocked(
                        window_id, window, bottom_buttons);
                    SetTouchChoiceHighlightWidgetLocked(
                        window_id, window, g_ui_touch.widget);
                    LOGI("menu choice touch highlight captured "
                         "widget=0x%08X", g_ui_touch.widget);
                    return;
                }
                if (conversation_window) {
                    g_ui_touch.active = true;
                    g_ui_touch.tap_eligible = true;
                    g_ui_touch.pointer_id =
                        AMotionEvent_getPointerId(event, 0u);
                    g_ui_touch.down_x = x;
                    g_ui_touch.down_y = y;
                    g_ui_touch.last_move_x = x;
                    g_ui_touch.last_move_y = y;
                    g_ui_touch.window_id = window_id;
                    g_ui_touch.window_object = window;
                    size_t hit_index = selected.index;
                    if (SelectUiButtonByGuestHitTestLocked(
                            bottom_buttons, x, y, &hit_index)) {
                        g_ui_touch.widget = bottom_buttons.widgets[hit_index];
                    } else {
                        g_ui_touch.widget =
                            bottom_buttons.widgets[selected.index];
                    }
                    g_ui_touch.mode = UiTouchMode::ConversationResponse;
                    ActivateTouchWidgetHighlightLocked(window_id, window);
                    RememberTouchHighlightChoicesLocked(
                        window_id, window, bottom_buttons);
                    SetTouchChoiceHighlightWidgetLocked(
                        window_id, window, g_ui_touch.widget);
                    uint32_t choice_count = 0u;
                    float fallback_content_extent_pixels = 0.0f;
                    const bool has_fallback_content_extent =
                        ReadConversationChoiceCountLocked(window, &choice_count) &&
                        EstimateConversationContentExtentPixelsLocked(
                            bottom_buttons, choice_count,
                            transform.surface_width, transform.surface_height,
                            &fallback_content_extent_pixels);
                    for (size_t index = 0; index < scrollbars.count; ++index) {
                        const uint32_t scrollbar = scrollbars.widgets[index];
                        if (!ValidGuestPointerLocked(scrollbar) ||
                            !GuestAddressRange(*g_host, scrollbar + 700u,
                                               3u * sizeof(uint32_t))) {
                            continue;
                        }
                        const warband::input::ScrollbarDragModel drag{
                            y,
                            scrollbars.rects[index].top,
                            scrollbars.rects[index].bottom,
                            ReadGuestFloatLocked(scrollbar + 700u),
                            ReadGuestFloatLocked(scrollbar + 704u),
                            ReadGuestFloatLocked(scrollbar + 708u),
                        };
                        const float normalized_extent =
                            scrollbars.content_extents[index];
                        float content_extent_pixels = 0.0f;
                        const float guest_y_scale =
                            transform.coordinate_scale * transform.y_factor;
                        if (std::isfinite(normalized_extent) &&
                            normalized_extent > 0.0f &&
                            std::isfinite(guest_y_scale) &&
                            guest_y_scale > 0.0f) {
                            content_extent_pixels = normalized_extent *
                                transform.surface_height / guest_y_scale;
                        } else if (has_fallback_content_extent) {
                            content_extent_pixels =
                                fallback_content_extent_pixels;
                        }
                        const warband::input::ScrollableContentDragModel
                            content_drag{
                                y,
                                content_extent_pixels,
                                drag.value_start,
                                drag.minimum,
                                drag.maximum,
                            };
                        if (!warband::input::ValidScrollbarDragModel(drag) ||
                            !warband::input::ValidScrollableContentDragModel(
                                content_drag)) {
                            continue;
                        }
                        g_ui_touch.scrollbar_widget = scrollbar;
                        g_ui_touch.scrollbar_track =
                            scrollbars.rects[index];
                        g_ui_touch.scrollbar_drag = drag;
                        g_ui_touch.content_drag = content_drag;
                        break;
                    }
                    LOGI("conversation response captured widget=0x%08X "
                         "scrollbars=%zu active_scrollbar=0x%08X y=%.1f "
                         "choices=%u fallback_extent_px=%.1f",
                         g_ui_touch.widget, scrollbars.count,
                         g_ui_touch.scrollbar_widget, y, choice_count,
                         fallback_content_extent_pixels);
                    // The old mobile DOWN/UP path rebuilds the response rows
                    // at index zero.  Own this stream from DOWN: a stationary
                    // gesture becomes one synthetic row event, while a drag
                    // writes the associated scrollbar directly.
                    SetTouchActionOverrideLocked(event);
                    return;
                }
                BeginSyntheticUiTouchLocked(
                    event, AMotionEvent_getPointerId(event, 0u), x, y,
                    window_id, window, bottom_buttons.widgets[selected.index],
                    UiTouchMode::SyntheticBottomButton);
                LOGI("bottom button touch captured window=%u widget=0x%08X "
                     "expanded=%d", window_id,
                     bottom_buttons.widgets[selected.index],
                     !selected.point_was_inside);
                return;
            }
        }

        if (window_id == kMapWindowId && bottom_buttons.count != 0u) {
            float dead_zone_top = transform.surface_height;
            for (size_t index = 0; index < bottom_buttons.count; ++index) {
                const auto& rect = bottom_buttons.rects[index];
                dead_zone_top = std::min(
                    dead_zone_top, rect.top - (rect.bottom - rect.top));
            }
            dead_zone_top = std::clamp(
                dead_zone_top, transform.surface_height * 0.65f,
                transform.surface_height);
            if (y >= dead_zone_top) {
                BeginSyntheticUiTouchLocked(
                    event, AMotionEvent_getPointerId(event, 0u), x, y,
                    window_id, window, 0u, UiTouchMode::MapDeadZone);
                LOGI("map bottom dead zone captured y=%.1f top=%.1f", y,
                     dead_zone_top);
                return;
            }
        }

        if (scrollbars.count == 0u) return;
        const float padding_x = std::clamp(UiTouchSlopLocked() * 1.35f,
                                           8.0f, 72.0f);
        const auto selected = warband::input::SelectRightExpandedTouchTarget(
            scrollbars.rects.data(), scrollbars.count, x, y,
            padding_x, transform.surface_width,
            transform.surface_height, scrollbars.clips.data());
        if (!selected) return;
        const uint32_t scrollbar = scrollbars.widgets[selected.index];
        if (!ValidGuestPointerLocked(scrollbar) ||
            !GuestAddressRange(*g_host, scrollbar + 700u,
                               3u * sizeof(uint32_t))) {
            return;
        }
        const uint32_t scrollbar_vtable =
            g_host->MemoryRead32(scrollbar + kWidgetVtableOffset);
        if (!ValidGuestPointerLocked(scrollbar_vtable) ||
            g_host->MemoryRead32(scrollbar_vtable + 236u) !=
                g_scrollbar_get_type_va) {
            return;
        }
        const warband::input::ScrollbarDragModel drag{
            y,
            scrollbars.rects[selected.index].top,
            scrollbars.rects[selected.index].bottom,
            ReadGuestFloatLocked(scrollbar + 700u),
            ReadGuestFloatLocked(scrollbar + 704u),
            ReadGuestFloatLocked(scrollbar + 708u),
        };
        if (!warband::input::ValidScrollbarDragModel(drag)) return;

        g_ui_touch.active = true;
        g_ui_touch.mode = UiTouchMode::ScrollbarDrag;
        g_ui_touch.pointer_id = AMotionEvent_getPointerId(event, 0u);
        g_ui_touch.down_x = x;
        g_ui_touch.down_y = y;
        g_ui_touch.window_id = window_id;
        g_ui_touch.window_object = window;
        g_ui_touch.widget = scrollbar;
        g_ui_touch.scrollbar_track = scrollbars.rects[selected.index];
        g_ui_touch.scrollbar_drag = drag;
        if (window_id == warband::input::kConversationWindowId) {
            ActivateTouchWidgetHighlightLocked(window_id, window);
            g_conversation_navigation_cleanup_pending = true;
        }
        SetTouchActionOverrideLocked(event);
        LOGI("scrollbar drag captured window=%u widget=0x%08X "
             "track=%.1f..%.1f value=%.5f range=%.5f..%.5f",
             window_id, scrollbar, drag.track_top, drag.track_bottom,
             drag.value_start, drag.minimum, drag.maximum);
        if (!selected.point_was_inside) {
            LOGI("scrollbar touch target expanded window=%u widget=0x%08X "
                 "snap_x=%.1f", window_id, scrollbar, selected.snap_x);
        }
        return;
    }

    if (!g_ui_touch.active) return;
    uint32_t active_window_id = kNoActiveWindowId;
    uint32_t active_window = 0;
    if (!g_host || !g_game_screen ||
        !ReadStableActiveWindowId(*g_host, g_game_screen, &active_window_id) ||
        active_window_id != g_ui_touch.window_id ||
        !ResolveActiveWindowObjectLocked(active_window_id, &active_window) ||
        active_window != g_ui_touch.window_object) {
        EndUiTouchOnEventLocked(event, true);
        return;
    }
    if (action == AMOTION_EVENT_ACTION_POINTER_DOWN ||
        action == AMOTION_EVENT_ACTION_CANCEL || pointer_count == 0u) {
        EndUiTouchOnEventLocked(
            event, action != AMOTION_EVENT_ACTION_CANCEL);
        return;
    }
    const int32_t pointer_index = MotionPointerIndex(event, g_ui_touch.pointer_id);
    if (pointer_index < 0) {
        EndUiTouchOnEventLocked(event, true);
        return;
    }

    if (g_ui_touch.mode == UiTouchMode::ScrollbarDrag) {
        SetTouchActionOverrideLocked(event);
        const uint32_t scrollbar = g_ui_touch.widget;
        if (!ValidGuestPointerLocked(scrollbar) ||
            !GuestAddressRange(*g_host, scrollbar + 700u,
                               3u * sizeof(uint32_t))) {
            EndUiTouchOnEventLocked(event, true);
            return;
        }
        const uint32_t scrollbar_vtable =
            g_host->MemoryRead32(scrollbar + kWidgetVtableOffset);
        if (!ValidGuestPointerLocked(scrollbar_vtable) ||
            g_host->MemoryRead32(scrollbar_vtable + 236u) !=
                g_scrollbar_get_type_va) {
            EndUiTouchOnEventLocked(event, true);
            return;
        }
        if (action == AMOTION_EVENT_ACTION_MOVE) {
            float next_value = 0.0f;
            const float pointer_y = AMotionEvent_getY(
                event, static_cast<size_t>(pointer_index));
            if (!warband::input::ScrollbarValueForPointer(
                    g_ui_touch.scrollbar_drag, pointer_y, &next_value)) {
                EndUiTouchOnEventLocked(event, true);
                return;
            }
            g_host->MemoryWrite32(scrollbar + 700u, FloatBits(next_value));
            if (g_ui_touch.window_id ==
                warband::input::kConversationWindowId) {
                RememberConversationScrollPositionLocked(
                    g_ui_touch.window_object, g_ui_touch.scrollbar_track,
                    next_value, g_ui_touch.scrollbar_drag.minimum,
                    g_ui_touch.scrollbar_drag.maximum);
            }
        }
    }

    if (g_ui_touch.mode == UiTouchMode::ConversationResponse) {
        SetTouchActionOverrideLocked(event);
        const float x = AMotionEvent_getX(
            event, static_cast<size_t>(pointer_index));
        const float y = AMotionEvent_getY(
            event, static_cast<size_t>(pointer_index));
        const float dx = x - g_ui_touch.down_x;
        const float dy = y - g_ui_touch.down_y;
        if (std::sqrt(dx * dx + dy * dy) > UiTouchSlopLocked() * 2.0f) {
            g_ui_touch.tap_eligible = false;
        }
        if (action == AMOTION_EVENT_ACTION_MOVE) {
            g_ui_touch.last_move_x = x;
            g_ui_touch.last_move_y = y;
            UpdateTouchChoiceHighlightAtPointLocked(
                g_ui_touch.window_id, g_ui_touch.window_object, x, y);
        }
        const uint32_t scrollbar = g_ui_touch.scrollbar_widget;
        if (warband::input::ShouldApplyScrollableContentDragSample(
                action == AMOTION_EVENT_ACTION_MOVE,
                g_ui_touch.tap_eligible) && scrollbar &&
            ValidGuestPointerLocked(scrollbar) &&
            GuestAddressRange(*g_host, scrollbar + 700u,
                              3u * sizeof(uint32_t))) {
            float next_value = 0.0f;
            if (warband::input::ScrollbarValueForContentPointer(
                    g_ui_touch.content_drag, y, &next_value)) {
                g_host->MemoryWrite32(scrollbar + 700u,
                                      FloatBits(next_value));
                RememberConversationScrollPositionLocked(
                    g_ui_touch.window_object, g_ui_touch.scrollbar_track,
                    next_value, g_ui_touch.content_drag.minimum,
                    g_ui_touch.content_drag.maximum);
                g_ui_touch.last_scroll_value = next_value;
                g_ui_touch.has_scroll_sample = true;
                LOGI("conversation response scroll widget=0x%08X "
                     "pointer_y=%.1f value=%.5f",
                     scrollbar, y, next_value);
            }
        }
    }

    if (g_ui_touch.mode == UiTouchMode::MenuChoiceHighlight &&
        action == AMOTION_EVENT_ACTION_MOVE) {
        const float x = AMotionEvent_getX(
            event, static_cast<size_t>(pointer_index));
        const float y = AMotionEvent_getY(
            event, static_cast<size_t>(pointer_index));
        g_ui_touch.last_move_x = x;
        g_ui_touch.last_move_y = y;
        UpdateTouchChoiceHighlightAtPointLocked(
            g_ui_touch.window_id, g_ui_touch.window_object, x, y);
    }

    if (g_ui_touch.mode == UiTouchMode::SyntheticBottomButton ||
        g_ui_touch.mode == UiTouchMode::MapDeadZone) {
        SetTouchActionOverrideLocked(event);
        const float x = AMotionEvent_getX(
            event, static_cast<size_t>(pointer_index));
        const float y = AMotionEvent_getY(
            event, static_cast<size_t>(pointer_index));
        const float dx = x - g_ui_touch.down_x;
        const float dy = y - g_ui_touch.down_y;
        if (std::sqrt(dx * dx + dy * dy) > UiTouchSlopLocked() * 2.0f) {
            g_ui_touch.tap_eligible = false;
        }
    }

    if (action == AMOTION_EVENT_ACTION_UP ||
        action == AMOTION_EVENT_ACTION_POINTER_UP) {
        const size_t action_index = static_cast<size_t>(
            (raw_action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
            AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
        if (action_index >= pointer_count ||
            AMotionEvent_getPointerId(event, action_index) ==
                g_ui_touch.pointer_id) {
            if (g_ui_touch.mode == UiTouchMode::ConversationResponse &&
                !g_ui_touch.tap_eligible &&
                g_ui_touch.has_scroll_sample) {
                LOGI("conversation response release retained value=%.5f "
                     "last_move=%.1f,%.1f up=%.1f,%.1f highlight=0x%08X",
                     g_ui_touch.last_scroll_value,
                     g_ui_touch.last_move_x, g_ui_touch.last_move_y,
                     AMotionEvent_getX(
                         event, static_cast<size_t>(pointer_index)),
                     AMotionEvent_getY(
                         event, static_cast<size_t>(pointer_index)),
                     g_ui_touch.highlight_widget);
            }
            if ((g_ui_touch.mode == UiTouchMode::SyntheticBottomButton ||
                 g_ui_touch.mode == UiTouchMode::ConversationResponse) &&
                g_ui_touch.tap_eligible && g_ui_touch.widget &&
                !g_ui_button_pending) {
                g_ui_button_pending = true;
                g_ui_button_pending_window = g_ui_touch.window_id;
                g_ui_button_pending_window_object = g_ui_touch.window_object;
                g_ui_button_pending_widget = g_ui_touch.widget;
            }
            EndUiTouchOnEventLocked(
                event, action_index >= pointer_count);
        }
    }
}

void ApplyUiButtonTapLocked() {
    if (!g_ui_button_pending) return;
    const uint32_t window_id = g_ui_button_pending_window;
    const uint32_t expected_window = g_ui_button_pending_window_object;
    const uint32_t widget = g_ui_button_pending_widget;
    g_ui_button_pending = false;
    g_ui_button_pending_window = kNoActiveWindowId;
    g_ui_button_pending_window_object = 0;
    g_ui_button_pending_widget = 0;

    uint32_t active_window_id = kNoActiveWindowId;
    uint32_t active_window = 0;
    if (!g_host || !g_game_screen || !g_simple_button_vtable ||
        !ReadStableActiveWindowId(*g_host, g_game_screen, &active_window_id) ||
        active_window_id != window_id ||
        !ResolveActiveWindowObjectLocked(active_window_id, &active_window) ||
        active_window != expected_window || !ValidGuestPointerLocked(widget) ||
        g_host->MemoryRead32(widget + kWidgetVtableOffset) !=
            g_simple_button_vtable ||
        (g_host->MemoryRead32(widget + kWidgetFlagsOffset) & 0x3u) != 0u) {
        return;
    }
    if (window_id == warband::input::kConversationWindowId) {
        ClearConversationScrollRetentionLocked();
    }
    AddInventoryWidgetEventLocked(widget);
    ClearInventoryWidgetDragPersistenceLocked(widget);
    LOGI("synthetic bottom button queued window=%u widget=0x%08X",
         window_id, widget);
}

void ApplyOrdersTapLocked() {
    if (!g_orders_tap_pending) return;
    const uint32_t expected_tactical = g_orders_tap_pending_tactical;
    const uint32_t row = g_orders_tap_pending_row;
    g_orders_tap_pending = false;
    g_orders_tap_pending_tactical = 0;
    g_orders_tap_pending_row = 0;

    uint32_t active_window_id = kNoActiveWindowId;
    uint32_t tactical = 0;
    if (!g_host || !g_game_screen ||
        !ReadStableActiveWindowId(*g_host, g_game_screen, &active_window_id) ||
        active_window_id != kTacticalWindowId ||
        !ResolveActiveWindowObjectLocked(active_window_id, &tactical) ||
        tactical != expected_tactical ||
        !GuestAddressRange(*g_host, tactical + kTacticalOrdersRootWidgetOffset,
                           kTacticalOrdersRowCountOffset -
                               kTacticalOrdersRootWidgetOffset +
                               sizeof(uint32_t))) {
        return;
    }

    const uint32_t root =
        g_host->MemoryRead32(tactical + kTacticalOrdersRootWidgetOffset);
    const uint32_t row_count =
        g_host->MemoryRead32(tactical + kTacticalOrdersRowCountOffset);
    if (!ValidGuestPointerLocked(root) ||
        !GuestAddressRange(*g_host, root + kWidgetFlagsOffset,
                           sizeof(uint32_t)) ||
        (g_host->MemoryRead32(root + kWidgetFlagsOffset) & 0x3u) != 0u ||
        row >= row_count || row_count > kTacticalOrdersRowWidgetCount) {
        return;
    }

    const uint32_t row_widget = g_host->MemoryRead32(
        tactical + kTacticalOrdersFirstRowWidgetOffset +
        row * sizeof(uint32_t));
    if (!ValidGuestPointerLocked(row_widget) ||
        !GuestAddressRange(*g_host, row_widget + kWidgetFlagsOffset,
                           sizeof(uint32_t)) ||
        (g_host->MemoryRead32(row_widget + kWidgetFlagsOffset) & 0x3u) != 0u) {
        return;
    }

    // Tactical_window::handle_user_input already owns the complete command
    // state machine.  Selecting its live cursor row and pulsing D-pad Right
    // follows the same code path as controller navigation, including submenu
    // transitions, order execution, networking, and panel dismissal.
    g_host->MemoryWrite32(tactical + kTacticalOrdersCursorOffset, row);
    g_host->MemoryWrite8(tactical + kTacticalOrdersDirtyOffset, 1u);
    g_orders_pulses.Enqueue(
        {warband::input::kDpadRight, 1u}, MonotonicMilliseconds());
}

// Point-directed depth-first search for a vertical rglScrollable_widget.
// Choosing the viewport under the finger is essential on Notes/Characters:
// both columns have independent scrollbars, and a blind reverse walk always
// selected the left one.  Children win over parents for nested scrollables.
bool ScrollWidgetTreeLocked(
        uint32_t widget, float dy_fraction, int depth, bool trace,
        const warband::input::WidgetCoordinateTransform& transform,
        float touch_x, float touch_y) {
    if (depth > 10) return false;
    if (!ValidGuestPointerLocked(widget)) return false;
    // Hidden or disabled containers (rglWidget_base::hide/disable set flags
    // bits 0/1 at +0x1BC) own invisible subtrees: the deactivated custom
    // settings scrollable is the last root child, so without this guard the
    // reverse depth-first search scrolls the hidden tab instead of the
    // visible native Options content.
    const uint32_t widget_flags = g_host->MemoryRead32(widget + 0x1BCu);
    if ((widget_flags & 0x3u) != 0u) return false;
    const uint32_t vtable = g_host->MemoryRead32(widget);
    if (!ValidGuestPointerLocked(vtable)) return false;
    const uint32_t get_type_fn = g_host->MemoryRead32(vtable + 236u);
    if (!IsGuestContainerWidgetLocked(widget)) return false;
    const uint32_t children_begin = g_host->MemoryRead32(widget + 860u);
    const uint32_t children_end = g_host->MemoryRead32(widget + 864u);
    if (ValidGuestPointerLocked(children_begin) &&
        ValidGuestPointerLocked(children_end) && children_end > children_begin &&
        (children_end - children_begin) <= 4u * 4096u) {
        for (uint32_t slot = children_end - sizeof(uint32_t);;
             slot -= sizeof(uint32_t)) {
            if (ScrollWidgetTreeLocked(
                    g_host->MemoryRead32(slot), dy_fraction, depth + 1,
                    trace, transform, touch_x, touch_y)) {
                return true;
            }
            if (slot == children_begin) break;
        }
    }

    if (get_type_fn != g_scrollable_get_type_va ||
        !GuestAddressRange(*g_host, widget + 1852u, 16u)) {
        return false;
    }
    warband::input::TouchTargetRect viewport_rect{};
    if (!ReadVisibleWidgetScreenRectLocked(
            widget, transform, 0u, &viewport_rect) ||
        !warband::input::TouchTargetContains(
            viewport_rect, touch_x, touch_y)) {
        return false;
    }
    const uint32_t scrollbar = g_host->MemoryRead32(widget + 1852u);
    if (!ValidGuestPointerLocked(scrollbar) ||
        g_host->MemoryRead32(widget + 1860u) != 1u) {
        if (trace) {
            static uint32_t inactive_reports = 0;
            if (inactive_reports++ < 16u) {
                LOGI("scroll_trace scrollable=0x%08X depth=%d flags=0x%X "
                     "scrollbar=0x%08X active=%u",
                     widget, depth, widget_flags, scrollbar,
                     ValidGuestPointerLocked(scrollbar)
                         ? g_host->MemoryRead32(widget + 1860u) : 0u);
            }
        }
        return false;
    }
    const uint32_t scrollbar_vtable = g_host->MemoryRead32(scrollbar);
    if (!ValidGuestPointerLocked(scrollbar_vtable) ||
        g_host->MemoryRead32(scrollbar_vtable + 236u) !=
            g_scrollbar_get_type_va) {
        return false;
    }
    const uint32_t outer = g_host->MemoryRead32(widget + 1864u);
    if (!ValidGuestPointerLocked(outer)) return false;
    const float content = ReadGuestFloatLocked(outer + 432u) -
                          ReadGuestFloatLocked(outer + 440u);
    const float viewport = ReadGuestFloatLocked(widget + 432u);
    const float range = content - viewport;
    if (!(range > 1.0e-5f)) return false;
    const float value = ReadGuestFloatLocked(scrollbar + 700u);
    const float minimum = ReadGuestFloatLocked(scrollbar + 704u);
    const float maximum = ReadGuestFloatLocked(scrollbar + 708u);
    if (!(maximum > minimum)) return false;
    // The scrollable tick derives its content offset from this value every
    // frame, so writing the field is sufficient; no guest call is needed.
    const float next = std::clamp(
        value + dy_fraction / range, minimum, maximum);
    g_host->MemoryWrite32(scrollbar + 700u, FloatBits(next));
    return true;
}

void ApplyMenuScrollLocked() {
    if (!g_menu_scroll_pending) return;
    // Item DOWN is classified synchronously. This guard now applies only to a
    // claimed item/recovery stream; empty-space inventory scrolling arrives
    // with tracking=false and is applied on the first MOVE frame.
    if (warband::input::ShouldDeferInventoryMenuScroll(
            g_inventory_touch.tracking, g_inventory_touch.drag_started)) {
        return;
    }
    g_menu_scroll_pending = false;
    const float dy = g_menu_scroll_pending_dy;
    const float touch_x = g_menu_scroll_pending_x;
    const float touch_y = g_menu_scroll_pending_y;
    const uint32_t expected_window_id = g_menu_scroll_pending_window;
    g_menu_scroll_pending_dy = 0.0f;
    g_menu_scroll_pending_x = 0.0f;
    g_menu_scroll_pending_y = 0.0f;
    g_menu_scroll_pending_window = kNoActiveWindowId;
    uint32_t active_window_id = kNoActiveWindowId;
    if (!g_host || !g_game_screen ||
        !ReadStableActiveWindowId(*g_host, g_game_screen, &active_window_id) ||
        active_window_id != expected_window_id ||
        active_window_id == kMapWindowId) {
        // Never replay a pending delta after a screen transition.  Inventory
        // is intentionally admitted: empty-space swipes are bridge-scrolled.
        return;
    }
    const bool scroll_trace = ScrollTraceEnabled();
    warband::input::WidgetCoordinateTransform transform{};
    if (dy == 0.0f || !std::isfinite(touch_x) || !std::isfinite(touch_y) ||
        !g_scrollable_get_type_va || !g_scrollbar_get_type_va ||
        !ReadCharacterCoordinateTransformLocked(&transform)) {
        return;
    }
    if (active_window_id == kNotesWindowId) {
        uint32_t notes_window = 0;
        if (ResolveActiveWindowObjectLocked(active_window_id, &notes_window)) {
            for (uint32_t offset = kNotesScrollableFirstOffset;
                 offset <= kNotesScrollableLastOffset;
                 offset += sizeof(uint32_t)) {
                const uint64_t field64 = static_cast<uint64_t>(notes_window) +
                                         offset;
                if (field64 > std::numeric_limits<uint32_t>::max() ||
                    !GuestAddressRange(*g_host, field64, sizeof(uint32_t))) {
                    continue;
                }
                const uint32_t scrollable = g_host->MemoryRead32(
                    static_cast<uint32_t>(field64));
                if (ScrollWidgetTreeLocked(
                        scrollable, dy, 0, scroll_trace, transform,
                        touch_x, touch_y)) {
                    static uint32_t notes_applied_reports = 0;
                    if (notes_applied_reports++ < 8u) {
                        LOGI("notes field scroll applied dy=%.4f "
                             "touch=%.1f,%.1f field=0x%X widget=0x%08X",
                             dy, touch_x, touch_y, offset, scrollable);
                    }
                    return;
                }
            }
        }
    }
    // rgl_window_manager is the exported object; its windows vector lives at
    // +0/+4.  The topmost window is the last element.
    const uint32_t window_manager = g_host->FindSymbol("rgl_window_manager");
    if (!window_manager || !ValidGuestPointerLocked(window_manager)) return;
    const uint32_t begin = g_host->MemoryRead32(window_manager);
    const uint32_t end = g_host->MemoryRead32(window_manager + 4u);
    if (!ValidGuestPointerLocked(begin) || !ValidGuestPointerLocked(end) ||
        end < begin || (end - begin) > 4u * 64u) {
        return;
    }
    for (uint32_t slot = end - sizeof(uint32_t);; slot -= sizeof(uint32_t)) {
        const uint32_t window = g_host->MemoryRead32(slot);
        if (ScrollWidgetTreeLocked(
                window, dy, 0, scroll_trace, transform, touch_x, touch_y)) {
            static uint32_t applied_reports = 0;
            if (applied_reports++ < 8) {
                LOGI("menu drag scroll applied dy=%.4f touch=%.1f,%.1f "
                     "active_window=%u root=0x%08X", dy, touch_x, touch_y,
                     active_window_id, window);
            }
            return;
        }
        if (slot == begin) return;
    }
}

void ApplyInventoryEdgeScrollLocked(float frame_seconds) {
    if (!g_inventory_touch.tracking || !g_inventory_touch.drag_started ||
        g_inventory_touch.release_pending ||
        !std::isfinite(g_inventory_touch.last_x) ||
        !std::isfinite(g_inventory_touch.last_y)) {
        return;
    }
    const InventoryGuestDragSnapshot guest_drag =
        ReadInventoryGuestDragLocked();
    if (!guest_drag.readable || !guest_drag.active) return;

    uint32_t window_id = kNoActiveWindowId;
    uint32_t window = 0;
    warband::input::WidgetCoordinateTransform transform{};
    if (!g_host || !g_game_screen ||
        !ReadStableActiveWindowId(*g_host, g_game_screen, &window_id) ||
        window_id != kInventoryWindowId ||
        !ResolveActiveWindowObjectLocked(window_id, &window) ||
        !ReadCharacterCoordinateTransformLocked(&transform)) {
        return;
    }
    // Use the captured pane geometry as the cheap broad phase.  The previous
    // screen-relative 25..75% filter cut through the lower one-sixth of the
    // actual list on Pixel (track 80..920, lower band 780..920, filter through
    // 810), so host geometry tests passed while part of the runtime zone was
    // unreachable.
    std::array<warband::input::TouchTargetRect, 16u> cached_tracks{};
    size_t cached_track_count = 0u;
    for (size_t index = 0;
         index < g_inventory_touch.scrollbar_snapshot_count &&
         cached_track_count < cached_tracks.size(); ++index) {
        const auto& snapshot = g_inventory_touch.scrollbar_snapshots[index];
        if (!snapshot.valid) continue;
        cached_tracks[cached_track_count++] = snapshot.track;
    }
    if (cached_track_count != 0u) {
        const auto cached_decision =
            warband::input::DecideInventoryEdgeScroll(
                cached_tracks.data(), cached_track_count,
                g_inventory_touch.last_x, g_inventory_touch.last_y,
                transform.surface_width, transform.surface_height,
                frame_seconds);
        if (!warband::input::ShouldApplyInventoryEdgeScroll(
                &g_inventory_touch.edge_scroll_gate, cached_decision)) {
            return;
        }
    }

    UiTouchTargetSet scrollbars{};
    GatherUiTouchTargetsLocked(
        window_id, window, transform, nullptr, &scrollbars);
    const auto decision = warband::input::DecideInventoryEdgeScroll(
        scrollbars.rects.data(), scrollbars.count,
        g_inventory_touch.last_x, g_inventory_touch.last_y,
        transform.surface_width, transform.surface_height, frame_seconds);
    if (cached_track_count == 0u &&
        !warband::input::ShouldApplyInventoryEdgeScroll(
            &g_inventory_touch.edge_scroll_gate, decision)) {
        return;
    }
    if (!decision || decision.scrollbar_index >= scrollbars.count) return;

    const uint32_t scrollbar =
        scrollbars.widgets[decision.scrollbar_index];
    if (!ValidGuestPointerLocked(scrollbar) ||
        !GuestAddressRange(*g_host, scrollbar + 700u,
                           3u * sizeof(uint32_t))) {
        return;
    }
    const uint32_t scrollbar_vtable =
        g_host->MemoryRead32(scrollbar + kWidgetVtableOffset);
    if (!ValidGuestPointerLocked(scrollbar_vtable) ||
        g_host->MemoryRead32(scrollbar_vtable + 236u) !=
            g_scrollbar_get_type_va) {
        return;
    }
    const float value = ReadGuestFloatLocked(scrollbar + 700u);
    const float minimum = ReadGuestFloatLocked(scrollbar + 704u);
    const float maximum = ReadGuestFloatLocked(scrollbar + 708u);
    if (!std::isfinite(value) || !std::isfinite(minimum) ||
        !std::isfinite(maximum) || !(maximum > minimum)) {
        return;
    }
    const float next = std::clamp(
        value + decision.value_range_delta * (maximum - minimum),
        minimum, maximum);
    if (next == value) return;
    g_host->MemoryWrite32(scrollbar + 700u, FloatBits(next));
    UpdateInventoryScrollSnapshotLocked(
        scrollbars.rects[decision.scrollbar_index], next, minimum, maximum,
        transform.surface_width, transform.surface_height);
    if (InventoryTraceEnabled()) {
        static uint32_t edge_reports = 0u;
        if (edge_reports < 8u || edge_reports % 30u == 0u) {
            LOGI("inventory held edge scroll widget=0x%08X "
                 "value=%.6f next=%.6f delta=%.6f touch=%.1f,%.1f",
                 scrollbar, value, next, decision.value_range_delta,
                 g_inventory_touch.last_x, g_inventory_touch.last_y);
        }
        ++edge_reports;
    }
}

bool ResolveMapWindowObjectLocked(uint32_t* map_window_out) {
    if (!map_window_out || !g_host || !g_game_screen) return false;
    uint32_t active_window_id = kNoActiveWindowId;
    if (!ReadStableActiveWindowId(*g_host, g_game_screen, &active_window_id) ||
        active_window_id != kMapWindowId) {
        return false;
    }
    const uint64_t slot64 = static_cast<uint64_t>(g_game_screen) +
                            sizeof(uint32_t) * (1u + kMapWindowId);
    if (!GuestAddressRange(*g_host, slot64, sizeof(uint32_t))) return false;
    const uint32_t map_window = g_host->MemoryRead32(static_cast<uint32_t>(slot64));
    if (!map_window ||
        !GuestAddressRange(*g_host, map_window, kMapCameraObjectSpan)) {
        return false;
    }
    *map_window_out = map_window;
    return true;
}

uint32_t ResolveCurrentGameObjectLocked() {
    if (!g_host || !g_cur_game_slot ||
        !GuestAddressRange(*g_host, g_cur_game_slot, sizeof(uint32_t))) {
        return 0;
    }
    const uint32_t current_game = g_host->MemoryRead32(g_cur_game_slot);
    if (!current_game ||
        !GuestAddressRange(*g_host, current_game, kCurrentGameCameraStateSpan)) {
        return 0;
    }
    return current_game;
}

uint32_t ResolvePlayerPartyLocked(uint32_t current_game) {
    if (!g_host || !current_game ||
        !GuestAddressRange(*g_host,
            static_cast<uint64_t>(current_game) + kCurrentGamePartyCountOffset,
            sizeof(uint32_t))) {
        return 0;
    }
    const uint32_t party_id = g_host->MemoryRead32(
        current_game + kCurrentGamePlayerPartyOffset);
    const uint32_t party_count = g_host->MemoryRead32(
        current_game + kCurrentGamePartyCountOffset);
    if (party_id >= party_count) return 0;

    constexpr uint32_t kHashSlots = 10007u;
    uint32_t slot = party_id < kHashSlots ? party_id : party_id % kHashSlots;
    uint32_t dense_index = std::numeric_limits<uint32_t>::max();
    for (uint32_t probe = 0; probe <= 1000u; ++probe) {
        const uint64_t entry = static_cast<uint64_t>(current_game) + 2000u +
                               8u * slot;
        if (!GuestAddressRange(*g_host, entry, 2u * sizeof(uint32_t))) return 0;
        if (g_host->MemoryRead32(static_cast<uint32_t>(entry)) == party_id) {
            dense_index = g_host->MemoryRead32(static_cast<uint32_t>(entry + 4u));
            break;
        }
        slot = (slot + 79u) % kHashSlots;
    }
    if (dense_index >= party_count) return 0;
    const uint32_t chunks = g_host->MemoryRead32(
        current_game + kCurrentGamePartyChunksOffset);
    const uint64_t chunk_slot = static_cast<uint64_t>(chunks) +
                                4u * (dense_index >> 4);
    if (!chunks || !GuestAddressRange(*g_host, chunk_slot, sizeof(uint32_t))) {
        return 0;
    }
    const uint32_t chunk = g_host->MemoryRead32(
        static_cast<uint32_t>(chunk_slot));
    const uint64_t party = static_cast<uint64_t>(chunk) +
                           23824u * (dense_index & 15u);
    if (!chunk || !GuestAddressRange(*g_host, party, kPlayerPartySpan)) return 0;
    return static_cast<uint32_t>(party);
}

MapRouteSnapshot ReadMapRouteLocked(uint32_t current_game, uint32_t party) {
    MapRouteSnapshot route{};
    route.current_game = current_game;
    route.party = party;
    for (size_t index = 0; index < route.behavior.size(); ++index) {
        route.behavior[index] = g_host->MemoryRead32(
            party + kPlayerPartyBehaviorOffset + 4u * index);
    }
    for (size_t index = 0; index < route.target.size(); ++index) {
        route.target[index] = g_host->MemoryRead32(
            party + kPlayerPartyTargetOffset + 4u * index);
    }
    for (size_t index = 0; index < route.movement.size(); ++index) {
        route.movement[index] = g_host->MemoryRead32(
            party + kPlayerPartyMovementOffset + 4u * index);
    }
    return route;
}

void WriteMapRouteLocked(const MapRouteSnapshot& route) {
    for (size_t index = 0; index < route.behavior.size(); ++index) {
        g_host->MemoryWrite32(route.party + kPlayerPartyBehaviorOffset + 4u * index,
                              route.behavior[index]);
    }
    for (size_t index = 0; index < route.target.size(); ++index) {
        g_host->MemoryWrite32(route.party + kPlayerPartyTargetOffset + 4u * index,
                              route.target[index]);
    }
    for (size_t index = 0; index < route.movement.size(); ++index) {
        g_host->MemoryWrite32(route.party + kPlayerPartyMovementOffset + 4u * index,
                              route.movement[index]);
    }
}

void ClearMapRouteLocked() {
    g_map_route_snapshot = {};
    g_map_route_candidate = {};
    g_map_marker_candidate = {};
    g_map_route_restore_pending = false;
    g_map_route_commit_pending = false;
}

// The yellow world-map arrow is the scene body at Game+104632. Its two
// position copies are updated by Map_window::handle_mouse on every raw DOWN,
// including the DOWN that later becomes a camera pan. Keep the last confirmed
// destination in world coordinates so camera movement cannot reposition it.
uint32_t ResolveMapMarkerLocked(uint32_t current_game) {
    constexpr uint32_t kMapMarkerOffset = 104632u;
    if (!current_game || !GuestAddressRange(*g_host,
            static_cast<uint64_t>(current_game) + kMapMarkerOffset,
            sizeof(uint32_t))) return 0u;
    const uint32_t marker = g_host->MemoryRead32(
        current_game + kMapMarkerOffset);
    return marker && GuestAddressRange(*g_host, marker, 148u)
        ? marker : 0u;
}

void CaptureMapMarkerLocked(uint32_t current_game, uint32_t marker) {
    if (!marker) {
        g_map_marker_anchor = {};
        return;
    }
    g_map_marker_anchor.current_game = current_game;
    g_map_marker_anchor.marker = marker;
    for (uint32_t index = 0; index < 3u; ++index) {
        g_map_marker_anchor.bounds_position[index] = g_host->MemoryRead32(
            marker + 72u + 4u * index);
        g_map_marker_anchor.transform_position[index] = g_host->MemoryRead32(
            marker + 136u + 4u * index);
    }
}

MapMarkerAnchor ReadMapMarkerLocked(uint32_t current_game, uint32_t marker) {
    MapMarkerAnchor position{};
    if (!marker) return position;
    position.current_game = current_game;
    position.marker = marker;
    for (uint32_t index = 0; index < 3u; ++index) {
        position.bounds_position[index] = g_host->MemoryRead32(
            marker + 72u + 4u * index);
        position.transform_position[index] = g_host->MemoryRead32(
            marker + 136u + 4u * index);
    }
    return position;
}

bool MapMarkerMoved(const MapMarkerAnchor& before,
                    const MapMarkerAnchor& after) {
    return before.current_game == after.current_game &&
        before.marker != 0u && before.marker == after.marker &&
        (before.bounds_position != after.bounds_position ||
         before.transform_position != after.transform_position);
}

void RestoreMapMarkerLocked(uint32_t current_game, uint32_t marker) {
    if (g_map_marker_anchor.current_game != current_game ||
        g_map_marker_anchor.marker != marker) return;
    for (uint32_t index = 0; index < 3u; ++index) {
        g_host->MemoryWrite32(marker + 72u + 4u * index,
                              g_map_marker_anchor.bounds_position[index]);
        g_host->MemoryWrite32(marker + 136u + 4u * index,
                              g_map_marker_anchor.transform_position[index]);
    }
}

void CaptureMapRouteLocked() {
    const uint32_t current_game = ResolveCurrentGameObjectLocked();
    const uint32_t party = ResolvePlayerPartyLocked(current_game);
    if (!party) {
        ClearMapRouteLocked();
        return;
    }
    const uint32_t marker = ResolveMapMarkerLocked(current_game);
    if (g_map_marker_anchor.current_game != current_game ||
        g_map_marker_anchor.marker != marker) {
        CaptureMapMarkerLocked(current_game, marker);
    }
    g_map_route_snapshot = ReadMapRouteLocked(current_game, party);
    g_map_route_candidate = {};
    g_map_marker_candidate = {};
    g_map_route_restore_pending = false;
    g_map_route_commit_pending = false;
}

bool MapRouteOrderChanged(const MapRouteSnapshot& before,
                          const MapRouteSnapshot& after) {
    return before.behavior != after.behavior || before.target != after.target;
}

bool MapPartyIsTravelling(uint32_t behavior) {
    return behavior == 1u || behavior == 4u || behavior == 6u ||
           behavior == 9u || behavior == 10u;
}

bool MapPartyReachedDestinationLocked(uint32_t party) {
    return party &&
        g_host->MemoryRead32(party + kPlayerPartyPositionOffset) ==
            g_host->MemoryRead32(party + kPlayerPartyTargetOffset) &&
        g_host->MemoryRead32(party + kPlayerPartyPositionOffset + 4u) ==
            g_host->MemoryRead32(party + kPlayerPartyTargetOffset + 4u);
}

void ApplyMapRouteAndTimeLocked(uint32_t current_game) {
    if (!g_host || !current_game ||
        current_game != ResolveCurrentGameObjectLocked() ||
        !GuestAddressRange(*g_host,
            static_cast<uint64_t>(current_game) + kCurrentGameMapTimeModeOffset,
            sizeof(uint32_t))) {
        ClearMapRouteLocked();
        g_map_marker_anchor = {};
        return;
    }
    const uint32_t party = ResolvePlayerPartyLocked(current_game);
    const uint32_t marker = ResolveMapMarkerLocked(current_game);
    if (g_map_marker_anchor.current_game != current_game ||
        g_map_marker_anchor.marker != marker) {
        CaptureMapMarkerLocked(current_game, marker);
    }
    const MapMarkerAnchor marker_now = ReadMapMarkerLocked(current_game, marker);
    bool touch_unresolved = false;
    bool accepted_order = false;
    if (g_map_route_snapshot.current_game) {
        if (g_map_route_snapshot.current_game != current_game ||
            g_map_route_snapshot.party != party || !party) {
            ClearMapRouteLocked();
        } else {
            const MapRouteSnapshot current = ReadMapRouteLocked(current_game, party);
            if (g_map_route_restore_pending) {
                WriteMapRouteLocked(g_map_route_snapshot);
                ClearMapRouteLocked();
            } else if (g_map_route_commit_pending) {
                const MapRouteSnapshot* accepted = &current;
                if (!MapRouteOrderChanged(g_map_route_snapshot, current) &&
                    g_map_route_candidate.current_game == current_game) {
                    accepted = &g_map_route_candidate;
                    WriteMapRouteLocked(*accepted);
                }
                if (MapRouteOrderChanged(g_map_route_snapshot, *accepted) &&
                    MapPartyIsTravelling(accepted->behavior[0])) {
                    g_map_time_running = true;
                    g_map_time_run_in_place = false;
                    accepted_order = true;
                }
                if (accepted_order) {
                    // DOWN can place the arrow before UP confirms the order.
                    // The provisional position was restored for each frame;
                    // commit that saved position with the accepted route.
                    if (MapMarkerMoved(g_map_marker_anchor, marker_now)) {
                        g_map_marker_anchor = marker_now;
                    } else if (g_map_marker_candidate.current_game == current_game &&
                               g_map_marker_candidate.marker == marker) {
                        g_map_marker_anchor = g_map_marker_candidate;
                    }
                }
                ClearMapRouteLocked();
            } else {
                // The stock map may publish its DOWN order before the finger
                // moves. Hide that order until UP confirms a content tap.
                if (current.behavior != g_map_route_snapshot.behavior ||
                    current.target != g_map_route_snapshot.target ||
                    current.movement != g_map_route_snapshot.movement) {
                    g_map_route_candidate = current;
                    WriteMapRouteLocked(g_map_route_snapshot);
                }
                if (MapMarkerMoved(g_map_marker_anchor, marker_now)) {
                    g_map_marker_candidate = marker_now;
                }
                touch_unresolved = true;
            }
        }
    }
    RestoreMapMarkerLocked(current_game, marker);

    if (g_map_time_running && !g_map_time_run_in_place && !touch_unresolved &&
        !accepted_order && (!party || !MapPartyIsTravelling(
            ReadMapRouteLocked(current_game, party).behavior[0]) ||
            MapPartyReachedDestinationLocked(party))) {
        // Stock can leave behavior 6 set after reaching the route target.
        g_map_time_running = false;
    }
    const uint32_t mode_address = current_game + kCurrentGameMapTimeModeOffset;
    const uint32_t stock_mode = g_host->MemoryRead32(mode_address);
    uint32_t desired_mode = stock_mode;
    if (touch_unresolved || !g_map_time_running) {
        desired_mode = 0u;
    } else if (stock_mode <= 2u) {
        // Stock travel mode stops at the destination; run-in-place is reserved
        // for an explicit Time tap while no route is active.
        desired_mode = g_map_time_run_in_place ? 2u : 1u;
    }
    if (desired_mode != stock_mode) {
        g_host->MemoryWrite32(mode_address, desired_mode);
    }

}

bool ReadInteractiveMapWaitLocked(uint32_t* current_game_out = nullptr) {
    if (current_game_out) *current_game_out = 0u;
    const uint32_t current_game = ResolveCurrentGameObjectLocked();
    if (!current_game) return false;
    const uint64_t remaining_address = static_cast<uint64_t>(current_game) +
                                       kCurrentGameWaitRemainingOffset;
    const uint64_t interactive_address = static_cast<uint64_t>(current_game) +
                                         kCurrentGameWaitInteractiveOffset;
    if (!GuestAddressRange(*g_host, remaining_address, sizeof(uint32_t)) ||
        !GuestAddressRange(*g_host, interactive_address, sizeof(uint32_t))) {
        return false;
    }
    const float remaining = ReadGuestFloatLocked(
        static_cast<uint32_t>(remaining_address));
    const bool active = std::isfinite(remaining) && remaining > 0.0f &&
        g_host->MemoryRead32(static_cast<uint32_t>(interactive_address)) != 0u;
    if (active && current_game_out) *current_game_out = current_game;
    return active;
}

void DrainMapWaitCancellationLocked() {
    uint32_t current_game = 0u;
    const bool active = ReadInteractiveMapWaitLocked(&current_game);
    if (g_map_cancel_wait_pending && active) {
        const uint32_t remaining_address = current_game +
                                           kCurrentGameWaitRemainingOffset;
        g_host->MemoryWrite32(remaining_address, FloatBits(0.0f));
        // rglKeys::set_controller_state(0) performs this exact write before
        // returning. It prevents the cancelled tap from leaking into the next
        // stock map-input pass.
        if (g_guest_keys && GuestAddressRange(
                *g_host, static_cast<uint64_t>(g_guest_keys) + kControllerState,
                sizeof(uint32_t))) {
            g_host->MemoryWrite32(g_guest_keys + kControllerState, 0u);
        }
    }
    g_map_cancel_wait_pending = false;
    g_map_interactive_wait_active = ReadInteractiveMapWaitLocked();
}

void SetMapFollowPlayerLocked(bool enabled) {
    const uint32_t current_game = ResolveCurrentGameObjectLocked();
    if (!current_game) return;
    const uint32_t player_party =
        g_host->MemoryRead32(current_game + kCurrentGamePlayerPartyOffset);
    g_host->MemoryWrite32(current_game + kCurrentGameFollowPartyOffset, player_party);
    g_host->MemoryWrite8(current_game + kCurrentGameFollowEnabledOffset,
                         enabled ? 1u : 0u);
}

float WrapMapAngle(float angle) {
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kTwoPi = 2.0f * kPi;
    if (!std::isfinite(angle)) return 0.0f;
    while (angle > kPi) angle -= kTwoPi;
    while (angle < -kPi) angle += kTwoPi;
    return angle;
}

void ApplyMapCameraGesturesLocked(float frame_seconds) {
    uint32_t map_window = 0;
    if (!ResolveMapWindowObjectLocked(&map_window)) {
        uint32_t active_window_id = kNoActiveWindowId;
        if (g_host && g_game_screen &&
            ReadStableActiveWindowId(*g_host, g_game_screen, &active_window_id) &&
            active_window_id != kMapWindowId) {
            ClearMapRouteLocked();
            g_map_time_running = false;
            g_map_time_run_in_place = false;
            g_map_marker_anchor = {};
        }
        g_map_pan_pending_x = 0.0f;
        g_map_pan_pending_y = 0.0f;
        g_map_rotation_pending = 0.0f;
        g_map_tilt_pending_pixels = 0.0f;
        g_map_pitch_target = std::numeric_limits<float>::quiet_NaN();
        g_map_compass_tap_pending = false;
        g_map_interactive_wait_active = false;
        g_map_cancel_wait_pending = false;
        g_map_recenter_phase = MapRecenterPhase::None;
        return;
    }

    DrainMapWaitCancellationLocked();

    const float distance = std::clamp(
        ReadGuestFloatLocked(map_window + kMapCameraDistanceOffset), 0.25f, 500.0f);
    const float stock_pitch = std::clamp(distance * 0.014f + 0.23f,
                                         0.1f, 1.57079632679f);
    if (g_map_pitch_initialized_window != map_window) {
        // update_map_camera formerly overwrote this field every frame. Seed it
        // with the exact stock formula before accepting manual 2D/3D tilt.
        g_host->MemoryWrite32(map_window + kMapCameraPitchOffset,
                              FloatBits(stock_pitch));
        g_map_pitch_initialized_window = map_window;
    }

    if (g_map_tilt_pending_pixels != 0.0f) {
        constexpr float kMinimumPitch = 0.30f;
        constexpr float kMaximumPitch = 1.35f;
        const float surface_height = std::max(g_map_touch_config.surface_height, 1.0f);
        // A drag across roughly three quarters of the screen spans the useful
        // pitch range. Small movements therefore retain fine control.
        const float radians_per_pixel =
            (kMaximumPitch - kMinimumPitch) / (surface_height * 0.75f);
        const float current_pitch = std::clamp(
            ReadGuestFloatLocked(map_window + kMapCameraPitchOffset),
            kMinimumPitch, kMaximumPitch);
        const float base_pitch = std::isfinite(g_map_pitch_target)
            ? g_map_pitch_target
            : current_pitch;
        g_map_pitch_target = std::clamp(
            base_pitch + g_map_tilt_pending_pixels * radians_per_pixel,
            kMinimumPitch, kMaximumPitch);
    }
    g_map_tilt_pending_pixels = 0.0f;

    float angle = WrapMapAngle(ReadGuestFloatLocked(
        map_window + kMapCameraAngleOffset));
    if (g_map_compass_tap_pending) {
        const float current_pitch = std::clamp(
            ReadGuestFloatLocked(map_window + kMapCameraPitchOffset),
            0.1f, 1.57079632679f);
        const bool ordinary_orientation = std::fabs(angle) < 0.01f &&
                                          std::fabs(current_pitch - stock_pitch) < 0.01f;
        if (ordinary_orientation) {
            // The first tap is orientation-only when needed. A later tap, made
            // after north and the stock pitch are restored, re-enables the
            // game's damped player-party follow without teleporting the view.
            g_map_recenter_phase = MapRecenterPhase::None;
            g_map_pitch_target = std::numeric_limits<float>::quiet_NaN();
            SetMapFollowPlayerLocked(true);
        } else {
            SetMapFollowPlayerLocked(false);
            g_map_recenter_phase = MapRecenterPhase::RestoreOrientation;
            g_map_pitch_target = stock_pitch;
        }
        g_map_compass_tap_pending = false;
    }
    const bool manual_pan = g_map_pan_pending_x != 0.0f ||
                            g_map_pan_pending_y != 0.0f;
    const bool manual_rotation = g_map_rotation_pending != 0.0f;
    if (manual_pan) {
        const float surface_height = std::max(g_map_touch_config.surface_height, 1.0f);
        const warband::guest_settings::Values* settings =
            &g_guest_settings_host_values;
        if (g_guest_settings_pending_valid) {
            settings = &g_guest_settings_pending_snapshot;
        } else if (g_guest_settings_custom_session &&
                   g_guest_settings_last_snapshot_valid) {
            settings = &g_guest_settings_last_snapshot;
        }
        const float pan_sensitivity = warband::guest_settings::BoundHostFloat(
            GST_FLOAT_MAP_PAN_SENSITIVITY,
            settings->floats[GST_FLOAT_MAP_PAN_SENSITIVITY]);
        const float units_per_pixel = std::clamp(
            2.0f * (distance + 10.0f) / surface_height * pan_sensitivity,
            0.0007f, 6.0f);
        const float cosine = std::cos(angle);
        const float sine = std::sin(angle);
        float center_x = ReadGuestFloatLocked(map_window + kMapCenterXOffset);
        float center_y = ReadGuestFloatLocked(map_window + kMapCenterYOffset);
        center_x += (-g_map_pan_pending_x * cosine +
                     g_map_pan_pending_y * sine) * units_per_pixel;
        center_y += (g_map_pan_pending_x * sine +
                     g_map_pan_pending_y * cosine) * units_per_pixel;
        if (std::isfinite(center_x) && std::isfinite(center_y)) {
            g_host->MemoryWrite32(map_window + kMapCenterXOffset, FloatBits(center_x));
            g_host->MemoryWrite32(map_window + kMapCenterYOffset, FloatBits(center_y));
        }
    }
    g_map_pan_pending_x = 0.0f;
    g_map_pan_pending_y = 0.0f;

    if (manual_rotation) {
        angle = WrapMapAngle(angle + g_map_rotation_pending);
        g_host->MemoryWrite32(map_window + kMapCameraAngleOffset, FloatBits(angle));
    }
    g_map_rotation_pending = 0.0f;
    if (manual_pan || manual_rotation) SetMapFollowPlayerLocked(false);

    const float safe_frame_seconds = std::clamp(
        std::isfinite(frame_seconds) ? frame_seconds : (1.0f / 60.0f),
        1.0f / 240.0f, 0.1f);
    if (std::isfinite(g_map_pitch_target)) {
        const float current_pitch = std::clamp(
            ReadGuestFloatLocked(map_window + kMapCameraPitchOffset),
            0.1f, 1.57079632679f);
        const float blend = 1.0f - std::exp(-10.0f * safe_frame_seconds);
        const float next_pitch = current_pitch +
                                 (g_map_pitch_target - current_pitch) * blend;
        g_host->MemoryWrite32(map_window + kMapCameraPitchOffset,
                              FloatBits(next_pitch));
        if (std::fabs(next_pitch - g_map_pitch_target) < 0.002f) {
            g_host->MemoryWrite32(map_window + kMapCameraPitchOffset,
                                  FloatBits(g_map_pitch_target));
            g_map_pitch_target = std::numeric_limits<float>::quiet_NaN();
        }
    }

    if (g_map_recenter_phase == MapRecenterPhase::RestoreOrientation) {
        angle = WrapMapAngle(ReadGuestFloatLocked(
            map_window + kMapCameraAngleOffset));
        const float blend = 1.0f - std::exp(-8.0f * safe_frame_seconds);
        angle = WrapMapAngle(angle + WrapMapAngle(-angle) * blend);
        if (std::fabs(angle) < 0.01f) {
            angle = 0.0f;
        }
        g_host->MemoryWrite32(map_window + kMapCameraAngleOffset, FloatBits(angle));
        if (angle == 0.0f && !std::isfinite(g_map_pitch_target)) {
            // Orientation is now ordinary; camera position stays detached.
            // Only another compass tap may re-enable player follow.
            g_map_recenter_phase = MapRecenterPhase::None;
        }
    }
}

void ApplyMapZoomLocked() {
    if (g_map_zoom_pending == 0) return;
    if (!g_host || !g_game_screen || !g_guest_keys) {
        g_map_zoom_pending = 0;
        g_map_zoom_remainder = 0.0f;
        return;
    }
    uint32_t active_window_id = kNoActiveWindowId;
    if (!ReadStableActiveWindowId(*g_host, g_game_screen, &active_window_id) ||
        active_window_id != kMapWindowId) {
        // A pinch can finish while a map popup is opening.  Never replay its
        // wheel burst on the next unrelated screen.
        g_map_zoom_pending = 0;
        g_map_zoom_remainder = 0.0f;
        return;
    }
    const uint64_t wheel_address64 = static_cast<uint64_t>(g_guest_keys) + kMapWheelDelta;
    const uint64_t controller_address64 = static_cast<uint64_t>(g_guest_keys) +
                                           kControllerState;
    if (!GuestAddressRange(*g_host, wheel_address64, sizeof(uint32_t)) ||
        !GuestAddressRange(*g_host, controller_address64, sizeof(uint32_t))) {
        g_map_zoom_pending = 0;
        g_map_zoom_remainder = 0.0f;
        return;
    }
    const uint32_t wheel_address = static_cast<uint32_t>(wheel_address64);
    const uint32_t controller_address = static_cast<uint32_t>(controller_address64);
    const int32_t current = static_cast<int32_t>(g_host->MemoryRead32(wheel_address));
    const int64_t sum = static_cast<int64_t>(current) + g_map_zoom_pending;
    const int32_t next = static_cast<int32_t>(std::clamp<int64_t>(
        sum, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max()));
    g_host->MemoryWrite32(wheel_address, static_cast<uint32_t>(next));
    // Map_window::get_map_camera_input consumes the wheel field only for the
    // touch|joystick controller state.  A pinch is cancelled before the guest
    // sees POINTER_DOWN, so restore that state explicitly for this frame.
    g_host->MemoryWrite32(controller_address, 3u);
    if (g_host->MemoryRead32(wheel_address) != static_cast<uint32_t>(next) ||
        g_host->MemoryRead32(controller_address) != 3u) {
        LOGE("map pinch write verification failed wheel=0x%08X", wheel_address);
    }
    static uint32_t reports = 0;
    if (reports++ < 16u) {
        LOGI("map pinch zoom wheel=%d current=%d", g_map_zoom_pending, current);
    }
    g_map_zoom_pending = 0;
}

uint32_t ResolveFocusedTacticalChatLocked(uint32_t* tactical_out) {
    if (!g_host || !g_window_manager || !tactical_out ||
        !GuestAddressRange(*g_host, static_cast<uint64_t>(g_window_manager) + 0xCu, 4u)) return 0;
    uint32_t active_window = kNoActiveWindowId;
    uint32_t tactical = 0;
    const uint32_t current_game = ResolveCurrentGameObjectLocked();
    if (!current_game ||
        !GuestAddressRange(*g_host, static_cast<uint64_t>(current_game) + 0x19368u, 4u) ||
        !ReadStableActiveWindowId(*g_host, g_game_screen, &active_window) ||
        active_window != kTacticalWindowId ||
        !ResolveActiveWindowObjectLocked(kTacticalWindowId, &tactical) ||
        !GuestAddressRange(*g_host, static_cast<uint64_t>(tactical) + 0x1CCu, 4u)) {
        return 0;
    }
    const uint32_t mode = g_host->MemoryRead32(current_game + 0x19368u);
    const uint32_t widget = g_host->MemoryRead32(tactical + 0x1CCu);
    if ((mode != 1u && mode != 2u) || !widget ||
        !GuestAddressRange(*g_host, widget, 4u) ||
        g_host->MemoryRead32(g_window_manager + 0xCu) != widget) return 0;
    *tactical_out = tactical;
    return widget;
}

bool DispatchTacticalWidgetEventLocked(uint32_t widget, uint32_t event,
                                       uint32_t virtual_offset);

void RetireHiddenTacticalChatFocusLocked() {
    if (!g_host || !g_window_manager ||
        !GuestAddressRange(*g_host, static_cast<uint64_t>(g_window_manager) + 0xCu, 4u)) return;
    uint32_t active_window = kNoActiveWindowId;
    uint32_t tactical = 0;
    const uint32_t current_game = ResolveCurrentGameObjectLocked();
    if (!current_game ||
        !GuestAddressRange(*g_host, static_cast<uint64_t>(current_game) + 0x19368u, 4u) ||
        g_host->MemoryRead32(current_game + 0x19368u) != 0u ||
        !ReadStableActiveWindowId(*g_host, g_game_screen, &active_window) ||
        active_window != kTacticalWindowId ||
        !ResolveActiveWindowObjectLocked(kTacticalWindowId, &tactical) ||
        !GuestAddressRange(*g_host, static_cast<uint64_t>(tactical) + 0x1CCu, 4u)) return;
    const uint32_t widget = g_host->MemoryRead32(tactical + 0x1CCu);
    if (!widget || g_host->MemoryRead32(g_window_manager + 0xCu) != widget ||
        !GuestAddressRange(*g_host, widget, 0x2E2u) ||
        (g_host->MemoryRead32(widget + kWidgetFlagsOffset) & 1u) == 0u) return;
    const uint32_t vtable = g_host->FindSymbol("_ZTV18rglText_box_simple");
    const uint32_t focus_released = g_host->FindSymbol("_ZN11rglText_box14focus_releasedEv");
    if (!vtable || !focus_released ||
        !GuestAddressRange(*g_host, static_cast<uint64_t>(vtable) + 8u + 0xB4u, 4u) ||
        g_host->MemoryRead32(widget) != vtable + 8u ||
        g_host->MemoryRead32(vtable + 8u + 0xB4u) != focus_released) return;
    // Stock chat close only hides the widget. Retire its native focus after
    // that close, so IME/session teardown cannot cancel the preceding submit.
    DispatchTacticalWidgetEventLocked(widget, 0u, 0xB4u);
}

void DrainTextInputLocked() {
    // A full controls reset or FIFO resync may discard the tagged DOWN.
    // Retire its ownership once neither a transition nor held Enter survives.
    if (g_text_submit_widget != 0 && g_discrete_buttons.PendingSize() == 0 &&
        ((g_discrete_buttons.CombinedHeld() | g_discrete_buttons.DeliveredHeld()) &
         warband::input::kEnterConfirm) == 0) {
        g_text_submit_token = 0;
        g_text_submit_tactical = 0;
        g_text_submit_widget = 0;
    }
    // An accepted chat submit is still owned by its original editor until the
    // frame gate delivers DOWN; a replacement editor must wait for that check.
    if (g_text_submit_token != 0) return;
    if (!g_host || g_text_input_queue.Empty()) return;
    if (!g_application_object ||
        g_pending_character_slot != g_application_object + kPendingCharacterObjectOffset ||
        !GuestAddressRange(*g_host, g_pending_character_slot, sizeof(uint32_t)) ||
        (g_pending_character_slot & (alignof(uint32_t) - 1u)) != 0) {
        LOGE("text input drain disabled by invalid guest slot address");
        InvalidateTextInputForBridgeLifecycleLocked(true);
        return;
    }

    uint32_t tactical = 0;
    const uint32_t chat = g_text_input_queue.SubmitPending()
        ? ResolveFocusedTacticalChatLocked(&tactical) : 0u;
    if (chat) {
        // All preceding text must already have passed WM tick. Starting from
        // an empty discrete FIFO keeps this DOWN first even at very low FPS.
        const uint32_t enter = warband::input::kEnterConfirm;
        if (g_host->MemoryRead32(g_pending_character_slot) !=
                warband::text_input::kPendingSlotEmpty ||
            g_discrete_buttons.PendingSize() != 0 ||
            g_discrete_buttons.ResyncRequired() ||
            ((g_discrete_buttons.CombinedHeld() | g_discrete_buttons.DeliveredHeld()) &
             enter) != 0) return;
        g_text_submit_token = g_text_input_queue.ActiveToken();
        g_text_submit_tactical = tactical;
        g_text_submit_widget = chat;
        g_text_input_queue.TakeSubmit();
        g_discrete_buttons.SetSynthetic(enter, true);
        return;
    }

    const auto result = g_text_input_queue.DrainOne(
        [] { return g_host->MemoryRead32(g_pending_character_slot); },
        [](uint32_t value) { g_host->MemoryWrite32(g_pending_character_slot, value); });
    if (result == warband::text_input::DrainResult::WriteNotObserved) {
        static uint32_t failed_write_reports = 0;
        if (failed_write_reports++ < 8u) {
            LOGE("text input pending-slot write verification failed");
        }
    }
}

void RemoveGuestFrameIntervalLimitLocked() {
    if (!g_host) return;
    ElfLoader* game = g_host->GetLibrary("libMBExpMobile.so");
    if (!game) return;

    // initialize_stuff derives Application's minimum frame interval at
    // libMBExpMobile.so+0x2786DD0 from rgl_config.max_framerate (30 by default).
    // The bridge's eglSwapBuffers limiter owns the selected FPS, so the guest
    // loop must not impose its separate startup interval first.
    constexpr uint32_t kInitializeStuffOffset = 0x002561D4u;
    constexpr uint32_t kFrameIntervalOffset = 0x02786DD0u;
    const uint32_t base = game->GetBaseAddress();
    const uint32_t initialize = g_host->FindSymbol("_Z16initialize_stuffPv");
    const uint64_t frame_interval64 = static_cast<uint64_t>(base) +
                                      kFrameIntervalOffset;
    if (initialize != base + kInitializeStuffOffset ||
        !GuestAddressRange(*g_host, frame_interval64, sizeof(uint32_t)) ||
        (frame_interval64 & (alignof(uint32_t) - 1u)) != 0u) {
        return;
    }

    const uint32_t frame_interval = static_cast<uint32_t>(frame_interval64);
    const uint32_t current_bits = g_host->MemoryRead32(frame_interval);
    float current_seconds = 0.0f;
    std::memcpy(&current_seconds, &current_bits, sizeof(current_seconds));
    if (!(current_seconds > 0.0f)) return;

    g_host->MemoryWrite32(frame_interval, FloatBits(0.0f));
    if (g_host->MemoryRead32(frame_interval) != FloatBits(0.0f)) return;

    static uint32_t reported_base = 0;
    if (reported_base != base) {
        reported_base = base;
        LOGI("disabled guest frame interval %.6f s at 0x%08X",
             current_seconds, frame_interval);
    }
}

bool InstallOfflineLanAuthHook(DynarmicHost& host) {
    constexpr const char* kSymbol = "_ZN7Request8SendHTTPERK9rglStringPKhiP11HTTPRequestb";
    constexpr const char* kThunk = "__warband_bridge_lan_http";
    ElfLoader* game = host.GetLibrary("libMBExpMobile.so");
    const uint32_t entry = host.FindSymbol(kSymbol);
    if (!game || entry != game->GetBaseAddress() + 0x1D2718u ||
        !GuestAddressRange(host, entry, 8u) ||
        host.MemoryRead32(entry) != 0xE92D4BF0u ||
        host.MemoryRead32(entry + 4u) != 0xE24DD098u) return false;
    ThunkManager& thunks = host.GetThunkManager();
    if (thunks.IsRegistered(kThunk)) return false;
    const uint32_t original = host.GuestAlloc(16u, 4u);
    if (!original || !GuestAddressRange(host, original, 16u)) return false;
    host.MemoryWrite32(original, 0xE92D4BF0u);
    host.MemoryWrite32(original + 4u, 0xE24DD098u);
    host.MemoryWrite32(original + 8u, 0xE51FF004u);  // ldr pc, [pc, #-4]
    host.MemoryWrite32(original + 12u, entry + 8u);
    thunks.RegisterThunk(kThunk, [original](Dynarmic::A32::Jit& jit,
                                          DynarmicHost& callback_host) {
        // Only the LAN-only loopback request for a private game server is local.
        // Other HTTP operations retain the original libcurl implementation.
        if (jit.Regs()[1] == 0u && jit.Regs()[2] == 0u &&
            TryOfflineLanAuth(callback_host, jit.Regs()[0], jit.Regs()[3])) {
            jit.Regs()[0] = 0u;
            return;
        }
        if (!GuestAddressRange(callback_host, jit.Regs()[13], 4u)) {
            jit.Regs()[0] = 1u;
            return;
        }
        jit.Regs()[0] = callback_host.CallGuestNested(original, {
            jit.Regs()[0], jit.Regs()[1], jit.Regs()[2], jit.Regs()[3],
            callback_host.MemoryRead32(jit.Regs()[13]),
        });
    });
    const uint32_t trampoline = thunks.GetOrCreateThunkAddress(kThunk);
    if (!thunks.IsValid() || !trampoline ||
        !GuestAddressRange(host, trampoline, 8u) ||
        (host.MemoryRead32(trampoline) & 0xFF000000u) != 0xEF000000u ||
        host.MemoryRead32(trampoline + 4u) != kArmBxLr) return false;
    host.MemoryWrite32(entry, host.MemoryRead32(trampoline));
    host.MemoryWrite32(entry + 4u, kArmBxLr);
    host.InvalidateGuestCode(entry, 8u);
    return true;
}

bool ApplyGuestMasterServerConfig(DynarmicHost& host, std::string_view mode,
                                  std::string_view requested_host,
                                  std::string_view previous_mode,
                                  std::string_view previous_host);
bool SetServerBrowserDefault(DynarmicHost& host, std::string_view mode);
uint32_t TacticalModalStateLocked();
void DrainTacticalUiTouchLocked();

void DrainMasterServerSelectionLocked() {
    if (!g_master_server_change_pending || !g_host) return;
    g_master_server_change_pending = false;
    if (!ApplyGuestMasterServerConfig(*g_host, g_pending_master_server_mode,
                                      g_pending_master_server_host,
                                      g_master_server_mode, g_master_server_host)) {
        LOGE("live master-server change rejected by guest string guard");
        return;
    }
    if (!SetServerBrowserDefault(*g_host, g_pending_master_server_mode)) {
        ApplyGuestMasterServerConfig(*g_host, g_master_server_mode,
                                     g_master_server_host,
                                     g_pending_master_server_mode,
                                     g_pending_master_server_host);
        LOGE("live master-server change rejected by browser hook guard");
        return;
    }
    g_master_server_mode = g_pending_master_server_mode;
    g_master_server_host = g_pending_master_server_host;
    LOGI("live master-server changed mode=%s host=%s",
         g_master_server_mode.c_str(), g_master_server_host.c_str());
}

void ReleaseSyntheticChatKeyLocked() {
    if (g_synthetic_chat_key_address && g_host &&
        GuestAddressRange(*g_host, g_synthetic_chat_key_address, sizeof(uint32_t)) &&
        g_host->MemoryRead32(g_synthetic_chat_key_address) == 1u) {
        g_host->MemoryWrite32(g_synthetic_chat_key_address, 0u);
    }
    g_synthetic_chat_key_address = 0;
}

void DrainPendingChatKeyLocked() {
    ReleaseSyntheticChatKeyLocked();
    if (!g_pending_open_chat) return;
    g_pending_open_chat = false;
    if (!g_host || !g_game_screen || !GuestKeys() ||
        !CurrentMissionIsMultiplayerLocked()) return;

    uint32_t window = kNoActiveWindowId;
    if (!ReadStableActiveWindowId(*g_host, g_game_screen, &window) ||
        window != kTacticalWindowId) return;
    const uint32_t current_game = ResolveCurrentGameObjectLocked();
    if (!current_game || !GuestAddressRange(
            *g_host, static_cast<uint64_t>(current_game) + 0x19368u, 4u) ||
        g_host->MemoryRead32(current_game + 0x19368u) != 0u) return;

    const uint32_t game_keys = g_host->FindSymbol("game_keys");
    if (!game_keys || !GuestAddressRange(
            *g_host, static_cast<uint64_t>(game_keys) + 0x148u, 8u)) return;
    uint32_t key = g_host->MemoryRead32(game_keys + 0x148u);
    if (key == 0u || key >= 256u) key = g_host->MemoryRead32(game_keys + 0x14Cu);
    if (key == 0u || key >= 256u) return;
    const uint32_t current = GuestKeys() + 4u * key;
    if (!GuestAddressRange(*g_host, static_cast<uint64_t>(current), 4u) ||
        !GuestAddressRange(*g_host, static_cast<uint64_t>(current) + 0x400u, 4u) ||
        g_host->MemoryRead32(current) != 0u ||
        g_host->MemoryRead32(current + 0x400u) != 0u) return;
    g_host->MemoryWrite32(current, 1u);
    g_synthetic_chat_key_address = current;
    LOGI("multiplayer all-chat key edge: binding=%u", key);
}

uint32_t DrainFrameInputLocked(float frame_seconds) {
    // This hook runs once after rglWindow_manager::tick on the guest frame
    // thread.  Only it may touch Application::pending_character, and at most
    // one queued command is offered to the guest per invocation.
    // The wrapper has just completed rglKeys::tick; deferred edges belong to
    // its next batch, even when a complete touch tap arrived between frames.
    g_discrete_buttons.FinishFrame();
    g_guest_frame_heartbeat_ms.store(MonotonicMilliseconds(),
                                     std::memory_order_release);
    DrainMasterServerSelectionLocked();
    DrainPendingChatKeyLocked();
    RetireTouchWidgetHighlightIfWindowChangedLocked();
    RetireHiddenTacticalChatFocusLocked();
    DrainTextInputLocked();
    ExpirePendingConversationTapLocked();
    RemoveGuestFrameIntervalLimitLocked();
    MigrateStockDirectionalControlDefaultsLocked();
    SyncGuestSettingsLocked();
    ApplyRendererGuestSettingsLocked();
    ApplyMergedControllerStateLocked();
    DrainTacticalUiTouchLocked();
    const bool tactical_controls_blocked = g_gameplay_active &&
        (g_overlay_edit_mode || TacticalModalStateLocked() == 1u);
    if (tactical_controls_blocked) {
        const uint32_t keys = GuestKeys();
        if (keys) {
            WriteAxis(keys, kAxisLeftX, 0.0f);
            WriteAxis(keys, kAxisLeftY, 0.0f);
            WriteAxis(keys, kAxisRightX, 0.0f);
            WriteAxis(keys, kAxisRightY, 0.0f);
            g_host->MemoryWrite32(keys + kTacticalCameraDeltaX, FloatBits(0.0f));
            g_host->MemoryWrite32(keys + kTacticalCameraDeltaY, FloatBits(0.0f));
        }
        g_pending_look_x = g_pending_look_y = 0.0f;
        g_pending_gyro_look_x = g_pending_gyro_look_y = 0.0f;
        g_relative_look_remainder_x = g_relative_look_remainder_y = 0.0f;
    }
    ApplyMapZoomLocked();
    ApplyMapCameraGesturesLocked(frame_seconds);
    MaintainInventoryScrollPositionLocked();
    MaintainConversationScrollPositionLocked();
    ApplyInventoryDragLocked();
    ApplyOrdersTapLocked();
    ApplyUiButtonTapLocked();
    ApplyMenuScrollLocked();
    ApplyInventoryEdgeScrollLocked(frame_seconds);
    // While the touch overlay owns tactical input, pin the guest's
    // input-device mode to 3 (touch | joystick).  Tactical_window::check
    // (guest 0x2C49D8) gates the whole Tactical_window::handle_user_input
    // call - movement AND camera look - behind dword_4506A8 == 3; window
    // switches leave it lower and the camera appears dead until a pause
    // round-trip happens to restore it.  A physical gamepad reaches mode 3
    // through its own touch+joystick events, so this matches the native
    // Shield controller state.
    if (g_host && g_gameplay_active) {
        if (ElfLoader* game = g_host->GetLibrary("libMBExpMobile.so")) {
            const uint32_t input_mode = game->GetBaseAddress() + 0x4506A8u;
            // While the overlay layout editor is active the guest must stop
            // consuming tactical input (camera/movement stay frozen under the
            // dragged buttons); release the pinned touch|joystick mode then.
            const uint32_t desired_mode = g_overlay_edit_mode ? 0u : 3u;
            if (ValidGuestPointerLocked(input_mode) &&
                g_host->MemoryRead32(input_mode) != desired_mode) {
                g_host->MemoryWrite32(input_mode, desired_mode);
            }
        }
    }
    const bool look_trace = [] {
        char value[2] = {0, 0};
        __system_property_get("debug.warband.look_trace", value);
        return value[0] == '1' || value[0] == '2';
    }();
    const uint32_t keys = GuestKeys();
    float gyro_look_x = 0.0f;
    float gyro_look_y = 0.0f;
    if (g_host && keys && (g_pending_gyro_look_x != 0.0f || g_pending_gyro_look_y != 0.0f)) {
        // Cancel the game's vertical look multiplier (rgl_config+0x94,
        // control_mouse_movement_y_scale, default 1.5) for gyro only. Keep
        // gyro separate so the mouse-look choice only changes touch swipes.
        float y_scale = 1.0f;
        if (ElfLoader* game = g_host->GetLibrary("libMBExpMobile.so")) {
            const uint32_t scale_addr = game->GetBaseAddress() + 0x451198u;
            if (ValidGuestPointerLocked(scale_addr)) {
                const float value = ReadGuestFloatLocked(scale_addr);
                if (value > 0.05f && value < 20.0f) y_scale = value;
            }
        }
        gyro_look_x = g_pending_gyro_look_x;
        gyro_look_y = std::clamp(g_pending_gyro_look_y / y_scale,
                                 -kMaxInjectedLookPerFrame,
                                 kMaxInjectedLookPerFrame);
        if (look_trace) {
            static uint32_t gyro_reports = 0;
            if (gyro_reports++ < 200u) {
                LOGI("gyro_look units=(%.2f,%.2f) y_scale=%.2f",
                     gyro_look_x, g_pending_gyro_look_y, y_scale);
            }
        }
    }
    g_pending_gyro_look_x = 0.0f;
    g_pending_gyro_look_y = 0.0f;
    const warband::guest_settings::Values* camera_settings =
        &g_guest_settings_host_values;
    if (g_guest_settings_pending_valid) {
        camera_settings = &g_guest_settings_pending_snapshot;
    } else if (g_guest_settings_custom_session &&
               g_guest_settings_last_snapshot_valid) {
        camera_settings = &g_guest_settings_last_snapshot;
    }
    const bool mouse_look =
        (camera_settings->bool_mask & GST_BOOL_CAMERA_MOUSE) != 0u;
    if (!g_gameplay_active || !mouse_look) {
        g_relative_look_remainder_x = 0.0f;
        g_relative_look_remainder_y = 0.0f;
    }
    const bool has_pending_look =
        g_pending_look_x != 0.0f || g_pending_look_y != 0.0f;
    const bool has_gyro_look = gyro_look_x != 0.0f || gyro_look_y != 0.0f;
    const bool has_relative_remainder = mouse_look && g_gameplay_active &&
        (g_relative_look_remainder_x != 0.0f ||
         g_relative_look_remainder_y != 0.0f);
    if (g_host && keys &&
        (has_pending_look || has_gyro_look || has_relative_remainder)) {
        if (g_gameplay_active) {
            if (mouse_look) {
                // Tactical_window consumes these relative-camera deltas in
                // the same frame. Scale to the Right stick path's 60 Hz input
                // units, and keep fractions because the guest truncates these
                // fields to integer mouse counts.
                if (has_pending_look || has_relative_remainder) {
                    const auto relative_x =
                        warband::camera_look::QuantizeRelativeDelta(
                            warband::camera_look::ScaledRelativeMouseDelta(
                                g_pending_look_x),
                            g_relative_look_remainder_x);
                    const auto relative_y =
                        warband::camera_look::QuantizeRelativeDelta(
                            warband::camera_look::ScaledRelativeMouseDelta(
                                g_pending_look_y),
                            g_relative_look_remainder_y);
                    g_relative_look_remainder_x = relative_x.remainder;
                    g_relative_look_remainder_y = relative_y.remainder;
                    if (relative_x.emitted != 0.0f || relative_y.emitted != 0.0f) {
                        float current_x = GuestSettingsFloatFromBits(
                            g_host->MemoryRead32(keys + kTacticalCameraDeltaX));
                        float current_y = GuestSettingsFloatFromBits(
                            g_host->MemoryRead32(keys + kTacticalCameraDeltaY));
                        if (!std::isfinite(current_x)) current_x = 0.0f;
                        if (!std::isfinite(current_y)) current_y = 0.0f;
                        g_host->MemoryWrite32(keys + kTacticalCameraDeltaX,
                            FloatBits(current_x + relative_x.emitted));
                        g_host->MemoryWrite32(keys + kTacticalCameraDeltaY,
                            FloatBits(current_y + relative_y.emitted));
                    }
                    if (look_trace) {
                        static uint32_t mouse_trace_reports = 0;
                        if (mouse_trace_reports++ < 400u) {
                            LOGI("look_mouse pending=(%.2f,%.2f) emitted=(%.0f,%.0f) "
                                 "remainder=(%.2f,%.2f)",
                                 g_pending_look_x, g_pending_look_y,
                                 relative_x.emitted, relative_y.emitted,
                                 relative_x.remainder, relative_y.remainder);
                        }
                    }
                }
                if (has_gyro_look) {
                    const float gyro_stick_x =
                        warband::camera_look::NormalizedStickAxis(
                            gyro_look_x, frame_seconds);
                    const float gyro_stick_y =
                        warband::camera_look::NormalizedStickAxis(
                            -gyro_look_y, frame_seconds);
                    WriteAxis(keys, kAxisRightX,
                              g_physical_right_x + gyro_stick_x);
                    WriteAxis(keys, kAxisRightY,
                              g_physical_right_y + gyro_stick_y);
                }
            } else if (has_pending_look || has_gyro_look) {
                // The stick route is intentionally available for players who
                // prefer analog acceleration. Normalize its integration by
                // the frame delta so one swipe turns the same amount at each
                // frame rate.
                const float combined_look_x = std::clamp(
                    g_pending_look_x + gyro_look_x,
                    -kMaxInjectedLookPerFrame, kMaxInjectedLookPerFrame);
                const float combined_look_y = std::clamp(
                    g_pending_look_y + gyro_look_y,
                    -kMaxInjectedLookPerFrame, kMaxInjectedLookPerFrame);
                const float stick_x =
                    warband::camera_look::NormalizedStickAxis(
                        combined_look_x, frame_seconds);
                const float stick_y =
                    warband::camera_look::NormalizedStickAxis(
                        -combined_look_y, frame_seconds);
                WriteAxis(keys, kAxisRightX, stick_x);
                WriteAxis(keys, kAxisRightY, stick_y);
                if (look_trace) {
                    static uint32_t stick_trace_reports = 0;
                    if (stick_trace_reports++ < 400u) {
                        LOGI("look_stick pending=(%.2f,%.2f) dt=%.5f axes=(%.2f,%.2f)",
                             combined_look_x, combined_look_y,
                             frame_seconds, stick_x, stick_y);
                    }
                }
            }
        } else {
            const int32_t current_x = static_cast<int32_t>(g_host->MemoryRead32(keys + kMouseDeltaX));
            const int32_t current_y = static_cast<int32_t>(g_host->MemoryRead32(keys + kMouseDeltaY));
            const int32_t next_x = AddClampedLook(current_x, g_pending_look_x);
            const int32_t next_y = AddClampedLook(current_y, g_pending_look_y);
            g_host->MemoryWrite32(keys + kMouseDeltaX, static_cast<uint32_t>(next_x));
            g_host->MemoryWrite32(keys + kMouseDeltaY, static_cast<uint32_t>(next_y));
            if (look_trace) {
                static uint32_t look_trace_reports = 0;
                if (look_trace_reports++ < 400u) {
                    LOGI("look_trace pending=(%.2f,%.2f) keys_before=(%d,%d) keys_after=(%d,%d)",
                         g_pending_look_x, g_pending_look_y, current_x, current_y, next_x, next_y);
                }
            }
        }
    }
    g_pending_look_x = 0.0f;
    g_pending_look_y = 0.0f;

    const bool sheath = g_pending_sheath;
    g_pending_sheath = false;
    return sheath ? ResolvePlayerAgentLocked() : 0;
}

bool ApplyPendingOrdersRequestLocked() {
    if (!g_pending_open_orders) return false;
    g_pending_open_orders = false;
    if (!g_host || !g_gameplay_active || !g_game_screen) return false;

    // Do not let a delayed overlay release open a panel after Escape, Options,
    // or the end of the mission.  The tactical window pointer is owned by the
    // stable Game_screen slot table and is consumed immediately below on this
    // same guest frame.
    uint32_t active_window = warband::input::kUnknownMenuWindowId;
    const bool stable_window =
        ReadStableActiveWindowId(*g_host, g_game_screen, &active_window);
    if (!stable_window || active_window != kTacticalWindowId) {
        return false;
    }
    const uint64_t slot64 = static_cast<uint64_t>(g_game_screen) +
                            4u * (1u + kTacticalWindowId);
    if (slot64 > std::numeric_limits<uint32_t>::max() ||
        !GuestAddressRange(*g_host, slot64, sizeof(uint32_t))) {
        return false;
    }
    const uint32_t tactical = g_host->MemoryRead32(static_cast<uint32_t>(slot64));
    if (!ValidGuestPointerLocked(tactical) ||
        !GuestAddressRange(*g_host, tactical,
                           kTacticalOrdersCursorOffset + sizeof(uint32_t))) {
        return false;
    }

    const uint32_t root_widget =
        g_host->MemoryRead32(tactical + kTacticalOrdersRootWidgetOffset);
    if (!ValidGuestPointerLocked(root_widget) ||
        !GuestAddressRange(*g_host, root_widget + kWidgetFlagsOffset,
                           sizeof(uint32_t))) {
        return false;
    }
    const uint32_t root_flags =
        g_host->MemoryRead32(root_widget + kWidgetFlagsOffset);
    if ((root_flags & 1u) == 0u) {
        // The native transition is an open request, not a toggle.  If the
        // panel is already visible, leave its current submenu untouched.
        return true;
    }

    g_host->MemoryWrite32(tactical + kTacticalOrdersCategoryOffset, 0xFFFFFFFFu);
    g_host->MemoryWrite32(tactical + kTacticalOrdersGroupOffset, 0xFFFFFFFFu);
    g_host->MemoryWrite8(tactical + kTacticalOrdersDirtyOffset, 1u);
    g_host->MemoryWrite32(tactical + kTacticalOrdersCursorOffset, 0u);
    return true;
}

uint32_t PopPendingGuestEventLocked() {
    if (g_menu_window_id != kNoActiveWindowId && !MenuWindowBindingMatchesLocked()) {
        CancelMenuInputLocked();
    }
    g_menu_pulses.Advance(MonotonicMilliseconds(), g_discrete_buttons);
    g_orders_pulses.Advance(MonotonicMilliseconds(), g_discrete_buttons);
    ReleaseIdleMenuWindowBindingLocked();
    const uint32_t pulse_drops = g_menu_pulses.DroppedCount();
    if (pulse_drops != g_reported_menu_pulse_drops) {
        LOGE("menu pulse queue saturated; dropped=%u", pulse_drops);
        g_reported_menu_pulse_drops = pulse_drops;
    }
    const uint32_t overflow_count = g_discrete_buttons.OverflowCount();
    if (overflow_count != g_reported_discrete_overflows) {
        LOGE("discrete input queue overflow; fail-safe state resync count=%u",
             overflow_count);
        g_reported_discrete_overflows = overflow_count;
    }
    if (g_pending_welcome_enter_character) {
        // A real Android Enter also places CR in Application::pending_character.
        // Reproduce that one-shot side effect on the guest frame thread so the
        // welcome presentation sees the same submit signal as a hardware key.
        if (g_host && g_pending_character_slot &&
            GuestAddressRange(*g_host, g_pending_character_slot, sizeof(uint32_t)) &&
            g_host->MemoryRead32(g_pending_character_slot) ==
                warband::text_input::kPendingSlotEmpty) {
            g_host->MemoryWrite32(g_pending_character_slot, 13u);
            if (g_host->MemoryRead32(g_pending_character_slot) != 13u) {
                LOGE("MOTD Enter pending-character write failed");
            } else {
                g_pending_welcome_enter_character = false;
                LOGI("MOTD Enter queued pending-character CR");
            }
        }
    }
    const uint32_t encoded_event = g_discrete_buttons.PopFrameEncodedEvent();
    if (encoded_event == 33u && g_text_submit_widget != 0) {
        uint32_t tactical = 0;
        const uint32_t chat = ResolveFocusedTacticalChatLocked(&tactical);
        const bool owned = g_text_submit_token != 0 &&
                           g_text_input_queue.ActiveToken() == g_text_submit_token &&
                           tactical == g_text_submit_tactical &&
                           chat == g_text_submit_widget;
        g_text_submit_token = 0;
        g_text_submit_tactical = 0;
        g_text_submit_widget = 0;
        // UP is queued now but the existing per-key frame gate delivers it
        // only on a later tick. End/hide cannot strand this owned release.
        g_discrete_buttons.SetSynthetic(warband::input::kEnterConfirm, false);
        if (!owned &&
            ((g_discrete_buttons.PhysicalHeld() | g_discrete_buttons.VirtualHeld()) &
             warband::input::kEnterConfirm) == 0) return 0u;
    }
    return encoded_event;
}

bool EncodeArmBl(uint32_t source, uint32_t target, uint32_t* instruction) {
    if (!instruction || (source & 3u) || (target & 3u)) return false;
    const int64_t delta = static_cast<int64_t>(target) -
                          (static_cast<int64_t>(source) + 8);
    if ((delta & 3) != 0 || delta < -(1LL << 25) || delta > ((1LL << 25) - 4)) {
        return false;
    }
    // Division is exact after the alignment guard and avoids relying on an
    // implementation-defined signed right shift for backward branches.
    *instruction = 0xEB000000u |
                   (static_cast<uint32_t>(delta / 4) & 0x00FFFFFFu);
    return true;
}

bool EncodeArmB(uint32_t source, uint32_t target, uint32_t* instruction) {
    if (!instruction || (source & 3u) || (target & 3u)) return false;
    const int64_t delta = static_cast<int64_t>(target) -
                          (static_cast<int64_t>(source) + 8);
    if ((delta & 3) != 0 || delta < -(1LL << 25) || delta > ((1LL << 25) - 4)) {
        return false;
    }
    *instruction = 0xEA000000u |
                   (static_cast<uint32_t>(delta / 4) & 0x00FFFFFFu);
    return true;
}

bool InstallInventoryHeldOverlayPatch(DynarmicHost& host) {
    ElfLoader* game = host.GetLibrary("libMBExpMobile.so");
    if (!game) {
        LOGE("inventory held overlay patch: game image not found");
        return false;
    }
    const uint32_t base = game->GetBaseAddress();
    const uint64_t branch64 = static_cast<uint64_t>(base) +
                              kInventoryHeldOverlayClearBranchOffset;
    if (branch64 > std::numeric_limits<uint32_t>::max() ||
        kInventoryHeldOverlayClearBranchOffset + sizeof(uint32_t) >
            game->GetImageSize() ||
        !GuestAddressRange(host, branch64, sizeof(uint32_t))) {
        LOGE("inventory held overlay patch: address guard failed base=0x%08X "
             "size=0x%08X", base, game->GetImageSize());
        return false;
    }
    const uint32_t branch = static_cast<uint32_t>(branch64);
    const uint32_t original = host.MemoryRead32(branch);
    if (original != kExpectedInventoryHeldOverlayClearBranch) {
        LOGE("inventory held overlay patch: instruction guard failed at "
             "0x%08X got=0x%08X", branch, original);
        return false;
    }
    host.MemoryWrite32(branch, kPreserveInventoryHeldOverlayBranch);
    host.InvalidateGuestCode(branch, sizeof(uint32_t));
    if (host.MemoryRead32(branch) != kPreserveInventoryHeldOverlayBranch) {
        LOGE("inventory held overlay patch: write verification failed at "
             "0x%08X", branch);
        return false;
    }
    LOGI("inventory held overlay patch installed branch=0x%08X", branch);
    return true;
}

bool InstallInventoryPostGamepadHook(DynarmicHost& host) {
    ElfLoader* game = host.GetLibrary("libMBExpMobile.so");
    if (!game) {
        LOGE("inventory post-gamepad hook: game image not found");
        return false;
    }
    const uint32_t base = game->GetBaseAddress();
    const uint32_t update = host.FindSymbol(
        "_ZN28rglWidget_gamepad_controller6updateE12Game_windows");
    const uint32_t callsite = base + kInventoryGamepadUpdateCallOffset;
    const uint32_t stub = base + kInventoryPostGamepadStubOffset;
    if (update != base + kInventoryGamepadUpdateOffset ||
        !GuestAddressRange(host, callsite, sizeof(uint32_t)) ||
        !GuestAddressRange(host, stub,
                           kInventoryPostGamepadStubWords * sizeof(uint32_t)) ||
        kInventoryPostGamepadStubOffset +
                kInventoryPostGamepadStubWords * sizeof(uint32_t) >
            game->GetImageSize()) {
        LOGE("inventory post-gamepad hook: address guard failed base=0x%08X "
             "update=0x%08X size=0x%08X",
             base, update, game->GetImageSize());
        return false;
    }
    if (host.MemoryRead32(callsite) != kExpectedInventoryGamepadUpdateCall) {
        LOGE("inventory post-gamepad hook: callsite guard failed at 0x%08X "
             "got=0x%08X",
             callsite, host.MemoryRead32(callsite));
        return false;
    }
    for (uint32_t offset = 0;
         offset < kInventoryPostGamepadStubWords * sizeof(uint32_t);
         offset += sizeof(uint32_t)) {
        if (host.MemoryRead32(stub + offset) != 0u) {
            LOGE("inventory post-gamepad hook: code cave is occupied at "
                 "0x%08X", stub + offset);
            return false;
        }
    }

    constexpr const char* kPostGamepadThunkName =
        "__warband_bridge_inventory_post_gamepad";
    ThunkManager& thunks = host.GetThunkManager();
    if (thunks.IsRegistered(kPostGamepadThunkName)) {
        LOGE("inventory post-gamepad hook: duplicate private thunk");
        return false;
    }
    thunks.RegisterThunk(kPostGamepadThunkName,
                         [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        std::lock_guard<std::mutex> lock(g_input_mutex);
        ApplyInventoryPostGamepadLocked(jit.Regs()[0]);
    });
    const uint32_t post_gamepad_thunk =
        thunks.GetOrCreateThunkAddress(kPostGamepadThunkName);
    if (!post_gamepad_thunk) {
        LOGE("inventory post-gamepad hook: thunk address missing");
        return false;
    }

    // Preserve AAPCS callee state, the controller pointer and the original
    // update return value.  The first BLX executes the untouched stock
    // controller; the second invokes the host guard at the only safe point
    // between navigation and slot logic.
    const std::array<uint32_t, kInventoryPostGamepadStubWords> code = {
        0xE92D4070u,  // push {r4-r6, lr}
        0xE1A04000u,  // mov r4, r0 (controller pointer)
        0xE59FC018u,  // ldr ip, [pc, #24] -> original update
        0xE12FFF3Cu,  // blx ip
        0xE1A05000u,  // mov r5, r0 (preserve original return value)
        0xE1A00004u,  // mov r0, r4 (host thunk argument)
        0xE59FC00Cu,  // ldr ip, [pc, #12] -> bridge post-call thunk
        0xE12FFF3Cu,  // blx ip
        0xE1A00005u,  // mov r0, r5
        0xE8BD8070u,  // pop {r4-r6, pc}
        update,
        post_gamepad_thunk,
    };
    uint32_t branch = 0u;
    if (!EncodeArmBl(callsite, stub, &branch)) {
        LOGE("inventory post-gamepad hook: stub is outside ARM BL reach");
        return false;
    }
    for (size_t index = 0; index < code.size(); ++index) {
        host.MemoryWrite32(
            stub + static_cast<uint32_t>(index * sizeof(uint32_t)),
            code[index]);
    }
    host.InvalidateGuestCode(stub, code.size() * sizeof(uint32_t));
    host.MemoryWrite32(callsite, branch);
    host.InvalidateGuestCode(callsite, sizeof(uint32_t));
    if (host.MemoryRead32(callsite) != branch ||
        host.MemoryRead32(stub) != code.front() ||
        host.MemoryRead32(stub + 11u * sizeof(uint32_t)) !=
            post_gamepad_thunk) {
        LOGE("inventory post-gamepad hook: write verification failed");
        return false;
    }
    LOGI("inventory post-gamepad hook installed call=0x%08X stub=0x%08X "
         "thunk=0x%08X", callsite, stub, post_gamepad_thunk);
    return true;
}

bool InstallConversationPostGamepadHook(DynarmicHost& host) {
    ElfLoader* game = host.GetLibrary("libMBExpMobile.so");
    if (!game) {
        LOGE("conversation post-gamepad hook: game image not found");
        return false;
    }
    const uint32_t base = game->GetBaseAddress();
    const uint32_t update = host.FindSymbol(
        "_ZN28rglWidget_gamepad_controller6updateE12Game_windows");
    const uint32_t set_highlight = host.FindSymbol(
        "_ZN17rglWindow_manager30set_active_widget_to_highlightEP14rglWidget_base");
    const uint32_t callsite = base + kConversationGamepadUpdateCallOffset;
    const uint32_t menu_callsite = base + kMenuGamepadUpdateCallOffset;
    const uint32_t initial_callsite = base + kInitialGamepadUpdateCallOffset;
    const uint32_t stub = base + kConversationPostGamepadStubOffset;
    if (update != base + kInventoryGamepadUpdateOffset ||
        set_highlight != base + kSetActiveWidgetHighlightOffset ||
         g_set_active_widget_to_highlight != set_highlight ||
         !GuestAddressRange(host, callsite, sizeof(uint32_t)) ||
         !GuestAddressRange(host, menu_callsite, sizeof(uint32_t)) ||
         !GuestAddressRange(host, initial_callsite, sizeof(uint32_t)) ||
         !GuestAddressRange(host, stub,
                           kConversationPostGamepadStubWords *
                               sizeof(uint32_t)) ||
        kConversationPostGamepadStubOffset +
                kConversationPostGamepadStubWords * sizeof(uint32_t) >
            game->GetImageSize()) {
        LOGE("conversation post-gamepad hook: address guard failed "
             "base=0x%08X update=0x%08X highlight=0x%08X size=0x%08X",
             base, update, set_highlight, game->GetImageSize());
        return false;
    }
    if (host.MemoryRead32(callsite) !=
        kExpectedConversationGamepadUpdateCall) {
        LOGE("conversation post-gamepad hook: callsite guard failed at "
             "0x%08X got=0x%08X",
             callsite, host.MemoryRead32(callsite));
        return false;
    }
    if (host.MemoryRead32(menu_callsite) !=
        kExpectedMenuGamepadUpdateCall) {
        LOGE("menu post-gamepad hook: callsite guard failed at "
             "0x%08X got=0x%08X",
             menu_callsite, host.MemoryRead32(menu_callsite));
        return false;
    }
    if (host.MemoryRead32(initial_callsite) !=
        kExpectedInitialGamepadUpdateCall) {
        LOGE("initial post-gamepad hook: callsite guard failed at "
             "0x%08X got=0x%08X",
             initial_callsite, host.MemoryRead32(initial_callsite));
        return false;
    }
    for (uint32_t offset = 0;
         offset < kConversationPostGamepadStubWords * sizeof(uint32_t);
         offset += sizeof(uint32_t)) {
        if (host.MemoryRead32(stub + offset) != 0u) {
            LOGE("conversation post-gamepad hook: code cave is occupied at "
                 "0x%08X", stub + offset);
            return false;
        }
    }

    constexpr const char* kPostGamepadThunkName =
        "__warband_bridge_conversation_post_gamepad";
    ThunkManager& thunks = host.GetThunkManager();
    if (thunks.IsRegistered(kPostGamepadThunkName)) {
        LOGE("conversation post-gamepad hook: duplicate private thunk");
        return false;
    }
    thunks.RegisterThunk(kPostGamepadThunkName,
                          [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        std::lock_guard<std::mutex> lock(g_input_mutex);
        ApplyDialogPostGamepadLocked(jit.Regs()[0]);
    });
    const uint32_t post_gamepad_thunk =
        thunks.GetOrCreateThunkAddress(kPostGamepadThunkName);
    if (!post_gamepad_thunk) {
        LOGE("conversation post-gamepad hook: thunk address missing");
        return false;
    }

    // Preserve the stock update and its return value.  The bridge callback
    // runs before Conversation_window::check reads controller[0] for desktop
    // focus-band auto-scrolling.
    const std::array<uint32_t, kConversationPostGamepadStubWords> code = {
        0xE92D4070u,  // push {r4-r6, lr}
        0xE1A04000u,  // mov r4, r0 (controller pointer)
        0xE59FC018u,  // ldr ip, [pc, #24] -> original update
        0xE12FFF3Cu,  // blx ip
        0xE1A05000u,  // mov r5, r0 (preserve original return value)
        0xE1A00004u,  // mov r0, r4 (host thunk argument)
        0xE59FC00Cu,  // ldr ip, [pc, #12] -> bridge post-call thunk
        0xE12FFF3Cu,  // blx ip
        0xE1A00005u,  // mov r0, r5
        0xE8BD8070u,  // pop {r4-r6, pc}
        update,
        post_gamepad_thunk,
    };
    uint32_t branch = 0u;
    if (!EncodeArmBl(callsite, stub, &branch)) {
        LOGE("conversation post-gamepad hook: stub is outside ARM BL reach");
        return false;
    }
    uint32_t menu_branch = 0u;
    if (!EncodeArmBl(menu_callsite, stub, &menu_branch)) {
        LOGE("menu post-gamepad hook: stub is outside ARM BL reach");
        return false;
    }
    uint32_t initial_branch = 0u;
    if (!EncodeArmBl(initial_callsite, stub, &initial_branch)) {
        LOGE("initial post-gamepad hook: stub is outside ARM BL reach");
        return false;
    }
    for (size_t index = 0; index < code.size(); ++index) {
        host.MemoryWrite32(
            stub + static_cast<uint32_t>(index * sizeof(uint32_t)),
            code[index]);
    }
    host.InvalidateGuestCode(stub, code.size() * sizeof(uint32_t));
    host.MemoryWrite32(callsite, branch);
    host.InvalidateGuestCode(callsite, sizeof(uint32_t));
    host.MemoryWrite32(menu_callsite, menu_branch);
    host.InvalidateGuestCode(menu_callsite, sizeof(uint32_t));
    host.MemoryWrite32(initial_callsite, initial_branch);
    host.InvalidateGuestCode(initial_callsite, sizeof(uint32_t));
    if (host.MemoryRead32(callsite) != branch ||
        host.MemoryRead32(menu_callsite) != menu_branch ||
        host.MemoryRead32(initial_callsite) != initial_branch ||
        host.MemoryRead32(stub) != code.front() ||
        host.MemoryRead32(stub + 11u * sizeof(uint32_t)) !=
            post_gamepad_thunk) {
        LOGE("conversation post-gamepad hook: write verification failed");
        return false;
    }
    LOGI("dialog post-gamepad hook installed conversation=0x%08X "
         "menu=0x%08X initial=0x%08X stub=0x%08X thunk=0x%08X",
         callsite, menu_callsite, initial_callsite, stub,
         post_gamepad_thunk);
    return true;
}

bool InstallFrameInputHook(DynarmicHost& host) {
    ElfLoader* game = host.GetLibrary("libMBExpMobile.so");
    if (!game) {
        LOGE("frame input hook: game image not found");
        return false;
    }
    const uint32_t base = game->GetBaseAddress();
    const uint32_t tick = host.FindSymbol("_ZN7rglKeys4tickEf");
    const uint32_t callsite = base + kFrameMoveKeysTickCallOffset;
    const uint32_t stub = base + kDirectLookStubOffset;
    const uint32_t conversation_callsite =
        base + kConversationContinueCheckOffset;
    const uint32_t conversation_stub = base + kConversationContinueStubOffset;
    if (tick != base + kRglKeysTickOffset ||
        !GuestAddressRange(host, callsite, sizeof(uint32_t)) ||
        !GuestAddressRange(host, stub,
                           kFrameInputStubWords * sizeof(uint32_t)) ||
        !GuestAddressRange(host, conversation_stub,
                           kConversationContinueStubWords * sizeof(uint32_t)) ||
        !GuestAddressRange(host, conversation_callsite, sizeof(uint32_t)) ||
        kDirectLookStubOffset + kFrameInputStubWords * sizeof(uint32_t) >
            game->GetImageSize() ||
        kConversationContinueStubOffset +
                kConversationContinueStubWords * sizeof(uint32_t) >
            game->GetImageSize()) {
        LOGE("frame input hook: address guard failed base=0x%08X tick=0x%08X size=0x%08X",
             base, tick, game->GetImageSize());
        return false;
    }
    if (host.MemoryRead32(callsite) != kExpectedKeysTickCall) {
        LOGE("frame input hook: callsite guard failed at 0x%08X got=0x%08X",
             callsite, host.MemoryRead32(callsite));
        return false;
    }
    if (host.MemoryRead32(conversation_callsite) !=
        kExpectedConversationContinueCheck) {
        LOGE("conversation continue hook: branch guard failed at 0x%08X got=0x%08X",
             conversation_callsite, host.MemoryRead32(conversation_callsite));
        return false;
    }
    for (uint32_t offset = 0; offset < kFrameInputStubWords * sizeof(uint32_t);
         offset += sizeof(uint32_t)) {
        if (host.MemoryRead32(stub + offset) != 0) {
            LOGE("frame input hook: code cave is occupied at 0x%08X", stub + offset);
            return false;
        }
    }
    for (uint32_t offset = 0;
         offset < kConversationContinueStubWords * sizeof(uint32_t);
         offset += sizeof(uint32_t)) {
        if (host.MemoryRead32(conversation_stub + offset) != 0) {
            LOGE("conversation continue hook: code cave is occupied at 0x%08X",
                 conversation_stub + offset);
            return false;
        }
    }
    if (!g_sheath_weapons) {
        LOGE("frame input hook: Agent::sheath_weapons symbol missing");
        return false;
    }

    constexpr const char* kDrainThunkName = "__warband_bridge_drain_frame_input";
    constexpr const char* kPopThunkName = "__warband_bridge_pop_input_event";
    constexpr const char* kConversationThunkName =
        "__warband_bridge_conversation_continue";
    ThunkManager& thunks = host.GetThunkManager();
    if (thunks.IsRegistered(kDrainThunkName) || thunks.IsRegistered(kPopThunkName) ||
        thunks.IsRegistered(kConversationThunkName)) {
        LOGE("frame input hook: duplicate private thunk");
        return false;
    }
    thunks.RegisterThunk(kDrainThunkName, [](Dynarmic::A32::Jit& jit,
                                             DynarmicHost&) {
        uint32_t sheath_target = 0;
        {
            std::lock_guard<std::mutex> lock(g_input_mutex);
            sheath_target = DrainFrameInputLocked(
                GuestSettingsFloatFromBits(jit.Regs()[7]));
            ApplyPendingOrdersRequestLocked();
        }
        jit.Regs()[0] = sheath_target;
    });
    thunks.RegisterThunk(kPopThunkName, [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        std::lock_guard<std::mutex> lock(g_input_mutex);
        jit.Regs()[0] = PopPendingGuestEventLocked();
    });
    thunks.RegisterThunk(kConversationThunkName,
                         [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        std::lock_guard<std::mutex> lock(g_input_mutex);
        jit.Regs()[0] = ConsumePendingConversationTapLocked(jit.Regs()[0]) ? 1u : 0u;
    });
    const uint32_t drain_thunk = thunks.GetOrCreateThunkAddress(kDrainThunkName);
    const uint32_t pop_thunk = thunks.GetOrCreateThunkAddress(kPopThunkName);
    const uint32_t conversation_thunk =
        thunks.GetOrCreateThunkAddress(kConversationThunkName);
    if (!drain_thunk || !pop_thunk || !g_guest_keys || !g_add_event ||
        !g_pending_character_slot || !conversation_thunk) {
        LOGE("frame input hook: input symbol/thunk address missing keys=0x%08X "
             "add=0x%08X pending_char=0x%08X drain=0x%08X pop=0x%08X "
             "conversation=0x%08X",
             g_guest_keys, g_add_event, g_pending_character_slot, drain_thunk,
             pop_thunk, conversation_thunk);
        return false;
    }

    // ARM wrapper (AAPCS-aligned stack): source-arbitrated discrete events are
    // injected on the guest frame thread before rglKeys::tick.  The original
    // tick arguments survive the loop in callee-saved registers.  Afterwards
    // touch movement/direct-look are merged and sheath is optionally invoked.
    // No JNI/input-queue callback ever calls DynarmicHost::RunFunction.
    const std::array<uint32_t, 30> code = {
        0xE92D41F0u,  // push {r4-r8, lr} (24 bytes; keep SP 8-byte aligned)
        0xE1A04000u,  // mov r4, r0 (original this/rgl_keys)
        0xE1A07001u,  // mov r7, r1 (original softfp float bits)
        0xE59F504Cu,  // ldr r5, [pc, #76] -> rgl_keys
        0xE59F604Cu,  // ldr r6, [pc, #76] -> add_android_input_event
        0xE59F804Cu,  // ldr r8, [pc, #76] -> pop bridge thunk
        0xE12FFF38u,  // event_loop: blx r8
        0xE3500000u,  // cmp r0, #0 (encoded event; zero means drained)
        0x0A000003u,  // beq tick
        0xE2401001u,  // sub r1, r0, #1 (decode guest event 0..33)
        0xE1A00005u,  // mov r0, r5
        0xE12FFF36u,  // blx r6
        0xEAFFFFF8u,  // b event_loop
        0xE1A00004u,  // tick: mov r0, r4
        0xE1A01007u,  // mov r1, r7
        0xE59FC028u,  // ldr ip, [pc, #40] -> original tick
        0xE12FFF3Cu,  // blx ip
        0xE59FC024u,  // ldr ip, [pc, #36] -> drain bridge thunk
        0xE12FFF3Cu,  // blx ip (returns optional player Agent*)
        0xE3500000u,  // cmp r0, #0
        0x0A000001u,  // beq done
        0xE59FC018u,  // ldr ip, [pc, #24] -> sheath_weapons
        0xE12FFF3Cu,  // blx ip
        0xE8BD81F0u,  // done: pop {r4-r8, pc}
        g_guest_keys,
        g_add_event,
        pop_thunk,
        tick,
        drain_thunk,
        g_sheath_weapons,
    };

    // Replace the stock conditional branch that tests A for an empty-choice
    // conversation. The thunk only consumes a validated host tap; the stub
    // then enters the stock refresh_conversation path and its normal result
    // handling. It preserves every live guest register and emits no input.
    std::array<uint32_t, kConversationContinueStubWords> conversation_code = {
        0xE92D503Fu,  // push {r0-r5, r12, lr}
        0xE1A0000Au,  // mov r0, r10 (Conversation_window::this)
        0xE59FC024u,  // ldr ip, [pc, #36] -> bridge tap thunk
        0xE12FFF3Cu,  // blx ip
        0xE3500000u,  // cmp r0, #0 (tap consumed?)
        0x1A000002u,  // bne advance
        0xE59D0000u,  // ldr r0, [sp] (original stock A state)
        0xE3500000u,  // cmp r0, #0
        0x0A000001u,  // beq skip_refresh
        0xE8BD503Fu,  // advance: pop {r0-r5, r12, lr}
        0u,            // b stock refresh_conversation call
        0xE8BD503Fu,  // skip_refresh: pop {r0-r5, r12, lr}
        0u,            // b stock post-refresh result handling
        conversation_thunk,
    };
    uint32_t branch = 0;
    if (!EncodeArmBl(callsite, stub, &branch)) {
        LOGE("frame input hook: wrapper is outside ARM BL reach");
        return false;
    }
    if (!EncodeArmB(conversation_stub + 10u * sizeof(uint32_t),
                    conversation_callsite + sizeof(uint32_t),
                    &conversation_code[10]) ||
        !EncodeArmB(conversation_stub + 12u * sizeof(uint32_t),
                    conversation_callsite + 5u * sizeof(uint32_t),
                    &conversation_code[12])) {
        LOGE("conversation continue hook: branch target is outside ARM reach");
        return false;
    }
    uint32_t conversation_branch = 0;
    if (!EncodeArmBl(conversation_callsite, conversation_stub,
                     &conversation_branch)) {
        LOGE("conversation continue hook: stub is outside ARM BL reach");
        return false;
    }
    for (size_t index = 0; index < code.size(); ++index) {
        host.MemoryWrite32(stub + static_cast<uint32_t>(index * sizeof(uint32_t)), code[index]);
    }
    for (size_t index = 0; index < conversation_code.size(); ++index) {
        host.MemoryWrite32(conversation_stub +
                               static_cast<uint32_t>(index * sizeof(uint32_t)),
                           conversation_code[index]);
    }
    host.InvalidateGuestCode(stub, code.size() * sizeof(uint32_t));
    host.InvalidateGuestCode(conversation_stub,
                             conversation_code.size() * sizeof(uint32_t));
    host.MemoryWrite32(callsite, branch);
    host.InvalidateGuestCode(callsite, sizeof(uint32_t));
    host.MemoryWrite32(conversation_callsite, conversation_branch);
    host.InvalidateGuestCode(conversation_callsite, sizeof(uint32_t));
    if (host.MemoryRead32(callsite) != branch || host.MemoryRead32(stub) != code.front() ||
        host.MemoryRead32(conversation_callsite) != conversation_branch ||
        host.MemoryRead32(conversation_stub) != conversation_code.front() ||
        host.MemoryRead32(conversation_stub + 13u * sizeof(uint32_t)) !=
            conversation_thunk) {
        LOGE("frame input hook: write verification failed");
        return false;
    }
    LOGI("frame input hook installed call=0x%08X stub=0x%08X thunk=0x%08X; "
         "conversation call=0x%08X stub=0x%08X thunk=0x%08X",
         callsite, stub, drain_thunk, conversation_callsite, conversation_stub,
         conversation_thunk);
    return true;
}

bool InstallMapGameTickHook(DynarmicHost& host) {
    ElfLoader* game = host.GetLibrary("libMBExpMobile.so");
    if (!game) {
        LOGE("map time hook: game image not found");
        return false;
    }
    const uint32_t base = game->GetBaseAddress();
    const uint32_t callsite = base + kMapGameTickCallOffset;
    const uint32_t tick = base + kMapGameTickOffset;
    const uint32_t stub = base + kMapGameTickStubOffset;
    constexpr uint32_t kStubWords = 6u;
    if (kMapGameTickCallOffset + sizeof(uint32_t) > game->GetImageSize() ||
        kMapGameTickOffset + sizeof(uint32_t) > game->GetImageSize() ||
        kMapGameTickStubOffset + kStubWords * sizeof(uint32_t) >
            game->GetImageSize() ||
        !GuestAddressRange(host, callsite, sizeof(uint32_t)) ||
        !GuestAddressRange(host, tick, sizeof(uint32_t)) ||
        !GuestAddressRange(host, stub, kStubWords * sizeof(uint32_t)) ||
        host.MemoryRead32(callsite) != kExpectedMapGameTickCall) {
        LOGE("map time hook: address or callsite guard failed base=0x%08X", base);
        return false;
    }
    for (uint32_t offset = 0; offset < kStubWords * sizeof(uint32_t);
         offset += sizeof(uint32_t)) {
        if (host.MemoryRead32(stub + offset) != 0u) {
            LOGE("map time hook: code cave occupied at 0x%08X", stub + offset);
            return false;
        }
    }
    constexpr const char* kThunkName = "__warband_bridge_map_game_tick";
    ThunkManager& thunks = host.GetThunkManager();
    if (thunks.IsRegistered(kThunkName)) return false;
    thunks.RegisterThunk(kThunkName,
                         [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        std::lock_guard<std::mutex> lock(g_input_mutex);
        ApplyMapRouteAndTimeLocked(jit.Regs()[0]);
    });
    const uint32_t thunk = thunks.GetOrCreateThunkAddress(kThunkName);
    uint32_t call_branch = 0;
    uint32_t tick_branch = 0;
    if (!thunk || !EncodeArmBl(callsite, stub, &call_branch) ||
        !EncodeArmB(stub + 4u * sizeof(uint32_t), tick, &tick_branch)) {
        LOGE("map time hook: thunk or branch target unavailable");
        return false;
    }
    const std::array<uint32_t, kStubWords> code = {
        0xE92D4013u,  // push {r0, r1, r4, lr}; retain Game::tick arguments
        0xE59FC008u,  // ldr ip, [pc, #8] -> bridge thunk
        0xE12FFF3Cu,  // blx ip
        0xE8BD4013u,  // pop {r0, r1, r4, lr}
        tick_branch,   // b Game::tick, preserving the stock return address
        thunk,
    };
    for (size_t index = 0; index < code.size(); ++index) {
        host.MemoryWrite32(stub + static_cast<uint32_t>(4u * index), code[index]);
    }
    host.InvalidateGuestCode(stub, code.size() * sizeof(uint32_t));
    host.MemoryWrite32(callsite, call_branch);
    host.InvalidateGuestCode(callsite, sizeof(uint32_t));
    if (host.MemoryRead32(callsite) != call_branch ||
        host.MemoryRead32(stub) != code.front() ||
        host.MemoryRead32(stub + 5u * sizeof(uint32_t)) != thunk) {
        LOGE("map time hook: write verification failed");
        return false;
    }
    LOGI("map time hook installed call=0x%08X stub=0x%08X thunk=0x%08X",
         callsite, stub, thunk);
    return true;
}

bool IsValidMasterServerHost(std::string_view host) {
    if (host.empty()) return true;
    if (host.size() > 26u || host.front() == '.' || host.back() == '.') return false;
    size_t label_length = 0;
    bool previous_dot = false;
    for (size_t index = 0; index < host.size(); ++index) {
        char value = host[index];
        if (value >= 'A' && value <= 'Z') value = static_cast<char>(value + ('a' - 'A'));
        if (value == '.') {
            if (previous_dot || label_length == 0u || host[index - 1u] == '-') return false;
            previous_dot = true;
            label_length = 0;
        } else {
            const bool alphanumeric = (value >= 'a' && value <= 'z') ||
                                      (value >= '0' && value <= '9');
            if (!alphanumeric && value != '-') return false;
            if (label_length == 0u && value == '-') return false;
            previous_dot = false;
            ++label_length;
        }
    }
    return label_length != 0u && host.back() != '-';
}

bool IsValidMasterServerMode(std::string_view mode) {
    return mode == "lan" || mode == "official" || mode == "oreo922" || mode == "custom";
}

bool MasterServerEndpoint(std::string_view mode, std::string_view requested_host,
                          std::string* url, std::string* hostname) {
    if (!IsValidMasterServerMode(mode)) return false;
    if (mode == "official") {
        if (!requested_host.empty()) return false;
        *hostname = "warbandmain.taleworlds.com";
    } else if (mode == "lan") {
        if (!requested_host.empty()) return false;
        *hostname = "127.0.0.1";
    } else if (mode == "oreo922") {
        if (requested_host != "oreo922.cn") return false;
        *hostname = "oreo922.cn";
    } else {
        if (requested_host.empty() || !IsValidMasterServerHost(requested_host))
            return false;
        hostname->assign(requested_host.data(), requested_host.size());
    }
    for (char& value : *hostname) {
        if (value >= 'A' && value <= 'Z') value = static_cast<char>(value + ('a' - 'A'));
    }
    *url = "http://" + *hostname + "/";
    return true;
}

bool ApplyGuestMasterServerConfig(DynarmicHost& host, std::string_view mode,
                                  std::string_view requested_host,
                                  std::string_view previous_mode,
                                  std::string_view previous_host) {
    std::string url;
    std::string master_host;
    std::string previous_url;
    std::string previous_hostname;
    if (!MasterServerEndpoint(mode, requested_host, &url, &master_host) ||
        !MasterServerEndpoint(previous_mode, previous_host, &previous_url,
                              &previous_hostname)) return false;

    constexpr char kOriginalUrl[] = "http://warbandmain.taleworlds.com/";
    constexpr char kOriginalHost[] = "warbandmain.taleworlds.com";
    constexpr uint32_t kMasterServerUrlOffset = 0x003A36FCu;
    constexpr uint32_t kMasterServerHostOffset = 0x003A371Fu;
    // Basic_game copies both literals into inline rglString objects during
    // global construction. Changing only the literals leaves the active client
    // querying the previous master until the process restarts.
    constexpr uint32_t kBasicGameOffset = 0x007B1908u;
    constexpr uint32_t kMasterUrlStringOffset = 0x90u;
    constexpr uint32_t kMasterHostStringOffset = 0x120u;
    constexpr uint32_t kRglStringInlineBytes = 0x80u;
    ElfLoader* game = host.GetLibrary("libMBExpMobile.so");
    if (!game || game->GetImageSize() <
            kBasicGameOffset + kMasterHostStringOffset +
                0x10u + kRglStringInlineBytes) {
        return false;
    }

    const uint32_t base = game->GetBaseAddress();
    const uint32_t basic_game = base + kBasicGameOffset;
    const uint32_t url_object = basic_game + kMasterUrlStringOffset;
    const uint32_t host_object = basic_game + kMasterHostStringOffset;
    const auto matches_at = [&](uint32_t offset, const char* expected, size_t length) {
        for (size_t index = 0; index < length; ++index) {
            if (host.MemoryRead8(base + offset + static_cast<uint32_t>(index)) !=
                static_cast<uint8_t>(expected[index])) {
                return false;
            }
        }
        return host.MemoryRead8(base + offset + static_cast<uint32_t>(length)) == 0u;
    };
    const auto object_uninitialized = [&](uint32_t object) {
        return host.MemoryRead32(object) == 0u &&
            host.MemoryRead32(object + 4u) == 0u &&
            host.MemoryRead32(object + 8u) == 0u &&
            host.MemoryRead32(object + 12u) == 0u;
    };
    const auto object_matches = [&](uint32_t object, const std::string& expected) {
        if (host.MemoryRead32(object + 4u) != object + 0x10u ||
            host.MemoryRead32(object + 8u) != kRglStringInlineBytes ||
            host.MemoryRead32(object + 12u) != expected.size()) return false;
        for (size_t index = 0; index < expected.size(); ++index) {
            if (host.MemoryRead8(object + 0x10u + static_cast<uint32_t>(index)) !=
                static_cast<uint8_t>(expected[index])) return false;
        }
        return host.MemoryRead8(object + 0x10u +
                                static_cast<uint32_t>(expected.size())) == 0u;
    };
    const bool runtime_uninitialized = object_uninitialized(url_object) &&
        object_uninitialized(host_object);
    if (!runtime_uninitialized &&
        (!object_matches(url_object, previous_url) ||
         !object_matches(host_object, previous_hostname))) return false;
    if (previous_url.size() >= sizeof(kOriginalUrl) ||
        previous_hostname.size() >= sizeof(kOriginalHost) ||
        !matches_at(kMasterServerUrlOffset, previous_url.c_str(), previous_url.size()) ||
        !matches_at(kMasterServerHostOffset, previous_hostname.c_str(),
                    previous_hostname.size())) {
        return false;
    }

    if (url.size() >= sizeof(kOriginalUrl)) return false;
    const uint32_t old_url_cache = runtime_uninitialized ? 0u :
        host.MemoryRead32(url_object);
    const uint32_t old_host_cache = runtime_uninitialized ? 0u :
        host.MemoryRead32(host_object);
    const auto write_object = [&](uint32_t object, const std::string& value,
                                  size_t old_length, uint32_t cache) {
        for (size_t index = 0; index <= std::max(old_length, value.size()); ++index) {
            host.MemoryWrite8(object + 0x10u + static_cast<uint32_t>(index),
                              index < value.size()
                                  ? static_cast<uint8_t>(value[index]) : 0u);
        }
        host.MemoryWrite32(object + 12u, static_cast<uint32_t>(value.size()));
        host.MemoryWrite32(object, cache);
    };
    for (size_t index = 0; index < sizeof(kOriginalUrl); ++index) {
        const uint8_t value = index < url.size()
            ? static_cast<uint8_t>(url[index]) : 0u;
        host.MemoryWrite8(base + kMasterServerUrlOffset +
                              static_cast<uint32_t>(index), value);
    }
    for (size_t index = 0; index < sizeof(kOriginalHost); ++index) {
        const uint8_t value = index < master_host.size()
            ? static_cast<uint8_t>(master_host[index]) : 0u;
        host.MemoryWrite8(base + kMasterServerHostOffset +
                              static_cast<uint32_t>(index), value);
    }
    if (!runtime_uninitialized) {
        write_object(url_object, url, previous_url.size(), 0u);
        write_object(host_object, master_host, previous_hostname.size(), 0u);
    }
    if (matches_at(kMasterServerUrlOffset, url.c_str(), url.size()) &&
        matches_at(kMasterServerHostOffset, master_host.c_str(), master_host.size()) &&
        (runtime_uninitialized ||
         (object_matches(url_object, url) &&
          object_matches(host_object, master_host)))) {
        return true;
    }
    if (!runtime_uninitialized) {
        write_object(url_object, previous_url, url.size(), old_url_cache);
        write_object(host_object, previous_hostname, master_host.size(), old_host_cache);
    }
    for (size_t index = 0; index < sizeof(kOriginalUrl); ++index) {
        host.MemoryWrite8(base + kMasterServerUrlOffset + static_cast<uint32_t>(index),
                          index < previous_url.size()
                              ? static_cast<uint8_t>(previous_url[index]) : 0u);
    }
    for (size_t index = 0; index < sizeof(kOriginalHost); ++index) {
        host.MemoryWrite8(base + kMasterServerHostOffset + static_cast<uint32_t>(index),
                          index < previous_hostname.size()
                              ? static_cast<uint8_t>(previous_hostname[index]) : 0u);
    }
    return false;
}

bool EncodeArmBranch(uint32_t source, uint32_t destination, bool link,
                     uint32_t* instruction) {
    if (!instruction || ((source | destination) & 3u) != 0u) return false;
    const int64_t displacement = static_cast<int64_t>(destination) -
                                 static_cast<int64_t>(source) - 8;
    if (displacement < -0x02000000ll || displacement > 0x01fffffcll) return false;
    const uint32_t opcode = link ? 0xEB000000u : 0xEA000000u;
    *instruction = opcode |
        (static_cast<uint32_t>(displacement / 4) & 0x00FFFFFFu);
    return true;
}

bool InstallServerBrowserDefault(DynarmicHost& host, std::string_view mode) {
    // Multiplayer_client_window::activate selects the source through a virtual
    // call before it starts the initial scan. Select LAN or Internet from the
    // active master-server choice before that first scan.
    constexpr uint32_t kCallsite = 0x0012F8C8u;
    constexpr uint32_t kReturnAddress = kCallsite + 4u;
    constexpr uint32_t kStub = 0x003CDF40u;
    constexpr uint32_t kExpectedCallsite = 0xE12FFF32u; // BLX R2
    constexpr size_t kStubWords = 3u;

    ElfLoader* game = host.GetLibrary("libMBExpMobile.so");
    if (!game || game->GetImageSize() < kStub + kStubWords * sizeof(uint32_t)) {
        return false;
    }
    const uint32_t base = game->GetBaseAddress();
    if (host.MemoryRead32(base + kCallsite) != kExpectedCallsite) {
        return false;
    }
    for (size_t index = 0; index < kStubWords; ++index) {
        if (host.MemoryRead32(base + kStub + static_cast<uint32_t>(index * 4u)) != 0u) {
            return false;
        }
    }

    uint32_t return_to_guest = 0;
    uint32_t branch_to_stub = 0;
    if (!EncodeArmBranch(base + kStub + 2u * sizeof(uint32_t), base + kReturnAddress,
                         false, &return_to_guest) ||
        !EncodeArmBranch(base + kCallsite, base + kStub, false, &branch_to_stub)) {
        return false;
    }

    const std::array<uint32_t, kStubWords> stub = {
        mode == "lan" ? 0xE3A01002u : 0xE3A01000u, // MOV R1, #2 / #0
        kExpectedCallsite, // BLX R2 (original combo method)
        return_to_guest,
    };
    for (size_t index = 0; index < stub.size(); ++index) {
        host.MemoryWrite32(base + kStub + static_cast<uint32_t>(index * 4u), stub[index]);
    }
    host.InvalidateGuestCode(base + kStub,
                             static_cast<uint32_t>(stub.size() * sizeof(uint32_t)));
    host.MemoryWrite32(base + kCallsite, branch_to_stub);
    host.InvalidateGuestCode(base + kCallsite, sizeof(uint32_t));

    if (host.MemoryRead32(base + kCallsite) != branch_to_stub ||
        host.MemoryRead32(base + kStub + 1u * sizeof(uint32_t)) != kExpectedCallsite ||
        host.MemoryRead32(base + kStub + 2u * sizeof(uint32_t)) != return_to_guest) {
        return false;
    }
    LOGI("server browser default hook installed source=%s", mode == "lan" ? "LAN" : "Internet");
    return true;
}

bool SetServerBrowserDefault(DynarmicHost& host, std::string_view mode) {
    constexpr uint32_t kCallsite = 0x0012F8C8u;
    constexpr uint32_t kStub = 0x003CDF40u;
    ElfLoader* game = host.GetLibrary("libMBExpMobile.so");
    if (!game || game->GetImageSize() < kStub + 12u ||
        !IsValidMasterServerMode(mode)) return false;
    const uint32_t base = game->GetBaseAddress();
    uint32_t branch = 0u;
    if (!EncodeArmBranch(base + kCallsite, base + kStub, false, &branch) ||
        host.MemoryRead32(base + kCallsite) != branch ||
        host.MemoryRead32(base + kStub + 4u) != 0xE12FFF32u) return false;
    const uint32_t instruction = mode == "lan" ? 0xE3A01002u : 0xE3A01000u;
    const uint32_t old_instruction = host.MemoryRead32(base + kStub);
    if (old_instruction != 0xE3A01002u && old_instruction != 0xE3A01000u)
        return false;
    if (old_instruction == instruction) return true;
    host.MemoryWrite32(base + kStub, instruction);
    host.InvalidateGuestCode(base + kStub, sizeof(uint32_t));
    return host.MemoryRead32(base + kStub) == instruction;
}

bool BootGuest(ANativeActivity* activity) {
    std::lock_guard<std::mutex> lock(g_boot_mutex);
    if (g_host) return true;
    // This is the only non-fatal global invalidation: a genuinely fresh bridge
    // initialization must not inherit a token from an earlier failed boot.
    InvalidateTextInputForBridgeLifecycle(true);
    const uint64_t startup_started = MonotonicMilliseconds();
    if (!activity || !activity->vm || !activity->assetManager) {
        LOGE("NativeActivity has no JavaVM/AssetManager");
        InvalidateTextInputForBridgeLifecycle(true);
        return false;
    }

    g_jvm = activity->vm;
    g_apk_path = ApplicationInfoString(activity, "sourceDir");
    if (g_apk_path.empty()) {
        LOGE("could not resolve ApplicationInfo.sourceDir");
        InvalidateTextInputForBridgeLifecycle(true);
        return false;
    }
    ApkSetPrimaryPackage(g_apk_path.c_str());
    // Decode the packaged Ogg effects on the host while the engine is still
    // booting and loading textures; createSound later hands FMOD ready PCM.
    // The APK entry index this enumerates exists only past this point.
    WarbandAudioPrefetchStart();

    g_guest_language_string = 0;
    auto host = std::make_unique<DynarmicHost>();
    BindGuestBridge(*host);
    const uint64_t initialize_started = MonotonicMilliseconds();
    if (!host->Initialize()) {
        LOGE("DynarmicHost::Initialize failed");
        InvalidateTextInputForBridgeLifecycle(true);
        return false;
    }
    const uint64_t initialize_ms = MonotonicMilliseconds() - initialize_started;

    g_host = std::move(host);
    g_report_scale_verified_base = std::numeric_limits<uint32_t>::max();
    g_report_scale_expected_words = warband::report_text::kOriginalScaleWords;
    const std::vector<DynarmicHost::GuestLibrary> libraries = {
        {"libfmodex.so", "guestlib/libfmodex.so"},
        {"libMBExpMobile.so", "guestlib/libMBExpMobile.so"},
    };
    const uint64_t libraries_started = MonotonicMilliseconds();
    if (!g_host->LoadGuestLibraries(activity->assetManager, libraries)) {
        LOGE("guest libraries failed to load");
        g_host.reset();
        InvalidateTextInputForBridgeLifecycle(true);
        return false;
    }
    const uint64_t libraries_ms = MonotonicMilliseconds() - libraries_started;

    if (!InstallOfflineLanAuthHook(*g_host)) {
        LOGE("startup bridge rejected the LAN HTTP hook");
        g_host.reset();
        InvalidateTextInputForBridgeLifecycle(true);
        return false;
    }
    if (!ApplyGuestMasterServerConfig(*g_host, g_master_server_mode,
                                      g_master_server_host, "official", "")) {
        LOGE("startup bridge rejected the configured master-server selection");
        g_host.reset();
        InvalidateTextInputForBridgeLifecycle(true);
        return false;
    }
    if (!InstallServerBrowserDefault(*g_host, g_master_server_mode)) {
        LOGE("startup bridge could not set the server-browser default safely");
        g_host.reset();
        InvalidateTextInputForBridgeLifecycle(true);
        return false;
    }

    // initialize_stuff is reached from the guest activity startup below. Patch its guarded
    // language literal only after relocation/loading, but before StartGuestActivity can call it.
    const uint64_t patches_started = MonotonicMilliseconds();
    if (!ApplyGuestLanguage(*g_host, g_game_language)) {
        LOGE("guest boot aborted because language selection could not be applied safely");
        g_guest_language_string = 0;
        g_host.reset();
        InvalidateTextInputForBridgeLifecycle(true);
        return false;
    }

    if (!InstallRussianTextProcessorHook(*g_host, g_game_language)) {
        LOGE("guest boot aborted because the Russian text hook could not be installed safely");
        g_guest_language_string = 0;
        g_host.reset();
        InvalidateTextInputForBridgeLifecycle(true);
        return false;
    }

    ResolveInputSymbols();
    if (!InstallInventoryHeldOverlayPatch(*g_host)) {
        LOGE("guest boot aborted because the inventory held overlay patch "
             "could not be installed safely");
        g_guest_language_string = 0;
        g_host.reset();
        InvalidateTextInputForBridgeLifecycle(true);
        return false;
    }
    if (!InstallInventoryPostGamepadHook(*g_host)) {
        LOGE("guest boot aborted because the inventory post-gamepad hook "
             "could not be installed safely");
        g_guest_language_string = 0;
        g_host.reset();
        InvalidateTextInputForBridgeLifecycle(true);
        return false;
    }
    if (!InstallConversationPostGamepadHook(*g_host)) {
        LOGE("guest boot aborted because the conversation post-gamepad hook "
             "could not be installed safely");
        g_guest_language_string = 0;
        g_host.reset();
        InvalidateTextInputForBridgeLifecycle(true);
        return false;
    }
    if (!InstallFrameInputHook(*g_host)) {
        LOGE("guest boot aborted because the frame input hook could not be installed safely");
        g_guest_language_string = 0;
        g_host.reset();
        InvalidateTextInputForBridgeLifecycle(true);
        return false;
    }
    if (!InstallMapGameTickHook(*g_host)) {
        LOGE("guest boot aborted because the map time hook could not be installed safely");
        g_guest_language_string = 0;
        g_host.reset();
        InvalidateTextInputForBridgeLifecycle(true);
        return false;
    }
    const uint64_t patches_ms = MonotonicMilliseconds() - patches_started;
    LOGI("startup bridge ready: total_ms=%llu initialize_ms=%llu guest_libraries_ms=%llu "
         "patches_and_input_ms=%llu",
         static_cast<unsigned long long>(MonotonicMilliseconds() - startup_started),
         static_cast<unsigned long long>(initialize_ms),
         static_cast<unsigned long long>(libraries_ms),
         static_cast<unsigned long long>(patches_ms));
    return true;
}

void SetGuestActivityClass(ANativeActivity* activity) {
    if (!g_host || !activity || !activity->env) return;
    JNIEnv* env = activity->env;
    if (!g_activity_class) {
        jclass local = env->GetObjectClass(activity->clazz);
        if (!local) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            LOGE("MBNativeActivity class not found");
            return;
        }
        g_activity_class = static_cast<jclass>(env->NewGlobalRef(local));
        env->DeleteLocalRef(local);
    }
    const uint32_t slot = g_host->FindSymbol("thizClass");
    const uint32_t handle = GuestHandleFor(g_activity_class);
    if (!slot || !handle) {
        LOGE("guest thizClass setup failed: slot=0x%08X handle=0x%08X", slot, handle);
        return;
    }
    g_host->MemoryWrite32(slot, handle);
    LOGI("guest thizClass=0x%08X handle=0x%08X", slot, handle);
}

jboolean Native_PrepareGameData(JNIEnv* env, jclass, jobject asset_manager,
                                 jstring main_root, jstring patch_root, jstring language,
                                 jstring selected_module_root, jstring save_root,
                                 jstring master_server_mode, jstring master_server_host) {
    if (!env || !asset_manager || !main_root || !patch_root || !language ||
        !selected_module_root || !save_root || !master_server_mode ||
        !master_server_host) return JNI_FALSE;
    const char* main_text = env->GetStringUTFChars(main_root, nullptr);
    const char* patch_text = main_text ? env->GetStringUTFChars(patch_root, nullptr) : nullptr;
    const char* language_text = patch_text ? env->GetStringUTFChars(language, nullptr) : nullptr;
    const char* module_text = language_text
        ? env->GetStringUTFChars(selected_module_root, nullptr) : nullptr;
    const char* save_text = module_text ? env->GetStringUTFChars(save_root, nullptr) : nullptr;
    const char* master_mode_text = save_text
        ? env->GetStringUTFChars(master_server_mode, nullptr) : nullptr;
    const char* master_host_text = master_mode_text
        ? env->GetStringUTFChars(master_server_host, nullptr) : nullptr;
    bool ok = false;
    std::string accepted_language;
    std::string accepted_master_mode;
    std::string accepted_master_host;
    if (main_text && patch_text && language_text && module_text && save_text &&
        master_mode_text && master_host_text) {
        const std::string_view selected(language_text);
        const std::string_view selected_master_mode(master_mode_text);
        const std::string_view selected_master_host(master_host_text);
        if (!IsSupportedGameLanguage(selected)) {
            LOGE("nativePrepareGameData: unsupported language '%s'", language_text);
        } else if (!IsValidMasterServerMode(selected_master_mode)) {
            LOGE("nativePrepareGameData: invalid master-server mode '%s'", master_mode_text);
        } else if ((selected_master_mode == "lan" || selected_master_mode == "official") &&
                   !selected_master_host.empty()) {
            LOGE("nativePrepareGameData: host is not allowed for selected master-server mode");
        } else if (selected_master_mode == "oreo922" &&
                   selected_master_host != "oreo922.cn") {
            LOGE("nativePrepareGameData: community mode must use oreo922.cn");
        } else if (selected_master_mode == "custom" &&
                   (selected_master_host.empty() ||
                    !IsValidMasterServerHost(selected_master_host))) {
            LOGE("nativePrepareGameData: invalid saved custom hostname");
        } else {
            std::lock_guard<std::mutex> lock(g_boot_mutex);
            if (g_host) {
                LOGE("nativePrepareGameData: rejected configuration after guest boot");
            } else if (SetGameDataRoots(main_text, patch_text, module_text) &&
                       SetGuestExternalDataPath(save_text)) {
                g_guest_save_root.assign(save_text);
                g_master_server_mode.assign(selected_master_mode.data(),
                                            selected_master_mode.size());
                g_master_server_host.assign(selected_master_host.data(),
                                            selected_master_host.size());
                RemoveEmptyGuestRglConfig(g_guest_save_root);
                g_game_language.assign(selected.data(), selected.size());
                accepted_master_mode = g_master_server_mode;
                accepted_master_host = g_master_server_host;
                accepted_language = g_game_language;
                ok = true;
            }
        }
    }
    if (main_text) env->ReleaseStringUTFChars(main_root, main_text);
    if (patch_text) env->ReleaseStringUTFChars(patch_root, patch_text);
    if (language_text) env->ReleaseStringUTFChars(language, language_text);
    if (module_text) env->ReleaseStringUTFChars(selected_module_root, module_text);
    if (save_text) env->ReleaseStringUTFChars(save_root, save_text);
    if (master_mode_text) {
        env->ReleaseStringUTFChars(master_server_mode, master_mode_text);
    }
    if (master_host_text) {
        env->ReleaseStringUTFChars(master_server_host, master_host_text);
    }
    LOGI("nativePrepareGameData: %s language=%s master=%s",
         ok ? "accepted" : "rejected", ok ? accepted_language.c_str() : "<invalid>",
         ok ? accepted_master_mode.c_str() : "<invalid>");
    if (ok && !accepted_master_host.empty()) {
        LOGI("master-server hostname configured: %s", accepted_master_host.c_str());
    }
    return ok ? JNI_TRUE : JNI_FALSE;
}

jboolean Native_SelectMasterServer(JNIEnv* env, jclass, jstring mode,
                                    jstring hostname) {
    if (!env || !mode || !hostname) return JNI_FALSE;
    const char* mode_text = env->GetStringUTFChars(mode, nullptr);
    const char* host_text = mode_text ? env->GetStringUTFChars(hostname, nullptr)
                                      : nullptr;
    if (!mode_text || !host_text) {
        if (mode_text) env->ReleaseStringUTFChars(mode, mode_text);
        return JNI_FALSE;
    }
    const std::string selected_mode(mode_text);
    const std::string selected_host(host_text);
    env->ReleaseStringUTFChars(mode, mode_text);
    env->ReleaseStringUTFChars(hostname, host_text);
    std::string url;
    std::string normalized_host;
    if (!MasterServerEndpoint(selected_mode, selected_host, &url,
                              &normalized_host)) return JNI_FALSE;
    std::lock_guard<std::mutex> lock(g_input_mutex);
    if (!g_host) return JNI_FALSE;
    g_pending_master_server_mode = selected_mode;
    g_pending_master_server_host = selected_host;
    g_master_server_change_pending = true;
    return JNI_TRUE;
}

jboolean Native_MasterServerApplied(JNIEnv* env, jclass, jstring mode,
                                      jstring hostname) {
    if (!env || !mode || !hostname) return JNI_FALSE;
    const char* mode_text = env->GetStringUTFChars(mode, nullptr);
    const char* host_text = mode_text ? env->GetStringUTFChars(hostname, nullptr)
                                      : nullptr;
    if (!mode_text || !host_text) {
        if (mode_text) env->ReleaseStringUTFChars(mode, mode_text);
        return JNI_FALSE;
    }
    std::lock_guard<std::mutex> lock(g_input_mutex);
    const bool applied = !g_master_server_change_pending &&
        g_master_server_mode == mode_text && g_master_server_host == host_text;
    env->ReleaseStringUTFChars(mode, mode_text);
    env->ReleaseStringUTFChars(hostname, host_text);
    return applied ? JNI_TRUE : JNI_FALSE;
}

void Native_Start(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lock(g_input_mutex);
    ClearInventoryDoubleTapLocked();
    g_master_server_change_pending = false;
    g_guest_frame_heartbeat_ms.store(MonotonicMilliseconds(),
                                     std::memory_order_release);
    CancelConversationTouchLocked();
    CancelMapTouchLocked();
    ClearMapRouteLocked();
    g_map_time_running = false;
    g_map_time_run_in_place = false;
    g_map_marker_anchor = {};
    g_map_pitch_initialized_window = 0;
    if (!PreserveInventoryRecoveryLocked("controls start")) {
        CancelInventoryTouchLocked();
    }
    CancelCharacterTouchLocked();
    ResetUiTouchLocked();
    ResetGuestSettingsSyncLocked(true);
    g_virtual_held = 0;
    g_discrete_buttons.ResetVirtual();
    CancelMenuInputLocked();
    g_virtual_left_x = 0.0f;
    g_virtual_left_y = 0.0f;
    g_pending_look_x = 0.0f;
    g_pending_look_y = 0.0f;
    g_pending_gyro_look_x = 0.0f;
    g_pending_gyro_look_y = 0.0f;
    g_relative_look_remainder_x = 0.0f;
    g_relative_look_remainder_y = 0.0f;
    g_pending_sheath = false;
    g_pending_open_orders = false;
    g_pending_open_chat = false;
    ReleaseSyntheticChatKeyLocked();
    ResolveInputSymbols();
    LOGI("controls started");
}

void Native_Shutdown(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lock(g_input_mutex);
    ClearInventoryDoubleTapLocked();
    g_guest_frame_heartbeat_ms.store(0, std::memory_order_release);
    CancelConversationTouchLocked();
    CancelMapTouchLocked();
    ClearMapRouteLocked();
    g_map_time_running = false;
    g_map_time_run_in_place = false;
    g_map_marker_anchor = {};
    g_map_pitch_initialized_window = 0;
    if (!PreserveInventoryRecoveryLocked("controls shutdown"))
        CancelInventoryTouchLocked();
    CancelCharacterTouchLocked();
    ResetUiTouchLocked();
    ResetGuestSettingsSyncLocked(true);
    g_virtual_held = 0;
    g_discrete_buttons.ResetVirtual();
    CancelMenuInputLocked();
    g_virtual_left_x = 0.0f;
    g_virtual_left_y = 0.0f;
    g_pending_look_x = 0.0f;
    g_pending_look_y = 0.0f;
    g_pending_gyro_look_x = 0.0f;
    g_pending_gyro_look_y = 0.0f;
    g_relative_look_remainder_x = 0.0f;
    g_relative_look_remainder_y = 0.0f;
    g_pending_sheath = false;
    g_pending_open_orders = false;
    g_pending_open_chat = false;
    ReleaseSyntheticChatKeyLocked();
    g_menu_touch_configured = false;
    g_map_touch_configured = false;
    g_inventory_touch_configured = false;
    LOGI("controls stopped");
}

void Native_ConfigureMenuTouch(JNIEnv*, jclass, jboolean swipe_navigation,
                               jboolean tap_confirm, jboolean shoulder_buttons,
                               jfloat touch_slop, jfloat density,
                               jint surface_width, jint surface_height) {
    const float safe_density = std::clamp(
        std::isfinite(density) ? static_cast<float>(density) : 1.0f, 0.5f, 8.0f);
    const float safe_slop = std::clamp(
        std::isfinite(touch_slop) ? static_cast<float>(touch_slop) : safe_density * 8.0f,
        1.0f, 512.0f);
    const float width = static_cast<float>(std::clamp<jint>(surface_width, 0, 32768));
    const float height = static_cast<float>(std::clamp<jint>(surface_height, 0, 32768));
    warband::input::MenuTouchConfig config{};
    config.swipe_navigation = swipe_navigation == JNI_TRUE;
    config.tap_confirm = tap_confirm == JNI_TRUE;
    config.shoulder_buttons = shoulder_buttons == JNI_TRUE;
    config.touch_slop = safe_slop;
    config.step_distance = std::max(safe_slop * 1.5f, safe_density * 18.0f);
    config.shoulder_size = std::max(safe_slop * 2.0f, safe_density * 48.0f);
    config.shoulder_tap_slop = std::max(safe_slop, safe_density * 12.0f);
    config.surface_width = width;
    config.surface_height = height;

    warband::input::MapTouchConfig map_config{};
    map_config.touch_slop = safe_slop;
    map_config.edge_dead_zone_fraction = 0.055f;
    map_config.bottom_control_exclusion_fraction = 0.12f;
    map_config.pinch_slop = std::max(safe_slop, safe_density * 12.0f);
    map_config.rotation_slop_radians = 0.055f;
    map_config.tilt_slop = std::max(safe_slop * 2.0f, safe_density * 24.0f);
    map_config.density = safe_density;
    map_config.surface_width = width;
    map_config.surface_height = height;

    std::lock_guard<std::mutex> lock(g_input_mutex);
    if (g_menu_touch_configured && SameMenuTouchConfig(config, g_menu_touch_config) &&
        g_map_touch_configured && SameMapTouchConfig(map_config, g_map_touch_config)) {
        return;
    }
    CancelConversationTouchLocked();
    CancelMapTouchLocked();
    ClearInventoryDoubleTapLocked();
    if (!PreserveInventoryRecoveryLocked("touch reconfigure")) {
        CancelInventoryTouchLocked();
    }
    CancelCharacterTouchLocked();
    ResetUiTouchLocked();
    CancelMenuInputLocked();
    g_menu_touch_config = config;
    g_menu_touch.Configure(config);
    g_menu_touch_configured = true;
    g_map_touch_config = map_config;
    g_map_touch.Configure(map_config);
    g_map_touch_configured = true;
    g_inventory_touch_configured = true;
    LOGI("menu touch config swipe=%d tap=%d shoulders=%d slop=%.1f step=%.1f "
         "surface=%.0fx%.0f",
         config.swipe_navigation, config.tap_confirm, config.shoulder_buttons,
         config.touch_slop, config.step_distance, width, height);
}

jintArray Native_ExchangeGuestSettings(JNIEnv* env, jclass,
                                       jboolean gyro_enabled,
                                       jboolean gyro_invert_y,
                                       jboolean menu_swipe,
                                       jboolean menu_shoulders,
                                       jboolean left_handed,
                                       jboolean camera_mouse,
                                       jboolean always_run,
                                       jboolean hdr_enabled,
                                       jboolean dof_enabled,
                                       jboolean auto_exposure,
                                       jboolean haptic_feedback,
                                       jboolean menu_aspect,
                                       jboolean fxaa_enabled,
                                       jboolean final_poll,
                                       jfloat move_sensitivity,
                                       jfloat gyro_sensitivity,
                                       jfloat gyro_deadzone,
                                       jfloat button_scale,
                                       jfloat button_opacity,
                                       jfloat fps_limit,
                                       jfloat hdr_brightness,
                                       jfloat shadow_mode,
                                       jfloat map_pan_sensitivity,
                                       jfloat report_text_scale) {
    if (!env) return nullptr;
    warband::guest_settings::Values proposed{};
    proposed.bool_mask = 0u;
    if (gyro_enabled == JNI_TRUE) proposed.bool_mask |= GST_BOOL_GYRO_ENABLED;
    if (gyro_invert_y == JNI_TRUE) proposed.bool_mask |= GST_BOOL_GYRO_INVERT_Y;
    if (menu_swipe == JNI_TRUE) proposed.bool_mask |= GST_BOOL_MENU_SWIPE;
    if (menu_shoulders == JNI_TRUE) proposed.bool_mask |= GST_BOOL_GYRO_RANGED_ONLY;
    if (left_handed == JNI_TRUE) proposed.bool_mask |= GST_BOOL_LEFT_HANDED;
    if (camera_mouse == JNI_TRUE) proposed.bool_mask |= GST_BOOL_CAMERA_MOUSE;
    if (always_run == JNI_TRUE) proposed.bool_mask |= GST_BOOL_ALWAYS_RUN;
    if (hdr_enabled == JNI_TRUE) proposed.bool_mask |= GST_BOOL_HDR_ENABLED;
    // DOF is retired; the JNI slot remains for the fixed Java bridge ABI.
    // if (dof_enabled == JNI_TRUE) proposed.bool_mask |= GST_BOOL_DOF_ENABLED;
    (void)dof_enabled;
    if (auto_exposure == JNI_TRUE) proposed.bool_mask |= GST_BOOL_AUTO_EXPOSURE;
    if (haptic_feedback == JNI_TRUE) proposed.bool_mask |= GST_BOOL_HAPTIC_FEEDBACK;
    if (menu_aspect == JNI_TRUE) proposed.bool_mask |= GST_BOOL_MENU_ASPECT;
    if (fxaa_enabled == JNI_TRUE) proposed.bool_mask |= GST_BOOL_FXAA_ENABLED;
    proposed.floats = {
        static_cast<float>(move_sensitivity),
        static_cast<float>(gyro_sensitivity),
        static_cast<float>(gyro_deadzone),
        static_cast<float>(button_scale),
        static_cast<float>(button_opacity),
        static_cast<float>(fps_limit),
        static_cast<float>(hdr_brightness),
        static_cast<float>(shadow_mode),
        static_cast<float>(map_pan_sensitivity),
        static_cast<float>(report_text_scale),
    };
    proposed = warband::guest_settings::BoundedHostValues(proposed);

    warband::guest_settings::Values pending{};
    warband::guest_settings::Values frame_limit_values = proposed;
    uint64_t pending_serial = 0;
    bool have_pending = false;
    {
        std::lock_guard<std::mutex> lock(g_input_mutex);
        if (!g_guest_settings_host_configured ||
            !warband::guest_settings::SameValues(
                proposed, g_guest_settings_host_values)) {
            g_guest_settings_host_values = proposed;
            g_guest_settings_host_configured = true;
            ++g_guest_settings_host_generation;
            if (!g_guest_settings_host_generation) {
                ++g_guest_settings_host_generation;
            }
        }
        if (final_poll == JNI_TRUE) {
            if (!g_guest_settings_pending_valid &&
                g_guest_settings_custom_session &&
                g_guest_settings_last_snapshot_valid) {
                QueueGuestSettingsCommitLocked(g_guest_settings_last_snapshot);
            }
            g_guest_settings_custom_session = false;
            g_guest_settings_last_snapshot_valid = false;
        }
        if (g_guest_settings_pending_valid) {
            pending = g_guest_settings_pending_snapshot;
            frame_limit_values = pending;
            pending_serial = g_guest_settings_pending_serial;
            have_pending = true;
        } else if (g_guest_settings_custom_session &&
                   g_guest_settings_last_snapshot_valid) {
            // Preferences are committed when the custom tab closes. While it
            // remains open, use its latest guest snapshot instead of repeatedly
            // restoring the stale Android preference on every UI poll.
            frame_limit_values = g_guest_settings_last_snapshot;
        }
    }
    SetFrameLimitFromGuestSettings(frame_limit_values);
    bridge_gl::SetHdrBrightness(
        frame_limit_values.floats[GST_FLOAT_HDR_BRIGHTNESS]);
    bridge_gl::SetFxaaEnabled(
        (frame_limit_values.bool_mask & GST_BOOL_FXAA_ENABLED) != 0u);
    if (!have_pending) return nullptr;

    std::array<jint, 1 + GST_FLOAT_COUNT> encoded{};
    encoded[0] = static_cast<jint>(pending.bool_mask);
    for (std::size_t index = 0; index < pending.floats.size(); ++index) {
        const uint32_t bits = GuestSettingsFloatBits(pending.floats[index]);
        static_assert(sizeof(bits) == sizeof(encoded[index + 1]));
        std::memcpy(&encoded[index + 1], &bits, sizeof(bits));
    }
    jintArray result = env->NewIntArray(static_cast<jsize>(encoded.size()));
    if (!result) return nullptr;
    env->SetIntArrayRegion(result, 0, static_cast<jsize>(encoded.size()), encoded.data());
    if (env->ExceptionCheck()) return nullptr;

    {
        std::lock_guard<std::mutex> lock(g_input_mutex);
        if (g_guest_settings_pending_valid &&
            g_guest_settings_pending_serial == pending_serial) {
            // A guest-options commit becomes the live native setting too.
            // The Java host proposal on this final poll still contains the
            // pre-edit preference values, so adopt the acknowledged guest
            // snapshot before clearing the pending commit.
            const warband::guest_settings::Values committed =
                warband::guest_settings::BoundedHostValues(pending);
            if (!g_guest_settings_host_configured ||
                !warband::guest_settings::SameValues(
                    committed, g_guest_settings_host_values)) {
                g_guest_settings_host_values = committed;
                g_guest_settings_host_configured = true;
                ++g_guest_settings_host_generation;
                if (!g_guest_settings_host_generation) {
                    ++g_guest_settings_host_generation;
                }
            }
            g_guest_settings_pending_valid = false;
        }
    }
    return result;
}

jboolean Native_Ready(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lock(g_input_mutex);
    ResolveInputSymbols();
    return GuestKeys() && g_add_event ? JNI_TRUE : JNI_FALSE;
}

jboolean Native_MissionActive(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lock(g_input_mutex);
    ResolveInputSymbols();
    if (!g_host || !g_mission_slot || !g_game_screen ||
        !GuestAddressRange(*g_host, g_mission_slot, sizeof(uint32_t))) {
        return RememberedGameplayState();
    }
    const uint32_t mission = g_host->MemoryRead32(g_mission_slot);
    if (!mission) return RememberGameplayState(false);
    if (mission > std::numeric_limits<uint32_t>::max() - 0x78u ||
        !GuestAddressRange(*g_host, mission + 0x78u, sizeof(uint32_t))) {
        return RememberedGameplayState();
    }
    // A spectator has no assigned player agent, but still owns tactical UI.
    // A mission object also remains alive under Escape/Options/book windows;
    // only the tactical window (id 10) may capture those overlay touches.
    uint32_t active_window = 0;
    if (!ReadStableActiveWindowId(*g_host, g_game_screen, &active_window)) {
        // Guest execution is concurrent with this JNI poll. An unstable snapshot is unknown,
        // not proof that gameplay ended; retain the last stable state to avoid UI flicker.
        return RememberedGameplayState();
    }
    return RememberGameplayState(active_window == kTacticalWindowId);
}

jboolean Native_MissionSessionActive(JNIEnv* env, jclass klass) {
    // Share the stable window gate before spawning and after leaving a mission.
    // Non-tactical windows keep NativeActivity's input queue.
    return Native_MissionActive(env, klass);
}

jint Native_ActiveMenuWindowId(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lock(g_input_mutex);
    ResolveInputSymbols();
    if (!g_host || !g_game_screen) return -1;
    uint32_t window_id = warband::input::kUnknownMenuWindowId;
    if (!ReadStableActiveWindowId(*g_host, g_game_screen, &window_id)) return -1;
    if (window_id == warband::input::kUnknownMenuWindowId) return -1;
    static uint32_t logged_window_id = std::numeric_limits<uint32_t>::max();
    if (logged_window_id != window_id) {
        logged_window_id = window_id;
        LOGI("active_window=%u", window_id);
    }
    return static_cast<jint>(window_id);
}

jboolean Native_MapTimeRunning(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lock(g_input_mutex);
    return g_map_time_running ? JNI_TRUE : JNI_FALSE;
}

jboolean Native_ToggleMapTime(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lock(g_input_mutex);
    if (!g_host || !g_game_screen) return JNI_FALSE;
    uint32_t window_id = kNoActiveWindowId;
    if (!ReadStableActiveWindowId(*g_host, g_game_screen, &window_id) ||
        window_id != kMapWindowId || !ResolveCurrentGameObjectLocked()) {
        return g_map_time_running ? JNI_TRUE : JNI_FALSE;
    }
    g_map_time_running = !g_map_time_running;
    g_map_time_run_in_place = false;
    if (g_map_time_running) {
        const uint32_t current_game = ResolveCurrentGameObjectLocked();
        const uint32_t party = ResolvePlayerPartyLocked(current_game);
        g_map_time_run_in_place = !party || !MapPartyIsTravelling(
            ReadMapRouteLocked(current_game, party).behavior[0]) ||
            MapPartyReachedDestinationLocked(party);
    }
    LOGI("map time %s", g_map_time_running ? "running" : "paused");
    return g_map_time_running ? JNI_TRUE : JNI_FALSE;
}

jint Native_ActivePresentationId(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lock(g_input_mutex);
    ResolveInputSymbols();
    if (!g_host || !g_cur_presentation_container ||
        !GuestAddressRange(*g_host, g_cur_presentation_container, sizeof(uint32_t))) {
        return -1;
    }

    // The guest updates this pointer when a presentation container is created
    // or cleared. Read it twice around the container's stable +0x0C table index
    // so a concurrent window transition cannot be mistaken for the MOTD.
    const uint32_t container_before =
        g_host->MemoryRead32(g_cur_presentation_container);
    if (!container_before || (container_before & (alignof(uint32_t) - 1u)) != 0 ||
        container_before > std::numeric_limits<uint32_t>::max() - 0x10u ||
        !GuestAddressRange(*g_host, container_before, 0x10u)) {
        return -1;
    }
    const uint32_t id_before = g_host->MemoryRead32(container_before + 0x0Cu);
    const uint32_t container_after =
        g_host->MemoryRead32(g_cur_presentation_container);
    if (container_before != container_after ||
        g_host->MemoryRead32(container_before + 0x0Cu) != id_before ||
        id_before > static_cast<uint32_t>(std::numeric_limits<jint>::max())) {
        return -1;
    }
    static uint32_t logged_presentation_id =
        std::numeric_limits<uint32_t>::max();
    if (logged_presentation_id != id_before) {
        logged_presentation_id = id_before;
        LOGI("active_presentation=%u", id_before);
    }
    return static_cast<jint>(id_before);
}

bool ReadTacticalPresentationsLocked(uint32_t* begin, uint32_t* end) {
    uint32_t window_id = kNoActiveWindowId, tactical = 0;
    if (!begin || !end || !g_host || !g_game_screen ||
        !ReadStableActiveWindowId(*g_host, g_game_screen, &window_id) ||
        window_id != kTacticalWindowId ||
        !ResolveActiveWindowObjectLocked(window_id, &tactical) ||
        !GuestAddressRange(*g_host, static_cast<uint64_t>(tactical) + 0x1BCu, 8u))
        return false;
    *begin = g_host->MemoryRead32(tactical + 0x1BCu);
    *end = g_host->MemoryRead32(tactical + 0x1C0u);
    return *end >= *begin && ((*end - *begin) & 3u) == 0u &&
        *end - *begin <= 1024u &&
        GuestAddressRange(*g_host, *begin, *end - *begin);
}

uint32_t TacticalModalStateLocked() {
    uint32_t begin = 0, end = 0;
    if (!ReadTacticalPresentationsLocked(&begin, &end)) return 0;
    const uint32_t current_game = g_cur_game_slot &&
        GuestAddressRange(*g_host, g_cur_game_slot, 4u)
        ? g_host->MemoryRead32(g_cur_game_slot) : 0u;
    if (ValidGuestPointerLocked(current_game) && GuestAddressRange(
            *g_host, static_cast<uint64_t>(current_game) + 0x1936Cu, 4u) &&
        g_host->MemoryRead32(current_game + 0x1936Cu) != 0u) return 1u;
    for (uint32_t address = begin; address < end; address += 4u) {
        const uint32_t container = g_host->MemoryRead32(address);
        if (ValidGuestPointerLocked(container) && GuestAddressRange(
                *g_host, static_cast<uint64_t>(container) + 0x10u, 4u) &&
            (g_host->MemoryRead32(container + 0x10u) & 1u) == 0u) return 2u;
    }
    return 0u;
}

bool TacticalUiRootIsLiveLocked(uint32_t container, uint32_t root) {
    uint32_t begin = 0, end = 0;
    if (!ReadTacticalPresentationsLocked(&begin, &end) ||
        !ValidGuestPointerLocked(container) ||
        !GuestAddressRange(*g_host, static_cast<uint64_t>(container) + 0x2Cu, 4u) ||
        g_host->MemoryRead32(container + 0x2Cu) != root) return false;
    for (uint32_t address = begin; address < end; address += 4u)
        if (g_host->MemoryRead32(address) == container) return true;
    return false;
}

int TacticalUiWidgetHitLocked(uint32_t widget, bool root, int depth,
        const warband::input::WidgetCoordinateTransform& transform,
        float x, float y, int kind, size_t* visited) {
    if (!visited || depth > 12 || ++*visited > kUiTouchTargetLimit) return 0;
    warband::input::TouchTargetRect rect{};
    if (!ReadVisibleWidgetScreenRectLocked(widget, transform, 0u, &rect, 1u) ||
        !warband::input::TouchTargetContains(rect, x, y)) return 0;
    const uint32_t flags = g_host->MemoryRead32(widget + kWidgetFlagsOffset);
    if ((flags & 2u) != 0u) return 2;
    // A presentation's default root spans the screen even for a tiny HUD.
    // Its real children, including backgrounds, define the touch-owned area.
    const bool fullscreen = rect.left <= 1.0f && rect.top <= 1.0f &&
        rect.right >= transform.surface_width - 1.0f &&
        rect.bottom >= transform.surface_height - 1.0f;
    if (!root && (!fullscreen || !IsGuestContainerWidgetLocked(widget)))
        return kind;
    if (!IsGuestContainerWidgetLocked(widget)) return 0;
    if (!GuestAddressRange(*g_host, static_cast<uint64_t>(widget) +
                           kWidgetChildrenBeginOffset, 8u)) return 0;
    const uint32_t begin = g_host->MemoryRead32(widget + kWidgetChildrenBeginOffset);
    const uint32_t end = g_host->MemoryRead32(widget + kWidgetChildrenEndOffset);
    if (end < begin || ((end - begin) & 3u) != 0u ||
        end - begin > kUiTouchTargetLimit * 4u ||
        !GuestAddressRange(*g_host, begin, end - begin)) return 0;
    for (uint32_t address = end; address > begin;) {
        address -= 4u;
        const int hit = TacticalUiWidgetHitLocked(g_host->MemoryRead32(address),
            false, depth + 1, transform, x, y, kind, visited);
        if (hit != 0) return hit;
    }
    return 0;
}

int ResolveTacticalUiHitLocked(float x, float y,
                               uint32_t* container_out, uint32_t* root_out) {
    uint32_t begin = 0, end = 0;
    warband::input::WidgetCoordinateTransform transform{};
    if (!std::isfinite(x) || !std::isfinite(y) ||
        !ReadTacticalPresentationsLocked(&begin, &end) ||
        !ReadCharacterCoordinateTransformLocked(&transform)) return 0;
    for (uint32_t address = end; address > begin;) {
        address -= 4u;
        const uint32_t container = g_host->MemoryRead32(address);
        if (!ValidGuestPointerLocked(container) || !GuestAddressRange(
                *g_host, static_cast<uint64_t>(container) + 0x10u, 0x20u)) continue;
        const uint32_t root = g_host->MemoryRead32(container + 0x2Cu);
        const int kind = (g_host->MemoryRead32(container + 0x10u) & 1u) ? 2 : 1;
        size_t visited = 0;
        const int hit = TacticalUiWidgetHitLocked(root, true, 0, transform,
                                                  x, y, kind, &visited);
        if (hit != 0) {
            if (container_out) *container_out = container;
            if (root_out) *root_out = root;
            return hit;
        }
    }
    return 0;
}

bool DispatchTacticalWidgetEventLocked(uint32_t widget, uint32_t event,
                                       uint32_t virtual_offset = 0xACu) {
    if (!ValidGuestPointerLocked(widget) ||
        !GuestAddressRange(*g_host, widget, 4u)) return false;
    const uint32_t vtable = g_host->MemoryRead32(widget);
    if (!ValidGuestPointerLocked(vtable) || !GuestAddressRange(
            *g_host, static_cast<uint64_t>(vtable) + virtual_offset, 4u)) return false;
    const uint32_t function = g_host->MemoryRead32(vtable + virtual_offset);
    if (!ValidGuestPointerLocked(function) ||
        !GuestAddressRange(*g_host, function & ~1u, 4u)) return false;
    return g_host->CallGuestNested(function, {widget, event}) != 0u;
}

void DrainTacticalUiTouchLocked() {
    auto& touch = g_tactical_ui_touch;
    if (touch.pointer_id < 0) return;
    if (!TacticalUiRootIsLiveLocked(touch.container, touch.root)) {
        touch = {};
        return;
    }
    uint32_t unused_x = 0, unused_y = 0;
    if (!SetInventoryGuestCursorForTouchLocked(touch.x, touch.y, &unused_x, &unused_y))
        return;
    // Keep this UI pointer's cursor between frames. WM::tick sends MOVE before
    // the next drain; restoring the old cursor would send LEAVE to the pressed
    // button and cancel its UP activation. Its optional touchscreen refresh
    // reads these pixel coordinates, so both cursor sources must agree. Keep
    // their history equal too: this UI pointer must not create mouse deltas.
    const uint32_t keys = GuestKeys();
    if (keys && GuestAddressRange(*g_host, keys + 0xC18u, 16u)) {
        const uint32_t pixel_x = static_cast<uint32_t>(std::clamp<double>(
            touch.x, 0.0, g_host->MemoryRead32(
                g_window_manager + kWindowManagerSurfaceWidthOffset)));
        const uint32_t pixel_y = static_cast<uint32_t>(std::clamp<double>(
            touch.y, 0.0, g_host->MemoryRead32(
                g_window_manager + kWindowManagerSurfaceHeightOffset)));
        g_host->MemoryWrite32(keys + 0xC18u, pixel_x);
        g_host->MemoryWrite32(keys + 0xC1Cu, pixel_y);
        g_host->MemoryWrite32(keys + 0xC20u, pixel_x);
        g_host->MemoryWrite32(keys + 0xC24u, pixel_y);
    }
    if (touch.down_pending && !touch.cancelled) {
        const uint32_t previous_focus = g_host->MemoryRead32(g_window_manager + 0x0Cu);
        SetInventoryGuestCursorForTouchLocked(touch.down_x, touch.down_y,
                                               &unused_x, &unused_y);
        DispatchTacticalWidgetEventLocked(touch.root, 1u);
        touch.semantic_consumed |= DispatchTacticalWidgetEventLocked(touch.root, 16u);
        touch.down_pending = false;
        const uint32_t new_focus = g_host->MemoryRead32(g_window_manager + 0x0Cu);
        touch.focused = new_focus != previous_focus ? new_focus : 0u;
        SetInventoryGuestCursorForTouchLocked(touch.x, touch.y,
                                               &unused_x, &unused_y);
    }
    if (!TacticalUiRootIsLiveLocked(touch.container, touch.root)) {
        touch = {};
        return;
    }
    const bool owns_focus = touch.focused != 0u &&
        g_host->MemoryRead32(g_window_manager + 0x0Cu) == touch.focused;
    if (!touch.cancelled)
        DispatchTacticalWidgetEventLocked(owns_focus ? touch.focused : touch.root, 1u);
    if (touch.release_pending) {
        const bool handled = !touch.cancelled && DispatchTacticalWidgetEventLocked(
            owns_focus ? touch.focused : touch.root, 8u);
        touch.semantic_consumed |= handled;
        uint32_t hit_container = 0, hit_root = 0;
        if (!touch.cancelled && !handled && !touch.semantic_consumed &&
            TacticalUiRootIsLiveLocked(touch.container, touch.root) &&
            g_host->MemoryRead32(touch.container + 0x0Cu) == 4u &&
            ResolveTacticalUiHitLocked(touch.x, touch.y, &hit_container, &hit_root) == 1 &&
            hit_container == touch.container && hit_root == touch.root &&
            std::hypot(touch.x - touch.down_x, touch.y - touch.down_y) <=
                g_menu_touch_config.touch_slop) {
            g_pending_welcome_enter_character = true;
            g_orders_pulses.Enqueue({warband::input::kEnterConfirm, 1u},
                                     MonotonicMilliseconds());
        }
        if (TacticalUiRootIsLiveLocked(touch.container, touch.root))
            DispatchTacticalWidgetEventLocked(touch.root, 4u);
        if (touch.cancelled && owns_focus &&
            TacticalUiRootIsLiveLocked(touch.container, touch.root) &&
            g_host->MemoryRead32(g_window_manager + 0x0Cu) == touch.focused)
            DispatchTacticalWidgetEventLocked(touch.focused, 0u, 0xB4u);
        touch = {};
    }
}

jint Native_TacticalUiHit(JNIEnv*, jclass, jfloat x, jfloat y) {
    std::lock_guard<std::mutex> lock(g_input_mutex);
    ResolveInputSymbols();
    return ResolveTacticalUiHitLocked(x, y, nullptr, nullptr);
}

jboolean Native_TacticalUiTouch(JNIEnv*, jclass, jint action, jint pointer_id,
                               jfloat x, jfloat y, jlong event_time) {
    (void)event_time;
    if (pointer_id < 0 || !std::isfinite(x) || !std::isfinite(y) ||
        action < 0 || action > 3) return JNI_FALSE;
    std::lock_guard<std::mutex> lock(g_input_mutex);
    auto& touch = g_tactical_ui_touch;
    if (action == 0) {
        if (touch.pointer_id >= 0 || ResolveTacticalUiHitLocked(
                x, y, &touch.container, &touch.root) != 1) return JNI_FALSE;
        touch.pointer_id = pointer_id;
        touch.down_x = x;
        touch.down_y = y;
        touch.down_pending = true;
    } else if (touch.pointer_id != pointer_id || touch.release_pending) {
        return JNI_FALSE;
    }
    touch.x = x;
    touch.y = y;
    touch.release_pending = action == 1 || action == 3;
    touch.cancelled = action == 3;
    return JNI_TRUE;
}

jint Native_TacticalModalState(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lock(g_input_mutex);
    ResolveInputSymbols();
    return static_cast<jint>(TacticalModalStateLocked());
}

jint Native_ConsumeInputToast(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lock(g_input_mutex);
    const uint32_t result = g_pending_input_toast;
    g_pending_input_toast = 0u;
    return static_cast<jint>(result);
}

jint Native_MissionButtonStates(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lock(g_input_mutex);
    ResolveInputSymbols();
    if (!g_host || !g_game_screen) return 0;
    using namespace warband::input::mission_buttons;
    const GuestMemoryView memory{
        g_host.get(),
        [](void* context, uint64_t address, uint64_t bytes) {
            const auto* host = static_cast<const DynarmicHost*>(context);
            return host && GuestAddressRange(*host, address, bytes);
        },
        [](void* context, uint32_t address) {
            auto* host = static_cast<DynarmicHost*>(context);
            return host ? host->MemoryRead32(address) : 0u;
        },
    };

    bool context_action = false;
    const uint32_t window_slot = g_game_screen + 4u * (1u + kTacticalWindowId);
    if (GuestAddressRange(*g_host, window_slot, sizeof(uint32_t))) {
        const uint32_t tactical = g_host->MemoryRead32(window_slot);
        uint32_t action_code = 0;
        uint32_t can_perform = 0;
        if (ValidGuestPointerLocked(tactical) &&
            memory.contains(memory.context, static_cast<uint64_t>(tactical) + 0x4A0u,
                            sizeof(uint32_t)) &&
            memory.contains(memory.context, static_cast<uint64_t>(tactical) + 0x4ACu,
                            sizeof(uint32_t))) {
            action_code = memory.read32(memory.context, tactical + 0x4A0u);
            can_perform = memory.read32(memory.context, tactical + 0x4ACu);
            context_action = ContextActionAvailable(
                action_code == 0xFFFFFFFFu ? -1 : static_cast<int32_t>(action_code),
                can_perform);
        }
    }

    bool has_wielded_item = false;
    bool has_commandable_ally = false;
    bool can_cycle_weapon = false;
    bool can_cycle_shield = false;
    bool player_mounted = false;
    const uint32_t mission = GuestAddressRange(
            *g_host, g_mission_slot, sizeof(uint32_t))
        ? g_host->MemoryRead32(g_mission_slot) : 0u;
    const uint32_t player_agent = mission ? ResolvePlayerAgentLocked() : 0u;
    if (mission && player_agent) {
        has_wielded_item = HasWieldedItem(memory, player_agent);
        player_mounted = PlayerMounted(memory, player_agent);
        uint32_t player_agent_id_word = 0;
        if (ReadWord(memory, static_cast<uint64_t>(mission) +
                              kMissionPlayerAgentOffset, &player_agent_id_word)) {
            has_commandable_ally = HasFriendlyOrderTarget(
                memory, mission, player_agent,
                static_cast<int32_t>(player_agent_id_word));
        }

        ElfLoader* game = g_host->GetLibrary("libMBExpMobile.so");
        if (game) {
            const uint64_t item_kinds_address =
                static_cast<uint64_t>(game->GetBaseAddress()) +
                kItemKindsPtrImageOffset;
            uint32_t item_kinds = 0;
            if (ReadWord(memory, item_kinds_address, &item_kinds) && item_kinds) {
                can_cycle_weapon = CanCycleWeapon(memory, player_agent,
                                                  item_kinds, 0);
                can_cycle_shield = CanCycleWeapon(memory, player_agent,
                                                  item_kinds, 1);
            }
        }
    }

    return static_cast<jint>(BuildStateMask(
        context_action, has_wielded_item, has_commandable_ally,
        can_cycle_weapon, can_cycle_shield, has_wielded_item,
        player_mounted) | (CurrentMissionIsMultiplayerLocked() ? 1u << 7 : 0u));
}

jboolean Native_GuestFrameResponsive(JNIEnv*, jclass) {
    const uint64_t last_ms =
        g_guest_frame_heartbeat_ms.load(std::memory_order_acquire);
    if (!last_ms) return JNI_FALSE;
    const uint64_t now_ms = MonotonicMilliseconds();
    if (now_ms < last_ms) return JNI_FALSE;
    return now_ms - last_ms <= kGuestFrameResponsiveWindowMs
               ? JNI_TRUE
               : JNI_FALSE;
}

jboolean Native_TapOrders(JNIEnv*, jclass, jfloat x, jfloat y) {
    std::lock_guard<std::mutex> lock(g_input_mutex);
    ResolveInputSymbols();
    if (g_orders_tap_pending) return JNI_FALSE;
    uint32_t tactical = 0;
    uint32_t row = 0;
    if (!ResolveOrdersRowAtPointLocked(x, y, &tactical, &row)) {
        return JNI_FALSE;
    }
    // Resolve on the UI/JNI thread, then validate and publish the selection
    // from the guest frame hook.  Guest state is never mutated concurrently
    // with Dynarmic execution.
    g_orders_tap_pending = true;
    g_orders_tap_pending_tactical = tactical;
    g_orders_tap_pending_row = row;
    return JNI_TRUE;
}

jboolean Native_RangedWeaponHeld(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lock(g_input_mutex);
    if (!g_host || !g_mission_slot) return JNI_FALSE;
    ElfLoader* game = g_host->GetLibrary("libMBExpMobile.so");
    if (!game) return JNI_FALSE;
    const uint32_t agent = ResolvePlayerAgentLocked();
    if (!agent) return JNI_FALSE;
    const uint32_t item_kinds_ptr_addr =
        game->GetBaseAddress() + kItemKindsPtrImageOffset;
    if (!GuestAddressRange(*g_host, item_kinds_ptr_addr, sizeof(uint32_t))) {
        return JNI_FALSE;
    }
    const uint32_t item_kinds_ptr = g_host->MemoryRead32(item_kinds_ptr_addr);
    if (!item_kinds_ptr) return JNI_FALSE;
    if (!GuestAddressRange(*g_host, item_kinds_ptr, 64u * kItemStride)) {
        return JNI_FALSE;
    }
    for (uint32_t hand = 0u; hand < 2u; ++hand) {
        const uint32_t slot_addr = agent + kAgentWieldedSlotOffset + 4u * hand;
        if (!GuestAddressRange(*g_host, slot_addr, sizeof(uint32_t))) continue;
        const int32_t slot =
            static_cast<int32_t>(g_host->MemoryRead32(slot_addr));
        if (slot < 0 || slot > 15u) continue;
        const uint32_t id_addr =
            agent + kAgentWieldedItemIdOffset + static_cast<uint32_t>(slot) * 8u;
        if (!GuestAddressRange(*g_host, id_addr, sizeof(uint32_t))) continue;
        const int32_t item_id = static_cast<int32_t>(g_host->MemoryRead32(id_addr));
        if (item_id < 0) continue;
        const uint32_t flags_addr = item_kinds_ptr +
            kItemStride * static_cast<uint32_t>(item_id) + kItemTypeFlagsOffset;
        if (!GuestAddressRange(*g_host, flags_addr, sizeof(uint32_t))) continue;
        const uint32_t type = g_host->MemoryRead32(flags_addr) & 0xFFu;
        // 8 = bow, 9 = crossbow, 0xA = thrown (itp_type_* low bits)
        if (type == 8u || type == 9u || type == 0xAu) return JNI_TRUE;
    }
    return JNI_FALSE;
}

void Native_SetOverlayEditMode(JNIEnv*, jclass, jboolean active) {
    std::lock_guard<std::mutex> lock(g_input_mutex);
    g_overlay_edit_mode = active == JNI_TRUE;
}

void Native_SetMoveAxes(JNIEnv*, jclass, jfloat left_x, jfloat left_y) {
    std::lock_guard<std::mutex> lock(g_input_mutex);
    g_virtual_left_x = ClampUnit(left_x);
    g_virtual_left_y = ClampUnit(-left_y);
}

void Native_AddGyroLookDelta(JNIEnv*, jclass, jfloat yaw_delta, jfloat pitch_delta) {
    std::lock_guard<std::mutex> lock(g_input_mutex);
    if (!std::isfinite(yaw_delta) || !std::isfinite(pitch_delta)) return;
    g_pending_gyro_look_x = std::clamp(g_pending_gyro_look_x + yaw_delta,
        -kMaxInjectedLookPerFrame, kMaxInjectedLookPerFrame);
    g_pending_gyro_look_y = std::clamp(g_pending_gyro_look_y + pitch_delta,
        -kMaxInjectedLookPerFrame, kMaxInjectedLookPerFrame);
}

void AddPendingLookDeltaLocked(float yaw_delta, float pitch_delta) {
    if (!std::isfinite(yaw_delta) || !std::isfinite(pitch_delta)) return;
    g_pending_look_x = std::clamp(g_pending_look_x + yaw_delta,
                                  -kMaxInjectedLookPerFrame, kMaxInjectedLookPerFrame);
    g_pending_look_y = std::clamp(g_pending_look_y + pitch_delta,
                                  -kMaxInjectedLookPerFrame, kMaxInjectedLookPerFrame);
}

void Native_AddLookDelta(JNIEnv*, jclass, jfloat yaw_delta, jfloat pitch_delta) {
    std::lock_guard<std::mutex> lock(g_input_mutex);
    AddPendingLookDeltaLocked(static_cast<float>(yaw_delta),
                              static_cast<float>(pitch_delta));
}

void Native_BeginDirectionalAction(JNIEnv*, jclass, jint action,
                                   jfloat yaw_delta, jfloat pitch_delta) {
    std::lock_guard<std::mutex> lock(g_input_mutex);
    if (action != 1 && action != 2) return;
    AddPendingLookDeltaLocked(static_cast<float>(yaw_delta),
                              static_cast<float>(pitch_delta));
    SetVirtualTriggerHeld(action == 1 ? kHeldRt : kHeldLt, true);
}

void Native_SetAction(JNIEnv*, jclass, jint action, jboolean down) {
    std::lock_guard<std::mutex> lock(g_input_mutex);
    const bool pressed = down == JNI_TRUE;
    switch (action) {
        case 1: SetVirtualTriggerHeld(kHeldRt, pressed); break;
        case 2: SetVirtualTriggerHeld(kHeldLt, pressed); break;
        case 3: g_discrete_buttons.SetVirtual(warband::input::kButtonY, pressed); break;
        case 4: g_discrete_buttons.SetVirtual(warband::input::kButtonA, pressed); break;
        case 5: g_discrete_buttons.SetVirtual(warband::input::kButtonX, pressed); break;
        case 6: g_discrete_buttons.SetVirtual(warband::input::kButtonStart, pressed); break;
        case 7: g_discrete_buttons.SetVirtual(warband::input::kDpadUp, pressed); break;
        case 8: g_discrete_buttons.SetVirtual(warband::input::kDpadDown, pressed); break;
        // Proven in pristine guest android_handle_input (0x2549D0): Android L1/R1
        // keycodes 102/103 dispatch the 20/21 and 22/23 event pairs respectively.
        case 9: g_discrete_buttons.SetVirtual(warband::input::kButtonL1, pressed); break;
        case 10: g_discrete_buttons.SetVirtual(warband::input::kButtonR1, pressed); break;
        case 11: g_discrete_buttons.SetVirtual(warband::input::kButtonB, pressed); break;
        case 12: g_discrete_buttons.SetVirtual(warband::input::kThumbRight, pressed); break;
        case 13: g_discrete_buttons.SetVirtual(warband::input::kThumbLeft, pressed); break;
        case 14: g_discrete_buttons.SetVirtual(warband::input::kButtonBack, pressed); break;
        case 15:
            if (pressed) g_pending_sheath = true;
            break;
        case 16:
            if (pressed) g_pending_open_orders = true;
            break;
        case 17:
            if (pressed) {
                g_pending_welcome_enter_character = true;
                LOGI("MOTD tap received native Enter down");
            }
            g_discrete_buttons.SetVirtual(warband::input::kEnterConfirm, pressed);
            break;
        case 18:
            if (pressed) g_pending_open_chat = true;
            break;
        default: break;
    }
}

void Native_Reset(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lock(g_input_mutex);
    ClearInventoryDoubleTapLocked();
    if (g_tactical_ui_touch.pointer_id >= 0) {
        g_tactical_ui_touch.cancelled = true;
        g_tactical_ui_touch.release_pending = true;
    }
    CancelConversationTouchLocked();
    CancelMapTouchLocked();
    if (!PreserveInventoryRecoveryLocked("controls reset")) {
        CancelInventoryTouchLocked();
    }
    CancelCharacterTouchLocked();
    ResetUiTouchLocked();
    CancelMenuInputLocked();
    g_virtual_left_x = 0.0f;
    g_virtual_left_y = 0.0f;
    g_pending_look_x = 0.0f;
    g_pending_look_y = 0.0f;
    g_pending_gyro_look_x = 0.0f;
    g_pending_gyro_look_y = 0.0f;
    g_relative_look_remainder_x = 0.0f;
    g_relative_look_remainder_y = 0.0f;
    g_pending_sheath = false;
    g_pending_open_orders = false;
    g_pending_welcome_enter_character = false;
    g_pending_open_chat = false;
    ReleaseSyntheticChatKeyLocked();
    g_virtual_held = 0;
    g_discrete_buttons.ResetVirtual();
}

static_assert(sizeof(jlong) == sizeof(uint64_t));

uint64_t FromJavaTextInputToken(jlong token) {
    uint64_t result = 0;
    std::memcpy(&result, &token, sizeof(result));
    return result;
}

jlong ToJavaTextInputToken(uint64_t token) {
    jlong result = 0;
    std::memcpy(&result, &token, sizeof(result));
    return result;
}

jlong Native_BeginTextSession(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lock(g_input_mutex);
    return ToJavaTextInputToken(g_text_input_queue.Begin());
}

jboolean Native_ApplyTextEdit(JNIEnv* env, jclass, jlong token, jint delete_before,
                              jstring inserted_text, jboolean submit) {
    if (!env || !inserted_text || env->ExceptionCheck()) return JNI_FALSE;

    const jsize length = env->GetStringLength(inserted_text);
    if (env->ExceptionCheck() || length < 0 ||
        length > static_cast<jsize>(warband::text_input::kMaxEditCodePoints)) {
        return JNI_FALSE;
    }

    const jchar* const chars = env->GetStringChars(inserted_text, nullptr);
    if (!chars) return JNI_FALSE;

    std::array<uint16_t, warband::text_input::kMaxEditCodePoints> units{};
    const bool acquisition_exception = env->ExceptionCheck();
    if (!acquisition_exception) {
        for (jsize index = 0; index < length; ++index) {
            units[static_cast<size_t>(index)] = static_cast<uint16_t>(chars[index]);
        }
    }
    env->ReleaseStringChars(inserted_text, chars);
    if (acquisition_exception || env->ExceptionCheck()) return JNI_FALSE;

    std::lock_guard<std::mutex> lock(g_input_mutex);
    const bool accepted = g_text_input_queue.Apply(
        FromJavaTextInputToken(token), static_cast<int32_t>(delete_before),
        std::span<const uint16_t>(units.data(), static_cast<size_t>(length)),
        submit == JNI_TRUE);
    return accepted ? JNI_TRUE : JNI_FALSE;
}

void Native_EndTextSession(JNIEnv*, jclass, jlong token) {
    std::lock_guard<std::mutex> lock(g_input_mutex);
    g_text_input_queue.End(FromJavaTextInputToken(token));
}

const JNINativeMethod kOverlayMethods[] = {
    {"nativePrepareGameData", "(Landroid/content/res/AssetManager;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Z",
     reinterpret_cast<void*>(Native_PrepareGameData)},
    {"nativeSelectMasterServer", "(Ljava/lang/String;Ljava/lang/String;)Z",
     reinterpret_cast<void*>(Native_SelectMasterServer)},
    {"nativeMasterServerApplied", "(Ljava/lang/String;Ljava/lang/String;)Z",
     reinterpret_cast<void*>(Native_MasterServerApplied)},
    {"nativeStart", "()V", reinterpret_cast<void*>(Native_Start)},
    {"nativeShutdown", "()V", reinterpret_cast<void*>(Native_Shutdown)},
    {"nativeReady", "()Z", reinterpret_cast<void*>(Native_Ready)},
    {"nativeGuestFrameResponsive", "()Z",
     reinterpret_cast<void*>(Native_GuestFrameResponsive)},
    {"nativeMissionActive", "()Z", reinterpret_cast<void*>(Native_MissionActive)},
    {"nativeMissionSessionActive", "()Z",
     reinterpret_cast<void*>(Native_MissionSessionActive)},
    {"nativeActiveMenuWindowId", "()I", reinterpret_cast<void*>(Native_ActiveMenuWindowId)},
    {"nativeMapTimeRunning", "()Z", reinterpret_cast<void*>(Native_MapTimeRunning)},
    {"nativeToggleMapTime", "()Z", reinterpret_cast<void*>(Native_ToggleMapTime)},
    {"nativeActivePresentationId", "()I",
     reinterpret_cast<void*>(Native_ActivePresentationId)},
    {"nativeTacticalUiHit", "(FF)I", reinterpret_cast<void*>(Native_TacticalUiHit)},
    {"nativeTacticalUiTouch", "(IIFFJ)Z", reinterpret_cast<void*>(Native_TacticalUiTouch)},
    {"nativeTacticalModalState", "()I",
     reinterpret_cast<void*>(Native_TacticalModalState)},
    {"nativeConsumeInputToast", "()I",
     reinterpret_cast<void*>(Native_ConsumeInputToast)},
    {"nativeMissionButtonStates", "()I", reinterpret_cast<void*>(Native_MissionButtonStates)},
    {"nativeTapOrders", "(FF)Z", reinterpret_cast<void*>(Native_TapOrders)},
    {"nativeConfigureMenuTouch", "(ZZZFFII)V",
     reinterpret_cast<void*>(Native_ConfigureMenuTouch)},
    {"nativeExchangeGuestSettings", "(ZZZZZZZZZZZZZZFFFFFFFFFF)[I",
     reinterpret_cast<void*>(Native_ExchangeGuestSettings)},
    {"nativeSetOverlayEditMode", "(Z)V",
     reinterpret_cast<void*>(Native_SetOverlayEditMode)},
    {"nativeRangedWeaponHeld", "()Z",
     reinterpret_cast<void*>(Native_RangedWeaponHeld)},
    {"nativeSetMoveAxes", "(FF)V", reinterpret_cast<void*>(Native_SetMoveAxes)},
    {"nativeAddLookDelta", "(FF)V", reinterpret_cast<void*>(Native_AddLookDelta)},
    {"nativeAddGyroLookDelta", "(FF)V", reinterpret_cast<void*>(Native_AddGyroLookDelta)},
    {"nativeBeginDirectionalAction", "(IFF)V",
     reinterpret_cast<void*>(Native_BeginDirectionalAction)},
    {"nativeSetAction", "(IZ)V", reinterpret_cast<void*>(Native_SetAction)},
    {"nativeReset", "()V", reinterpret_cast<void*>(Native_Reset)},
};

const JNINativeMethod kTextInputMethods[] = {
    {"nativeBeginTextSession", "()J", reinterpret_cast<void*>(Native_BeginTextSession)},
    {"nativeApplyTextEdit", "(JILjava/lang/String;Z)Z",
     reinterpret_cast<void*>(Native_ApplyTextEdit)},
    {"nativeEndTextSession", "(J)V", reinterpret_cast<void*>(Native_EndTextSession)},
};

}  // namespace

void BeginHostInputEvent(const AInputEvent* event) {
    if (!event) return;
    std::lock_guard<std::mutex> lock(g_input_mutex);
    // AInputEvent storage may be reused after finishEvent.  Drop a stale slot
    // before this new lifecycle begins without dereferencing the old pointer.
    ForgetObservedInputEventLocked(event);
    if (g_touch_action_override_event == event) {
        g_touch_action_override_event = nullptr;
        g_touch_action_override_value = -1;
    }
    if (g_character_touch_remap.ending_event == event) {
        CancelCharacterTouchLocked();
    }
    if (g_ui_touch.ending_event == event) {
        CancelUiTouchLocked();
    }
}

int32_t WarbandTouchActionOverride(const AInputEvent* event) {
    if (!event) return -1;
    std::lock_guard<std::mutex> lock(g_input_mutex);
    return event == g_touch_action_override_event ? g_touch_action_override_value : -1;
}

float WarbandTouchCoordinateOverride(const AInputEvent* event,
                                     std::size_t pointer_index,
                                     bool y_axis, float original_value) {
    if (!event || !std::isfinite(original_value) ||
        AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION ||
        (AInputEvent_getSource(event) & AINPUT_SOURCE_TOUCHSCREEN) !=
            AINPUT_SOURCE_TOUCHSCREEN) {
        return original_value;
    }
    std::lock_guard<std::mutex> lock(g_input_mutex);
    if (pointer_index >= AMotionEvent_getPointerCount(event)) {
        return original_value;
    }
    const int32_t pointer_id = AMotionEvent_getPointerId(event, pointer_index);
    float delta = 0.0f;
    if (g_character_touch_remap.active &&
        pointer_id == g_character_touch_remap.pointer_id) {
        delta = y_axis ? g_character_touch_remap.delta_y
                       : g_character_touch_remap.delta_x;
    } else {
        return original_value;
    }
    const float remapped = original_value + delta;
    return std::isfinite(remapped) ? remapped : original_value;
}

void ClearWarbandTouchActionOverride(const AInputEvent* event) {
    if (!event) return;
    std::lock_guard<std::mutex> lock(g_input_mutex);
    if (g_touch_action_override_event == event) {
        g_touch_action_override_event = nullptr;
        g_touch_action_override_value = -1;
    }
}

void ObserveHostInputEvent(const AInputEvent* event) {
    if (!event) return;
    const int32_t type = AInputEvent_getType(event);
    const int32_t source = AInputEvent_getSource(event);
    std::lock_guard<std::mutex> lock(g_input_mutex);
    if (!MarkInputEventObservedLocked(event)) return;

    if (type == AINPUT_EVENT_TYPE_KEY) {
        const int32_t key = AKeyEvent_getKeyCode(event);
        const int32_t action = AKeyEvent_getAction(event);
        if (action != AKEY_EVENT_ACTION_DOWN && action != AKEY_EVENT_ACTION_UP) return;
        const bool down = action == AKEY_EVENT_ACTION_DOWN;
        if (down) {
            // A real key immediately hands both colour and outline ownership
            // back to the stock keyboard/gamepad controller.
            ReleaseTouchWidgetHighlightOwnershipLocked();
        }
        if (!ShouldSuppressHostKeyEvent(event)) return;

        const uint64_t token =
            (static_cast<uint64_t>(static_cast<uint32_t>(AInputEvent_getDeviceId(event))) << 32u) |
            static_cast<uint32_t>(key);
        auto existing = std::find(g_physical_keys_down.begin(), g_physical_keys_down.end(), token);
        bool changed = false;
        if (down) {
            if (existing != g_physical_keys_down.end()) return;
            const auto empty = std::find(g_physical_keys_down.begin(),
                                         g_physical_keys_down.end(), 0u);
            if (empty == g_physical_keys_down.end()) {
                ++g_physical_key_table_overflows;
                LOGE("physical key table saturated; ignored device=%d key=%d count=%u",
                     AInputEvent_getDeviceId(event), key, g_physical_key_table_overflows);
                return;
            }
            *empty = token;
            changed = true;
        } else if (existing != g_physical_keys_down.end()) {
            *existing = 0;
            changed = true;
        }
        if (!changed) return;

        uint32_t trigger_mask = 0;
        uint32_t discrete_mask = 0;
        for (const uint64_t held_token : g_physical_keys_down) {
            if (!held_token) continue;
            const int32_t held_key = static_cast<int32_t>(held_token & 0xFFFFFFFFu);
            if (held_key == AKEYCODE_BUTTON_R2) trigger_mask |= kHeldRt;
            if (held_key == AKEYCODE_BUTTON_L2) trigger_mask |= kHeldLt;
            for (const PhysicalDiscreteKeyBinding& binding : kPhysicalDiscreteKeys) {
                if (binding.key_code == held_key) discrete_mask |= binding.mask;
            }
        }
        g_physical_button_held = trigger_mask;
        g_discrete_buttons.SetPhysicalMask(discrete_mask);
        return;
    }

    if (type != AINPUT_EVENT_TYPE_MOTION) return;

    if ((source & AINPUT_SOURCE_TOUCHSCREEN) == AINPUT_SOURCE_TOUCHSCREEN &&
        OwnsActiveInventoryTouchLocked() &&
        warband::input::ShouldQuarantineInventoryRawStream(
            InventoryRecoveryInFlightLocked(), g_inventory_touch.tracking,
            g_inventory_touch.release_pending,
            g_inventory_touch.release_event_queued,
            g_inventory_touch.restore_pending)) {
        // Stop this raw stream before any observer can reset inventory state or
        // reinterpret it as map/menu input.  MOVE carries no inventory commit
        // semantics, so even its eventual UP cannot drop a held C34 item at an
        // unrelated touch position while source recovery is pending.
        SetTouchActionOverrideLocked(event, AMOTION_EVENT_ACTION_MOVE);
        return;
    }

    ObserveConversationTouchLocked(event, source);
    ObserveCharacterTouchLocked(event, source);
    if (g_character_touch_remap.active) {
        CancelUiTouchLocked();
    } else {
        ObserveUiTouchLocked(event, source);
    }
    ObserveMapTouchLocked(event, source);
    ObserveInventoryTouchLocked(event, source);
    if (g_character_touch_remap.active || UiTouchBlocksMenuInputLocked() ||
        OwnsActiveInventoryTouchLocked()) {
        // A magnetic '+' press is still the guest's native widget stream.  Do
        // not also turn any bridge-owned widget/drag stream into menu scrolling.
        CancelMenuInputLocked();
    } else {
        ObserveMenuTouchLocked(event, source);
    }
    if ((source & AINPUT_SOURCE_JOYSTICK) != AINPUT_SOURCE_JOYSTICK) {
        return;
    }
    const int32_t action = AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
    if (action == AMOTION_EVENT_ACTION_MOVE) {
        ReleaseTouchWidgetHighlightOwnershipLocked();
    }
    if (action == AMOTION_EVENT_ACTION_CANCEL) {
        g_physical_left_x = g_physical_left_y = 0.0f;
        g_physical_right_x = g_physical_right_y = 0.0f;
        g_physical_axis_held = 0;
        return;
    }
    if (action != AMOTION_EVENT_ACTION_MOVE) return;

    g_physical_left_x = ClampUnit(
        AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_X, 0));
    g_physical_left_y = ClampUnit(
        -AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_Y, 0));
    float right_x = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_Z, 0);
    float right_y = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_RZ, 0);
    const float fallback_x = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_RX, 0);
    const float fallback_y = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_RY, 0);
    if (std::fabs(right_x) < kAxisFallbackEpsilon &&
        std::fabs(fallback_x) >= kAxisFallbackEpsilon) {
        right_x = fallback_x;
    }
    if (std::fabs(right_y) < kAxisFallbackEpsilon &&
        std::fabs(fallback_y) >= kAxisFallbackEpsilon) {
        right_y = fallback_y;
    }
    g_physical_right_x = ClampUnit(right_x);
    g_physical_right_y = ClampUnit(-right_y);

    const float left_trigger = std::max(
        AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_LTRIGGER, 0),
        AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_BRAKE, 0));
    const float right_trigger = std::max(
        AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_RTRIGGER, 0),
        AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_GAS, 0));
    g_physical_axis_held = 0;
    if (std::isfinite(left_trigger) && left_trigger > kTriggerThreshold) {
        g_physical_axis_held |= kHeldLt;
    }
    if (std::isfinite(right_trigger) && right_trigger > kTriggerThreshold) {
        g_physical_axis_held |= kHeldRt;
    }
}

void EndHostInputEvent(const AInputEvent* event) {
    if (!event) return;
    std::lock_guard<std::mutex> lock(g_input_mutex);
    ForgetObservedInputEventLocked(event);
    if (g_character_touch_remap.ending_event == event) {
        CancelCharacterTouchLocked();
    }
    if (g_ui_touch.ending_event == event) {
        CancelUiTouchLocked();
    }
}

bool ShouldSuppressHostKeyEvent(const AInputEvent* event) {
    if (!event || AInputEvent_getType(event) != AINPUT_EVENT_TYPE_KEY) return false;
    const int32_t key = AKeyEvent_getKeyCode(event);
    if (key == AKEYCODE_BUTTON_L2 || key == AKEYCODE_BUTTON_R2) return true;
    return std::any_of(kPhysicalDiscreteKeys.begin(), kPhysicalDiscreteKeys.end(),
                       [key](const PhysicalDiscreteKeyBinding& binding) {
                           return binding.key_code == key;
                       });
}

void ResetObservedPhysicalInput() {
    std::lock_guard<std::mutex> lock(g_input_mutex);
    CancelConversationTouchLocked();
    CancelMapTouchLocked();
    CancelCharacterTouchLocked();
    ResetUiTouchLocked();
    g_physical_button_held = 0;
    g_physical_axis_held = 0;
    g_physical_keys_down.fill(0);
    g_discrete_buttons.ResetPhysical();
    g_physical_left_x = g_physical_left_y = 0.0f;
    g_physical_right_x = g_physical_right_y = 0.0f;
    g_observed_input_events.fill(nullptr);
    CancelMenuInputLocked();
}

JavaVM* GetHostJavaVM() { return g_jvm; }
DynarmicHost* GetDynarmicHostForJni() { return g_host.get(); }

namespace {

jint NativeInspectModTexture(JNIEnv* env, jclass, jstring path, jint target) {
    if (!path) return -1;
    const char* bytes = env->GetStringUTFChars(path, nullptr);
    if (!bytes) return -1;
    std::string error;
    const int result = mod_texture::InspectModTexture(bytes, target, &error);
    if (result < 0) LOGE("mod_texture_inspect_failed path='%s' error='%s'", bytes, error.c_str());
    env->ReleaseStringUTFChars(path, bytes);
    return result;
}

jstring NativeConvertModTexture(JNIEnv* env, jclass, jstring path, jint target) {
    if (!path) return env->NewStringUTF("Missing texture path.");
    const char* bytes = env->GetStringUTFChars(path, nullptr);
    if (!bytes) return nullptr;  // ART's pending allocation exception is retained.
    const std::string error = mod_texture::ConvertModTexture(bytes, target);
    if (!error.empty()) LOGE("mod_texture_conversion_failed path='%s' error='%s'", bytes, error.c_str());
    env->ReleaseStringUTFChars(path, bytes);
    return error.empty() ? nullptr : env->NewStringUTF(error.c_str());
}

const JNINativeMethod kModTextureMethods[] = {
    {"nativeInspect", "(Ljava/lang/String;I)I", reinterpret_cast<void*>(NativeInspectModTexture)},
    {"nativeConvert", "(Ljava/lang/String;I)Ljava/lang/String;", reinterpret_cast<void*>(NativeConvertModTexture)},
};

}  // namespace

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    g_jvm = vm;
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;

    jclass overlay = env->FindClass("com/taleworlds/mbwarband/MBTouchOverlay");
    if (!overlay) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return JNI_ERR;
    }
    const jint overlay_count =
        static_cast<jint>(sizeof(kOverlayMethods) / sizeof(kOverlayMethods[0]));
    const jint result = env->RegisterNatives(overlay, kOverlayMethods, overlay_count);
    env->DeleteLocalRef(overlay);
    if (result != JNI_OK) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return JNI_ERR;
    }

    jclass text_input = env->FindClass("com/taleworlds/mbwarband/MBTextInputView");
    if (!text_input) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return JNI_ERR;
    }
    const jint text_input_count =
        static_cast<jint>(sizeof(kTextInputMethods) / sizeof(kTextInputMethods[0]));
    const jint text_input_result =
        env->RegisterNatives(text_input, kTextInputMethods, text_input_count);
    env->DeleteLocalRef(text_input);
    if (text_input_result != JNI_OK) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return JNI_ERR;
    }

    jclass converter = env->FindClass("com/taleworlds/mbwarband/MBModTextureConverter");
    if (!converter) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return JNI_ERR;
    }
    const jint converter_count = static_cast<jint>(sizeof(kModTextureMethods) / sizeof(kModTextureMethods[0]));
    const jint converter_result = env->RegisterNatives(converter, kModTextureMethods, converter_count);
    env->DeleteLocalRef(converter);
    if (converter_result != JNI_OK) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return JNI_ERR;
    }
    LOGI("JNI_OnLoad: registered %d MBTouchOverlay, %d MBTextInputView and %d MBModTextureConverter natives",
         overlay_count, text_input_count, converter_count);
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT void JNICALL
ANativeActivity_onCreate(ANativeActivity* activity, void* saved_state, size_t saved_state_size) {
    const uint64_t startup_started = MonotonicMilliseconds();
    LOGI("ANativeActivity_onCreate(activity=%p, saved=%p/%zu)", activity, saved_state,
         saved_state_size);
    if (!BootGuest(activity)) {
        LOGE("startup NativeActivity failed during bridge boot after %llu ms",
             static_cast<unsigned long long>(MonotonicMilliseconds() - startup_started));
        return;
    }
    const uint64_t boot_finished = MonotonicMilliseconds();
    SetGuestActivityClass(activity);
    if (!StartGuestActivity(*g_host, activity, saved_state, saved_state_size)) {
        LOGE("guest ANativeActivity_onCreate failed");
        InvalidateTextInputForBridgeLifecycle(false);
        return;
    }
    LOGI("startup NativeActivity returned: total_ms=%llu bridge_ms=%llu guest_create_ms=%llu",
         static_cast<unsigned long long>(MonotonicMilliseconds() - startup_started),
         static_cast<unsigned long long>(boot_finished - startup_started),
         static_cast<unsigned long long>(MonotonicMilliseconds() - boot_finished));
}
