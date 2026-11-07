#!/usr/bin/env python3
"""Read-only analyzer for DS2_switch shader packs (DS2SPVL1 / DSS2IDX1).

Format derived from DS2_switch_src/sdk/src/graphics/pipeline/shader/spirv_library.cpp:

  .spvlib header : <4I  (magic "DS2S", magic "PVL1", 1, Modification::kVersion)
  record         : <10Q (80 bytes)
      [0] ucode_data_hash   [1] modification   [2] translator signature
      [3] shader type (0/1) [4] ucode_dword_count  [5] spirv byte count
      [6] texture bindings  [7] sampler bindings   [8] used_texture_mask
      [9] XXH3_64(payload)
  payload        : ucode(r[4]*4) | SPIR-V(r[5]) | tex(r[6]*4) | smp(r[7]*4)
  .idx header    : <6Q (magic "DSS2IDX1", 1, spvlib size, count, XXH3, ABI)
  .idx records   : <5Q (key[0..3], offset), sorted by key

Usage: python spvlib_analyze.py <pack.spvlib> [--hash]
"""
import argparse
import collections
import hashlib
import os
import struct
import sys
import zlib

HDR = struct.Struct("<4I")
REC = struct.Struct("<10Q")
IDXH = struct.Struct("<6Q")
IDXR = struct.Struct("<5Q")

SPVL_MAGIC = b"DS2SPVL1"
IDX_MAGIC = 0x3158444932535344  # "DSS2IDX1" little-endian


def read_index(path, spv_size):
    with open(path, "rb") as f:
        h = f.read(IDXH.size)
        if len(h) != IDXH.size:
            return None
        magic, ver, size, count, xxh, abi = IDXH.unpack(h)
        return {"magic": magic, "file_size": size, "count": count, "abi": abi,
                "bytes_expected": count * IDXR.size}


def analyze(pack, do_hash=True):
    size = os.path.getsize(pack)
    with open(pack, "rb") as f:
        head = f.read(HDR.size)
        if len(head) != HDR.size:
            sys.exit("truncated header")
        a, b, ver, abi = HDR.unpack(head)
        if struct.pack("<2I", a, b) != SPVL_MAGIC:
            sys.exit(f"bad magic: {head[:8]!r}")
        print(f"pack          : {pack}")
        print(f"size          : {size:,} bytes ({size / 2**20:,.1f} MiB)")
        print(f"header        : version={ver} ABI={abi}")

        idx = pack + ".idx"
        if os.path.exists(idx):
            info = read_index(idx, size)
            print(f"index         : {info['count']:,} records, ABI={info['abi']}, "
                  f"points to {info['file_size']:,} bytes (pack {size:,})")
            if info["file_size"] != size:
                print("  !! index file_size does not match pack")

        off = HDR.size
        n = 0
        total_payload = 0
        total_ucode = 0
        total_spirv = 0
        total_meta = 0
        type_hist = collections.Counter()
        mod_hist = collections.Counter()
        seen = {}
        dup_records = 0
        dup_bytes = 0
        uniq_spirv = 0
        compressed_sample = 0
        sample_in = 0
        bad = 0

        while off < size:
            if size - off < REC.size:
                bad += 1
                break
            r = REC.unpack(f.read(REC.size))
            # PayloadSize() validation, exactly as in the C++ source.
            if r[3] > 1 or r[4] > 262144 or not r[5] or r[5] > (4 << 20) or (r[5] & 3) or r[6] > 64 or r[7] > 64:
                print(f"  !! invalid record at {off:,}: {r}")
                bad += 1
                break
            payload = r[4] * 4 + r[5] + (r[6] + r[7]) * 4
            if payload > size - off - REC.size:
                print(f"  !! truncated payload at {off:,}")
                bad += 1
                break
            body = f.read(payload)
            if len(body) != payload:
                bad += 1
                break

            ucode_end = r[4] * 4
            spirv_end = ucode_end + r[5]
            spirv = body[ucode_end:spirv_end]
            total_payload += payload
            total_ucode += ucode_end
            total_spirv += r[5]
            total_meta += (r[6] + r[7]) * 4
            type_hist[r[3]] += 1
            mod_hist[r[1]] += 1
            n += 1

            if do_hash:
                h = hashlib.blake2b(spirv, digest_size=16).digest()
                if h in seen:
                    dup_records += 1
                    dup_bytes += r[5]
                else:
                    seen[h] = r[5]
                    uniq_spirv += r[5]

            # Sample-compress a limited number of payloads to estimate ratio.
            if sample_in < 64 << 20:
                compressed_sample += len(zlib.compress(body, 1))
                sample_in += payload

            off += REC.size + payload

        print(f"records       : {n:,} (bad/trailing: {bad})")
        print(f"record hdrs   : {n * REC.size:,.0f} bytes")
        print(f"payload total : {total_payload:,} bytes")
        print(f"  ucode prefix: {total_ucode:,} ({total_ucode / total_payload:.1%})  <- duplicated from game shader microcode")
        print(f"  SPIR-V      : {total_spirv:,} ({total_spirv / total_payload:.1%})")
        print(f"  bindings    : {total_meta:,} ({total_meta / total_payload:.1%})")
        print(f"types         : {dict(type_hist)}")
        print(f"modifications : {len(mod_hist):,} distinct (top 5: {mod_hist.most_common(5)})")
        if do_hash:
            print(f"unique SPIR-V : {len(seen):,} blobs / {uniq_spirv:,} bytes")
            print(f"duplicates    : {dup_records:,} records / {dup_bytes:,} bytes "
                  f"({dup_bytes / total_spirv:.1%} of SPIR-V)")
        if sample_in:
            print(f"zlib -1 sample: {sample_in:,} -> {compressed_sample:,} bytes "
                  f"(x{sample_in / compressed_sample:.2f})")

        # Projection for the proposed dedup + compressed-store pack.
        if do_hash:
            store = uniq_spirv + total_ucode + total_meta + n * REC.size
            zratio = sample_in / compressed_sample if compressed_sample else 1.0
            print()
            print("PROJECTION (dedup SPIR-V store + compressed per-record payload):")
            print(f"  unique blobs + ucode + bindings + index : {store:,} bytes ({store / 2**20:,.1f} MiB)")
            print(f"  after zlib -1 on the same mix          : ~{store / zratio:,.0f} bytes "
                  f"(~{store / zratio / 2**20:,.1f} MiB)")
        return n


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("pack")
    ap.add_argument("--no-hash", action="store_true", help="skip duplicate detection (faster)")
    args = ap.parse_args()
    analyze(args.pack, do_hash=not args.no_hash)
