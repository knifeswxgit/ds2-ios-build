#!/usr/bin/env bash
# Package version NN: out/app-switch/releases/vNN/{ds2.nro,ds2.elf,ds2.toml[,ds2.toml.B],SHA256SUMS}
# and 14 MB parts in /mnt/user-data/outputs/vNN for delivery to the Mac (join with `cat part_* > ds2.nro`).
# The TOML comes from docs/handoff/15-ds2.toml.vNN (and 15-ds2.toml.vNNB if present).
set -euo pipefail
. "$(dirname "$0")/env.sh"
V=${1:?version number, e.g. 38}
R="$DS2_ROOT/out/app-switch/releases/v$V"
[ -e "$R" ] && { echo "$R exists: earlier versions are never overwritten"; exit 1; }
mkdir -p "$R"
cp "$DS2_ROOT/out/app-switch/ds2.nro" "$R/"
cp "$DS2_ROOT/out/app-switch/ds2" "$R/ds2.elf"
cp "$DS2_ROOT/docs/handoff/15-ds2.toml.v$V" "$R/ds2.toml"
[ -f "$DS2_ROOT/docs/handoff/15-ds2.toml.v${V}B" ] && cp "$DS2_ROOT/docs/handoff/15-ds2.toml.v${V}B" "$R/ds2.toml.B"
(cd "$R" && sha256sum ds2.nro ds2.toml* ds2.elf > SHA256SUMS && cat SHA256SUMS)
O=/mnt/user-data/outputs/v$V
mkdir -p "$O"
split -b 14680064 -d -a 2 "$R/ds2.nro" "$O/part_"
cp "$R"/ds2.toml* "$O/"
echo "Parts in $O. On the Mac: cat part_* > ds2.nro; shasum -a 256 ds2.nro"
