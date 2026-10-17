#include "map_water_sampler_portability.h"

#include <cassert>

int main() {
    using bridge_gl::MapWaterSamplerRole;
    using bridge_gl::PortableMapWaterSamplerUnit;

    assert(PortableMapWaterSamplerUnit(MapWaterSamplerRole::Diffuse, 0) == 9);
    assert(PortableMapWaterSamplerUnit(MapWaterSamplerRole::Normal, 0) == 3);
    assert(PortableMapWaterSamplerUnit(MapWaterSamplerRole::Environment, 0) == 5);
    assert(PortableMapWaterSamplerUnit(MapWaterSamplerRole::Other, 7) == 7);
    return 0;
}
