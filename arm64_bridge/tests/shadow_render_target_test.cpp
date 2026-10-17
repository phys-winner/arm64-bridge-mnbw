#include "../src/shadow_render_target.h"

using bridge_gl::GlesVersion;

static_assert(bridge_gl::ParseGlesVersion("OpenGL ES 2.0 V@1") == GlesVersion{2, 0});
static_assert(bridge_gl::ParseGlesVersion("OpenGL ES 3.2 build") == GlesVersion{3, 2});
static_assert(bridge_gl::ParseGlesVersion("OpenGL ES-CM 1.1") == GlesVersion{1, 1});
static_assert(bridge_gl::ParseGlesVersion("desktop GL") == GlesVersion{});

static_assert(bridge_gl::IsLegacySingleChannelColorFormat(GL_ALPHA, GL_ALPHA));
static_assert(bridge_gl::IsLegacySingleChannelColorFormat(GL_LUMINANCE, GL_LUMINANCE));
static_assert(bridge_gl::IsLegacySingleChannelColorFormat(
    GL_LUMINANCE_ALPHA, GL_LUMINANCE_ALPHA));
static_assert(!bridge_gl::IsLegacySingleChannelColorFormat(GL_RGBA, GL_RGBA));

static_assert(bridge_gl::PreferredShadowInternalFormat({2, 0}) == GL_RGBA);
static_assert(bridge_gl::PreferredShadowInternalFormat({3, 0}) == bridge_gl::kGlRgba8);
static_assert(std::string_view(
    bridge_gl::ShadowInternalFormatName(bridge_gl::kGlRgba16f)) == "GL_RGBA16F");
static_assert(bridge_gl::IsMipmapMinFilter(GL_NEAREST_MIPMAP_NEAREST));
static_assert(bridge_gl::IsMipmapMinFilter(GL_LINEAR_MIPMAP_NEAREST));
static_assert(bridge_gl::IsMipmapMinFilter(GL_NEAREST_MIPMAP_LINEAR));
static_assert(bridge_gl::IsMipmapMinFilter(GL_LINEAR_MIPMAP_LINEAR));
static_assert(!bridge_gl::IsMipmapMinFilter(GL_LINEAR));
static_assert(bridge_gl::PortableRenderTargetMinFilter(
                  bridge_gl::kGlRgba16f, true, GL_LINEAR_MIPMAP_LINEAR) == GL_LINEAR);
static_assert(bridge_gl::PortableRenderTargetMinFilter(
                  bridge_gl::kGlRg16f, true, GL_LINEAR_MIPMAP_NEAREST) == GL_LINEAR);
static_assert(bridge_gl::PortableRenderTargetMinFilter(
                  bridge_gl::kGlRgba8, true, GL_LINEAR_MIPMAP_LINEAR) ==
              GL_LINEAR);
static_assert(bridge_gl::PortableRenderTargetMinFilter(
                  GL_RGB, true, GL_NEAREST_MIPMAP_LINEAR) == GL_LINEAR);
static_assert(bridge_gl::PortableRenderTargetMinFilter(
                  GL_RGBA, true, GL_LINEAR) == GL_LINEAR);
static_assert(bridge_gl::PortableRenderTargetMinFilter(
                  bridge_gl::kGlRgba16f, false, GL_LINEAR_MIPMAP_LINEAR) ==
              GL_LINEAR_MIPMAP_LINEAR);

constexpr bool StatusLogDeduplicationWorks() {
    bridge_gl::ShadowFramebufferStatusSnapshot snapshot;
    return snapshot.Record(GL_RGBA, 0x8CD5) &&
           !snapshot.Record(GL_RGBA, 0x8CD5) &&
           snapshot.Record(bridge_gl::kGlRgba8, 0x8CD5) &&
           snapshot.Record(bridge_gl::kGlRgba8, 0x8CD6);
}

static_assert(StatusLogDeduplicationWorks());

int main() {
    return 0;
}
