#pragma once

#include <cstdint>
#include <string>

class DynarmicHost;

// libunity.so does not export its Java_com_unity3d_player_* entry points - they
// are private symbols handed to RegisterNatives from JNI_OnLoad. Intercepting
// that call is the only way to learn their guest addresses, so the guest JNIEnv
// records them here instead of forwarding to the real JVM.
void RecordGuestNative(const std::string& class_hint, const std::string& name,
                       const std::string& signature, uint32_t guest_fn);

// Guest address of a registered native, or 0. The registry keeps the full
// class/name/signature contract and also permits name-only lookup after proving
// the retained surface has no collisions.
uint32_t GuestNative(const std::string& class_name, const std::string& name,
                     const std::string& signature);
uint32_t GuestNative(const std::string& name);
bool GuestNativeRegistryValid();

// Runs the guest libraries' JNI_OnLoad so their natives get registered.
void RunGuestJniOnLoad(DynarmicHost& host, uint32_t guest_vm);

class ThunkManager;

// Fills the guest's JNIEnv function table with slots that marshal to the real JVM.
void InstallGuestJNIEnv(ThunkManager& thunks, DynarmicHost& host, uint32_t table_addr);

struct _JNIEnv;
class _jobject;

// Hands an object that arrived from Java to the guest as a 32-bit handle.
uint32_t GuestRefForHostObject(_JNIEnv* env, _jobject* obj);
// Direct ByteBuffer support: the guest gets a shadow copy in its own arena, and
// the caller flushes it back once the guest has written into it.
uint32_t GuestDirectBufferShadow(_JNIEnv* env, _jobject* buffer);
void FlushGuestDirectBuffer(_JNIEnv* env, _jobject* buffer);

// Captures the activity's ClassLoader so guest threads can resolve app classes.
void BindGuestClassLoader(_JNIEnv* env, _jobject* activity);
