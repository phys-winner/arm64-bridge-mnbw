#include "../src/renderer_preferences.h"
#include "../src/shader_source_fixes.h"

#include <cassert>
#include <string>

int main() {
    const std::string character_shadow =
        "precision lowp samplerCube;\r\n"
        "precision lowp sampler2D;\r\n"
        "precision mediump int;\r\n"
        "precision highp   float;\r\n"
        "void main ()\r\n"
        "{\r\n"
        "  gl_FragColor = vec4(1.0, 1.0, 1.0, 1.0);\r\n"
        "}\r\n"
        "\r\n";
    const auto shadow = bridge_gl::ApplyWarbandShaderSourceFixes(character_shadow);
    // These opaque silhouette passes intentionally do not sample diffuse alpha.
    // Character textures use alpha for non-opacity material data, so injecting a
    // sampler here stamps faces into the projected shadow and onto the sky.
    assert(!shadow.changed());
    assert(!shadow.hdr_brightness);
    assert(!shadow.finite_normal);
    assert(!shadow.finite_bloom);
    assert(!shadow.postfx_precision);
    assert(shadow.source == character_shadow);

    const std::string hdr =
        "precision highp float;\n"
        "uniform sampler2D postFX_sampler0;\n"
        "uniform sampler2D postFX_sampler1;\n"
        "void main() { lowp vec4 blur_2; lowp vec4 color_3; "
        "blur_2.xyz = (tmpvar_9.xyz * 16.0); "
        "color_3.xyz = sqrt((vec3(1.0))); "
        "gl_FragColor = color_3; }\n";
    const auto exposure = bridge_gl::ApplyWarbandShaderSourceFixes(hdr);
    assert(exposure.changed());
    assert(exposure.hdr_brightness);
    assert(exposure.hdr_tone_map);
    assert(exposure.postfx_precision);
    assert(exposure.source.find("uniform highp float mb_HDRBrightness;") !=
           std::string::npos);
    assert(exposure.source.find("sqrt((max((mb_HDRBrightness * ") != std::string::npos);
    assert(exposure.source.find("vec3(1.0) + max((mb_HDRBrightness *") !=
           std::string::npos);
    assert(exposure.source.find("blur_2.xyz = (tmpvar_9.xyz * 16.0);") !=
           std::string::npos);
    assert(exposure.source.find("highp vec4 color_3") != std::string::npos);
    assert(exposure.source.find("void main() { lowp") == std::string::npos);

    const std::string water =
        "precision highp float;\n"
        "void main() { lowp vec3 normal_1; "
        "normal_1.z = sqrt((1.0 - dot (normal_1.xy, normal_1.xy))); }\n";
    const auto finite_normal = bridge_gl::ApplyWarbandShaderSourceFixes(water);
    assert(finite_normal.changed());
    assert(finite_normal.finite_normal);
    assert(finite_normal.source.find(
        "sqrt(max(0.0, 1.0 - dot(normal_1.xy, normal_1.xy)))") !=
        std::string::npos);

    const std::string bright_pass =
        "precision highp float;\n"
        "uniform sampler2D postFX_sampler0;\n"
        "void main() { lowp vec3 color_1; lowp float tmpvar_2; "
        "color_1 = (color_1 * (tmpvar_2 / tmpvar_2)); "
        "gl_FragColor = vec4(color_1, 1.0); }\n";
    const auto finite_bloom =
        bridge_gl::ApplyWarbandShaderSourceFixes(bright_pass);
    assert(finite_bloom.changed());
    assert(finite_bloom.finite_bloom);
    assert(finite_bloom.postfx_precision);
    assert(finite_bloom.source.find("tmpvar_2 / tmpvar_2") ==
           std::string::npos);
    assert(finite_bloom.source.find(
        "((tmpvar_2 > 0.0) ? color_1 : vec3(0.0))") !=
        std::string::npos);

    const auto unrelated = bridge_gl::ApplyWarbandShaderSourceFixes(
        "void main() { gl_FragColor = vec4(1.0); }");
    assert(!unrelated.changed());

    assert(bridge_gl::BoundHdrBrightness(0.1f) == 0.5f);
    assert(bridge_gl::BoundHdrBrightness(2.0f) == 1.25f);
    bridge_gl::SetHdrBrightness(0.8f);
    assert(bridge_gl::HdrBrightness() == 0.8f);
    return 0;
}
