# Compile check of generated DS2 code: object files only, no host app. Not a product build.
#   cmake -S tools/compile_check -B out/compile-full/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
#     -DREXSDK_DIR=$PWD/sdk -DGENERATED_DIR=$PWD/app/generated/diag-dataonly \
#     -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20 -DCMAKE_C_FLAGS=-march=x86-64-v3 -DCMAKE_CXX_FLAGS=-march=x86-64-v3
#   cmake --build out/compile-full/build --target ds2_recomp_objs -j2
# Link probe (all symbols resolve; nothing is executed):
#   clang++-20 -fuse-ld=lld -march=x86-64-v3 -Wl,--no-relax -Wl,--no-undefined tools/compile_check/link_probe_main.cpp \
#     <objects> -L sdk/out/linux-amd64 -lrexruntime -o out/compile-full/ds2_link_probe
cmake_minimum_required(VERSION 3.25)
project(ds2_compile_check LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(REXSDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../sdk" CACHE PATH "ReXGlue SDK source tree")
set(GENERATED_DIR "${CMAKE_CURRENT_LIST_DIR}/../../app/generated/diag-dataonly" CACHE PATH "codegen output directory")
add_subdirectory(${REXSDK_DIR} rexglue-sdk)
include(${GENERATED_DIR}/sources.cmake)
add_library(ds2_recomp_objs OBJECT ${GENERATED_SOURCES})
target_include_directories(ds2_recomp_objs PRIVATE ${GENERATED_DIR})
target_link_libraries(ds2_recomp_objs PRIVATE rex::runtime)
target_precompile_headers(ds2_recomp_objs PRIVATE ${GENERATED_DIR}/ds2_pch.h)
target_compile_options(ds2_recomp_objs PRIVATE -gline-tables-only)
rexglue_apply_target_settings(ds2_recomp_objs)
