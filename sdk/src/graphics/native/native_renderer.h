// DS2 port: experimental native renderer (v51).
//
// A second graphics system next to the Xenos emulation ("xenos"), selected with ds2_native_renderer = true.
// It keeps the SDK's PM4 command processor (ring, registers, waits, interrupts) and the SDK's Xenos shader
// translator (microcode -> SPIR-V), and replaces everything the emulation does with the EDRAM:
//   - render targets are ordinary Vulkan images per (EDRAM base, pitch, format), no ownership transfers;
//   - a resolve is an image copy into a native texture registered at the destination address; a texture fetch
//     of that address samples the native image, nothing goes through guest memory;
//   - vertex and index data of each draw are copied into a per-frame upload buffer (no 512 MB shared memory
//     buffer, no write watches);
//   - guest textures are untiled on the CPU into native images, cached by fetch constant and content hash.
// docs/handoff/18-native-renderer.md describes the design, what works and what is missing.

#pragma once

#include <memory>

#include <rex/system/interfaces/graphics.h>

namespace rex::graphics::native {

bool NativeRendererRequested();
rex::system::IGraphicsSystem* CreateNativeGraphicsSystem();

}  // namespace rex::graphics::native
