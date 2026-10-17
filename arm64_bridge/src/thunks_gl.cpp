#include "thunk_manager.h"
#include "dynarmic_host.h"
#include "thunk_types.h"
#include "bridge_config.h"
#include "compressed_texture_support.h"
#include "default_framebuffer_background.h"
#include "etc1_encoder.h"
#include "gl_deferred_deletes.h"
#include "map_water_sampler_portability.h"
#include "mod_astc_encoder.h"
#include "mod_texture_bridge.h"
#include "postfx_portability.h"
#include "renderer_preferences.h"
#include "shader_diagnostics.h"
#include "shader_source_fixes.h"
#include "shadow_render_target.h"
#include "startup_frame_guard.h"
#include "astc.h"
#include "etc.h"
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <android/log.h>
#include <sys/system_properties.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <cmath>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>


// With a custom UI scale the guest believes the screen is GetVirtual{Width,Height}() and sizes
// its viewport accordingly, but the real default framebuffer is still the surface size. Offscreen
// targets are allocated in the guest's own units, so only the window-system framebuffer is mapped.
static void ScaleViewportToSurface(GLint& x, GLint& y, GLsizei& w, GLsizei& h) {
    GLint bound_framebuffer = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &bound_framebuffer);
    if (bound_framebuffer != 0) return;
    const uint32_t vw = GetVirtualWidth(), vh = GetVirtualHeight();
    const uint32_t rw = ThunkManager::s_screenWidth, rh = ThunkManager::s_screenHeight;
    if (!vw || !vh || !rw || !rh || (vw == rw && vh == rh)) return;
    x = static_cast<GLint>((float)x * rw / vw);
    w = static_cast<GLsizei>((float)w * rw / vw);
    y = static_cast<GLint>((float)y * rh / vh);
    h = static_cast<GLsizei>((float)h * rh / vh);
}
#define LOG_TAG "CMR_GLThunks"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {
std::atomic<uint64_t> g_shader_context_generation{1};
thread_local bool g_default_framebuffer_draw_started = false;

bool GlResourceTraceEnabled();

enum class DeferredDeleteKind : uint8_t {
    Buffer,
    Framebuffer,
    Renderbuffer,
    Texture,
    Shader,
    Program,
};

struct DeferredDeleteCommand {
    DeferredDeleteKind kind{};
    std::vector<GLuint> objects;
};

std::mutex g_deferred_delete_mutex;
std::deque<DeferredDeleteCommand> g_deferred_deletes;
uint64_t g_deferred_delete_commands = 0;
uint64_t g_deferred_delete_objects = 0;
uint64_t g_drained_delete_commands = 0;
uint64_t g_drained_delete_objects = 0;
uint64_t g_last_logged_drained_delete_objects = 0;

bool DeferDeleteWithoutContext(DeferredDeleteKind kind, GLsizei count,
                               const GLuint* objects) {
    if (eglGetCurrentContext() != EGL_NO_CONTEXT || count <= 0 || !objects) {
        return false;
    }
    DeferredDeleteCommand command;
    command.kind = kind;
    command.objects.assign(objects, objects + count);
    {
        std::lock_guard<std::mutex> lock(g_deferred_delete_mutex);
        ++g_deferred_delete_commands;
        g_deferred_delete_objects += command.objects.size();
        g_deferred_deletes.emplace_back(std::move(command));
    }
    return true;
}

bool DeferDeleteWithoutContext(DeferredDeleteKind kind, GLuint object) {
    return DeferDeleteWithoutContext(kind, 1, &object);
}
}

namespace bridge_gl {

bool ReplaceDefaultFramebufferColourClear() noexcept {
    if (eglGetCurrentContext() == EGL_NO_CONTEXT) return false;

    GLint framebuffer = -1;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);

    const EGLDisplay display = eglGetCurrentDisplay();
    const EGLSurface surface = eglGetCurrentSurface(EGL_DRAW);
    if (display == EGL_NO_DISPLAY || surface == EGL_NO_SURFACE) return false;

    EGLint width = 0;
    EGLint height = 0;
    if (eglQuerySurface(display, surface, EGL_WIDTH, &width) != EGL_TRUE ||
        eglQuerySurface(display, surface, EGL_HEIGHT, &height) != EGL_TRUE ||
        !warband::default_framebuffer_background::ShouldReplaceColourClear(
            framebuffer, width, height)) {
        return false;
    }

    const GLboolean scissor_enabled = glIsEnabled(GL_SCISSOR_TEST);
    GLfloat clear_colour[4] = {};
    GLboolean colour_mask[4] = {};
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear_colour);
    glGetBooleanv(GL_COLOR_WRITEMASK, colour_mask);

    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_SCISSOR_TEST);

    // Opaque black masks undefined default-framebuffer pixels outside the
    // legacy viewport; no viewport, projection, or guest layout value changes.
    constexpr GLfloat kBackgroundColour[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    glClearColor(kBackgroundColour[0], kBackgroundColour[1],
                 kBackgroundColour[2], kBackgroundColour[3]);
    glClear(GL_COLOR_BUFFER_BIT);

    glClearColor(clear_colour[0], clear_colour[1], clear_colour[2],
                 clear_colour[3]);
    glColorMask(colour_mask[0], colour_mask[1], colour_mask[2], colour_mask[3]);
    if (scissor_enabled) glEnable(GL_SCISSOR_TEST);
    return true;
}

bool ClearDefaultFramebufferBeforeDraw(int framebuffer) noexcept {
    if (g_default_framebuffer_draw_started || framebuffer != 0 ||
        eglGetCurrentContext() == EGL_NO_CONTEXT) {
        return false;
    }

    const EGLDisplay display = eglGetCurrentDisplay();
    const EGLSurface surface = eglGetCurrentSurface(EGL_DRAW);
    if (display == EGL_NO_DISPLAY || surface == EGL_NO_SURFACE) return false;

    EGLint width = 0;
    EGLint height = 0;
    if (eglQuerySurface(display, surface, EGL_WIDTH, &width) != EGL_TRUE ||
        eglQuerySurface(display, surface, EGL_HEIGHT, &height) != EGL_TRUE) {
        return false;
    }

    g_default_framebuffer_draw_started = true;
    if (!warband::default_framebuffer_background::
            ShouldClearBeforeDefaultDraw(
                true, framebuffer, width, height)) {
        return false;
    }

    const GLboolean scissor_enabled = glIsEnabled(GL_SCISSOR_TEST);
    GLfloat clear_colour[4] = {};
    GLboolean colour_mask[4] = {};
    GLboolean depth_mask = GL_TRUE;
    GLfloat depth_clear = 1.0f;
    GLint stencil_mask = 0;
    GLint stencil_clear = 0;
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear_colour);
    glGetBooleanv(GL_COLOR_WRITEMASK, colour_mask);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);
    glGetFloatv(GL_DEPTH_CLEAR_VALUE, &depth_clear);
    glGetIntegerv(GL_STENCIL_WRITEMASK, &stencil_mask);
    glGetIntegerv(GL_STENCIL_CLEAR_VALUE, &stencil_clear);

    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glStencilMask(~0u);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClearDepthf(1.0f);
    glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    glClearColor(clear_colour[0], clear_colour[1], clear_colour[2],
                 clear_colour[3]);
    glClearDepthf(depth_clear);
    glClearStencil(stencil_clear);
    glColorMask(colour_mask[0], colour_mask[1], colour_mask[2], colour_mask[3]);
    glDepthMask(depth_mask);
    glStencilMask(static_cast<GLuint>(stencil_mask));
    if (scissor_enabled) glEnable(GL_SCISSOR_TEST);

    static thread_local uint32_t logged_frames = 0;
    if (logged_frames < 3u) {
        LOGI("wide_default_frame_cleared size=%dx%d framebuffer=%d",
             width, height, framebuffer);
        ++logged_frames;
    }
    return true;
}

void NotifyDefaultFramebufferFramePresented() noexcept {
    g_default_framebuffer_draw_started = false;
}

uint64_t ShaderContextGeneration() noexcept {
    return g_shader_context_generation.load(std::memory_order_relaxed);
}

void NotifyShaderContextLifecycle() noexcept {
    g_shader_context_generation.fetch_add(1, std::memory_order_relaxed);
    g_default_framebuffer_draw_started = false;
}

void DrainDeferredDeletes() noexcept {
    if (eglGetCurrentContext() == EGL_NO_CONTEXT) return;

    std::deque<DeferredDeleteCommand> commands;
    uint64_t queued_commands = 0;
    uint64_t queued_objects = 0;
    {
        std::lock_guard<std::mutex> lock(g_deferred_delete_mutex);
        if (g_deferred_deletes.empty()) return;
        commands.swap(g_deferred_deletes);
        queued_commands = g_deferred_delete_commands;
        queued_objects = g_deferred_delete_objects;
    }

    uint64_t drained_objects = 0;
    for (const DeferredDeleteCommand& command : commands) {
        const GLsizei count = static_cast<GLsizei>(command.objects.size());
        const GLuint* objects = command.objects.data();
        drained_objects += command.objects.size();
        switch (command.kind) {
            case DeferredDeleteKind::Buffer:
                glDeleteBuffers(count, objects);
                break;
            case DeferredDeleteKind::Framebuffer:
                glDeleteFramebuffers(count, objects);
                break;
            case DeferredDeleteKind::Renderbuffer:
                glDeleteRenderbuffers(count, objects);
                break;
            case DeferredDeleteKind::Texture:
                glDeleteTextures(count, objects);
                break;
            case DeferredDeleteKind::Shader:
                for (GLuint object : command.objects) glDeleteShader(object);
                break;
            case DeferredDeleteKind::Program:
                for (GLuint object : command.objects) glDeleteProgram(object);
                break;
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_deferred_delete_mutex);
        g_drained_delete_commands += commands.size();
        g_drained_delete_objects += drained_objects;
        constexpr uint64_t kDeferredDeleteLogInterval = 4096;
        const bool should_log = GlResourceTraceEnabled() &&
            (g_last_logged_drained_delete_objects == 0 ||
             g_drained_delete_objects - g_last_logged_drained_delete_objects >=
                 kDeferredDeleteLogInterval);
        if (should_log) {
            g_last_logged_drained_delete_objects = g_drained_delete_objects;
            LOGI("gl_deferred_delete drained_commands=%zu drained_objects=%llu "
                 "total_enqueued_commands=%llu total_enqueued_objects=%llu "
                 "total_drained_commands=%llu total_drained_objects=%llu pending=%zu",
                 commands.size(), static_cast<unsigned long long>(drained_objects),
                 static_cast<unsigned long long>(queued_commands),
                 static_cast<unsigned long long>(queued_objects),
                 static_cast<unsigned long long>(g_drained_delete_commands),
                 static_cast<unsigned long long>(g_drained_delete_objects),
                 g_deferred_deletes.size());
        }
    }
}

void DiscardDeferredDeletes() noexcept {
    std::lock_guard<std::mutex> lock(g_deferred_delete_mutex);
    if (!g_deferred_deletes.empty()) {
        uint64_t discarded_objects = 0;
        for (const DeferredDeleteCommand& command : g_deferred_deletes) {
            discarded_objects += command.objects.size();
        }
        if (GlResourceTraceEnabled()) {
            LOGI("gl_deferred_delete discarded_commands=%zu discarded_objects=%llu",
                 g_deferred_deletes.size(),
                 static_cast<unsigned long long>(discarded_objects));
        }
        g_deferred_deletes.clear();
    }
}

}  // namespace bridge_gl

