#pragma once

#include <cstdint>
#include <vector>

// Encode one decoded RGBA8 mip into native ASTC LDR blocks. The bridge keeps
// one single-threaded codec context per footprint and serializes access; this
// avoids allocating astcenc's relatively large search tables for every mod
// texture while keeping guest render-thread behavior deterministic.
bool EncodeModTextureAstc(const uint8_t* rgba, uint32_t width, uint32_t height,
                          uint32_t block_width, uint32_t block_height,
                          std::vector<uint8_t>& encoded);

