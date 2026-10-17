#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

struct ANativeActivity;
class DynarmicHost;

// 32-bit stand-ins for the 64-bit host pointers the guest is handed: jobject,
// JavaVM*, ANativeWindow*, AInputQueue*, AAssetManager*. The guest never
// dereferences them, it only passes them back into a thunk, so a handle is
// enough. They are carved out of the guest heap so they can never collide with
// an address the guest allocated for itself.
uint32_t GuestHandleFor(const void* host_ptr);
void* HostPointerFor(uint32_t handle);
// Drops a handle and returns its cell to the guest heap. Only use this when the
// bridge controls or has just observed the pointer's lifetime boundary: a JNI
// global ref it is about to delete, a closed AAsset, a successfully destroyed
// EGL object, or a borrowed NativeActivity object after its destroy callback.
// A handle the guest still holds afterwards resolves to null, which is a wrong
// answer rather than a use-after-free of the invalid host object.
void ReleaseGuestHandle(const void* host_ptr);

// Must run once the host exists and before any guest code can call JNI: the
// handle table carves its ids out of the guest heap.
void BindGuestBridge(DynarmicHost& host);

// Guest-readable JNIInvokeInterface. Built on first use so both the activity
// struct and GetJavaVM hand back the same one.
uint32_t GuestJavaVM();
ANativeActivity* HostNativeActivity();
// Overrides only the guest-visible external data directory. The platform
// activity remains untouched; this isolates save/config files per selected mod.
bool SetGuestExternalDataPath(const std::string& path);

// Builds the guest-side ANativeActivity, calls the guest's own
// ANativeActivity_onCreate, and wires the callbacks it registered back to the
// platform's ANativeActivityCallbacks.
bool StartGuestActivity(DynarmicHost& host, ANativeActivity* activity, void* saved_state,
                        size_t saved_state_size);