namespace {

constexpr uint64_t kFnv1a64Offset = 14695981039346656037ULL;
constexpr uint64_t kFnv1a64Prime = 1099511628211ULL;
constexpr size_t kMaxDiagnosticSourceBytes = 4 * 1024 * 1024;
constexpr size_t kMaxDiagnosticLogBytes = 16 * 1024;
constexpr GLint kMaxDiagnosticAttachedShaders = 64;

struct ShaderFingerprint {
    GLenum type = 0;
    GLint compiled = 0;
    uint64_t hash = kFnv1a64Offset;
    size_t bytes = 0;
    bool complete = true;
};

struct ShaderTimingState {
    EGLContext context = EGL_NO_CONTEXT;
    uint64_t context_generation = 0;
    bool environment_logged = false;
    bool first_use_logged = false;
    uint32_t compile_calls = 0;
    uint32_t link_calls = 0;
    uint64_t compile_nanoseconds = 0;
    uint64_t link_nanoseconds = 0;
    std::chrono::steady_clock::time_point batch_started{};
};

ShaderTimingState& CurrentShaderTimingState() {
    thread_local ShaderTimingState state;
    const EGLContext context = eglGetCurrentContext();
    const uint64_t generation = bridge_gl::ShaderContextGeneration();
    if (state.context != context || state.context_generation != generation) {
        state = ShaderTimingState{};
        state.context = context;
        state.context_generation = generation;
    }
    return state;
}

bool ShaderTraceEnabled() {
    // GL status queries can synchronize an otherwise asynchronous driver. Keep
    // all extra calls out of production and sample the opt-in property once per
    // process; a diagnostic run must set it before launching the activity.
    static const bool enabled = [] {
        char value[PROP_VALUE_MAX] = {};
        if (__system_property_get("debug.warband.shader_trace", value) <= 0) return false;
        return strcmp(value, "1") == 0 || strcmp(value, "true") == 0 ||
               strcmp(value, "yes") == 0 || strcmp(value, "on") == 0;
    }();
    return enabled;
}

// Aggregate texture-upload counters (opt-in trace via debug.warband.tex_trace).
// They answer the only question that matters for startup tuning: does the guest
// engine upload compressed ETC blocks, or did some path expand to plain RGBA?
struct TextureUploadCounters {
    std::atomic<uint64_t> compressed_calls{0};
    std::atomic<uint64_t> compressed_bytes{0};
    std::atomic<uint64_t> compressed_nanoseconds{0};
    std::atomic<uint64_t> astc_to_etc1_calls{0};
    std::atomic<uint64_t> astc_to_etc1_bytes{0};
    std::atomic<uint64_t> astc_to_etc1_errors{0};
    std::atomic<uint64_t> mod_to_astc_calls{0};
    std::atomic<uint64_t> mod_to_astc_bytes{0};
    std::atomic<uint64_t> mod_to_astc_errors{0};
    std::atomic<uint64_t> plain_calls{0};
    std::atomic<uint64_t> plain_bytes{0};
    std::atomic<uint64_t> plain_nanoseconds{0};
};
TextureUploadCounters g_texture_uploads;

void RecordTextureUploadLog() {
    auto& c = g_texture_uploads;
    LOGI("texture uploads: compressed=%llu calls %llu bytes %llu ms; "
         "astc_to_etc1=%llu calls %llu bytes errors=%llu; "
         "mod_to_astc=%llu calls %llu bytes errors=%llu; "
         "plain=%llu calls %llu bytes %llu ms",
         static_cast<unsigned long long>(c.compressed_calls.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(c.compressed_bytes.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(c.compressed_nanoseconds.load(std::memory_order_relaxed) / 1000000),
         static_cast<unsigned long long>(c.astc_to_etc1_calls.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(c.astc_to_etc1_bytes.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(c.astc_to_etc1_errors.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(c.mod_to_astc_calls.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(c.mod_to_astc_bytes.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(c.mod_to_astc_errors.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(c.plain_calls.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(c.plain_bytes.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(c.plain_nanoseconds.load(std::memory_order_relaxed) / 1000000));
}

bool TextureTraceEnabled() {
    static const bool enabled = [] {
        char value[PROP_VALUE_MAX] = {};
        if (__system_property_get("debug.warband.tex_trace", value) <= 0) return false;
        return strcmp(value, "1") == 0 || strcmp(value, "true") == 0 ||
               strcmp(value, "yes") == 0 || strcmp(value, "on") == 0;
    }();
    return enabled;
}

// Opt-in accounting for GPU object lifetime investigations.  This deliberately
// observes the calls the guest already makes and never creates, deletes, binds,
// or queries an extra GL object.  In particular it is safe to use while
// diagnosing the multiplayer map selector without masking a guest ownership
// bug.  Set debug.warband.gl_resource_trace before process start.
bool GlResourceTraceEnabled() {
#ifdef WARBAND_BRIDGE_NO_LOGS
    return false;
#else
    static const bool enabled = [] {
        char value[PROP_VALUE_MAX] = {};
        if (__system_property_get("debug.warband.gl_resource_trace", value) <= 0) {
            return false;
        }
        return strcmp(value, "1") == 0 || strcmp(value, "true") == 0 ||
               strcmp(value, "yes") == 0 || strcmp(value, "on") == 0;
    }();
    return enabled;
#endif
}

// Exact return addresses in the unmodified 1.071 guest.  Matching the caller,
// rather than applying a global GL policy, keeps the repairs limited to the
// ownership bugs in the original renderer:
//
// * CD3DFont::RestoreDeviceObjects overwrites its existing VBO name;
// * rglTexture::restore_Imp overwrites the depth RBO at this+224;
// * rglOpenGL_effect's constructor loses the heap strings after glShaderSource,
//   even though OpenGL has already copied their contents;
// * the same constructor links a unique vertex/fragment shader pair into each
//   program but never marks either shader object for deletion.
constexpr uint32_t kFontVboGenReturn = 0x000CC790u;
constexpr uint32_t kDepthRboGenReturn = 0x002129E8u;
constexpr uint32_t kVertexShaderSourceReturn = 0x001E2E30u;
constexpr uint32_t kFragmentShaderSourceReturn = 0x001E2E70u;
constexpr uint32_t kEffectLinkReturn = 0x001E2EA8u;

uint32_t GuestImageOffset(uint32_t address) {
    // The game image is loaded at an implementation-selected 32-bit base.  All
    // relevant offsets are below 16 MiB, matching the convention already used
    // by the PostFX diagnostics in this translation unit.
    return address & 0x00FFFFFFu;
}

bool MemoryLeakTraceEnabled() {
#ifdef WARBAND_BRIDGE_NO_LOGS
    return false;
#else
    static const bool enabled = [] {
        char value[PROP_VALUE_MAX] = {};
        if (__system_property_get("debug.warband.memory_leak_trace", value) <= 0) {
            return false;
        }
        return strcmp(value, "1") == 0 || strcmp(value, "true") == 0 ||
               strcmp(value, "yes") == 0 || strcmp(value, "on") == 0;
    }();
    return enabled;
#endif
}

bool MemoryLeakFixesEnabled() {
#ifdef WARBAND_BRIDGE_NO_LOGS
    // Release builds never expose the diagnostic kill switch.
    return true;
#else
    static const bool disabled = [] {
        char value[PROP_VALUE_MAX] = {};
        if (__system_property_get("debug.warband.disable_leak_fixes", value) <= 0) {
            return false;
        }
        return strcmp(value, "1") == 0 || strcmp(value, "true") == 0 ||
               strcmp(value, "yes") == 0 || strcmp(value, "on") == 0;
    }();
    return !disabled;
#endif
}

#ifndef WARBAND_BRIDGE_NO_LOGS
struct MemoryLeakTraceState {
    uint64_t font_restore_calls = 0;
    uint64_t font_live_replacements = 0;
    uint64_t font_reclaimed = 0;
    uint64_t depth_restore_calls = 0;
    uint64_t depth_live_replacements = 0;
    uint64_t depth_reclaimed = 0;
    uint64_t shader_source_calls = 0;
    uint64_t shader_source_reclaimed = 0;
    uint64_t effect_link_calls = 0;
    uint64_t shader_objects_seen = 0;
    uint64_t shader_objects_delete_marked = 0;
    std::unordered_map<uint32_t, uint32_t> outstanding_shader_sources;
};

MemoryLeakTraceState& CurrentMemoryLeakTraceState() {
    static thread_local MemoryLeakTraceState state;
    return state;
}

uint64_t OutstandingShaderSourceBytes(const MemoryLeakTraceState& state) {
    uint64_t bytes = 0;
    for (const auto& [unused, size] : state.outstanding_shader_sources) {
        (void)unused;
        bytes += size;
    }
    return bytes;
}
#endif

struct GlResourceTraceState {
    EGLContext context = EGL_NO_CONTEXT;
    GLenum active_texture = GL_TEXTURE0;
    std::unordered_map<GLenum, GLuint> bound_texture_2d;
    GLuint bound_array_buffer = 0;
    GLuint bound_element_array_buffer = 0;
    GLuint bound_renderbuffer = 0;
    std::unordered_set<GLuint> buffers;
    std::unordered_set<GLuint> textures;
    std::unordered_set<GLuint> renderbuffers;
    std::unordered_set<GLuint> framebuffers;
    std::unordered_set<GLuint> shaders;
    std::unordered_set<GLuint> programs;
    std::unordered_map<GLuint, uint64_t> buffer_bytes;
    std::unordered_map<GLuint, uint64_t> texture_bytes;
    std::unordered_map<uint64_t, uint64_t> texture_level_bytes;
    std::unordered_map<GLuint, uint64_t> renderbuffer_bytes;
    uint64_t generated_buffers = 0;
    uint64_t deleted_buffers = 0;
    uint64_t generated_textures = 0;
    uint64_t deleted_textures = 0;
    uint64_t generated_renderbuffers = 0;
    uint64_t deleted_renderbuffers = 0;
    uint64_t generated_framebuffers = 0;
    uint64_t deleted_framebuffers = 0;
    uint64_t created_shaders = 0;
    uint64_t deleted_shaders = 0;
    uint64_t created_programs = 0;
    uint64_t deleted_programs = 0;
    uint64_t storage_calls = 0;
    uint64_t sequence = 0;
};

GlResourceTraceState& CurrentGlResourceTraceState() {
    thread_local GlResourceTraceState state;
    const EGLContext context = eglGetCurrentContext();
    if (state.context != context) {
        state = GlResourceTraceState{};
        state.context = context;
    }
    return state;
}

uint64_t SumObjectBytes(const std::unordered_map<GLuint, uint64_t>& values) {
    uint64_t total = 0;
    for (const auto& [unused, bytes] : values) {
        (void)unused;
        total += bytes;
    }
    return total;
}

GLuint CurrentTrackedTexture2D(GlResourceTraceState& state) {
    const auto it = state.bound_texture_2d.find(state.active_texture);
    return it == state.bound_texture_2d.end() ? 0 : it->second;
}

void LogGlResourceSummary(const char* reason, uint32_t caller_lr);

void MaybeLogGlLifetime(const char* reason, uint32_t caller_lr) {
    if (!GlResourceTraceEnabled()) return;
    auto& s = CurrentGlResourceTraceState();
    const uint64_t changes = s.generated_buffers + s.deleted_buffers +
        s.generated_textures + s.deleted_textures + s.generated_renderbuffers +
        s.deleted_renderbuffers + s.generated_framebuffers + s.deleted_framebuffers +
        s.created_shaders + s.deleted_shaders + s.created_programs + s.deleted_programs;
    if ((changes % 128u) == 0u) LogGlResourceSummary(reason, caller_lr);
}

void LogGlResourceSummary(const char* reason, uint32_t caller_lr) {
    if (!GlResourceTraceEnabled()) return;
    auto& s = CurrentGlResourceTraceState();
    LOGI("gl_resource seq=%llu reason=%s lr=0x%08x context=%p "
         "buffers=%zu/%lluB gen=%llu del=%llu textures=%zu/%lluB gen=%llu del=%llu "
         "rbos=%zu/%lluB gen=%llu del=%llu fbos=%zu gen=%llu del=%llu "
         "shaders=%zu gen=%llu del=%llu programs=%zu gen=%llu del=%llu storage=%llu",
         static_cast<unsigned long long>(++s.sequence), reason, caller_lr, s.context,
         s.buffers.size(), static_cast<unsigned long long>(SumObjectBytes(s.buffer_bytes)),
         static_cast<unsigned long long>(s.generated_buffers),
         static_cast<unsigned long long>(s.deleted_buffers), s.textures.size(),
         static_cast<unsigned long long>(SumObjectBytes(s.texture_bytes)),
         static_cast<unsigned long long>(s.generated_textures),
         static_cast<unsigned long long>(s.deleted_textures), s.renderbuffers.size(),
         static_cast<unsigned long long>(SumObjectBytes(s.renderbuffer_bytes)),
         static_cast<unsigned long long>(s.generated_renderbuffers),
         static_cast<unsigned long long>(s.deleted_renderbuffers), s.framebuffers.size(),
         static_cast<unsigned long long>(s.generated_framebuffers),
         static_cast<unsigned long long>(s.deleted_framebuffers), s.shaders.size(),
         static_cast<unsigned long long>(s.created_shaders),
         static_cast<unsigned long long>(s.deleted_shaders), s.programs.size(),
         static_cast<unsigned long long>(s.created_programs),
         static_cast<unsigned long long>(s.deleted_programs),
         static_cast<unsigned long long>(s.storage_calls));
}

void MaybeLogGlStorage(const char* reason, uint32_t caller_lr, uint64_t bytes,
                       GLuint object, GLsizei width = 0, GLsizei height = 0,
                       GLint level = 0) {
    if (!GlResourceTraceEnabled()) return;
    auto& s = CurrentGlResourceTraceState();
    ++s.storage_calls;
    if (bytes >= 256u * 1024u || (s.storage_calls % 64u) == 0u) {
        LOGI("gl_resource_storage kind=%s object=%u level=%d size=%dx%d bytes=%llu "
             "lr=0x%08x storage=%llu",
             reason, object, level, width, height,
             static_cast<unsigned long long>(bytes), caller_lr,
             static_cast<unsigned long long>(s.storage_calls));
        LogGlResourceSummary(reason, caller_lr);
    }
}

uint64_t TextureLevelKey(GLuint texture, GLint level) {
    return (static_cast<uint64_t>(texture) << 32u) |
           static_cast<uint32_t>(level);
}

uint64_t ApproximateTextureBytes(GLsizei width, GLsizei height, GLenum format,
                                 GLenum type) {
    if (width <= 0 || height <= 0) return 0;
    uint64_t components = 4;
    if (format == GL_ALPHA || format == GL_LUMINANCE) components = 1;
    if (format == GL_LUMINANCE_ALPHA) components = 2;
    if (format == GL_RGB) components = 3;
    uint64_t bytes_per_component = 1;
    if (type == GL_FLOAT) bytes_per_component = 4;
    if (type == GL_UNSIGNED_SHORT || type == GL_SHORT) bytes_per_component = 2;
    if (type == GL_UNSIGNED_SHORT_5_6_5 || type == GL_UNSIGNED_SHORT_4_4_4_4 ||
        type == GL_UNSIGNED_SHORT_5_5_5_1) {
        components = 1;
        bytes_per_component = 2;
    }
    return static_cast<uint64_t>(width) * static_cast<uint64_t>(height) *
           components * bytes_per_component;
}

void TrackTextureStorage(GLuint texture, GLint level, uint64_t bytes,
                         uint32_t caller_lr, const char* kind, GLsizei width,
                         GLsizei height) {
    if (!GlResourceTraceEnabled() || texture == 0) return;
    auto& s = CurrentGlResourceTraceState();
    s.textures.insert(texture);
    const uint64_t key = TextureLevelKey(texture, level);
    const uint64_t old_bytes = s.texture_level_bytes[key];
    s.texture_level_bytes[key] = bytes;
    uint64_t& object_bytes = s.texture_bytes[texture];
    object_bytes = object_bytes >= old_bytes ? object_bytes - old_bytes + bytes : bytes;
    MaybeLogGlStorage(kind, caller_lr, bytes, texture, width, height, level);
}

void ForgetTextureStorage(GLuint texture) {
    auto& s = CurrentGlResourceTraceState();
    s.texture_bytes.erase(texture);
    const uint64_t prefix = static_cast<uint64_t>(texture) << 32u;
    for (auto it = s.texture_level_bytes.begin(); it != s.texture_level_bytes.end();) {
        if ((it->first & 0xffffffff00000000ULL) == prefix) {
            it = s.texture_level_bytes.erase(it);
        } else {
            ++it;
        }
    }
}

bool ForceEtc1TranscodeForDiagnostics() {
    // This is deliberately opt-in and process-scoped. It lets a capable
    // device produce an apples-to-apples ASTC/ETC1 screenshot comparison
    // without changing the release decision: production still follows the
    // actual context capabilities.
    static const bool enabled = [] {
        char value[PROP_VALUE_MAX] = {};
        if (__system_property_get("debug.warband.force_etc1", value) <= 0) {
            return false;
        }
        return strcmp(value, "1") == 0 || strcmp(value, "true") == 0 ||
               strcmp(value, "yes") == 0 || strcmp(value, "on") == 0;
    }();
    return enabled;
}

bool UseGles2CompatibilityCapabilities() {
    // Some EGL implementations legally return their newest backwards-
    // compatible context even when EGL_CONTEXT_CLIENT_VERSION requests 2.
    // Keep guest-visible capabilities in sync with the requested GLES2 mode.
    static const bool enabled = [] {
        char value[PROP_VALUE_MAX] = {};
        if (__system_property_get("debug.warband.force_gles2", value) > 0) {
            return strcmp(value, "1") == 0 || strcmp(value, "true") == 0 ||
                   strcmp(value, "yes") == 0 || strcmp(value, "on") == 0;
        }
        return __system_property_get("ro.board.platform", value) > 0 &&
               strcmp(value, "mt6785") == 0;
    }();
    return enabled;
}

bool ForceRgbaFallbackForDiagnostics() {
    // This isolates the encoder/ETC1 upload from the ASTC decoder and the
    // texture-state path on a capable device. It is intentionally diagnostic
    // only; normal builds still select the path from actual GL capabilities.
    static const bool enabled = [] {
        char value[PROP_VALUE_MAX] = {};
        if (__system_property_get("debug.warband.force_rgba", value) <= 0) {
            return false;
        }
        return strcmp(value, "1") == 0 || strcmp(value, "true") == 0 ||
               strcmp(value, "yes") == 0 || strcmp(value, "on") == 0;
    }();
    return enabled;
}

bool ForceModRgbaFallbackForDiagnostics() {
    static const bool forced = [] {
        char value[PROP_VALUE_MAX] = {};
        if (__system_property_get("debug.warband.force_mod_rgba", value) <= 0) {
            return false;
        }
        return strcmp(value, "1") == 0 || strcasecmp(value, "true") == 0;
    }();
    return forced;
}

bool ForceRgbaNormalFallbackForDiagnostics() {
    static const bool enabled = [] {
        char value[PROP_VALUE_MAX] = {};
        if (__system_property_get("debug.warband.force_rgba_normal", value) <= 0) {
            return false;
        }
        return strcmp(value, "1") == 0 || strcmp(value, "true") == 0 ||
               strcmp(value, "yes") == 0 || strcmp(value, "on") == 0;
    }();
    return enabled;
}

int ForceRgbaSizeForDiagnostics() {
    static const int size = [] {
        char value[PROP_VALUE_MAX] = {};
        if (__system_property_get("debug.warband.force_rgba_size", value) <= 0) {
            return 0;
        }
        char* end = nullptr;
        const long parsed = std::strtol(value, &end, 10);
        if (end == value || *end != '\0' || parsed <= 0 || parsed > 16384) {
            return 0;
        }
        return static_cast<int>(parsed);
    }();
    return size;
}

bool PostFxTraceEnabled() {
    // Pixel readback is deliberately diagnostic-only: it synchronizes the GPU
    // and consumes GL error state.  Sample the property once before the first
    // render just like the shader/texture traces.
    static const bool enabled = [] {
        char value[PROP_VALUE_MAX] = {};
        if (__system_property_get("debug.warband.postfx_trace", value) <= 0) return false;
        return strcmp(value, "1") == 0 || strcmp(value, "true") == 0 ||
               strcmp(value, "yes") == 0 || strcmp(value, "on") == 0;
    }();
    return enabled;
}

const char* SafeGlString(GLenum name) {
    const GLubyte* value = glGetString(name);
    return value ? reinterpret_cast<const char*>(value) : "<unavailable>";
}

const char* ShaderTypeName(GLenum type) {
    switch (type) {
        case GL_VERTEX_SHADER: return "vertex";
        case GL_FRAGMENT_SHADER: return "fragment";
        default: return "unknown";
    }
}

uint64_t Fnv1a64(const char* data, size_t size) {
    uint64_t hash = kFnv1a64Offset;
    for (size_t i = 0; i < size; ++i) {
        hash ^= static_cast<uint8_t>(data[i]);
        hash *= kFnv1a64Prime;
    }
    return hash;
}

uint64_t TexturePayloadFingerprint(const void* data, GLsizei image_size) {
    if (!data || image_size <= 0) return 0;
    const auto* bytes = static_cast<const uint8_t*>(data);
    const size_t size = static_cast<size_t>(image_size);
    const size_t head_size = std::min<size_t>(size, 32);
    uint64_t hash = Fnv1a64(reinterpret_cast<const char*>(bytes), head_size);
    const size_t tail_size = std::min<size_t>(size - head_size, 32);
    if (tail_size != 0) {
        hash = Fnv1a64(reinterpret_cast<const char*>(bytes + size - tail_size), tail_size) ^
               (hash * kFnv1a64Prime);
    }
    hash ^= static_cast<uint64_t>(image_size) * kFnv1a64Prime;
    return hash;
}

bool LooksLikeNormalMap(const std::vector<uint32_t>& rgba) {
    if (rgba.size() < 16) return false;
    const auto* bytes = reinterpret_cast<const uint8_t*>(rgba.data());
    const size_t stride = std::max<size_t>(1, rgba.size() / 2048);
    uint64_t red = 0;
    uint64_t green = 0;
    uint64_t blue = 0;
    size_t samples = 0;
    for (size_t pixel = 0; pixel < rgba.size(); pixel += stride) {
        red += bytes[pixel * 4 + 0];
        green += bytes[pixel * 4 + 1];
        blue += bytes[pixel * 4 + 2];
        ++samples;
    }
    if (samples == 0) return false;
    const uint64_t average_red = red / samples;
    const uint64_t average_green = green / samples;
    const uint64_t average_blue = blue / samples;
    return average_blue >= 190 && average_blue > average_red + 24 &&
           average_blue > average_green + 24;
}

ShaderFingerprint InspectShader(GLuint shader) {
    ShaderFingerprint fingerprint;
    GLint shader_type = 0;
    glGetShaderiv(shader, GL_SHADER_TYPE, &shader_type);
    fingerprint.type = static_cast<GLenum>(shader_type);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &fingerprint.compiled);

    GLint source_length = 0;
    glGetShaderiv(shader, GL_SHADER_SOURCE_LENGTH, &source_length);
    if (source_length <= 1) return fingerprint;

    const size_t reported_bytes = static_cast<size_t>(source_length - 1);
    const size_t requested_bytes = std::min(reported_bytes, kMaxDiagnosticSourceBytes);
    std::vector<GLchar> source(requested_bytes + 1, '\0');
    GLsizei written = 0;
    glGetShaderSource(shader, static_cast<GLsizei>(source.size()), &written, source.data());
    fingerprint.bytes = written > 0 ? static_cast<size_t>(written) : 0;
    fingerprint.hash = Fnv1a64(source.data(), fingerprint.bytes);
    fingerprint.complete = reported_bytes <= kMaxDiagnosticSourceBytes &&
                           fingerprint.bytes == reported_bytes;
    return fingerprint;
}

std::string ShaderInfoLog(GLuint shader) {
    GLint reported_length = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &reported_length);
    if (reported_length <= 1) return "<empty>";
    const size_t requested = std::min(static_cast<size_t>(reported_length),
                                      kMaxDiagnosticLogBytes);
    std::vector<GLchar> log(requested, '\0');
    GLsizei written = 0;
    glGetShaderInfoLog(shader, static_cast<GLsizei>(log.size()), &written, log.data());
    return written > 0 ? std::string(log.data(), static_cast<size_t>(written)) : "<empty>";
}

std::string ProgramInfoLog(GLuint program) {
    GLint reported_length = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &reported_length);
    if (reported_length <= 1) return "<empty>";
    const size_t requested = std::min(static_cast<size_t>(reported_length),
                                      kMaxDiagnosticLogBytes);
    std::vector<GLchar> log(requested, '\0');
    GLsizei written = 0;
    glGetProgramInfoLog(program, static_cast<GLsizei>(log.size()), &written, log.data());
    return written > 0 ? std::string(log.data(), static_cast<size_t>(written)) : "<empty>";
}

void LogShaderEnvironment(ShaderTimingState& state) {
    if (state.environment_logged) return;
    state.environment_logged = true;

    GLint vertex_range[2] = {0, 0};
    GLint vertex_precision = 0;
    GLint fragment_range[2] = {0, 0};
    GLint fragment_precision = 0;
    glGetShaderPrecisionFormat(GL_VERTEX_SHADER, GL_HIGH_FLOAT,
                               vertex_range, &vertex_precision);
    glGetShaderPrecisionFormat(GL_FRAGMENT_SHADER, GL_HIGH_FLOAT,
                               fragment_range, &fragment_precision);
    LOGI("shader_environment context=%p vendor='%s' renderer='%s' gl='%s' glsl='%s' "
         "vertex_highp=[%d,%d]/%d fragment_highp=[%d,%d]/%d",
         state.context, SafeGlString(GL_VENDOR), SafeGlString(GL_RENDERER),
         SafeGlString(GL_VERSION), SafeGlString(GL_SHADING_LANGUAGE_VERSION),
         vertex_range[0], vertex_range[1], vertex_precision,
         fragment_range[0], fragment_range[1], fragment_precision);
}

void LogShaderCompileFailure(GLuint shader, ShaderTimingState& state) {
    LogShaderEnvironment(state);
    const ShaderFingerprint fingerprint = InspectShader(shader);
    const std::string log = ShaderInfoLog(shader);
    LOGE("shader_compile_failure shader=%u type=%s source_bytes=%zu "
         "source_fnv1a64=%016llx source_complete=%s driver_log='%s'",
         shader, ShaderTypeName(fingerprint.type), fingerprint.bytes,
         static_cast<unsigned long long>(fingerprint.hash),
         fingerprint.complete ? "true" : "false", log.c_str());
}

void LogProgramLinkFailure(GLuint program, ShaderTimingState& state) {
    LogShaderEnvironment(state);
    GLint attached_count = 0;
    glGetProgramiv(program, GL_ATTACHED_SHADERS, &attached_count);
    const GLint requested_count = std::clamp(attached_count, 0,
                                             kMaxDiagnosticAttachedShaders);
    std::vector<GLuint> attached(static_cast<size_t>(requested_count));
    GLsizei actual_count = 0;
    if (!attached.empty()) {
        glGetAttachedShaders(program, requested_count, &actual_count, attached.data());
    }
    const std::string log = ProgramInfoLog(program);
    LOGE("program_link_failure program=%u attached=%d inspected=%d driver_log='%s'",
         program, attached_count, actual_count, log.c_str());
    for (GLsizei i = 0; i < actual_count; ++i) {
        const ShaderFingerprint fingerprint = InspectShader(attached[static_cast<size_t>(i)]);
        LOGE("program_link_shader program=%u shader=%u type=%s compiled=%d "
             "source_bytes=%zu source_fnv1a64=%016llx source_complete=%s",
             program, attached[static_cast<size_t>(i)], ShaderTypeName(fingerprint.type),
             fingerprint.compiled, fingerprint.bytes,
             static_cast<unsigned long long>(fingerprint.hash),
             fingerprint.complete ? "true" : "false");
    }
}

struct TextureImageInfo {
    GLsizei width = 0;
    GLsizei height = 0;
    GLint source_internal_format = 0;
    GLenum source_format = 0;
    GLenum source_type = 0;
    bool source_had_pixels = false;
    bool legacy_single_channel = false;
    bool promoted = false;
    bool render_target = false;
    bool min_filter_override_reported = false;
    bool framebuffer_fallback_attempted = false;
    bool framebuffer_disabled_reported = false;
    GLenum allocated_internal_format = 0;
    bridge_gl::ShadowFramebufferStatusSnapshot framebuffer_status;
    std::vector<uint8_t> initial_rgba;
};

struct FramebufferInfo {
    GLuint color_texture = 0;
    GLenum texture_target = GL_TEXTURE_2D;
    GLint level = 0;
    bool disabled = false;
    bridge_gl::ShadowFramebufferStatusSnapshot untracked_logged_status;
};

struct PostFxProgramBindings {
    bool scanned = false;
    bool portable_units_initialized = false;
    bool portable_uniforms_reported = false;
    GLint half_pixel_viewport_location = -1;
    GLint hdr_frame_time_location = -1;
    GLint hdr_brightness_location = -1;
    GLint dof_focus_location = -1;
    GLint dof_range_location = -1;
    std::unordered_map<GLint, GLint> sampler_units_by_location;
};

struct MapWaterProgramBindings {
    bool scanned = false;
    bool portable_units_initialized = false;
    GLint diffuse_location = -1;
    GLint normal_location = -1;
    GLint environment_location = -1;

    bool is_reflective_water() const noexcept {
        return diffuse_location >= 0 && normal_location >= 0 &&
               environment_location >= 0;
    }
};

struct GlCompatibilityState {
    EGLContext context = EGL_NO_CONTEXT;
    uint64_t context_generation = 0;
    bridge_gl::GlesVersion version{};
    std::string extensions;
    std::unordered_set<GLenum> compressed_formats;
    std::unordered_map<GLuint, TextureImageInfo> textures;
    // ASTC fallback must use one internal format for every mip level of a
    // texture. Mixing ETC1 and RGBA8 makes the GLES texture incomplete.
    std::unordered_map<GLuint, bool> astc_rgba_fallback;
    // Likewise, a mod texture which starts as native ASTC must keep native
    // ASTC for every mip. The level-zero decision is cached per GL texture.
    std::unordered_map<GLuint, bool> mod_native_astc;
    std::unordered_map<GLuint, FramebufferInfo> framebuffers;
    std::unordered_map<GLuint, PostFxProgramBindings> postfx_programs;
    std::unordered_map<GLuint, MapWaterProgramBindings> map_water_programs;
    GLuint bound_framebuffer = 0;
    GLuint bound_program = 0;
    GLint viewport[4] = {};
    bool postfx_float_pass_seen = false;
};

constexpr GLenum kGlHalfFloat = 0x140Bu;
constexpr GLenum kGlHalfFloatOes = 0x8D61u;
constexpr GLenum kGlTextureBaseLevel = 0x813Cu;
constexpr GLenum kGlTextureMaxLevel = 0x813Du;

float HalfToFloat(uint16_t value) {
    const uint32_t sign = static_cast<uint32_t>(value & 0x8000u) << 16;
    int32_t exponent = (value >> 10) & 0x1Fu;
    uint32_t mantissa = value & 0x03FFu;
    uint32_t bits = 0;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            exponent = 1;
            while ((mantissa & 0x0400u) == 0) {
                mantissa <<= 1;
                --exponent;
            }
            mantissa &= 0x03FFu;
            bits = sign | (static_cast<uint32_t>(exponent + 112) << 23) |
                   (mantissa << 13);
        }
    } else if (exponent == 0x1F) {
        bits = sign | 0x7F800000u | (mantissa << 13);
    } else {
        bits = sign | (static_cast<uint32_t>(exponent + 112) << 23) |
               (mantissa << 13);
    }
    float result = 0.0f;
    memcpy(&result, &bits, sizeof(result));
    return result;
}

bool DecodeReadbackPixel(const void* storage, GLenum type, float values[4]) {
    std::fill(values, values + 4, 0.0f);
    if (type == GL_UNSIGNED_BYTE) {
        const auto* bytes = static_cast<const uint8_t*>(storage);
        for (int i = 0; i < 4; ++i) values[i] = bytes[i] / 255.0f;
        return true;
    }
    if (type == GL_FLOAT) {
        memcpy(values, storage, sizeof(float) * 4);
        return true;
    }
    if (type == kGlHalfFloat || type == kGlHalfFloatOes) {
        uint16_t halves[4] = {};
        memcpy(halves, storage, sizeof(halves));
        for (int i = 0; i < 4; ++i) values[i] = HalfToFloat(halves[i]);
        return true;
    }
    if (type == GL_UNSIGNED_SHORT) {
        uint16_t words[4] = {};
        memcpy(words, storage, sizeof(words));
        for (int i = 0; i < 4; ++i) values[i] = words[i] / 65535.0f;
        return true;
    }
    if (type == GL_UNSIGNED_SHORT_5_6_5) {
        uint16_t packed = 0;
        memcpy(&packed, storage, sizeof(packed));
        values[0] = static_cast<float>((packed >> 11) & 0x1Fu) / 31.0f;
        values[1] = static_cast<float>((packed >> 5) & 0x3Fu) / 63.0f;
        values[2] = static_cast<float>(packed & 0x1Fu) / 31.0f;
        values[3] = 1.0f;
        return true;
    }
    return false;
}

GlCompatibilityState& CurrentGlCompatibilityState() {
    thread_local GlCompatibilityState state;
    const EGLContext context = eglGetCurrentContext();
    const uint64_t generation = bridge_gl::ShaderContextGeneration();
    if (state.context == context && state.context_generation == generation) return state;

    state = GlCompatibilityState{};
    state.context = context;
    state.context_generation = generation;
    const char* version = SafeGlString(GL_VERSION);
    state.version = bridge_gl::ParseGlesVersion(version);
    const GLubyte* extensions = glGetString(GL_EXTENSIONS);
    if (extensions) state.extensions = reinterpret_cast<const char*>(extensions);

    GLint count = 0;
    glGetIntegerv(GL_NUM_COMPRESSED_TEXTURE_FORMATS, &count);
    if (count > 0 && count <= 4096) {
        std::vector<GLint> formats(static_cast<size_t>(count));
        glGetIntegerv(GL_COMPRESSED_TEXTURE_FORMATS, formats.data());
        for (GLint format : formats) state.compressed_formats.insert(static_cast<GLenum>(format));
    }
    const bridge_gl::GlesVersion driver_version = state.version;
    if (UseGles2CompatibilityCapabilities()) {
        state.version = {2, 0};
        state.extensions = "GL_OES_compressed_ETC1_RGB8_texture";
        state.compressed_formats.clear();
        state.compressed_formats.insert(bridge_gl::kGlEtc1Rgb8Oes);
    }
    GLint framebuffer = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
    if (framebuffer > 0) state.bound_framebuffer = static_cast<GLuint>(framebuffer);
    GLint program = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    if (program > 0) state.bound_program = static_cast<GLuint>(program);
    glGetIntegerv(GL_VIEWPORT, state.viewport);

    GLint max_texture_size = 0;
    GLint max_vertex_uniform_vectors = 0;
    GLint highp_range[2] = {};
    GLint highp_precision = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture_size);
    glGetIntegerv(GL_MAX_VERTEX_UNIFORM_VECTORS, &max_vertex_uniform_vectors);
    glGetShaderPrecisionFormat(GL_FRAGMENT_SHADER, GL_HIGH_FLOAT,
                               highp_range, &highp_precision);
    bool astc_listed = false;
    bool etc1_listed = false;
    for (const GLenum format : state.compressed_formats) {
        astc_listed = astc_listed || bridge_gl::IsAstcFormat(format);
        etc1_listed = etc1_listed || format == bridge_gl::kGlEtc1Rgb8Oes;
    }
    const bool astc_supported = astc_listed ||
        bridge_gl::VersionGuaranteesCompressedFormat(
            bridge_gl::kGlCompressedRgbaAstc4x4Khr, state.version) ||
        bridge_gl::ExtensionGuaranteesCompressedFormat(
            bridge_gl::kGlCompressedRgbaAstc4x4Khr, state.extensions);
    const bool etc1_supported = etc1_listed ||
        bridge_gl::ExtensionGuaranteesCompressedFormat(
            bridge_gl::kGlEtc1Rgb8Oes, state.extensions);
    LOGI("gles_caps gles=%d.%d driver_gles=%d.%d forced_gles2=%d "
         "compressed_formats=%zu astc_supported=%d "
         "etc1_supported=%d max_texture_size=%d max_vertex_uniform_vectors=%d "
         "fragment_highp_range=%d..%d fragment_highp_precision=%d vendor='%s' "
         "renderer='%s' version='%s'",
         state.version.major, state.version.minor,
         driver_version.major, driver_version.minor,
         UseGles2CompatibilityCapabilities() ? 1 : 0,
         state.compressed_formats.size(),
         astc_supported ? 1 : 0, etc1_supported ? 1 : 0, max_texture_size,
         max_vertex_uniform_vectors, highp_range[0], highp_range[1], highp_precision,
         SafeGlString(GL_VENDOR), SafeGlString(GL_RENDERER), SafeGlString(GL_VERSION));
    if (state.version.major < 2) {
        LOGE("gles_caps requirement_failed required=2.0 actual=%d.%d",
             state.version.major, state.version.minor);
    }
    if (max_texture_size < 4096) {
        LOGE("gles_caps requirement_failed max_texture_size=%d required=4096",
             max_texture_size);
    }
    if (max_vertex_uniform_vectors < 144) {
        LOGE("gles_caps requirement_failed max_vertex_uniform_vectors=%d required=144",
             max_vertex_uniform_vectors);
    }
    return state;
}

PostFxProgramBindings& InspectPostFxProgram(GlCompatibilityState& state,
                                            GLuint program) {
    auto [it, inserted] = state.postfx_programs.try_emplace(program);
    PostFxProgramBindings& bindings = it->second;
    if (bindings.scanned) return bindings;
    bindings.scanned = true;

    constexpr const char* kSamplerNames[5] = {
        "postFX_sampler0", "postFX_sampler1", "postFX_sampler2",
        "postFX_sampler3", "postFX_sampler4",
    };
    for (GLint unit = 0; unit < 5; ++unit) {
        const GLint location = glGetUniformLocation(program, kSamplerNames[unit]);
        if (location >= 0) {
            bindings.sampler_units_by_location.emplace(location, unit);
        }
    }
    bindings.half_pixel_viewport_location = glGetUniformLocation(
        program, "g_HalfPixel_ViewportSizeInv");
    bindings.hdr_frame_time_location = glGetUniformLocation(
        program, "g_HDR_frameTime");
    bindings.hdr_brightness_location = glGetUniformLocation(
        program, "mb_HDRBrightness");
    bindings.dof_focus_location = glGetUniformLocation(program, "g_DOF_Focus");
    bindings.dof_range_location = glGetUniformLocation(program, "g_DOF_Range");
    if (PostFxTraceEnabled() && !bindings.sampler_units_by_location.empty()) {
        LOGI("postfx_sampler_layout program=%u active_samplers=%zu "
             "half_pixel_location=%d hdr_frame_time_location=%d "
             "hdr_brightness_location=%d "
             "dof_focus_location=%d dof_range_location=%d",
             program, bindings.sampler_units_by_location.size(),
             bindings.half_pixel_viewport_location,
             bindings.hdr_frame_time_location,
             bindings.hdr_brightness_location,
             bindings.dof_focus_location, bindings.dof_range_location);
    }
    return bindings;
}

MapWaterProgramBindings& InspectMapWaterProgram(GlCompatibilityState& state,
                                                GLuint program) {
    auto [it, inserted] = state.map_water_programs.try_emplace(program);
    MapWaterProgramBindings& bindings = it->second;
    if (bindings.scanned) return bindings;
    bindings.scanned = true;
    bindings.diffuse_location = glGetUniformLocation(program, "diffuse_texture");
    bindings.normal_location = glGetUniformLocation(program, "normal_texture");
    bindings.environment_location = glGetUniformLocation(program, "env_texture");
    return bindings;
}

void InitializePortableMapWaterSamplerUnits(GlCompatibilityState& state,
                                             GLuint program) {
    if (program == 0) return;
    MapWaterProgramBindings& bindings = InspectMapWaterProgram(state, program);
    if (!bindings.is_reflective_water() || bindings.portable_units_initialized) {
        return;
    }

    // The legacy effect cache uploads diffuse and normal stages but leaves the
    // projected reflection sampler at GLSL's default unit zero.  Unit zero is
    // ordinary material/UI state by the time the campaign-water pass runs.
    // The renderer binds its screen-sized reflection target to stage five.
    glUniform1i(bindings.diffuse_location,
                bridge_gl::PortableMapWaterSamplerUnit(
                    bridge_gl::MapWaterSamplerRole::Diffuse, 0));
    glUniform1i(bindings.normal_location,
                bridge_gl::PortableMapWaterSamplerUnit(
                    bridge_gl::MapWaterSamplerRole::Normal, 0));
    glUniform1i(bindings.environment_location,
                bridge_gl::PortableMapWaterSamplerUnit(
                    bridge_gl::MapWaterSamplerRole::Environment, 0));
    bindings.portable_units_initialized = true;
    if (PostFxTraceEnabled()) {
        LOGI("map_water_sampler_init program=%u diffuse=9 normal=3 env=5",
             program);
    }
}

GLint PortableMapWaterSamplerUnit(GlCompatibilityState& state, GLint location,
                                  GLint requested_unit) {
    if (state.bound_program == 0) return requested_unit;
    MapWaterProgramBindings& bindings =
        InspectMapWaterProgram(state, state.bound_program);
    if (!bindings.is_reflective_water()) return requested_unit;
    bridge_gl::MapWaterSamplerRole role = bridge_gl::MapWaterSamplerRole::Other;
    if (location == bindings.diffuse_location) {
        role = bridge_gl::MapWaterSamplerRole::Diffuse;
    } else if (location == bindings.normal_location) {
        role = bridge_gl::MapWaterSamplerRole::Normal;
    } else if (location == bindings.environment_location) {
        role = bridge_gl::MapWaterSamplerRole::Environment;
    }
    return bridge_gl::PortableMapWaterSamplerUnit(role, requested_unit);
}

void InitializePortablePostFxSamplerUnits(GlCompatibilityState& state,
                                          GLuint program) {
    if (program == 0) return;
    PostFxProgramBindings& bindings = InspectPostFxProgram(state, program);
    if (bindings.portable_units_initialized ||
        bindings.sampler_units_by_location.empty()) {
        return;
    }

    // GLSL initializes every sampler uniform to texture unit zero.  The old
    // renderer only uploads the sampler whose internal state it considers
    // dirty, relying on Tegra's legacy assignment behavior for the rest.
    // GLES requires the application to assign every active sampler explicitly.
    for (const auto& [location, unit] : bindings.sampler_units_by_location) {
        glUniform1i(location, unit);
    }
    bindings.portable_units_initialized = true;
    if (PostFxTraceEnabled()) {
        LOGI("postfx_sampler_init program=%u assigned_samplers=%zu",
             program, bindings.sampler_units_by_location.size());
    }
}

GLint PortablePostFxSamplerUnit(GLint location, GLint requested_unit) {
    auto& state = CurrentGlCompatibilityState();
    if (state.bound_program == 0) return requested_unit;
    PostFxProgramBindings& bindings =
        InspectPostFxProgram(state, state.bound_program);
    const auto it = bindings.sampler_units_by_location.find(location);
    if (it == bindings.sampler_units_by_location.end()) return requested_unit;

    const GLint portable_unit = it->second;
    if (PostFxTraceEnabled() && requested_unit != portable_unit) {
        static thread_local uint32_t report_count = 0;
        if (report_count++ < 64) {
            LOGI("postfx_sampler_fix program=%u location=%d requested_unit=%d "
                 "portable_unit=%d",
                 state.bound_program, location, requested_unit, portable_unit);
        }
    }
    return portable_unit;
}

bool BoundProgramUsesPostFxSamplers(GlCompatibilityState& state) {
    if (state.bound_program == 0) return false;
    const auto it = state.postfx_programs.find(state.bound_program);
    return it != state.postfx_programs.end() && it->second.scanned &&
           !it->second.sampler_units_by_location.empty();
}

class FullscreenPostFxStateGuard {
public:
    FullscreenPostFxStateGuard() {
        depth_test_ = glIsEnabled(GL_DEPTH_TEST);
        stencil_test_ = glIsEnabled(GL_STENCIL_TEST);
        scissor_test_ = glIsEnabled(GL_SCISSOR_TEST);
        cull_face_ = glIsEnabled(GL_CULL_FACE);
        blend_ = glIsEnabled(GL_BLEND);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_write_);
        glGetBooleanv(GL_COLOR_WRITEMASK, color_write_);

        if (depth_test_) glDisable(GL_DEPTH_TEST);
        if (stencil_test_) glDisable(GL_STENCIL_TEST);
        if (scissor_test_) glDisable(GL_SCISSOR_TEST);
        if (cull_face_) glDisable(GL_CULL_FACE);
        if (blend_) glDisable(GL_BLEND);
        glDepthMask(GL_FALSE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    }

    ~FullscreenPostFxStateGuard() {
        glDepthMask(depth_write_);
        glColorMask(color_write_[0], color_write_[1],
                    color_write_[2], color_write_[3]);
        if (depth_test_) glEnable(GL_DEPTH_TEST);
        if (stencil_test_) glEnable(GL_STENCIL_TEST);
        if (scissor_test_) glEnable(GL_SCISSOR_TEST);
        if (cull_face_) glEnable(GL_CULL_FACE);
        if (blend_) glEnable(GL_BLEND);
    }

    FullscreenPostFxStateGuard(const FullscreenPostFxStateGuard&) = delete;
    FullscreenPostFxStateGuard& operator=(
        const FullscreenPostFxStateGuard&) = delete;

private:
    GLboolean depth_test_ = GL_FALSE;
    GLboolean stencil_test_ = GL_FALSE;
    GLboolean scissor_test_ = GL_FALSE;
    GLboolean cull_face_ = GL_FALSE;
    GLboolean blend_ = GL_FALSE;
    GLboolean depth_write_ = GL_TRUE;
    GLboolean color_write_[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
};

bool PostFxSourceTextureSize(GlCompatibilityState& state,
                             const PostFxProgramBindings& bindings,
                             GLsizei& width, GLsizei& height) {
    if (state.bound_program == 0) return false;
    const auto sampler0 = std::find_if(
        bindings.sampler_units_by_location.begin(),
        bindings.sampler_units_by_location.end(),
        [](const auto& sampler) { return sampler.second == 0; });
    if (sampler0 == bindings.sampler_units_by_location.end()) return false;

    GLint texture_unit = 0;
    glGetUniformiv(state.bound_program, sampler0->first, &texture_unit);
    if (texture_unit < 0 || texture_unit > 31) return false;

    GLint previous_active_texture = GL_TEXTURE0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previous_active_texture);
    glActiveTexture(static_cast<GLenum>(GL_TEXTURE0 + texture_unit));
    GLint texture = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    glActiveTexture(static_cast<GLenum>(previous_active_texture));
    if (texture <= 0) return false;

    const auto texture_it = state.textures.find(static_cast<GLuint>(texture));
    if (texture_it == state.textures.end() || texture_it->second.width <= 0 ||
        texture_it->second.height <= 0) {
        return false;
    }
    width = texture_it->second.width;
    height = texture_it->second.height;
    return true;
}

void ApplyPortablePostFxUniformDefaults(GlCompatibilityState& state) {
    if (state.bound_program == 0) return;
    const auto program_it = state.postfx_programs.find(state.bound_program);
    if (program_it == state.postfx_programs.end()) return;
    PostFxProgramBindings& bindings = program_it->second;
    if (!bindings.scanned || bindings.sampler_units_by_location.empty()) return;

    GLsizei source_width = state.viewport[2];
    GLsizei source_height = state.viewport[3];
    PostFxSourceTextureSize(state, bindings, source_width, source_height);
    const bridge_gl::PostFxUniformDefaults values =
        bridge_gl::MakePostFxUniformDefaults(source_width, source_height);
    if (!values.valid_viewport) return;

    // GLES starts these uniforms at zero, while the legacy mobile backend does
    // not reliably publish the authored effect defaults or derived source
    // texture constants. Downsample and blur passes write into smaller targets
    // than the texture they sample, so their offsets must use sampler0's size
    // instead of the destination viewport. Apply them after CommitChanges and
    // immediately before the fullscreen draw.
    if (bindings.half_pixel_viewport_location >= 0) {
        glUniform4f(bindings.half_pixel_viewport_location,
                    values.half_pixel_x, values.half_pixel_y,
                    values.inverse_width, values.inverse_height);
    }
    if (bindings.hdr_frame_time_location >= 0) {
        glUniform1f(bindings.hdr_frame_time_location, values.hdr_frame_time);
    }
    if (bindings.hdr_brightness_location >= 0) {
        glUniform1f(bindings.hdr_brightness_location,
                    bridge_gl::HdrBrightness());
    }
    if (bindings.dof_focus_location >= 0) {
        glUniform1f(bindings.dof_focus_location, values.dof_focus);
    }
    if (bindings.dof_range_location >= 0) {
        glUniform1f(bindings.dof_range_location, values.dof_range);
    }
    if (PostFxTraceEnabled() && !bindings.portable_uniforms_reported) {
        bindings.portable_uniforms_reported = true;
        LOGI("postfx_uniform_defaults program=%u viewport=%dx%d source=%dx%d "
             "half_pixel=%.9g,%.9g,%.9g,%.9g hdr_frame_time=%.9g "
             "hdr_brightness=%.9g "
             "dof_focus=%.9g dof_range=%.9g",
             state.bound_program, state.viewport[2], state.viewport[3],
             source_width, source_height,
             values.half_pixel_x, values.half_pixel_y,
             values.inverse_width, values.inverse_height,
             values.hdr_frame_time, bridge_gl::HdrBrightness(),
             values.dof_focus, values.dof_range);
    }
}

void EnforcePortableRenderTargetMinFilter(GLuint texture,
                                          TextureImageInfo& info) {
    if (!info.render_target) return;
    GLint previous_binding = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_binding);
    if (static_cast<GLuint>(previous_binding) != texture) {
        glBindTexture(GL_TEXTURE_2D, texture);
    }
    GLint requested_filter = GL_LINEAR;
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &requested_filter);
    const GLint portable_filter = bridge_gl::PortableRenderTargetMinFilter(
        info.allocated_internal_format, true, requested_filter);
    if (portable_filter != requested_filter) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, portable_filter);
        if (PostFxTraceEnabled() && !info.min_filter_override_reported) {
            info.min_filter_override_reported = true;
            LOGI("postfx_min_filter_fix texture=%u allocated=0x%04x "
                 "requested=0x%04x portable=0x%04x",
                 texture, info.allocated_internal_format, requested_filter,
                 portable_filter);
        }
    }
    if (static_cast<GLuint>(previous_binding) != texture) {
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous_binding));
    }
}

