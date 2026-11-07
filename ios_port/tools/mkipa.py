#!/usr/bin/env python3
"""Standalone .ipa builder - no Xcode, no macOS, no external tools required.

Builds Payload/<App>.app/, writes a correctly structured .ipa, and can verify
one back. Runs on Windows/Linux/macOS. Signing is delegated to zsign when the
user supplies a P12 + mobileprovision (optional).

Already-compressed payloads (.spvz, .bdt, .fsb, .dcx, .wmv, ...) are STORED
instead of DEFLATEd: recompressing 500 MiB of zlib data wastes minutes and
makes the archive larger. The .ipa is just a container; iOS unpacks it anyway.

Usage:
  python mkipa.py --executable ds2 --plist Info.plist \
      --resource ds2_ios.toml --resource ds2_shaders.spvz --out ds2.ipa
  python mkipa.py --verify ds2.ipa
  python mkipa.py ... --sign --p12 cert.p12 --p12-pass secret --mp profile.mobileprovision
"""
import argparse
import hashlib
import os
import pathlib
import plistlib
import shutil
import struct
import subprocess
import sys
import zipfile

# Extensions that are already compressed - store them verbatim in the zip.
STORE_EXT = {".spvz", ".spvz.idx", ".bdt", ".bhd", ".dcx", ".fsb", ".wmv",
             ".mp4", ".png", ".jpg", ".jpeg", ".zip", ".gz", ".zst", ".7z", ".ipa"}
MACHO_64 = b"\xcf\xfa\xed\xfe"   # MH_MAGIC_64 little-endian
MACHO_32 = b"\xce\xfa\xed\xfe"
FAT_MAGIC = b"\xca\xfe\xba\xbe"


def is_stored(name):
    low = name.lower()
    return any(low.endswith(e) for e in STORE_EXT)


def check_executable(path):
    with open(path, "rb") as f:
        head = f.read(8)
    if head[:4] in (MACHO_64, MACHO_32, FAT_MAGIC):
        kind = "Mach-O 64" if head[:4] == MACHO_64 else ("Mach-O 32" if head[:4] == MACHO_32 else "FAT")
        cpu = None
        if head[:4] == MACHO_64:
            with open(path, "rb") as f:
                f.seek(4)
                cpu = struct.unpack("<I", f.read(4))[0]
            if cpu != 0x0100000C:  # CPU_TYPE_ARM64
                print(f"  WARNING: cputype 0x{cpu:08X} is not arm64 - this will not run on an iPhone")
        return kind
    if head[:2] == b"MZ":
        return "PE (NOT an iOS binary)"
    if head[:4] == b"\x7fELF":
        return "ELF (NOT an iOS binary)"
    return f"unknown magic {head[:4].hex()}"


