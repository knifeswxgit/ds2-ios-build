#!/usr/bin/env python3
"""Decrypts and unpacks the PE image of an Xbox 360 XEX2 (basic compression) for read-only analysis.

It repeats what ReXGlue's loader does (sdk/src/system/xex_module.cpp, ReadImage / ReadImageBasicCompressed), so the
result is byte-for-byte the image that the code generator analyses:

  * session key = AES-128-ECB-decrypt(retail key, security_info.aes_key)  (the key is the one in xex_module.cpp)
  * image       = AES-128-CBC-decrypt(session key, IV = 0, one chain over all data blocks); zero blocks are skipped

The input file is only read. The output goes to the path given with -o (keep it under out/, which is git-ignored).
Self-checks: MZ header, PE signature, machine type 0x01F2, PE32 magic, exception directory inside the image.
Compressed (LZX) and delta-patch XEX files are rejected.

Usage:
  python tools/ds2_imagedump.py assets/game_root/Default.xex -o out/ds2_image.bin
  python tools/ds2_imagedump.py assets/game_root/Default.xex --info
"""
import argparse
import struct
import sys

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

RETAIL_KEY = bytes([0x20, 0xB1, 0x85, 0xA5, 0x9D, 0x28, 0xFD, 0xC3, 0x40, 0x58, 0x3F, 0xBB, 0x08, 0x96, 0xBF, 0x91])
DEVKIT_KEY = bytes(16)
KEY_FILE_FORMAT = 0x000003FF


class ImageError(Exception):
    pass


def be32(d, o):
    return struct.unpack_from('>I', d, o)[0]


def be16(d, o):
    return struct.unpack_from('>H', d, o)[0]


def _aes_cbc_decrypt(key, data):
    if len(data) % 16:
        raise ImageError('encrypted data is not a multiple of 16 bytes')
    dec = Cipher(algorithms.AES(key), modes.CBC(bytes(16))).decryptor()
    return dec.update(data) + dec.finalize()


def _optional_header(d, wanted):
    count = be32(d, 20)
    for i in range(count):
        key, val = be32(d, 24 + i * 8), be32(d, 28 + i * 8)
        if key != wanted:
            continue
        sc = key & 0xFF
        if sc <= 1:
            return val
        size = be32(d, val) if sc == 0xFF else sc * 4
        return d[val:val + size]
    return None


