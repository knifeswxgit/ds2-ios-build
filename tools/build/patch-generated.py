#!/usr/bin/env python3
"""Bring already generated code up to the current codegen output without re-running codegen (hours).

- REX_FUNC_COUNT(addr) after REX_FUNC_PROLOGUE() in every sub_XXXXXXXX (sdk/src/codegen/function_graph.cpp emits it
  since v41). Idempotent: files already patched are left untouched (their timestamps too, so nothing rebuilds).
Usage: tools/build/patch-generated.py app/generated/diag-dataonly
"""
import os, re, sys

d = sys.argv[1]
pat = re.compile(r'(DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\) \{\n\tREX_FUNC_PROLOGUE\(\);)\n')
changed = 0
for name in sorted(os.listdir(d)):
    if not name.endswith('.cpp'):
        continue
    p = os.path.join(d, name)
    s = open(p).read()
    s2, n = pat.subn(lambda m: m.group(1) + ' REX_FUNC_COUNT(0x%su);\n' % m.group(2), s)
    if n:
        open(p, 'w').write(s2)
        changed += 1
print('patch-generated: %d files updated' % changed)
