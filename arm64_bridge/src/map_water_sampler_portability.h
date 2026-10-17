#pragma once

namespace bridge_gl {

enum class MapWaterSamplerRole {
    Other,
    Diffuse,
    Normal,
    Environment,
};

// The original renderer explicitly binds the campaign-water diffuse and normal
// stages, but leaves env_texture at GLSL's default unit zero. On modern drivers
// unit zero contains unrelated material/UI state by the time water is drawn.
// The reflection render target is produced on stage five.
constexpr int PortableMapWaterSamplerUnit(MapWaterSamplerRole role,
                                          int requested_unit) noexcept {
    switch (role) {
        case MapWaterSamplerRole::Diffuse:
            return 9;
        case MapWaterSamplerRole::Normal:
            return 3;
        case MapWaterSamplerRole::Environment:
            return 5;
        case MapWaterSamplerRole::Other:
            return requested_unit;
    }
    return requested_unit;
}

}  // namespace bridge_gl
