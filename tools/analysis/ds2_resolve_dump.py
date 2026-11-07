#!/usr/bin/env python3
"""Decode resolve dumps (resolve_dump_dir cvar) into PNG images.

Usage: ds2_resolve_dump.py <dump_NN folder> [out folder]

Each r###_ADDR.bin is raw guest memory written by one resolve (tiled Xenos 2D layout, guest endianness).
index.txt has the copy registers. Writes r###_ADDR_<fmt>_<w>x<h>.png and prints min/max/mean per resolve,
so a frame that turns black shows at which resolve it happens.
"""
import os
import struct
import sys

import numpy as np
from PIL import Image

FORMATS = {6: ("8888", 2), 7: ("2_10_10_10", 2), 22: ("24_8", 2), 23: ("24_8F", 2), 25: ("16_16", 2),
           26: ("16x4", 3), 31: ("16_16F", 2), 32: ("16x4F", 3), 36: ("32F", 2), 37: ("32_32F", 3),
           2: ("8", 0), 10: ("8_8", 1), 30: ("16F", 1), 16: ("10_11_11", 2), 17: ("11_11_10", 2)}


def tiled_offset(x, y, pitch, bpb_log2):
    pitch = (pitch + 31) & ~31
    macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (bpb_log2 + 7)
    micro = ((x & 7) + ((y & 0xE) << 2)) << bpb_log2
    offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4)
    return (((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
            (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F))


def tiled_offsets(w, h, pitch, bpb_log2):
    ys, xs = np.mgrid[0:h, 0:w].astype(np.int64)
    pitch = (pitch + 31) & ~31
    macro = ((xs >> 5) + (ys >> 5) * (pitch >> 5)) << (bpb_log2 + 7)
    micro = ((xs & 7) + ((ys & 0xE) << 2)) << bpb_log2
    offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((ys & 1) << 4)
    return (((offset & ~0x1FF) << 3) + ((ys & 16) << 7) + ((offset & 0x1C0) << 2) +
            (((((ys & 8) >> 2) + (xs >> 3)) & 3) << 6) + (offset & 0x3F))


def swap(data, endian, bpp):
    a = np.frombuffer(data, dtype=np.uint8)
    if endian == 1:  # 8in16
        return a.reshape(-1, 2)[:, ::-1].reshape(-1)
    if endian == 2:  # 8in32
        return a.reshape(-1, 4)[:, ::-1].reshape(-1)
    if endian == 3:  # 16in32
        return a.reshape(-1, 2, 2)[:, ::-1, :].reshape(-1)
    return a


def decode(path, e, out_dir, name):
    raw = open(path, "rb").read()
    info = e["dest_info"]
    endian = info & 7
    fmt = (info >> 7) & 0x3F
    pitch = e["dest_pitch"] & 0x3FFF
    height = (e["dest_pitch"] >> 16) & 0x3FFF
    fname, bpb_log2 = FORMATS.get(fmt, ("fmt%d" % fmt, 2))
    bpp = 1 << bpb_log2
    # Place the written range relative to the destination base.
    start = e["addr"] - e["dest_base"]
    total = tiled_offset(pitch - 1, height - 1, pitch, bpb_log2) + bpp
    total = max(total, start + len(raw))
    buf = np.zeros(total + 64, dtype=np.uint8)
    if start < 0:
        return None
    buf[start:start + len(raw)] = np.frombuffer(raw, dtype=np.uint8)
    buf = swap(buf[: (len(buf) // 4) * 4].tobytes(), endian, bpp)
    offs = tiled_offsets(pitch, height, pitch, bpb_log2)
    idx = offs[..., None] + np.arange(bpp)
    px = buf[np.clip(idx, 0, len(buf) - 1)]  # h, w, bpp
    note = ""
    if bpp == 4:
        v = px.view("<u4")[..., 0] if px.flags["C_CONTIGUOUS"] else np.ascontiguousarray(px).view("<u4")[..., 0]
        if fname == "8888":
            rgb = np.stack([px[..., 2], px[..., 1], px[..., 0]], -1)  # BGRA in memory after swap
            alt = np.stack([px[..., 0], px[..., 1], px[..., 2]], -1)
            img = Image.fromarray(np.ascontiguousarray(rgb if (info >> 24) & 3 else alt))
            note = "a=%d..%d" % (px[..., 3].min(), px[..., 3].max())
        elif fname == "2_10_10_10":
            r = (v & 1023) >> 2
            g = ((v >> 10) & 1023) >> 2
            b = ((v >> 20) & 1023) >> 2
            img = Image.fromarray(np.stack([r, g, b], -1).astype(np.uint8))
        elif fname in ("24_8", "24_8F"):
            d = (v >> 8).astype(np.float64)
            d = (d - d.min()) / max(1.0, d.max() - d.min()) * 255
            img = Image.fromarray(d.astype(np.uint8))
            note = "depth"
        elif fname == "16_16F":
            f = np.ascontiguousarray(px).view("<f2").reshape(height, pitch, 2).astype(np.float32)
            note = "r %.3g..%.3g g %.3g..%.3g" % (f[..., 0].min(), f[..., 0].max(), f[..., 1].min(), f[..., 1].max())
            img = Image.fromarray((np.clip(np.nan_to_num(f[..., 0]), 0, 1) * 255).astype(np.uint8))
        elif fname == "32F":
            f = np.ascontiguousarray(px).view("<f4").reshape(height, pitch).astype(np.float32)
            note = "%.3g..%.3g nan=%d" % (np.nanmin(f), np.nanmax(f), np.isnan(f).sum())
            img = Image.fromarray((np.clip(np.nan_to_num(f), 0, 1) * 255).astype(np.uint8))
        else:
            img = Image.fromarray(np.ascontiguousarray(px[..., :3]))
    elif bpp == 8 and fname == "16x4F":
        f = np.ascontiguousarray(px).view("<f2").reshape(height, pitch, 4).astype(np.float32)
        nan = int(np.isnan(f).sum())
        note = "rgb %.3g..%.3g mean %.3g a %.3g..%.3g nan=%d inf=%d" % (
            np.nanmin(f[..., :3]), np.nanmax(f[..., :3]), np.nanmean(f[..., :3]), np.nanmin(f[..., 3]),
            np.nanmax(f[..., 3]), nan, int(np.isinf(f).sum()))
        img = Image.fromarray((np.clip(np.nan_to_num(f[..., :3]), 0, 1) * 255).astype(np.uint8))
    else:
        img = Image.fromarray(np.ascontiguousarray(px[..., 0]))
    arr = np.asarray(img).astype(np.float32)
    out = os.path.join(out_dir, "%s_%s_%dx%d.png" % (name, fname, pitch, height))
    img.save(out)
    return "%-24s %-10s %4dx%-4d mean=%6.1f max=%5.1f %s" % (name, fname, pitch, height, arr.mean(), arr.max(), note)


def main():
    src = sys.argv[1]
    out_dir = sys.argv[2] if len(sys.argv) > 2 else os.path.join(src, "png")
    os.makedirs(out_dir, exist_ok=True)
    for line in open(os.path.join(src, "index.txt")):
        if not line.startswith("r"):
            print(line.strip())
            continue
        parts = line.split()
        e = {}
        for p in parts[1:]:
            k, v = p.split("=")
            e[k] = int(v, 16) if k not in ("len", "draws") else int(v)
        e["addr"] = e.pop("addr")
        name = parts[0][:-4]
        try:
            r = decode(os.path.join(src, parts[0]), e, out_dir, name)
        except Exception as ex:  # keep going on odd formats
            r = "%s: %s" % (name, ex)
        print("%s draws=%d %s" % (r, e["draws"], "info=%08X" % e["dest_info"]))


if __name__ == "__main__":
    main()
