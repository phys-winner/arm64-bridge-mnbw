#include "mod_texture_conversion.h"
#include "astc.h"
#include "etc.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
using mod_texture::DxtFormat;

void Check(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::abort(); }
}

void Put32(std::vector<uint8_t>& data, size_t offset, uint32_t value) {
    for (size_t i = 0; i < 4; ++i) data[offset + i] = static_cast<uint8_t>(value >> (8u * i));
}

std::vector<uint8_t> Read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    Check(input.good(), "open fixture");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void Write(const fs::path& path, const std::vector<uint8_t>& data) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    Check(output.good(), "write fixture");
}

std::vector<uint8_t> Fixture(DxtFormat format, uint32_t width, uint32_t height,
                            uint32_t mips, int alpha_level = -1) {
    std::vector<uint8_t> bytes(128, 0);
    std::copy_n("DDS ", 4, bytes.begin());
    Put32(bytes, 4, 124);
    Put32(bytes, 8, 0x000a1007u);
    Put32(bytes, 12, height);
    Put32(bytes, 16, width);
    Put32(bytes, 28, mips);
    Put32(bytes, 76, 32);
    Put32(bytes, 108, mips > 1 ? 0x00401008u : 0x1000u);
    if (format == DxtFormat::kBgra8 || format == DxtFormat::kBgrx8) {
        Put32(bytes, 80, format == DxtFormat::kBgra8 ? 0x41u : 0x40u);
        Put32(bytes, 88, 32);
        Put32(bytes, 92, 0x00ff0000u);
        Put32(bytes, 96, 0x0000ff00u);
        Put32(bytes, 100, 0x000000ffu);
        Put32(bytes, 104, format == DxtFormat::kBgra8 ? 0xff000000u : 0u);
    } else {
        Put32(bytes, 80, 4);
        const char* fourcc = format == DxtFormat::kDxt1 ? "DXT1" :
            (format == DxtFormat::kDxt3 ? "DXT3" : "DXT5");
        std::copy_n(fourcc, 4, bytes.begin() + 84);
    }
    for (uint32_t level = 0; level < mips; ++level) {
        const size_t offset = bytes.size();
        const size_t size = mod_texture::DxtBytes(format, width, height);
        if (level == 0) Put32(bytes, 20, static_cast<uint32_t>(size));
        bytes.resize(offset + size);
        if (format == DxtFormat::kBgra8 || format == DxtFormat::kBgrx8) {
            for (uint32_t y = 0; y < height; ++y) {
                for (uint32_t x = 0; x < width; ++x) {
                    const size_t p = offset + (static_cast<size_t>(y) * width + x) * 4u;
                    bytes[p] = y < height / 2u ? 16u : 224u;
                    bytes[p + 1] = static_cast<uint8_t>(40u + x % 20u);
                    bytes[p + 2] = y < height / 2u ? 224u : 16u;
                    bytes[p + 3] = static_cast<int>(level) == alpha_level ? 64u : 255u;
                }
            }
        } else {
            const size_t block_bytes = format == DxtFormat::kDxt1 ? 8u : 16u;
            for (size_t b = 0; b < size; b += block_bytes) {
                const size_t color = offset + b + (block_bytes == 8 ? 0u : 8u);
                bytes[color] = 0x00;
                bytes[color + 1] = 0xf8;  // red 565 endpoint, index 0
                if (format == DxtFormat::kDxt3)
                    std::fill_n(bytes.begin() + static_cast<ptrdiff_t>(offset + b), 8, 0x44u);
                if (format == DxtFormat::kDxt5) bytes[offset + b] = 64u;
                if (format == DxtFormat::kDxt1 && static_cast<int>(level) == alpha_level) {
                    bytes[color] = bytes[color + 1] = 0;
                    bytes[color + 2] = 0xff;
                    bytes[color + 3] = 0xff;
                    std::fill_n(bytes.begin() + static_cast<ptrdiff_t>(color + 4), 4, 0xffu);
                }
            }
        }
        width = std::max(1u, width / 2u);
        height = std::max(1u, height / 2u);
    }
    return bytes;
}

