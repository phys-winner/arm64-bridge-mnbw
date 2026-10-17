#pragma once

#if __has_include(<GLES2/gl2.h>)
#include <GLES2/gl2.h>
#else
using GLenum = unsigned int;
using GLint = int;
inline constexpr GLenum GL_DEPTH_RANGE = 0x0B70;
inline constexpr GLenum GL_VIEWPORT = 0x0BA2;
inline constexpr GLenum GL_COLOR_CLEAR_VALUE = 0x0C22;
inline constexpr GLenum GL_SCISSOR_BOX = 0x0C10;
inline constexpr GLenum GL_MAX_VIEWPORT_DIMS = 0x0D3A;
inline constexpr GLenum GL_COLOR_WRITEMASK = 0x0C23;
inline constexpr GLenum GL_BLEND_COLOR = 0x8005;
inline constexpr GLenum GL_ALIASED_POINT_SIZE_RANGE = 0x846D;
inline constexpr GLenum GL_ALIASED_LINE_WIDTH_RANGE = 0x846E;
inline constexpr GLenum GL_NUM_COMPRESSED_TEXTURE_FORMATS = 0x86A2;
inline constexpr GLenum GL_COMPRESSED_TEXTURE_FORMATS = 0x86A3;
inline constexpr GLenum GL_SHADER_BINARY_FORMATS = 0x8DF8;
inline constexpr GLenum GL_NUM_SHADER_BINARY_FORMATS = 0x8DF9;
inline void glGetIntegerv(GLenum, GLint*) {}
#endif

#include <cstddef>

namespace bridge_gl {

// glGetFloatv writes a pname-dependent number of values. A zero result is
// reserved for the two variable-length GLES2 state arrays.
constexpr std::size_t FixedFloatStateValueCount(GLenum pname) noexcept {
    switch (pname) {
        case GL_COMPRESSED_TEXTURE_FORMATS:
        case GL_SHADER_BINARY_FORMATS:
            return 0;
        case GL_ALIASED_LINE_WIDTH_RANGE:
        case GL_ALIASED_POINT_SIZE_RANGE:
        case GL_DEPTH_RANGE:
        case GL_MAX_VIEWPORT_DIMS:
            return 2;
        case GL_BLEND_COLOR:
        case GL_COLOR_CLEAR_VALUE:
        case GL_COLOR_WRITEMASK:
        case GL_SCISSOR_BOX:
        case GL_VIEWPORT:
            return 4;
        default:
            return 1;
    }
}

template <typename GetInteger>
constexpr std::size_t FloatStateValueCount(GLenum pname, GetInteger get_integer) {
    const std::size_t fixed_count = FixedFloatStateValueCount(pname);
    if (fixed_count != 0) return fixed_count;

    const GLenum count_pname = pname == GL_COMPRESSED_TEXTURE_FORMATS
        ? GL_NUM_COMPRESSED_TEXTURE_FORMATS
        : GL_NUM_SHADER_BINARY_FORMATS;
    GLint count = 0;
    get_integer(count_pname, &count);
    return count > 0 ? static_cast<std::size_t>(count) : 0;
}

inline std::size_t FloatStateValueCount(GLenum pname) {
    return FloatStateValueCount(pname, [](GLenum count_pname, GLint* count) {
        glGetIntegerv(count_pname, count);
    });
}

}  // namespace bridge_gl
