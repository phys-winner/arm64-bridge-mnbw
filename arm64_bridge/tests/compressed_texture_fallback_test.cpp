#include "../src/compressed_texture_support.h"
#include "../src/etc1_encoder.h"

#include "astc.h"
#include "etc.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static uint64_t RgbError(const uint8_t* expected, const uint8_t* actual,
                         int width, int height) {
    uint64_t error = 0;
    for (int pixel = 0; pixel < width * height; ++pixel) {
        for (int channel = 0; channel < 3; ++channel) {
            const int difference = static_cast<int>(expected[pixel * 4 + channel]) -
                                   static_cast<int>(actual[pixel * 4 + channel]);
            error += static_cast<uint64_t>(difference * difference);
        }
    }
    return error;
}

static bool CheckOpaqueAstcDds(const char* path, const char* encoded_path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    std::vector<uint8_t> file;
    file.assign(std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>());
    if (file.size() < 128 || file[0] != 'D' || file[1] != 'D' ||
        file[2] != 'S' || file[3] != ' ') {
        return false;
    }
    const auto read_u32 = [&file](size_t offset) -> uint32_t {
        return static_cast<uint32_t>(file[offset]) |
               (static_cast<uint32_t>(file[offset + 1]) << 8) |
               (static_cast<uint32_t>(file[offset + 2]) << 16) |
               (static_cast<uint32_t>(file[offset + 3]) << 24);
    };
    const uint32_t width = read_u32(16);
    const uint32_t height = read_u32(12);
    if (width == 0 || height == 0 || width > 2048 || height > 2048 ||
        file[84] != 'D' || file[85] != 'X' || file[86] != 'T' || file[87] != '1') {
        return false;
    }
    const size_t blocks_x = (static_cast<size_t>(width) + 5) / 6;
    const size_t blocks_y = (static_cast<size_t>(height) + 5) / 6;
    const size_t level_bytes = blocks_x * blocks_y * 16;
    if (level_bytes > file.size() - 128) return false;

    const size_t pixel_count = static_cast<size_t>(width) * height;
    std::vector<uint32_t> astc_pixels(pixel_count);
    if (!decode_astc(file.data() + 128, static_cast<int>(width),
                     static_cast<int>(height), 6, 6, astc_pixels.data())) {
        return false;
    }
    std::vector<uint8_t> rgba(pixel_count * 4);
    for (size_t pixel = 0; pixel < pixel_count; ++pixel) {
        const uint8_t* raw = reinterpret_cast<const uint8_t*>(&astc_pixels[pixel]);
        rgba[pixel * 4 + 0] = raw[2];
        rgba[pixel * 4 + 1] = raw[1];
        rgba[pixel * 4 + 2] = raw[0];
        rgba[pixel * 4 + 3] = raw[3];
    }
    std::vector<uint8_t> encoded;
    if (!bridge_gl::EncodeEtc1(rgba.data(), static_cast<int>(width),
                               static_cast<int>(height), encoded)) {
        return false;
    }
    std::vector<uint32_t> etc_pixels(pixel_count);
    if (!decode_etc1(encoded.data(), static_cast<int>(width),
                     static_cast<int>(height), etc_pixels.data())) {
        return false;
    }
    std::vector<uint8_t> decoded_rgba(pixel_count * 4);
    for (size_t pixel = 0; pixel < pixel_count; ++pixel) {
        const uint8_t* raw = reinterpret_cast<const uint8_t*>(&etc_pixels[pixel]);
        decoded_rgba[pixel * 4 + 0] = raw[2];
        decoded_rgba[pixel * 4 + 1] = raw[1];
        decoded_rgba[pixel * 4 + 2] = raw[0];
        decoded_rgba[pixel * 4 + 3] = raw[3];
    }
    const uint64_t error = RgbError(rgba.data(), decoded_rgba.data(),
                                    static_cast<int>(width), static_cast<int>(height));
    if (encoded_path != nullptr) {
        std::ofstream output(encoded_path, std::ios::binary);
        if (!output) return false;
        output.write(reinterpret_cast<const char*>(encoded.data()),
                     static_cast<std::streamsize>(encoded.size()));
        if (!output) return false;
    }
    std::printf("opaque_astc_dds=%s size=%ux%u encoded_bytes=%zu rgb_error=%llu\n",
                path, width, height, encoded.size(),
                static_cast<unsigned long long>(error));
    return error <= pixel_count * 2200;
}

