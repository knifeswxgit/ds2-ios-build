#!/usr/bin/env python3
"""Build the slim DS2 bundle manifest for iOS (no game files are deleted).

Read-only by default. Replaces/removes targets and writes a manifest that the
.ipa packaging step can consume. With --materialize it creates the slim tree
via NTFS hardlinks (instant, no duplicate disk usage, originals untouched).

Cuts (measured against the local v121 release):
  ds2_shaders.spvlib(+.idx)  -> ds2_shaders.spvz(+.spvz.idx)   -1141 MiB
  dlc/ (PIRS X360 DLC)                                         -1176 MiB
  movie/prologue.wmv                                            -284 MiB
  10 non-Russian font dirs                                       ~-6 MiB
  AvatarAssetPack, nxeart (X360 dashboard assets)                -15 MiB
"""
import argparse
import os
import pathlib
import sys

LANG_DIRS = {"Chinese", "English", "French", "Germany", "Italian",
             "Japanese", "Korean", "Polish", "Portuguese", "Spanish"}
DROP_NAMES = {"AvatarAssetPack", "nxeart", "prologue.wmv", "Thumbs.db"}
DROP_PARTS = {"dlc"}


def human(n):
    return f"{n / 2**20:,.1f} MiB"


def scan(root):
    keep, drop = [], []
    for p in sorted(root.rglob("*")):
        if not p.is_file():
            continue
        rel = p.relative_to(root)
        parts = set(rel.parts)
        reason = None
        if rel.name in ("ds2_shaders.spvlib", "ds2_shaders.spvlib.idx"):
            reason = "replaced by compressed .spvz"
        elif parts & DROP_PARTS:
            reason = "DLC dropped"
        elif rel.name in DROP_NAMES:
            reason = "X360 dashboard asset / intro movie"
        elif "font" in parts and any(d in LANG_DIRS for d in parts):
            reason = "non-Russian font"
        size = p.stat().st_size
        (drop if reason else keep).append((rel, size, reason))
    return keep, drop


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root", help="switch/ds2 folder from the release")
    ap.add_argument("--compressed", default=None,
                    help="folder holding ds2_shaders.spvz and .spvz.idx")
    ap.add_argument("--manifest", default=None, help="write slim_manifest.txt here")
    ap.add_argument("--materialize", default=None, help="create slim tree here (hardlinks)")
    a = ap.parse_args()

    root = pathlib.Path(a.root)
    keep, drop = scan(root)
    raw = sum(s for _, s, _ in keep) + sum(s for _, s, _ in drop)
    slim = sum(s for _, s, _ in keep)
    extra = []
    if a.compressed:
        c = pathlib.Path(a.compressed)
        for name in ("ds2_shaders.spvz", "ds2_shaders.spvz.idx"):
            p = c / name
            if p.exists():
                extra.append((p, p.stat().st_size))
                slim += p.stat().st_size
            else:
                print(f"WARN missing compressed file: {p}")

    print(f"source        : {root}")
    print(f"files         : {len(keep)} kept / {len(drop)} dropped")
    print(f"original      : {human(raw)}")
    print(f"slim          : {human(slim)}  (saved {human(raw - slim)}, x{raw / slim:.2f})")
    if extra:
        print(f"compressed add: {human(sum(s for _, s in extra))}")
    print("\nBy reason:")
    reasons = {}
    for _, s, r in drop:
        reasons[r] = reasons.get(r, 0) + s
    for r, s in sorted(reasons.items(), key=lambda kv: -kv[1]):
        print(f"  {r:38s} {human(s)}")

    lines = [f"# slim DS2 bundle manifest ({human(slim)})",
             "# format: <relative-path>\\t<size-bytes>\\t<source-root-tag>"]
    for rel, size, _ in keep:
        lines.append(f"{rel.as_posix()}\t{size}\tgame")
    for p, size in extra:
        lines.append(f"{p.name}\t{size}\tcompressed")
    if a.manifest:
        pathlib.Path(a.manifest).write_text("\n".join(lines) + "\n", encoding="utf-8")
        print(f"\nmanifest      : {a.manifest}")

    if a.materialize:
        dst = pathlib.Path(a.materialize)
        for rel, _, _ in keep:
            src, out = root / rel, dst / rel
            out.parent.mkdir(parents=True, exist_ok=True)
            if out.exists():
                continue
            try:
                os.link(src, out)
            except OSError:
                import shutil
                shutil.copy2(src, out)
        for p, _ in extra:
            out = dst / p.name
            out.parent.mkdir(parents=True, exist_ok=True)
            if not out.exists():
                import shutil
                shutil.copy2(p, out)
        print(f"materialized  : {dst} (hardlinks; originals untouched)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
