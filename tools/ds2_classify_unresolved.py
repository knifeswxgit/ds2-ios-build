#!/usr/bin/env python3
"""Classifies the `UnresolvedCall` targets of a failed `rexglue codegen` run from instruction evidence (read-only).

STATUS: written in the previous session but NEVER RUN. Treat its output as unverified until checked against
05-unresolved-analysis.md. Its main purpose is the second-order check: after declaring a thunk/stub as a function,
would its own tail-call target still be unknown (=> a new UnresolvedCall on the next codegen run)?

Needs out/ds2_image.bin (tools/ds2_imagedump.py) and the codegen log. Writes out/reports/classification.tsv.

Classes:
  THUNK         `lwz r3,N(r3)` / `addi r3,r3,N` then `b X`; a self-contained function
  LEAF_TAILCALL block preceded by a terminator, ending in a terminator (includes vtable-dispatch stubs)
  JOIN_POINT    target lies inside the control-flow reach of an entry that has only data-pointer evidence
  JOIN_POINT_UNREFERENCED  same, but the owning entry has no reference of any kind in the image
"""
import argparse
import bisect
import collections
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
import ds2_analyze_unresolved as A  # noqa: E402
import ds2_imagedump as D  # noqa: E402

B = 0x82000000
sx = A.sext


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--image', default='out/ds2_image.bin')
    ap.add_argument('--log', default='out/logs/runner.out')
    ap.add_argument('--out', default='out/reports/classification.tsv')
    args = ap.parse_args()

    img = open(args.image, 'rb').read()
    pe = D.parse_pe(img, B)
    pd = D.pdata_functions(img, B, pe)
    secs = {s['name']: (B + s['va'], B + s['va'] + s['vsize']) for s in pe['sections'] if s['mapped']}
    rows = A.parse_log(args.log)
    w = lambda a: struct.unpack_from('>I', img, a - B)[0]
    starts = [b for b, _, _, _ in pd]
    ends = [b + s for b, s, _, _ in pd]

    def pd_inside(a):
        i = bisect.bisect_right(starts, a) - 1
        return i >= 0 and starts[i] <= a < ends[i]

    def gap_of(a):
        i = bisect.bisect_right(starts, a) - 1
        g0 = ends[i] if i >= 0 else secs['.text'][0]
        g1 = starts[i + 1] if i + 1 < len(starts) else secs['.text'][1]
        return g0, g1

    gaps = sorted({gap_of(x) for t, s in rows for x in (t, s)})
    inside = lambda a: any(g0 <= a < g1 for g0, g1 in gaps)

    tlo, thi = secs['.text']
    n = (thi - tlo) // 4
    words = struct.unpack('>%dI' % n, img[tlo - B:tlo - B + n * 4])
    all_bl = set()
    refs = collections.defaultdict(lambda: collections.defaultdict(list))
    for i, x in enumerate(words):
        a = tlo + i * 4
        op = x >> 26
        if op == 18:
            li = sx(x & 0x03FFFFFC, 26)
            t = li & 0xFFFFFFFF if x & 2 else (a + li) & 0xFFFFFFFF
            if x & 1:
                all_bl.add(t)
            if inside(t):
                refs[t]['bl' if x & 1 else 'b'].append(a)
        elif op == 16:
            bd = sx(x & 0xFFFC, 16)
            t = bd & 0xFFFFFFFF if x & 2 else (a + bd) & 0xFFFFFFFF
            if inside(t):
                refs[t]['bc'].append(a)
        elif op == 15 and ((x >> 16) & 31) == 0:
            rd, hi = (x >> 21) & 31, x & 0xFFFF
            for k in range(1, 5):
                if i + k >= n:
                    break
                w2 = words[i + k]
                if (w2 >> 21) & 31 == rd and (w2 >> 16) & 31 == rd and w2 >> 26 in (14, 24):
                    v = ((hi << 16) + sx(w2 & 0xFFFF, 16)) & 0xFFFFFFFF if w2 >> 26 == 14 else (hi << 16) | (w2 & 0xFFFF)
                    if inside(v):
                        refs[v]['lis'].append(a)
                    break
    for name in ('.rdata', '.data'):
        lo, hi = secs[name]
        seg = img[lo - B:hi - B]
        for off in range(0, len(seg) - 3, 4):
            v = struct.unpack_from('>I', seg, off)[0]
            if v & 3 == 0 and inside(v):
                refs[v]['data'].append(lo + off)
    entry_kinds = lambda a: {k for k in ('bl', 'data', 'lis') if refs.get(a, {}).get(k)}

    def reach(entry, g):
        g0, g1 = g
        seen, wl = set(), [entry]
        while wl:
            a = wl.pop()
            while g0 <= a < g1 and a not in seen:
                seen.add(a)
                x = w(a)
                op = x >> 26
                if op == 18:
                    li = sx(x & 0x03FFFFFC, 26)
                    t = li & 0xFFFFFFFF if x & 2 else (a + li) & 0xFFFFFFFF
                    if x & 1:
                        a += 4
                        continue
                    if g0 <= t < g1:
                        wl.append(t)
                    break
                if op == 16:
                    bd = sx(x & 0xFFFC, 16)
                    t = bd & 0xFFFFFFFF if x & 2 else (a + bd) & 0xFFFFFFFF
                    if g0 <= t < g1:
                        wl.append(t)
                    a += 4
                    continue
                if op == 19 and ((x >> 1) & 0x3FF) in (16, 528) and (((x >> 21) & 31) & 0x14) == 0x14 and not x & 1:
                    break
                a += 4
        return seen

    def is_thunk(t):
        i1, i2 = w(t), w(t + 4)
        adj = (i1 >> 26 in (32, 14)) and ((i1 >> 21) & 31) == 3 and ((i1 >> 16) & 31) == 3
        return adj and i2 >> 26 == 18 and not i2 & 1 and not i2 & 2

    def block_exits(t, limit=24):
        a, exits = t, []
        for k in range(limit):
            x = w(a)
            op = x >> 26
            if op == 18 and not x & 1:
                li = sx(x & 0x03FFFFFC, 26)
                exits.append((li & 0xFFFFFFFF) if x & 2 else (a + li) & 0xFFFFFFFF)
                return 'b', exits, k + 1
            if x == 0x4E800020:
                return 'blr', exits, k + 1
            if x == 0x4E800420:
                return 'bctr', exits, k + 1
            if op == 16:
                bd = sx(x & 0xFFFC, 16)
                tt = bd & 0xFFFFFFFF if x & 2 else (a + bd) & 0xFFFFFFFF
                if abs(tt - t) > 0x80:
                    exits.append(tt)
            a += 4
        return 'long', exits, limit

    def prev_kind(t):
        p = w(t - 4)
        if p == 0x4E800020:
            return 'blr'
        if p == 0x4E800420:
            return 'bctr'
        if p >> 26 == 18 and not p & 1:
            return 'b'
        if p == 0:
            return 'pad'
        return 'falls'

    out_rows = []
    for t, s in sorted(rows):
        g = gap_of(t)
        owners = [e for e in sorted(a for a in range(g[0], g[1], 4) if entry_kinds(a)) if e != t and t in reach(e, g)]
        pk = prev_kind(t)
        if owners:
            o = owners[-1]
            cls, fix = 'JOIN_POINT', o
            ev = f'reached from entry 0x{o:08X} ({"/".join(sorted(entry_kinds(o)))} refs only); prev={pk}'
        elif pk == 'falls':
            cand = t
            while True:
                cand -= 4
                p = w(cand - 4)
                if cand < g[0] or p in (0x4E800020, 0x4E800420, 0) or (p >> 26 == 18 and not p & 1):
                    break
            cls, fix = 'JOIN_POINT_UNREFERENCED', cand
            ev = f'falls into target; nearest start after a terminator 0x{cand:08X}, no reference found'
        elif is_thunk(t):
            cls, fix = 'THUNK', t
            ev = f'{A.mnem(w(t))}+b 0x{block_exits(t)[1][0]:08X}; prev={pk}'
        else:
            kind, ex, ln = block_exits(t)
            cls, fix = 'LEAF_TAILCALL', t
            ev = f'self-contained block of {ln} instr ending in {kind}; prev={pk}'
        second = []
        if cls in ('THUNK', 'LEAF_TAILCALL'):
            for x in block_exits(t)[1]:
                if x in starts or x in all_bl:
                    second.append(f'0x{x:08X}:known')
                elif pd_inside(x):
                    second.append(f'0x{x:08X}:inside-pdata')
                else:
                    second.append(f'0x{x:08X}:UNKNOWN')
        out_rows.append((t, s, cls, fix, ev, ';'.join(second)))

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, 'w') as f:
        f.write('target\tsource\tclass\tdeclare_entry\tevidence\texits_after_declaring\n')
        for t, s, cls, fix, ev, sec in out_rows:
            f.write(f'0x{t:08X}\t0x{s:08X}\t{cls}\t0x{fix:08X}\t{ev}\t{sec}\n')
    print('classes:', dict(collections.Counter(r[2] for r in out_rows)))
    print('distinct entries to declare:', len({r[3] for r in out_rows}))
    unknown = [(r[0], r[5]) for r in out_rows if 'UNKNOWN' in r[5]]
    print('targets whose own tail-call exit would still be unknown:', len(unknown))
    for t, sec in unknown:
        print(f'   0x{t:08X}: {sec}')
    print('wrote', args.out)


if __name__ == '__main__':
    main()
