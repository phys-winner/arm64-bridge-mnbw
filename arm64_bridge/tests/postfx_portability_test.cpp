#include "../src/postfx_portability.h"

constexpr auto kSquare = bridge_gl::MakePostFxUniformDefaults(64, 64);
static_assert(kSquare.valid_viewport);
static_assert(kSquare.half_pixel_x == 0.0078125f);
static_assert(kSquare.half_pixel_y == 0.0078125f);
static_assert(kSquare.inverse_width == 0.015625f);
static_assert(kSquare.inverse_height == 0.015625f);
static_assert(kSquare.hdr_frame_time == 1.0f);
static_assert(kSquare.dof_focus == -0.005f);
static_assert(kSquare.dof_range == 5.19876f);

constexpr auto kRectangle = bridge_gl::MakePostFxUniformDefaults(32, 16);
static_assert(kRectangle.valid_viewport);
static_assert(kRectangle.half_pixel_x == 0.015625f);
static_assert(kRectangle.half_pixel_y == 0.03125f);
static_assert(kRectangle.inverse_width == 0.03125f);
static_assert(kRectangle.inverse_height == 0.0625f);

static_assert(!bridge_gl::MakePostFxUniformDefaults(0, 64).valid_viewport);
static_assert(!bridge_gl::MakePostFxUniformDefaults(64, 0).valid_viewport);
static_assert(!bridge_gl::MakePostFxUniformDefaults(-1, 64).valid_viewport);

int main() {
    return 0;
}
