#include "etc1_encoder.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace bridge_gl {
namespace {

constexpr int kModifierTable[8][2] = {
    {2, 8}, {5, 17}, {9, 29}, {13, 42},
    {18, 60}, {24, 80}, {33, 106}, {47, 183},
};

// The selector bit stream is column-major in ETC1, while the decoded image is
// row-major. This is the inverse of WriteOrderTable in texture2ddecoder.
constexpr int kWriteOrder[16] = {
    0, 4, 8, 12, 1, 5, 9, 13,
    2, 6, 10, 14, 3, 7, 11, 15,
};

struct Rgb {
    int r = 0;
    int g = 0;
    int b = 0;
};

struct BlockResult {
    std::array<uint8_t, 8> bytes{};
    uint64_t error = std::numeric_limits<uint64_t>::max();
};

struct SelectorResult {
    int tables[2]{};
    std::array<uint8_t, 16> selectors{};
    uint64_t error = std::numeric_limits<uint64_t>::max();
};

int ClampByte(int value) {
    return std::clamp(value, 0, 255);
}

int Quantize4(int value) {
    return std::clamp((value + 8) / 17, 0, 15);
}

int Quantize5(int value) {
    return std::clamp((value + 4) / 8, 0, 31);
}

int Expand4(int value) {
    return value * 17;
}

int Expand5(int value) {
    return value * 8 + (value >> 2);
}

uint64_t ColorError(const Rgb& actual, const Rgb& target) {
    const int dr = actual.r - target.r;
    const int dg = actual.g - target.g;
    const int db = actual.b - target.b;
    return static_cast<uint64_t>(dr * dr + dg * dg + db * db);
}

int SubblockFor(int pixel, bool flip) {
    const int x = pixel & 3;
    const int y = pixel >> 2;
    // ETC1's flip bit selects the split encoded by Etc1SubblockTable:
    // bit 0 is a left/right split, bit 1 is a top/bottom split.
    return flip ? y / 2 : x / 2;
}

int ModifierFor(int table, int selector) {
    const int modifier = kModifierTable[table][selector & 1];
    return (selector & 2) != 0 ? -modifier : modifier;
}

SelectorResult SelectModifiers(const Rgb pixels[16], const Rgb bases[2],
                               bool flip) {
    SelectorResult result;
    result.error = 0;
    for (int subblock = 0; subblock < 2; ++subblock) {
        uint64_t best_subblock_error = std::numeric_limits<uint64_t>::max();
        int best_table = 0;
        std::array<uint8_t, 16> best_selectors{};
        for (int table = 0; table < 8; ++table) {
            uint64_t subblock_error = 0;
            std::array<uint8_t, 16> candidate_selectors{};
            for (int pixel = 0; pixel < 16; ++pixel) {
                if (SubblockFor(pixel, flip) != subblock) continue;
                uint64_t best_pixel_error = std::numeric_limits<uint64_t>::max();
                int best_selector = 0;
                for (int selector = 0; selector < 4; ++selector) {
                    const int modifier = ModifierFor(table, selector);
                    const Rgb actual{
                        ClampByte(bases[subblock].r + modifier),
                        ClampByte(bases[subblock].g + modifier),
                        ClampByte(bases[subblock].b + modifier),
                    };
                    const uint64_t error = ColorError(actual, pixels[pixel]);
                    if (error < best_pixel_error) {
                        best_pixel_error = error;
                        best_selector = selector;
                    }
                }
                candidate_selectors[pixel] = static_cast<uint8_t>(best_selector);
                subblock_error += best_pixel_error;
            }
            if (subblock_error < best_subblock_error) {
                best_subblock_error = subblock_error;
                best_table = table;
                best_selectors = candidate_selectors;
            }
        }
        result.tables[subblock] = best_table;
        result.error += best_subblock_error;
        for (int pixel = 0; pixel < 16; ++pixel) {
            if (SubblockFor(pixel, flip) == subblock) {
                result.selectors[pixel] = best_selectors[pixel];
            }
        }
    }
    return result;
}

BlockResult EncodeBlock(const Rgb pixels[16], bool flip) {
    Rgb averages[2]{};
    for (int pixel = 0; pixel < 16; ++pixel) {
        Rgb& average = averages[SubblockFor(pixel, flip)];
        average.r += pixels[pixel].r;
        average.g += pixels[pixel].g;
        average.b += pixels[pixel].b;
    }
    for (Rgb& average : averages) {
        average.r = (average.r + 4) / 8;
        average.g = (average.g + 4) / 8;
        average.b = (average.b + 4) / 8;
    }

    int individual_quantized[2][3]{};
    Rgb individual_bases[2]{};
    int differential_base[3]{};
    int differential_delta[3]{};
    bool differential_valid = true;
    for (int subblock = 0; subblock < 2; ++subblock) {
        const int values[3] = {
            averages[subblock].r, averages[subblock].g, averages[subblock].b,
        };
        for (int channel = 0; channel < 3; ++channel) {
            const int q4 = Quantize4(values[channel]);
            individual_quantized[subblock][channel] = q4;
            const int value = Expand4(q4);
            if (channel == 0) individual_bases[subblock].r = value;
            else if (channel == 1) individual_bases[subblock].g = value;
            else individual_bases[subblock].b = value;
        }
    }
    for (int channel = 0; channel < 3; ++channel) {
        const int value0 = channel == 0 ? averages[0].r :
                           channel == 1 ? averages[0].g : averages[0].b;
        const int value1 = channel == 0 ? averages[1].r :
                           channel == 1 ? averages[1].g : averages[1].b;
        const int q0 = Quantize5(value0);
        const int q1 = Quantize5(value1);
        const int delta = q1 - q0;
        if (delta < -4 || delta > 3) differential_valid = false;
        differential_base[channel] = q0;
        differential_delta[channel] = delta;
    }

    Rgb differential_bases[2]{};
    for (int channel = 0; channel < 3; ++channel) {
        const int value0 = Expand5(differential_base[channel]);
        const int value1 = Expand5(differential_base[channel] +
                                   differential_delta[channel]);
        if (channel == 0) {
            differential_bases[0].r = value0;
            differential_bases[1].r = value1;
        } else if (channel == 1) {
            differential_bases[0].g = value0;
            differential_bases[1].g = value1;
        } else {
            differential_bases[0].b = value0;
            differential_bases[1].b = value1;
        }
    }

    // Select the modifier table using the complete RGB reconstruction error,
    // then compare ETC1 modes using that same error. The old heuristic picked
    // a table from the largest channel delta and could select a very large
    // negative modifier for a thin brown detail, crushing it to black.
    const SelectorResult individual_selection =
        SelectModifiers(pixels, individual_bases, flip);
    const SelectorResult differential_selection =
        differential_valid ? SelectModifiers(pixels, differential_bases, flip)
                            : SelectorResult{};
    const bool use_differential = differential_valid &&
                                  differential_selection.error <
                                      individual_selection.error;
    const SelectorResult& selected = use_differential ? differential_selection
                                                        : individual_selection;
    int individual_q[2][3]{};
    for (int subblock = 0; subblock < 2; ++subblock) {
        individual_q[subblock][0] = individual_quantized[subblock][0];
        individual_q[subblock][1] = individual_quantized[subblock][1];
        individual_q[subblock][2] = individual_quantized[subblock][2];
    }

    BlockResult result;
    result.error = selected.error;
    uint8_t* bytes = result.bytes.data();
    if (use_differential) {
        bytes[0] = static_cast<uint8_t>((differential_base[0] << 3) |
                                        (differential_delta[0] < 0
                                             ? differential_delta[0] + 8
                                             : differential_delta[0]));
        bytes[1] = static_cast<uint8_t>((differential_base[1] << 3) |
                                        (differential_delta[1] < 0
                                             ? differential_delta[1] + 8
                                             : differential_delta[1]));
        bytes[2] = static_cast<uint8_t>((differential_base[2] << 3) |
                                        (differential_delta[2] < 0
                                             ? differential_delta[2] + 8
                                             : differential_delta[2]));
    } else {
        bytes[0] = static_cast<uint8_t>((individual_q[0][0] << 4) | individual_q[1][0]);
        bytes[1] = static_cast<uint8_t>((individual_q[0][1] << 4) | individual_q[1][1]);
        bytes[2] = static_cast<uint8_t>((individual_q[0][2] << 4) | individual_q[1][2]);
    }
    bytes[3] = static_cast<uint8_t>((selected.tables[0] << 5) |
                                    (selected.tables[1] << 2) |
                                    (use_differential ? 2 : 0) | (flip ? 1 : 0));

    uint16_t most_significant = 0;
    uint16_t least_significant = 0;
    for (int stream_index = 0; stream_index < 16; ++stream_index) {
        const int pixel = kWriteOrder[stream_index];
        const uint8_t selector = selected.selectors[pixel];
        least_significant |= static_cast<uint16_t>(selector & 1) << stream_index;
        most_significant |= static_cast<uint16_t>((selector >> 1) & 1) << stream_index;
    }
    bytes[4] = static_cast<uint8_t>(most_significant >> 8);
    bytes[5] = static_cast<uint8_t>(most_significant);
    bytes[6] = static_cast<uint8_t>(least_significant >> 8);
    bytes[7] = static_cast<uint8_t>(least_significant);
    return result;
}

}  // namespace

