# Switch toolchain from sources (no devkitPro package servers)

pkg.devkitpro.org / downloads.devkitpro.org / Docker Hub / rustup are not reachable from the build environment
used for this port (2026-10-20). These scripts rebuild the same pieces from sources that are reachable:
Ubuntu's package pool (archive.ubuntu.com) and public GitHub repositories over git.

| Piece | Version | Source |
|---|---|---|
| binutils | 2.46 + devkitPro `binutils-2.46.0-1.patch` | `archive.ubuntu.com/.../binutils_2.46.orig.tar.xz` |
| GCC | 16.1.0 + devkitPro `gcc-16.1.0-1.patch` (devkitA64 r30 uses 16.1.0) | git tag `releases/gcc-16.1.0` of github.com/gcc-mirror/gcc |
| newlib | 4.6.0.20260123 + devkitPro `newlib-...-4.patch` | `archive.ubuntu.com/.../newlib_4.6.0.20260123.orig.tar.xz` |
| buildscripts, devkita64-rules, general-tools, cmake files, pkg-config wrapper | master | github.com/devkitPro/* |
| libnx, switch-tools (elf2nro, nacptool) | v4.12.0, master | github.com/switchbrew/* |
| zlib 1.3.1, zstd 1.5.7, expat 2.7.1 | | GitHub tags |
| Mesa NVK (mesa-switch 1a8c1a66d6f + nfsmw-nx `mesa-switch-nfsmw.patch`) | 26.2.1 | github.com/danfromtico/mesa-switch |
| Rust 1.91 + std for `aarch64-unknown-linux-gnu` | Ubuntu `rustc-1.91`, `rust-1.91-src` | std built with `-Zbuild-std` |

Order (each step is idempotent; everything installs under `/opt/devkitpro`, as the official packages do):

```sh
WORK=/path/to/work tools/switch/toolchain/fetch-sources.sh      # git clones + Ubuntu tarballs
WORK=... tools/switch/toolchain/build-devkita64.sh               # ~35 min on 2 cores
WORK=... tools/switch/toolchain/build-libnx-tools.sh             # libnx, rules, cmake files, elf2nro...
WORK=... tools/switch/toolchain/build-portlibs.sh                # zlib, zstd, expat
WORK=... tools/switch/toolchain/build-mesa-nvk.sh                # -> $WORK/mesa-sdk/opt/devkitpro/portlibs/switch
```

Two fixes that the official build does not need, because it builds over an existing install:

- GCC's `limits.h` is generated during stage 1, before newlib's headers exist, so it does not chain to newlib's
  `limits.h` and `PATH_MAX` is missing (libnx fails in `path_buf.h`). `build-devkita64.sh` regenerates it from
  `limitx.h + glimits.h + limity.h` after the install.
- expat 2.7 refuses to build without an entropy source on Horizon: `-DXML_POOR_ENTROPY` (Mesa uses it only for
  driconf XML).

Mesa is built Vulkan-only (no Gallium/EGL/GL), which is all the ReXGlue Switch build links
(`sdk/cmake/rexglue_switch.cmake`: `libvulkan.a`, expat, zstd, z, nx).
