# ds2 - host application for the recompiled Dark Souls II: Scholar of the First Sin (Xbox 360, 465307E4).
#
# Unlike the template from `rexglue init`, this file does not include generated/rexglue.cmake: that file
# re-runs codegen with a hard-wired ds2_manifest.toml (the working manifest, which still fails validation)
# and is rewritten by every codegen run. Here the generated directory is chosen explicitly.
#
#   cmake -S app -B out/app-host -G Ninja -DCMAKE_BUILD_TYPE=Release \
#     -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20 \
#     -DCMAKE_C_FLAGS=-march=x86-64-v3 -DCMAKE_CXX_FLAGS=-march=x86-64-v3
#   cmake --build out/app-host --target ds2 -j2
#
# Nintendo Switch (devkitA64 + libnx + the Mesa NVK SDK, see docs/handoff/10-switch-build.md):
#   cmake -S app -B out/app-switch -G Ninja -DCMAKE_BUILD_TYPE=Release \
#     -DCMAKE_TOOLCHAIN_FILE=tools/switch/cmake/switch-devkitA64.cmake \
#     -DREXGLUE_SWITCH_NVK_SDK=<mesa-sdk>/opt/devkitpro/portlibs/switch
#   cmake --build out/app-switch --target ds2 -j2        -> out/app-switch/ds2.nro

