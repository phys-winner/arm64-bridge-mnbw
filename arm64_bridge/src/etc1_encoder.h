#pragma once

#include <cstdint>
#include <vector>

namespace bridge_gl {

// Encode an opaque RGBA8 image into the raw ETC1 block stream expected by
// GL_ETC1_RGB8_OES. The input and output use the same row order as a GLES
// glTexImage2D upload. ETC1 cannot represent alpha; callers must provide an
// image whose alpha channel is 255 for every pixel.
bool EncodeEtc1(const uint8_t* rgba, int width, int height,
                std::vector<uint8_t>& encoded);

}  // namespace bridge_gl
