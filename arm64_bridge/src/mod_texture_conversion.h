#pragma once

#include "mod_texture_codec.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mod_texture {

constexpr int kTargetAstc = 1;
constexpr int kTargetEtc1 = 2;
// Actual GLES upload formats; the container does not pretend ETC/RGBA is DXT.
constexpr uint32_t kStoredRgba8 = 0x1908u;
constexpr uint32_t kStoredEtc1 = 0x8d64u;
constexpr uint32_t kStoredAstc4x4 = 0x93b0u;
constexpr uint32_t kStoredAstc6x6 = 0x93b4u;

struct ConvertedMip {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;
    uint32_t checksum = 0;
    uint64_t offset = 0;
    uint64_t size = 0;
};

struct ConvertedTexture {
    std::string path;
    int target = 0;
    DxtFormat source_format = DxtFormat::kNone;
    uint32_t width = 0;
    uint32_t height = 0;
    bool has_alpha = false;
    std::vector<ConvertedMip> mips;
};

// 0: unrelated/unsupported non-DXT input, 1: convertible original,
// 2: the converted container and every stored mip are verified for this target,
// -1: invalid supported DDS (or unsupported DXT/DX10). Never trusts an extension
// or completion marker as proof that a texture has already been converted.
int InspectModTexture(const char* path, int target, std::string* error = nullptr);

// Atomically replaces path after validating and syncing a temporary sibling.
// Empty string means success. Until publication the original remains intact.
// Matching containers are never encoded again. A different target requires the
// original mod archive, avoiding additional lossy compression of converted data.
std::string ConvertModTexture(const char* path, int target);

// Loader fast path verifies metadata; every mip is verified when read. A
// recognized but damaged converted file sets handled=true and fails closed.
bool ReadConvertedModTexture(const char* path, ConvertedTexture& texture,
                              bool* handled = nullptr);
bool ReadConvertedModMip(const std::string& path, const ConvertedMip& mip,
                         std::vector<uint8_t>& payload);
uint32_t ModTextureCrc32(const void* data, size_t size);
#ifdef WARBAND_MOD_TEXTURE_TESTS
// Host-only storage fault injection; never present in the Android library.
void SetModTextureTestWriteBudget(int64_t bytes);
#endif

}  // namespace mod_texture
