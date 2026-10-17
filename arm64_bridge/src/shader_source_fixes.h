#pragma once

#include <string>
#include <string_view>

namespace bridge_gl {

struct ShaderSourceFixResult {
    std::string source;
    bool hdr_brightness = false;
    bool hdr_tone_map = false;
    bool finite_normal = false;
    bool finite_bloom = false;
    bool postfx_precision = false;
    bool shadow_precision = false;

    bool changed() const noexcept {
        return hdr_brightness || hdr_tone_map || finite_normal || finite_bloom ||
               postfx_precision || shadow_precision;
    }
};

inline bool ReplaceFirst(std::string& text, std::string_view needle,
                         std::string_view replacement) {
    const std::size_t at = text.find(needle);
    if (at == std::string::npos) return false;
    text.replace(at, needle.size(), replacement);
    return true;
}

inline bool ReplaceAllAfter(std::string& text, std::size_t begin,
                            std::string_view needle,
                            std::string_view replacement) {
    bool changed = false;
    std::size_t at = begin;
    while ((at = text.find(needle, at)) != std::string::npos) {
        text.replace(at, needle.size(), replacement);
        at += replacement.size();
        changed = true;
    }
    return changed;
}

inline bool ApplyHdrHighlightCompression(std::string& source) {
    constexpr std::string_view kHdrOutput = "color_3.xyz = sqrt(";
    const std::size_t output = source.find(kHdrOutput);
    if (output == std::string::npos) return false;

    const std::size_t open = output + kHdrOutput.size() - 1;
    if (open >= source.size() || source[open] != '(') return false;
    std::size_t depth = 0;
    std::size_t close = std::string::npos;
    for (std::size_t at = open; at < source.size(); ++at) {
        if (source[at] == '(') {
            ++depth;
        } else if (source[at] == ')' && --depth == 0) {
            close = at;
            break;
        }
    }
    if (close == std::string::npos || close <= open + 1) return false;

    const std::string linear = source.substr(open + 1, close - open - 1);
    const std::string nonnegative = "max((" + linear + "), vec3(0.0))";
    const std::string mapped = "(" + nonnegative + " / (vec3(1.0) + " +
                               nonnegative + "))";
    source.replace(open + 1, close - open - 1, mapped);
    return true;
}

inline bool PromoteOptimizedShadowPrecision(std::string& source) {
    bool changed = false;
    if ((source.find("varying highp float outDepth;") != std::string::npos ||
         source.find("varying float outDepth;") != std::string::npos) &&
        source.find("finalColor_1 = vec4(outDepth);") != std::string::npos) {
        changed |= ReplaceFirst(source, "lowp vec4 finalColor_1;",
                                "highp vec4 finalColor_1;");
    }
    if (source.find("uniform sampler2D shadowmap_texture;") ==
            std::string::npos) {
        return changed;
    }
    changed |= ReplaceFirst(source, "uniform sampler2D shadowmap_texture;",
                            "uniform highp sampler2D shadowmap_texture;");
    constexpr std::string_view sample = " = texture2D (shadowmap_texture,";
    std::size_t search = 0;
    while (true) {
        const std::size_t use = source.find(sample, search);
        if (use == std::string::npos) break;
        const std::size_t line = source.rfind('\n', use);
        const std::size_t variable = source.find(
            "tmpvar_", line == std::string::npos ? 0 : line + 1);
        if (variable == std::string::npos || variable >= use) break;
        const std::string name = source.substr(variable, use - variable);
        changed |= ReplaceFirst(source, "lowp vec4 " + name + ";",
                                "highp vec4 " + name + ";");
        search = use + sample.size();
    }
    return changed;
}

inline ShaderSourceFixResult ApplyWarbandShaderSourceFixes(
        std::string_view original) {
    ShaderSourceFixResult result{std::string(original)};

    // Normal maps are filtered/compressed data. Their reconstructed x/y pair
    // may therefore be a fraction longer than one. RGBA8 used to clamp the
    // resulting NaN on old renderers, while an HDR float target preserves it
    // and lets it poison bloom and the final square root. Clamp the radicand at
    // the source; this also covers equivalent shaders supplied by modules.
    constexpr std::string_view kNormal1 =
        "normal_1.z = sqrt((1.0 - dot (normal_1.xy, normal_1.xy)));";
    constexpr std::string_view kNormal2 =
        "normal_2.z = sqrt((1.0 - dot (normal_2.xy, normal_2.xy)));";
    constexpr std::string_view kNormal3 =
        "normal_3.z = sqrt((1.0 - dot (normal_3.xy, normal_3.xy)));";
    constexpr std::string_view kSafeNormal1 =
        "normal_1.z = sqrt(max(0.0, 1.0 - dot(normal_1.xy, normal_1.xy)));";
    constexpr std::string_view kSafeNormal2 =
        "normal_2.z = sqrt(max(0.0, 1.0 - dot(normal_2.xy, normal_2.xy)));";
    constexpr std::string_view kSafeNormal3 =
        "normal_3.z = sqrt(max(0.0, 1.0 - dot(normal_3.xy, normal_3.xy)));";
    const bool fixed_normal1 =
        ReplaceFirst(result.source, kNormal1, kSafeNormal1);
    const bool fixed_normal2 =
        ReplaceFirst(result.source, kNormal2, kSafeNormal2);
    const bool fixed_normal3 =
        ReplaceFirst(result.source, kNormal3, kSafeNormal3);
    result.finite_normal = fixed_normal1 || fixed_normal2 || fixed_normal3;

    // The optimized auto-exposure bright pass contains luminance/luminance.
    // It is an identity for every positive value, but becomes 0/0 on black
    // pixels and contaminates the HDR chain with NaNs. Keep the intended
    // identity and explicitly preserve zero instead.
    constexpr std::string_view kUnsafeBloom =
        "color_1 = (color_1 * (tmpvar_2 / tmpvar_2));";
    constexpr std::string_view kSafeBloom =
        "color_1 = ((tmpvar_2 > 0.0) ? color_1 : vec3(0.0));";
    result.finite_bloom =
        ReplaceFirst(result.source, kUnsafeBloom, kSafeBloom);

    // Generated PostFX shaders explicitly downgrade all temporaries to lowp,
    // including 16-tap luminance sums and HDR values. Promote only executable
    // declarations (not sampler defaults) so GLES2 implementations with a
    // narrow lowp range cannot overflow or quantize the float render targets.
    if (result.source.find("postFX_sampler") != std::string::npos) {
        const std::size_t main_at = result.source.find("void main");
        if (main_at != std::string::npos) {
            result.postfx_precision = ReplaceAllAfter(
                result.source, main_at, "lowp ", "highp ");
        }
    }

    // The optimized depth writer and four PCF readers use lowp temporaries
    // even though the shadow target is RGBA16F. Keep depth samples highp.
    result.shadow_precision = PromoteOptimizedShadowPrecision(result.source);

    // All optimized HDR final variants linearize the scene and finish with
    // this sqrt.  Multiplying inside sqrt changes exposure before output gamma,
    // retaining highlight detail instead of merely dimming already-clipped RGB.
    constexpr std::string_view kHdrOutput = "color_3.xyz = sqrt(";
    if (result.source.find("uniform sampler2D postFX_sampler0;") !=
            std::string::npos &&
        result.source.find("uniform sampler2D postFX_sampler1;") !=
            std::string::npos &&
        result.source.find(kHdrOutput) != std::string::npos &&
        result.source.find("gl_FragColor = color_3;") != std::string::npos) {
        const std::size_t main_at = result.source.find("void main");
        if (main_at != std::string::npos) {
            result.source.insert(main_at,
                                 "uniform highp float mb_HDRBrightness;\n");
            if (ReplaceFirst(result.source, kHdrOutput,
                             "color_3.xyz = sqrt(mb_HDRBrightness * ")) {
                result.hdr_brightness = true;
                result.hdr_tone_map =
                    ApplyHdrHighlightCompression(result.source);
            }
        }
    }

    return result;
}

}  // namespace bridge_gl
