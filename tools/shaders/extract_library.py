#!/usr/bin/env python3
import argparse
import hashlib
import json
from pathlib import Path
import runpy
import struct
import zlib

parser = argparse.ArgumentParser()
parser.add_argument("game_root", type=Path)
parser.add_argument("output", type=Path)
args = parser.parse_args()
if args.output.exists():
    raise SystemExit("output must be a new directory")
args.output.mkdir(parents=True)
bnd4 = runpy.run_path(str(Path(__file__).resolve().parents[1] / "analysis" / "bnd4.py"))["bnd4"]
entries = []
for archive in sorted((args.game_root / "shader").glob("*.bnd")):
    archive_data = archive.read_bytes()
    for name, offset, compressed_size, _ in bnd4(archive_data):
        if not name.lower().endswith((".fpo", ".vpo")):
            continue
        data = archive_data[offset:offset + compressed_size]
        if len(data) >= 2 and data[0] == 0x78:
            data = zlib.decompress(data)
        if len(data) < 0x24:
            raise SystemExit(f"truncated shader: {archive.name}/{name}")
        magic, virtual_size, physical_size = struct.unpack_from(">III", data)
        shader_offset = struct.unpack_from(">I", data, 0x18)[0]
        if (magic & 0xFFFFFF00) != 0x102A1100 or shader_offset + 0x18 > len(data):
            raise SystemExit(f"invalid shader header: {archive.name}/{name}")
        physical_offset, code_size = struct.unpack_from(">II", data, shader_offset)
        code_offset = virtual_size + physical_offset
        stage = magic & 1
        if not code_size or code_size % 4 or physical_offset + code_size > physical_size or code_offset + code_size > len(data):
            raise SystemExit(f"invalid shader bytecode: {archive.name}/{name}")
        code = data[code_offset:code_offset + code_size]
        filename = f"{len(entries):05d}_{'vs' if stage else 'ps'}.ucode"
        (args.output / filename).write_bytes(code)
        entries.append({"stage": "vs" if stage else "ps", "file": filename,
                        "source": f"{archive.name}/{name}", "sha256": hashlib.sha256(code).hexdigest()})
if not entries:
    raise SystemExit("no shader containers found")
(args.output / "manifest.txt").write_text("".join(f"{item['stage']} {item['file']}\n" for item in entries))
(args.output / "sources.json").write_text(json.dumps(entries, indent=2))
print(f"extracted {len(entries)} shader containers into {args.output}")
