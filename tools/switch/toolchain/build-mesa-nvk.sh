#!/usr/bin/env bash
# Mesa NVK (Vulkan only) for Horizon, as mesa-switch's build-switch.sh does in Docker, but on a Linux host:
#   host packages: rustc-1.91 cargo-1.91 rust-1.91-src llvm-18-dev libclang-18-dev clang-18 libclc-18-dev
#                  libllvmspirvlib-18-dev spirv-tools glslang-tools python3-mako ninja-build pkg-config;
#   pip: meson; cargo install: bindgen-cli (>= 0.71.1), cbindgen (>= 0.28) into /opt/rusttools.
# Output: $WORK/mesa-sdk/opt/devkitpro/portlibs/switch (pass it as REXGLUE_SWITCH_NVK_SDK).
# Variant used by diag4 (newest upstream, no nfsmw-nx patch):
#   git -C $WORK/src/mesa-switch worktree add ../mesa-switch-head 0ff52079ca9
#   MESA_DIR=$WORK/src/mesa-switch-head NFSMW_PATCH=none SDK_OUT=$WORK/mesa-sdk-head build-mesa-nvk.sh
set -euo pipefail
WORK=${WORK:?set WORK}; S=$WORK/src; M=${MESA_DIR:-$S/mesa-switch}; SDK_OUT=${SDK_OUT:-$WORK/mesa-sdk}
HERE=$(cd "$(dirname "$0")" && pwd)
NFSMW_PATCH=${NFSMW_PATCH:-$HERE/mesa-switch-nfsmw.patch}  # from nfsmw-nx mesa/, unchanged
RUSTBIN=/usr/lib/rust-1.91/bin
export PATH=/opt/rusttools/bin:$RUSTBIN:$PATH DEVKITPRO=/opt/devkitpro
P=/opt/devkitpro/portlibs/switch

# 1. Rust std for aarch64-unknown-linux-gnu (rustup's prebuilt std is what mesa-switch links; NAK only needs
#    core/alloc/std objects, the Linux syscalls it would pull are stubbed by src/nouveau/vulkan/rust_switch_stubs.c).
STD=/usr/lib/rust-1.91/lib/rustlib/aarch64-unknown-linux-gnu/lib
if [ ! -f "$(ls $STD/libstd-*.rlib 2>/dev/null | head -1)" ]; then
  mkdir -p $WORK/rust-std/src && cd $WORK/rust-std
  printf '[package]\nname="stdbuild"\nversion="0.1.0"\nedition="2021"\n[lib]\npath="src/lib.rs"\n' > Cargo.toml
  echo 'pub fn f() -> usize { std::env::args().count() }' > src/lib.rs
  RUSTC_BOOTSTRAP=1 cargo build --release -Zbuild-std=std,panic_abort,panic_unwind --target aarch64-unknown-linux-gnu
  mkdir -p $STD && cp $(ls target/aarch64-unknown-linux-gnu/release/deps/*.rlib | grep -v stdbuild) $STD/
fi

# 2. Source: mesa-switch 1a8c1a66d6f + the nfsmw-nx patch (35 files).
cd $M
if [ "$NFSMW_PATCH" != none ] && [ -z "$(git status --porcelain)" ]; then git apply "$NFSMW_PATCH"; fi

# 3. Headers and placeholder POSIX archives the build expects in portlibs (build-switch.sh does the same).
mkdir -p $P/include $P/lib
cp src/gallium/winsys/nouveau/drm/nouveau.h src/nouveau/headers/nv_device_info.h $P/include/
if [ ! -f $P/lib/libdl.a ]; then
  echo "void __mesa_switch_stub_lib(void) {}" > $WORK/stub_posix.c
  $DEVKITPRO/devkitA64/bin/aarch64-none-elf-gcc -c $WORK/stub_posix.c -o $WORK/stub_posix.o
  for L in dl rt util; do $DEVKITPRO/devkitA64/bin/aarch64-none-elf-ar rcs $P/lib/lib$L.a $WORK/stub_posix.o; done
fi

# 4. Native host tools (mesa_clc, vtn_bindgen2) with LLVM 18.
sed "s|@MESA@|$M|g" $HERE/mesa_native_linux.ini > $M/../mesa_native_$(basename $M).ini
sed "s|@MESA@|$M|g" $HERE/mesa_cross_switch_linux.ini > $M/../mesa_cross_$(basename $M).ini
if [ ! -x builddir-native/src/compiler/clc/mesa_clc ]; then
  printf "[binaries]\nllvm-config = '/usr/lib/llvm-18/bin/llvm-config'\n" > $WORK/mesa_llvm18.ini
  meson setup builddir-native --native-file $WORK/mesa_llvm18.ini -Dvulkan-drivers= -Dgallium-drivers= \
    -Dshader-cache=true -Dplatforms= -Dglx=disabled -Degl=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled \
    -Dtools=[] -Dllvm=enabled -Dmesa-clc=enabled -Dprecomp-compiler=enabled -Dinstall-mesa-clc=true
  ninja -C builddir-native src/compiler/clc/mesa_clc src/compiler/spirv/vtn_bindgen2
fi

# 5. Cross build.
export MESA_SWITCH_RUSTC=$RUSTBIN/rustc MESA_SWITCH_RUST_TARGET=aarch64-unknown-linux-gnu \
       MESA_SWITCH_BINDGEN=/opt/rusttools/bin/bindgen
[ -d builddir-switch ] || meson setup builddir-switch --cross-file $M/../mesa_cross_$(basename $M).ini \
  --native-file $M/../mesa_native_$(basename $M).ini --default-library=static --prefix=$P --libdir=lib --buildtype=release \
  -Doptimization=2 -Db_lto=false -Db_ndebug=true -Dvulkan-drivers=nouveau -Dgallium-drivers= -Dgallium-rusticl=false \
  -Dplatforms=switch -Dglx=disabled -Degl=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Dvideo-codecs= \
  -Dshader-cache=enabled -Dxmlconfig=enabled -Dexpat=enabled -Dtools=[] -Dllvm=disabled -Dshared-glapi=disabled \
  -Dshared-llvm=disabled -Dmesa-clc=system -Dprecomp-compiler=system -Dcpp_rtti=false -Dbuild-tests=false
ninja -C builddir-switch -j${JOBS:-2}
meson install -C builddir-switch --destdir $SDK_OUT
cp builddir-switch/src/nouveau/vulkan/libnvk.a builddir-switch/src/nouveau/compiler/libnak_rs.a \
   $SDK_OUT$P/lib/
echo "MESA NVK OK: REXGLUE_SWITCH_NVK_SDK=$SDK_OUT$P"
