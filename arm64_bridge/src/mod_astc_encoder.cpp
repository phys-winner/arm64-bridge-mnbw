#include "mod_astc_encoder.h"

#include <astcenc.h>

#include <array>
#include <limits>
#include <memory>
#include <mutex>

namespace {

struct AstcContextDeleter {
    void operator()(astcenc_context* context) const {
        if (context) astcenc_context_free(context);
    }
};

struct EncoderState {
    std::mutex mutex;
    std::unique_ptr<astcenc_context, AstcContextDeleter> context_4x4;
    std::unique_ptr<astcenc_context, AstcContextDeleter> context_6x6;
};

EncoderState& State() {
    static EncoderState state;
    return state;
}

astcenc_context* GetContext(EncoderState& state, uint32_t block_width,
                            uint32_t block_height) {
    if (block_width != block_height ||
        (block_width != 4u && block_width != 6u)) {
        return nullptr;
    }
    auto& slot = block_width == 4u ? state.context_4x4 : state.context_6x6;
    if (slot) return slot.get();

    astcenc_config config{};
    const unsigned flags = ASTCENC_FLG_USE_ALPHA_WEIGHT |
        ASTCENC_FLG_USE_DECODE_UNORM8;
    if (astcenc_config_init(ASTCENC_PRF_LDR, block_width, block_height, 1u,
                            ASTCENC_PRE_FAST, flags, &config) != ASTCENC_SUCCESS) {
        return nullptr;
    }
    astcenc_context* context = nullptr;
    if (astcenc_context_alloc(&config, 1u, &context, nullptr) != ASTCENC_SUCCESS) {
        return nullptr;
    }
    slot.reset(context);
    return slot.get();
}

}  // namespace

bool EncodeModTextureAstc(const uint8_t* rgba, uint32_t width, uint32_t height,
                          uint32_t block_width, uint32_t block_height,
                          std::vector<uint8_t>& encoded) {
    encoded.clear();
    if (!rgba || width == 0u || height == 0u ||
        block_width == 0u || block_height == 0u) {
        return false;
    }
    const size_t blocks_x = (static_cast<size_t>(width) + block_width - 1u) /
        block_width;
    const size_t blocks_y = (static_cast<size_t>(height) + block_height - 1u) /
        block_height;
    if (blocks_x > std::numeric_limits<size_t>::max() / blocks_y ||
        blocks_x * blocks_y > std::numeric_limits<size_t>::max() / 16u) {
        return false;
    }
    encoded.resize(blocks_x * blocks_y * 16u);

    EncoderState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    astcenc_context* context = GetContext(state, block_width, block_height);
    if (!context) {
        encoded.clear();
        return false;
    }

    void* slices[] = {const_cast<uint8_t*>(rgba)};
    astcenc_image image{};
    image.dim_x = width;
    image.dim_y = height;
    image.dim_z = 1u;
    image.data_type = ASTCENC_TYPE_U8;
    image.data = slices;
    const astcenc_swizzle swizzle{
        ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
    const astcenc_error status = astcenc_compress_image(
        context, &image, &swizzle, encoded.data(), encoded.size(), 0u);
    const astcenc_error reset = astcenc_compress_reset(context);
    if (status != ASTCENC_SUCCESS || reset != ASTCENC_SUCCESS) {
        encoded.clear();
        return false;
    }
    return true;
}

