// Guest-side ANativeActivity marshalling.
//
// The platform hands libmain.so a 64-bit ANativeActivity whose every field is a
// host pointer. The guest's ANativeActivity_onCreate expects the 32-bit layout,
// so it gets its own copy in guest memory with handles in place of pointers, and
// its own ANativeActivityCallbacks block to fill in. Once it returns, the slots
// it wrote are read back out and forwarded from real host callbacks.
//
// Unity's libmain.so writes exactly three of them (onDestroy at +0x14,
// onInputQueueCreated at +0x2c, onInputQueueDestroyed at +0x30) before it
// dlopen()s libunity.so; the rest are installed anyway so a later engine path
// that registers more does not silently go nowhere.

#include "native_activity_bridge.h"

#include <android/log.h>
#include <android/native_activity.h>
#include <android/rect.h>

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <dynarmic/interface/A32/a32.h>

#include "bridge_input.h"
#include "dynarmic_host.h"
#include "thunk_manager.h"
#include "unity_jni.h"

#define LOG_TAG "CMR_Activity"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

// ANativeActivity as the 32-bit guest sees it: ten 4-byte fields.
constexpr uint32_t kActCallbacks        = 0x00;
constexpr uint32_t kActVm               = 0x04;
constexpr uint32_t kActEnv              = 0x08;
constexpr uint32_t kActClazz            = 0x0c;
constexpr uint32_t kActInternalDataPath = 0x10;
constexpr uint32_t kActExternalDataPath = 0x14;
constexpr uint32_t kActSdkVersion       = 0x18;
constexpr uint32_t kActInstance         = 0x1c;
constexpr uint32_t kActAssetManager     = 0x20;
constexpr uint32_t kActObbPath          = 0x24;
constexpr uint32_t kActSize             = 0x28;

// Slot indices into ANativeActivityCallbacks, in declaration order.
enum CallbackSlot {
    kOnStart = 0,
    kOnResume,
    kOnSaveInstanceState,
    kOnPause,
    kOnStop,
    kOnDestroy,
    kOnWindowFocusChanged,
    kOnNativeWindowCreated,
    kOnNativeWindowResized,
    kOnNativeWindowRedrawNeeded,
    kOnNativeWindowDestroyed,
    kOnInputQueueCreated,
    kOnInputQueueDestroyed,
    kOnContentRectChanged,
    kOnConfigurationChanged,
    kOnLowMemory,
    kCallbackCount,
};

const char* const kSlotNames[kCallbackCount] = {
    "onStart", "onResume", "onSaveInstanceState", "onPause",
    "onStop", "onDestroy", "onWindowFocusChanged", "onNativeWindowCreated",
    "onNativeWindowResized", "onNativeWindowRedrawNeeded", "onNativeWindowDestroyed",
    "onInputQueueCreated", "onInputQueueDestroyed", "onContentRectChanged",
    "onConfigurationChanged", "onLowMemory",
};

DynarmicHost* g_host = nullptr;
ANativeActivity* g_host_activity = nullptr;
uint32_t g_guest_activity = 0;
uint32_t g_guest_callbacks = 0;
std::mutex g_data_path_mutex;
std::string g_external_data_path_override;

std::mutex g_handle_mutex;
std::unordered_map<uint32_t, void*> g_handle_to_host;
std::unordered_map<const void*, uint32_t> g_host_to_handle;

uint32_t GuestStrDup(DynarmicHost& host, const char* s) {
    if (!s) return 0;
    size_t n = strlen(s) + 1;
    uint32_t addr = host.GuestAlloc(n, 1);
    if (addr) memcpy(host.GetGuestMemoryBase() + addr, s, n);
    return addr;
}

