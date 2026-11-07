#!/usr/bin/env bash
# devkitA64 (binutils 2.46 + GCC 16.1.0 + newlib 4.6.0.20260123 with devkitPro's patches), configured like
# devkitPro/buildscripts (build-binutils.sh, build-gcc-stage1.sh, build-newlib.sh, build-gcc-stage2.sh, target 3).
# Host packages: build-essential libgmp-dev libmpfr-dev libmpc-dev libisl-dev texinfo flex bison.
set -euo pipefail
WORK=${WORK:?set WORK}
S=$WORK/src; B=$WORK/build; PATCHES=$S/buildscripts/patches
prefix=/opt/devkitpro/devkitA64; target=aarch64-none-elf; MAKE="make -j${JOBS:-2}"
export PATH=$prefix/bin:$PATH
mkdir -p $B && cd $B
if [ ! -d binutils-2.46.0 ]; then
  tar -xJf $S/binutils_2.46.orig.tar.xz && mv binutils-with-gold-2.46 binutils-2.46.0
  patch -p1 -d binutils-2.46.0 -i $PATCHES/binutils-2.46.0-1.patch
fi
if [ ! -d newlib-4.6.0.20260123 ]; then
  tar -xJf $S/newlib_4.6.0.20260123.orig.tar.xz
  patch -p1 -d newlib-4.6.0.20260123 -i $PATCHES/newlib-4.6.0.20260123-4.patch
fi
[ -f gcc-patched ] || { patch -p1 -d $S/gcc-16.1.0 -i $PATCHES/gcc-16.1.0-1.patch && touch gcc-patched; }
opts='--with-march=armv8 --enable-multilib --with-pkgversion=devkitA64'
mkdir -p $target/binutils $target/gcc $target/newlib
cd $B/$target/binutils
[ -f configured ] || { $B/binutils-2.46.0/configure --prefix=$prefix --target=$target --disable-nls --disable-werror \
  --disable-shared --disable-debug --enable-lto --enable-plugins --enable-poison-system-directories --disable-gdb \
  --disable-gdbserver --disable-sim --disable-gprofng --disable-gold && touch configured; }
[ -f built ] || { $MAKE && touch built; }
[ -f installed ] || { make install && touch installed; }
cd $B/$target/gcc
[ -f configured ] || { CFLAGS_FOR_TARGET="-O2 -ffunction-sections -fdata-sections" \
  CXXFLAGS_FOR_TARGET="-O2 -ffunction-sections -fdata-sections" LDFLAGS_FOR_TARGET="" \
  $S/gcc-16.1.0/configure --target=$target --prefix=$prefix --enable-languages=c,c++,lto \
  --with-gnu-as --with-gnu-ld --with-gcc --enable-cxx-flags='-ffunction-sections' --disable-libstdcxx-verbose \
  --enable-poison-system-directories --enable-threads=posix --disable-win32-registry --disable-nls --disable-debug \
  --disable-libmudflap --disable-libssp --disable-libgomp --disable-libstdcxx-pch --enable-libstdcxx-time=yes \
  --enable-libstdcxx-filesystem-ts --with-newlib=yes --with-native-system-header-dir=/include \
  --with-sysroot=$prefix/$target --enable-lto --disable-tm-clone-registry --disable-__cxa_atexit \
  --with-bugurl=https://devkitpro.org --disable-bootstrap $opts && touch configured; }
[ -f built-stage1 ] || { $MAKE all-gcc && touch built-stage1; }
[ -f installed-stage1 ] || { make install-gcc && touch installed-stage1; }
cd $B/$target/newlib
[ -f configured ] || { (unset CC CXX CFLAGS; CFLAGS_FOR_TARGET="-O2 -ffunction-sections -fdata-sections" \
  $B/newlib-4.6.0.20260123/configure --disable-newlib-supplied-syscalls --enable-newlib-mb \
  --disable-newlib-wide-orient --enable-newlib-register-fini --target=$target --prefix=$prefix) && touch configured; }
[ -f built ] || { $MAKE && touch built; }
[ -f installed ] || { make install -j1 && touch installed; }
cd $B/$target/gcc
[ -f built-stage2 ] || { $MAKE && touch built-stage2; }
[ -f installed-stage2 ] || { make install-strip && touch installed-stage2; }
# GCC's limits.h from stage 1 does not chain to newlib's (see README.md).
G=$S/gcc-16.1.0/gcc
cat $G/limitx.h $G/glimits.h $G/limity.h > $prefix/lib/gcc/$target/16.1.0/include/limits.h
echo '#include <limits.h>
int x[PATH_MAX];' | $prefix/bin/$target-gcc -x c -c - -o /dev/null
echo DEVKITA64 OK