void TracePostFxUseProgram(Dynarmic::A32::Jit& jit, DynarmicHost& host) {
    // The guest call at rglRenderer::draw_postFX_node+0xc4 keeps the node,
    // renderer and context in callee-saved registers.  The guest LR includes
    // the ELF load bias, so compare its module-relative low 24 bits.  Restrict
    // guest-memory inspection to that call site so ordinary glUseProgram calls
    // cannot be mistaken for a PostFX node.
    constexpr uint32_t kDrawPostFxUseProgramReturn = 0x001EAFC0u;
    constexpr uint32_t kRendererEffectSlot = 0x0000AF60u;
    constexpr uint32_t kNodeTechniqueName = 0x0000000Cu;
    constexpr uint32_t kNodeCachedProgram = 0x0000009Cu;
    constexpr uint32_t kEffectTechniqueCount = 0x00000044u;
    constexpr size_t kMaxTechniqueNameBytes = 95;
    constexpr uint32_t kMaxReports = 64;

    if (!PostFxTraceEnabled()) return;
    const auto& compatibility = CurrentGlCompatibilityState();
    const bool exact_caller =
        (jit.Regs()[14] & 0x00FFFFFFu) == kDrawPostFxUseProgramReturn;
    // The thunk trampoline may replace LR before dispatching on some loader
    // builds.  A zero program after the first floating-point PostFX attachment
    // is an equally narrow diagnostic fallback; include LR in the report so
    // the exact dispatch convention remains observable.
    if (!exact_caller &&
        (jit.Regs()[0] != 0 || !compatibility.postfx_float_pass_seen)) return;
    static thread_local uint32_t report_count = 0;
    if (report_count++ >= kMaxReports) return;

    const uint32_t renderer = jit.Regs()[6];
    const uint32_t context = jit.Regs()[5];
    const uint32_t node = jit.Regs()[9];
    const uint32_t effect = host.MemoryRead32(renderer + kRendererEffectSlot);
    const uint32_t technique_count = effect != 0
        ? host.MemoryRead32(effect + kEffectTechniqueCount)
        : 0;
    const uint32_t cached_program = host.MemoryRead32(node + kNodeCachedProgram);
    const uint32_t heap_name = host.MemoryRead32(node + kNodeTechniqueName + 4u);
    const uint32_t name_address = heap_name != 0
        ? heap_name
        : node + kNodeTechniqueName + 4u;
    char technique[kMaxTechniqueNameBytes + 1] = {};
    size_t length = 0;
    for (; length < kMaxTechniqueNameBytes; ++length) {
        technique[length] = static_cast<char>(host.MemoryRead8(
            name_address + static_cast<uint32_t>(length)));
        if (technique[length] == '\0') break;
    }
    technique[kMaxTechniqueNameBytes] = '\0';

    LOGI("postfx_program requested=%u cached=%u caller_lr=0x%08x exact_caller=%u "
         "renderer=0x%08x context=0x%08x node=0x%08x effect=0x%08x "
         "technique_count=%u technique='%s'",
         jit.Regs()[0], cached_program, jit.Regs()[14], exact_caller ? 1u : 0u,
         renderer, context, node, effect, technique_count, technique);
}