// The guest calls (*vm)->AttachCurrentThread on every thread it spins up, so the
// vm field cannot just be a handle - it has to be a guest-readable JNIInvokeInterface.
uint32_t BuildGuestJavaVM(DynarmicHost& host) {
    static uint32_t cached = 0;
    if (cached) return cached;
    ThunkManager& thunks = host.GetThunkManager();
    const uint32_t guest_env = thunks.GetGuestJNIEnv();

    // JNIInvokeInterface: three reserved words, then DestroyJavaVM,
    // AttachCurrentThread, DetachCurrentThread, GetEnv, AttachCurrentThreadAsDaemon.
    constexpr uint32_t kInvokeSlots = 8;
    uint32_t table = host.GuestAlloc(kInvokeSlots * 4, 8);
    for (uint32_t i = 0; i < kInvokeSlots; ++i) host.MemoryWrite32(table + i * 4, 0);

    // Both attach forms and GetEnv have the same shape here: write the guest
    // JNIEnv through the out-parameter in r1 and report JNI_OK.
    auto hand_back_env = [guest_env](Dynarmic::A32::Jit& jit, DynarmicHost& h) {
        if (jit.Regs()[1]) h.MemoryWrite32(jit.Regs()[1], guest_env);
        jit.Regs()[0] = 0;  // JNI_OK
    };
    thunks.RegisterThunk("JVM_AttachCurrentThread", hand_back_env);
    thunks.RegisterThunk("JVM_AttachCurrentThreadAsDaemon", hand_back_env);
    thunks.RegisterThunk("JVM_GetEnv", hand_back_env);
    thunks.RegisterThunk("JVM_DetachCurrentThread",
                         [](Dynarmic::A32::Jit& jit, DynarmicHost&) { jit.Regs()[0] = 0; });
    thunks.RegisterThunk("JVM_DestroyJavaVM",
                         [](Dynarmic::A32::Jit& jit, DynarmicHost&) { jit.Regs()[0] = 0; });

    host.MemoryWrite32(table + 3 * 4, thunks.GetOrCreateThunkAddress("JVM_DestroyJavaVM"));
    host.MemoryWrite32(table + 4 * 4, thunks.GetOrCreateThunkAddress("JVM_AttachCurrentThread"));
    host.MemoryWrite32(table + 5 * 4, thunks.GetOrCreateThunkAddress("JVM_DetachCurrentThread"));
    host.MemoryWrite32(table + 6 * 4, thunks.GetOrCreateThunkAddress("JVM_GetEnv"));
    host.MemoryWrite32(table + 7 * 4,
                       thunks.GetOrCreateThunkAddress("JVM_AttachCurrentThreadAsDaemon"));

    uint32_t vm = host.GuestAlloc(4, 4);
    host.MemoryWrite32(vm, table);
    cached = vm;
    return vm;
}

uint32_t GuestCallback(int slot) {
    if (!g_host || !g_guest_callbacks) return 0;
    return g_host->MemoryRead32(g_guest_callbacks + slot * 4);
}

uint32_t Invoke(int slot, std::vector<uint32_t> args) {
    uint32_t fn = GuestCallback(slot);
    if (!fn) return 0;
    LOGI("guest callback %s fn=0x%08x", kSlotNames[slot], fn);
    args.insert(args.begin(), g_guest_activity);
    uint32_t result = g_host->RunFunction(fn, args);
    LOGI("guest callback %s returned 0x%08x", kSlotNames[slot], result);
    return result;
}

void OnStart(ANativeActivity*)   { Invoke(kOnStart, {}); }
void OnResume(ANativeActivity*)  { Invoke(kOnResume, {}); }
void OnPause(ANativeActivity*)   { Invoke(kOnPause, {}); }
void OnStop(ANativeActivity*)    { Invoke(kOnStop, {}); }
void OnDestroy(ANativeActivity*) { Invoke(kOnDestroy, {}); }
void OnLowMemory(ANativeActivity*) { Invoke(kOnLowMemory, {}); }
void OnConfigurationChanged(ANativeActivity*) { Invoke(kOnConfigurationChanged, {}); }

void OnWindowFocusChanged(ANativeActivity*, int has_focus) {
    Invoke(kOnWindowFocusChanged, {static_cast<uint32_t>(has_focus)});
}

