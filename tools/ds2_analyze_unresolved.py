#!/usr/bin/env python3
"""Explains the `UnresolvedCall` errors of a failed `rexglue codegen` run, using only the decrypted XEX image.

Inputs : out/ds2_image.bin (from ds2_imagedump.py), the codegen log, out/ds2_image.elf (for llvm-objdump text).
Output : a per-target report (out/reports/unresolved_targets.txt) and a summary on stdout. Nothing is modified.

For every logged `b 0xTARGET from 0xSOURCE` it collects instruction-level evidence:
  * the source instruction really is `b TARGET` (checks that the dump matches what codegen saw)
  * whether source/target lie inside a PDATA (exception table) function
  * the instruction in front of the target (terminator = blr/bctr/b/padding, otherwise code falls into it)
  * how the target starts (frame-creating prologue, plain leaf code, epilogue-only code)
  * who refers to the target in the whole image: direct bl/b/bc branches, 32-bit pointers in .rdata/.data
    (vtables, function pointers) and lis+addi/ori address constructions
and derives a rough automatic classification (the manual review in 05-unresolved-analysis.md supersedes it).

Disassembler: set OBJDUMP env var (default: llvm-objdump-20, falls back to llvm-objdump).
"""
import argparse
import bisect
import collections
import os
import re
import shutil
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(__file__))
import ds2_imagedump as D  # noqa: E402

BASE = 0x82000000
BLR, BCTR, NOP = 0x4E800020, 0x4E800420, 0x60000000
OBJDUMP = os.environ.get('OBJDUMP') or shutil.which('llvm-objdump-20') or shutil.which('llvm-objdump') or 'llvm-objdump'


