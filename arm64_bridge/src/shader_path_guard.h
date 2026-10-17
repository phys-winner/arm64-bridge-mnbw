#pragma once

#include <cstddef>
#include <string_view>

namespace warband::shader_path_guard {

// rglOpenGL_effect builds both optimized shader filenames in 256-byte stack
// arrays as:
//
//   <root> + "GLShadersOptimized/" + ("vs_" | "fs_") + <technique> + ".glsl"
//
// The fixed pieces and the terminating NUL consume 28 bytes, leaving 228 for
// root + technique.  The original ARM code uses unbounded strcpy/strcat.
inline constexpr std::size_t kGuestPathCapacity = 256u;
inline constexpr std::size_t kOptimizedPrefixBytes = 22u;
inline constexpr std::size_t kGlslSuffixAndNulBytes = 6u;
inline constexpr std::size_t kMaxRootAndTechniqueBytes =
    kGuestPathCapacity - kOptimizedPrefixBytes - kGlslSuffixAndNulBytes;
inline constexpr std::string_view kFallbackTechnique = "notexture";

struct Selection {
    std::string_view name;
    bool used_fallback = false;
};

inline bool IsSafeTechniqueName(std::string_view name) {
    if (name.empty()) return false;
    for (const unsigned char character : name) {
        const bool ascii_letter =
            (character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z');
        const bool ascii_digit = character >= '0' && character <= '9';
        if (!ascii_letter && !ascii_digit && character != '_') return false;
    }
    return true;
}

inline Selection SelectTechnique(std::size_t root_bytes,
                                 std::string_view requested) {
    if (root_bytes <= kMaxRootAndTechniqueBytes &&
        requested.size() <= kMaxRootAndTechniqueBytes - root_bytes &&
        IsSafeTechniqueName(requested)) {
        return {requested, false};
    }
    return {kFallbackTechnique, true};
}

}  // namespace warband::shader_path_guard
