#!/usr/bin/env python3
"""Wraps the raw image from ds2_imagedump.py in a minimal ELF32 big-endian PowerPC file with one .text section at
0x82000000, so `llvm-objdump -d --triple=powerpc64-unknown-unknown` prints real guest addresses.
Needs no objcopy (older llvm-objcopy lacks --change-section-address).

Usage: python tools/ds2_make_elf.py out/ds2_image.bin out/ds2_image.elf
"""
import struct
import sys

BASE = 0x82000000


def main(src, dst):
    data = open(src, 'rb').read()
    shstr = b'\0.text\0.shstrtab\0'
    eh = 52
    off_data = eh
    off_shstr = off_data + len(data)
    off_sh = (off_shstr + len(shstr) + 3) & ~3
    e = bytearray(b'\x7fELF\x01\x02\x01' + bytes(9))  # ELFCLASS32, big-endian, version 1
    e += struct.pack('>HHIIIIIHHHHHH', 2, 20, 1, BASE, 0, off_sh, 0, eh, 0, 0, 40, 3, 2)  # EM_PPC = 20
    sh = lambda n, t, f, a, o, s, al: struct.pack('>IIIIIIIIII', n, t, f, a, o, s, 0, 0, al, 0)
    out = bytes(e) + data + shstr
    out += bytes(off_sh - len(out))
    out += sh(0, 0, 0, 0, 0, 0, 0) + sh(1, 1, 6, BASE, off_data, len(data), 4) + sh(7, 3, 0, 0, off_shstr, len(shstr), 1)
    open(dst, 'wb').write(out)
    print('wrote', dst)


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
