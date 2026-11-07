#!/usr/bin/env bash
# zlib, zstd and expat into /opt/devkitpro/portlibs/switch, with dkp-toolchain-vars switchvars.sh flags.
set -euo pipefail
WORK=${WORK:?set WORK}; S=$WORK/src
export DEVKITPRO=/opt/devkitpro
source $DEVKITPRO/switchvars.sh
P=$PORTLIBS_PREFIX
(cd $S/zlib && CHOST=aarch64-none-elf ./configure --prefix=$P --static && make -j2 libz.a && make install)
(cd $S/zstd/lib && make -j2 libzstd.a-mt CC=aarch64-none-elf-gcc AR=aarch64-none-elf-gcc-ar \
   CFLAGS="$CFLAGS $CPPFLAGS" ZSTD_LIB_DICTBUILDER=0 && make install-static install-includes install-pc PREFIX=$P)
mkdir -p $S/libexpat/expat/build-switch
(cd $S/libexpat/expat/build-switch && cmake .. -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake \
   -DCMAKE_INSTALL_PREFIX=$P -DEXPAT_BUILD_TOOLS=OFF -DEXPAT_BUILD_EXAMPLES=OFF -DEXPAT_BUILD_TESTS=OFF \
   -DEXPAT_BUILD_DOCS=OFF -DEXPAT_SHARED_LIBS=OFF -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-DXML_POOR_ENTROPY \
   && make -j2 && make install)
echo PORTLIBS OK
