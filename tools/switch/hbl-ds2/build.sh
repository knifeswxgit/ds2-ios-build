#!/usr/bin/env bash
# Builds hbl_ds2.nsp: switchbrew/nx-hbloader (tag v2.4.5) with one change in hbl.json, system_resource_size.
# Under hbloader the game process otherwise has no system resource and takes its kernel page tables from the heap
# Horizon shares with every system process (see docs/handoff/11-switch-level-load-diag.md, test 19).
# Usage: tools/switch/hbl-ds2/build.sh <work dir> [size, default 0x4000000, "none" to skip] [highest cpu, default 2]
# highest cpu 3 opens the fourth core to the process (mask 0xF instead of 0x7); every thread that
# keeps the process mask can then run there.
set -euo pipefail
W=${1:?work dir}; SIZE=${2:-0x4000000}; CPU=${3:-2}
export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
export PATH=$DEVKITPRO/devkitA64/bin:$DEVKITPRO/tools/bin:$PATH
[ -d "$W/nx-hbloader" ] || git clone --branch v2.4.5 --depth 1 https://github.com/switchbrew/nx-hbloader.git "$W/nx-hbloader"
cd "$W/nx-hbloader"
git checkout -- hbl.json
python3 - "$SIZE" "$CPU" <<'PY'
import sys
s = open('hbl.json').read()
key = '"main_thread_stack_size": "0x100000",'
assert key in s and 'system_resource_size' not in s
if sys.argv[1] != 'none':
    s = s.replace(key, key + '\n    "system_resource_size": "%s",' % sys.argv[1], 1)
assert '"highest_cpu_id": 2' in s
s = s.replace('"highest_cpu_id": 2', '"highest_cpu_id": %d' % int(sys.argv[2]), 1)
open('hbl.json', 'w').write(s)
PY
make clean >/dev/null; make -j4
python3 -c "import struct;d=open('hbl.npdm','rb').read();print('NPDM system_resource_size =',hex(struct.unpack_from('<I',d,0x14)[0]))"
cp hbl.nsp hbl_ds2.nsp; sha256sum hbl_ds2.nsp
