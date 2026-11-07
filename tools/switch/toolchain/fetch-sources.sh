#!/usr/bin/env bash
# Fetches every source the Switch toolchain needs. See README.md.
set -euo pipefail
WORK=${WORK:?set WORK}
mkdir -p "$WORK/src" && cd "$WORK/src"
U=http://archive.ubuntu.com/ubuntu/pool
[ -f binutils_2.46.orig.tar.xz ] || curl -fLO $U/main/b/binutils/binutils_2.46.orig.tar.xz
[ -f newlib_4.6.0.20260123.orig.tar.xz ] || curl -fLO $U/universe/n/newlib/newlib_4.6.0.20260123.orig.tar.xz
clone() { [ -d "$2" ] || git clone -q ${3:+--depth 1 --branch $3} "https://github.com/$1" "$2"; }
clone gcc-mirror/gcc gcc-16.1.0 releases/gcc-16.1.0
clone devkitPro/buildscripts buildscripts
clone devkitPro/devkita64-rules devkita64-rules
clone devkitPro/general-tools general-tools
clone devkitPro/pacman-packages pacman-packages
clone switchbrew/libnx libnx v4.12.0
clone switchbrew/switch-tools switch-tools
clone madler/zlib zlib v1.3.1
clone facebook/zstd zstd v1.5.7
clone libexpat/libexpat libexpat R_2_7_1
if [ ! -d mesa-switch ]; then
  git clone -q https://github.com/danfromtico/mesa-switch.git && git -C mesa-switch checkout -q 1a8c1a66d6f
fi
echo FETCH OK
