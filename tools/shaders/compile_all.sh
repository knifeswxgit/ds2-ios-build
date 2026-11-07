#!/bin/bash
f="$1"; o="${f%.hlsl}.spv"
case "$f" in *.fpo.hlsl) T="-T ps_6_0";; *) T="-T vs_6_0 -fvk-invert-y";; esac
if LD_LIBRARY_PATH=/home/claude/renderbag/dxc-bin/lib/x64 /home/claude/renderbag/dxc-bin/bin/x64/dxc-linux $T -E main -HV 2021 -all-resources-bound -spirv -fvk-use-dx-layout -Qstrip_debug -Fo "$o" "$f" > "${f%.hlsl}.log" 2>&1; then echo OK; else echo FAIL; fi
