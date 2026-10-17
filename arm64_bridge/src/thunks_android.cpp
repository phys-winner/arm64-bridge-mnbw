// libandroid passthrough.
//
// The reference bridge had none of this: Galaxy on Fire 2 was driven from Java,
// so nothing in the guest ever touched the NDK's activity APIs. a NativeActivity game is a
// NativeActivity, and its whole main loop is ALooper + AInputQueue, with the
// asset manager and the window coming through the same door.
//
// Everything here is a straight forward to the host implementation. The only
// translation needed is the pointer width: host objects become 32-bit handles,
// out-parameters are written back into guest memory, and floats travel as raw
// bit patterns because the guest ABI is softfp.

#include "thunk_manager.h"

#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/configuration.h>
#include <android/input.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <cstdint>
#include <cstring>

#include <dynarmic/interface/A32/a32.h>

#include "dynarmic_host.h"
#include "bridge_input.h"
#include "native_activity_bridge.h"
#include "thunk_types.h"

#define LOG_TAG "CMR_Android"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

JavaVM* GetHostJavaVM();

namespace {

using Jit = Dynarmic::A32::Jit;

template <typename T>
T* Host(uint32_t handle) {
    return static_cast<T*>(HostPointerFor(handle));
}

uint32_t FloatBits(float f) {
    uint32_t bits;
    memcpy(&bits, &f, sizeof(bits));
    return bits;
}

void SetInt64(Jit& jit, int64_t v) {
    auto u = static_cast<uint64_t>(v);
    jit.Regs()[0] = static_cast<uint32_t>(u);
    jit.Regs()[1] = static_cast<uint32_t>(u >> 32);
}

// ALooper_pollAll is gone from the NDK 27 headers. Its behaviour was pollOnce in a
// loop that swallows callback-dispatch results, which is exactly what the guest
// expects, so it is spelled out here rather than worked around.
int PollAll(int timeout_ms, int* out_fd, int* out_events, void** out_data) {
    for (;;) {
        int result = ALooper_pollOnce(timeout_ms, out_fd, out_events, out_data);
        if (result != ALOOPER_POLL_CALLBACK) return result;
    }
}

JNIEnv* HostEnv() {
    JNIEnv* env = nullptr;
    JavaVM* vm = GetHostJavaVM();
    if (!vm) return nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return nullptr;
    }
    return env;
}

}  // namespace

