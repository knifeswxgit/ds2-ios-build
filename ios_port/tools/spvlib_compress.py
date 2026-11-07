#!/usr/bin/env python3
"""Repack a DS2SPVL1 shader library into a random-access compressed pack.

Measured on the real v121 pack (1,722,442,548 B / 35,248 records):
  - duplicate SPIR-V blobs only 1.3% -> dedup is NOT the lever
  - zlib -1 on a 67 MB sample: x2.92 -> compression IS the lever

Output format DS2SPVZ1 (single file, little-endian, record-aligned blocks):

  FileHeader (32 B)   <8s I I Q I I>
      magic "DS2SPVZ1", version=1, abi (13), original_size, block_target, block_count
  TOC (32 B * block_count)  <Q Q I I Q Q>
      virtual_start, compressed_offset, compressed_size, uncompressed_size,
      reserved, blake2b-8 of the uncompressed block
  Blocks              concatenated zlib streams of WHOLE original records
                      (80 B record header + payload), so no record straddles
                      a block and Restore() can seek by virtual offset.

The virtual address space is byte-identical to the original .spvlib, so the
existing .idx key->offset map stays valid. See ios_port/include/spvz_reader.h
for the C++ runtime side.

Usage:
  python spvlib_compress.py <input.spvlib> <output.spvz> [--level 6] [--block 1048576]
  python spvlib_compress.py --verify <input.spvlib> <output.spvz>
"""
import argparse
import hashlib
import os
import struct
import sys
import zlib

HDR = struct.Struct("<4I")
REC = struct.Struct("<10Q")
FILEH = struct.Struct("<8sIIQII")
TOC = struct.Struct("<QQIIQQ")
MAGIC = b"DS2SPVZ1"
SPVL_MAGIC = b"DS2SPVL1"


def digest8(data):
    return int.from_bytes(hashlib.blake2b(data, digest_size=8).digest(), "little")


def iter_records(f, size):
    """Yield (virtual_offset, record_bytes, payload_bytes) for every record."""
    head = f.read(HDR.size)
    if len(head) != HDR.size or struct.pack("<2I", *HDR.unpack(head)[:2]) != SPVL_MAGIC:
        sys.exit("not a DS2SPVL1 pack")
    yield 0, head, b""
    off = HDR.size
    while off < size:
        rb = f.read(REC.size)
        if len(rb) != REC.size:
            sys.exit(f"truncated record header at {off}")
        r = REC.unpack(rb)
        if r[3] > 1 or r[4] > 262144 or not r[5] or r[5] > (4 << 20) or (r[5] & 3) or r[6] > 64 or r[7] > 64:
            sys.exit(f"invalid record at {off}: {r}")
        payload = r[4] * 4 + r[5] + (r[6] + r[7]) * 4
        pb = f.read(payload)
        if len(pb) != payload:
            sys.exit(f"truncated payload at {off}")
        yield off, rb, pb
        off += REC.size + payload


def compress(src, dst, level, block_target):
    src_size = os.path.getsize(src)
    blocks = []          # (virtual_start, compressed_offset, csize, usize, digest)
    raw_hash = hashlib.blake2b(digest_size=16)
    pending = bytearray()
    pending_start = HDR.size
    compressed_parts = []
    data_offset = None

    with open(src, "rb") as f:
        for off, rb, pb in iter_records(f, src_size):
            if off == 0:
                raw_hash.update(rb)
                continue
            chunk = rb + pb
            raw_hash.update(chunk)
            if pending and len(pending) + len(chunk) > block_target:
                c = zlib.compress(bytes(pending), level)
                blocks.append([pending_start, 0, len(c), len(pending), 0, digest8(pending)])
                compressed_parts.append(c)
                pending_start = off
                pending = bytearray()
            pending += chunk

        if pending:
            c = zlib.compress(bytes(pending), level)
            blocks.append([pending_start, 0, len(c), len(pending), 0, digest8(pending)])
            compressed_parts.append(c)

    abi = HDR.unpack(open(src, "rb").read(HDR.size))[3]
    toc_size = TOC.size * len(blocks)
    data_offset = FILEH.size + toc_size
    cur = data_offset
    for b in blocks:
        b[1] = cur
        cur += b[2]

    with open(dst, "wb") as o:
        o.write(FILEH.pack(MAGIC, 1, abi, src_size, block_target, len(blocks)))
        for b in blocks:
            o.write(TOC.pack(*b))
        for c in compressed_parts:
            o.write(c)

    return {"original": src_size, "compressed": os.path.getsize(dst), "blocks": len(blocks),
            "abi": abi, "raw_hash": raw_hash.hexdigest(), "toc_offset": FILEH.size,
            "data_offset": data_offset}


def verify(src, dst, expected_hash=None):
    """Full round-trip: reconstruct the original byte stream and compare."""
    src_size = os.path.getsize(src)
    with open(dst, "rb") as f:
        h = f.read(FILEH.size)
        magic, ver, abi, orig_size, block_target, n = FILEH.unpack(h)
        if magic != MAGIC or ver != 1 or orig_size != src_size:
            sys.exit(f"bad DS2SPVZ1 header: {magic!r} v{ver} orig={orig_size} expected={src_size}")
        toc = [list(TOC.unpack(f.read(TOC.size))) for _ in range(n)]
        h2 = hashlib.blake2b(digest_size=16)
        with open(src, "rb") as s:
            h2.update(s.read(HDR.size))  # header is stored verbatim
            count = 0
            for b in toc:
                f.seek(b[1])
                raw = zlib.decompress(f.read(b[2]))
                if len(raw) != b[3] or digest8(raw) != b[5]:
                    sys.exit(f"block at virtual {b[0]} failed checksum")
                h2.update(raw)
                count += 1
            got = h2.hexdigest()
            ref = hashlib.blake2b(digest_size=16)
            with open(src, "rb") as s:
                while True:
                    buf = s.read(8 << 20)
                    if not buf:
                        break
                    ref.update(buf)
            ref = ref.hexdigest()
    print(f"verify         : {count} blocks reconstructed, {orig_size:,} bytes")
    print(f"stream digest  : {got}")
    print(f"original digest: {ref}")
    print("ROUND-TRIP     : " + ("OK - byte identical" if got == ref else "MISMATCH"))
    return got == ref


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("dst", nargs="?")
    ap.add_argument("--level", type=int, default=6)
    ap.add_argument("--block", type=int, default=1 << 20)
    ap.add_argument("--verify", action="store_true")
    a = ap.parse_args()

    if a.verify:
        ok = verify(a.src, a.dst)
        sys.exit(0 if ok else 1)

    if not a.dst:
        ap.error("output path required (or use --verify)")

    info = compress(a.src, a.dst, a.level, a.block)
    ratio = info["original"] / info["compressed"]
    print(f"source         : {a.src}")
    print(f"  size         : {info['original']:,} B ({info['original'] / 2**20:,.1f} MiB)")
    print(f"output         : {a.dst}")
    print(f"  size         : {info['compressed']:,} B ({info['compressed'] / 2**20:,.1f} MiB)")
    print(f"  ratio        : x{ratio:.2f}  (saved {(info['original'] - info['compressed']) / 2**20:,.1f} MiB)")
    print(f"  blocks       : {info['blocks']:,} @ {a.block:,} B target, zlib level {a.level}")
    print(f"  ABI          : {info['abi']}  TOC at {info['toc_offset']}, data at {info['data_offset']:,}")
    verify(a.src, a.dst, info["raw_hash"])