bool ConvertLegacyPixelsToRgba(GLsizei width, GLsizei height, GLenum format,
                               GLenum type, const void* pixels,
                               std::vector<uint8_t>& rgba) {
    if (!pixels || width <= 0 || height <= 0 || type != GL_UNSIGNED_BYTE) return false;
    int channels = 0;
    switch (format) {
        case GL_ALPHA:
        case GL_LUMINANCE:
            channels = 1;
            break;
        case GL_LUMINANCE_ALPHA:
            channels = 2;
            break;
        default:
            return false;
    }
    const size_t pixel_count = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (pixel_count > std::numeric_limits<size_t>::max() / 4) return false;
    GLint unpack_alignment = 4;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack_alignment);
    if (unpack_alignment != 1 && unpack_alignment != 2 &&
        unpack_alignment != 4 && unpack_alignment != 8) {
        unpack_alignment = 4;
    }
    const size_t row_bytes = static_cast<size_t>(width) * static_cast<size_t>(channels);
    const size_t source_stride =
        (row_bytes + static_cast<size_t>(unpack_alignment) - 1) &
        ~(static_cast<size_t>(unpack_alignment) - 1);
    rgba.resize(pixel_count * 4);
    const auto* source = static_cast<const uint8_t*>(pixels);
    for (GLsizei y = 0; y < height; ++y) {
        const uint8_t* row = source + static_cast<size_t>(y) * source_stride;
        for (GLsizei x = 0; x < width; ++x) {
            const uint8_t* input = row + static_cast<size_t>(x) * channels;
            uint8_t* output = rgba.data() +
                              (static_cast<size_t>(y) * width + x) * 4;
            if (format == GL_ALPHA) {
                output[0] = output[1] = output[2] = 0;
                output[3] = input[0];
            } else {
                output[0] = output[1] = output[2] = input[0];
                output[3] = format == GL_LUMINANCE_ALPHA ? input[1] : 255;
            }
        }
    }
    return true;
}

void TrackTextureImage(GLenum target, GLint level, GLint internal_format,
                       GLsizei width, GLsizei height, GLenum format, GLenum type,
                       const void* pixels) {
    if (target != GL_TEXTURE_2D || level != 0) return;
    GLint binding = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
    if (binding <= 0) return;
    auto& state = CurrentGlCompatibilityState();
    const GLuint texture = static_cast<GLuint>(binding);
    TextureImageInfo info;
    info.width = width;
    info.height = height;
    info.source_internal_format = internal_format;
    info.source_format = format;
    info.source_type = type;
    info.source_had_pixels = pixels != nullptr;
    info.legacy_single_channel =
        bridge_gl::IsLegacySingleChannelColorFormat(internal_format, format);
    info.allocated_internal_format = static_cast<GLenum>(internal_format);
    if (info.legacy_single_channel) {
        ConvertLegacyPixelsToRgba(width, height, format, type, pixels, info.initial_rgba);
    }
    state.textures[texture] = std::move(info);
}

void PromoteTextureToRgba(GLuint texture, TextureImageInfo& info,
                          GLenum internal_format) {
    GLint previous_binding = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_binding);
    glBindTexture(GL_TEXTURE_2D, texture);
    const void* pixels = info.initial_rgba.empty() ? nullptr : info.initial_rgba.data();
    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(internal_format),
                 info.width, info.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous_binding));
    info.promoted = true;
    info.allocated_internal_format = internal_format;
}

void LogFramebuffer(GLuint framebuffer, GLuint texture,
                    const TextureImageInfo* texture_info,
                    GLenum status, const char* stage) {
    auto& state = CurrentGlCompatibilityState();
    const GLenum allocated_format =
        texture_info ? texture_info->allocated_internal_format : 0;
    LOGI("render_fbo stage=%s fbo=%u texture=%u size=%dx%d source_internal=0x%04x "
         "source_format=0x%04x source_type=0x%04x allocated=%s(0x%04x) gles=%d.%d "
         "status=0x%04x vendor='%s' renderer='%s' version='%s'",
         stage, framebuffer, texture,
         texture_info ? texture_info->width : 0,
         texture_info ? texture_info->height : 0,
         texture_info ? texture_info->source_internal_format : 0,
         texture_info ? texture_info->source_format : 0,
         texture_info ? texture_info->source_type : 0,
         bridge_gl::ShadowInternalFormatName(allocated_format), allocated_format,
         state.version.major, state.version.minor, status,
         SafeGlString(GL_VENDOR), SafeGlString(GL_RENDERER), SafeGlString(GL_VERSION));
}

void ValidateFramebuffer(GLenum target, GLuint framebuffer,
                         FramebufferInfo& framebuffer_info) {
    auto& state = CurrentGlCompatibilityState();
    auto texture_it = state.textures.find(framebuffer_info.color_texture);
    TextureImageInfo* texture_info =
        texture_it == state.textures.end() ? nullptr : &texture_it->second;
    const GLenum allocated_format =
        texture_info ? texture_info->allocated_internal_format : 0;

    GLenum status = glCheckFramebufferStatus(target);
    const bool status_changed = texture_info
        ? texture_info->framebuffer_status.Record(allocated_format, status)
        : framebuffer_info.untracked_logged_status.Record(allocated_format, status);
    if (status_changed) {
        LogFramebuffer(framebuffer, framebuffer_info.color_texture,
                       texture_info, status, "attach");
    }
    if (status == GL_FRAMEBUFFER_COMPLETE) {
        if (texture_info) texture_info->framebuffer_disabled_reported = false;
        framebuffer_info.disabled = false;
        return;
    }
    const bool was_disabled = texture_info
        ? texture_info->framebuffer_disabled_reported
        : framebuffer_info.disabled;
    if (texture_info && !texture_info->framebuffer_fallback_attempted &&
        !texture_info->source_had_pixels) {
        texture_info->framebuffer_fallback_attempted = true;
        // Re-specifying level zero releases the incomplete storage while
        // preserving the guest-owned texture name and sampler parameters.
        PromoteTextureToRgba(framebuffer_info.color_texture, *texture_info, GL_RGBA);
        glFramebufferTexture2D(target, GL_COLOR_ATTACHMENT0,
                               framebuffer_info.texture_target,
                               framebuffer_info.color_texture,
                               framebuffer_info.level);
        status = glCheckFramebufferStatus(target);
        texture_info->framebuffer_status.Record(GL_RGBA, status);
        LogFramebuffer(framebuffer, framebuffer_info.color_texture,
                       texture_info, status, "rgba8_fallback");
    }
    framebuffer_info.disabled = status != GL_FRAMEBUFFER_COMPLETE;
    if (texture_info) {
        texture_info->framebuffer_disabled_reported = framebuffer_info.disabled;
    }
    if (framebuffer_info.disabled && !was_disabled) {
        LOGE("render_fbo disabled fbo=%u texture=%u after fallback status=0x%04x",
             framebuffer, framebuffer_info.color_texture, status);
    }
}

bool FramebufferDrawDisabled() {
    auto& state = CurrentGlCompatibilityState();
    const auto it = state.framebuffers.find(state.bound_framebuffer);
    return it != state.framebuffers.end() && it->second.disabled;
}

void TracePostFxSurface(const char* stage) {
    auto& state = CurrentGlCompatibilityState();
    const GLuint framebuffer = state.bound_framebuffer;
    GLuint texture = 0;
    const TextureImageInfo* texture_info = nullptr;
    const auto framebuffer_it = state.framebuffers.find(framebuffer);
    if (framebuffer_it != state.framebuffers.end()) {
        texture = framebuffer_it->second.color_texture;
        const auto texture_it = state.textures.find(texture);
        if (texture_it != state.textures.end()) texture_info = &texture_it->second;
    }
    const bool float_surface = texture_info &&
        texture_info->allocated_internal_format == bridge_gl::kGlRgba16f;
    if ((!float_surface && framebuffer != 0) ||
        (framebuffer == 0 && !state.postfx_float_pass_seen) ||
        !PostFxTraceEnabled()) {
        return;
    }

    GLint viewport[4] = {};
    GLint program = 0;
    GLint active_texture = 0;
    GLint array_buffer = 0;
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active_texture);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &array_buffer);

    GLint texture_bindings[5] = {};
    GLint texture_min_filters[5] = {};
    GLint texture_base_levels[5] = {};
    GLint texture_max_levels[5] = {};
    for (int unit = 0; unit < 5; ++unit) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture_bindings[unit]);
        if (texture_bindings[unit] != 0) {
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                                &texture_min_filters[unit]);
            glGetTexParameteriv(GL_TEXTURE_2D, kGlTextureBaseLevel,
                                &texture_base_levels[unit]);
            glGetTexParameteriv(GL_TEXTURE_2D, kGlTextureMaxLevel,
                                &texture_max_levels[unit]);
        }
    }
    glActiveTexture(static_cast<GLenum>(active_texture));

    GLint sampler_units[5] = {-1, -1, -1, -1, -1};
    GLfloat half_pixel_viewport[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
    GLfloat hdr_frame_time = -1.0f;
    GLfloat dof_focus = -1.0f;
    GLfloat dof_range = -1.0f;
    if (program != 0) {
        constexpr const char* kSamplerNames[5] = {
            "postFX_sampler0", "postFX_sampler1", "postFX_sampler2",
            "postFX_sampler3", "postFX_sampler4",
        };
        for (int sampler = 0; sampler < 5; ++sampler) {
            const GLint location = glGetUniformLocation(
                static_cast<GLuint>(program), kSamplerNames[sampler]);
            if (location >= 0) {
                glGetUniformiv(static_cast<GLuint>(program), location,
                               &sampler_units[sampler]);
            }
        }
        const auto read_uniform = [program](const char* name, GLfloat* values) {
            const GLint location = glGetUniformLocation(
                static_cast<GLuint>(program), name);
            if (location >= 0) {
                glGetUniformfv(static_cast<GLuint>(program), location, values);
            }
        };
        read_uniform("g_HalfPixel_ViewportSizeInv", half_pixel_viewport);
        read_uniform("g_HDR_frameTime", &hdr_frame_time);
        read_uniform("g_DOF_Focus", &dof_focus);
        read_uniform("g_DOF_Range", &dof_range);
    }

    GLint blend_src_rgb = 0;
    GLint blend_dst_rgb = 0;
    GLint blend_src_alpha = 0;
    GLint blend_dst_alpha = 0;
    GLint blend_equation_rgb = 0;
    GLint blend_equation_alpha = 0;
    GLboolean color_mask[4] = {};
    glGetIntegerv(GL_BLEND_SRC_RGB, &blend_src_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &blend_dst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &blend_src_alpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &blend_dst_alpha);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &blend_equation_rgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &blend_equation_alpha);
    glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);

    const GLenum pre_read_error = glGetError();
    GLint implementation_read_format = GL_RGBA;
    GLint implementation_read_type = GL_UNSIGNED_BYTE;
    glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT,
                  &implementation_read_format);
    glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE,
                  &implementation_read_type);
    float center[4] = {};
    uint32_t nonzero_rgb_samples = 0;
    float max_abs_rgb = 0.0f;
    GLenum read_error = GL_NO_ERROR;
    bool readback_type_supported = true;
    if (viewport[2] > 0 && viewport[3] > 0) {
        constexpr int kFractions[] = {1, 2, 3};
        for (int fy : kFractions) {
            for (int fx : kFractions) {
                alignas(16) uint8_t storage[64] = {};
                float pixel[4] = {};
                const GLint x = viewport[0] + viewport[2] * fx / 4;
                const GLint y = viewport[1] + viewport[3] * fy / 4;
                glReadPixels(x, y, 1, 1,
                             static_cast<GLenum>(implementation_read_format),
                             static_cast<GLenum>(implementation_read_type), storage);
                read_error = glGetError();
                if (read_error != GL_NO_ERROR) break;
                readback_type_supported = DecodeReadbackPixel(
                    storage, static_cast<GLenum>(implementation_read_type), pixel);
                if (!readback_type_supported) break;
                const float sample_max = std::max(
                    std::fabs(pixel[0]),
                    std::max(std::fabs(pixel[1]), std::fabs(pixel[2])));
                max_abs_rgb = std::max(max_abs_rgb, sample_max);
                if (sample_max > 1.0e-7f) ++nonzero_rgb_samples;
                if (fx == 2 && fy == 2) memcpy(center, pixel, sizeof(center));
            }
            if (read_error != GL_NO_ERROR || !readback_type_supported) break;
        }
    }

    static thread_local uint32_t report_count = 0;
    if (report_count++ >= 256) return;
    LOGI("postfx_trace stage=%s fbo=%u texture=%u size=%dx%d allocated=0x%04x "
         "viewport=%d,%d,%d,%d program=%d array_buffer=%d active_texture=0x%04x "
         "tex0=%d tex1=%d tex2=%d tex3=%d tex4=%d "
         "min0=0x%04x min1=0x%04x min2=0x%04x min3=0x%04x min4=0x%04x "
         "base0=%d base1=%d base2=%d base3=%d base4=%d "
         "max0=%d max1=%d max2=%d max3=%d max4=%d "
         "sampler0=%d sampler1=%d sampler2=%d sampler3=%d sampler4=%d "
         "half_pixel=%.9g,%.9g,%.9g,%.9g hdr_frame_time=%.9g "
         "dof_focus=%.9g dof_range=%.9g "
         "blend=%d blend_rgb=0x%04x,0x%04x blend_alpha=0x%04x,0x%04x "
         "blend_eq=0x%04x,0x%04x color_mask=%u,%u,%u,%u "
         "depth=%d scissor=%d cull=%d samples_nonzero_rgb=%u/9 "
         "samples_max_abs_rgb=%.9g center=%.9g,%.9g,%.9g,%.9g "
         "read_format=0x%04x read_type=0x%04x read_type_supported=%u "
         "pre_read_error=0x%04x read_error=0x%04x",
         stage, framebuffer, texture,
         texture_info ? texture_info->width : 0,
         texture_info ? texture_info->height : 0,
         texture_info ? texture_info->allocated_internal_format : 0,
         viewport[0], viewport[1], viewport[2], viewport[3], program, array_buffer,
         active_texture, texture_bindings[0], texture_bindings[1],
         texture_bindings[2], texture_bindings[3], texture_bindings[4],
         texture_min_filters[0], texture_min_filters[1], texture_min_filters[2],
         texture_min_filters[3], texture_min_filters[4],
         texture_base_levels[0], texture_base_levels[1], texture_base_levels[2],
         texture_base_levels[3], texture_base_levels[4],
         texture_max_levels[0], texture_max_levels[1], texture_max_levels[2],
         texture_max_levels[3], texture_max_levels[4],
         sampler_units[0], sampler_units[1], sampler_units[2], sampler_units[3],
         sampler_units[4], half_pixel_viewport[0], half_pixel_viewport[1],
         half_pixel_viewport[2], half_pixel_viewport[3], hdr_frame_time,
         dof_focus, dof_range, glIsEnabled(GL_BLEND), blend_src_rgb, blend_dst_rgb,
         blend_src_alpha, blend_dst_alpha, blend_equation_rgb, blend_equation_alpha,
         color_mask[0], color_mask[1], color_mask[2], color_mask[3],
         glIsEnabled(GL_DEPTH_TEST), glIsEnabled(GL_SCISSOR_TEST),
         glIsEnabled(GL_CULL_FACE), nonzero_rgb_samples, max_abs_rgb,
         center[0], center[1], center[2], center[3], implementation_read_format,
         implementation_read_type, readback_type_supported ? 1u : 0u,
         pre_read_error, read_error);
}

