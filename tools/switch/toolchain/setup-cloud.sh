#!/usr/bin/env bash
# Full Switch build environment in a fresh Ubuntu 24.04 cloud container (no devkitPro servers, no rustup).
# Reproduces the toolchain of RC58-RC62: devkitA64 r30 (GCC 16.1.0), libnx 4.12.0, zlib/zstd/expat,
# Mesa NVK = mesa-switch 0ff52079ca + mesa-switch-ds2-perf.patch (Mesa 26.2.4, "mesa-sdk-ds2-rc58").
# Reachable sources used: archive.ubuntu.com, git over github.com, crates.io sparse index, PyPI.
# Usage: WORK=/home/claude/swtc tools/switch/toolchain/setup-cloud.sh   (~1.5 h on 2 cores)
# Then: generated code (app/generated/<name>, from Default.xex codegen, not in git) and
#       python3 tools/fetch_thirdparty.py, then the cmake configure/build in docs (RC-ver0NN-context / HANDOFF).
set -euo pipefail
WORK=${WORK:?set WORK}; export WORK JOBS=${JOBS:-2}
HERE=$(cd "$(dirname "$0")" && pwd)
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq build-essential libgmp-dev libmpfr-dev libmpc-dev \
  libisl-dev texinfo flex bison liblz4-dev autoconf automake libtool rustc-1.91 cargo-1.91 rust-1.91-src \
  llvm-18-dev libclang-18-dev clang-18 libclc-18-dev libllvmspirvlib-18-dev spirv-tools glslang-tools \
  python3-mako ninja-build pkg-config xz-utils zstd
pip install --break-system-packages -q meson mako pyyaml packaging
export PATH=/usr/lib/rust-1.91/bin:$PATH
[ -x /opt/rusttools/bin/bindgen ] || cargo install --root /opt/rusttools bindgen-cli --version ">=0.71.1" --locked
[ -x /opt/rusttools/bin/cbindgen ] || cargo install --root /opt/rusttools cbindgen --version ">=0.28" --locked
"$HERE/fetch-sources.sh"
M=$WORK/src/mesa-switch-head
if [ ! -d "$M" ]; then
  git -C "$WORK/src/mesa-switch" worktree add -q ../mesa-switch-head 0ff52079ca9
  git -C "$M" -c user.email=build@local -c user.name=build am -q "$HERE/mesa-switch-ds2-perf.patch"
fi
"$HERE/build-devkita64.sh"
"$HERE/build-libnx-tools.sh"
"$HERE/build-portlibs.sh"
MESA_DIR=$M NFSMW_PATCH=none SDK_OUT=$WORK/mesa-sdk-ds2-rc58 "$HERE/build-mesa-nvk.sh"
ls -la "$WORK/mesa-sdk-ds2-rc58/opt/devkitpro/portlibs/switch/lib/libvulkan.a"
echo "SETUP OK: -DREXGLUE_SWITCH_NVK_SDK=$WORK/mesa-sdk-ds2-rc58/opt/devkitpro/portlibs/switch"
echo "Note: delete local git tags (v0.55, v0.56, v0.62) before configuring: sdk/cmake/rex_version.cmake rejects"
echo "      a tag above the SDK floor 0.10 (git tag -d \$(git tag)); they stay on GitHub."
