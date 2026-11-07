#!/usr/bin/env python3
"""Finds which diagnostic CONFIG entries split real functions.

codegen emits `REX_FATAL("Unresolved branch|call from 0xS to 0xT")` when a branch/bl targets the middle of another
function (sdk/src/codegen/builders/context.cpp, emit_conditional_branch / calls). Compares the REX_FATAL sites of a
generated directory with a baseline directory and, for every new site, lists the declared entries that lie between
source and target (those entries cut the function that contains both). Read-only.

Usage: python3 tools/ds2_find_split_culprits.py <generated dir> <baseline generated dir> <overrides.toml>...
"""
import collections
import glob
import re
import sys
import tomllib

PAT = re.compile(r'REX_FATAL\("Unresolved (branch|call) from 0x([0-9A-F]{8}) to 0x([0-9A-F]{8})"')


def sites(directory):
    out = set()
    for f in glob.glob(directory + '/*.cpp'):
        for m in PAT.finditer(open(f, errors='replace').read()):
            out.add((m[1], int(m[2], 16), int(m[3], 16)))
    return out


def main(gen, base_dir, tomls):
    now, base = sites(gen), sites(base_dir)
    print(f'REX_FATAL sites: {len(now)} now, {len(base)} in baseline, {len(now - base)} new, {len(base - now)} gone')
    decl = {}
    for t in tomls:
        for k in tomllib.load(open(t, 'rb')).get('functions', {}):
            decl[int(k, 16)] = t
    ds = sorted(decl)
    culprit = collections.defaultdict(list)
    orphan = []
    for kind, s, t in sorted(now - base):
        lo, hi = min(s, t), max(s, t)
        c = [d for d in ds if lo < d <= hi]
        if not c:
            orphan.append((kind, s, t))
        for d in c:
            culprit[d].append((kind, s, t))
    print(f'entries that split a function: {len(culprit)}')
    for d, v in sorted(culprit.items()):
        print(f'  0x{d:08X} ({decl[d].split("/")[-1]}) {len(v)} site(s), e.g. {v[0][0]} 0x{v[0][1]:08X} -> 0x{v[0][2]:08X}')
    for kind, s, t in orphan:
        print(f'  new site without a declared entry in between: {kind} 0x{s:08X} -> 0x{t:08X}')
    print('baseline sites:', ', '.join(f'{k} 0x{s:08X}->0x{t:08X}' for k, s, t in sorted(base)))


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2], sys.argv[3:])
