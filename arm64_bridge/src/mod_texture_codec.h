#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace mod_texture {

enum class DxtFormat : uint8_t {
    kNone = 0,
    kDxt1 = 1,
    kDxt3 = 3,
    kDxt5 = 5,
    // Classic desktop Warband modules commonly ship uncompressed DDS files
    // whose channel layout is A8R8G8B8 in the DDS masks (BGRA byte order).
    kBgra8 = 8,
    kBgrx8 = 9,
};

constexpr uint32_t kTokenMagic0 = 0x444D424Du;  // "MBMD"
constexpr uint32_t kTokenMagic1 = 0x31545844u;  // "DXT1" bridge token version
constexpr uint32_t kTokenSalt = 0xA79E51C3u;

struct Token {
    uint32_t magic0;
    uint32_t magic1;
    uint32_t id;
    uint32_t guard;
};
static_assert(sizeof(Token) == 16);

inline Token MakeToken(uint32_t id) {
    return Token{kTokenMagic0, kTokenMagic1, id,
                 kTokenMagic0 ^ kTokenMagic1 ^ id ^ kTokenSalt};
}

inline bool ReadToken(const void* data, size_t size, uint32_t& id) {
    if (!data || size < sizeof(Token)) return false;
    Token token{};
    std::memcpy(&token, data, sizeof(token));
    if (token.magic0 != kTokenMagic0 || token.magic1 != kTokenMagic1 ||
        token.id == 0 ||
        token.guard != (token.magic0 ^ token.magic1 ^ token.id ^ kTokenSalt)) {
        return false;
    }
    id = token.id;
    return true;
}

inline size_t DxtBytes(DxtFormat format, uint32_t width, uint32_t height) {
    if (format == DxtFormat::kNone || width == 0 || height == 0) return 0;
    if (format == DxtFormat::kBgra8 || format == DxtFormat::kBgrx8) {
        const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
        if (pixels > std::numeric_limits<size_t>::max() / 4u) return 0;
        return pixels * 4u;
    }
    const size_t blocks_x = (static_cast<size_t>(width) + 3u) / 4u;
    const size_t blocks_y = (static_cast<size_t>(height) + 3u) / 4u;
    const size_t block_bytes = format == DxtFormat::kDxt1 ? 8u : 16u;
    if (blocks_x > std::numeric_limits<size_t>::max() / blocks_y ||
        blocks_x * blocks_y > std::numeric_limits<size_t>::max() / block_bytes) {
        return 0;
    }
    return blocks_x * blocks_y * block_bytes;
}

// The Android guest remaps desktop DXT1 to ASTC 6x6 and DXT3/5 (including
// virtualized BGRA/BGRX) to ASTC 4x4. These helpers describe the byte layout
// of the synthetic payload handed to that loader.
inline size_t VirtualAstcBytes(DxtFormat format, uint32_t width, uint32_t height) {
    if (format == DxtFormat::kNone || width == 0u || height == 0u) return 0u;
    const size_t block = format == DxtFormat::kDxt1 ? 6u : 4u;
    const size_t blocks_x = (static_cast<size_t>(width) + block - 1u) / block;
    const size_t blocks_y = (static_cast<size_t>(height) + block - 1u) / block;
    if (blocks_x > std::numeric_limits<size_t>::max() / blocks_y ||
        blocks_x * blocks_y > std::numeric_limits<size_t>::max() / 16u) {
        return 0u;
    }
    return blocks_x * blocks_y * 16u;
}

inline size_t VirtualAstcLastRowOffset(DxtFormat format, uint32_t width,
                                       uint32_t height) {
    if (format == DxtFormat::kNone || width == 0u || height == 0u) return 0u;
    const size_t block = format == DxtFormat::kDxt1 ? 6u : 4u;
    const size_t blocks_x = (static_cast<size_t>(width) + block - 1u) / block;
    const size_t blocks_y = (static_cast<size_t>(height) + block - 1u) / block;
    if (blocks_x > std::numeric_limits<size_t>::max() / 16u) return 0u;
    const size_t row_bytes = blocks_x * 16u;
    if (blocks_y - 1u > std::numeric_limits<size_t>::max() / row_bytes) return 0u;
    return (blocks_y - 1u) * row_bytes;
}