void OnNativeWindowCreated(ANativeActivity*, ANativeWindow* w) {
    Invoke(kOnNativeWindowCreated, {GuestHandleFor(w)});
}
void OnNativeWindowResized(ANativeActivity*, ANativeWindow* w) {
    Invoke(kOnNativeWindowResized, {GuestHandleFor(w)});
}
void OnNativeWindowRedrawNeeded(ANativeActivity*, ANativeWindow* w) {
    Invoke(kOnNativeWindowRedrawNeeded, {GuestHandleFor(w)});
}
void OnNativeWindowDestroyed(ANativeActivity*, ANativeWindow* w) {
    Invoke(kOnNativeWindowDestroyed, {GuestHandleFor(w)});
    // NativeActivity owns this borrowed window.  Its pointer is valid through
    // the destroy callback, but the NDK explicitly forbids touching it after
    // the callback returns.  Drop both translations at that exact boundary so
    // a later window cannot inherit a stale guest handle for a recycled host
    // address.
    ReleaseGuestHandle(w);
}

void OnInputQueueCreated(ANativeActivity*, AInputQueue* q) {
    Invoke(kOnInputQueueCreated, {GuestHandleFor(q)});
}
void OnInputQueueDestroyed(ANativeActivity*, AInputQueue* q) {
    ResetObservedPhysicalInput();
    Invoke(kOnInputQueueDestroyed, {GuestHandleFor(q)});
    // As with the native window, the queue is borrowed from NativeActivity and
    // becomes invalid when this callback returns.  The guest callback has had
    // its final chance to detach it from the looper, so retaining its mapping
    // would only leak a guest-heap cell and permit stale-pointer aliasing.
    ReleaseGuestHandle(q);
}

void OnContentRectChanged(ANativeActivity*, const ARect* rect) {
    uint32_t guest_rect = 0;
    if (rect) {
        guest_rect = g_host->GuestAlloc(4 * 4, 4);
        if (guest_rect) {
            g_host->MemoryWrite32(guest_rect + 0,  static_cast<uint32_t>(rect->left));
            g_host->MemoryWrite32(guest_rect + 4,  static_cast<uint32_t>(rect->top));
            g_host->MemoryWrite32(guest_rect + 8,  static_cast<uint32_t>(rect->right));
            g_host->MemoryWrite32(guest_rect + 12, static_cast<uint32_t>(rect->bottom));
        }
    }
    Invoke(kOnContentRectChanged, {guest_rect});
    // The platform ARect is callback-scoped, so the 32-bit marshalling copy is
    // callback-scoped as well.  Keeping every copy permanently consumed one
    // guest-heap block per inset/content-rect notification.
    if (guest_rect) g_host->GuestFree(guest_rect);
}

// The platform frees this with free(), so the guest's blob has to be copied out
// of guest memory into a real host allocation.
void* OnSaveInstanceState(ANativeActivity*, size_t* out_len) {
    if (!out_len) return nullptr;
    *out_len = 0;
    if (!GuestCallback(kOnSaveInstanceState)) return nullptr;

    uint32_t guest_len = g_host->GuestAlloc(4, 4);
    if (!guest_len) {
        LOGE("guest onSaveInstanceState: unable to allocate length slot");
        return nullptr;
    }
    g_host->MemoryWrite32(guest_len, 0);
    uint32_t guest_blob = Invoke(kOnSaveInstanceState, {guest_len});
    uint32_t len = g_host->MemoryRead32(guest_len);
    g_host->GuestFree(guest_len);
    if (!guest_blob) return nullptr;
    if (!len) {
        g_host->GuestFree(guest_blob);
        return nullptr;
    }
    const size_t guest_memory_size = g_host->GetGuestMemorySize();
    if (static_cast<uint64_t>(guest_blob) + len > guest_memory_size) {
        LOGE("guest onSaveInstanceState returned an invalid range 0x%08x + %u",
             guest_blob, len);
        g_host->GuestFree(guest_blob);
        return nullptr;
    }

    void* out = malloc(len);
    if (!out) {
        g_host->GuestFree(guest_blob);
        return nullptr;
    }
    memcpy(out, g_host->GetGuestMemoryBase() + guest_blob, len);
    // Android owns and eventually free()s only `out`.  The callback allocated
    // `guest_blob` through the emulated malloc thunk, so the bridge must release
    // that original after copying or every saved-state request leaks guest heap.
    g_host->GuestFree(guest_blob);
    *out_len = len;
    return out;
}

