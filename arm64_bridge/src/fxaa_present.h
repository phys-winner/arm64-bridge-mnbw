#pragma once

#include <EGL/egl.h>

namespace bridge_gl {

// Written by the settings bridge; read on the rendering thread.
void SetFxaaEnabled(bool enabled);

// Smooth the completed frame, including scenes that bypass the game's PostFX.
void ApplyFxaaBeforeSwap(EGLDisplay display, EGLSurface surface);

}  // namespace bridge_gl