def sext(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def mnem(w):
    """Very small decoder, enough for structural classification."""
    op = w >> 26
    if w == BLR:
        return 'blr'
    if w == BCTR:
        return 'bctr'
    if w == 0x4E800421:
        return 'bctrl'
    if w == NOP:
        return 'nop'
    if w == 0:
        return 'zero'
    if op == 18:
        return 'bl' if w & 1 else 'b'
    if op == 16:
        return 'bc'
    if (w & 0xFC1FFFFF) == 0x7C0802A6:
        return 'mflr'
    if (w & 0xFC1FFFFF) == 0x7C0803A6:
        return 'mtlr'
    ra, rs = (w >> 16) & 31, (w >> 21) & 31
    if op == 37 and ra == 1:
        return 'stwu_r1'
    if op == 62 and (w & 3) == 1 and ra == 1:
        return 'stdu_r1'
    if op in (36, 62) and ra == 1:
        return 'st_r1'
    if op in (32, 58) and ra == 1:
        return 'ld_r1'
    if op == 14 and rs == 1 and ra == 1:
        return 'addi_r1'
    return 'other'


def load(args):
    img = open(args.image, 'rb').read()
    pe = D.parse_pe(img, BASE)
    pd = D.pdata_functions(img, BASE, pe)
    secs = {s['name']: (BASE + s['va'], BASE + s['va'] + s['vsize']) for s in pe['sections'] if s['mapped']}
    return img, pe, pd, secs


def parse_log(path):
    txt = open(path, encoding='utf-8', errors='replace').read()
    return [(int(t, 16), int(s, 16)) for t, s in re.findall(r'^\s+0x([0-9A-F]{8}) from 0x([0-9A-F]{8}): b ', txt, re.M)]


def disasm(elf, start, stop):
    r = subprocess.run([OBJDUMP, '-d', '--triple=powerpc64-unknown-unknown', '--mattr=+altivec',
                        f'--start-address=0x{start:08X}', f'--stop-address=0x{stop:08X}', elf],
                       capture_output=True, text=True)
    lines = []
    for ln in r.stdout.splitlines():
        m = re.match(r'\s*([0-9a-f]{8}):\s+(?:[0-9a-f]{2} ){4}\s*(.*)', ln)
        if m:
            txt = re.sub(r'\s*<(?:_binary|\.text)[^>]*>', '', m.group(2))
            lines.append(f'{m.group(1)}: {txt}')
    return lines


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--image', default='out/ds2_image.bin')
    ap.add_argument('--elf', default='out/ds2_image.elf')
    ap.add_argument('--log', default='out/logs/runner.out')
    ap.add_argument('--report', default='out/reports/unresolved_targets.txt')
    args = ap.parse_args()

    img, pe, pd, secs = load(args)
    rows = parse_log(args.log)
    word = lambda a: struct.unpack_from('>I', img, a - BASE)[0]
    starts = [b for b, _, _, _ in pd]

    def pdfind(a):
        i = bisect.bisect_right(starts, a) - 1
        return pd[i] if i >= 0 and pd[i][0] <= a < pd[i][0] + pd[i][1] else None

    wanted = {t for t, _ in rows}
    # 1. self-check against what codegen saw
    ok = 0
    for t, s in rows:
        w = word(s)
        if w >> 26 == 18 and not w & 3 and (s + sext(w & 0x03FFFFFC, 26)) & 0xFFFFFFFF == t:
            ok += 1
    print(f'self-check: {ok}/{len(rows)} logged sources are `b <target>` in the dump')

    # 2. cross references over the whole image
    tlo, thi = secs['.text']
    text = img[tlo - BASE:thi - BASE]
    n = len(text) // 4
    words = struct.unpack('>%dI' % n, text[:n * 4])
    bl_in, b_in, bc_in, addr_in = (collections.defaultdict(list) for _ in range(4))
    for i, w in enumerate(words):
        a = tlo + i * 4
        op = w >> 26
        if op == 18:
            li = sext(w & 0x03FFFFFC, 26)
            t = li & 0xFFFFFFFF if w & 2 else (a + li) & 0xFFFFFFFF
            if t in wanted:
                (bl_in if w & 1 else b_in)[t].append(a)
        elif op == 16:
            bd = sext(w & 0xFFFC, 16)
            t = bd & 0xFFFFFFFF if w & 2 else (a + bd) & 0xFFFFFFFF
            if t in wanted:
                bc_in[t].append(a)
        elif op == 15 and ((w >> 16) & 31) == 0:  # lis rD, hi
            rd, hi = (w >> 21) & 31, w & 0xFFFF
            for k in range(1, 5):
                if i + k >= n:
                    break
                w2 = words[i + k]
                if (w2 >> 21) & 31 == rd and (w2 >> 16) & 31 == rd:
                    if w2 >> 26 == 14:
                        v = ((hi << 16) + sext(w2 & 0xFFFF, 16)) & 0xFFFFFFFF
                    elif w2 >> 26 == 24:
                        v = (hi << 16) | (w2 & 0xFFFF)
                    else:
                        continue
                    if v in wanted:
                        addr_in[v].append(a)
                    break
    data_in = collections.defaultdict(list)
    for name in ('.rdata', '.data'):
        lo, hi = secs[name]
        seg = img[lo - BASE:hi - BASE]
        for i in range(0, len(seg) - 3, 4):
            v = struct.unpack_from('>I', seg, i)[0]
            if v in wanted:
                data_in[v].append((name, lo + i))

    # 3. per-target evidence and rough classification
    os.makedirs(os.path.dirname(args.report), exist_ok=True)
    out = open(args.report, 'w')
    table = []
    src_of = dict(rows)
    for t in sorted(src_of):
        src = src_of[t]
        prev = word(t - 4)
        pk = mnem(prev)
        first = [mnem(word(t + 4 * k)) for k in range(6)]
        prologue = 'mflr' in first[:4] and any(x in ('stwu_r1', 'stdu_r1') for x in first[:6])
        epilogue_only = first[0] in ('addi_r1', 'ld_r1', 'mtlr')
        terminator_before = pk in ('blr', 'bctr', 'b', 'zero')
        n_bl, n_data, n_addr = len(bl_in[t]), len(data_in[t]), len(addr_in[t])
        n_b, n_bc = len(set(b_in[t])), len(bc_in[t])
        if n_bl + n_data + n_addr:
            cls = 'standalone (entry evidence)'
        elif terminator_before and prologue:
            cls = 'standalone (prologue after terminator)'
        elif epilogue_only and not terminator_before:
            cls = 'continuation (epilogue-only, falls in from above)'
        elif not terminator_before:
            cls = 'continuation? (code falls into target)'
        elif epilogue_only:
            cls = 'shared epilogue chunk'
        else:
            cls = 'unclear (needs context)'
        table.append(dict(t=t, s=src, cls=cls, n_bl=n_bl, n_data=n_data, n_addr=n_addr,
                          pd_t=pdfind(t), pd_s=pdfind(src)))
        out.write(f'=== target 0x{t:08X}  from 0x{src:08X}  -> {cls}\n')
        out.write(f'    refs to target: bl={n_bl} b={n_b} bc={n_bc} data={n_data} lis+addi={n_addr}; '
                  f'prev={pk}; first={first}\n')
        if data_in[t]:
            out.write('    data refs: ' + ', '.join(f'{nm}@0x{a:08X}' for nm, a in data_in[t][:6]) + '\n')
        if bl_in[t]:
            out.write('    bl callers: ' + ', '.join(f'0x{a:08X}' for a in bl_in[t][:6]) + '\n')
        out.write('  -- source context\n')
        for ln in disasm(args.elf, src - 16, src + 8):
            out.write('    ' + ln + '\n')
        out.write('  -- target context\n')
        for ln in disasm(args.elf, t - 12, t + 40):
            out.write('    ' + ln + '\n')
        out.write('\n')
    out.close()

    c = collections.Counter(r['cls'] for r in table)
    print('classification counts:')
    for k, v in c.most_common():
        print(f'  {v:3d}  {k}')
    print('sources inside PDATA function:', sum(1 for r in table if r['pd_s']), '/', len(table))
    print('targets inside PDATA function:', sum(1 for r in table if r['pd_t']), '/', len(table))
    print('targets with >=1 bl caller   :', sum(1 for r in table if r['n_bl']))
    print('targets referenced from data :', sum(1 for r in table if r['n_data']))
    print('targets built via lis+addi   :', sum(1 for r in table if r['n_addr']))
    print('report ->', args.report)


if __name__ == '__main__':
    main()
