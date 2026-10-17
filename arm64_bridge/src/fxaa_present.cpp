#include "fxaa_present.h"

#include <GLES3/gl3.h>
#include <android/log.h>
#include <sys/system_properties.h>

#include <atomic>
#include <cstring>

namespace bridge_gl {
namespace {

constexpr char kVertexShader[] = R"(
attribute vec2 aPosition;
varying highp vec2 vTex;
void main() {
    vTex = (aPosition + 1.0) * 0.5;
    gl_Position = vec4(aPosition, 0.0, 1.0);
}
)";

constexpr char kFragmentShader[] = R"(
precision highp float;
uniform sampler2D uScene;
uniform vec2 uInvSize;
varying highp vec2 vTex;

float luma(vec3 color) {
    return dot(color, vec3(0.299, 0.587, 0.114));
}

void main() {
    vec4 center = texture2D(uScene, vTex);
    vec3 nw = texture2D(uScene, vTex + vec2(-1.0, -1.0) * uInvSize).rgb;
    vec3 ne = texture2D(uScene, vTex + vec2( 1.0, -1.0) * uInvSize).rgb;
    vec3 sw = texture2D(uScene, vTex + vec2(-1.0,  1.0) * uInvSize).rgb;
    vec3 se = texture2D(uScene, vTex + vec2( 1.0,  1.0) * uInvSize).rgb;
    float lnw = luma(nw);
    float lne = luma(ne);
    float lsw = luma(sw);
    float lse = luma(se);
    float lm = luma(center.rgb);
    float low = min(lm, min(min(lnw, lne), min(lsw, lse)));
    float high = max(lm, max(max(lnw, lne), max(lsw, lse)));
    if (high - low < max(0.0312, high * 0.125)) {
        gl_FragColor = center;
        return;
    }

    vec2 direction = vec2(-((lnw + lne) - (lsw + lse)),
                            (lnw + lsw) - (lne + lse));
    float reduce = max((lnw + lne + lsw + lse) * 0.0078125, 0.0009765625);
    direction = clamp(direction / (min(abs(direction.x), abs(direction.y)) + reduce),
                      vec2(-8.0), vec2(8.0)) * uInvSize;
    vec3 a = 0.5 * (
        texture2D(uScene, vTex + direction * (-1.0 / 6.0)).rgb +
        texture2D(uScene, vTex + direction * ( 1.0 / 6.0)).rgb);
    vec3 b = a * 0.5 + 0.25 * (
        texture2D(uScene, vTex + direction * -0.5).rgb +
        texture2D(uScene, vTex + direction *  0.5).rgb);
    float lb = luma(b);
    gl_FragColor = vec4((lb < low || lb > high) ? a : b, center.a);
}
)";

struct FxaaResources {
    EGLContext context = EGL_NO_CONTEXT;
    GLuint program = 0;
    GLuint vertex_array = 0;
    GLuint vertex_buffer = 0;
    GLuint texture = 0;
    GLuint framebuffer = 0;
    GLint inverse_size_location = -1;
    GLint scene_location = -1;
    EGLint width = 0;
    EGLint height = 0;
    bool failed = false;
};

thread_local FxaaResources g_fxaa;
std::atomic<bool> g_fxaa_enabled{false};

struct SavedState {
    GLint program = 0;
    GLint vertex_array = 0;
    GLint array_buffer = 0;
    GLint draw_framebuffer = 0;
    GLint read_framebuffer = 0;
    GLint read_buffer = 0;
    GLint active_texture = 0;
    GLint texture0 = 0;
    GLint viewport[4] = {};
    GLboolean color_mask[4] = {};
    GLboolean scissor = GL_FALSE;
    GLboolean blend = GL_FALSE;
    GLboolean depth = GL_FALSE;
    GLboolean stencil = GL_FALSE;
    GLboolean cull = GL_FALSE;

    SavedState() {
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vertex_array);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &array_buffer);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_framebuffer);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_framebuffer);
        glGetIntegerv(GL_READ_BUFFER, &read_buffer);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active_texture);
        glGetIntegerv(GL_VIEWPORT, viewport);
        glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);
        scissor = glIsEnabled(GL_SCISSOR_TEST);
        blend = glIsEnabled(GL_BLEND);
        depth = glIsEnabled(GL_DEPTH_TEST);
        stencil = glIsEnabled(GL_STENCIL_TEST);
        cull = glIsEnabled(GL_CULL_FACE);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture0);
    }

    ~SavedState() {
        glUseProgram(static_cast<GLuint>(program));
        glBindVertexArray(static_cast<GLuint>(vertex_array));
        glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(array_buffer));
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER,
                          static_cast<GLuint>(draw_framebuffer));
        glBindFramebuffer(GL_READ_FRAMEBUFFER,
                          static_cast<GLuint>(read_framebuffer));
        glReadBuffer(static_cast<GLenum>(read_buffer));
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture0));
        glActiveTexture(static_cast<GLenum>(active_texture));
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        glColorMask(color_mask[0], color_mask[1], color_mask[2], color_mask[3]);
        Restore(GL_SCISSOR_TEST, scissor);
        Restore(GL_BLEND, blend);
        Restore(GL_DEPTH_TEST, depth);
        Restore(GL_STENCIL_TEST, stencil);
        Restore(GL_CULL_FACE, cull);
    }

    static void Restore(GLenum capability, GLboolean enabled) {
        if (enabled) glEnable(capability);
        else glDisable(capability);
    }
};

