#!/usr/bin/env bash
# Rebuild Mesa NVK for Switch and install it as the SDK the app links against.
# Usage: tools/build/build-mesa.sh <sdk-name>   (e.g. mesa-sdk-head-ds2-v38; then set DS2_MESA_SDK to it)
set -euo pipefail
. "$(dirname "$0")/env.sh"
NAME=${1:?sdk name}
DEST=$(dirname "$DS2_MESA_SDK")/$NAME
ninja -C "$DS2_MESA_SRC/builddir-switch"
meson install -C "$DS2_MESA_SRC/builddir-switch" --destdir "$DEST" --no-rebuild
# The static Rust/NVK pieces must sit next to libvulkan.a in portlibs.
cp "$DS2_MESA_SRC"/builddir-switch/src/nouveau/vulkan/libnvk.a "$DEST/opt/devkitpro/portlibs/switch/lib/" 2>/dev/null || true
cp "$DS2_MESA_SRC"/builddir-switch/src/nouveau/compiler/libnak_rs.a "$DEST/opt/devkitpro/portlibs/switch/lib/" 2>/dev/null || true
echo "SDK: $DEST/opt/devkitpro/portlibs/switch (libvulkan.a must be ~48 MB, not ~2 MB)"
ls -la "$DEST/opt/devkitpro/portlibs/switch/lib/libvulkan.a"
# Export the patch after committing in the Mesa tree:
#   git -C $DS2_MESA_SRC format-patch --stdout 0ff52079ca9e > tools/switch/toolchain/mesa-switch-ds2-perf.patch