void InstallHostCallbacks(ANativeActivity* activity) {
    ANativeActivityCallbacks* cb = activity->callbacks;
    cb->onStart = OnStart;
    cb->onResume = OnResume;
    cb->onSaveInstanceState = OnSaveInstanceState;
    cb->onPause = OnPause;
    cb->onStop = OnStop;
    cb->onDestroy = OnDestroy;
    cb->onWindowFocusChanged = OnWindowFocusChanged;
    cb->onNativeWindowCreated = OnNativeWindowCreated;
    cb->onNativeWindowResized = OnNativeWindowResized;
    cb->onNativeWindowRedrawNeeded = OnNativeWindowRedrawNeeded;
    cb->onNativeWindowDestroyed = OnNativeWindowDestroyed;
    cb->onInputQueueCreated = OnInputQueueCreated;
    cb->onInputQueueDestroyed = OnInputQueueDestroyed;
    cb->onContentRectChanged = OnContentRectChanged;
    cb->onConfigurationChanged = OnConfigurationChanged;
    cb->onLowMemory = OnLowMemory;
}

}  // namespace

void BindGuestBridge(DynarmicHost& host) { g_host = &host; }

uint32_t GuestJavaVM() { return g_host ? BuildGuestJavaVM(*g_host) : 0; }

ANativeActivity* HostNativeActivity() { return g_host_activity; }

bool SetGuestExternalDataPath(const std::string& path) {
    if (path.empty() || path.front() != '/' || path.back() == '/' ||
        path.size() >= 512 || path.find("\\") != std::string::npos ||
        path.find("/../") != std::string::npos || path.find("/./") != std::string::npos) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_data_path_mutex);
    g_external_data_path_override = path;
    return true;
}

uint32_t GuestHandleFor(const void* host_ptr) {
    if (!host_ptr || !g_host) return 0;
    std::lock_guard<std::mutex> lock(g_handle_mutex);
    auto it = g_host_to_handle.find(host_ptr);
    if (it != g_host_to_handle.end()) return it->second;

    uint32_t handle = g_host->GuestAlloc(4, 4);
    if (!handle) return 0;
    g_host_to_handle[host_ptr] = handle;
    g_handle_to_host[handle] = const_cast<void*>(host_ptr);
    return handle;
}

void* HostPointerFor(uint32_t handle) {
    if (!handle) return nullptr;
    std::lock_guard<std::mutex> lock(g_handle_mutex);
    auto it = g_handle_to_host.find(handle);
    return it == g_handle_to_host.end() ? nullptr : it->second;
}

void ReleaseGuestHandle(const void* host_ptr) {
    if (!host_ptr || !g_host) return;
    std::lock_guard<std::mutex> lock(g_handle_mutex);
    auto it = g_host_to_handle.find(host_ptr);
    if (it == g_host_to_handle.end()) return;
    const uint32_t handle = it->second;
    g_host_to_handle.erase(it);
    g_handle_to_host.erase(handle);
    // Without this the table is bounded but the cells it hands out are not: the
    // JNI ref table evicts continuously once it is full, and every eviction used
    // to strand another four bytes of arena.
    g_host->GuestFree(handle);
}

