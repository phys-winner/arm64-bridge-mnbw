// EGL for the guest.
//
// Without this Unity gets EGL_NO_DISPLAY from the very first call, prints
// "[EGL] Unable to retrieve the default EGL display!" and calls exit(1) - which
// in a bridge does not end the process, it just unwinds and lets the whole
// start-up run again, so the log fills with a second mono_init asserting on
// icalls it already registered. Every symptom downstream of the first frame
// starts here.
//
// EGLDisplay, EGLConfig, EGLSurface and EGLContext are all opaque host pointers
// and do not fit in a guest word, so they travel as handles - the same table
// that already carries ANativeWindow and jobject. Attribute lists are EGL_NONE
// terminated arrays in guest memory and are copied out before use.

#include "thunk_manager.h"
#include "frame_limit.h"
#include "gl_deferred_deletes.h"
#include "dynarmic_host.h"
#include "thunk_types.h"
#include "native_activity_bridge.h"
#include "default_framebuffer_background.h"
#include "shader_diagnostics.h"
#include "startup_frame_guard.h"
#include "fxaa_present.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <android/native_window.h>
#include <android/log.h>
#include <sys/system_properties.h>

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <dlfcn.h>

#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <time.h>

#define LOG_TAG "CMR_Egl"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

using Jit = Dynarmic::A32::Jit;