bool CompressedFormatSupported(GLenum format, const GlCompatibilityState& state) {
    return state.compressed_formats.count(format) != 0 ||
           bridge_gl::VersionGuaranteesCompressedFormat(format, state.version) ||
           bridge_gl::ExtensionGuaranteesCompressedFormat(format, state.extensions);
}

bool DecodeCompressedTexture(GLenum format, GLsizei width, GLsizei height,
                             GLsizei image_size, const void* data,
                             std::vector<uint32_t>& decoded) {
    const bridge_gl::BlockCompressionInfo block = bridge_gl::CompressedBlockInfo(format);
    const size_t expected = bridge_gl::ExpectedCompressedBytes(width, height, block);
    if (!block || !data || expected == 0 || image_size < 0 ||
        static_cast<size_t>(image_size) != expected) {
        return false;
    }
    const size_t pixel_count = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (pixel_count > std::numeric_limits<size_t>::max() / sizeof(uint32_t)) return false;
    decoded.resize(pixel_count);
    const auto* source = static_cast<const uint8_t*>(data);
    int ok = 0;
    if (bridge_gl::IsAstcFormat(format)) {
        ok = decode_astc(source, width, height, block.block_width,
                         block.block_height, decoded.data());
    } else {
        switch (format) {
            case bridge_gl::kGlEtc1Rgb8Oes:
                ok = decode_etc1(source, width, height, decoded.data());
                break;
            case bridge_gl::kGlCompressedRgb8Etc2:
            case bridge_gl::kGlCompressedSrgb8Etc2:
                ok = decode_etc2(source, width, height, decoded.data());
                break;
            case bridge_gl::kGlCompressedRgb8PunchthroughAlpha1Etc2:
            case bridge_gl::kGlCompressedSrgb8PunchthroughAlpha1Etc2:
                ok = decode_etc2a1(source, width, height, decoded.data());
                break;
            case bridge_gl::kGlCompressedRgba8Etc2Eac:
            case bridge_gl::kGlCompressedSrgb8Alpha8Etc2Eac:
                ok = decode_etc2a8(source, width, height, decoded.data());
                break;
            case bridge_gl::kGlCompressedR11Eac:
                ok = decode_eacr(source, width, height, decoded.data());
                break;
            case bridge_gl::kGlCompressedSignedR11Eac:
                ok = decode_eacr_signed(source, width, height, decoded.data());
                break;
            case bridge_gl::kGlCompressedRg11Eac:
                ok = decode_eacrg(source, width, height, decoded.data());
                break;
            case bridge_gl::kGlCompressedSignedRg11Eac:
                ok = decode_eacrg_signed(source, width, height, decoded.data());
                break;
            default:
                return false;
        }
    }
    if (!ok) return false;

    // Texture2DDecoder emits BGRA8 bytes on little-endian Android. GLES2's
    // portable uncompressed upload is RGBA8, so normalize in place.
    auto* bytes = reinterpret_cast<uint8_t*>(decoded.data());
    for (size_t i = 0; i < pixel_count; ++i) {
        std::swap(bytes[i * 4], bytes[i * 4 + 2]);
    }
    return true;
}

}  // namespace