void ThunkManager::RegisterAndroidThunks() {
    // --- ALooper -----------------------------------------------------------
    RegisterThunk("ALooper_prepare", [](Jit& jit, DynarmicHost&) {
        ALooper* looper = ALooper_prepare(static_cast<int>(jit.Regs()[0]));
        jit.Regs()[0] = GuestHandleFor(looper);
    });
    RegisterThunk("ALooper_forThread", [](Jit& jit, DynarmicHost&) {
        jit.Regs()[0] = GuestHandleFor(ALooper_forThread());
    });
    RegisterThunk("ALooper_wake", [](Jit& jit, DynarmicHost&) {
        if (ALooper* looper = Host<ALooper>(jit.Regs()[0])) ALooper_wake(looper);
        jit.Regs()[0] = 0;
    });
    RegisterThunk("ALooper_addFd", [](Jit& jit, DynarmicHost& host) {
        ALooper* looper = Host<ALooper>(jit.Regs()[0]);
        const uint32_t callback = host.MemoryRead32(jit.Regs()[13]);
        const uint32_t data = host.MemoryRead32(jit.Regs()[13] + 4);
        if (callback) {
            LOGE("ALooper_addFd with a guest callback 0x%08X - not bridged", callback);
            jit.Regs()[0] = static_cast<uint32_t>(-1);
            return;
        }
        const int fd = static_cast<int>(jit.Regs()[1]);
        const int ident = static_cast<int>(jit.Regs()[2]);
        // User data is an opaque guest value, not a host object to intern.
        const int result = looper ? ALooper_addFd(looper, fd, ident,
            static_cast<int>(jit.Regs()[3]), nullptr,
            reinterpret_cast<void*>(static_cast<uintptr_t>(data))) : -1;
        LOGI("ALooper_addFd fd=%d ident=%d data=0x%08X result=%d", fd, ident, data, result);
        jit.Regs()[0] = static_cast<uint32_t>(result);
    });
    // outData must round-trip the caller's guest poll-source pointer unchanged.
    RegisterThunk("ALooper_pollAll", [](Jit& jit, DynarmicHost& host) {
        static thread_local unsigned traced_polls = 0;
        const bool trace = traced_polls < 8;
        const int timeout = static_cast<int>(jit.Regs()[0]);
        if (trace) {
            ++traced_polls;
            LOGI("ALooper_pollAll enter timeout=%d", timeout);
        }
        int fd = 0;
        int events = 0;
        void* data = nullptr;
        int result = PollAll(timeout, &fd, &events, &data);
        const uintptr_t token = reinterpret_cast<uintptr_t>(data);
        if (token > UINT32_MAX) {
            LOGE("ALooper_pollAll returned foreign host data %p", data);
            result = ALOOPER_POLL_ERROR;
            fd = events = 0;
        }
        const uint32_t guest_data = token <= UINT32_MAX ? static_cast<uint32_t>(token) : 0;
        if (jit.Regs()[1]) host.MemoryWrite32(jit.Regs()[1], static_cast<uint32_t>(fd));
        if (jit.Regs()[2]) host.MemoryWrite32(jit.Regs()[2], static_cast<uint32_t>(events));
        if (jit.Regs()[3]) host.MemoryWrite32(jit.Regs()[3], guest_data);
        if (trace) LOGI("ALooper_pollAll result=%d fd=%d events=0x%X data=0x%08X",
                        result, fd, events, guest_data);
        jit.Regs()[0] = static_cast<uint32_t>(result);
    });

    // --- AInputQueue -------------------------------------------------------
    RegisterThunk("AInputQueue_attachLooper", [](Jit& jit, DynarmicHost& host) {
        AInputQueue* queue = Host<AInputQueue>(jit.Regs()[0]);
        ALooper* looper = Host<ALooper>(jit.Regs()[1]);
        const uint32_t data = host.MemoryRead32(jit.Regs()[13]);
        if (jit.Regs()[3]) {
            // Guest callbacks need a trampoline; do not silently change semantics.
            LOGE("AInputQueue_attachLooper with a guest callback 0x%08X - not bridged",
                 jit.Regs()[3]);
            jit.Regs()[0] = 0;
            return;
        }
        if (queue && looper) {
            AInputQueue_attachLooper(queue, looper, static_cast<int>(jit.Regs()[2]), nullptr,
                                     reinterpret_cast<void*>(static_cast<uintptr_t>(data)));
            LOGI("AInputQueue_attachLooper ident=%d data=0x%08X",
                 static_cast<int>(jit.Regs()[2]), data);
        }
        jit.Regs()[0] = 0;
    });
    RegisterThunk("AInputQueue_detachLooper", [](Jit& jit, DynarmicHost&) {
        if (AInputQueue* queue = Host<AInputQueue>(jit.Regs()[0])) AInputQueue_detachLooper(queue);
        jit.Regs()[0] = 0;
    });
    RegisterThunk("AInputQueue_getEvent", [](Jit& jit, DynarmicHost& host) {
        AInputQueue* queue = Host<AInputQueue>(jit.Regs()[0]);
        AInputEvent* event = nullptr;
        int32_t result = queue ? AInputQueue_getEvent(queue, &event) : -1;
        if (result >= 0 && event) BeginHostInputEvent(event);
        if (jit.Regs()[1]) host.MemoryWrite32(jit.Regs()[1], GuestHandleFor(event));
        jit.Regs()[0] = static_cast<uint32_t>(result);
    });
    RegisterThunk("AInputQueue_preDispatchEvent", [](Jit& jit, DynarmicHost&) {
        AInputQueue* queue = Host<AInputQueue>(jit.Regs()[0]);
        AInputEvent* event = Host<AInputEvent>(jit.Regs()[1]);
        const int32_t dispatched =
            (queue && event) ? AInputQueue_preDispatchEvent(queue, event) : 0;
        // Commit controller state only after the framework declines pre-dispatch.
        // Observing in getEvent is too early: BACK and other framework-owned keys
        // may be returned here and must never be injected into the guest.
        if (!dispatched && queue && event) ObserveHostInputEvent(event);
        // A non-zero result transfers the event back to the framework.  The guest must not
        // call finishEvent in that case, so this is the only lifetime boundary at which its
        // transient guest handle can be released.
        if (dispatched && event) {
            ClearWarbandTouchActionOverride(event);
            EndHostInputEvent(event);
            ReleaseGuestHandle(event);
        }
        jit.Regs()[0] = static_cast<uint32_t>(dispatched);
    });
    RegisterThunk("AInputQueue_finishEvent", [](Jit& jit, DynarmicHost&) {
        AInputQueue* queue = Host<AInputQueue>(jit.Regs()[0]);
        AInputEvent* event = Host<AInputEvent>(jit.Regs()[1]);
        // Android requires preDispatchEvent before processing, but observing again
        // here is an idempotent fail-safe for guests which omit that call.  It must
        // happen before finishEvent invalidates the AInputEvent pointer.
        if (queue && event) ObserveHostInputEvent(event);
        if (queue && event) AInputQueue_finishEvent(queue, event, static_cast<int>(jit.Regs()[2]));
        // AInputEvent pointers are valid only from getEvent until preDispatch/finish.  Keeping
        // their four-byte guest cells forever leaked guest arena memory on every distinct host
        // event pointer during long menu/mission sessions.
        if (event) {
            ClearWarbandTouchActionOverride(event);
            EndHostInputEvent(event);
            ReleaseGuestHandle(event);
        }
        jit.Regs()[0] = 0;
    });

    // --- AInputEvent / AKeyEvent -------------------------------------------
    auto event_int = [](int32_t (*fn)(const AInputEvent*)) {
        return [fn](Jit& jit, DynarmicHost&) {
            const AInputEvent* e = Host<AInputEvent>(jit.Regs()[0]);
            jit.Regs()[0] = e ? static_cast<uint32_t>(fn(e)) : 0;
        };
    };
    RegisterThunk("AInputEvent_getType", event_int(&AInputEvent_getType));
    RegisterThunk("AInputEvent_getDeviceId", event_int(&AInputEvent_getDeviceId));
    RegisterThunk("AInputEvent_getSource", event_int(&AInputEvent_getSource));
    RegisterThunk("AKeyEvent_getAction", event_int(&AKeyEvent_getAction));
    RegisterThunk("AKeyEvent_getKeyCode", [](Jit& jit, DynarmicHost&) {
        const AInputEvent* event = Host<AInputEvent>(jit.Regs()[0]);
        // Managed controller keys are converted into source-arbitrated guest
        // events on the frame thread.  Returning UNKNOWN prevents the original
        // physical UP from racing that stream and releasing a touch-held action.
        jit.Regs()[0] = event
            ? static_cast<uint32_t>(ShouldSuppressHostKeyEvent(event)
                                        ? AKEYCODE_UNKNOWN
                                        : AKeyEvent_getKeyCode(event))
            : 0u;
    });
    RegisterThunk("AKeyEvent_getMetaState", event_int(&AKeyEvent_getMetaState));
    RegisterThunk("AMotionEvent_getAction", [](Jit& jit, DynarmicHost&) {
        const AInputEvent* e = Host<AInputEvent>(jit.Regs()[0]);
        // A release whose press landed on a different game screen is reported
        // as CANCEL so the guest cannot press a phantom widget on the new one.
        const int32_t override_action = WarbandTouchActionOverride(e);
        jit.Regs()[0] = static_cast<uint32_t>(
            override_action >= 0 ? override_action
                                 : (e ? AMotionEvent_getAction(e) : 0));
    });
    RegisterThunk("AMotionEvent_getFlags", event_int(&AMotionEvent_getFlags));
    RegisterThunk("AMotionEvent_getMetaState", event_int(&AMotionEvent_getMetaState));
    RegisterThunk("AMotionEvent_getEdgeFlags", event_int(&AMotionEvent_getEdgeFlags));

    // --- AMotionEvent ------------------------------------------------------
    auto motion_size_t = [](size_t (*fn)(const AInputEvent*)) {
        return [fn](Jit& jit, DynarmicHost&) {
            const AInputEvent* e = Host<AInputEvent>(jit.Regs()[0]);
            jit.Regs()[0] = e ? static_cast<uint32_t>(fn(e)) : 0;
        };
    };
    RegisterThunk("AMotionEvent_getPointerCount", motion_size_t(&AMotionEvent_getPointerCount));
    RegisterThunk("AMotionEvent_getHistorySize", motion_size_t(&AMotionEvent_getHistorySize));

    auto motion_time = [](int64_t (*fn)(const AInputEvent*)) {
        return [fn](Jit& jit, DynarmicHost&) {
            const AInputEvent* e = Host<AInputEvent>(jit.Regs()[0]);
            SetInt64(jit, e ? fn(e) : 0);
        };
    };
    RegisterThunk("AMotionEvent_getDownTime", motion_time(&AMotionEvent_getDownTime));
    RegisterThunk("AMotionEvent_getEventTime", motion_time(&AMotionEvent_getEventTime));

    auto motion_float = [](float (*fn)(const AInputEvent*)) {
        return [fn](Jit& jit, DynarmicHost&) {
            const AInputEvent* e = Host<AInputEvent>(jit.Regs()[0]);
            jit.Regs()[0] = FloatBits(e ? fn(e) : 0.0f);
        };
    };
    RegisterThunk("AMotionEvent_getXPrecision", motion_float(&AMotionEvent_getXPrecision));
    RegisterThunk("AMotionEvent_getYPrecision", motion_float(&AMotionEvent_getYPrecision));

    // Per-pointer accessors: (event, pointer_index) -> float.
    auto pointer_float = [](float (*fn)(const AInputEvent*, size_t)) {
        return [fn](Jit& jit, DynarmicHost&) {
            const AInputEvent* e = Host<AInputEvent>(jit.Regs()[0]);
            jit.Regs()[0] = FloatBits(e ? fn(e, jit.Regs()[1]) : 0.0f);
        };
    };
    auto pointer_coordinate = [](float (*fn)(const AInputEvent*, size_t), bool y_axis) {
        return [fn, y_axis](Jit& jit, DynarmicHost&) {
            const AInputEvent* e = Host<AInputEvent>(jit.Regs()[0]);
            const size_t pointer_index = jit.Regs()[1];
            const float original = e ? fn(e, pointer_index) : 0.0f;
            jit.Regs()[0] = FloatBits(
                WarbandTouchCoordinateOverride(e, pointer_index, y_axis, original));
        };
    };
    RegisterThunk("AMotionEvent_getX", pointer_coordinate(&AMotionEvent_getX, false));
    RegisterThunk("AMotionEvent_getY", pointer_coordinate(&AMotionEvent_getY, true));
    RegisterThunk("AMotionEvent_getPressure", pointer_float(&AMotionEvent_getPressure));
    RegisterThunk("AMotionEvent_getSize", pointer_float(&AMotionEvent_getSize));
    RegisterThunk("AMotionEvent_getOrientation", pointer_float(&AMotionEvent_getOrientation));
    RegisterThunk("AMotionEvent_getTouchMajor", pointer_float(&AMotionEvent_getTouchMajor));
    RegisterThunk("AMotionEvent_getTouchMinor", pointer_float(&AMotionEvent_getTouchMinor));
    RegisterThunk("AMotionEvent_getToolMajor", pointer_float(&AMotionEvent_getToolMajor));
    RegisterThunk("AMotionEvent_getToolMinor", pointer_float(&AMotionEvent_getToolMinor));
    RegisterThunk("AMotionEvent_getRawX", pointer_coordinate(&AMotionEvent_getRawX, false));
    RegisterThunk("AMotionEvent_getRawY", pointer_coordinate(&AMotionEvent_getRawY, true));

    RegisterThunk("AMotionEvent_getPointerId", [](Jit& jit, DynarmicHost&) {
        const AInputEvent* e = Host<AInputEvent>(jit.Regs()[0]);
        jit.Regs()[0] = e ? static_cast<uint32_t>(AMotionEvent_getPointerId(e, jit.Regs()[1])) : 0;
    });
    RegisterThunk("AMotionEvent_getToolType", [](Jit& jit, DynarmicHost&) {
        const AInputEvent* e = Host<AInputEvent>(jit.Regs()[0]);
        jit.Regs()[0] = e ? static_cast<uint32_t>(AMotionEvent_getToolType(e, jit.Regs()[1])) : 0;
    });
    RegisterThunk("AMotionEvent_getButtonState", [](Jit& jit, DynarmicHost&) {
        const AInputEvent* e = Host<AInputEvent>(jit.Regs()[0]);
        jit.Regs()[0] = e ? static_cast<uint32_t>(AMotionEvent_getButtonState(e)) : 0;
    });
    RegisterThunk("AMotionEvent_getAxisValue", [](Jit& jit, DynarmicHost&) {
        const AInputEvent* e = Host<AInputEvent>(jit.Regs()[0]);
        jit.Regs()[0] = FloatBits(
            e ? AMotionEvent_getAxisValue(e, static_cast<int32_t>(jit.Regs()[1]), jit.Regs()[2]) : 0.0f);
    });

    // Historical samples: (event, pointer_index, history_index).
    auto historical_float = [](float (*fn)(const AInputEvent*, size_t, size_t)) {
        return [fn](Jit& jit, DynarmicHost&) {
            const AInputEvent* e = Host<AInputEvent>(jit.Regs()[0]);
            jit.Regs()[0] = FloatBits(e ? fn(e, jit.Regs()[1], jit.Regs()[2]) : 0.0f);
        };
    };
    auto historical_coordinate = [](
            float (*fn)(const AInputEvent*, size_t, size_t), bool y_axis) {
        return [fn, y_axis](Jit& jit, DynarmicHost&) {
            const AInputEvent* e = Host<AInputEvent>(jit.Regs()[0]);
            const size_t pointer_index = jit.Regs()[1];
            const float original = e ? fn(e, pointer_index, jit.Regs()[2]) : 0.0f;
            jit.Regs()[0] = FloatBits(
                WarbandTouchCoordinateOverride(e, pointer_index, y_axis, original));
        };
    };
    RegisterThunk("AMotionEvent_getHistoricalX",
                  historical_coordinate(&AMotionEvent_getHistoricalX, false));
    RegisterThunk("AMotionEvent_getHistoricalY",
                  historical_coordinate(&AMotionEvent_getHistoricalY, true));
    RegisterThunk("AMotionEvent_getHistoricalPressure", historical_float(&AMotionEvent_getHistoricalPressure));
    RegisterThunk("AMotionEvent_getHistoricalSize", historical_float(&AMotionEvent_getHistoricalSize));
    // libunity imports the C++-mangled names for these; the loader looks them up
    // verbatim, so both spellings have to exist.
    RegisterThunk("_Z30AMotionEvent_getHistoricalRawXP11AInputEventjj",
                  historical_coordinate(&AMotionEvent_getHistoricalRawX, false));
    RegisterThunk("_Z30AMotionEvent_getHistoricalRawYP11AInputEventjj",
                  historical_coordinate(&AMotionEvent_getHistoricalRawY, true));
    RegisterThunk("_Z36AMotionEvent_getHistoricalTouchMajorP11AInputEventjj", historical_float(&AMotionEvent_getHistoricalTouchMajor));
    RegisterThunk("_Z36AMotionEvent_getHistoricalTouchMinorP11AInputEventjj", historical_float(&AMotionEvent_getHistoricalTouchMinor));
    RegisterThunk("_Z35AMotionEvent_getHistoricalToolMajorP11AInputEventjj", historical_float(&AMotionEvent_getHistoricalToolMajor));
    RegisterThunk("_Z35AMotionEvent_getHistoricalToolMinorP11AInputEventjj", historical_float(&AMotionEvent_getHistoricalToolMinor));
    RegisterThunk("_Z37AMotionEvent_getHistoricalOrientationP11AInputEventjj", historical_float(&AMotionEvent_getHistoricalOrientation));
    RegisterThunk("AMotionEvent_getHistoricalEventTime", [](Jit& jit, DynarmicHost&) {
        const AInputEvent* e = Host<AInputEvent>(jit.Regs()[0]);
        SetInt64(jit, e ? AMotionEvent_getHistoricalEventTime(e, jit.Regs()[1]) : 0);
    });

    // --- AConfiguration ----------------------------------------------------
    RegisterThunk("AConfiguration_new", [](Jit& jit, DynarmicHost&) {
        AConfiguration* config = AConfiguration_new();
        const uint32_t handle = GuestHandleFor(config);
        // AConfiguration_new transfers ownership to the caller.  If the bridge
        // cannot represent the pointer in a guest word, there is no caller that
        // can later delete it, so unwind the host allocation here.
        if (config && !handle) AConfiguration_delete(config);
        jit.Regs()[0] = handle;
    });
    RegisterThunk("AConfiguration_delete", [](Jit& jit, DynarmicHost&) {
        if (AConfiguration* config = Host<AConfiguration>(jit.Regs()[0])) {
            // Invalidate the translation before deleting the owned host object.
            // A stale guest copy will then resolve to null instead of freed host
            // memory, while the guest-heap handle cell becomes recyclable.
            ReleaseGuestHandle(config);
            AConfiguration_delete(config);
        }
        jit.Regs()[0] = 0;
    });
    RegisterThunk("AConfiguration_fromAssetManager", [](Jit& jit, DynarmicHost&) {
        AConfiguration* config = Host<AConfiguration>(jit.Regs()[0]);
        AAssetManager* assets = Host<AAssetManager>(jit.Regs()[1]);
        if (config && assets) AConfiguration_fromAssetManager(config, assets);
        jit.Regs()[0] = 0;
    });

    // --- AAssetManager / AAsset --------------------------------------------
    RegisterThunk("AAssetManager_fromJava", [](Jit& jit, DynarmicHost&) {
        JNIEnv* env = HostEnv();
        jobject java_assets = static_cast<jobject>(HostPointerFor(jit.Regs()[1]));
        if (!env || !java_assets) {
            LOGE("AAssetManager_fromJava: no host env or asset manager object");
            jit.Regs()[0] = 0;
            return;
        }
        jit.Regs()[0] = GuestHandleFor(AAssetManager_fromJava(env, java_assets));
    });
    RegisterThunk("AAssetManager_open", [](Jit& jit, DynarmicHost& host) {
        AAssetManager* assets = Host<AAssetManager>(jit.Regs()[0]);
        const char* name = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]));
        if (name && (strstr(name, ".bmp") || strstr(name, "main"))) {
            LOGI("AAssetManager_open probe '%s'", name);
        }
        AAsset* asset = (assets && name) ? AAssetManager_open(assets, name, static_cast<int>(jit.Regs()[2])) : nullptr;
        if (!asset) LOGE("AAssetManager_open('%s') failed", name ? name : "(null)");
        jit.Regs()[0] = GuestHandleFor(asset);
    });
    RegisterThunk("AAsset_read", [](Jit& jit, DynarmicHost& host) {
        AAsset* asset = Host<AAsset>(jit.Regs()[0]);
        uint32_t buf = jit.Regs()[1];
        uint32_t count = jit.Regs()[2];
        int read = (asset && buf) ? AAsset_read(asset, host.GetGuestMemoryBase() + buf, count) : -1;
        jit.Regs()[0] = static_cast<uint32_t>(read);
    });
    RegisterThunk("AAsset_seek", [](Jit& jit, DynarmicHost&) {
        AAsset* asset = Host<AAsset>(jit.Regs()[0]);
        off_t pos = asset ? AAsset_seek(asset, static_cast<int32_t>(jit.Regs()[1]),
                                        static_cast<int>(jit.Regs()[2]))
                          : -1;
        jit.Regs()[0] = static_cast<uint32_t>(pos);
    });
    RegisterThunk("AAsset_getLength", [](Jit& jit, DynarmicHost&) {
        AAsset* asset = Host<AAsset>(jit.Regs()[0]);
        jit.Regs()[0] = static_cast<uint32_t>(asset ? AAsset_getLength(asset) : 0);
    });
    RegisterThunk("AAsset_close", [](Jit& jit, DynarmicHost&) {
        if (AAsset* asset = Host<AAsset>(jit.Regs()[0])) {
            AAsset_close(asset);
            // The handle table is interned by host pointer, so a closed asset that
            // is not dropped both strands its cell and leaves the guest able to
            // reach freed memory if the address is handed out again.
            ReleaseGuestHandle(asset);
        }
        jit.Regs()[0] = 0;
    });

    // --- ANativeWindow -----------------------------------------------------
    RegisterThunk("ANativeWindow_fromSurface", [](Jit& jit, DynarmicHost&) {
        JNIEnv* env = HostEnv();
        jobject surface = static_cast<jobject>(HostPointerFor(jit.Regs()[1]));
        if (!env || !surface) { jit.Regs()[0] = 0; return; }
        jit.Regs()[0] = GuestHandleFor(ANativeWindow_fromSurface(env, surface));
    });
    RegisterThunk("ANativeWindow_acquire", [](Jit& jit, DynarmicHost&) {
        if (ANativeWindow* w = Host<ANativeWindow>(jit.Regs()[0])) ANativeWindow_acquire(w);
        jit.Regs()[0] = 0;
    });
    RegisterThunk("ANativeWindow_release", [](Jit& jit, DynarmicHost&) {
        if (ANativeWindow* w = Host<ANativeWindow>(jit.Regs()[0])) ANativeWindow_release(w);
        jit.Regs()[0] = 0;
    });
    RegisterThunk("ANativeWindow_setBuffersGeometry", [](Jit& jit, DynarmicHost&) {
        ANativeWindow* w = Host<ANativeWindow>(jit.Regs()[0]);
        int32_t r = w ? ANativeWindow_setBuffersGeometry(w, static_cast<int32_t>(jit.Regs()[1]),
                                                         static_cast<int32_t>(jit.Regs()[2]),
                                                         static_cast<int32_t>(jit.Regs()[3]))
                      : -1;
        jit.Regs()[0] = static_cast<uint32_t>(r);
    });
    RegisterThunk("ANativeWindow_getWidth", [](Jit& jit, DynarmicHost&) {
        ANativeWindow* w = Host<ANativeWindow>(jit.Regs()[0]);
        jit.Regs()[0] = static_cast<uint32_t>(w ? ANativeWindow_getWidth(w) : 0);
    });
    RegisterThunk("ANativeWindow_getHeight", [](Jit& jit, DynarmicHost&) {
        ANativeWindow* w = Host<ANativeWindow>(jit.Regs()[0]);
        jit.Regs()[0] = static_cast<uint32_t>(w ? ANativeWindow_getHeight(w) : 0);
    });
    RegisterThunk("ANativeWindow_getFormat", [](Jit& jit, DynarmicHost&) {
        ANativeWindow* w = Host<ANativeWindow>(jit.Regs()[0]);
        jit.Regs()[0] = static_cast<uint32_t>(w ? ANativeWindow_getFormat(w) : 0);
    });

    // --- ASensorManager ----------------------------------------------------
    // The game is touch-only; reporting no sensors is what makes Unity skip the
    // whole accelerometer path rather than poll an event queue that never fills.
    auto no_sensor = [](Jit& jit, DynarmicHost&) { jit.Regs()[0] = 0; };
    for (const char* name : {"ASensorManager_getInstance", "ASensorManager_getDefaultSensor",
                             "ASensorManager_getSensorList", "ASensorManager_createEventQueue",
                             "ASensorManager_destroyEventQueue", "ASensorEventQueue_enableSensor",
                             "ASensorEventQueue_disableSensor", "ASensorEventQueue_setEventRate",
                             "ASensorEventQueue_hasEvents", "ASensorEventQueue_getEvents",
                             "ASensor_getName", "ASensor_getVendor", "ASensor_getType",
                             "ASensor_getResolution", "ASensor_getMinDelay"}) {
        RegisterThunk(name, no_sensor);
    }

    // No audio backend is registered until Colin's imports prove one is needed.

    LOGI("libandroid thunks registered");
}