def build(exe, plist, resources, out, bundle_id=None, display_name=None, sign=None):
    exe = pathlib.Path(exe)
    plist_path = pathlib.Path(plist)
    with open(plist_path, "rb") as f:
        info = plistlib.load(f)
    name = display_name or info.get("CFBundleName", "App")
    app_dir = pathlib.Path(name + ".app")

    kind = check_executable(exe)
    print(f"executable    : {exe}  [{kind}]")
    exe_name = info.get("CFBundleExecutable", "ds2")
    if bundle_id:
        info["CFBundleIdentifier"] = bundle_id
    if display_name:
        info["CFBundleName"] = name
        info["CFBundleDisplayName"] = name
    info.setdefault("CFBundleExecutable", exe_name)
    info.setdefault("CFBundlePackageType", "APPL")
    info.setdefault("MinimumOSVersion", "17.0")
    info.setdefault("UIFileSharingEnabled", True)
    info.setdefault("LSSupportsOpeningDocumentsInPlace", True)

    items = [(f"Payload/{app_dir.name}/{exe_name}", exe, 0o755)]
    # Serialize the (possibly patched) plist rather than copying the source file.
    tmp_plist = pathlib.Path(out).parent / "_Info.plist.generated"
    tmp_plist.parent.mkdir(parents=True, exist_ok=True)
    with open(tmp_plist, "wb") as f:
        plistlib.dump(info, f)
    items.append((f"Payload/{app_dir.name}/Info.plist", tmp_plist, 0o644))

    for r in resources:
        p = pathlib.Path(r)
        if p.is_dir():
            for sub in sorted(p.rglob("*")):
                if sub.is_file():
                    rel = sub.relative_to(p.parent)
                    items.append((f"Payload/{app_dir.name}/{rel.as_posix()}", sub, 0o644))
        elif p.is_file():
            items.append((f"Payload/{app_dir.name}/{p.name}", p, 0o644))
        else:
            sys.exit(f"resource not found: {p}")

    out = pathlib.Path(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    total = 0
    with zipfile.ZipFile(out, "w", allowZip64=True) as z:
        for arcname, src, mode in items:
            zi = zipfile.ZipInfo(arcname)
            zi.external_attr = (0o100000 | mode) << 16
            zi.create_system = 3  # UNIX, so the mode bits survive
            zi.compress_type = zipfile.ZIP_STORED if is_stored(arcname) else zipfile.ZIP_DEFLATED
            # Stream: BDT archives are 2.7 GB each, never read them into RAM.
            size = pathlib.Path(src).stat().st_size
            zi.file_size = size
            with open(src, "rb") as r, z.open(zi, "w", force_zip64=size > 100 << 20) as w:
                shutil.copyfileobj(r, w, length=4 << 20)
            total += size
    tmp_plist.unlink(missing_ok=True)

    print(f"bundle id     : {info['CFBundleIdentifier']}")
    print(f"resources     : {len(items) - 2}")
    print(f"uncompressed  : {total:,} B ({total / 2**20:,.1f} MiB)")
    print(f"ipa           : {out}  ({out.stat().st_size:,} B)")

    if sign:
        sign_ipa(out, sign)
    return out


def sign_ipa(ipa, opt):
    if not shutil.which("zsign"):
        sys.exit("zsign not found in PATH - install it or sign locally with your own tool")
    cmd = ["zsign", "-k", opt["p12"], "-p", opt["pass"], "-m", opt["mp"]]
    signed = ipa.with_name(ipa.stem + "-signed.ipa")
    cmd += ["-o", str(signed), str(ipa)]
    print("signing       :", " ".join(cmd[:1] + cmd[-2:]))
    subprocess.run(cmd, check=True)
    print(f"signed ipa    : {signed} ({signed.stat().st_size:,} B)")


def verify(ipa):
    ipa = pathlib.Path(ipa)
    print(f"verifying     : {ipa}")
    with zipfile.ZipFile(ipa) as z:
        names = z.namelist()
        apps = {n.split("/")[1] for n in names if n.startswith("Payload/") and "/" in n[8:]}
        if len(apps) != 1:
            sys.exit(f"expected exactly one .app in Payload/, got {apps}")
        app = apps.pop()
        prefix = f"Payload/{app}/"
        if prefix + "Info.plist" not in names:
            sys.exit("Info.plist missing")
        info = plistlib.loads(z.read(prefix + "Info.plist"))
        exe = prefix + info.get("CFBundleExecutable", "")
        if exe not in names:
            sys.exit(f"executable {info.get('CFBundleExecutable')} missing")
        magic = z.read(exe)[:4]
        ok_macho = magic in (MACHO_64, MACHO_32, FAT_MAGIC)
        worst = z.testzip()
        total = sum(i.file_size for i in z.infolist())
        stored = sum(1 for i in z.infolist() if i.compress_type == zipfile.ZIP_STORED)
        print(f"  app         : {app}")
        print(f"  bundle id   : {info.get('CFBundleIdentifier')}")
        print(f"  executable  : {info.get('CFBundleExecutable')}  magic={magic.hex()} "
              f"{'Mach-O OK' if ok_macho else 'NOT Mach-O'}")
        print(f"  entries     : {len(names)} ({stored} stored uncompressed)")
        print(f"  size        : {total:,} B uncompressed / {ipa.stat().st_size:,} B packed")
        print(f"  crc         : {'OK' if worst is None else 'BAD at ' + worst}")
        print(f"  VERDICT     : {'STRUCTURE OK' if ok_macho and worst is None else 'FAILED'}")
        return ok_macho and worst is None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--executable")
    ap.add_argument("--plist")
    ap.add_argument("--resource", action="append", default=[])
    ap.add_argument("--out")
    ap.add_argument("--bundle-id")
    ap.add_argument("--display-name")
    ap.add_argument("--verify", metavar="IPA")
    ap.add_argument("--sign", action="store_true")
    ap.add_argument("--p12")
    ap.add_argument("--p12-pass", default="")
    ap.add_argument("--mp")
    a = ap.parse_args()

    if a.verify:
        return 0 if verify(a.verify) else 1
    if not (a.executable and a.plist and a.out):
        ap.error("--executable, --plist and --out are required (or use --verify)")
    sign = {"p12": a.p12, "pass": a.p12_pass, "mp": a.mp} if a.sign else None
    build(a.executable, a.plist, a.resource, a.out, a.bundle_id, a.display_name, sign)
    return 0


if __name__ == "__main__":
    sys.exit(main())
