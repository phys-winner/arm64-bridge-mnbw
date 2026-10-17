#pragma once

#include "guest_settings_protocol.h"
#include <bit>

namespace warband::report_text {

// Pristine 1.071 post_message_line constructs the glyph-size Vec2 here.
// Its recomputed bounding-box height also supplies native report spacing.
// ponytail: queued reports retain their size for <=10 s; regenerate meshes if
// immediate resizing of existing reports becomes a requirement.
inline constexpr uint32_t kPostMessageLineVa = 0x0021ADA0u;
inline constexpr uint32_t kScaleMovwVa = 0x0021B01Cu;
inline constexpr uint32_t kScaleMovtVa = 0x0021B024u;
inline constexpr uint32_t kAuthoredScaleBits = 0x3CD0E560u;
inline constexpr std::array<uint32_t, 2> kOriginalScaleWords = {
    0xE30E2560u, 0xE3432CD0u,
};

struct Instruction {
    uint32_t va;
    uint32_t word;
};
inline constexpr std::array<Instruction, 6> kContextPreimage = {{
    {0x0021B018u, 0xE30C0CCDu},  // independent wrap width 0.8
    {0x0021B020u, 0xE3430F4Cu},
    {0x0021B040u, 0xE58D2088u},  // glyph scale X
    {0x0021B048u, 0xE58D208Cu},  // glyph scale Y
    {0x0021B0C4u, 0xE58D4000u},  // Vec2 argument on caller stack
    {0x0021B0E0u, 0xEBFEFB25u},  // draw_text_with_image
}};

inline std::array<uint32_t, 2> ScaleWords(float scale) {
    constexpr float authored = std::bit_cast<float>(kAuthoredScaleBits);
    const float glyph_scale = authored * guest_settings::BoundHostFloat(
        GST_FLOAT_REPORT_TEXT_SCALE, scale);
    const uint32_t bits = std::bit_cast<uint32_t>(glyph_scale);
    // ARM MOVW/MOVT r2 preserve every neighboring input to the font factory.
    return {0xE3002000u | ((bits & 0xF000u) << 4) | (bits & 0xFFFu),
            0xE3402000u | ((bits >> 12) & 0xF0000u) | ((bits >> 16) & 0xFFFu)};
}

}  // namespace warband::report_text