def extract_image(xex, use_dev_key=False):
    """Returns (image bytes, load_address, image_size). Raises ImageError on anything unexpected."""
    if xex[:4] != b'XEX2':
        raise ImageError('not a XEX2 file')
    pe_offset = be32(xex, 8)
    sec = be32(xex, 16)
    ff = _optional_header(xex, KEY_FILE_FORMAT)
    if not isinstance(ff, (bytes, bytearray)):
        raise ImageError('missing file format info')
    info_size, enc, comp = be32(ff, 0), be16(ff, 4), be16(ff, 6)
    if enc != 1:
        raise ImageError(f'unsupported encryption type {enc} (only "normal" is handled here)')
    if comp != 1:
        raise ImageError(f'unsupported compression type {comp} (only "basic" is handled here)')
    image_size = be32(xex, sec + 4)
    load_address = be32(xex, sec + 0x110)
    session = Cipher(algorithms.AES(DEVKIT_KEY if use_dev_key else RETAIL_KEY), modes.ECB()).decryptor().update(
        bytes(xex[sec + 0x150:sec + 0x160]))
    blocks = [struct.unpack_from('>II', ff, 8 + i * 8) for i in range((info_size - 8) // 8)]
    total = sum(a + b for a, b in blocks)
    if total != image_size:
        raise ImageError(f'basic blocks cover 0x{total:X} bytes but the security info says 0x{image_size:X}')
    data_total = sum(a for a, _ in blocks)
    if pe_offset + data_total != len(xex):
        raise ImageError('file size does not match header size + data blocks (truncated or padded file?)')
    plain = _aes_cbc_decrypt(session, bytes(xex[pe_offset:pe_offset + data_total]))
    image = bytearray(image_size)
    src = dst = 0
    for data_size, zero_size in blocks:
        image[dst:dst + data_size] = plain[src:src + data_size]
        src += data_size
        dst += data_size + zero_size
    return bytes(image), load_address, image_size


def parse_pe(image, base):
    """Minimal PE parser. PE headers are little-endian (only the code and PDATA are big-endian)."""
    le32 = lambda o: struct.unpack_from('<I', image, o)[0]
    le16 = lambda o: struct.unpack_from('<H', image, o)[0]
    if image[:2] != b'MZ':
        raise ImageError('decrypted image does not start with MZ (wrong key or damaged file)')
    lfanew = le32(0x3C)
    if lfanew + 0x100 > len(image) or image[lfanew:lfanew + 4] != b'PE\0\0':
        raise ImageError('missing PE signature')
    machine = le16(lfanew + 4)
    if machine != 0x01F2:
        raise ImageError(f'unexpected machine type 0x{machine:04X} (expected 0x01F2, PowerPC big-endian)')
    n_sections = le16(lfanew + 6)
    opt_size = le16(lfanew + 20)
    opt = lfanew + 24
    magic = le16(opt)
    if magic != 0x10B or n_sections > 96:
        raise ImageError('implausible PE optional header')
    image_base = le32(opt + 28)
    exc_rva, exc_size = le32(opt + 96 + 3 * 8), le32(opt + 96 + 3 * 8 + 4)
    sections = []
    sh = opt + opt_size
    for i in range(n_sections):
        o = sh + i * 40
        name = image[o:o + 8].rstrip(b'\0').decode('ascii', 'replace')
        vsize, vaddr, rawsize, rawptr = struct.unpack_from('<IIII', image, o + 8)
        chars = le32(o + 36)
        # Sections such as .reloc may be described in the header but not mapped into the loaded image.
        sections.append({'name': name, 'va': vaddr, 'vsize': vsize, 'chars': chars,
                         'mapped': vaddr + vsize <= len(image)})
    return {'machine': machine, 'magic': magic, 'image_base': image_base, 'sections': sections,
            'exc_rva': exc_rva, 'exc_size': exc_size, 'n_sections': n_sections}


def pdata_functions(image, base, pe):
    """Returns sorted list of (begin_va, size_bytes, prolog_len, exception_flag) from the exception directory."""
    start = pe['exc_rva']
    if start >= base:  # directory address may be stored as an absolute VA
        start -= base
    if start + pe['exc_size'] > len(image):
        raise ImageError('exception directory lies outside the image')
    out = []
    for o in range(start, start + pe['exc_size'], 8):
        begin, data = be32(image, o), be32(image, o + 4)
        if begin == 0:
            continue
        prolog = data & 0xFF
        length = (data >> 8) & 0x3FFFFF
        exc = (data >> 31) & 1
        out.append((begin, max(length * 4, 4), prolog, exc))
    out.sort()
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('xex')
    ap.add_argument('-o', '--out', help='write the raw image (base address = load address) to this file')
    ap.add_argument('--info', action='store_true', help='print PE sections and PDATA statistics')
    ap.add_argument('--dev-key', action='store_true')
    args = ap.parse_args(argv)
    with open(args.xex, 'rb') as f:
        xex = f.read()
    try:
        image, base, size = extract_image(xex, args.dev_key)
        pe = parse_pe(image, base)
    except ImageError as e:
        print(f'error: {e}', file=sys.stderr)
        return 2
    print(f'image: {size} bytes (0x{size:X}) at 0x{base:08X}; MZ ok; machine 0x{pe["machine"]:04X}; '
          f'PE32 magic 0x{pe["magic"]:X}; PE image base 0x{pe["image_base"]:08X}')
    if args.info or not args.out:
        for s in pe['sections']:
            note = '' if s['mapped'] else '  (not mapped in the loaded image)'
            print(f'  {s["name"]:<8} va 0x{base + s["va"]:08X} size 0x{s["vsize"]:08X} flags 0x{s["chars"]:08X}{note}')
        pd = pdata_functions(image, base, pe)
        print(f'  PDATA: {len(pd)} entries, exception directory rva 0x{pe["exc_rva"]:X} size 0x{pe["exc_size"]:X}')
    if args.out:
        with open(args.out, 'wb') as f:
            f.write(image)
        print(f'wrote {args.out}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
