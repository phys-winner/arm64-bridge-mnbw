#include "../src/mod_astc_encoder.h"

#include "astc.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace {

void Check(bool condition) {
    if (!condition) std::abort();
}

void CheckFootprint(uint32_t block, uint32_t width, uint32_t height) {
    std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4u);
    for (size_t pixel = 0; pixel < rgba.size() / 4u; ++pixel) {
        rgba[pixel * 4u + 0u] = static_cast<uint8_t>(24u + pixel % 17u);
        rgba[pixel * 4u + 1u] = static_cast<uint8_t>(80u + pixel % 31u);
        rgba[pixel * 4u + 2u] = static_cast<uint8_t>(180u - pixel % 23u);
        rgba[pixel * 4u + 3u] = static_cast<uint8_t>(96u + pixel % 127u);
    }

    std::vector<uint8_t> encoded;
    Check(EncodeModTextureAstc(rgba.data(), width, height, block, block, encoded));
    const size_t expected = ((static_cast<size_t>(width) + block - 1u) / block) *
        ((static_cast<size_t>(height) + block - 1u) / block) * 16u;
    Check(encoded.size() == expected);
    Check(std::any_of(encoded.begin(), encoded.end(), [](uint8_t byte) {
        return byte != 0u;
    }));

    std::vector<uint32_t> decoded(static_cast<size_t>(width) * height);
    Check(decode_astc(encoded.data(), static_cast<long>(width),
                     static_cast<long>(height), static_cast<int>(block),
                     static_cast<int>(block), decoded.data()) != 0);
}

}  // namespace

int main() {
    CheckFootprint(4u, 13u, 11u);
    CheckFootprint(6u, 17u, 13u);
    std::vector<uint8_t> output;
    Check(!EncodeModTextureAstc(nullptr, 4u, 4u, 4u, 4u, output));
    Check(!EncodeModTextureAstc(reinterpret_cast<const uint8_t*>("x"),
                               4u, 4u, 5u, 5u, output));
    return 0;
}