cmake_minimum_required(VERSION 3.25)
project(ds2 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(REXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../sdk" CACHE PATH "ReXGlue SDK source tree")
set(DS2_GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated/diag-dataonly" CACHE PATH
    "codegen output directory (diagnostic configuration that passes validation)")
if(NOT EXISTS "${DS2_GENERATED_DIR}/sources.cmake")
    message(FATAL_ERROR "No generated code in ${DS2_GENERATED_DIR}. Run codegen first "
                        "(MANIFEST=app/ds2_manifest.diag-dataonly.toml tools/run_codegen_diag.sh <tag>).")
endif()

add_subdirectory("${REXSDK_DIR}" rexglue-sdk)

# Recompiled guest code, with the same options generated/rexglue.cmake would apply.
include("${DS2_GENERATED_DIR}/sources.cmake")
add_library(ds2_recomp OBJECT ${GENERATED_SOURCES})
target_include_directories(ds2_recomp PRIVATE "${DS2_GENERATED_DIR}")
target_link_libraries(ds2_recomp PRIVATE rex::runtime)
target_precompile_headers(ds2_recomp PRIVATE "${DS2_GENERATED_DIR}/ds2_pch.h")
target_compile_options(ds2_recomp PRIVATE $<$<CXX_COMPILER_ID:Clang,AppleClang>:-gline-tables-only>)
# RC97: guest memory is accessed through u8/u16/u32/u64 pointers to the same bytes (stfd + lwz of one half,
# etc.). The SDK's add_compile_options(-fno-strict-aliasing) does not reach this target; without volatile
# accesses GCC reordered such pairs (RC97 crash: assert in sub_8275AAD8 on a garbage pool count).
target_compile_options(ds2_recomp PRIVATE -fno-strict-aliasing)
rexglue_apply_target_settings(ds2_recomp)

# v108: profile-guided optimisation of the recompiled game code. GEN: counters on every branch (-fprofile-arcs;
# devkitA64's libgcov has no value profilers and no file output, see src/ds2_pgo.cpp). USE: the .gcda files put
# back next to the objects of the same build folder (tools/pgo/restore_gcda.py).
set(DS2_PGO "OFF" CACHE STRING "Profile-guided optimisation of the game code: OFF, GEN or USE")
if(DS2_PGO STREQUAL "GEN")
    target_compile_options(ds2_recomp PRIVATE -fprofile-arcs -fprofile-update=single
                           -fprofile-info-section=ds2_gcov_info)
elseif(DS2_PGO STREQUAL "USE")
    target_compile_options(ds2_recomp PRIVATE -fbranch-probabilities -fprofile-use -fno-profile-values
                           -fprofile-partial-training -fprofile-correction -Wno-missing-profile
                           -Wno-coverage-mismatch)
endif()

set(DS2_SOURCES
    src/main.cpp
    src/ds2_capture.cpp
    src/ds2_input_script.cpp
    src/ds2_dlc.cpp
    src/ds2_watchdog.cpp
    src/ds2_func_counts.cpp
    src/ds2_editor_wait.cpp
    src/ds2_switch_platform.cpp
    src/ds2_mode_overlay.cpp
    src/ds2_pgo.cpp
)
add_executable(ds2 ${DS2_SOURCES})
target_include_directories(ds2 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${DS2_GENERATED_DIR}")
if(REXGLUE_PLATFORM_IOS)
    target_sources(ds2 PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../ios_port/src/ios_paths.mm"
        "${CMAKE_CURRENT_SOURCE_DIR}/../ios_port/src/ios_touch_input_driver.cpp")
    target_include_directories(ds2 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../ios_port/include")
    if(REXGLUE_IOS_SDL3)
        target_link_libraries(ds2 PRIVATE "${REXGLUE_IOS_SDL3}")
    endif()
    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/../ios_port/src/ios_paths.mm"
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
endif()
target_link_libraries(ds2 PRIVATE ds2_recomp rex::runtime)
if(DS2_PGO STREQUAL "GEN")
    target_compile_definitions(ds2 PRIVATE DS2_PGO_GEN=1)
    target_link_options(ds2 PRIVATE -fprofile-arcs)
endif()
if(REXGLUE_PLATFORM_SWITCH)
    # One static executable: the GPU plugin is linked in instead of loaded from a shared library.
    rexglue_configure_target(ds2 GPU_PLUGINS xenos)
    set(DS2_DISPLAY_VERSION "0.62 RC62" CACHE STRING "Version displayed by homebrew launchers and forwarders")
    rexglue_switch_add_nro(ds2
        NAME "Dark Souls II: Scholar of the First Sin"
        AUTHOR "xDASCATx"
        VERSION "${DS2_DISPLAY_VERSION}"
        ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.jpg")
    # v116: the version on the start screen (only this file is rebuilt when it changes).
    set_source_files_properties(src/ds2_dlc.cpp PROPERTIES COMPILE_DEFINITIONS "DS2_VERSION_STRING=\"${DS2_DISPLAY_VERSION}\"")
elseif(REXGLUE_PLATFORM_IOS)
    # No dlopen on iOS: link the Xenos GPU plugin into the executable, exactly
    # as the Switch build does.
    rexglue_configure_target(ds2 GPU_PLUGINS xenos)
    # Static MoltenVK exports vkGetInstanceProcAddr when linked directly;
    # zlib is needed by ios_port/include/spvz_reader.h.
    if(REXGLUE_IOS_MOLTENVK)
        target_link_libraries(ds2 PRIVATE "${REXGLUE_IOS_MOLTENVK}")
    else()
        message(FATAL_ERROR "REXGLUE_IOS_MOLTENVK must point at libMoltenVK.a")
    endif()
    target_link_libraries(ds2 PRIVATE z)
    foreach(fw Metal QuartzCore UIKit Foundation CoreGraphics CoreAudio
               AudioToolbox GameController CoreHaptics)
        target_link_libraries(ds2 PRIVATE "-framework ${fw}")
    endforeach()
else()
    rexglue_configure_target(ds2)
endif()

# rex_app.cpp includes imgui.h, but rexruntime links imgui privately, so the include path does not reach the
# executable when the SDK is a subdirectory (same fix as nfsmw-nx app/CMakeLists.txt; on Switch
# rexglue_switch.cmake does it).
if(NOT REXGLUE_PLATFORM_SWITCH AND TARGET imgui)
    target_include_directories(ds2 PRIVATE $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>)
endif()

# Put ds2 next to librexruntime.so (sdk/out/<platform>/) so the $ORIGIN rpath set by rexglue_configure_target finds it.
if(NOT REXGLUE_PLATFORM_SWITCH)
    set_target_properties(ds2 PROPERTIES RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:rex::runtime>")
endif()
