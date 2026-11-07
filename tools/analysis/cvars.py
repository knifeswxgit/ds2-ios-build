#!/usr/bin/env python3
# DS2 Switch tool: List cvars defined/used in the SDK and app (source of docs/handoff/17a-cvars-list.md). Usage: cvars.py <repo root>
import re, os, sys, json
root = sys.argv[1]
defs = {}
uses = {}
rx_def = re.compile(r'REXCVAR_DEFINE_(BOOL|INT32|INT64|UINT32|UINT64|DOUBLE|STRING|COMMAND|COMMAND_ARGS)\s*\(\s*([A-Za-z0-9_]+)\s*,(.*?)\)\s*(\.[^;]*)?;', re.S)
rx_use = re.compile(r'REXCVAR_(GET|SET|QUERY|DECLARE)\s*\(\s*(?:[A-Za-z0-9_:<> ]+,\s*)?([A-Za-z0-9_]+)\s*\)')
for d in ['sdk/src', 'sdk/include', 'app']:
    for dp, dn, fn in os.walk(os.path.join(root, d)):
        if 'thirdparty' in dp: continue
        for f in fn:
            if not f.endswith(('.cpp', '.h', '.cc', '.inc')): continue
            p = os.path.join(dp, f)
            try: s = open(p, encoding='utf-8', errors='replace').read()
            except: continue
            rel = os.path.relpath(p, root)
            for m in rx_def.finditer(s):
                typ, name, rest, chain = m.group(1), m.group(2), m.group(3), m.group(4) or ''
                strs = re.findall(r'"((?:[^"\\]|\\.)*)"', rest)
                parts = rest.split(',')
                default = parts[0].strip() if parts else ''
                ci = 1 if typ == 'STRING' and default.startswith('"') else 0
                if typ.startswith('COMMAND'): ci = 0
                cat = strs[ci] if len(strs) > ci else ''
                desc = ''.join(strs[ci+1:])
                lc = 'live'
                if 'kInitOnly' in chain: lc = 'init-only'
                elif 'kRequiresRestart' in chain: lc = 'restart'
                defs[name] = dict(type=typ, default=default, cat=cat, desc=desc[:140], lc=lc, file=rel)
            for m in rx_use.finditer(s):
                if m.group(1) == 'DECLARE': continue
                uses.setdefault(m.group(2), set()).add(rel)
out = []
for n, d in sorted(defs.items(), key=lambda kv: (kv[1]['cat'], kv[0])):
    d['uses'] = sorted(uses.get(n, []))
    out.append(dict(name=n, **d))
json.dump(out, open(sys.argv[2], 'w'), ensure_ascii=False, indent=1)
print(len(out))
from collections import Counter
print(Counter(o['cat'] for o in out).most_common(60))
