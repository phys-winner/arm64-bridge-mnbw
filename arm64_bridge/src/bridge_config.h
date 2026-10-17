#pragma once
#include <cstdint>

// Host-side knobs for the bridge itself. The GOF2 reference kept its engine tweaks
// here too; those are gone - this is only what the emulation core reads.
struct BridgeConfig {
    int fastmem = 1;      // 1: JIT fastmem (direct guest loads/stores), 0: memory callbacks
    int forced_dpi = 0;   // 0: report the real display DPI to the guest
    int ui_inset = 0;     // safe-area side padding in pixels, 0 = off
    int target_fps = 60;
};

BridgeConfig& GetBridgeConfig();

// Resolution the guest engine is told about. The reference bridge could report a
// DPI-scaled virtual size here and stretch it back at the viewport; this build
// hands over the real surface size unchanged.
uint32_t GetVirtualWidth();
uint32_t GetVirtualHeight();