bool StartGuestActivity(DynarmicHost& host, ANativeActivity* activity, void* saved_state,
                        size_t saved_state_size) {
    g_host_activity = activity;
    BindGuestBridge(host);
    // Do this while still on the UI thread: it is the only one whose FindClass can
    // see app classes without help.
    BindGuestClassLoader(activity->env, activity->clazz);

    uint32_t entry = host.FindSymbol("ANativeActivity_onCreate");
    if (entry == 0) {
        LOGE("guest ANativeActivity_onCreate not found");
        return false;
    }

    g_guest_callbacks = host.GuestAlloc(kCallbackCount * 4, 8);
    for (int i = 0; i < kCallbackCount; ++i) host.MemoryWrite32(g_guest_callbacks + i * 4, 0);

    g_guest_activity = host.GuestAlloc(kActSize, 8);
    const uint32_t guest_vm = GuestJavaVM();
    host.MemoryWrite32(g_guest_activity + kActCallbacks, g_guest_callbacks);
    host.MemoryWrite32(g_guest_activity + kActVm, guest_vm);
    host.MemoryWrite32(g_guest_activity + kActEnv, host.GetThunkManager().GetGuestJNIEnv());
    host.MemoryWrite32(g_guest_activity + kActClazz, GuestHandleFor(activity->clazz));
    std::string external_data_path;
    {
        std::lock_guard<std::mutex> lock(g_data_path_mutex);
        external_data_path = g_external_data_path_override;
    }
    // Warband has more than the two personal-file call sites covered by the
    // original ARM32 binary patch.  In particular, profiles and save-slot
    // enumeration can still read ANativeActivity.internalDataPath directly.
    // When Java supplies the validated app-specific external save root, expose
    // it through both guest fields so every personal-file helper uses the same
    // per-module location.  The real host activity remains untouched.
    const char* guest_internal_data_path = external_data_path.empty()
        ? activity->internalDataPath : external_data_path.c_str();
    const char* guest_external_data_path = external_data_path.empty()
        ? activity->externalDataPath : external_data_path.c_str();
    host.MemoryWrite32(g_guest_activity + kActInternalDataPath,
                       GuestStrDup(host, guest_internal_data_path));
    host.MemoryWrite32(g_guest_activity + kActExternalDataPath,
                       GuestStrDup(host, guest_external_data_path));
    host.MemoryWrite32(g_guest_activity + kActSdkVersion,
                       static_cast<uint32_t>(activity->sdkVersion));
    host.MemoryWrite32(g_guest_activity + kActInstance, 0);
    host.MemoryWrite32(g_guest_activity + kActAssetManager, GuestHandleFor(activity->assetManager));
    host.MemoryWrite32(g_guest_activity + kActObbPath, GuestStrDup(host, activity->obbPath));

    uint32_t guest_saved = 0;
    if (saved_state && saved_state_size) {
        guest_saved = host.GuestAlloc(saved_state_size, 8);
        if (guest_saved) memcpy(host.GetGuestMemoryBase() + guest_saved, saved_state, saved_state_size);
    }

    LOGI("entering guest ANativeActivity_onCreate at 0x%08x (activity=0x%08x, sdk=%d)", entry,
         g_guest_activity, activity->sdkVersion);
    host.ArmBootstrapProbe(entry);
    host.RunFunction(entry, {g_guest_activity, guest_saved,
                             static_cast<uint32_t>(saved_state_size)});
    const auto probe = host.GetBootstrapProbeResult();
    if (!probe.code_fetch_observed || !probe.pc_advanced ||
        host.GetThunkManager().DiagnosticTrapExecuted()) {
        LOGE("bootstrap probe failed: fetched=%s advanced=%s trap=%s final=0x%08x halt=0x%x",
             probe.code_fetch_observed ? "yes" : "no", probe.pc_advanced ? "yes" : "no",
             host.GetThunkManager().DiagnosticTrapExecuted() ? "yes" : "no", probe.final_pc,
             probe.halt_reason);
        return false;
    }
    LOGI("returned from guest ANativeActivity_onCreate after verified PC advance");

    for (int i = 0; i < kCallbackCount; ++i) {
        uint32_t fn = GuestCallback(i);
        if (fn) LOGI("  guest %s = 0x%08x", kSlotNames[i], fn);
    }

    // Only now is libunity.so "loaded" from the guest's point of view, so this is
    // the earliest its JNI_OnLoad can run and hand over its native method table.
    RunGuestJniOnLoad(host, guest_vm);
    if (!GuestNativeRegistryValid()) {
        LOGE("guest JNI registration contract is invalid");
        return false;
    }

    InstallHostCallbacks(activity);
    return true;
}