void ThunkManager::RegisterGLThunks() {
    uint8_t* mem = m_host.GetGuestMemoryBase();

    RegisterThunk("glActiveTexture", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLenum texture = static_cast<GLenum>(jit.Regs()[0]);
        glActiveTexture(texture);
        if (GlResourceTraceEnabled()) {
            CurrentGlResourceTraceState().active_texture = texture;
        }
    });

    RegisterThunk("glAttachShader", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glAttachShader(jit.Regs()[0], jit.Regs()[1]);
    });

    RegisterThunk("glBindBuffer", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLenum target = static_cast<GLenum>(jit.Regs()[0]);
        const GLuint buffer = jit.Regs()[1];
        glBindBuffer(target, buffer);
        if (GlResourceTraceEnabled()) {
            auto& state = CurrentGlResourceTraceState();
            if (target == GL_ARRAY_BUFFER) state.bound_array_buffer = buffer;
            if (target == GL_ELEMENT_ARRAY_BUFFER) state.bound_element_array_buffer = buffer;
            if (buffer != 0) state.buffers.insert(buffer);
        }
    });

    RegisterThunk("glBindFramebuffer", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLenum target = static_cast<GLenum>(jit.Regs()[0]);
        const GLuint framebuffer = jit.Regs()[1];
        auto& state = CurrentGlCompatibilityState();
        if (target == GL_FRAMEBUFFER && state.bound_framebuffer != framebuffer) {
            TracePostFxSurface("before_bind");
        }
        glBindFramebuffer(target, framebuffer);
        if (target == GL_FRAMEBUFFER) {
            state.bound_framebuffer = framebuffer;
        }
    });

    RegisterThunk("glBindRenderbuffer", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLenum target = static_cast<GLenum>(jit.Regs()[0]);
        const GLuint renderbuffer = jit.Regs()[1];
        glBindRenderbuffer(target, renderbuffer);
        if (GlResourceTraceEnabled() && target == GL_RENDERBUFFER) {
            auto& state = CurrentGlResourceTraceState();
            state.bound_renderbuffer = renderbuffer;
            if (renderbuffer != 0) state.renderbuffers.insert(renderbuffer);
        }
    });

    RegisterThunk("glBindTexture", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLenum target = static_cast<GLenum>(jit.Regs()[0]);
        const GLuint texture = jit.Regs()[1];
        glBindTexture(target, texture);
        if (GlResourceTraceEnabled() && target == GL_TEXTURE_2D) {
            auto& state = CurrentGlResourceTraceState();
            state.bound_texture_2d[state.active_texture] = texture;
            if (texture != 0) state.textures.insert(texture);
        }
    });

    RegisterThunk("glBlendFunc", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glBlendFunc(static_cast<GLenum>(jit.Regs()[0]), static_cast<GLenum>(jit.Regs()[1]));
    });

    RegisterThunk("glBufferData", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        GLenum target = static_cast<GLenum>(jit.Regs()[0]);
        GLsizeiptr size = static_cast<GLsizeiptr>(jit.Regs()[1]);
        const void* data = GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[2]);
        GLenum usage = static_cast<GLenum>(jit.Regs()[3]);
        glBufferData(target, size, data, usage);
        if (GlResourceTraceEnabled()) {
            auto& state = CurrentGlResourceTraceState();
            const GLuint buffer = target == GL_ARRAY_BUFFER ? state.bound_array_buffer :
                (target == GL_ELEMENT_ARRAY_BUFFER ? state.bound_element_array_buffer : 0);
            if (buffer != 0) {
                state.buffers.insert(buffer);
                state.buffer_bytes[buffer] = size > 0 ? static_cast<uint64_t>(size) : 0;
                MaybeLogGlStorage("buffer", jit.Regs()[14],
                                  size > 0 ? static_cast<uint64_t>(size) : 0,
                                  buffer);
            }
        }
    });

    RegisterThunk("glCheckFramebufferStatus", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        jit.Regs()[0] = glCheckFramebufferStatus(static_cast<GLenum>(jit.Regs()[0]));
    });

    RegisterThunk("glClear", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        if (FramebufferDrawDisabled()) return;
        GLbitfield mask = static_cast<GLbitfield>(jit.Regs()[0]);
        if ((mask & GL_COLOR_BUFFER_BIT) != 0 &&
            bridge_gl::ReplaceDefaultFramebufferColourClear()) {
            mask &= ~GL_COLOR_BUFFER_BIT;
        }
        if (mask != 0) glClear(mask);
    });

    RegisterThunk("glClearColor", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        float r = *reinterpret_cast<float*>(&jit.Regs()[0]);
        float g = *reinterpret_cast<float*>(&jit.Regs()[1]);
        float b = *reinterpret_cast<float*>(&jit.Regs()[2]);
        float a = *reinterpret_cast<float*>(&jit.Regs()[3]);
        glClearColor(r, g, b, a);
    });

    RegisterThunk("glColorMask", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glColorMask(jit.Regs()[0], jit.Regs()[1], jit.Regs()[2], jit.Regs()[3]);
    });

    RegisterThunk("glCompileShader", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        GLuint shader = jit.Regs()[0];
        if (!ShaderTraceEnabled()) {
            glCompileShader(shader);
            return;
        }
        ShaderTimingState& timing = CurrentShaderTimingState();
        LogShaderEnvironment(timing);
        const auto started = std::chrono::steady_clock::now();
        if (timing.compile_calls == 0) timing.batch_started = started;
        glCompileShader(shader);
        const auto finished = std::chrono::steady_clock::now();
        timing.compile_nanoseconds += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count());
        ++timing.compile_calls;
        GLint compiled = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        if (!compiled) {
            LogShaderCompileFailure(shader, timing);
        }
    });

    RegisterThunk("glCompressedTexImage2D", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        GLenum target = jit.Regs()[0];
        GLint level = jit.Regs()[1];
        GLenum internalformat = jit.Regs()[2];
        GLsizei width = jit.Regs()[3];
        uint32_t sp = jit.Regs()[13];
        GLsizei height = host.MemoryRead32(sp);
        GLint border = host.MemoryRead32(sp + 4);
        GLsizei imageSize = host.MemoryRead32(sp + 8);
        const void* data = GuestToHost(host.GetGuestMemoryBase(), host.MemoryRead32(sp + 12));
        const bool trace = TextureTraceEnabled();
        const auto started = trace ? std::chrono::steady_clock::now()
                                   : std::chrono::steady_clock::time_point{};
        auto& compatibility = CurrentGlCompatibilityState();
        const bool force_etc1 = ForceEtc1TranscodeForDiagnostics();
        const bool force_rgba = ForceRgbaFallbackForDiagnostics();
        const bool force_mod_rgba = ForceModRgbaFallbackForDiagnostics();
        const bool force_rgba_normal = ForceRgbaNormalFallbackForDiagnostics();
        const bool astc_format = bridge_gl::IsAstcFormat(internalformat);
        std::vector<uint8_t> stored_payload;
        uint32_t stored_format = 0;
        bool stored_has_alpha = false;
        const int stored_status = border == 0
            ? LoadConvertedModTextureToken(data, imageSize, width, height,
                                            stored_payload, stored_format, stored_has_alpha)
            : 0;
        std::vector<uint32_t> mod_decoded;
        bool mod_has_alpha = false;
        const bool mod_texture = stored_status == 0 && border == 0 &&
            DecodeModTextureToken(data, imageSize, width, height,
                                  mod_decoded, mod_has_alpha);
        const int force_rgba_size = ForceRgbaSizeForDiagnostics();
        const bool size_forced = astc_format && force_rgba_size > 0 &&
            width == force_rgba_size && height == force_rgba_size;
        GLuint bound_texture = 0;
        if (target == GL_TEXTURE_2D) {
            GLint binding = 0;
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
            if (binding > 0) bound_texture = static_cast<GLuint>(binding);
        }
        bool cached_rgba = false;
        bool has_cached_fallback = false;
        bool cached_mod_native_astc = false;
        bool has_cached_mod_native_astc = false;
        if (astc_format && bound_texture != 0) {
            if (level == 0) {
                compatibility.astc_rgba_fallback.erase(bound_texture);
                compatibility.mod_native_astc.erase(bound_texture);
            } else {
                const auto it = compatibility.astc_rgba_fallback.find(bound_texture);
                if (it != compatibility.astc_rgba_fallback.end()) {
                    cached_rgba = it->second;
                    has_cached_fallback = true;
                }
                const auto mod_it = compatibility.mod_native_astc.find(bound_texture);
                if (mod_it != compatibility.mod_native_astc.end()) {
                    cached_mod_native_astc = mod_it->second;
                    has_cached_mod_native_astc = true;
                }
            }
        }
        const bool native_supported = CompressedFormatSupported(internalformat, compatibility);
        if (stored_status != 0) {
            const bool stored_compressed = stored_format != GL_RGBA;
            const bool direct = stored_status > 0 &&
                (!stored_compressed || CompressedFormatSupported(stored_format, compatibility)) &&
                !(stored_compressed && (force_rgba || force_mod_rgba));
            if (direct && stored_compressed) {
                glCompressedTexImage2D(target, level, stored_format, width, height, 0,
                                       static_cast<GLsizei>(stored_payload.size()),
                                       stored_payload.data());
            } else if (direct) {
                glTexImage2D(target, level, GL_RGBA, width, height, 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, stored_payload.data());
            } else {
                std::vector<uint32_t> decoded;
                const bool decoded_ok = stored_status > 0 && DecodeCompressedTexture(
                    stored_format, width, height, static_cast<GLsizei>(stored_payload.size()),
                    stored_payload.data(), decoded);
                glTexImage2D(target, level, GL_RGBA, width, height, 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, decoded_ok ? decoded.data() : nullptr);
                if (!decoded_ok) {
                    LOGE("mod_texture_stored_invalid format=0x%04x size=%dx%d "
                         "status=%d", stored_format, width, height, stored_status);
                }
            }
            if (level == 0) {
                LOGI("mod_texture_stored_upload format=0x%04x size=%dx%d bytes=%zu "
                     "direct=%d alpha=%d encode=0", stored_format, width, height,
                     stored_payload.size(), direct ? 1 : 0, stored_has_alpha ? 1 : 0);
            }
            imageSize = static_cast<GLsizei>(stored_payload.size());
        } else if (!mod_texture && native_supported &&
            !(astc_format && (force_etc1 || force_rgba))) {
            glCompressedTexImage2D(target, level, internalformat, width, height,
                                   border, imageSize, data);
        } else {
            std::vector<uint32_t> decoded;
            if (mod_texture) decoded = std::move(mod_decoded);
            if (mod_texture ||
                (border == 0 && DecodeCompressedTexture(internalformat, width, height,
                                                        imageSize, data, decoded))) {
                const unsigned long long source_fingerprint =
                    static_cast<unsigned long long>(
                        TexturePayloadFingerprint(data, imageSize));
                const bool native_astc_candidate = mod_texture && astc_format &&
                    native_supported && !force_etc1 && !force_rgba && !force_mod_rgba &&
                    (!has_cached_mod_native_astc || cached_mod_native_astc);
                std::vector<uint8_t> native_astc;
                bool native_astc_encoded = false;
                if (native_astc_candidate) {
                    const bridge_gl::BlockCompressionInfo block =
                        bridge_gl::CompressedBlockInfo(internalformat);
                    native_astc_encoded = block &&
                        EncodeModTextureAstc(
                            reinterpret_cast<const uint8_t*>(decoded.data()),
                            static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                            static_cast<uint32_t>(block.block_width),
                            static_cast<uint32_t>(block.block_height), native_astc);
                    if (bound_texture != 0 && level == 0) {
                        compatibility.mod_native_astc[bound_texture] = native_astc_encoded;
                    }
                    if (!native_astc_encoded) {
                        g_texture_uploads.mod_to_astc_errors.fetch_add(
                            1, std::memory_order_relaxed);
                        LOGE("mod_texture_transcode_failed to=ASTC format=0x%04x "
                             "size=%dx%d source_bytes=%d",
                             internalformat, width, height, imageSize);
                    }
                }

                if (native_astc_encoded) {
                    const GLenum pre_upload_error = trace ? glGetError() : GL_NO_ERROR;
                    glCompressedTexImage2D(
                        target, level, internalformat, width, height, border,
                        static_cast<GLsizei>(native_astc.size()), native_astc.data());
                    const GLenum upload_error = trace ? glGetError() : GL_NO_ERROR;
                    if (upload_error != GL_NO_ERROR) {
                        g_texture_uploads.mod_to_astc_errors.fetch_add(
                            1, std::memory_order_relaxed);
                    }
                    if (trace &&
                        (pre_upload_error != GL_NO_ERROR || upload_error != GL_NO_ERROR)) {
                        LOGE("mod_texture_astc_upload_error size=%dx%d "
                             "pre_error=0x%04x error=0x%04x output_bytes=%zu",
                             width, height, pre_upload_error, upload_error,
                             native_astc.size());
                    }
                    g_texture_uploads.mod_to_astc_calls.fetch_add(
                        1, std::memory_order_relaxed);
                    g_texture_uploads.mod_to_astc_bytes.fetch_add(
                        static_cast<uint64_t>(native_astc.size()),
                        std::memory_order_relaxed);
                    LOGI("mod_texture_transcode dxt_token=1 to=ASTC format=0x%04x "
                         "size=%dx%d source_bytes=%d output_bytes=%zu gles=%d.%d "
                         "source_fingerprint=0x%016llx vendor='%s' renderer='%s'",
                         internalformat, width, height, imageSize, native_astc.size(),
                         compatibility.version.major, compatibility.version.minor,
                         source_fingerprint,
                         SafeGlString(GL_VENDOR), SafeGlString(GL_RENDERER));
                } else {
                    std::vector<uint8_t> etc1;
                    const bool etc1_supported = CompressedFormatSupported(
                        bridge_gl::kGlEtc1Rgb8Oes, compatibility);
                    const bool normal_like = !has_cached_fallback && force_rgba_normal &&
                        LooksLikeNormalMap(decoded);
                    const bool rgba_fallback = force_rgba ||
                        (mod_texture && force_mod_rgba) || mod_has_alpha ||
                        cached_rgba || normal_like ||
                        (!has_cached_fallback && size_forced);
                    if (astc_format && bound_texture != 0 && !has_cached_fallback) {
                        compatibility.astc_rgba_fallback[bound_texture] = rgba_fallback;
                    }
                    const bool transcoded = astc_format && !rgba_fallback &&
                        etc1_supported &&
                        bridge_gl::EncodeEtc1(
                            reinterpret_cast<const uint8_t*>(decoded.data()),
                            width, height, etc1);
                    if (transcoded) {
                        const GLenum pre_upload_error = trace ? glGetError() : GL_NO_ERROR;
                        glCompressedTexImage2D(
                            target, level, bridge_gl::kGlEtc1Rgb8Oes, width, height,
                            border, static_cast<GLsizei>(etc1.size()), etc1.data());
                        const GLenum upload_error = trace ? glGetError() : GL_NO_ERROR;
                        if (upload_error != GL_NO_ERROR) {
                            g_texture_uploads.astc_to_etc1_errors.fetch_add(
                                1, std::memory_order_relaxed);
                        }
                        if (trace &&
                            (pre_upload_error != GL_NO_ERROR ||
                             upload_error != GL_NO_ERROR)) {
                            LOGE("compressed_texture_transcode_upload_error size=%dx%d "
                                 "pre_error=0x%04x error=0x%04x output_bytes=%zu",
                                 width, height, pre_upload_error, upload_error,
                                 etc1.size());
                        }
                        g_texture_uploads.astc_to_etc1_calls.fetch_add(
                            1, std::memory_order_relaxed);
                        g_texture_uploads.astc_to_etc1_bytes.fetch_add(
                            static_cast<uint64_t>(etc1.size()),
                            std::memory_order_relaxed);
                        LOGI("compressed_texture_transcode astc=0x%04x to=ETC1 "
                             "size=%dx%d source_bytes=%d output_bytes=%zu gles=%d.%d "
                             "source_fingerprint=0x%016llx forced=%s vendor='%s' "
                             "renderer='%s'",
                             internalformat, width, height, imageSize, etc1.size(),
                             compatibility.version.major, compatibility.version.minor,
                             source_fingerprint, force_etc1 ? "true" : "false",
                             SafeGlString(GL_VENDOR), SafeGlString(GL_RENDERER));
                    } else {
                        glTexImage2D(target, level, GL_RGBA, width, height, 0,
                                     GL_RGBA, GL_UNSIGNED_BYTE, decoded.data());
                        LOGI("compressed_texture_fallback format=0x%04x size=%dx%d bytes=%d "
                             "path=rgba8 alpha_or_etc1_unavailable=%s gles=%d.%d "
                             "source_fingerprint=0x%016llx vendor='%s' renderer='%s'",
                             internalformat, width, height, imageSize,
                             (force_rgba && astc_format) ? "diagnostic_forced" :
                                 (normal_like ? "diagnostic_forced_normal" :
                                 (size_forced ? "diagnostic_forced_size" :
                                     (etc1_supported ? "alpha" :
                                         "etc1_unavailable"))),
                             compatibility.version.major, compatibility.version.minor,
                             source_fingerprint,
                             SafeGlString(GL_VENDOR), SafeGlString(GL_RENDERER));
                    }
                }
            } else {
                // Never forward an enum the context has already proved it does
                // not support. Allocate valid transparent RGBA storage so the
                // caller gets deterministic failure containment.
                glTexImage2D(target, level, GL_RGBA, width, height, 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                LOGE("compressed_texture_unsupported format=0x%04x size=%dx%d bytes=%d "
                     "gles=%d.%d decode_failed=true",
                     internalformat, width, height, imageSize,
                     compatibility.version.major, compatibility.version.minor);
            }
        }
        if (GlResourceTraceEnabled() && target == GL_TEXTURE_2D && bound_texture != 0) {
            TrackTextureStorage(bound_texture, level,
                                imageSize > 0 ? static_cast<uint64_t>(imageSize) : 0,
                                jit.Regs()[14], "compressed_texture", width, height);
        }
        auto& c = g_texture_uploads.compressed_calls;
        const uint64_t total = c.fetch_add(1, std::memory_order_relaxed) + 1;
        g_texture_uploads.compressed_bytes.fetch_add(
            static_cast<uint64_t>(imageSize > 0 ? imageSize : 0), std::memory_order_relaxed);
        if (trace) {
            g_texture_uploads.compressed_nanoseconds.fetch_add(
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now() - started).count()),
                std::memory_order_relaxed);
            if (total % 32 == 0) RecordTextureUploadLog();
        }
    });

    RegisterThunk("glCreateProgram", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLuint program = glCreateProgram();
        jit.Regs()[0] = program;
        if (GlResourceTraceEnabled() && program != 0) {
            auto& state = CurrentGlResourceTraceState();
            state.programs.insert(program);
            ++state.created_programs;
            MaybeLogGlLifetime("create_program", jit.Regs()[14]);
        }
    });

    RegisterThunk("glCreateShader", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLuint shader = glCreateShader(static_cast<GLenum>(jit.Regs()[0]));
        jit.Regs()[0] = shader;
        if (GlResourceTraceEnabled() && shader != 0) {
            auto& state = CurrentGlResourceTraceState();
            state.shaders.insert(shader);
            ++state.created_shaders;
            MaybeLogGlLifetime("create_shader", jit.Regs()[14]);
        }
    });

    RegisterThunk("glCullFace", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glCullFace(static_cast<GLenum>(jit.Regs()[0]));
    });

    RegisterThunk("glDeleteBuffers", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLsizei count = jit.Regs()[0];
        const auto* buffers = reinterpret_cast<const GLuint*>(
            GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]));
        if (!DeferDeleteWithoutContext(DeferredDeleteKind::Buffer, count, buffers)) {
            glDeleteBuffers(count, buffers);
        }
        if (GlResourceTraceEnabled() && buffers) {
            auto& state = CurrentGlResourceTraceState();
            for (GLsizei i = 0; i < count; ++i) {
                state.buffers.erase(buffers[i]);
                state.buffer_bytes.erase(buffers[i]);
                if (state.bound_array_buffer == buffers[i]) state.bound_array_buffer = 0;
                if (state.bound_element_array_buffer == buffers[i]) {
                    state.bound_element_array_buffer = 0;
                }
                ++state.deleted_buffers;
            }
            MaybeLogGlLifetime("delete_buffers", jit.Regs()[14]);
        }
    });

    RegisterThunk("glDeleteFramebuffers", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLsizei count = jit.Regs()[0];
        const auto* framebuffers = reinterpret_cast<const GLuint*>(
            GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]));
        auto& state = CurrentGlCompatibilityState();
        for (GLsizei i = 0; framebuffers && i < count; ++i) {
            state.framebuffers.erase(framebuffers[i]);
            if (state.bound_framebuffer == framebuffers[i]) state.bound_framebuffer = 0;
        }
        if (!DeferDeleteWithoutContext(DeferredDeleteKind::Framebuffer,
                                       count, framebuffers)) {
            glDeleteFramebuffers(count, framebuffers);
        }
        if (GlResourceTraceEnabled() && framebuffers) {
            auto& trace_state = CurrentGlResourceTraceState();
            for (GLsizei i = 0; i < count; ++i) {
                trace_state.framebuffers.erase(framebuffers[i]);
                ++trace_state.deleted_framebuffers;
            }
            MaybeLogGlLifetime("delete_framebuffers", jit.Regs()[14]);
        }
    });

    RegisterThunk("glDeleteProgram", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLuint program = jit.Regs()[0];
        auto& compatibility = CurrentGlCompatibilityState();
        compatibility.postfx_programs.erase(program);
        compatibility.map_water_programs.erase(program);
        if (!DeferDeleteWithoutContext(DeferredDeleteKind::Program, program)) {
            glDeleteProgram(program);
        }
        if (GlResourceTraceEnabled()) {
            auto& state = CurrentGlResourceTraceState();
            state.programs.erase(program);
            ++state.deleted_programs;
            MaybeLogGlLifetime("delete_program", jit.Regs()[14]);
        }
    });

    RegisterThunk("glDeleteRenderbuffers", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLsizei count = jit.Regs()[0];
        const auto* renderbuffers = reinterpret_cast<const GLuint*>(
            GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]));
        if (!DeferDeleteWithoutContext(DeferredDeleteKind::Renderbuffer,
                                       count, renderbuffers)) {
            glDeleteRenderbuffers(count, renderbuffers);
        }
        if (GlResourceTraceEnabled() && renderbuffers) {
            auto& state = CurrentGlResourceTraceState();
            for (GLsizei i = 0; i < count; ++i) {
                state.renderbuffers.erase(renderbuffers[i]);
                state.renderbuffer_bytes.erase(renderbuffers[i]);
                if (state.bound_renderbuffer == renderbuffers[i]) {
                    state.bound_renderbuffer = 0;
                }
                ++state.deleted_renderbuffers;
            }
            MaybeLogGlLifetime("delete_renderbuffers", jit.Regs()[14]);
        }
    });

    RegisterThunk("glDeleteShader", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLuint shader = jit.Regs()[0];
        if (!DeferDeleteWithoutContext(DeferredDeleteKind::Shader, shader)) {
            glDeleteShader(shader);
        }
        if (GlResourceTraceEnabled()) {
            auto& state = CurrentGlResourceTraceState();
            state.shaders.erase(shader);
            ++state.deleted_shaders;
            MaybeLogGlLifetime("delete_shader", jit.Regs()[14]);
        }
    });

    RegisterThunk("glDeleteTextures", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLsizei count = jit.Regs()[0];
        const auto* textures = reinterpret_cast<const GLuint*>(
            GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]));
        auto& state = CurrentGlCompatibilityState();
        for (GLsizei i = 0; textures && i < count; ++i) {
            state.textures.erase(textures[i]);
            state.astc_rgba_fallback.erase(textures[i]);
            state.mod_native_astc.erase(textures[i]);
            for (auto it = state.framebuffers.begin();
                 it != state.framebuffers.end();) {
                if (it->second.color_texture == textures[i]) {
                    it = state.framebuffers.erase(it);
                } else {
                    ++it;
                }
            }
        }
        if (!DeferDeleteWithoutContext(DeferredDeleteKind::Texture, count, textures)) {
            glDeleteTextures(count, textures);
        }
        if (GlResourceTraceEnabled() && textures) {
            auto& trace_state = CurrentGlResourceTraceState();
            for (GLsizei i = 0; i < count; ++i) {
                trace_state.textures.erase(textures[i]);
                ForgetTextureStorage(textures[i]);
                for (auto& [unit, binding] : trace_state.bound_texture_2d) {
                    (void)unit;
                    if (binding == textures[i]) binding = 0;
                }
                ++trace_state.deleted_textures;
            }
            MaybeLogGlLifetime("delete_textures", jit.Regs()[14]);
        }
    });

    RegisterThunk("glDepthFunc", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glDepthFunc(static_cast<GLenum>(jit.Regs()[0]));
    });

    RegisterThunk("glDepthMask", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glDepthMask(static_cast<GLboolean>(jit.Regs()[0]));
    });

    RegisterThunk("glDisable", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glDisable(static_cast<GLenum>(jit.Regs()[0]));
    });

    RegisterThunk("glDisableVertexAttribArray", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glDisableVertexAttribArray(jit.Regs()[0]);
    });

    RegisterThunk("glDrawArrays", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        if (FramebufferDrawDisabled()) return;
        const GLenum mode = static_cast<GLenum>(jit.Regs()[0]);
        const GLint first = static_cast<GLint>(jit.Regs()[1]);
        const GLsizei count = static_cast<GLsizei>(jit.Regs()[2]);
        auto& state = CurrentGlCompatibilityState();
        bridge_gl::ClearDefaultFramebufferBeforeDraw(
            static_cast<int>(state.bound_framebuffer));
        const bool postfx_draw = BoundProgramUsesPostFxSamplers(state);
        if (postfx_draw) {
            FullscreenPostFxStateGuard state_guard;
            ApplyPortablePostFxUniformDefaults(state);
            glDrawArrays(mode, first, count);
            TracePostFxSurface("after_fullscreen_draw");
        } else {
            glDrawArrays(mode, first, count);
        }
    });

    RegisterThunk("glDrawElements", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        if (FramebufferDrawDisabled()) return;
        auto& state = CurrentGlCompatibilityState();
        bridge_gl::ClearDefaultFramebufferBeforeDraw(
            static_cast<int>(state.bound_framebuffer));
        const bool postfx_draw = BoundProgramUsesPostFxSamplers(state);
        const uint32_t indices_gaddr = jit.Regs()[3];
        // Client-memory null and index-buffer offset zero are both nullptr.
        const void* indices = nullptr;
        if (indices_gaddr != 0u) {
            GLint element_buffer = 0;
            glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &element_buffer);
            indices = (element_buffer != 0) ?
                reinterpret_cast<const void*>(static_cast<uintptr_t>(indices_gaddr)) :
                GuestToHost(host.GetGuestMemoryBase(), indices_gaddr);
        }
        const auto draw = [&] {
            glDrawElements(static_cast<GLenum>(jit.Regs()[0]),
                           static_cast<GLsizei>(jit.Regs()[1]),
                           static_cast<GLenum>(jit.Regs()[2]), indices);
        };
        if (postfx_draw) {
            FullscreenPostFxStateGuard state_guard;
            ApplyPortablePostFxUniformDefaults(state);
            draw();
            TracePostFxSurface("after_fullscreen_draw");
        } else {
            draw();
        }
    });

    RegisterThunk("glEnable", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glEnable(static_cast<GLenum>(jit.Regs()[0]));
    });

    RegisterThunk("glEnableVertexAttribArray", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glEnableVertexAttribArray(jit.Regs()[0]);
    });

    RegisterThunk("glFlush", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glFlush();
    });

    RegisterThunk("glFramebufferRenderbuffer", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLenum target = jit.Regs()[0];
        glFramebufferRenderbuffer(target, jit.Regs()[1], jit.Regs()[2], jit.Regs()[3]);
        auto& state = CurrentGlCompatibilityState();
        const auto it = state.framebuffers.find(state.bound_framebuffer);
        if (target == GL_FRAMEBUFFER && it != state.framebuffers.end()) {
            ValidateFramebuffer(target, state.bound_framebuffer, it->second);
        }
    });

    RegisterThunk("glFramebufferTexture2D", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLenum target = jit.Regs()[0];
        const GLenum attachment = jit.Regs()[1];
        const GLenum texture_target = jit.Regs()[2];
        const GLuint texture = jit.Regs()[3];
        const GLint level = host.MemoryRead32(jit.Regs()[13]);
        auto& state = CurrentGlCompatibilityState();
        auto texture_it = state.textures.find(texture);
        const bool color_attachment =
            target == GL_FRAMEBUFFER && attachment == GL_COLOR_ATTACHMENT0 &&
            state.bound_framebuffer != 0;
        const auto previous_framebuffer = state.framebuffers.find(state.bound_framebuffer);
        if (color_attachment && previous_framebuffer != state.framebuffers.end() &&
            previous_framebuffer->second.color_texture != texture) {
            TracePostFxSurface("before_attach");
        }
        bool storage_recreated = false;
        if (color_attachment && texture_target == GL_TEXTURE_2D && level == 0 &&
            texture != 0 && texture_it != state.textures.end() &&
            texture_it->second.legacy_single_channel && !texture_it->second.promoted) {
            const GLenum preferred =
                bridge_gl::PreferredShadowInternalFormat(state.version);
            PromoteTextureToRgba(texture, texture_it->second, preferred);
            storage_recreated = true;
            if (texture_it->second.source_had_pixels &&
                texture_it->second.initial_rgba.empty()) {
                LOGE("render_fbo texture=%u had unsupported legacy initial pixels; "
                     "storage recreated empty",
                     texture);
            }
        }
        if (color_attachment && texture_target == GL_TEXTURE_2D && level == 0 &&
            texture != 0 && texture_it != state.textures.end()) {
            texture_it->second.render_target = true;
            EnforcePortableRenderTargetMinFilter(texture, texture_it->second);
        }
        glFramebufferTexture2D(target, attachment, texture_target, texture, level);
        if (target != GL_FRAMEBUFFER || state.bound_framebuffer == 0) return;
        if (attachment != GL_COLOR_ATTACHMENT0) {
            const auto it = state.framebuffers.find(state.bound_framebuffer);
            if (it != state.framebuffers.end()) {
                ValidateFramebuffer(target, state.bound_framebuffer, it->second);
            }
            return;
        }
        if (texture == 0) {
            state.framebuffers.erase(state.bound_framebuffer);
            return;
        }
        auto [it, inserted] = state.framebuffers.try_emplace(state.bound_framebuffer);
        FramebufferInfo& info = it->second;
        if (inserted || storage_recreated || info.color_texture != texture ||
            info.texture_target != texture_target || info.level != level) {
            info.color_texture = texture;
            info.texture_target = texture_target;
            info.level = level;
            info.disabled = false;
        }
        const auto attached_texture = state.textures.find(texture);
        if (attached_texture != state.textures.end() &&
            attached_texture->second.allocated_internal_format == bridge_gl::kGlRgba16f) {
            state.postfx_float_pass_seen = true;
        }
        ValidateFramebuffer(target, state.bound_framebuffer, it->second);
    });

    RegisterThunk("glFrontFace", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glFrontFace(static_cast<GLenum>(jit.Regs()[0]));
    });

    RegisterThunk("glGenBuffers", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLsizei count = jit.Regs()[0];
        auto* buffers = reinterpret_cast<GLuint*>(
            GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]));
        const bool font_restore = count == 1 && buffers &&
            GuestImageOffset(jit.Regs()[14]) == kFontVboGenReturn;
        const GLuint replaced = font_restore ? buffers[0] : 0;
        const bool replaced_is_live = replaced != 0 && glIsBuffer(replaced) == GL_TRUE;
#ifndef WARBAND_BRIDGE_NO_LOGS
        bool reclaimed = false;
#endif
        if (font_restore) {
#ifndef WARBAND_BRIDGE_NO_LOGS
            auto& leak = CurrentMemoryLeakTraceState();
            ++leak.font_restore_calls;
            if (replaced_is_live) ++leak.font_live_replacements;
#endif
            if (replaced_is_live && MemoryLeakFixesEnabled()) {
                glDeleteBuffers(1, &replaced);
#ifndef WARBAND_BRIDGE_NO_LOGS
                reclaimed = true;
                ++leak.font_reclaimed;
#endif
                if (GlResourceTraceEnabled()) {
                    auto& trace = CurrentGlResourceTraceState();
                    trace.buffers.erase(replaced);
                    trace.buffer_bytes.erase(replaced);
                    if (trace.bound_array_buffer == replaced) trace.bound_array_buffer = 0;
                    if (trace.bound_element_array_buffer == replaced) {
                        trace.bound_element_array_buffer = 0;
                    }
                    ++trace.deleted_buffers;
                }
            }
        }
        glGenBuffers(count, buffers);
        if (GlResourceTraceEnabled() && buffers) {
            auto& state = CurrentGlResourceTraceState();
            for (GLsizei i = 0; i < count; ++i) {
                state.buffers.insert(buffers[i]);
                ++state.generated_buffers;
            }
            MaybeLogGlLifetime("gen_buffers", jit.Regs()[14]);
        }
