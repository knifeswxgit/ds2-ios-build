#!/usr/bin/env bash
# devkitA64 rules, devkitPro cmake/pkg-config/vars files (installed as their PKGBUILDs do), general-tools,
# switch-tools (elf2nro, nacptool; needs liblz4-dev, autoconf, automake) and libnx.
set -euo pipefail
WORK=${WORK:?set WORK}; S=$WORK/src; D=/opt/devkitpro
export DEVKITPRO=$D PATH=$D/devkitA64/bin:$D/tools/bin:$PATH
make -C $S/devkita64-rules install
K=$S/pacman-packages
install -d $D/cmake/Platform $D/portlibs/switch/bin $D/devkitA64/bin
cp $K/cmake/switch/aarch64-none-elf-cmake $D/portlibs/switch/bin/
cp $K/cmake/switch/Switch.cmake $D/cmake/; cp $K/cmake/switch/NintendoSwitch.cmake $D/cmake/Platform/
cp $K/cmake/devkita64/aarch64-none-elf-cmake $D/devkitA64/bin/; cp $K/cmake/devkita64/devkitA64.cmake $D/cmake/
for f in dkp-initialize-path dkp-toolchain-common dkp-impl-helpers dkp-rule-overrides dkp-linker-utils \
         dkp-custom-target dkp-embedded-binary dkp-asset-folder; do cp $K/cmake/common-utils/$f.cmake $D/cmake/; done
cp $K/cmake/common-utils/Generic-dkP.cmake $D/cmake/Platform/
install -m644 $K/dkp-toolchain-vars/devkita64.sh $K/dkp-toolchain-vars/switchvars.sh $D/
install -m755 $K/dkp-toolchain-vars/portlibs_prefix.sh $D/
cp $K/switch/pkg-config/pkg-config.in $D/portlibs/switch/bin/aarch64-none-elf-pkg-config
chmod +x $D/portlibs/switch/bin/* $D/devkitA64/bin/aarch64-none-elf-cmake
for t in general-tools switch-tools; do
  (cd $S/$t && { [ -f configure ] || ./autogen.sh; } && ./configure --prefix=$D/tools && make -j${JOBS:-2} && make install)
done
make -C $S/libnx/nx -j${JOBS:-2} && make -C $S/libnx/nx install
echo LIBNX-TOOLS OK