void CheckConversion(const fs::path& root, DxtFormat format, int target,
                     uint32_t expected_format, int alpha_level = -1) {
    const auto path = root / ("case-" + std::to_string(static_cast<int>(format)) + "-" +
        std::to_string(target) + "-" + std::to_string(alpha_level) + ".dds");
    const auto source = Fixture(format, 16, 8, 5, alpha_level);
    Write(path, source);
    const auto name = path.string();
    Check(mod_texture::InspectModTexture(name.c_str(), target) == 1, "original needs conversion");
    const auto error = mod_texture::ConvertModTexture(name.c_str(), target);
    if (!error.empty()) std::fprintf(stderr, "%s\n", error.c_str());
    Check(error.empty(), "conversion succeeds");
    const auto converted = Read(path);
    Check(converted != source && std::equal(converted.begin(), converted.begin() + 8, "MBTXC001"),
          "conversion replaces original bytes with a distinct container");
    Check(!fs::exists(name + ".mbtex"), "no duplicate sidecar remains");
    Check(mod_texture::InspectModTexture(name.c_str(), target) == 2, "fully verified converted");
    const auto modified = fs::last_write_time(path);
    Check(mod_texture::ConvertModTexture(name.c_str(), target).empty(), "resume skips converted");
    Check(Read(path) == converted && fs::last_write_time(path) == modified,
          "resume performs no replacement or additional lossy encoding");
    Check(mod_texture::InspectModTexture(name.c_str(), target == 1 ? 2 : 1) == -1,
          "different target requires original instead of lossy recompression");
    mod_texture::ConvertedTexture stored;
    bool handled = false;
    Check(mod_texture::ReadConvertedModTexture(name.c_str(), stored, &handled) && handled,
          "runtime recognizes verified metadata");
    Check(stored.mips.size() == 5 && stored.width == 16 && stored.height == 8, "mip chain and dimensions preserved");
    size_t source_offset = 128;
    for (const auto& mip : stored.mips) {
        Check(mip.format == expected_format, "all mips have one correct upload format");
        std::vector<uint8_t> payload;
        Check(mod_texture::ReadConvertedModMip(name, mip, payload), "read verifies stored mip CRC");
        std::vector<uint8_t> reference;
        bool alpha = false;
        const size_t source_size = mod_texture::DxtBytes(format, mip.width, mip.height);
        Check(mod_texture::DecodeDxt(format, source.data() + source_offset, source_size,
                                     mip.width, mip.height, reference, alpha), "reference decode");
        if (expected_format == mod_texture::kStoredRgba8) {
            Check(payload == reference, "RGBA fallback preserves exact alpha, channels and row orientation");
        } else {
            std::vector<uint32_t> decoded(static_cast<size_t>(mip.width) * mip.height);
            const int block = expected_format == mod_texture::kStoredAstc6x6 ? 6 : 4;
            const int ok = expected_format == mod_texture::kStoredEtc1
                ? decode_etc1(payload.data(), mip.width, mip.height, decoded.data())
                : decode_astc(payload.data(), mip.width, mip.height, block, block, decoded.data());
            Check(ok != 0, "stored compression is a real decodable stream");
            const auto* pixels = reinterpret_cast<const uint8_t*>(decoded.data());
            if ((format == DxtFormat::kBgra8 || format == DxtFormat::kBgrx8) && mip.height >= 4) {
                const size_t bottom = static_cast<size_t>(mip.height - 1) * mip.width * 4u;
                Check(pixels[2] > pixels[0] + 100u && pixels[bottom] > pixels[bottom + 2] + 100u,
                      "compressed rows and red/blue channels retain orientation");
            }
            if (alpha) Check(pixels[3] < 150u, "compressed alpha is preserved");
        }
        source_offset += source_size;
    }
    auto damaged = converted;
    damaged.back() ^= 0x80u;
    Write(path, damaged);
    Check(mod_texture::InspectModTexture(name.c_str(), target) == -1, "payload corruption fails closed");
    Check(!mod_texture::ConvertModTexture(name.c_str(), target).empty() && Read(path) == damaged,
          "damaged converted texture is never re-encoded as DXT");
    damaged = converted;
    damaged[8] = 2;
    Write(path, damaged);
    Check(mod_texture::InspectModTexture(name.c_str(), target) == -1, "unknown container version fails closed");
    damaged = converted;
    damaged.resize(damaged.size() - 1);
    Write(path, damaged);
    Check(mod_texture::InspectModTexture(name.c_str(), target) == -1, "truncated converted file fails closed");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2) {
        size_t dds_count = 0, convertible = 0;
        for (const auto& entry : fs::recursive_directory_iterator(argv[1])) {
            if (!entry.is_regular_file() || entry.path().extension() != ".dds") continue;
            ++dds_count;
            std::string error;
            const auto name = entry.path().string();
            const int status = mod_texture::InspectModTexture(name.c_str(), 1, &error);
            if (status != 1) {
                std::fprintf(stderr, "Bad supplied mod texture: %s: status=%d %s\n",
                             name.c_str(), status, error.c_str());
                return 1;
            }
            ++convertible;
        }
        std::printf("Pendor DDS inventory: %zu files, %zu convertible\n", dds_count, convertible);
        return dds_count == 853 && convertible == 853 ? 0 : 1;
    }
    const auto serial = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() / ("warband-mod-conversion-" + std::to_string(serial));
    Check(fs::create_directory(root), "create isolated test directory");
    Check(mod_texture::ModTextureCrc32("123456789", 9) == 0xcbf43926u, "standard CRC32 check vector");
    CheckConversion(root, DxtFormat::kDxt1, 1, mod_texture::kStoredAstc6x6);
    CheckConversion(root, DxtFormat::kDxt3, 1, mod_texture::kStoredAstc4x4);
    CheckConversion(root, DxtFormat::kDxt5, 1, mod_texture::kStoredAstc4x4);
    CheckConversion(root, DxtFormat::kBgra8, 1, mod_texture::kStoredAstc4x4, 0);
    CheckConversion(root, DxtFormat::kBgrx8, 1, mod_texture::kStoredAstc4x4);
    CheckConversion(root, DxtFormat::kDxt1, 2, mod_texture::kStoredEtc1);
    CheckConversion(root, DxtFormat::kDxt1, 2, mod_texture::kStoredRgba8, 4);
    CheckConversion(root, DxtFormat::kDxt3, 2, mod_texture::kStoredRgba8);
    CheckConversion(root, DxtFormat::kDxt5, 2, mod_texture::kStoredRgba8);
    CheckConversion(root, DxtFormat::kBgra8, 2, mod_texture::kStoredRgba8, 4);
    CheckConversion(root, DxtFormat::kBgra8, 2, mod_texture::kStoredEtc1);
    CheckConversion(root, DxtFormat::kBgrx8, 2, mod_texture::kStoredEtc1);

    const auto path = root / "failure.dds";
    const auto name = path.string();
    auto original = Fixture(DxtFormat::kBgra8, 9, 7, 4);
    Write(path, original);
    const auto stale = fs::path(name + ".mbtex.tmp.999999.0");
    Write(stale, original);
    mod_texture::SetModTextureTestWriteBudget(80 + 4 * 32 + 96);
    const auto failure = mod_texture::ConvertModTexture(name.c_str(), 1);
    mod_texture::SetModTextureTestWriteBudget(-1);
    Check(failure.find("free storage") != std::string::npos, "storage failure is actionable");
    Check(Read(path) == original && mod_texture::InspectModTexture(name.c_str(), 1) == 1,
          "failed conversion leaves every original byte intact for retry");
    Check(!fs::exists(stale), "an interrupted conversion's temp file is reclaimed");
    for (const auto& entry : fs::directory_iterator(root))
        Check(entry.path().filename().string().find(".tmp.") == std::string::npos,
              "failed conversion cleans its owned temporary file");
    Check(mod_texture::ConvertModTexture(name.c_str(), 1).empty(), "retry succeeds including non-block-aligned sizes");
    original.pop_back();
    Write(path, original);
    Check(mod_texture::InspectModTexture(name.c_str(), 1) == -1, "truncated raw DDS fails closed");
    Check(!mod_texture::ConvertModTexture(name.c_str(), 1).empty() && Read(path) == original,
          "malformed original never overwritten");
    original = Fixture(DxtFormat::kDxt1, 4, 4, 1);
    Put32(original, 112, 0x200u);
    Write(path, original);
    Check(mod_texture::InspectModTexture(name.c_str(), 1) == -1, "unsupported cubemap cannot be marked complete");
    original = Fixture(DxtFormat::kDxt1, 4, 4, 1);
    Put32(original, 8, 0x008a1007u);
    Put32(original, 24, 1u);
    Put32(original, 112, 0x00200000u);
    Write(path, original);
    Check(mod_texture::InspectModTexture(name.c_str(), 1) == 1,
          "exact one-slice legacy DDS volume flag is ordinary 2D data");
    Check(mod_texture::ConvertModTexture(name.c_str(), 1).empty(),
          "legacy one-slice texture converts in place");
    Check(mod_texture::InspectModTexture(name.c_str(), 1) == 2,
          "converted legacy one-slice texture is verified");
    fs::remove_all(root);
    std::puts("mod_texture_conversion_test PASS: formats, mips, alpha, orientation, resume, corruption, atomic storage failure");
    return 0;
}