#ifndef WARBAND_BRIDGE_NO_LOGS
        if (font_restore && MemoryLeakTraceEnabled()) {
            const auto& leak = CurrentMemoryLeakTraceState();
            LOGI("memory_leak_font_vbo call=%llu old=%u old_live=%d reclaimed=%d "
                 "new=%u live_replacements=%llu total_reclaimed=%llu fixes=%d",
                 static_cast<unsigned long long>(leak.font_restore_calls), replaced,
                 replaced_is_live ? 1 : 0, reclaimed ? 1 : 0, buffers[0],
                 static_cast<unsigned long long>(leak.font_live_replacements),
                 static_cast<unsigned long long>(leak.font_reclaimed),
                 MemoryLeakFixesEnabled() ? 1 : 0);
        }
#endif
    });

    RegisterThunk("glGenFramebuffers", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLsizei count = jit.Regs()[0];
        auto* framebuffers = reinterpret_cast<GLuint*>(
            GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]));
        glGenFramebuffers(count, framebuffers);
        if (GlResourceTraceEnabled() && framebuffers) {
            auto& state = CurrentGlResourceTraceState();
            for (GLsizei i = 0; i < count; ++i) {
                state.framebuffers.insert(framebuffers[i]);
                ++state.generated_framebuffers;
            }
            MaybeLogGlLifetime("gen_framebuffers", jit.Regs()[14]);
        }
    });

    RegisterThunk("glGenRenderbuffers", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLsizei count = jit.Regs()[0];
        auto* renderbuffers = reinterpret_cast<GLuint*>(
            GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]));
        const bool depth_restore = count == 1 && renderbuffers &&
            GuestImageOffset(jit.Regs()[14]) == kDepthRboGenReturn;
        const GLuint replaced = depth_restore ? renderbuffers[0] : 0;
        const bool replaced_is_live =
            replaced != 0 && glIsRenderbuffer(replaced) == GL_TRUE;
#ifndef WARBAND_BRIDGE_NO_LOGS
        bool reclaimed = false;
#endif
        if (depth_restore) {
#ifndef WARBAND_BRIDGE_NO_LOGS
            auto& leak = CurrentMemoryLeakTraceState();
            ++leak.depth_restore_calls;
            if (replaced_is_live) ++leak.depth_live_replacements;
#endif
            if (replaced_is_live && MemoryLeakFixesEnabled()) {
                glDeleteRenderbuffers(1, &replaced);
#ifndef WARBAND_BRIDGE_NO_LOGS
                reclaimed = true;
                ++leak.depth_reclaimed;
#endif
                if (GlResourceTraceEnabled()) {
                    auto& trace = CurrentGlResourceTraceState();
                    trace.renderbuffers.erase(replaced);
                    trace.renderbuffer_bytes.erase(replaced);
                    if (trace.bound_renderbuffer == replaced) {
                        trace.bound_renderbuffer = 0;
                    }
                    ++trace.deleted_renderbuffers;
                }
            }
        }
        glGenRenderbuffers(count, renderbuffers);
        if (GlResourceTraceEnabled() && renderbuffers) {
            auto& state = CurrentGlResourceTraceState();
            for (GLsizei i = 0; i < count; ++i) {
                state.renderbuffers.insert(renderbuffers[i]);
                ++state.generated_renderbuffers;
            }
            MaybeLogGlLifetime("gen_renderbuffers", jit.Regs()[14]);
        }
#ifndef WARBAND_BRIDGE_NO_LOGS
        if (depth_restore && MemoryLeakTraceEnabled()) {
            const auto& leak = CurrentMemoryLeakTraceState();
            LOGI("memory_leak_depth_rbo call=%llu old=%u old_live=%d reclaimed=%d "
                 "new=%u live_replacements=%llu total_reclaimed=%llu fixes=%d",
                 static_cast<unsigned long long>(leak.depth_restore_calls), replaced,
                 replaced_is_live ? 1 : 0, reclaimed ? 1 : 0, renderbuffers[0],
                 static_cast<unsigned long long>(leak.depth_live_replacements),
                 static_cast<unsigned long long>(leak.depth_reclaimed),
                 MemoryLeakFixesEnabled() ? 1 : 0);
        }
#endif
    });

    RegisterThunk("glGenTextures", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLsizei count = jit.Regs()[0];
        auto* textures = reinterpret_cast<GLuint*>(
            GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]));
        glGenTextures(count, textures);
        if (GlResourceTraceEnabled() && textures) {
            auto& state = CurrentGlResourceTraceState();
            for (GLsizei i = 0; i < count; ++i) {
                state.textures.insert(textures[i]);
                ++state.generated_textures;
            }
            MaybeLogGlLifetime("gen_textures", jit.Regs()[14]);
        }
    });

    RegisterThunk("glGenerateMipmap", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLenum target = static_cast<GLenum>(jit.Regs()[0]);
        if (!PostFxTraceEnabled() || target != GL_TEXTURE_2D) {
            glGenerateMipmap(target);
            return;
        }
        auto& state = CurrentGlCompatibilityState();
        GLint texture = 0;
        GLint min_filter = 0;
        GLint base_level = 0;
        GLint max_level = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
        glGetTexParameteriv(target, GL_TEXTURE_MIN_FILTER, &min_filter);
        glGetTexParameteriv(target, kGlTextureBaseLevel, &base_level);
        glGetTexParameteriv(target, kGlTextureMaxLevel, &max_level);
        const auto texture_it = state.textures.find(static_cast<GLuint>(texture));
        const TextureImageInfo* info =
            texture_it == state.textures.end() ? nullptr : &texture_it->second;
        const GLenum pre_error = glGetError();
        glGenerateMipmap(target);
        const GLenum error = glGetError();
        static thread_local uint32_t report_count = 0;
        if (report_count++ < 64) {
            LOGI("postfx_mipmap texture=%d size=%dx%d allocated=0x%04x "
                 "min_filter=0x%04x base_level=%d max_level=%d "
                 "pre_error=0x%04x error=0x%04x",
                 texture, info ? info->width : 0, info ? info->height : 0,
                 info ? info->allocated_internal_format : 0, min_filter,
                 base_level, max_level, pre_error, error);
        }
    });

    RegisterThunk("glGetActiveAttrib", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t sp = jit.Regs()[13];
        GLsizei* length = reinterpret_cast<GLsizei*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[3]));
        GLint* size = reinterpret_cast<GLint*>(GuestToHost(host.GetGuestMemoryBase(), host.MemoryRead32(sp)));
        GLenum* type = reinterpret_cast<GLenum*>(GuestToHost(host.GetGuestMemoryBase(), host.MemoryRead32(sp + 4)));
        GLchar* name = reinterpret_cast<GLchar*>(GuestToHost(host.GetGuestMemoryBase(), host.MemoryRead32(sp + 8)));
        glGetActiveAttrib(jit.Regs()[0], jit.Regs()[1], jit.Regs()[2], length, size, type, name);
    });

    RegisterThunk("glGetActiveUniform", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t sp = jit.Regs()[13];
        GLsizei* length = reinterpret_cast<GLsizei*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[3]));
        GLint* size = reinterpret_cast<GLint*>(GuestToHost(host.GetGuestMemoryBase(), host.MemoryRead32(sp)));
        GLenum* type = reinterpret_cast<GLenum*>(GuestToHost(host.GetGuestMemoryBase(), host.MemoryRead32(sp + 4)));
        GLchar* name = reinterpret_cast<GLchar*>(GuestToHost(host.GetGuestMemoryBase(), host.MemoryRead32(sp + 8)));
        glGetActiveUniform(jit.Regs()[0], jit.Regs()[1], jit.Regs()[2], length, size, type, name);
    });

    RegisterThunk("glGetAttribLocation", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        jit.Regs()[0] = glGetAttribLocation(jit.Regs()[0], reinterpret_cast<const GLchar*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1])));
    });

    RegisterThunk("glGetError", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        jit.Regs()[0] = glGetError();
    });

    RegisterThunk("glGetIntegerv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLenum name = static_cast<GLenum>(jit.Regs()[0]);
        const uint32_t output = jit.Regs()[1];
        if (UseGles2CompatibilityCapabilities()) {
            constexpr GLenum kGlMajorVersion = 0x821B;
            constexpr GLenum kGlMinorVersion = 0x821C;
            constexpr GLenum kGlNumExtensions = 0x821D;
            if (name == kGlMajorVersion) {
                host.MemoryWrite32(output, 2);
                return;
            }
            if (name == kGlMinorVersion) {
                host.MemoryWrite32(output, 0);
                return;
            }
            if (name == kGlNumExtensions) {
                host.MemoryWrite32(output, 1);
                return;
            }
            if (name == GL_NUM_COMPRESSED_TEXTURE_FORMATS) {
                host.MemoryWrite32(output, 1);
                return;
            }
            if (name == GL_COMPRESSED_TEXTURE_FORMATS) {
                host.MemoryWrite32(output, bridge_gl::kGlEtc1Rgb8Oes);
                return;
            }
        }
        glGetIntegerv(name, reinterpret_cast<GLint*>(
            GuestToHost(host.GetGuestMemoryBase(), output)));
    });

    RegisterThunk("glGetProgramInfoLog", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        GLsizei* length = jit.Regs()[2] ?
            reinterpret_cast<GLsizei*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[2])) : nullptr;
        glGetProgramInfoLog(jit.Regs()[0], jit.Regs()[1], length,
                           reinterpret_cast<GLchar*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[3])));
    });

    RegisterThunk("glGetProgramiv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glGetProgramiv(jit.Regs()[0], static_cast<GLenum>(jit.Regs()[1]),
                       reinterpret_cast<GLint*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[2])));
    });

    RegisterThunk("glGetRenderbufferParameteriv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glGetRenderbufferParameteriv(jit.Regs()[0], jit.Regs()[1],
                                     reinterpret_cast<GLint*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[2])));
    });

    RegisterThunk("glGetShaderInfoLog", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        GLsizei* length = jit.Regs()[2] ?
            reinterpret_cast<GLsizei*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[2])) : nullptr;
        glGetShaderInfoLog(jit.Regs()[0], jit.Regs()[1], length,
                          reinterpret_cast<GLchar*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[3])));
    });

    RegisterThunk("glGetShaderiv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glGetShaderiv(jit.Regs()[0], static_cast<GLenum>(jit.Regs()[1]),
                      reinterpret_cast<GLint*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[2])));
    });

    // GL promises a pointer that stays valid for the life of the context, and
    // callers keep it. One block per enum, so a driver-capability check inside a
    // frame loop costs nothing after the first frame - GL_EXTENSIONS alone is a
    // couple of KiB.
    RegisterThunk("glGetString", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        static std::mutex m;
        static std::unordered_map<GLenum, uint32_t> cache;
        static DynarmicHost* cached_host = nullptr;
        static EGLContext cached_context = EGL_NO_CONTEXT;
        GLenum name = static_cast<GLenum>(jit.Regs()[0]);
        std::lock_guard<std::mutex> lock(m);
        const EGLContext context = eglGetCurrentContext();
        if (cached_host != &host || cached_context != context) {
            // GL string lifetime and contents are context-scoped.  In
            // particular, never return an address allocated in an old guest
            // arena after the bridge or EGL context has been recreated.
            cache.clear();
            cached_host = &host;
            cached_context = context;
        }
        auto it = cache.find(name);
        if (it == cache.end()) {
            const GLubyte* str = nullptr;
            if (UseGles2CompatibilityCapabilities()) {
                switch (name) {
                    case GL_VERSION:
                        str = reinterpret_cast<const GLubyte*>(
                            "OpenGL ES 2.0 Warband diagnostic profile");
                        break;
                    case GL_SHADING_LANGUAGE_VERSION:
                        str = reinterpret_cast<const GLubyte*>(
                            "OpenGL ES GLSL ES 1.00");
                        break;
                    case GL_EXTENSIONS:
                        str = reinterpret_cast<const GLubyte*>(
                            "GL_OES_compressed_ETC1_RGB8_texture");
                        break;
                    default:
                        break;
                }
            }
            if (!str) str = glGetString(name);
            uint32_t guest_addr = 0;
            if (str) {
                size_t len = strlen(reinterpret_cast<const char*>(str)) + 1;
                guest_addr = host.GuestAlloc(len);
                if (guest_addr) memcpy(host.GetGuestMemoryBase() + guest_addr, str, len);
            }
            it = cache.emplace(name, guest_addr).first;
        }
        jit.Regs()[0] = it->second;
    });

    RegisterThunk("glGetUniformLocation", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        jit.Regs()[0] = glGetUniformLocation(jit.Regs()[0], reinterpret_cast<const GLchar*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1])));
    });

    RegisterThunk("glHint", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glHint(static_cast<GLenum>(jit.Regs()[0]), static_cast<GLenum>(jit.Regs()[1]));
    });

    RegisterThunk("glLineWidth", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glLineWidth(*reinterpret_cast<float*>(&jit.Regs()[0]));
    });

    RegisterThunk("glLinkProgram", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        GLuint prog = jit.Regs()[0];
        auto& compatibility = CurrentGlCompatibilityState();
        compatibility.postfx_programs.erase(prog);
        compatibility.map_water_programs.erase(prog);
        if (!ShaderTraceEnabled()) {
            glLinkProgram(prog);
        } else {
            ShaderTimingState& timing = CurrentShaderTimingState();
            const auto started = std::chrono::steady_clock::now();
            glLinkProgram(prog);
            const auto finished = std::chrono::steady_clock::now();
            timing.link_nanoseconds += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count());
            ++timing.link_calls;
            GLint linked = 0;
            glGetProgramiv(prog, GL_LINK_STATUS, &linked);
            if (!linked) {
                LogProgramLinkFailure(prog, timing);
            }
        }

        if (GuestImageOffset(jit.Regs()[14]) != kEffectLinkReturn) return;

        GLint attached_count = 0;
        glGetProgramiv(prog, GL_ATTACHED_SHADERS, &attached_count);
        if (attached_count < 0 || attached_count > 16) {
            LOGE("effect_link invalid attached shader count=%d program=%u",
                 attached_count, prog);
            return;
        }
        std::array<GLuint, 16> attached{};
        GLsizei returned_count = 0;
        if (attached_count != 0) {
            glGetAttachedShaders(prog, attached_count, &returned_count,
                                 attached.data());
        }
#ifndef WARBAND_BRIDGE_NO_LOGS
        auto& leak = CurrentMemoryLeakTraceState();
        ++leak.effect_link_calls;
        leak.shader_objects_seen += static_cast<uint64_t>(returned_count);
#endif
        if (MemoryLeakFixesEnabled()) {
            for (GLsizei index = 0; index < returned_count; ++index) {
                glDeleteShader(attached[static_cast<size_t>(index)]);
            }
#ifndef WARBAND_BRIDGE_NO_LOGS
            leak.shader_objects_delete_marked +=
                static_cast<uint64_t>(returned_count);
#endif
            if (GlResourceTraceEnabled()) {
                auto& trace = CurrentGlResourceTraceState();
                for (GLsizei index = 0; index < returned_count; ++index) {
                    trace.shaders.erase(attached[static_cast<size_t>(index)]);
                    ++trace.deleted_shaders;
                }
            }
        }
#ifndef WARBAND_BRIDGE_NO_LOGS
        if (MemoryLeakTraceEnabled() &&
            (leak.effect_link_calls == 1 || leak.effect_link_calls % 260u == 0)) {
            LOGI("memory_leak_shader_objects links=%llu attached_seen=%llu "
                 "delete_marked=%llu unowned=%llu fixes=%d",
                 static_cast<unsigned long long>(leak.effect_link_calls),
                 static_cast<unsigned long long>(leak.shader_objects_seen),
                 static_cast<unsigned long long>(
                     leak.shader_objects_delete_marked),
                 static_cast<unsigned long long>(
                     leak.shader_objects_seen - leak.shader_objects_delete_marked),
                 MemoryLeakFixesEnabled() ? 1 : 0);
        }