namespace {

using NativeWindowSetFrameRate = int32_t (*)(ANativeWindow*, float, int8_t);

struct WindowSurface {
    ANativeWindow* window = nullptr;
    int last_requested_fps = std::numeric_limits<int>::min();
};

std::mutex g_window_surfaces_mutex;
std::unordered_map<EGLSurface, WindowSurface> g_window_surfaces;

struct PresentationGuard {
    std::chrono::steady_clock::time_point previous_swap_call{};
    warband::startup_frame_guard::State state{};
    bool suppression_reported = false;
};

thread_local PresentationGuard g_presentation_guard;

#ifndef WARBAND_BRIDGE_NO_LOGS
struct MemoryLeakProbeState {
    bool configured = false;
    std::string route;
    uint32_t frame = 0;
    uint32_t delay_frames = 900;
    uint32_t interval_frames = 90;
    uint32_t requested_iterations = 12;
    uint32_t completed_iterations = 0;
    uint32_t target = 0;
    uint32_t secondary_target = 0;
};

uint32_t DiagnosticPropertyU32(const char* name, uint32_t fallback,
                               uint32_t minimum, uint32_t maximum) {
    char value[PROP_VALUE_MAX] = {};
    if (__system_property_get(name, value) <= 0) return fallback;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (!end || end == value || *end != '\0') return fallback;
    return static_cast<uint32_t>(
        std::clamp<unsigned long>(parsed, minimum, maximum));
}

void MaybeRunMemoryLeakProbe(DynarmicHost& host) {
    static thread_local MemoryLeakProbeState state;
    if (!state.configured) {
        state.configured = true;
        char route[PROP_VALUE_MAX] = {};
        if (__system_property_get("debug.warband.memory_leak_probe", route) <= 0) {
            return;
        }
        if (std::strcmp(route, "graphics_reset") != 0 &&
            std::strcmp(route, "shader_rebuild") != 0 &&
            std::strcmp(route, "combined") != 0) {
            LOGE("memory_leak_probe invalid_route='%s'", route);
            return;
        }
        state.route = route;
        state.delay_frames = DiagnosticPropertyU32(
            "debug.warband.memory_leak_probe_delay_frames", 900, 60, 10000);
        state.interval_frames = DiagnosticPropertyU32(
            "debug.warband.memory_leak_probe_interval_frames", 90, 15, 3600);
        state.requested_iterations = DiagnosticPropertyU32(
            "debug.warband.memory_leak_probe_count", 12, 1, 100);
        const char* symbol = state.route == "shader_rebuild" ?
            "_Z20initialize_dx_effectv" : "_Z20refresh_video_memoryv";
        state.target = host.FindSymbol(symbol);
        if (state.route == "combined") {
            state.secondary_target = host.FindSymbol("_Z20initialize_dx_effectv");
        }
        LOGI("memory_leak_probe configured route=%s target=0x%08x "
             "secondary=0x%08x delay=%u interval=%u count=%u",
             state.route.c_str(), state.target, state.secondary_target,
             state.delay_frames, state.interval_frames,
             state.requested_iterations);
    }
    if (state.route.empty() || state.target == 0 ||
        (state.route == "combined" && state.secondary_target == 0) ||
        state.completed_iterations >= state.requested_iterations) {
        return;
    }

    ++state.frame;
    const uint64_t target_frame = static_cast<uint64_t>(state.delay_frames) +
        static_cast<uint64_t>(state.completed_iterations) * state.interval_frames;
    if (state.frame < target_frame) return;

    const bool secondary_phase = state.secondary_target != 0 &&
        (state.completed_iterations & 1u) != 0;
    host.CallGuestNested(secondary_phase ? state.secondary_target : state.target);
    ++state.completed_iterations;
    LOGI("memory_leak_probe fired route=%s phase=%s iteration=%u/%u frame=%u",
         state.route.c_str(),
         secondary_phase ? "shader_rebuild" : "graphics_reset",
         state.completed_iterations,
         state.requested_iterations, state.frame);
}
#else
void MaybeRunMemoryLeakProbe(DynarmicHost&) {}
#endif

bool SuppressTransientStartupFrame() {
    using namespace std::chrono_literals;
    const auto now = std::chrono::steady_clock::now();
    PresentationGuard& guard = g_presentation_guard;
    const bool resumed_after_gap =
        guard.previous_swap_call.time_since_epoch().count() != 0 &&
        now - guard.previous_swap_call >= 500ms;
    if (resumed_after_gap) {
        warband::startup_frame_guard::Arm(guard.state);
        guard.suppression_reported = false;
    }
    guard.previous_swap_call = now;
    if (!guard.state.armed) return false;

    GLint framebuffer = 0;
    GLint viewport[4] = {};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
    glGetIntegerv(GL_VIEWPORT, viewport);
    if (framebuffer != 0 || viewport[2] < 64 || viewport[3] < 64) {
        guard.state.armed = false;
        return false;
    }

    const GLint sample_x = viewport[0] + viewport[2] * 12 / 100;
    const GLint sample_y = viewport[1] + viewport[3] * 20 / 100;
    const GLsizei sample_width = viewport[2] * 68 / 100;
    constexpr GLsizei kSampleHeight = 4;
    std::vector<std::uint8_t> pixels(
        static_cast<std::size_t>(sample_width) * kSampleHeight * 4u);
    glReadPixels(sample_x, sample_y, sample_width, kSampleHeight,
                 GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    const auto classified = warband::startup_frame_guard::ClassifyRgbaStrip(
        pixels.data(), pixels.size() / 4u);
    const bool suppress = warband::startup_frame_guard::Decide(
        guard.state, classified) ==
        warband::startup_frame_guard::Decision::kSuppress;
    if (suppress && !guard.suppression_reported &&
        classified.font_atlas_artifact) {
        guard.suppression_reported = true;
        LOGI("withholding transient font-atlas HUD frame (%zu/%zu chroma pixels)",
             classified.artifact_pixels, classified.sampled_pixels);
    }
    if (!guard.state.armed && guard.suppression_reported) {
        LOGI("first stable post-load frame released after %u withheld frames",
             guard.state.suppressed_frames);
    }
    return suppress;
}

bool UseGles2CompatibilityMode() {
    static const bool forced = [] {
        char value[PROP_VALUE_MAX] = {};
        if (__system_property_get("debug.warband.force_gles2", value) > 0) {
            return std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0;
        }
        // The mt6785 GLES3 path can reach the main menu with a black guest
        // surface; its GLES2 path renders the same stock game correctly.
        return __system_property_get("ro.board.platform", value) > 0 &&
               std::strcmp(value, "mt6785") == 0;
    }();
    return forced;
}

void ForceAttribute(std::vector<EGLint>& attributes, EGLint name, EGLint value) {
    for (size_t index = 0; index + 1 < attributes.size(); index += 2) {
        if (attributes[index] == EGL_NONE) break;
        if (attributes[index] == name) {
            attributes[index + 1] = value;
            return;
        }
    }
    if (!attributes.empty() && attributes.back() == EGL_NONE) attributes.pop_back();
    attributes.push_back(name);
    attributes.push_back(value);
    attributes.push_back(EGL_NONE);
}

NativeWindowSetFrameRate ResolveNativeWindowSetFrameRate() {
    static const NativeWindowSetFrameRate set_frame_rate = [] {
        // The bridge targets API 26, while this API was added in API 30. Resolve
        // it at runtime so older Android releases keep the existing EGL path.
        void* android_library = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
        if (!android_library) {
            LOGI("ANativeWindow_setFrameRate is unavailable: %s", dlerror());
            return static_cast<NativeWindowSetFrameRate>(nullptr);
        }
        auto* function = reinterpret_cast<NativeWindowSetFrameRate>(
            dlsym(android_library, "ANativeWindow_setFrameRate"));
        if (!function) {
            LOGI("ANativeWindow_setFrameRate is unavailable on this Android version");
        }
        return function;
    }();
    return set_frame_rate;
}

void TrackWindowSurface(EGLSurface surface, ANativeWindow* window) {
    if (surface == EGL_NO_SURFACE || !window) return;

    ANativeWindow_acquire(window);
    ANativeWindow* replaced_window = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_window_surfaces_mutex);
        const auto [it, inserted] = g_window_surfaces.try_emplace(
            surface, WindowSurface{window, std::numeric_limits<int>::min()});
        if (!inserted) {
            replaced_window = it->second.window;
            it->second = {window, std::numeric_limits<int>::min()};
        }
    }
    if (replaced_window) ANativeWindow_release(replaced_window);
}