bool EncodeEtc1(const uint8_t* rgba, int width, int height,
                std::vector<uint8_t>& encoded) {
    encoded.clear();
    if (!rgba || width <= 0 || height <= 0) return false;

    const size_t width_size = static_cast<size_t>(width);
    const size_t height_size = static_cast<size_t>(height);
    if (width_size > std::numeric_limits<size_t>::max() / height_size ||
        width_size * height_size > std::numeric_limits<size_t>::max() / 4) {
        return false;
    }
    const size_t pixel_count = width_size * height_size;
    for (size_t pixel = 0; pixel < pixel_count; ++pixel) {
        if (rgba[pixel * 4 + 3] != 255) return false;
    }

    const size_t blocks_x = (width_size + 3) / 4;
    const size_t blocks_y = (height_size + 3) / 4;
    if (blocks_x > std::numeric_limits<size_t>::max() / blocks_y ||
        blocks_x * blocks_y > std::numeric_limits<size_t>::max() / 8) {
        return false;
    }
    const size_t encoded_bytes = blocks_x * blocks_y * 8;
    if (encoded_bytes > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return false;
    }

    try {
        encoded.resize(encoded_bytes);
    } catch (...) {
        encoded.clear();
        return false;
    }

    size_t output_offset = 0;
    for (size_t by = 0; by < blocks_y; ++by) {
        for (size_t bx = 0; bx < blocks_x; ++bx) {
            Rgb pixels[16]{};
            for (int y = 0; y < 4; ++y) {
                const size_t source_y = std::min(by * 4 + static_cast<size_t>(y), height_size - 1);
                for (int x = 0; x < 4; ++x) {
                    const size_t source_x = std::min(bx * 4 + static_cast<size_t>(x), width_size - 1);
                    const uint8_t* source = rgba + (source_y * width_size + source_x) * 4;
                    Rgb& pixel = pixels[y * 4 + x];
                    pixel.r = source[0];
                    pixel.g = source[1];
                    pixel.b = source[2];
                }
            }
            const BlockResult horizontal = EncodeBlock(pixels, false);
            const BlockResult vertical = EncodeBlock(pixels, true);
            const BlockResult& best = vertical.error < horizontal.error ? vertical : horizontal;
            std::copy(best.bytes.begin(), best.bytes.end(), encoded.begin() + output_offset);
            output_offset += 8;
        }
    }
    return true;
}

}  // namespace bridge_gl