#endif
    });

    RegisterThunk("glPixelStorei", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glPixelStorei(static_cast<GLenum>(jit.Regs()[0]), jit.Regs()[1]);
    });

    RegisterThunk("glPolygonOffset", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glPolygonOffset(*reinterpret_cast<float*>(&jit.Regs()[0]), *reinterpret_cast<float*>(&jit.Regs()[1]));
    });

    RegisterThunk("glReadPixels", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t sp = jit.Regs()[13];
        GLenum format = host.MemoryRead32(sp);
        GLenum type = host.MemoryRead32(sp + 4);
        void* pixels = GuestToHost(host.GetGuestMemoryBase(), host.MemoryRead32(sp + 8));
        glReadPixels(jit.Regs()[0], jit.Regs()[1], jit.Regs()[2], jit.Regs()[3], format, type, pixels);
    });

    RegisterThunk("glRenderbufferStorage", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLenum target = jit.Regs()[0];
        const GLenum internal_format = jit.Regs()[1];
        const GLsizei width = jit.Regs()[2];
        const GLsizei height = jit.Regs()[3];
        glRenderbufferStorage(target, internal_format, width, height);
        if (GlResourceTraceEnabled() && target == GL_RENDERBUFFER) {
            auto& state = CurrentGlResourceTraceState();
            const GLuint renderbuffer = state.bound_renderbuffer;
            if (renderbuffer != 0) {
                uint64_t bytes_per_pixel = 4;
                if (internal_format == GL_DEPTH_COMPONENT16 || internal_format == GL_RGBA4 ||
                    internal_format == GL_RGB565 || internal_format == GL_RGB5_A1) {
                    bytes_per_pixel = 2;
                }
                const uint64_t bytes = width > 0 && height > 0 ?
                    static_cast<uint64_t>(width) * static_cast<uint64_t>(height) *
                        bytes_per_pixel : 0;
                state.renderbuffers.insert(renderbuffer);
                state.renderbuffer_bytes[renderbuffer] = bytes;
                MaybeLogGlStorage("renderbuffer", jit.Regs()[14], bytes,
                                  renderbuffer, width, height);
            }
        }
    });

    RegisterThunk("glScissor", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        GLint x = jit.Regs()[0], y = jit.Regs()[1];
        GLsizei w = jit.Regs()[2], h = jit.Regs()[3];
        ScaleViewportToSurface(x, y, w, h);
        glScissor(x, y, w, h);
    });

    RegisterThunk("glShaderSource", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        GLuint shader = jit.Regs()[0];
        GLsizei count = jit.Regs()[1];
        uint32_t str_array_gaddr = jit.Regs()[2];
        uint32_t len_array_gaddr = jit.Regs()[3];

        if (count <= 0 || count > 64) {
            LOGE("invalid glShaderSource count=%d shader=%u", count, shader);
            return;
        }

        std::vector<const char*> strings(count);
        std::vector<uint32_t> string_guest_addresses(count);
        for (GLsizei i = 0; i < count; ++i) {
            uint32_t str_gaddr = host.MemoryRead32(str_array_gaddr + i * 4);
            string_guest_addresses[static_cast<size_t>(i)] = str_gaddr;
            strings[i] = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), str_gaddr));
        }

        const GLint* lengths = len_array_gaddr ?
            reinterpret_cast<const GLint*>(GuestToHost(host.GetGuestMemoryBase(), len_array_gaddr)) : nullptr;

        std::string combined;
        for (GLsizei i = 0; i < count; ++i) {
            if (!strings[static_cast<size_t>(i)]) continue;
            const GLint length = lengths ? lengths[i] : -1;
            if (length >= 0) {
                combined.append(strings[static_cast<size_t>(i)],
                                static_cast<size_t>(length));
            } else {
                combined.append(strings[static_cast<size_t>(i)]);
            }
        }
        const bridge_gl::ShaderSourceFixResult fixed =
            bridge_gl::ApplyWarbandShaderSourceFixes(combined);
        if (fixed.changed()) {
            const GLchar* source = fixed.source.c_str();
            const GLint source_length = static_cast<GLint>(fixed.source.size());
            glShaderSource(shader, 1, &source, &source_length);
            LOGI("shader_source_fix shader=%u hdr_brightness=%d "
                 "finite_normal=%d finite_bloom=%d postfx_precision=%d "
                 "shadow_precision=%d "
                 "source_bytes=%d",
                 shader, fixed.hdr_brightness ? 1 : 0,
                 fixed.finite_normal ? 1 : 0,
                 fixed.finite_bloom ? 1 : 0,
                 fixed.postfx_precision ? 1 : 0,
                 fixed.shadow_precision ? 1 : 0, source_length);
        } else {
            glShaderSource(shader, count, strings.data(), lengths);
        }

        const uint32_t caller = GuestImageOffset(jit.Regs()[14]);
        const bool effect_source = caller == kVertexShaderSourceReturn ||
                                   caller == kFragmentShaderSourceReturn;
        if (effect_source) {
#ifndef WARBAND_BRIDGE_NO_LOGS
            auto& leak = CurrentMemoryLeakTraceState();
            for (auto it = leak.outstanding_shader_sources.begin();
                 it != leak.outstanding_shader_sources.end();) {
                if (host.GuestAllocSize(it->first) == 0) {
                    it = leak.outstanding_shader_sources.erase(it);
                } else {
                    ++it;
                }
            }

            uint64_t current_bytes = 0;
            uint64_t reclaimed_bytes = 0;
#endif
            for (uint32_t address : string_guest_addresses) {
                const uint32_t allocation_size = host.GuestAllocSize(address);
                if (allocation_size == 0) continue;
#ifndef WARBAND_BRIDGE_NO_LOGS
                current_bytes += allocation_size;
                ++leak.shader_source_calls;
#endif
                if (MemoryLeakFixesEnabled()) {
                    host.GuestFree(address);
#ifndef WARBAND_BRIDGE_NO_LOGS
                    reclaimed_bytes += allocation_size;
                    ++leak.shader_source_reclaimed;
                    leak.outstanding_shader_sources.erase(address);
#endif
#ifndef WARBAND_BRIDGE_NO_LOGS
                } else {
                    leak.outstanding_shader_sources[address] = allocation_size;
#endif
                }
            }
#ifndef WARBAND_BRIDGE_NO_LOGS
            if (MemoryLeakTraceEnabled()) {
                LOGI("memory_leak_glsl_source caller=0x%08x shader=%u blocks=%d "
                     "current_bytes=%llu reclaimed_bytes=%llu outstanding=%zu/%lluB "
                     "calls=%llu total_reclaimed=%llu fixes=%d",
                     caller, shader, count,
                     static_cast<unsigned long long>(current_bytes),
                     static_cast<unsigned long long>(reclaimed_bytes),
                     leak.outstanding_shader_sources.size(),
                     static_cast<unsigned long long>(OutstandingShaderSourceBytes(leak)),
                     static_cast<unsigned long long>(leak.shader_source_calls),
                     static_cast<unsigned long long>(leak.shader_source_reclaimed),
                     MemoryLeakFixesEnabled() ? 1 : 0);
            }
#endif
        }
    });

    RegisterThunk("glTexImage2D", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        GLenum target = jit.Regs()[0];
        GLint level = jit.Regs()[1];
        GLint internalformat = jit.Regs()[2];
        GLsizei width = jit.Regs()[3];
        uint32_t sp = jit.Regs()[13];
        GLsizei height = host.MemoryRead32(sp);
        GLint border = host.MemoryRead32(sp + 4);
        GLenum format = host.MemoryRead32(sp + 8);
        GLenum type = host.MemoryRead32(sp + 12);
        uint32_t pixels_gaddr = host.MemoryRead32(sp + 16);
        const void* pixels = (pixels_gaddr < 0x10000) ?
            reinterpret_cast<const void*>(static_cast<uintptr_t>(pixels_gaddr)) :
            GuestToHost(host.GetGuestMemoryBase(), pixels_gaddr);
        TrackTextureImage(target, level, internalformat, width, height, format, type, pixels);
        const bool trace = TextureTraceEnabled();
        const auto started = trace ? std::chrono::steady_clock::now()
                                   : std::chrono::steady_clock::time_point{};
        glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels);
        if (GlResourceTraceEnabled() && target == GL_TEXTURE_2D) {
            auto& state = CurrentGlResourceTraceState();
            const GLuint texture = CurrentTrackedTexture2D(state);
            TrackTextureStorage(texture, level,
                                ApproximateTextureBytes(width, height, format, type),
                                jit.Regs()[14], "texture", width, height);
        }
        auto& c = g_texture_uploads.plain_calls;
        const uint64_t total = c.fetch_add(1, std::memory_order_relaxed) + 1;
        GLsizei pixels_bytes = 0;
        if (trace && pixels && width > 0 && height > 0) {
            // Rough per-type size; the goal is only to distinguish a megabyte
            // scale RGBA expansion from a small render-target update.
            switch (type) {
                case GL_UNSIGNED_BYTE: pixels_bytes = width * height * 4; break;
                case GL_UNSIGNED_SHORT_5_6_5:
                case GL_UNSIGNED_SHORT_4_4_4_4:
                case GL_UNSIGNED_SHORT_5_5_5_1:
                    pixels_bytes = width * height * 2; break;
                default: pixels_bytes = width * height * 4; break;
            }
            g_texture_uploads.plain_bytes.fetch_add(static_cast<uint64_t>(pixels_bytes),
                                                    std::memory_order_relaxed);
            g_texture_uploads.plain_nanoseconds.fetch_add(
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now() - started).count()),
                std::memory_order_relaxed);
            if (total % 32 == 0) RecordTextureUploadLog();
        }
    });

    RegisterThunk("glTexParameterf", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glTexParameterf(jit.Regs()[0], jit.Regs()[1], *reinterpret_cast<float*>(&jit.Regs()[2]));
    });

    RegisterThunk("glTexParameteri", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLenum target = static_cast<GLenum>(jit.Regs()[0]);
        const GLenum pname = static_cast<GLenum>(jit.Regs()[1]);
        const GLint requested = static_cast<GLint>(jit.Regs()[2]);
        GLint effective = requested;
        if (target == GL_TEXTURE_2D && pname == GL_TEXTURE_MIN_FILTER &&
            bridge_gl::IsMipmapMinFilter(requested)) {
            GLint binding = 0;
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
            auto& state = CurrentGlCompatibilityState();
            const auto texture_it = state.textures.find(
                static_cast<GLuint>(binding));
            if (texture_it != state.textures.end()) {
                TextureImageInfo& info = texture_it->second;
                effective = bridge_gl::PortableRenderTargetMinFilter(
                    info.allocated_internal_format, info.render_target, requested);
                if (effective != requested && PostFxTraceEnabled() &&
                    !info.min_filter_override_reported) {
                    info.min_filter_override_reported = true;
                    LOGI("postfx_min_filter_fix texture=%d allocated=0x%04x "
                         "requested=0x%04x portable=0x%04x",
                         binding, info.allocated_internal_format, requested,
                         effective);
                }
            }
        }
        glTexParameteri(target, pname, effective);
    });

    RegisterThunk("glUniform1f", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLint location = static_cast<GLint>(jit.Regs()[0]);
        const GLfloat requested = *reinterpret_cast<float*>(&jit.Regs()[1]);
        glUniform1f(location, requested);
    });

    RegisterThunk("glUniform1i", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLint location = static_cast<GLint>(jit.Regs()[0]);
        const GLint requested_unit = static_cast<GLint>(jit.Regs()[1]);
        auto& state = CurrentGlCompatibilityState();
        const GLint map_water_unit = PortableMapWaterSamplerUnit(
            state, location, requested_unit);
        glUniform1i(location,
                    PortablePostFxSamplerUnit(location, map_water_unit));
    });

    RegisterThunk("glUniform2f", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glUniform2f(jit.Regs()[0], *reinterpret_cast<float*>(&jit.Regs()[1]), *reinterpret_cast<float*>(&jit.Regs()[2]));
    });

    RegisterThunk("glUniform3f", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glUniform3f(jit.Regs()[0], *reinterpret_cast<float*>(&jit.Regs()[1]),
                    *reinterpret_cast<float*>(&jit.Regs()[2]), *reinterpret_cast<float*>(&jit.Regs()[3]));
    });

    RegisterThunk("glUniform3fv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glUniform3fv(jit.Regs()[0], jit.Regs()[1],
                     reinterpret_cast<const GLfloat*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[2])));
    });

    RegisterThunk("glUniform4f", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t sp = jit.Regs()[13];
        uint32_t w_bits = host.MemoryRead32(sp);
        float w = *reinterpret_cast<float*>(&w_bits);
        glUniform4f(jit.Regs()[0], *reinterpret_cast<float*>(&jit.Regs()[1]),
                    *reinterpret_cast<float*>(&jit.Regs()[2]), *reinterpret_cast<float*>(&jit.Regs()[3]), w);
    });

    RegisterThunk("glUniform4fv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glUniform4fv(jit.Regs()[0], jit.Regs()[1],
                     reinterpret_cast<const GLfloat*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[2])));
    });

    RegisterThunk("glUniformMatrix3fv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glUniformMatrix3fv(jit.Regs()[0], jit.Regs()[1], jit.Regs()[2],
                           reinterpret_cast<const GLfloat*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[3])));
    });

    RegisterThunk("glUniformMatrix4fv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glUniformMatrix4fv(jit.Regs()[0], jit.Regs()[1], jit.Regs()[2],
                           reinterpret_cast<const GLfloat*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[3])));
    });

    RegisterThunk("glUseProgram", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        if (ShaderTraceEnabled()) {
            ShaderTimingState& timing = CurrentShaderTimingState();
            if (!timing.first_use_logged && timing.compile_calls != 0) {
                timing.first_use_logged = true;
                const double wall_ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - timing.batch_started).count();
                LOGI("shader_startup_summary context=%p compile_calls=%u link_calls=%u "
                     "driver_compile_ms=%.3f driver_link_ms=%.3f batch_wall_ms=%.3f",
                     timing.context, timing.compile_calls, timing.link_calls,
                     static_cast<double>(timing.compile_nanoseconds) / 1000000.0,
                     static_cast<double>(timing.link_nanoseconds) / 1000000.0,
                     wall_ms);
            }
        }
        TracePostFxUseProgram(jit, host);
        const GLuint program = jit.Regs()[0];
        glUseProgram(program);
        auto& state = CurrentGlCompatibilityState();
        state.bound_program = program;
        InitializePortablePostFxSamplerUnits(state, program);
        InitializePortableMapWaterSamplerUnits(state, program);
    });

    RegisterThunk("glVertexAttribPointer", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        GLuint index = jit.Regs()[0];
        GLint size = jit.Regs()[1];
        GLenum type = jit.Regs()[2];
        GLboolean normalized = jit.Regs()[3];
        uint32_t sp = jit.Regs()[13];
        GLsizei stride = host.MemoryRead32(sp);
        uint32_t ptr_gaddr = host.MemoryRead32(sp + 4);

        // Buffer bindings are EGL-context state. A process-global mirror becomes stale
        // across context switches/deletion and can reinterpret a client pointer as a VBO
        // offset (or vice versa), so ask the current context at the point of use.
        GLint bound_array_buffer = 0;
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &bound_array_buffer);
        const void* ptr = (bound_array_buffer != 0) ?
            reinterpret_cast<const void*>(static_cast<uintptr_t>(ptr_gaddr)) :
            GuestToHost(host.GetGuestMemoryBase(), ptr_gaddr);
        
        glVertexAttribPointer(index, size, type, normalized, stride, ptr);
    });

    RegisterThunk("glViewport", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        GLint x = jit.Regs()[0], y = jit.Regs()[1];
        GLsizei w = jit.Regs()[2], h = jit.Regs()[3];
        ScaleViewportToSurface(x, y, w, h);
        glViewport(x, y, w, h);
        auto& state = CurrentGlCompatibilityState();
        state.viewport[0] = x;
        state.viewport[1] = y;
        state.viewport[2] = w;
        state.viewport[3] = h;
    });

    // The rest of the GLES2 entry points libunity imports. These were all
    // landing on the default stub, which returns zero and draws nothing - a
    // stencilled UI element simply never appears, with no error anywhere.

    RegisterThunk("glFinish", [](Dynarmic::A32::Jit&, DynarmicHost&) { glFinish(); });
    RegisterThunk("glClearStencil", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        glClearStencil(static_cast<GLint>(jit.Regs()[0]));
    });
    // Softfp: the float arrives as its bit pattern in a core register.
    RegisterThunk("glClearDepthf", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        glClearDepthf(*reinterpret_cast<float*>(&jit.Regs()[0]));
    });
    RegisterThunk("glStencilFunc", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        glStencilFunc(static_cast<GLenum>(jit.Regs()[0]), static_cast<GLint>(jit.Regs()[1]),
                      jit.Regs()[2]);
    });
    RegisterThunk("glStencilFuncSeparate", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        glStencilFuncSeparate(static_cast<GLenum>(jit.Regs()[0]), static_cast<GLenum>(jit.Regs()[1]),
                              static_cast<GLint>(jit.Regs()[2]), jit.Regs()[3]);
    });
    RegisterThunk("glStencilMask", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        glStencilMask(jit.Regs()[0]);
    });
    RegisterThunk("glStencilOp", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        glStencilOp(static_cast<GLenum>(jit.Regs()[0]), static_cast<GLenum>(jit.Regs()[1]),
                    static_cast<GLenum>(jit.Regs()[2]));
    });
    RegisterThunk("glStencilOpSeparate", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        glStencilOpSeparate(static_cast<GLenum>(jit.Regs()[0]), static_cast<GLenum>(jit.Regs()[1]),
                            static_cast<GLenum>(jit.Regs()[2]), static_cast<GLenum>(jit.Regs()[3]));
    });
    RegisterThunk("glBlendEquationSeparate", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        glBlendEquationSeparate(static_cast<GLenum>(jit.Regs()[0]), static_cast<GLenum>(jit.Regs()[1]));
    });
    RegisterThunk("glBlendFuncSeparate", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        glBlendFuncSeparate(static_cast<GLenum>(jit.Regs()[0]), static_cast<GLenum>(jit.Regs()[1]),
                            static_cast<GLenum>(jit.Regs()[2]), static_cast<GLenum>(jit.Regs()[3]));
    });
    RegisterThunk("glBindAttribLocation", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* name = reinterpret_cast<const char*>(
            GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[2]));
        glBindAttribLocation(jit.Regs()[0], jit.Regs()[1], name);
    });
    RegisterThunk("glBufferSubData", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glBufferSubData(static_cast<GLenum>(jit.Regs()[0]), static_cast<GLintptr>(jit.Regs()[1]),
                        static_cast<GLsizeiptr>(jit.Regs()[2]),
                        GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[3]));
    });
    RegisterThunk("glTexSubImage2D", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t sp = jit.Regs()[13];
        glTexSubImage2D(static_cast<GLenum>(jit.Regs()[0]), static_cast<GLint>(jit.Regs()[1]),
                        static_cast<GLint>(jit.Regs()[2]), static_cast<GLint>(jit.Regs()[3]),
                        static_cast<GLsizei>(host.MemoryRead32(sp)),
                        static_cast<GLsizei>(host.MemoryRead32(sp + 4)),
                        static_cast<GLenum>(host.MemoryRead32(sp + 8)),
                        static_cast<GLenum>(host.MemoryRead32(sp + 12)),
                        GuestToHost(host.GetGuestMemoryBase(), host.MemoryRead32(sp + 16)));
    });
    RegisterThunk("glCompressedTexSubImage2D", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t sp = jit.Regs()[13];
        glCompressedTexSubImage2D(static_cast<GLenum>(jit.Regs()[0]), static_cast<GLint>(jit.Regs()[1]),
                                  static_cast<GLint>(jit.Regs()[2]), static_cast<GLint>(jit.Regs()[3]),
                                  static_cast<GLsizei>(host.MemoryRead32(sp)),
                                  static_cast<GLsizei>(host.MemoryRead32(sp + 4)),
                                  static_cast<GLenum>(host.MemoryRead32(sp + 8)),
                                  static_cast<GLsizei>(host.MemoryRead32(sp + 12)),
                                  GuestToHost(host.GetGuestMemoryBase(), host.MemoryRead32(sp + 16)));
    });
    RegisterThunk("glCopyTexSubImage2D", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t sp = jit.Regs()[13];
        glCopyTexSubImage2D(static_cast<GLenum>(jit.Regs()[0]), static_cast<GLint>(jit.Regs()[1]),
                            static_cast<GLint>(jit.Regs()[2]), static_cast<GLint>(jit.Regs()[3]),
                            static_cast<GLint>(host.MemoryRead32(sp)),
                            static_cast<GLint>(host.MemoryRead32(sp + 4)),
                            static_cast<GLsizei>(host.MemoryRead32(sp + 8)),
                            static_cast<GLsizei>(host.MemoryRead32(sp + 12)));
    });
    RegisterThunk("glGetFramebufferAttachmentParameteriv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        GLint value = 0;
        glGetFramebufferAttachmentParameteriv(static_cast<GLenum>(jit.Regs()[0]),
                                              static_cast<GLenum>(jit.Regs()[1]),
                                              static_cast<GLenum>(jit.Regs()[2]), &value);
        if (jit.Regs()[3]) host.MemoryWrite32(jit.Regs()[3], static_cast<uint32_t>(value));
    });

    // glUniform*v(location, count, value): `value` points at count*N words in
    // guest memory, which the arena makes directly addressable.
    auto uniform_v = [this](const char* name, void (*fn)(GLint, GLsizei, const void*)) {
        RegisterThunk(name, [fn](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
            fn(static_cast<GLint>(jit.Regs()[0]), static_cast<GLsizei>(jit.Regs()[1]),
               GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[2]));
        });
    };
    uniform_v("glUniform1fv", [](GLint l, GLsizei c, const void* v) {
        glUniform1fv(l, c, static_cast<const GLfloat*>(v)); });
    uniform_v("glUniform2fv", [](GLint l, GLsizei c, const void* v) {
        glUniform2fv(l, c, static_cast<const GLfloat*>(v)); });
    uniform_v("glUniform1iv", [](GLint l, GLsizei c, const void* v) {
        glUniform1iv(l, c, static_cast<const GLint*>(v)); });
    uniform_v("glUniform2iv", [](GLint l, GLsizei c, const void* v) {
        glUniform2iv(l, c, static_cast<const GLint*>(v)); });
    uniform_v("glUniform3iv", [](GLint l, GLsizei c, const void* v) {
        glUniform3iv(l, c, static_cast<const GLint*>(v)); });
    uniform_v("glUniform4iv", [](GLint l, GLsizei c, const void* v) {
        glUniform4iv(l, c, static_cast<const GLint*>(v)); });
}
