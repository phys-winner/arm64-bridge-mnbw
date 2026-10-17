#pragma once

namespace bridge_gl {

// GLES initializes active uniforms to zero. The legacy mobile backend does not
// reliably publish the HLSL effect defaults or its derived sampling constants,
// so keep the original postFX values explicit for every driver. Sampling
// constants are based on the source texture dimensions, which can differ from
// the viewport during downsample and blur passes.
struct PostFxUniformDefaults {
    bool valid_viewport = false;
    float half_pixel_x = 0.0f;
    float half_pixel_y = 0.0f;
    float inverse_width = 0.0f;
    float inverse_height = 0.0f;
    float hdr_frame_time = 1.0f;
    float dof_focus = -0.005f;
    float dof_range = 5.19876f;
};

constexpr PostFxUniformDefaults MakePostFxUniformDefaults(
        int source_width, int source_height) noexcept {
    PostFxUniformDefaults values;
    if (source_width <= 0 || source_height <= 0) return values;

    values.valid_viewport = true;
    values.inverse_width = 1.0f / static_cast<float>(source_width);
    values.inverse_height = 1.0f / static_cast<float>(source_height);
    values.half_pixel_x = values.inverse_width * 0.5f;
    values.half_pixel_y = values.inverse_height * 0.5f;
    return values;
}

}  // namespace bridge_gl
