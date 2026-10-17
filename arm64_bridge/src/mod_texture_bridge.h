#pragma once

#include <cstddef>
#include <cstdint>
#include <sys/types.h>
#include <vector>

// Converts an external desktop DDS (DXT1/3/5 or 32-bit BGRA/BGRX) into the
// ASTC-sized stream expected by the patched guest. The stream contains
// authenticated bridge tokens; pixel data is decoded lazily when GLES receives
// each mip. `handled` distinguishes an unsupported file from a recognized but
// malformed DDS that must fail closed.
int OpenModDdsVirtualized(const char* path, off_t* out_size, bool* handled);

// Computes the size of that virtual stream without allocating tokens, a memfd,
// or decoded pixels. Directory/texture discovery calls stat thousands of times;
// keeping it side-effect free prevents token growth and unnecessary I/O.
bool StatModDdsVirtualized(const char* path, off_t* out_size, bool* handled);

// Resolves and decodes one authenticated mip token. Returns false for ordinary
// packaged ASTC/ETC payloads so their established path is unchanged.
bool DecodeModTextureToken(const void* data, size_t size, int width, int height,
                           std::vector<uint32_t>& rgba, bool& has_alpha);

// Resolves a stored converted mip without encoding it. 0 means an ordinary
// payload/raw DDS token, 1 is a verified converted mip, -1 is a converted token
// whose backing file failed validation and must never be sent to the GPU.
int LoadConvertedModTextureToken(const void* data, size_t size, int width, int height,
                                 std::vector<uint8_t>& payload, uint32_t& format,
                                 bool& has_alpha);