void UntrackWindowSurface(EGLSurface surface) {
    ANativeWindow* window = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_window_surfaces_mutex);
        const auto it = g_window_surfaces.find(surface);
        if (it != g_window_surfaces.end()) {
            window = it->second.window;
            g_window_surfaces.erase(it);
        }
    }
    if (window) ANativeWindow_release(window);
}

void ApplyWindowFrameRate(EGLSurface surface) {
    const NativeWindowSetFrameRate set_frame_rate = ResolveNativeWindowSetFrameRate();
    if (!set_frame_rate) return;

    const int fps = warband::frame_limit::TargetFps();
    std::lock_guard<std::mutex> lock(g_window_surfaces_mutex);
    const auto it = g_window_surfaces.find(surface);
    if (it == g_window_surfaces.end() || it->second.last_requested_fps == fps) return;

    const float requested_rate = fps > 0 ? static_cast<float>(fps) : 0.0f;
    const int8_t compatibility = fps > 0
        ? ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE
        : ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT;
    const int32_t result = set_frame_rate(it->second.window, requested_rate, compatibility);
    it->second.last_requested_fps = fps;
    if (result == 0) {
        LOGI("requested native window frame rate: %s",
             fps > 0 ? (fps == 30 ? "30 fps" : fps == 60 ? "60 fps" :
                        fps == 90 ? "90 fps" : "custom fps") : "display rate");
    } else {
        LOGE("ANativeWindow_setFrameRate(%d) failed: %d", fps, result);
    }
}

template <typename T>
T HandleTo(uint32_t handle) {
    return static_cast<T>(HostPointerFor(handle));
}

// EGL_DEFAULT_DISPLAY is 0 and so is "no handle", which happens to be the same
// value on both sides - but only by luck, so it is spelled out rather than
// relied on.
EGLNativeDisplayType NativeDisplay(uint32_t guest_value) {
    return guest_value == 0 ? EGL_DEFAULT_DISPLAY
                            : static_cast<EGLNativeDisplayType>(HostPointerFor(guest_value));
}

// Attribute lists end at EGL_NONE. The cap is a guard against a guest pointer
// that is not a list at all; a real EGL config request is a dozen entries.
std::vector<EGLint> ReadAttribList(DynarmicHost& host, uint32_t addr) {
    std::vector<EGLint> attribs;
    if (!addr) {
        attribs.push_back(EGL_NONE);
        return attribs;
    }
    for (int i = 0; i < 128; ++i) {
        auto value = static_cast<EGLint>(host.MemoryRead32(addr + i * 4));
        attribs.push_back(value);
        if (value == EGL_NONE) return attribs;
    }
    LOGE("attribute list at 0x%08X is not EGL_NONE terminated", addr);
    attribs.push_back(EGL_NONE);
    return attribs;
}

}  // namespace

