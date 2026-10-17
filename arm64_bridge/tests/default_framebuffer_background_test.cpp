#include "default_framebuffer_background.h"

#include <cstdlib>
#include <iostream>

namespace {

void Check(bool value, const char* message) {
    if (!value) {
        std::cerr << "default framebuffer background test failed: " << message
                  << '\n';
        std::exit(1);
    }
}

}  // namespace

int main() {
    using namespace warband::default_framebuffer_background;

    Check(IsEligibleSurface(2400, 1080), "reported 2400x1080 surface is eligible");
    Check(IsEligibleSurface(2424, 1080), "test-device ultrawide is eligible");
    Check(IsEligibleSurface(2160, 1080), "2:1 boundary is eligible");
    Check(!IsEligibleSurface(1920, 1080), "ordinary 16:9 is untouched");
    Check(!IsEligibleSurface(1080, 2400), "portrait surface is untouched");
    Check(!IsEligibleSurface(0, 1080), "invalid surface is untouched");

    Check(!ShouldReplaceColourClear(7, 2400, 1080),
          "offscreen framebuffer is never painted");
    Check(!ShouldReplaceColourClear(0, 1920, 1080),
          "narrow default framebuffer is never painted");
    Check(ShouldReplaceColourClear(0, 2400, 1080),
          "eligible default framebuffer is painted");
    Check(ShouldReplaceColourClear(0, 2400, 1080),
          "a later colour clear in the frame is also replaced");

    Check(ShouldClearBeforeDefaultDraw(true, 0, 2400, 1080),
          "first wide default-framebuffer draw starts with a clean buffer");
    Check(!ShouldClearBeforeDefaultDraw(false, 0, 2400, 1080),
          "later draws in one frame do not clear the rendered scene");
    Check(!ShouldClearBeforeDefaultDraw(true, 4, 2400, 1080),
          "offscreen framebuffer is never cleared by the frame guard");
    Check(!ShouldClearBeforeDefaultDraw(true, 0, 1920, 1080),
          "ordinary 16:9 frame is not changed");

    std::cout << "default framebuffer background tests passed\n";
    return 0;
}
