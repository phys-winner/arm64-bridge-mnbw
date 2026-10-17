#pragma once

#include <cstdint>

namespace bridge_gl {

// EGL handles are opaque pointers and drivers may recycle their values.  A
// process-wide generation lets optional shader diagnostics distinguish a new
// context even when eglGetCurrentContext() returns an address seen before.
uint64_t ShaderContextGeneration() noexcept;
void NotifyShaderContextLifecycle() noexcept;

}  // namespace bridge_gl

