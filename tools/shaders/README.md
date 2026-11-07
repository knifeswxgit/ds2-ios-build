# DS2 shaders through XenosRecomp (research, not part of the build)

- `drv.cpp` - driver: walks a folder of `.fpo/.vpo` files (unpacked `shader/*.bnd`, see `tools/analysis/bnd4.py`)
  and writes HLSL through XenosRecomp (hedge-dev, or the nfsmw-nx fork `NFSMW_RECOMP`). Build:
  `g++ -std=c++20 -I<XenosRecomp>/XenosRecomp -Istub drv.cpp <XenosRecomp>/XenosRecomp/shader_recompiler.cpp -o drv`.
- `compile_all.sh <file.hlsl>` - HLSL to SPIR-V through DXC (the Linux binary from renderbag/dxc-bin).
- `stub/` - header stubs, so that only the translator is built, without its dependencies.
- `build_library.cpp` - `rex-shader-library`: builds the offline SPIR-V shader pack (`ds2_shaders_*.spvlib` + `.idx`)
  from the game's shader microcode and the learned caches written on the console.
  `REX_FOLD_TEXTURE_SIGNS=1` builds it for `ds2_native_fold_texture_signs = true`.
- `optimize_pack.py` - runs every SPIR-V variant of a pack through spirv-opt and checks it with spirv-val.

Results of the XenosRecomp study: 2271 of 2275 shaders translated to HLSL, 2166 compiled to SPIR-V. The shaders
themselves are game data and are never committed.
