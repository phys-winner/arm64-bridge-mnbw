#pragma once

#if __has_include(<GLES2/gl2.h>)
#include <GLES2/gl2.h>
#else
using GLenum = unsigned int;
using GLint = int;
using GLsizei = int;
inline constexpr GLenum GL_ALPHA = 0x1906;
inline constexpr GLenum GL_RGB = 0x1907;
inline constexpr GLenum GL_RGBA = 0x1908;
inline constexpr GLenum GL_LUMINANCE = 0x1909;
inline constexpr GLenum GL_LUMINANCE_ALPHA = 0x190A;
inline constexpr GLenum GL_LINEAR = 0x2601;
inline constexpr GLenum GL_NEAREST_MIPMAP_NEAREST = 0x2700;
inline constexpr GLenum GL_LINEAR_MIPMAP_NEAREST = 0x2701;
inline constexpr GLenum GL_NEAREST_MIPMAP_LINEAR = 0x2702;
inline constexpr GLenum GL_LINEAR_MIPMAP_LINEAR = 0x2703;
#endif

#include <cstddef>
#include <string_view>

namespace bridge_gl {

// GL_RGBA8 is core in GLES 3.0, but the bridge intentionally compiles against
// the GLES 2.0 headers so the same binary keeps an unsized GLES2 fallback.
constexpr GLenum kGlRgba8 = 0x8058;
constexpr GLenum kGlRgba16f = 0x881A;
constexpr GLenum kGlRg16f = 0x822F;
constexpr GLenum kGlR16f = 0x822D;
constexpr GLenum kGlR32f = 0x822E;

struct GlesVersion {
    int major = 0;
    int minor = 0;

    constexpr bool operator==(const GlesVersion&) const noexcept = default;
};

struct ShadowFramebufferStatusSnapshot {
    bool initialized = false;
    GLenum format = 0;
    GLenum status = 0;

    constexpr bool Record(GLenum new_format, GLenum new_status) noexcept {
        const bool changed = !initialized || format != new_format || status != new_status;
        initialized = true;
        format = new_format;
        status = new_status;
        return changed;
    }
};

constexpr GlesVersion ParseGlesVersion(std::string_view text) noexcept {
    constexpr std::string_view prefix = "OpenGL ES";
    const std::size_t prefix_at = text.find(prefix);
    if (prefix_at == std::string_view::npos) return {};

    std::size_t cursor = prefix_at + prefix.size();
    while (cursor < text.size() && (text[cursor] == ' ' || text[cursor] == '-')) {
        ++cursor;
    }
    // Accept vendor strings such as "OpenGL ES-CM 1.1".
    while (cursor < text.size() && (text[cursor] < '0' || text[cursor] > '9')) {
        ++cursor;
    }
    if (cursor == text.size()) return {};

    int major = 0;
    while (cursor < text.size() && text[cursor] >= '0' && text[cursor] <= '9') {
        major = major * 10 + (text[cursor++] - '0');
    }
    int minor = 0;
    if (cursor < text.size() && text[cursor] == '.') {
        ++cursor;
        while (cursor < text.size() && text[cursor] >= '0' && text[cursor] <= '9') {
            minor = minor * 10 + (text[cursor++] - '0');
        }
    }
    return {major, minor};
}

constexpr bool IsLegacySingleChannelColorFormat(GLint internal_format,
                                                 GLenum format) noexcept {
    return internal_format == GL_ALPHA || internal_format == GL_LUMINANCE ||
           internal_format == GL_LUMINANCE_ALPHA || format == GL_ALPHA ||
           format == GL_LUMINANCE || format == GL_LUMINANCE_ALPHA;
}

constexpr GLenum PreferredShadowInternalFormat(GlesVersion version) noexcept {
    return version.major >= 3 ? kGlRgba8 : GL_RGBA;
}

constexpr bool IsMipmapMinFilter(GLint filter) noexcept {
    return filter == static_cast<GLint>(GL_NEAREST_MIPMAP_NEAREST) ||
           filter == static_cast<GLint>(GL_LINEAR_MIPMAP_NEAREST) ||
           filter == static_cast<GLint>(GL_NEAREST_MIPMAP_LINEAR) ||
           filter == static_cast<GLint>(GL_LINEAR_MIPMAP_LINEAR);
}

constexpr bool IsFloatingPointRenderTargetFormat(GLenum format) noexcept {
    return format == kGlRgba16f || format == kGlRg16f ||
           format == kGlR16f || format == kGlR32f;
}

constexpr GLint PortableRenderTargetMinFilter(GLenum internal_format,
                                               bool render_target,
                                               GLint requested_filter) noexcept {
    (void)internal_format;
    // Warband allocates several transient post-FX targets with level zero only.
    // The GLES default minification filter requires a complete mip chain, so an
    // otherwise complete LDR RGB/RGBA framebuffer can sample as black on strict
    // drivers (notably Adreno) just like the floating-point HDR targets.  These
    // attachments are explicitly downsampled into separate textures; they are
    // never meant to consume mip levels from the attachment itself.
    return render_target && IsMipmapMinFilter(requested_filter)
               ? static_cast<GLint>(GL_LINEAR)
               : requested_filter;
}

constexpr const char* ShadowInternalFormatName(GLenum format) noexcept {
    return format == kGlRgba8 ? "GL_RGBA8" :
           format == kGlRgba16f ? "GL_RGBA16F" :
           format == kGlRg16f ? "GL_RG16F" :
           format == kGlR16f ? "GL_R16F" :
           format == kGlR32f ? "GL_R32F" :
           format == GL_RGBA ? "GL_RGBA" : "unknown";
}

}  // namespace bridge_gl