void ThunkManager::RegisterEglThunks() {
    RegisterThunk("eglGetDisplay", [](Jit& jit, DynarmicHost&) {
        EGLDisplay dpy = eglGetDisplay(NativeDisplay(jit.Regs()[0]));
        jit.Regs()[0] = GuestHandleFor(dpy);
    });

    RegisterThunk("eglInitialize", [](Jit& jit, DynarmicHost& host) {
        EGLint major = 0, minor = 0;
        EGLBoolean ok = eglInitialize(HandleTo<EGLDisplay>(jit.Regs()[0]), &major, &minor);
        if (jit.Regs()[1]) host.MemoryWrite32(jit.Regs()[1], static_cast<uint32_t>(major));
        if (jit.Regs()[2]) host.MemoryWrite32(jit.Regs()[2], static_cast<uint32_t>(minor));
        jit.Regs()[0] = ok;
    });

    RegisterThunk("eglTerminate", [](Jit& jit, DynarmicHost&) {
        const EGLBoolean ok = eglTerminate(HandleTo<EGLDisplay>(jit.Regs()[0]));
        if (ok) bridge_gl::NotifyShaderContextLifecycle();
        jit.Regs()[0] = ok;
    });

    // eglChooseConfig(dpy, attrib_list, configs, config_size, num_config): the
    // last argument is on the stack, and `configs` is an array of guest words
    // that has to receive handles rather than the host pointers themselves.
    RegisterThunk("eglChooseConfig", [](Jit& jit, DynarmicHost& host) {
        EGLDisplay dpy = HandleTo<EGLDisplay>(jit.Regs()[0]);
        std::vector<EGLint> attribs = ReadAttribList(host, jit.Regs()[1]);
        if (UseGles2CompatibilityMode()) {
            ForceAttribute(attribs, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT);
        }
        uint32_t configs_addr = jit.Regs()[2];
        EGLint config_size = static_cast<EGLint>(jit.Regs()[3]);
        uint32_t num_addr = host.MemoryRead32(jit.Regs()[13]);

        std::vector<EGLConfig> configs(config_size > 0 && configs_addr ? config_size : 0);
        EGLint num = 0;
        EGLBoolean ok = eglChooseConfig(dpy, attribs.data(), configs.empty() ? nullptr : configs.data(),
                                        configs.empty() ? 0 : config_size, &num);
        if (ok) {
            for (EGLint i = 0; i < num && i < static_cast<EGLint>(configs.size()); ++i) {
                host.MemoryWrite32(configs_addr + i * 4, GuestHandleFor(configs[i]));
            }
        }
        if (num_addr) host.MemoryWrite32(num_addr, static_cast<uint32_t>(num));
        jit.Regs()[0] = ok;
    });

    // eglGetConfigs has the same opaque-handle array ABI as eglChooseConfig,
    // just without an attribute list.
    RegisterThunk("eglGetConfigs", [](Jit& jit, DynarmicHost& host) {
        EGLDisplay dpy = HandleTo<EGLDisplay>(jit.Regs()[0]);
        uint32_t configs_addr = jit.Regs()[1];
        EGLint config_size = static_cast<EGLint>(jit.Regs()[2]);
        uint32_t num_addr = jit.Regs()[3];

        std::vector<EGLConfig> configs(config_size > 0 && configs_addr ? config_size : 0);
        EGLint num = 0;
        EGLBoolean ok = eglGetConfigs(dpy, configs.empty() ? nullptr : configs.data(),
                                      configs.empty() ? 0 : config_size, &num);
        if (ok) {
            for (EGLint i = 0; i < num && i < static_cast<EGLint>(configs.size()); ++i) {
                host.MemoryWrite32(configs_addr + i * 4, GuestHandleFor(configs[i]));
            }
        }
        if (num_addr) host.MemoryWrite32(num_addr, static_cast<uint32_t>(num));
        jit.Regs()[0] = ok;
    });

    RegisterThunk("eglGetConfigAttrib", [](Jit& jit, DynarmicHost& host) {
        EGLint value = 0;
        EGLBoolean ok = eglGetConfigAttrib(HandleTo<EGLDisplay>(jit.Regs()[0]),
                                           HandleTo<EGLConfig>(jit.Regs()[1]),
                                           static_cast<EGLint>(jit.Regs()[2]), &value);
        if (jit.Regs()[3]) host.MemoryWrite32(jit.Regs()[3], static_cast<uint32_t>(value));
        jit.Regs()[0] = ok;
    });

    RegisterThunk("eglQuerySurface", [](Jit& jit, DynarmicHost& host) {
        EGLint value = 0;
        EGLBoolean ok = eglQuerySurface(HandleTo<EGLDisplay>(jit.Regs()[0]),
                                        HandleTo<EGLSurface>(jit.Regs()[1]),
                                        static_cast<EGLint>(jit.Regs()[2]), &value);
        if (jit.Regs()[3]) host.MemoryWrite32(jit.Regs()[3], static_cast<uint32_t>(value));
        jit.Regs()[0] = ok;
    });

    RegisterThunk("eglCreateWindowSurface", [](Jit& jit, DynarmicHost& host) {
        auto* window = HandleTo<ANativeWindow*>(jit.Regs()[2]);
        const EGLDisplay display = HandleTo<EGLDisplay>(jit.Regs()[0]);
        const EGLConfig config = HandleTo<EGLConfig>(jit.Regs()[1]);
        std::vector<EGLint> attribs = ReadAttribList(host, jit.Regs()[3]);
        EGLSurface surface = eglCreateWindowSurface(display, config, window,
                                                   attribs.data());
        if (surface == EGL_NO_SURFACE) LOGE("eglCreateWindowSurface failed: 0x%04X", eglGetError());
        else {
            TrackWindowSurface(surface, window);
            ApplyWindowFrameRate(surface);
        }
        jit.Regs()[0] = GuestHandleFor(surface);
    });

    RegisterThunk("eglDestroySurface", [](Jit& jit, DynarmicHost&) {
        EGLSurface surface = HandleTo<EGLSurface>(jit.Regs()[1]);
        const EGLBoolean ok = eglDestroySurface(HandleTo<EGLDisplay>(jit.Regs()[0]), surface);
        // A successful destroy makes the EGLSurface handle unusable even when
        // physical deletion is deferred because it is current on another
        // thread.  Retaining the bridge translation would leak its guest cell
        // and expose a stale opaque handle if the driver recycles the address.
        if (ok && surface != EGL_NO_SURFACE) {
            UntrackWindowSurface(surface);
            ReleaseGuestHandle(surface);
        }
        jit.Regs()[0] = ok;
    });

    RegisterThunk("eglCreateContext", [](Jit& jit, DynarmicHost& host) {
        std::vector<EGLint> attribs = ReadAttribList(host, jit.Regs()[3]);
        if (UseGles2CompatibilityMode()) {
            constexpr EGLint kEglContextMinorVersionKhr = 0x30FB;
            ForceAttribute(attribs, EGL_CONTEXT_CLIENT_VERSION, 2);
            ForceAttribute(attribs, kEglContextMinorVersionKhr, 0);
            LOGI("using OpenGL ES 2 compatibility context");
        }
        EGLContext ctx = eglCreateContext(HandleTo<EGLDisplay>(jit.Regs()[0]),
                                          HandleTo<EGLConfig>(jit.Regs()[1]),
                                          HandleTo<EGLContext>(jit.Regs()[2]), attribs.data());
        if (ctx == EGL_NO_CONTEXT) LOGE("eglCreateContext failed: 0x%04X", eglGetError());
        else {
            if (UseGles2CompatibilityMode()) {
                EGLint actual_major = 0;
                EGLint actual_minor = 0;
                constexpr EGLint kEglContextMinorVersionKhr = 0x30FB;
                eglQueryContext(HandleTo<EGLDisplay>(jit.Regs()[0]), ctx,
                                EGL_CONTEXT_CLIENT_VERSION, &actual_major);
                eglQueryContext(HandleTo<EGLDisplay>(jit.Regs()[0]), ctx,
                                kEglContextMinorVersionKhr, &actual_minor);
                LOGI("compatibility EGL context created as %d.%d",
                     actual_major, actual_minor);
            }
            bridge_gl::NotifyShaderContextLifecycle();
        }
        jit.Regs()[0] = GuestHandleFor(ctx);
    });

    RegisterThunk("eglDestroyContext", [](Jit& jit, DynarmicHost&) {
        EGLContext context = HandleTo<EGLContext>(jit.Regs()[1]);
        bridge_gl::DrainDeferredDeletes();
        EGLBoolean ok = eglDestroyContext(HandleTo<EGLDisplay>(jit.Regs()[0]), context);
        if (ok && context != EGL_NO_CONTEXT) {
            bridge_gl::DiscardDeferredDeletes();
            ReleaseGuestHandle(context);
            bridge_gl::NotifyShaderContextLifecycle();
        }
        jit.Regs()[0] = ok;
    });

    RegisterThunk("eglMakeCurrent", [](Jit& jit, DynarmicHost&) {
        const EGLContext previous = eglGetCurrentContext();
        const EGLContext context = HandleTo<EGLContext>(jit.Regs()[3]);
        EGLBoolean ok = eglMakeCurrent(HandleTo<EGLDisplay>(jit.Regs()[0]),
                                       HandleTo<EGLSurface>(jit.Regs()[1]),
                                       HandleTo<EGLSurface>(jit.Regs()[2]),
                                       context);
        if (!ok) LOGE("eglMakeCurrent failed: 0x%04X", eglGetError());
        else {
            if (context != EGL_NO_CONTEXT) bridge_gl::DrainDeferredDeletes();
            if (previous != context) bridge_gl::NotifyShaderContextLifecycle();
        }
        jit.Regs()[0] = ok;
    });

    RegisterThunk("eglSwapBuffers", [](Jit& jit, DynarmicHost& host) {
        const EGLSurface surface = HandleTo<EGLSurface>(jit.Regs()[1]);
        bridge_gl::DrainDeferredDeletes();
        ApplyWindowFrameRate(surface);
        warband::frame_limit::WaitBeforeSwap();
        MaybeRunMemoryLeakProbe(host);
        if (SuppressTransientStartupFrame()) {
            // Keep the last complete loading/menu buffer on screen.  The guest
            // continues rendering into the same back buffer until its dynamic
            // HUD state is stable, then the next real swap publishes it.
            jit.Regs()[0] = EGL_TRUE;
            return;
        }
        const EGLDisplay display = HandleTo<EGLDisplay>(jit.Regs()[0]);
        bridge_gl::ApplyFxaaBeforeSwap(display, surface);
        const EGLBoolean swapped = eglSwapBuffers(display, surface);
        if (swapped == EGL_TRUE) {
            bridge_gl::NotifyDefaultFramebufferFramePresented();
        }
        jit.Regs()[0] = swapped;
    });

    RegisterThunk("eglGetError", [](Jit& jit, DynarmicHost&) {
        jit.Regs()[0] = static_cast<uint32_t>(eglGetError());
    });

    auto return_u64 = [](Jit& jit, uint64_t value) {
        jit.Regs()[0] = static_cast<uint32_t>(value);
        jit.Regs()[1] = static_cast<uint32_t>(value >> 32);
    };
    RegisterThunk("eglGetSystemTimeFrequencyNV", [return_u64](Jit& jit, DynarmicHost&) {
        return_u64(jit, 1000000000ULL);
    });
    RegisterThunk("eglGetSystemTimeNV", [return_u64](Jit& jit, DynarmicHost&) {
        timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return_u64(jit, static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
                            static_cast<uint64_t>(ts.tv_nsec));
    });

    // The address handed back is one the guest will branch to, so it has to be a
    // guest address - a host function pointer would be executed as ARM code.
    // Only names that already have a thunk get one: minting a trampoline for
    // anything else would turn "extension missing" into "extension present and
    // silently does nothing", which is far harder to diagnose.
    RegisterThunk("eglGetProcAddress", [this](Jit& jit, DynarmicHost& host) {
        const char* name = reinterpret_cast<const char*>(
            GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        if (!name) { jit.Regs()[0] = 0; return; }
        if (!IsRegistered(name)) {
            jit.Regs()[0] = 0;
            return;
        }
        jit.Regs()[0] = GetOrCreateThunkAddress(name);
    });
}