static_assert(bridge_gl::CompressedBlockInfo(
                  bridge_gl::kGlCompressedRgbaAstc4x4Khr).block_width == 4);
static_assert(bridge_gl::CompressedBlockInfo(
                  bridge_gl::kGlCompressedRgbaAstc4x4Khr + 4).block_width == 6);
static_assert(bridge_gl::CompressedBlockInfo(
                  bridge_gl::kGlCompressedRgbaAstc4x4Khr + 4).block_height == 6);
static_assert(bridge_gl::ExpectedCompressedBytes(
                  13, 11,
                  bridge_gl::CompressedBlockInfo(
                      bridge_gl::kGlCompressedRgbaAstc4x4Khr + 4)) == 96);
static_assert(bridge_gl::ExpectedCompressedBytes(
                  13, 11,
                  bridge_gl::CompressedBlockInfo(
                      bridge_gl::kGlCompressedRgbaAstc4x4Khr)) == 192);
static_assert(bridge_gl::VersionGuaranteesCompressedFormat(
    bridge_gl::kGlCompressedRgbaAstc4x4Khr, {3, 2}));
static_assert(!bridge_gl::VersionGuaranteesCompressedFormat(
    bridge_gl::kGlCompressedRgbaAstc4x4Khr, {3, 1}));
static_assert(bridge_gl::VersionGuaranteesCompressedFormat(
    bridge_gl::kGlCompressedRgba8Etc2Eac, {3, 0}));
static_assert(bridge_gl::ExtensionGuaranteesCompressedFormat(
    bridge_gl::kGlEtc1Rgb8Oes,
    "GL_EXT_alpha GL_OES_compressed_ETC1_RGB8_texture GL_EXT_beta"));
static_assert(!bridge_gl::ExtensionGuaranteesCompressedFormat(
    bridge_gl::kGlEtc1Rgb8Oes,
    "GL_OES_compressed_ETC1_RGB8_texture_suffix"));

int main(int argc, char** argv) {
    // ASTC void-extent block: transparent black. This exercises the exact
    // decoder used when a GLES2/3.0/3.1 driver lacks ASTC support.
    constexpr std::array<uint8_t, 16> transparent_block = {
        0xfc, 0xfd, 0xff, 0xff, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    };
    std::array<uint32_t, 16> decoded{};
    if (!decode_astc(transparent_block.data(), 4, 4, 4, 4, decoded.data())) return 1;
    for (uint32_t pixel : decoded) {
        if (pixel != 0) return 2;
    }

    // The fallback must produce a valid ETC1 stream, preserve its block
    // geometry for edge-sized textures, and reject alpha rather than silently
    // destroying the texture's transparency semantics.
    constexpr int width = 7;
    constexpr int height = 5;
    std::array<uint8_t, width * height * 4> source{};
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int pixel = (y * width + x) * 4;
            source[pixel + 0] = static_cast<uint8_t>(20 + x * 25);
            source[pixel + 1] = static_cast<uint8_t>(30 + y * 35);
            source[pixel + 2] = static_cast<uint8_t>(220 - x * 20);
            source[pixel + 3] = 255;
        }
    }
    std::vector<uint8_t> encoded;
    if (!bridge_gl::EncodeEtc1(source.data(), width, height, encoded)) return 3;
    if (encoded.size() != 4 * 8) return 4;
    std::array<uint32_t, width * height> decoded_etc{};
    if (!decode_etc1(encoded.data(), width, height, decoded_etc.data())) return 5;
    std::array<uint8_t, width * height * 4> decoded_rgba{};
    for (size_t pixel = 0; pixel < decoded_etc.size(); ++pixel) {
        const uint8_t* raw = reinterpret_cast<const uint8_t*>(&decoded_etc[pixel]);
        decoded_rgba[pixel * 4 + 0] = raw[2];
        decoded_rgba[pixel * 4 + 1] = raw[1];
        decoded_rgba[pixel * 4 + 2] = raw[0];
        decoded_rgba[pixel * 4 + 3] = raw[3];
    }
    if (RgbError(source.data(), decoded_rgba.data(), width, height) > 220000) return 6;
    source[3] = 254;
    if (bridge_gl::EncodeEtc1(source.data(), width, height, encoded)) return 7;
    if (argc > 1 && !CheckOpaqueAstcDds(argv[1], argc > 2 ? argv[2] : nullptr)) return 8;
    return 0;
}