GLuint Compile(GLenum type, const char* source) {
    const GLuint shader = glCreateShader(type);
    if (!shader) return 0;
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_TRUE) return shader;
    glDeleteShader(shader);
    return 0;
}

bool Prepare(FxaaResources& fxaa) {
    const GLuint vertex = Compile(GL_VERTEX_SHADER, kVertexShader);
    const GLuint fragment = Compile(GL_FRAGMENT_SHADER, kFragmentShader);
    if (!vertex || !fragment) {
        if (vertex) glDeleteShader(vertex);
        if (fragment) glDeleteShader(fragment);
        return false;
    }
    fxaa.program = glCreateProgram();
    glAttachShader(fxaa.program, vertex);
    glAttachShader(fxaa.program, fragment);
    glBindAttribLocation(fxaa.program, 0, "aPosition");
    glLinkProgram(fxaa.program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint linked = GL_FALSE;
    glGetProgramiv(fxaa.program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) return false;
    fxaa.inverse_size_location = glGetUniformLocation(fxaa.program, "uInvSize");
    fxaa.scene_location = glGetUniformLocation(fxaa.program, "uScene");

    constexpr GLfloat quad[] = {-1.0f, -1.0f, 1.0f, -1.0f,
                                -1.0f,  1.0f, 1.0f,  1.0f};
    glGenVertexArrays(1, &fxaa.vertex_array);
    glBindVertexArray(fxaa.vertex_array);
    glGenBuffers(1, &fxaa.vertex_buffer);
    glBindBuffer(GL_ARRAY_BUFFER, fxaa.vertex_buffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glGenTextures(1, &fxaa.texture);
    glBindTexture(GL_TEXTURE_2D, fxaa.texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &fxaa.framebuffer);
    return fxaa.vertex_array && fxaa.vertex_buffer && fxaa.texture &&
           fxaa.framebuffer;
}

bool DisabledForDiagnostics() {
    char value[PROP_VALUE_MAX] = {};
    return __system_property_get("debug.warband.disable_fxaa", value) > 0 &&
           (std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0);
}

}  // namespace

void SetFxaaEnabled(bool enabled) {
    g_fxaa_enabled.store(enabled, std::memory_order_relaxed);
}

void ApplyFxaaBeforeSwap(EGLDisplay display, EGLSurface surface) {
    if (!g_fxaa_enabled.load(std::memory_order_relaxed) ||
        DisabledForDiagnostics()) return;
    const EGLContext context = eglGetCurrentContext();
    if (context == EGL_NO_CONTEXT) return;
    const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    if (!version || (!std::strstr(version, "OpenGL ES 3.") &&
                     !std::strstr(version, "OpenGL ES 4."))) return;
    GLint framebuffer = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
    if (framebuffer != 0) return;
    EGLint width = 0;
    EGLint height = 0;
    if (eglQuerySurface(display, surface, EGL_WIDTH, &width) != EGL_TRUE ||
        eglQuerySurface(display, surface, EGL_HEIGHT, &height) != EGL_TRUE ||
        width < 64 || height < 64) return;
    GLint max_size = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_size);
    if (width > max_size || height > max_size) return;

    FxaaResources& fxaa = g_fxaa;
    if (fxaa.context != context) {
        // A destroyed EGL context releases its GL objects. Its numeric names
        // cannot safely be deleted after a new context becomes current.
        fxaa = {};
        fxaa.context = context;
    }
    if (fxaa.failed) return;
    SavedState saved;
    if (!fxaa.program && !Prepare(fxaa)) {
        fxaa.failed = true;
        __android_log_print(ANDROID_LOG_ERROR, "CMR_FXAA",
                            "FXAA shader or resource initialization failed");
        return;
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, fxaa.texture);
    if (fxaa.width != width || fxaa.height != height) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        fxaa.width = width;
        fxaa.height = height;
    }
    // The window buffer can be multisampled. CopyTexSubImage2D rejects that
    // source on GLES, so resolve it into our single-sample texture by blitting.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fxaa.framebuffer);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, fxaa.texture, 0);
    if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fxaa.failed = true;
        return;
    }
    glBlitFramebuffer(0, 0, width, height, 0, 0, width, height,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, width, height);
    glUseProgram(fxaa.program);
    glUniform1i(fxaa.scene_location, 0);
    glUniform2f(fxaa.inverse_size_location,
                1.0f / static_cast<float>(width),
                1.0f / static_cast<float>(height));
    glBindVertexArray(fxaa.vertex_array);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

}  // namespace bridge_gl
