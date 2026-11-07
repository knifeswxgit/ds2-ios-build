# CMake toolchain: ReXGlue / DS2 -> iOS arm64 (device).
#
# Usage (on a macOS host or a GitHub Actions macos-* runner):
#   cmake -S app -B out/app-ios -G Xcode \
#     -DCMAKE_TOOLCHAIN_FILE=/path/to/ios_port/cmake/ios-arm64.cmake \
#     -DDS2_GENERATED_DIR=/path/to/app/generated/diag-dataonly \
#     -DMoltenVK_DIR=/path/to/MoltenVK.xcframework/ios-arm64
#
# Do NOT set CMAKE_C(XX)_COMPILER here: with the Xcode generator CMake uses
# xcrun and picks the Apple Clang that owns the selected SDK.

set(CMAKE_SYSTEM_NAME iOS)
set(CMAKE_SYSTEM_VERSION "17.0")
set(CMAKE_OSX_SYSROOT "iphoneos" CACHE STRING "iOS device SDK" FORCE)
set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "" FORCE)
set(CMAKE_OSX_DEPLOYMENT_TARGET "17.0" CACHE STRING "" FORCE)

# try_compile cannot link an executable against an iOS SDK without a bundle.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# ---- ReXGlue SDK switches -------------------------------------------------
# iOS is Apple/POSIX: sdk/include/rex/platform.h sees TARGET_OS_MAC and sets
# REX_PLATFORM_MAC=1, so the whole sdk/src/core/*_posix.cpp layer applies.
# sdk/CMakeLists.txt currently labels every APPLE build "mac-arm64" and stages
# dylibs; patch its APPLE branch with if(IOS) -> REX_PLATFORM "ios-arm64".
set(REXGLUE_PLATFORM_IOS ON CACHE BOOL "" FORCE)
set(REXGLUE_PLATFORM_SWITCH OFF CACHE BOOL "" FORCE)
set(REXGLUE_USE_VULKAN ON CACHE BOOL "" FORCE)
set(REXGLUE_USE_D3D12 OFF CACHE BOOL "" FORCE)
set(REXGLUE_STAGE_MACOS_VULKAN_RUNTIME OFF CACHE BOOL "" FORCE)

# Dynamic loading of arbitrary dylibs is not available to App Store / sideload
# apps. The Switch build already links the GPU plugin statically via
# rexglue_configure_target(ds2 GPU_PLUGINS xenos); do the same, and link
# MoltenVK statically with direct vkGetInstanceProcAddr entry points (the path
# described in sdk/src/ui/vulkan/vulkan_instance.cpp for NRO builds).
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# iOS on arm64 has no makecontext/swapcontext. sdk/src/core/fiber_posix.cpp
# must be replaced with an AArch64 assembly context switch before linking.
set(REXGLUE_FIBER_BACKEND "asm-aarch64" CACHE STRING "" FORCE)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO")
set(CMAKE_XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1")
set(CMAKE_XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "17.0")
