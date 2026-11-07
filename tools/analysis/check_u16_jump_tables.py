#!/usr/bin/env python3
"""Check every halfword (lhzx) jump table in generated code against the XEX image.

  check_u16_jump_tables.py <generated dir> <ds2_image.bin (memory image at 0x82000000)>

Pattern emitted by codegen for `lis r12,T@ha; addi r12,r12,T@l; rlwinm r0,rX,1; lhzx r0,r12,r0; lis r12,B@ha;
addi r12,r12,B@l; rlwinm r0,r0,S; add r12,r12,r0; mtctr r12; bctr` followed by `switch (ctx.rX.u32) { case N:
goto loc_X; ...}`. Each case must equal B + (u16 table[N] << S). Prints mismatching tables (the v45 bug: entries
read as bytes).
"""
import os, re, struct, sys

gen, img = sys.argv[1], open(sys.argv[2], 'rb').read()
def u16(a):
    o = a - 0x82000000
    return struct.unpack('>H', img[o:o + 2])[0]
pat = re.compile(
    r'ctx\.r12\.s64 = (-?\d+);\n(?:.*\n){0,3}?\s*ctx\.r12\.s64 = ctx\.r12\.s64 \+ (-?\d+);\n'
    r'(?:.*\n){0,4}?\s*ctx\.r0\.u64 = REX_LOAD_U16\(ctx\.r12\.u32 \+ ctx\.r0\.u32\);\n'
    r'(?:.*\n){0,3}?\s*ctx\.r12\.s64 = (-?\d+);\n'
    r'(?:.*\n){0,3}?\s*ctx\.r0\.u64 = __builtin_rotateleft64\(ctx\.r0\.u32 \| \(ctx\.r0\.u64 << 32\), (\d+)\)[^\n]*\n'
    r'(?:.*\n){0,3}?\s*ctx\.r12\.s64 = ctx\.r12\.s64 \+ (-?\d+);\n'
    r'(?:.*\n){0,6}?\s*switch \(ctx\.r\d+\.u32\) \{\n((?:\s*case \d+:\n\s*goto loc_[0-9A-F]+;\n)+)')
total = bad = 0
for name in sorted(os.listdir(gen)):
    if not name.endswith('.cpp'):
        continue
    s = open(os.path.join(gen, name)).read()
    for m in pat.finditer(s):
        t = (int(m.group(1)) + int(m.group(2))) & 0xFFFFFFFF
        b = (int(m.group(3)) + int(m.group(5))) & 0xFFFFFFFF
        sh = int(m.group(4))
        cases = re.findall(r'case (\d+):\n\s*goto loc_([0-9A-F]+);', m.group(6))
        total += 1
        wrong = [(int(c), l) for c, l in cases if (b + (u16(t + 2 * int(c)) << sh)) & 0xFFFFFFFF != int(l, 16)]
        if wrong:
            bad += 1
            fn = s.rfind('DEFINE_REX_FUNC(', 0, m.start())
            print('%s %s table %08X base %08X: %d of %d cases wrong' % (name, s[fn + 16:fn + 28], t, b, len(wrong),
                                                                        len(cases)))
print('u16 jump tables: %d, wrong: %d' % (total, bad))
