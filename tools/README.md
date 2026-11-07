# Tools

Scripts and small programs used to build, generate and analyse the DS2 port. Several names are Spanish; each one
is explained below.

## Build

| Path | What it does |
|---|---|
| `fetch_thirdparty.py` | Fetches the third-party sources of the SDK into `sdk/thirdparty`. The repository only keeps the files this port changed. |
| `switch/toolchain/` | Builds the whole Switch toolchain from source (devkitA64 GCC, libnx, portlibs, Mesa NVK with the port patches). `setup-cloud.sh` does it in one go on Linux. |
| `switch/cmake/switch-devkitA64.cmake` | The CMake toolchain file for devkitA64 and libnx. |
| `build/` | Configure and build the Switch NRO (`build-switch.sh`), the Linux PC build (`build-pc.sh`), Mesa (`build-mesa.sh`), release packaging and the NSP forwarder (`make-forwarder.py`). |
| `switch/hbl-ds2/` | Builds `hbl_ds2.nsp`: nx-hbloader with a larger system resource for the game process (kernel page tables), for testing. |
| `run/` | Running the PC build. |

## Code generation

| Path | What it does |
|---|---|
| `run_codegen_diag.sh`, `run_codegen_monitored.sh` | Run `rexglue codegen` on `Default.xex` with the manifests in `app/` and the overrides in `config/`, logging time and memory. |
| `rc97/` | The optimised code generation used since build 97 (registers as C++ locals, `share_registers` marks, direct calls, guest memory without `volatile`). Steps and pitfalls: [rc97/README.md](rc97/README.md). |
| `huecos.py`, `huecos_excluir.txt` ("gaps") | Finds code the generator left outside any function and writes candidate entries (`huecos_functions.toml`, not committed); `config/ds2_overrides.gaps.toml` was made from it. |
| `ds2_find_dataonly_entries.py`, `ds2_find_split_culprits.py`, `ds2_analyze_unresolved.py`, `ds2_classify_unresolved.py` | Find and classify function entries referenced only from data or code pointers, and unresolved branches (the `config/ds2_overrides.*.toml` inputs). |
| `pgo/restore_gcda.py` | Profile-guided optimisation of the game code: puts the profiles written by a `DS2_PGO=GEN` build on the console (`sdmc:/switch/ds2/pgo`) back next to the objects, for the `DS2_PGO=USE` build of the same build folder. |
| `build/patch-generated.py` | Brings already generated code up to the current generator output without a full codegen run. |

## Shaders

| Path | What it does |
|---|---|
| `shaders/` | The offline SPIR-V shader pack builder (`build_library.cpp`, `rex-shader-library`), spirv-opt pass over a pack, and the XenosRecomp research. See [shaders/README.md](shaders/README.md). |
| `ds2/gen_direct_resolve_shaders.sh` | Compiles the direct resolve compute shader into the SPIR-V headers used by the SDK. |

## Analysis

| Path | What it does |
|---|---|
| `analysis/` | Console log and profile analysis (`perf.py`, `cvars.py`, `ds2_func_counts.py`, `ds2_resolve_dump.py`), BND4 archives (`bnd4.py`), the 16-bit jump table check, SPIR-V extraction from generated headers (`hdr2spv.py`). |
| `ds2_trace_*.py`, `ds2_imagedump.py` | Read the native renderer's frame captures (L3+R3): draw traces and dumped images. |
| `ds2_xexinfo.py`, `ds2_make_elf.py` | XEX header information; the game image wrapped in a PowerPC ELF so disassemblers show guest addresses. |
| `compile_check/`, `switch/gm-sim/` | Link probe and a small GM20B command simulator used while bringing up the driver. |

Nothing in this folder contains game data. Game files, generated code and built binaries are never committed.
