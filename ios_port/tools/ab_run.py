#!/usr/bin/env python3
"""One-shot A/B run for the classic Xenos -> Vulkan path on a connected iPhone.

The DS2 iOS port is CPU/GPU bound and every setting has to be measured in the
same scene, so this script does the whole loop in one command:

  1. push a set of cvars into Documents/ds2.toml (over USB/AFC, no rebuild),
  2. relaunch the app with `dvt launch --kill-existing`,
  3. wait for the boot (default 30 s),
  4. sample CoreAnimation FPS for N seconds,
  5. print a short summary and optionally restore the previous config.

The caller must have loaded a save (gameplay) on the phone; the script cannot
do that itself. Take a screenshot with `dvt screenshot` to confirm the scene.

Examples
--------
    python ab_run.py --set texture_cache_memory_limit_soft=768 \
        --set texture_cache_memory_limit_hard=1536 --label cache768

    python ab_run.py --restore --label baseline
"""
import argparse
import asyncio
import json
import pathlib
import re
import subprocess
import sys
import tempfile

from pymobiledevice3.lockdown import create_using_usbmux
from pymobiledevice3.services.house_arrest import HouseArrestService

BUNDLE_ID = "com.yourteam.ds2ios"
CONFIG = "/Documents/ds2.toml"
BACKUP = pathlib.Path(__file__).resolve().parent.parent / "out" / "device_ds2_config_backup.toml"


def pmd3(*args, timeout=180):
    cmd = [sys.executable, "-m", "pymobiledevice3", *args]
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)


async def pull_config():
    lockdown = await create_using_usbmux()
    svc = HouseArrestService(lockdown=lockdown)
    await svc.send_command(BUNDLE_ID)
    with tempfile.TemporaryDirectory() as td:
        local = pathlib.Path(td) / "ds2.toml"
        await svc.pull(CONFIG, str(local))
        return local.read_text(encoding="utf-8")


async def push_config(text):
    lockdown = await create_using_usbmux()
    svc = HouseArrestService(lockdown=lockdown)
    await svc.send_command(BUNDLE_ID)
    with tempfile.TemporaryDirectory() as td:
        local = pathlib.Path(td) / "ds2.toml"
        local.write_text(text, encoding="utf-8", newline="")
        await svc.push(str(local), CONFIG)


def set_values(text, pairs):
    lines = text.splitlines(keepends=True)
    for item in pairs:
        if "=" not in item:
            raise SystemExit(f"bad --set {item!r}; expected KEY=VALUE")
        key, value = (p.strip() for p in item.split("=", 1))
        pat = re.compile(rf"^\s*{re.escape(key)}\s*=")
        for i, line in enumerate(lines):
            if pat.match(line):
                eol = "\r\n" if line.endswith("\r\n") else "\n"
                lines[i] = f"{key} = {value}  # set by ab_run.py{eol}"
                break
        else:
            lines.append(f"\n{key} = {value}  # added by ab_run.py\n")
    return "".join(lines)


def sample_fps(seconds):
    proc = pmd3("developer", "dvt", "fps", timeout=seconds + 30)
    values = []
    for m in re.finditer(r'"fps":\s*([0-9.]+)', proc.stdout):
        values.append(float(m.group(1)))
    return values


def summarize(values):
    if not values:
        return "no samples"
    game = [v for v in values if 0 < v < 30]
    if not game:
        return "all samples >= 30 fps (title screen / idle?)"
    game_sorted = sorted(game)
    mid = len(game_sorted) // 2
    return (
        f"n={len(game)} min={min(game):g} mean={sum(game) / len(game):.2f} "
        f"median={game_sorted[mid]:g} max={max(game):g} "
        f"zeros={values.count(0)}"
    )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--set", action="append", default=[], metavar="KEY=VALUE")
    ap.add_argument("--label", default="run")
    ap.add_argument("--boot", type=int, default=30, help="seconds to wait after launch")
    ap.add_argument("--measure", type=int, default=30, help="seconds of FPS samples")
    ap.add_argument("--restore", action="store_true", help="restore the saved backup and exit")
    ap.add_argument("--backup-only", action="store_true", help="save the current config and exit")
    a = ap.parse_args()

    if a.restore:
        if not BACKUP.exists():
            raise SystemExit(f"no backup at {BACKUP}")
        asyncio.run(push_config(BACKUP.read_text(encoding="utf-8")))
        print(f"restored {BACKUP}")
        return

    current = asyncio.run(pull_config())
    if a.backup_only:
        BACKUP.parent.mkdir(parents=True, exist_ok=True)
        BACKUP.write_text(current, encoding="utf-8", newline="")
        print(f"backup -> {BACKUP} ({len(current)} B)")
        return

    BACKUP.parent.mkdir(parents=True, exist_ok=True)
    BACKUP.write_text(current, encoding="utf-8", newline="")

    text = set_values(current, a.set) if a.set else current
    asyncio.run(push_config(text))
    print(f"[{a.label}] pushed {len(a.set)} cvar(s)")

    pmd3("developer", "dvt", "launch", "--kill-existing", BUNDLE_ID, timeout=120)
    print(f"[{a.label}] launched, waiting {a.boot}s")
    import time

    time.sleep(a.boot)
    values = sample_fps(a.measure)
    print(f"[{a.label}] {summarize(values)}")
    print(json.dumps(values))


if __name__ == "__main__":
    main()
