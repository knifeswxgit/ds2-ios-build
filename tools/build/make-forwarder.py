#!/usr/bin/env python3
"""Package a standalone NSP shortcut to an existing NRO on the SD card."""

import argparse
import hashlib
import re
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path


TITLE_ID = "0100d52000560000"
SDMC_NRO = "sdmc:/switch/ds2/ds2.nro"
LOADER_HASHES = {
    "main": "b8089e39f63af1827c9a41898c7a70be18cab447e6f41af3e1aa8fe1c4afafe8",
    "main.npdm": "cf3236ed4eaa743fea90678f5dc73c608221ee52429ebe0e93c74aa596d6cc8f",
}


def nro_assets(nro):
    if nro[0x10:0x14] != b"NRO0":
        raise ValueError("invalid NRO")
    size = struct.unpack_from("<I", nro, 0x18)[0]
    if nro[size : size + 4] != b"ASET" or len(nro) < size + 0x38:
        raise ValueError("NRO has no asset header")
    assets = []
    for offset in (8, 0x18):
        position, length = struct.unpack_from("<QQ", nro, size + offset)
        if position < 0x38 or size + position + length > len(nro):
            raise ValueError("invalid NRO asset range")
        assets.append(nro[size + position : size + position + length])
    icon, nacp = assets
    if not icon.startswith(b"\xff\xd8") or len(nacp) != 0x4000:
        raise ValueError("NRO needs a JPEG icon and a NACP")
    return icon, nacp


def verify_loader(path):
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    if digest != LOADER_HASHES[path.name]:
        raise ValueError(f"loader {path.name} differs from the pinned NTON binary")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("nro", type=Path, help="ds2.nro whose icon, title, publisher and version will be embedded in the NSP")
    parser.add_argument("output", type=Path, help="new NSP file path (must not exist)")
    parser.add_argument("--keys", required=True, type=Path, help="local prod.keys; never copied into the NSP")
    parser.add_argument("--packer", required=True, type=Path, help="local hacbrewpack executable")
    parser.add_argument("--loader-dir", required=True, type=Path, help="NTON exefs directory, pinned to c5d543a91b7c")
    args = parser.parse_args()
    if args.output.exists():
        parser.error("output already exists; refusing to overwrite")
    if not args.keys.is_file() or not args.packer.is_file():
        parser.error("keyset or packer not found")
    loader = {name: (args.loader_dir / name).resolve() for name in LOADER_HASHES}
    for path in loader.values():
        if not path.is_file():
            parser.error(f"loader {path.name} not found")
        verify_loader(path)
    if not re.fullmatch(r"01[0-9a-f]{14}", TITLE_ID):
        raise AssertionError("forwarder Title ID must be 16 hexadecimal digits")
    icon, nacp = nro_assets(args.nro.read_bytes())
    if not nacp[0x3060:0x3070].split(b"\0")[0]:
        parser.error("NRO has no display version")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="ds2-forwarder-") as temporary:
        stage = Path(temporary)
        for directory in ("exefs", "control", "romfs", "nsp", "nca", "temp", "backup"):
            (stage / directory).mkdir()
        for name, source in loader.items():
            shutil.copyfile(source, stage / "exefs" / name)
        (stage / "control" / "control.nacp").write_bytes(nacp)
        (stage / "control" / "icon_AmericanEnglish.dat").write_bytes(icon)
        (stage / "romfs" / "nextNroPath").write_text(SDMC_NRO, encoding="ascii")
        (stage / "romfs" / "nextArgv").write_text(SDMC_NRO, encoding="ascii")
        command = [
            str(args.packer.resolve()), "--titleid", TITLE_ID, "--nologo",
            "--nspdir", str(stage / "nsp"), "--ncadir", str(stage / "nca"),
            "--tempdir", str(stage / "temp"), "--backupdir", str(stage / "backup"),
            "--exefsdir", str(stage / "exefs"), "--controldir", str(stage / "control"),
            "--romfsdir", str(stage / "romfs"), "-k", str(args.keys.resolve()),
        ]
        result = subprocess.run(command, cwd=stage, capture_output=True, text=True, check=False)
        if result.returncode:
            raise RuntimeError(f"hacbrewpack exited with status {result.returncode}; output withheld to protect keys")
        package = stage / "nsp" / f"{TITLE_ID}.nsp"
        if not package.is_file() or package.stat().st_size < 0x20:
            raise RuntimeError("hacbrewpack did not produce an NSP")
        with package.open("rb") as source, args.output.open("xb") as destination:
            shutil.copyfileobj(source, destination)
    with args.output.open("rb") as package:
        if package.read(4) != b"PFS0":
            raise ValueError("invalid NSP container")
    print("title ID:", TITLE_ID)
    print("forwarded path:", SDMC_NRO)
    print("NSP sha256:", hashlib.sha256(args.output.read_bytes()).hexdigest())


if __name__ == "__main__":
    main()
