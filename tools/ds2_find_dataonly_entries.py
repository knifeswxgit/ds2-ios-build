#!/usr/bin/env python3
"""Finds code addresses that are referenced only from data (vtables, function-pointer tables) and that the generated
code does not have as function entries. Read-only; writes a candidate [functions] TOML for diagnostic runs.

A candidate must:
  * be a 4-aligned address inside .text that some 32-bit word of .rdata/.data points to;
  * not be a function entry of a successful codegen run (codegen.partition.json "assignments");
  * be preceded by a terminator (blr, bctr, unconditional b, zero padding, or a `bl` to a function that never
    returns), so no code falls into it;
  * not start with a word that is itself a .text address (an inline jump table after bctr, not code);
  * not be an import thunk (trace: "added import function 0x...");
  * not be listed in config/ds2_dataonly_exclude.txt (entries proven to split a real function);
  * not be referenced only from inside absolute (type 0) jump tables detected by codegen (trace lines
    "detectJumpTable: ... reading N entries from table=0x... base=0x... type=0").

With --code-built, the source of candidates is instead every address that code materializes with
`lis rD,hi` + `addi/ori rX,rD,lo` (function pointers passed as callbacks or stored at run time), with the same
terminator/entry/import filters, and excluding bases and targets of jump tables detected by codegen.

Usage:
  python3 tools/ds2_find_dataonly_entries.py out/ds2_image.bin app/generated/<dir>/codegen.partition.json \
      out/logs/diag/<run>.log config/ds2_overrides.dataonly.toml [--code-built]
"""
import json
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
import ds2_analyze_unresolved as A  # noqa: E402
import ds2_imagedump as D  # noqa: E402

B = 0x82000000


