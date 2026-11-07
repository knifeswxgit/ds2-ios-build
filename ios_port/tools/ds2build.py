#!/usr/bin/env python3
"""One entry point for the whole DS2 -> .ipa build, on Windows and in CI.

Checks what this machine can do, runs the stages it can, and prints the exact
GitHub Actions command for the stages it cannot (codegen / Xcode build).

  python ds2build.py status              full readiness report
  python ds2build.py patch               re-apply iOS SDK patches
  python ds2build.py package             build a .ipa from a Mach-O (or verify)
  python ds2build.py ci                  print the commands to launch CI
"""
import argparse
import json
import os
import pathlib
import shutil
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent
PROJECT = ROOT.parent
CLONE = PROJECT / "DS2_switch_src"
OUT = ROOT / "out"
RELEASE = PROJECT / "darksouls_nintendo" / (
    "DARK SOULS II Scholar of the First Sin (homebrew port) for Nintendo Switch [NSP]"
) / "switch" / "ds2"


def tool(name):
    return shutil.which(name) is not None


def check():
    items = []
    def add(label, ok, note=""):
        items.append((label, bool(ok), note))

    add("DS2_switch clone", CLONE.is_dir(), str(CLONE))
    add("iOS patcher", (HERE / "apply_ios_patches.py").is_file())
    add("iOS toolchain", (ROOT / "cmake" / "ios-arm64.cmake").is_file())
    add("Info.plist", (ROOT / "Info.plist").is_file())
    add("compressed shader pack", (OUT / "ds2_shaders.spvz").is_file(),
        f"{(OUT / 'ds2_shaders.spvz').stat().st_size / 2**20:,.1f} MiB"
        if (OUT / "ds2_shaders.spvz").is_file() else "MISSING")
    add("shader .idx sidecar", (OUT / "ds2_shaders.spvz.idx").is_file())
    add("ds2_ios.toml", (OUT / "ds2_ios.toml").is_file())
    add("slim manifest", (OUT / "slim_manifest.txt").is_file())
    add("spvz_reader.h", (ROOT / "include" / "spvz_reader.h").is_file())
    add("full CI workflow", (ROOT / "ci" / "ds2-full.yml").is_file())
    add("Default.xex (for codegen)", (RELEASE / "game_root" / "Default.xex").is_file())
    add("generated ARM64 C++", (CLONE / "app" / "generated" / "diag-dataonly" / "sources.cmake").is_file(),
        "produced by the codegen CI job")
    add("built iOS Mach-O", any((CLONE / "out").rglob("ds2")) if (CLONE / "out").exists() else False,
        "produced by the macOS build job")

    add("git", tool("git"))
    add("python3", sys.version_info >= (3, 11))
    add("cmake", tool("cmake"))
    add("ninja", tool("ninja"), "needed by the Linux codegen job")
    add("clang", tool("clang"), "needed by the Linux codegen job")
    add("Xcode / iPhoneOS SDK", tool("xcodebuild"), "macOS only - use the CI runner")
    add("zsign", tool("zsign"), "optional, for signing with your own certificate")

    print("=== DS2 iOS build readiness ===")
    for label, ok, note in items:
        print(f"  [{'OK' if ok else '--'}] {label:30s} {note}")
    ready_local = all(ok for label, ok, _ in items[:11])
    print()
    print(f"local stages available : patch, package, verify, status")
    print(f"CI stages required     : codegen (Linux), build (macOS)")
    print(f"all local artifacts    : {'READY' if ready_local else 'INCOMPLETE'}")
    return items


def run_patch():
    if not CLONE.is_dir():
        sys.exit(f"clone not found: {CLONE}")
    subprocess.run([sys.executable, str(HERE / "apply_ios_patches.py"), str(CLONE)], check=False)
    subprocess.run(["git", "-C", str(CLONE), "diff", "--stat"], check=False)


def run_package(exe, out, sign, p12, password, mp):
    if not exe:
        # No Mach-O yet: still prove the packer works with the smoke stub.
        stub = OUT / "smoke" / "ds2"
        if not stub.is_file():
            sys.exit("pass --exe <path-to-Mach-O>; no built executable found")
        exe = stub
        print(f"NOTE: using smoke-test stub {exe} - replace with the real Mach-O after CI build")
    cmd = [sys.executable, str(HERE / "mkipa.py"),
           "--executable", str(exe), "--plist", str(ROOT / "Info.plist"),
           "--resource", str(OUT / "ds2_ios.toml"),
           "--resource", str(OUT / "ds2_shaders.spvz"),
           "--resource", str(OUT / "ds2_shaders.spvz.idx"),
           "--out", str(out), "--bundle-id", "com.yourteam.ds2ios"]
    if sign:
        cmd += ["--sign", "--p12", p12 or "", "--p12-pass", password or "", "--mp", mp or ""]
    subprocess.run(cmd, check=True)
    subprocess.run([sys.executable, str(HERE / "mkipa.py"), "--verify", str(out)], check=False)


def print_ci():
    print("""# 1. Create a PRIVATE fork/build repo (never make it public: it holds game files)
gh repo create ds2-ios-build --private --clone
cd ds2-ios-build
#    copy DS2_switch_src/ (with ios_port/ inside) here and commit
git add -A && git commit -m "ds2 ios port" && git push -u origin main

# 2. Upload ONLY the 21 MB executable to a PRIVATE release.
#    Do NOT upload GameData*.bdt, DLC or the 8 GB of data.
gh release create ds2-private-assets --title "private inputs" --notes "personal use only" \\
  "../darksouls_nintendo/.../switch/ds2/game_root/Default.xex"

# 3. Secrets for signing with YOUR certificate (optional).
gh secret set P12_BASE64              --body "$(base64 -w0 cert.p12)"
gh secret set P12_PASSWORD            --body '<p12 password>'
gh secret set MOBILEPROVISION_BASE64  --body "$(base64 -w0 profile.mobileprovision)"

# 4. Launch the pipeline.
gh workflow run ds2-full.yml -f do_sign=true
gh run list --workflow=ds2-full.yml
gh run watch

# 5. Fetch the result.
gh run download <run-id> -n ds2-ios-ipa

# macOS runner minutes are billed ~10x Linux minutes. Free plans include
# ~2000 minutes/month; a few builds fit, check Billing -> Actions if it stalls.
""")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("command", choices=["status", "patch", "package", "ci"])
    ap.add_argument("--exe")
    ap.add_argument("--out", default=str(OUT / "ds2-ios.ipa"))
    ap.add_argument("--sign", action="store_true")
    ap.add_argument("--p12")
    ap.add_argument("--p12-pass")
    ap.add_argument("--mp")
    a = ap.parse_args()

    if a.command == "status":
        check()
    elif a.command == "patch":
        run_patch()
    elif a.command == "package":
        run_package(a.exe, a.out, a.sign, a.p12, a.p12_pass, a.mp)
    elif a.command == "ci":
        print_ci()
    return 0


if __name__ == "__main__":
    sys.exit(main())
