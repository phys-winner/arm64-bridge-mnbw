#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#include "shadow_render_target.h"

namespace bridge_gl {

constexpr GLenum kGlEtc1Rgb8Oes = 0x8D64;
constexpr GLenum kGlCompressedR11Eac = 0x9270;
constexpr GLenum kGlCompressedSignedR11Eac = 0x9271;
constexpr GLenum kGlCompressedRg11Eac = 0x9272;
constexpr GLenum kGlCompressedSignedRg11Eac = 0x9273;
constexpr GLenum kGlCompressedRgb8Etc2 = 0x9274;
constexpr GLenum kGlCompressedSrgb8Etc2 = 0x9275;
constexpr GLenum kGlCompressedRgb8PunchthroughAlpha1Etc2 = 0x9276;
constexpr GLenum kGlCompressedSrgb8PunchthroughAlpha1Etc2 = 0x9277;
constexpr GLenum kGlCompressedRgba8Etc2Eac = 0x9278;
constexpr GLenum kGlCompressedSrgb8Alpha8Etc2Eac = 0x9279;
constexpr GLenum kGlCompressedRgbaAstc4x4Khr = 0x93B0;
constexpr GLenum kGlCompressedRgbaAstc12x12Khr = 0x93BD;
constexpr GLenum kGlCompressedSrgb8Alpha8Astc4x4Khr = 0x93D0;
constexpr GLenum kGlCompressedSrgb8Alpha8Astc12x12Khr = 0x93DD;

struct BlockCompressionInfo {
    int block_width = 0;
    int block_height = 0;
    int block_bytes = 0;

    constexpr explicit operator bool() const noexcept {
        return block_width > 0 && block_height > 0 && block_bytes > 0;
    }
};

constexpr std::array<BlockCompressionInfo, 14> kAstcBlocks = {{
    {4, 4, 16}, {5, 4, 16}, {5, 5, 16}, {6, 5, 16}, {6, 6, 16},
    {8, 5, 16}, {8, 6, 16}, {8, 8, 16}, {10, 5, 16}, {10, 6, 16},
    {10, 8, 16}, {10, 10, 16}, {12, 10, 16}, {12, 12, 16},
}};

constexpr BlockCompressionInfo CompressedBlockInfo(GLenum format) noexcept {
    if (format >= kGlCompressedRgbaAstc4x4Khr &&
        format <= kGlCompressedRgbaAstc12x12Khr) {
        return kAstcBlocks[format - kGlCompressedRgbaAstc4x4Khr];
    }
    if (format >= kGlCompressedSrgb8Alpha8Astc4x4Khr &&
        format <= kGlCompressedSrgb8Alpha8Astc12x12Khr) {
        return kAstcBlocks[format - kGlCompressedSrgb8Alpha8Astc4x4Khr];
    }
    switch (format) {
        case kGlEtc1Rgb8Oes:
        case kGlCompressedR11Eac:
        case kGlCompressedSignedR11Eac:
        case kGlCompressedRgb8Etc2:
        case kGlCompressedSrgb8Etc2:
        case kGlCompressedRgb8PunchthroughAlpha1Etc2:
        case kGlCompressedSrgb8PunchthroughAlpha1Etc2:
            return {4, 4, 8};
        case kGlCompressedRg11Eac:
        case kGlCompressedSignedRg11Eac:
        case kGlCompressedRgba8Etc2Eac:
        case kGlCompressedSrgb8Alpha8Etc2Eac:
            return {4, 4, 16};
        default:
            return {};
    }
}

constexpr bool IsAstcFormat(GLenum format) noexcept {
    return (format >= kGlCompressedRgbaAstc4x4Khr &&
            format <= kGlCompressedRgbaAstc12x12Khr) ||
           (format >= kGlCompressedSrgb8Alpha8Astc4x4Khr &&
            format <= kGlCompressedSrgb8Alpha8Astc12x12Khr);
}

constexpr bool IsEtc2OrEacFormat(GLenum format) noexcept {
    return format >= kGlCompressedR11Eac && format <= kGlCompressedSrgb8Alpha8Etc2Eac;
}

constexpr bool HasExtensionToken(std::string_view extensions,
                                 std::string_view token) noexcept {
    std::size_t cursor = 0;
    while (cursor < extensions.size()) {
        while (cursor < extensions.size() && extensions[cursor] == ' ') ++cursor;
        const std::size_t end = extensions.find(' ', cursor);
        const std::size_t length = (end == std::string_view::npos ? extensions.size() : end) - cursor;
        if (extensions.substr(cursor, length) == token) return true;
        if (end == std::string_view::npos) break;
        cursor = end + 1;
    }
    return false;
}

constexpr bool VersionGuaranteesCompressedFormat(GLenum format,
                                                 GlesVersion version) noexcept {
    if (IsAstcFormat(format)) {
        return version.major > 3 || (version.major == 3 && version.minor >= 2);
    }
    if (IsEtc2OrEacFormat(format)) return version.major >= 3;
    return false;
}

constexpr bool ExtensionGuaranteesCompressedFormat(GLenum format,
                                                   std::string_view extensions) noexcept {
    if (IsAstcFormat(format)) {
        return HasExtensionToken(extensions, "GL_KHR_texture_compression_astc_ldr") ||
               HasExtensionToken(extensions, "GL_KHR_texture_compression_astc_hdr");
    }
    if (format == kGlEtc1Rgb8Oes) {
        return HasExtensionToken(extensions, "GL_OES_compressed_ETC1_RGB8_texture");
    }
    return false;
}

constexpr std::size_t ExpectedCompressedBytes(GLsizei width, GLsizei height,
                                              BlockCompressionInfo info) noexcept {
    if (width <= 0 || height <= 0 || !info) return 0;
    const std::size_t blocks_x =
        (static_cast<std::size_t>(width) + info.block_width - 1) / info.block_width;
    const std::size_t blocks_y =
        (static_cast<std::size_t>(height) + info.block_height - 1) / info.block_height;
    if (blocks_x > std::numeric_limits<std::size_t>::max() / blocks_y) return 0;
    const std::size_t blocks = blocks_x * blocks_y;
    if (blocks > std::numeric_limits<std::size_t>::max() /
                     static_cast<std::size_t>(info.block_bytes)) {
        return 0;
    }
    return blocks * static_cast<std::size_t>(info.block_bytes);
}

}  // namespace bridge_gl
