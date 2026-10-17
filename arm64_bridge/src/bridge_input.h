#pragma once

#include <cstddef>
#include <cstdint>

struct AInputEvent;

void BeginHostInputEvent(const AInputEvent* event);
// Observe accepted host input before the ARM32 guest consumes it. Controller
// state is source-arbitrated; non-tactical touchscreen gestures are recognized
// alongside (never removed from) the guest's NativeActivity event stream.
void ObserveHostInputEvent(const AInputEvent* event);
void EndHostInputEvent(const AInputEvent* event);
bool ShouldSuppressHostKeyEvent(const AInputEvent* event);
void ResetObservedPhysicalInput();
// Menu-screen-change guard: the guest observes the overridden action (CANCEL)
// for a release whose press happened on a different game screen.  -1 = none.
int32_t WarbandTouchActionOverride(const AInputEvent* event);
void ClearWarbandTouchActionOverride(const AInputEvent* event);
// Returns a per-stream translated coordinate for character-sheet increment
// buttons, or the unmodified value when no invisible expanded target owns it.
float WarbandTouchCoordinateOverride(const AInputEvent* event,
                                     std::size_t pointer_index,
                                     bool y_axis, float original_value);
