#include "../src/gl_float_state_count.h"

namespace {

constexpr GLenum kMaxTextureLodBias = 0x84FD;
constexpr GLenum kGuestMaxTextureAnisotropy = 0x84FF;

constexpr auto NoDynamicQuery = [](GLenum, GLint*) {};

constexpr auto PositiveDynamicCounts = [](GLenum pname, GLint* count) {
    *count = pname == GL_NUM_COMPRESSED_TEXTURE_FORMATS ? 7 : 3;
};

constexpr auto InvalidDynamicCounts = [](GLenum, GLint* count) {
    *count = -1;
};

constexpr auto NoDynamicFormats = [](GLenum, GLint* count) {
    *count = 0;
};

static_assert(bridge_gl::FloatStateValueCount(kMaxTextureLodBias, NoDynamicQuery) == 1);
static_assert(bridge_gl::FloatStateValueCount(kGuestMaxTextureAnisotropy, NoDynamicQuery) == 1);
static_assert(bridge_gl::FloatStateValueCount(GL_DEPTH_RANGE, NoDynamicQuery) == 2);
static_assert(bridge_gl::FloatStateValueCount(GL_VIEWPORT, NoDynamicQuery) == 4);
static_assert(bridge_gl::FloatStateValueCount(
                  GL_COMPRESSED_TEXTURE_FORMATS, PositiveDynamicCounts) == 7);
static_assert(bridge_gl::FloatStateValueCount(
                  GL_SHADER_BINARY_FORMATS, PositiveDynamicCounts) == 3);
static_assert(bridge_gl::FloatStateValueCount(
                  GL_COMPRESSED_TEXTURE_FORMATS, InvalidDynamicCounts) == 0);
static_assert(bridge_gl::FloatStateValueCount(
                  GL_SHADER_BINARY_FORMATS, NoDynamicFormats) == 0);

}  // namespace

int main() {
    return 0;
}
