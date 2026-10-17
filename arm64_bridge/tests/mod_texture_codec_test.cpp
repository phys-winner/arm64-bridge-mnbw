#include "mod_texture_codec.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

int main() {
    const auto token = mod_texture::MakeToken(17u);
    uint32_t token_id = 0;
    if (!mod_texture::ReadToken(&token, sizeof(token), token_id)) return 1;
    if (token_id != 17u) return 2;
    auto broken_token = token;
    broken_token.guard ^= 1u;
    if (mod_texture::ReadToken(&broken_token, sizeof(broken_token), token_id)) return 3;

    std::array<uint8_t, 8> dxt1_red{
        0x00, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    std::vector<uint8_t> rgba;
    bool alpha = true;
    if (!mod_texture::DecodeDxt(mod_texture::DxtFormat::kDxt1,
                                dxt1_red.data(), dxt1_red.size(),
                                4u, 4u, rgba, alpha)) return 4;
    if (alpha || rgba.size() != 64u) return 5;
    for (size_t pixel = 0; pixel < 16u; ++pixel) {
        if (rgba[pixel * 4u + 0] != 255u ||
            rgba[pixel * 4u + 1] != 0u ||
            rgba[pixel * 4u + 2] != 0u ||
            rgba[pixel * 4u + 3] != 255u) return 6;
    }

    std::array<uint8_t, 8> dxt1_transparent{
        0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
    };
    if (!mod_texture::DecodeDxt(mod_texture::DxtFormat::kDxt1,
                                dxt1_transparent.data(), dxt1_transparent.size(),
                                3u, 2u, rgba, alpha)) return 7;
    if (!alpha || rgba.size() != 24u) return 8;
    for (size_t pixel = 0; pixel < 6u; ++pixel) {
        if (rgba[pixel * 4u + 3] != 0u) return 9;
    }

    std::array<uint8_t, 16> dxt3_blue{};
    std::memset(dxt3_blue.data(), 0x88, 8u);
    dxt3_blue[8] = 0x1f;
    dxt3_blue[9] = 0x00;
    if (!mod_texture::DecodeDxt(mod_texture::DxtFormat::kDxt3,
                                dxt3_blue.data(), dxt3_blue.size(),
                                4u, 4u, rgba, alpha)) return 10;
    if (!alpha) return 11;
    if (rgba[0] != 0u || rgba[1] != 0u || rgba[2] != 255u || rgba[3] != 136u) return 12;

    std::array<uint8_t, 16> dxt5_green{};
    dxt5_green[0] = 255u;
    dxt5_green[1] = 0u;
    dxt5_green[8] = 0xe0;
    dxt5_green[9] = 0x07;
    if (!mod_texture::DecodeDxt(mod_texture::DxtFormat::kDxt5,
                                dxt5_green.data(), dxt5_green.size(),
                                4u, 4u, rgba, alpha)) return 13;
    if (alpha) return 14;
    if (rgba[0] != 0u || rgba[1] != 255u || rgba[2] != 0u || rgba[3] != 255u) return 15;

    if (mod_texture::DecodeDxt(mod_texture::DxtFormat::kDxt1,
                               dxt1_red.data(), dxt1_red.size() - 1u,
                               4u, 4u, rgba, alpha)) return 16;

    std::array<uint8_t, 8> bgra{
        0x30, 0x20, 0x10, 0xff,
        0x03, 0x02, 0x01, 0x80,
    };
    if (!mod_texture::DecodeDxt(mod_texture::DxtFormat::kBgra8,
                                bgra.data(), bgra.size(),
                                2u, 1u, rgba, alpha)) return 17;
    if (!alpha || rgba.size() != 8u) return 18;
    const std::array<uint8_t, 8> expected_rgba{
        0x10, 0x20, 0x30, 0xff,
        0x01, 0x02, 0x03, 0x80,
    };
    if (!std::equal(rgba.begin(), rgba.end(), expected_rgba.begin())) return 19;
    if (mod_texture::DxtBytes(mod_texture::DxtFormat::kBgra8, 2u, 1u) != 8u) {
        return 20;
    }
    if (!mod_texture::DecodeDxt(mod_texture::DxtFormat::kBgrx8,
                                bgra.data(), bgra.size(),
                                2u, 1u, rgba, alpha)) return 21;
    if (alpha || rgba[3] != 255u || rgba[7] != 255u) return 22;
    if (mod_texture::VirtualAstcBytes(mod_texture::DxtFormat::kBgra8,
                                      1024u, 512u) != 524288u ||
        mod_texture::VirtualAstcLastRowOffset(mod_texture::DxtFormat::kBgra8,
                                              1024u, 512u) != 520192u) {
        return 23;
    }
    if (mod_texture::VirtualAstcBytes(mod_texture::DxtFormat::kDxt1,
                                      1024u, 512u) != 235296u ||
        mod_texture::VirtualAstcLastRowOffset(mod_texture::DxtFormat::kDxt1,
                                              1024u, 512u) != 232560u) {
        return 24;
    }
    if (mod_texture::VirtualAstcLastRowOffset(mod_texture::DxtFormat::kDxt5,
                                              4u, 4u) != 0u) {
        return 25;
    }
    return 0;
}
