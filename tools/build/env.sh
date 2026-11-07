#!/usr/bin/env bash
# DS2 Switch: all paths and variables used by the build. Source it: `. tools/build/env.sh`.
# Override any value before sourcing (e.g. DS2_MESA_SDK=... . tools/build/env.sh).
export DS2_ROOT="${DS2_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
# Mesa (NVK for Switch): source tree, branch, and the installed SDK the app links against.
#   source: https://github.com/danfromtico/mesa-switch, base 0ff52079ca9e9e837ed27eb95a8d239f439a1864
#   + tools/switch/toolchain/mesa-switch-ds2-perf.patch (branch ds2-perf)
export DS2_MESA_SRC="${DS2_MESA_SRC:-/home/claude/swtc/src/mesa-switch-head}"
export DS2_MESA_SDK="${DS2_MESA_SDK:-/home/claude/swtc/mesa-sdk-head-ds2-v35}"
# Rust for NAK: only rustc 1.91 matches the prebuilt rlibs (another one gives E0514).
export MESA_SWITCH_RUSTC=/usr/lib/rust-1.91/bin/rustc
export MESA_SWITCH_RUST_TARGET=aarch64-unknown-linux-gnu
export MESA_SWITCH_BINDGEN=/opt/rusttools/bin/bindgen
export PATH=/opt/rusttools/bin:/usr/lib/rust-1.91/bin:$DEVKITPRO/devkitA64/bin:$PATH
# Game files for PC runs (never committed): copy of the owner's DS2 folder + DLC.
export GAME_ROOT="${GAME_ROOT:-$DS2_ROOT/out/run/game_root}"
export DS2_DLC="${DS2_DLC:-$DS2_ROOT/out/run/dlc/D4B91B6B4DA1509C280F56F77B09203DE7D39AE646}"
# DXC for shader research (tools/shaders).
export DS2_DXC="${DS2_DXC:-/home/claude/renderbag/dxc-bin}"
