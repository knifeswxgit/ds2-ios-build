#!/usr/bin/env python3
"""Inspects the header of an Xbox 360 default.xex (XEX2) before it is handed to the code generator.

Checks that the file is an Xbox 360 executable (not a PC or Xbox One build), prints the title id, entry point, base
address, encryption/compression mode and imported kernel libraries, and exits non-zero if the file is something the
recompilation chain cannot use.

Usage: python tools/ds2_xexinfo.py path/to/default.xex [--json]
Only the header is read; no game data is written anywhere.
"""
import argparse
import json
import struct
import sys

ENCRYPTION = {0: 'none', 1: 'normal'}
COMPRESSION = {0: 'none', 1: 'basic', 2: 'normal (LZX)', 3: 'delta patch'}

KEY_FILE_FORMAT = 0x000003FF
KEY_ENTRY_POINT = 0x00010100
KEY_IMAGE_BASE = 0x00010201
KEY_ORIGINAL_BASE = 0x00010001
KEY_IMPORTS = 0x000103FF
KEY_ORIGINAL_PE_NAME = 0x000183FF
KEY_STACK_SIZE = 0x00020200
KEY_EXECUTION_INFO = 0x00040006


class XexError(Exception):
    pass


def u32(data, off):
    if off < 0 or off + 4 > len(data):
        raise XexError(f'read past end of file at 0x{off:X}')
    return struct.unpack_from('>I', data, off)[0]


def u16(data, off):
    if off < 0 or off + 2 > len(data):
        raise XexError(f'read past end of file at 0x{off:X}')
    return struct.unpack_from('>H', data, off)[0]


def parse_xex(data):
    magic = data[:4]
    if magic == b'XEX1':
        raise XexError('XEX1 (early beta format) is not supported')
    if magic != b'XEX2':
        raise XexError(f'not an Xbox 360 XEX2 file (magic {magic!r}); PC (MZ) and Xbox One builds are not valid inputs')

    info = {
        'module_flags': u32(data, 4),
        'pe_data_offset': u32(data, 8),
        'security_offset': u32(data, 16),
    }
    count = u32(data, 20)
    if count > 512:
        raise XexError(f'implausible optional header count {count}')

    headers = {}
    for i in range(count):
        key = u32(data, 24 + i * 8)
        val = u32(data, 28 + i * 8)
        size_class = key & 0xFF
        if size_class <= 1:
            headers[key] = val                       # value stored inline
        elif size_class == 0xFF:
            size = u32(data, val)                    # variable size, first word is the size
            headers[key] = data[val:val + size]
        else:
            headers[key] = data[val:val + size_class * 4]

    ff = headers.get(KEY_FILE_FORMAT)
    if not isinstance(ff, (bytes, bytearray)) or len(ff) < 8:
        raise XexError('missing file format info header')
    info['encryption'] = u16(ff, 4)
    info['compression'] = u16(ff, 6)

    info['entry_point'] = headers.get(KEY_ENTRY_POINT)
    info['image_base'] = headers.get(KEY_IMAGE_BASE)
    info['original_base'] = headers.get(KEY_ORIGINAL_BASE)

    sec = info['security_offset']
    info['image_size'] = u32(data, sec + 4)
    info['load_address'] = u32(data, sec + 0x110)

    ex = headers.get(KEY_EXECUTION_INFO)
    if isinstance(ex, (bytes, bytearray)) and len(ex) >= 24:
        info['title_id'] = u32(ex, 12)
        info['media_id'] = u32(ex, 0)
        info['version'] = u32(ex, 4)
        info['executable_type'] = ex[17]
        info['disc_number'] = ex[18]
        info['disc_count'] = ex[19]

    pe_name = headers.get(KEY_ORIGINAL_PE_NAME)
    if isinstance(pe_name, (bytes, bytearray)) and len(pe_name) > 4:
        info['original_pe_name'] = bytes(pe_name[4:]).split(b'\0')[0].decode('ascii', 'replace')

    stack = headers.get(KEY_STACK_SIZE)
    if isinstance(stack, int):
        info['stack_size'] = stack

    imports = headers.get(KEY_IMPORTS)
    if isinstance(imports, (bytes, bytearray)) and len(imports) >= 12:
        table_size = u32(imports, 4)
        n_libs = u32(imports, 8)
        pos = 12
        names = bytes(imports[pos:pos + table_size])
        libs = [n.decode('ascii', 'replace') for n in names.split(b'\0') if n][:n_libs]
        info['import_libraries'] = libs
    info['optional_header_keys'] = sorted(f'0x{k:08X}' for k in headers)
    return info


def verdict(info):
    problems = []
    if info['encryption'] not in ENCRYPTION:
        problems.append(f"unknown encryption type {info['encryption']}")
    if info['compression'] == 3:
        problems.append('delta-patch XEX: apply the title update to the base XEX first')
    if info['compression'] not in COMPRESSION:
        problems.append(f"unknown compression type {info['compression']}")
    if info.get('entry_point') is None:
        problems.append('no entry point header')
    return problems


def render(info):
    lines = []
    lines.append(f"title id        : {info['title_id']:08X}" if 'title_id' in info else 'title id        : (none)')
    if 'original_pe_name' in info:
        lines.append(f"original PE     : {info['original_pe_name']}")
    lines.append(f"entry point     : 0x{info['entry_point']:08X}" if info.get('entry_point') is not None else 'entry point     : (none)')
    base = info.get('image_base') if info.get('image_base') is not None else info['load_address']
    lines.append(f'image base      : 0x{base:08X}')
    lines.append(f"image size      : 0x{info['image_size']:X}")
    lines.append(f"encryption      : {ENCRYPTION.get(info['encryption'], info['encryption'])}")
    lines.append(f"compression     : {COMPRESSION.get(info['compression'], info['compression'])}")
    if 'import_libraries' in info:
        lines.append('imports         : ' + ', '.join(info['import_libraries']))
    return '\n'.join(lines)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('xex')
    ap.add_argument('--json', action='store_true')
    args = ap.parse_args(argv)
    with open(args.xex, 'rb') as f:
        data = f.read(1 << 20)  # headers always fit in the first MiB
    try:
        info = parse_xex(data)
    except XexError as e:
        print(f'error: {e}', file=sys.stderr)
        return 2
    problems = verdict(info)
    if args.json:
        print(json.dumps({**info, 'problems': problems}, default=str, indent=2))
    else:
        print(render(info))
        for p in problems:
            print(f'problem: {p}', file=sys.stderr)
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