inline uint16_t Read16(const uint8_t* source) {
    return static_cast<uint16_t>(source[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(source[1]) << 8u);
}

inline uint32_t Read32(const uint8_t* source) {
    return static_cast<uint32_t>(source[0]) |
           (static_cast<uint32_t>(source[1]) << 8u) |
           (static_cast<uint32_t>(source[2]) << 16u) |
           (static_cast<uint32_t>(source[3]) << 24u);
}

inline uint8_t Expand5(uint32_t value) {
    return static_cast<uint8_t>((value << 3u) | (value >> 2u));
}

inline uint8_t Expand6(uint32_t value) {
    return static_cast<uint8_t>((value << 2u) | (value >> 4u));
}

inline void Decode565(uint16_t packed, uint8_t color[4]) {
    color[0] = Expand5((packed >> 11u) & 31u);
    color[1] = Expand6((packed >> 5u) & 63u);
    color[2] = Expand5(packed & 31u);
    color[3] = 255u;
}

inline void DecodeColorPalette(const uint8_t* source, bool allow_transparency,
                               uint8_t colors[4][4]) {
    const uint16_t color0 = Read16(source);
    const uint16_t color1 = Read16(source + 2);
    Decode565(color0, colors[0]);
    Decode565(color1, colors[1]);
    if (!allow_transparency || color0 > color1) {
        for (size_t channel = 0; channel < 3; ++channel) {
            colors[2][channel] = static_cast<uint8_t>(
                (2u * colors[0][channel] + colors[1][channel]) / 3u);
            colors[3][channel] = static_cast<uint8_t>(
                (colors[0][channel] + 2u * colors[1][channel]) / 3u);
        }
        colors[2][3] = 255u;
        colors[3][3] = 255u;
    } else {
        for (size_t channel = 0; channel < 3; ++channel) {
            colors[2][channel] = static_cast<uint8_t>(
                (colors[0][channel] + colors[1][channel]) / 2u);
            colors[3][channel] = 0u;
        }
        colors[2][3] = 255u;
        colors[3][3] = 0u;
    }
}

inline bool DecodeDxt(DxtFormat format, const uint8_t* source, size_t source_size,
                      uint32_t width, uint32_t height, std::vector<uint8_t>& rgba,
                      bool& has_alpha) {
    const size_t expected = DxtBytes(format, width, height);
    const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (!source || expected == 0 || source_size != expected ||
        width > 16384u || height > 16384u ||
        pixels > std::numeric_limits<size_t>::max() / 4u) {
        return false;
    }
    rgba.assign(pixels * 4u, 0u);
    has_alpha = false;
    if (format == DxtFormat::kBgra8 || format == DxtFormat::kBgrx8) {
        for (size_t pixel = 0; pixel < pixels; ++pixel) {
            const size_t offset = pixel * 4u;
            rgba[offset + 0u] = source[offset + 2u];
            rgba[offset + 1u] = source[offset + 1u];
            rgba[offset + 2u] = source[offset + 0u];
            const uint8_t alpha = format == DxtFormat::kBgrx8
                ? 255u : source[offset + 3u];
            rgba[offset + 3u] = alpha;
            has_alpha = has_alpha || alpha != 255u;
        }
        return true;
    }
    const uint32_t blocks_x = (width + 3u) / 4u;
    const uint32_t blocks_y = (height + 3u) / 4u;
    const size_t block_bytes = format == DxtFormat::kDxt1 ? 8u : 16u;

    for (uint32_t by = 0; by < blocks_y; ++by) {
        for (uint32_t bx = 0; bx < blocks_x; ++bx) {
            const uint8_t* block = source +
                (static_cast<size_t>(by) * blocks_x + bx) * block_bytes;
            const uint8_t* color_block = block + (format == DxtFormat::kDxt1 ? 0u : 8u);
            uint8_t colors[4][4]{};
            DecodeColorPalette(color_block, format == DxtFormat::kDxt1, colors);
            const uint32_t color_indices = Read32(color_block + 4);

            uint8_t alpha_values[8]{};
            uint64_t alpha_indices = 0;
            uint64_t explicit_alpha = 0;
            if (format == DxtFormat::kDxt3) {
                for (size_t i = 0; i < 8; ++i) {
                    explicit_alpha |= static_cast<uint64_t>(block[i]) << (i * 8u);
                }
            } else if (format == DxtFormat::kDxt5) {
                alpha_values[0] = block[0];
                alpha_values[1] = block[1];
                if (alpha_values[0] > alpha_values[1]) {
                    for (uint32_t i = 1; i <= 6; ++i) {
                        alpha_values[i + 1] = static_cast<uint8_t>(
                            ((7u - i) * alpha_values[0] + i * alpha_values[1]) / 7u);
                    }
                } else {
                    for (uint32_t i = 1; i <= 4; ++i) {
                        alpha_values[i + 1] = static_cast<uint8_t>(
                            ((5u - i) * alpha_values[0] + i * alpha_values[1]) / 5u);
                    }
                    alpha_values[6] = 0u;
                    alpha_values[7] = 255u;
                }
                for (size_t i = 0; i < 6; ++i) {
                    alpha_indices |= static_cast<uint64_t>(block[2 + i]) << (i * 8u);
                }
            }

            for (uint32_t py = 0; py < 4; ++py) {
                for (uint32_t px = 0; px < 4; ++px) {
                    const uint32_t x = bx * 4u + px;
                    const uint32_t y = by * 4u + py;
                    if (x >= width || y >= height) continue;
                    const uint32_t local = py * 4u + px;
                    const uint32_t color_index = (color_indices >> (local * 2u)) & 3u;
                    uint8_t alpha = colors[color_index][3];
                    if (format == DxtFormat::kDxt3) {
                        alpha = static_cast<uint8_t>(
                            ((explicit_alpha >> (local * 4u)) & 15u) * 17u);
                    } else if (format == DxtFormat::kDxt5) {
                        alpha = alpha_values[(alpha_indices >> (local * 3u)) & 7u];
                    }
                    has_alpha = has_alpha || alpha != 255u;
                    const size_t destination =
                        (static_cast<size_t>(y) * width + x) * 4u;
                    rgba[destination + 0] = colors[color_index][0];
                    rgba[destination + 1] = colors[color_index][1];
                    rgba[destination + 2] = colors[color_index][2];
                    rgba[destination + 3] = alpha;
                }
            }
        }
    }
    return true;
}

}  // namespace mod_texture