def main(image_path, partition_path, trace_path, out_path, code_built=False):
    img = open(image_path, 'rb').read()
    pe = D.parse_pe(img, B)
    w = lambda a: struct.unpack_from('>I', img, a - B)[0]
    secs = {s['name']: (B + s['va'], B + s['va'] + s['vsize']) for s in pe['sections'] if s['mapped']}
    tlo, thi = secs['.text']
    entries = {int(k, 16) for k in json.load(open(partition_path))['assignments']}

    tables = []
    pat = re.compile(r'reading (\d+) entries from table=0x([0-9A-F]+) base=0x([0-9A-F]+) type=(\d+)')
    with open(trace_path, errors='replace') as f:
        for line in f:
            if 'reading' in line and 'entries from table' in line:
                m = pat.search(line)
                # Only kAbsolute (type 0, lwzx) tables hold 32-bit addresses; types 1-3 hold 1/2-byte offsets
                # (sdk/src/codegen/function_scanner.cpp, JumpTableType), so they cannot contain a pointer word.
                if m and int(m[4]) == 0:
                    tables.append((int(m[2], 16), int(m[2], 16) + int(m[1]) * 4))
    in_table = lambda o: any(a <= o < b for a, b in tables)
    jt_bases, jt_targets = set(), set()
    with open(trace_path, errors='replace') as f:
        for line in f:
            if 'detectJumpTable' not in line:
                continue
            m = re.search(r'entries from table=0x([0-9A-F]+) base=0x([0-9A-F]+)', line)
            if m:
                jt_bases.add(int(m[1], 16))  # the table itself (inline tables live in .text)
                jt_bases.add(int(m[2], 16))
            m = re.search(r'target=0x([0-9A-F]+)', line)
            if m:
                jt_targets.add(int(m[1], 16))
    imports = set()
    imp = re.compile(r'added import function 0x([0-9A-F]{8})')
    with open(trace_path, errors='replace') as f:
        for line in f:
            if 'added import function' in line:
                imports.add(int(imp.search(line)[1], 16))

    # Functions that never return: the target of some `bl` that is followed by a zero padding word (the compiler does
    # not pad after a call that returns), e.g. _CxxThrowException and the throw helpers that wrap it. Code right
    # after a `bl` to one of them is unreachable from above, like code after `blr`.
    noreturn = set()
    n_text = (thi - tlo) // 4
    text_words = struct.unpack('>%dI' % n_text, img[tlo - B:tlo - B + n_text * 4])
    for i in range(n_text - 1):
        x = text_words[i]
        if x >> 26 == 18 and x & 1 and text_words[i + 1] == 0:
            a = tlo + 4 * i
            li = A.sext(x & 0x03FFFFFC, 26)
            noreturn.add((li & 0xFFFFFFFF) if x & 2 else (a + li) & 0xFFFFFFFF)

    def after_terminator(v):
        if A.mnem(w(v - 4)) in ('blr', 'bctr', 'b', 'zero'):
            return True
        x = w(v - 4)
        if x >> 26 == 18 and x & 1:
            li = A.sext(x & 0x03FFFFFC, 26)
            t = (li & 0xFFFFFFFF) if x & 2 else (v - 4 + li) & 0xFFFFFFFF
            return t in noreturn
        return False

    refs = {}
    if not code_built:
        for name in ('.rdata', '.data'):
            lo, hi = secs[name]
            for o in range(lo, hi - 3, 4):
                v = w(o)
                if tlo <= v < thi and not v & 3:
                    refs.setdefault(v, []).append(o)
    else:
        n = (thi - tlo) // 4
        words = struct.unpack('>%dI' % n, img[tlo - B:tlo - B + n * 4])
        for i, x in enumerate(words):
            if x >> 26 != 15 or (x >> 16) & 31:  # lis rD, hi  (addis rD, 0, hi)
                continue
            rd, hi = (x >> 21) & 31, x & 0xFFFF
            for k in range(1, 9):
                if i + k >= n:
                    break
                y = words[i + k]
                op = y >> 26
                if op in (14, 24) and (y >> 16) & 31 == rd:  # addi / ori rX, rD, lo
                    v = ((hi << 16) + A.sext(y & 0xFFFF, 16)) & 0xFFFFFFFF if op == 14 else (hi << 16) | (y & 0xFFFF)
                    if tlo <= v < thi and not v & 3 and v not in jt_bases and v not in jt_targets:
                        refs.setdefault(v, []).append(tlo + 4 * i)
                    break

    exclude = set()
    ex_path = os.path.join(os.path.dirname(__file__), '..', 'config', 'ds2_dataonly_exclude.txt')
    if os.path.exists(ex_path):
        for line in open(ex_path):
            tok = line.split('#')[0].strip()
            if tok:
                exclude.add(int(tok, 16))

    out = []
    for v in sorted(refs):
        if v in exclude:
            continue
        if v in entries or v in imports:  # imports are not in the partition; codegen ignores config entries on them
            continue
        if not after_terminator(v):
            continue
        if tlo <= w(v) < thi:  # first word is itself a .text address: an inline table (e.g. after bctr), not code
            continue
        if not code_built and all(in_table(o) for o in refs[v]):
            continue
        out.append((v, refs[v]))

    with open(out_path, 'w') as f:
        f.write('# GENERATED by tools/ds2_find_dataonly_entries.py - DIAGNOSTIC candidates, not applied to the working manifest.\n')
        f.write(f'# Inputs: {os.path.basename(partition_path)} ({len(entries)} entries), {len(tables)} jump tables from the trace.\n')
        f.write('# Each entry: code after a terminator, ' + ('built by lis+addi/ori in code' if code_built else 'pointed to from .rdata/.data')
                + ', not a function entry of that run.\n\n')
        f.write('[functions]\n')
        for v, r in out:
            f.write(f'"0x{v:08X}" = {{}}   # refs: {" ".join("0x%08X" % o for o in r[:3])}{" ..." if len(r) > 3 else ""}\n')
    print(f'data-referenced .text addresses: {len(refs)}; not entries: {sum(1 for v in refs if v not in entries)}; '
          f'noreturn targets: {len(noreturn)}; imports: {len(imports)}; excluded: {len(exclude)}; jump tables: {len(tables)}; candidates written: {len(out)} -> {out_path}')


if __name__ == '__main__':
    main(*sys.argv[1:5], code_built='--code-built' in sys.argv[5:])
