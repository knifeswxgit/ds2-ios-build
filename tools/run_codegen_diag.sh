#!/usr/bin/env bash
# Diagnostic codegen run with ${MANIFEST:-app/ds2_manifest.diag-overrides.toml}. Usage: tools/run_codegen_diag.sh <tag>
# Writes out/logs/diag/<tag>.{log,time.txt,inputs.sha256}; never overwrites an existing log.
set -u
cd "$(dirname "$0")/.."
T=${1:?tag}; D=out/logs/diag; mkdir -p "$D"
[ -e "$D/$T.log" ] && { echo "exists: $D/$T.log" >&2; exit 2; }
sha256sum app/ds2_manifest.toml ${MANIFEST:-app/ds2_manifest.diag-overrides.toml} config/ds2_overrides.proposed.toml \
  config/ds2_overrides.diag.toml config/ds2_overrides.dataonly.toml config/ds2_overrides.codeptr.toml assets/game_root/Default.xex sdk/out/linux-amd64/rexglue > "$D/$T.inputs.sha256"
s=$(date +%s)
C="sdk/out/linux-amd64/rexglue --log-level trace --log-file $D/$T.diag.log codegen ${MANIFEST:-app/ds2_manifest.diag-overrides.toml}"
echo "CMD: $C" > "$D/$T.log"
/usr/bin/time -v -o "$D/$T.time.txt" $C >> "$D/$T.log" 2>&1
echo "EXIT_CODE=$? SECONDS=$(( $(date +%s)-s ))" >> "$D/$T.log"
