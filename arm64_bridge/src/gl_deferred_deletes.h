#pragma once

namespace bridge_gl {

// The ARM32 engine can retire GL objects from a guest worker thread.  EGL
// contexts are host-thread-local, so forwarding glDelete* from that thread is
// a silent no-op.  The render thread drains those commands as soon as its
// context is current again.
void DrainDeferredDeletes() noexcept;

// Object names belong to an EGL context/share group.  Never carry queued names
// across a successful context destruction where the driver may recycle them.
void DiscardDeferredDeletes() noexcept;

}  // namespace bridge_gl
