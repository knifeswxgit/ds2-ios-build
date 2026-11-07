#!/usr/bin/env bash
# Configure (first time) and build the Switch NRO: out/app-switch/ds2.nro (+ unstripped ELF out/app-switch/ds2).
set -euo pipefail
. "$(dirname "$0")/env.sh"
cd "$DS2_ROOT"
if [ ! -f out/app-switch/CMakeCache.txt ]; then
  cmake -S app -B out/app-switch -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$DS2_ROOT/tools/switch/cmake/switch-devkitA64.cmake" \
    -DREXGLUE_SWITCH_NVK_SDK="$DS2_MESA_SDK/opt/devkitpro/portlibs/switch"
fi
# -j2: more parallel jobs run out of memory on the 7 GB container (cc1plus killed).
python3 "$DS2_ROOT/tools/build/patch-generated.py" app/generated/diag-dataonly
cmake --build out/app-switch --target ds2 -j"${JOBS:-2}"
ls -la out/app-switch/ds2.nro
