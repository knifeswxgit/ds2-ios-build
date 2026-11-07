#!/usr/bin/env python3
"""Pull DS2 logs from the connected iPhone over USB and print the tail.

Usage:  python pull_logs.py [--tail N] [--save DIR]
"""
import argparse
import asyncio
import pathlib
import shutil
import sys

from pymobiledevice3.lockdown import create_using_usbmux
from pymobiledevice3.services.house_arrest import HouseArrestService

BUNDLE_ID = "com.yourteam.ds2ios"


async def collect(service: HouseArrestService, out: pathlib.Path) -> list[pathlib.Path]:
    pulled = []
    out.mkdir(parents=True, exist_ok=True)

    async def pull_file(remote: str) -> pathlib.Path | None:
        dst = out / pathlib.Path(remote).name
        try:
            await service.pull(remote, str(dst))
            pulled.append(dst)
            return dst
        except Exception as e:
            print(f"  skip {remote}: {type(e).__name__}")
            return None

    print("Documents:", await service.listdir("/Documents"))
    try:
        for name in await service.listdir("/Documents/logs"):
            if name.endswith(".log"):
                await pull_file(f"/Documents/logs/{name}")
    except Exception as e:
        print(f"  logs/: {type(e).__name__}")

    if await service.exists("/Documents/ds2_log.txt"):
        await pull_file("/Documents/ds2_log.txt")
    return pulled


async def main_async(tail: int, save: str | None) -> int:
    lockdown = await create_using_usbmux()
    service = HouseArrestService(lockdown=lockdown)
    await service.send_command(BUNDLE_ID)

    out = pathlib.Path(save) if save else pathlib.Path(__file__).resolve().parents[1] / "out" / "device_logs"
    pulled = await collect(service, out)
    if not pulled:
        print("нет логов — приложение ещё не записало их")
        return 1

    newest = max(pulled, key=lambda p: p.stat().st_mtime)
    print(f"\n=== {newest.name} (последние {tail} строк) ===")
    lines = newest.read_text(encoding="utf-8", errors="replace").splitlines()
    for line in lines[-tail:]:
        print(line)
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--tail", type=int, default=80)
    ap.add_argument("--save", default=None, help="куда сохранить (по умолчанию ios_port/out/device_logs)")
    a = ap.parse_args()
    try:
        return asyncio.run(main_async(a.tail, a.save))
    except Exception as e:
        print(f"ошибка: {type(e).__name__}: {e}")
        print("проверь, что iPhone подключён по USB и доверяет этому ПК")
        return 1


if __name__ == "__main__":
    sys.exit(main())
