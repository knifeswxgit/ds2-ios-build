#!/usr/bin/env bash
# Runs rexglue codegen on the DS2 manifest and records wall time and peak memory. Not run automatically.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p out/logs
/usr/bin/time -v -o out/logs/codegen_time.txt \
  sdk/out/linux-amd64/rexglue --log-file out/logs/codegen.log codegen app/ds2_manifest.toml \
  2>&1 | tee out/logs/runner.out
grep -E "Maximum resident|Elapsed" out/logs/codegen_time.txt
