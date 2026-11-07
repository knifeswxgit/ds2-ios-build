#!/usr/bin/env bash
# PC (Linux, lavapipe) build. The GPU backend is a separate plugin: librexgpu-xenos.so must be rebuilt too.
set -euo pipefail
. "$(dirname "$0")/env.sh"
cd "$DS2_ROOT"
if [ ! -f out/app-host/CMakeCache.txt ]; then
  cmake -S app -B out/app-host -G Ninja -DCMAKE_BUILD_TYPE=Release -DREXGLUE_ENABLE_PERF_COUNTERS=ON
fi
python3 "$DS2_ROOT/tools/build/patch-generated.py" app/generated/diag-dataonly
cmake --build out/app-host --target ds2 rexgpu-xenos -j"${JOBS:-2}"
ls -la sdk/out/linux-amd64/ds2 sdk/out/linux-amd64/librexgpu-xenos.so
