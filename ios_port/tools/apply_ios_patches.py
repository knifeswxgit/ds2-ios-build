#!/usr/bin/env python3
"""Apply the iOS platform patches to a local clone of XDASCATX/DS2_switch.

Idempotent via explicit per-hunk markers; originals are kept as <file>.orig.
Every anchor is replaced in ALL its occurrences (validated by `count`).
Run `git diff --stat` in the clone afterwards to see the result.
"""
import argparse
import pathlib
import shutil
import sys

HUNKS = []


def hunk(rel, old, new, label, marker, count=1):
    HUNKS.append(dict(rel=rel, old=old, new=new, label=label, marker=marker, count=count))


# --- platform detection ---------------------------------------------------
hunk("sdk/include/rex/platform.h",
"""#if defined(TARGET_OS_MAC) && TARGET_OS_MAC
#define REX_PLATFORM_MAC 1
#elif defined(WIN32) || defined(_WIN32)
""",
"""#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
// iOS keeps the whole Darwin/POSIX layer, but has no makecontext/swapcontext
// on arm64, so it opts into the AArch64 assembly fiber backend too.
#define REX_PLATFORM_IOS 1
#define REX_PLATFORM_MAC 1
#elif defined(TARGET_OS_MAC) && TARGET_OS_MAC
#define REX_PLATFORM_MAC 1
#elif defined(WIN32) || defined(_WIN32)
""", "platform: iOS branch", "#define REX_PLATFORM_IOS 1")

hunk("sdk/include/rex/platform.h",
"""#ifndef REX_PLATFORM_SWITCH
#define REX_PLATFORM_SWITCH 0
#endif
""",
"""#ifndef REX_PLATFORM_IOS
#define REX_PLATFORM_IOS 0
#endif
#ifndef REX_PLATFORM_SWITCH
#define REX_PLATFORM_SWITCH 0
#endif
""", "platform: normalize IOS macro", "#ifndef REX_PLATFORM_IOS")

# --- fibers: reuse the existing AArch64 asm backend ------------------------
hunk("sdk/include/rex/thread/fiber.h",
     "#if REX_PLATFORM_SWITCH", "#if REX_PLATFORM_SWITCH || REX_PLATFORM_IOS",
     "fiber.h: include + trampoline declaration", "#if REX_PLATFORM_SWITCH || REX_PLATFORM_IOS", count=2)

hunk("sdk/include/rex/thread/fiber.h",
     "#elif REX_PLATFORM_SWITCH", "#elif REX_PLATFORM_SWITCH || REX_PLATFORM_IOS",
     "fiber.h: Context struct branch", "#elif REX_PLATFORM_SWITCH || REX_PLATFORM_IOS")

hunk("sdk/src/core/fiber_switch.cpp",
     "#if REX_PLATFORM_SWITCH", "#if REX_PLATFORM_SWITCH || REX_PLATFORM_IOS",
     "fiber_switch.cpp: enable for iOS", "#if REX_PLATFORM_SWITCH || REX_PLATFORM_IOS")

hunk("sdk/src/core/CMakeLists.txt",
"""# Platform-specific sources
if(WIN32)
""",
"""# Platform-specific sources
# iOS arm64 has no ucontext: use the AArch64 assembly fiber backend already
# written for Horizon (fiber_switch.cpp) instead of fiber_posix.cpp.
if(REXGLUE_PLATFORM_IOS)
    set(REX_FIBER_SOURCE fiber_switch.cpp)
else()
    set(REX_FIBER_SOURCE fiber_posix.cpp)
endif()

if(WIN32)
""", "core cmake: fiber source selection", "set(REX_FIBER_SOURCE fiber_switch.cpp)")

hunk("sdk/src/core/CMakeLists.txt",
     "        fiber_posix.cpp\n", "        ${REX_FIBER_SOURCE}\n",
     "core cmake: substitute fiber source", "${REX_FIBER_SOURCE}")

hunk("sdk/CMakeLists.txt",
"""elseif(APPLE)
    if(REX_TARGET_PROCESSOR MATCHES "AMD64|x86_64")
""",
"""elseif(APPLE)
    if(IOS OR REXGLUE_PLATFORM_IOS)
        set(REX_PLATFORM "ios-arm64")
    elseif(REX_TARGET_PROCESSOR MATCHES "AMD64|x86_64")
""", "sdk cmake: ios-arm64 platform tag", 'set(REX_PLATFORM "ios-arm64")')

# --- compressed shader pack ----------------------------------------------
hunk("sdk/include/rex/graphics/pipeline/shader/spirv_library.h",
"""#include <array>
#include <cstdio>
#include <filesystem>
#include <map>
""",
"""#include <array>
#include <cstdio>
#include <filesystem>
#include <map>

#include <rex/platform.h>
#if REX_PLATFORM_IOS
#include <memory>
#include "spvz_reader.h"
#endif
""", "spirv_library.h: compressed reader include", '#include "spvz_reader.h"')

hunk("sdk/include/rex/graphics/pipeline/shader/spirv_library.h",
"""  std::map<Key, uint64_t> entries_;
};
""",
"""  std::map<Key, uint64_t> entries_;
#if REX_PLATFORM_IOS
  // DS2SPVZ1: random-access compressed pack, ~3.3x smaller than the raw
  // .spvlib. The .idx offsets still address the original (virtual) stream.
  std::unique_ptr<ds2_ios::SpvzReader> spvz_;
#endif
};
""", "spirv_library.h: reader member", "std::unique_ptr<ds2_ios::SpvzReader> spvz_;")

hunk("sdk/src/graphics/pipeline/shader/spirv_library.cpp",
"""bool SpirvShaderLibrary::Open(const std::filesystem::path& path, uint64_t expected_signature, bool tolerate_tail) {
  static_assert(std::endian::native == std::endian::little);
  if (file_) fclose(file_);
""",
"""bool SpirvShaderLibrary::Open(const std::filesystem::path& path, uint64_t expected_signature, bool tolerate_tail) {
  static_assert(std::endian::native == std::endian::little);
  if (file_) fclose(file_);
#if REX_PLATFORM_IOS
  // Prefer the compressed sibling. Its .idx sidecar is copied next to it and
  // keeps pointing into the original (virtual) byte stream.
  if (!tolerate_tail) {
    auto spvz_path = path;
    spvz_path.replace_extension(".spvz");
    if (std::filesystem::exists(spvz_path)) {
      auto reader = std::make_unique<ds2_ios::SpvzReader>();
      if (!reader->Open(spvz_path, kHeader[3])) {
        REXGPU_WARN("[shader library] compressed pack rejected; runtime fallback");
      } else {
        Record first_record;
        if (!reader->Read(sizeof(kHeader), first_record.data(), sizeof(first_record)) ||
            (expected_signature && first_record[2] != expected_signature)) {
          REXGPU_WARN("[shader library] compressed pack signature mismatch; runtime fallback");
        } else {
          file_signature_ = first_record[2];
          file_size_ = reader->original_size();
          spvz_ = std::move(reader);
          if (!ReadIndex(std::filesystem::path(spvz_path.string() + ".idx"))) {
            REXGPU_WARN("[shader library] compressed pack needs its .idx; runtime fallback");
            spvz_.reset();
          } else {
            indexed_ = true;
            return true;
          }
        }
      }
    }
  }
#endif  // REX_PLATFORM_IOS
""", "spirv_library.cpp: open compressed pack", "compressed pack rejected; runtime fallback")

hunk("sdk/src/graphics/pipeline/shader/spirv_library.cpp",
"""  const auto found = entries_.find(Key{shader->ucode_data_hash(), translation.modification(), signature,
                                      uint64_t(shader->type())});
  if (found == entries_.end() || fseek(file_, long(found->second), SEEK_SET)) return false;
  last_failure_ = "record/signature mismatch";
  Record record;
  if (fread(record.data(), 1, sizeof(record), file_) != sizeof(record)) return false;
""",
"""  const auto found = entries_.find(Key{shader->ucode_data_hash(), translation.modification(), signature,
                                      uint64_t(shader->type())});
  if (found == entries_.end()) return false;
  last_failure_ = "record/signature mismatch";
  Record record;
#if REX_PLATFORM_IOS
  if (spvz_) {
    if (!spvz_->Read(found->second, record.data(), sizeof(record))) return false;
  } else
#endif
  if (fseek(file_, long(found->second), SEEK_SET) ||
      fread(record.data(), 1, sizeof(record), file_) != sizeof(record)) return false;
""", "spirv_library.cpp: read record from compressed pack", "!spvz_->Read(found->second, record.data(), sizeof(record))")

hunk("sdk/src/graphics/pipeline/shader/spirv_library.cpp",
"""  if (fread(payload.data(), 1, payload.size(), file_) != payload.size() ||
      XXH3_64bits(payload.data(), payload.size()) != record[9] ||
""",
"""#if REX_PLATFORM_IOS
  const bool payload_ok = spvz_ ? spvz_->Read(found->second + sizeof(record), payload.data(), payload.size())
                                : fread(payload.data(), 1, payload.size(), file_) == payload.size();
#else
  const bool payload_ok = fread(payload.data(), 1, payload.size(), file_) == payload.size();
#endif
  if (!payload_ok ||
      XXH3_64bits(payload.data(), payload.size()) != record[9] ||
""", "spirv_library.cpp: read payload from compressed pack", "const bool payload_ok = spvz_")


# --- iOS input driver: touch + MFi gamepad -> X_INPUT_STATE ---------------
hunk("sdk/src/input/input_system.cpp",
"""#if REX_PLATFORM_SWITCH
#include <rex/input/switch/switch_input_driver.h>
#else
#include <rex/input/sdl/sdl_input_driver.h>
#endif
""",
"""#if REX_PLATFORM_SWITCH
#include <rex/input/switch/switch_input_driver.h>
#else
#include <rex/input/sdl/sdl_input_driver.h>
#endif
#if REX_PLATFORM_IOS
#include "ds2_ios/ios_touch_input_driver.h"
#endif
""", "input_system: include iOS touch driver", '#include "ds2_ios/ios_touch_input_driver.h"')

hunk("sdk/src/input/input_system.cpp",
"""    // MnK driver (keyboard/mouse -> controller emulation)
""",
"""#if REX_PLATFORM_IOS
    {
      auto touch_driver = std::make_unique<ds2_ios::IosTouchInputDriver>(nullptr, 0);
      if (touch_driver->Setup() == X_STATUS_SUCCESS) {
        input->AddDriver(std::move(touch_driver));
      }
    }
#endif

    // MnK driver (keyboard/mouse -> controller emulation)
""", "input_system: register iOS touch driver", "IosTouchInputDriver>(nullptr, 0)")

# --- Paths: game data in Documents, config/shaders in the app bundle -------
hunk("app/src/ds2_app.h",
"""#include <rex/cvar.h>
#include <rex/filesystem.h>
""",
"""#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/platform.h>
#if REX_PLATFORM_IOS
#include "ds2_ios/ios_paths.h"
#endif
""", "ds2_app.h: iOS path helper includes", '#include "ds2_ios/ios_paths.h"')

hunk("app/src/ds2_app.h",
"""  void OnConfigurePaths(rex::PathConfig& paths) override {
    const auto dir = rex::filesystem::GetExecutableFolder();
""",
"""  void OnConfigurePaths(rex::PathConfig& paths) override {
#if REX_PLATFORM_IOS
    // 5.4 GiB of game data is copied into Documents via Files, not bundled.
    // ds2.toml and the compressed shader pack stay in the .app bundle.
    const auto docs = ds2_ios::DocumentsPath();
    if (paths.game_data_root.empty()) paths.game_data_root = docs / "game_root";
    if (paths.user_data_root.empty()) paths.user_data_root = docs / "user";
    if (paths.cache_root.empty()) paths.cache_root = docs / "user" / "cache";
    if (paths.config_path.empty()) paths.config_path = ds2_ios::BundlePath() / "ds2.toml";
    return;
#endif
    const auto dir = rex::filesystem::GetExecutableFolder();
""", "ds2_app.h: iOS Documents/bundle path mapping", "docs / \"game_root\"")

# --- Build: compile the iOS platform sources into the app ------------------
hunk("app/CMakeLists.txt",
"""add_executable(ds2 ${DS2_SOURCES})
target_include_directories(ds2 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${DS2_GENERATED_DIR}")
""",
"""add_executable(ds2 ${DS2_SOURCES})
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
""", "app cmake: compile iOS platform layer", '"${CMAKE_CURRENT_SOURCE_DIR}/../ios_port/src/ios_paths.mm"')

# --- MoltenVK: static entry points instead of dlopen on iOS ----------------
hunk("sdk/src/ui/vulkan/vulkan_instance.cpp",
"""#if REX_PLATFORM_MAC
  const MacOSVulkanRuntimePaths macos_runtime_paths = DetectMacOSVulkanRuntimePaths();
""",
"""#if REX_PLATFORM_MAC && !REX_PLATFORM_IOS
  const MacOSVulkanRuntimePaths macos_runtime_paths = DetectMacOSVulkanRuntimePaths();
""",
     "vulkan_instance: skip macOS dlopen path", "#if REX_PLATFORM_MAC && !REX_PLATFORM_IOS\n  const MacOSVulkanRuntimePaths")

hunk("sdk/src/ui/vulkan/vulkan_instance.cpp",
     "#elif REX_PLATFORM_SWITCH\n",
     "#elif REX_PLATFORM_SWITCH || REX_PLATFORM_IOS\n",
     "vulkan_instance: direct vkGetInstanceProcAddr for MoltenVK",
     "#elif REX_PLATFORM_SWITCH || REX_PLATFORM_IOS")

# api.h defines VK_NO_PROTOTYPES for every platform, so the two statically
# linked entry points are not declared by vulkan.h. The Switch build declares
# them by hand; iOS (static libMoltenVK.a) needs exactly the same declarations.
hunk("sdk/src/ui/vulkan/vulkan_instance.cpp",
"""#if REX_PLATFORM_SWITCH
// NVK for Horizon is linked statically (Mesa's libvulkan.a) and exports the
// entry points directly. api.h defines VK_NO_PROTOTYPES, so they have to be
// declared by hand.
""",
"""#if REX_PLATFORM_SWITCH || REX_PLATFORM_IOS
// NVK for Horizon (Mesa's libvulkan.a) and MoltenVK for iOS (libMoltenVK.a)
// are linked statically and export the entry points directly. api.h defines
// VK_NO_PROTOTYPES, so they have to be declared by hand.
""", "vulkan_instance: declare static Vulkan entry points on iOS",
     "MoltenVK for iOS (libMoltenVK.a)")

hunk("sdk/src/ui/vulkan/vulkan_instance.cpp",
     "#endif  // REX_PLATFORM_SWITCH\n",
     "#endif  // REX_PLATFORM_SWITCH || REX_PLATFORM_IOS\n",
     "vulkan_instance: entry point guard comment",
     "#endif  // REX_PLATFORM_SWITCH || REX_PLATFORM_IOS")

# On iOS there is no dlopen'ed loader: GetSymbol() would return nullptr and
# overwrite the two pointers set above, making Create() fail at runtime.
hunk("sdk/src/ui/vulkan/vulkan_instance.cpp",
     "#if !REX_PLATFORM_SWITCH\n",
"""#if !REX_PLATFORM_SWITCH && !REX_PLATFORM_IOS
// iOS / Switch already filled these two from the statically linked runtime.
""", "vulkan_instance: do not clobber static entry points on iOS",
     "#if !REX_PLATFORM_SWITCH && !REX_PLATFORM_IOS")

hunk("sdk/cmake/rexglue_helpers.cmake",
     "    if(APPLE AND REXGLUE_USE_VULKAN)\n",
     "    if(APPLE AND NOT REXGLUE_PLATFORM_IOS AND REXGLUE_USE_VULKAN)\n",
     "cmake helpers: no macOS dylib staging on iOS", "NOT REXGLUE_PLATFORM_IOS AND REXGLUE_USE_VULKAN")

hunk("app/CMakeLists.txt",
"""else()
    rexglue_configure_target(ds2)
endif()
""",
"""elseif(REXGLUE_PLATFORM_IOS)
    # No dlopen on iOS: link the Xenos GPU plugin into the executable, exactly
    # as the Switch build does.
    rexglue_configure_target(ds2 GPU_PLUGINS xenos)
else()
    rexglue_configure_target(ds2)
endif()
""", "app cmake: static GPU plugin on iOS", "    # No dlopen on iOS: link the Xenos GPU plugin")

# Depends on the iOS `elseif()` branch above, so it must stay AFTER it.
hunk("app/CMakeLists.txt",
"""    rexglue_configure_target(ds2 GPU_PLUGINS xenos)
else()
""",
"""    rexglue_configure_target(ds2 GPU_PLUGINS xenos)
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
""", "app cmake: link MoltenVK, zlib and iOS frameworks",
     "REXGLUE_IOS_MOLTENVK must point at libMoltenVK.a")

hunk("sdk/src/graphics/pipeline/shader/spirv_library.cpp",
"""  if (!shader || !file_ || translation.is_translated()) return false;
""",
"""#if REX_PLATFORM_IOS
  // A compressed pack has no FILE* - SpvzReader owns the data instead.
  if (!shader || (!file_ && !spvz_) || translation.is_translated()) return false;
#else
  if (!shader || !file_ || translation.is_translated()) return false;
#endif
""", "spirv_library.cpp: allow file-less compressed Restore", "(!file_ && !spvz_)")

hunk("app/src/ds2_app.h",
"""    if (rex::cvar::GetFlagInfo("ds2_shader_library") == nullptr) return;
    const auto dir = rex::filesystem::GetExecutableFolder();
    if (dir.empty()) return;
    std::error_code ec;
""",
"""    if (rex::cvar::GetFlagInfo("ds2_shader_library") == nullptr) return;
#if defined(REX_PLATFORM_IOS) && REX_PLATFORM_IOS
    // The 501 MiB compressed shader pack is copied into Documents via Files,
    // together with game_root/. It never goes through GitHub or the .ipa.
    const auto dir = ds2_ios::DocumentsPath();
#else
    const auto dir = rex::filesystem::GetExecutableFolder();
#endif
    if (dir.empty()) return;
    std::error_code ec;
""", "ds2_app.h: resolve shader pack from Documents on iOS",
     "The 501 MiB compressed shader pack is copied into Documents")

hunk("sdk/thirdparty/CMakeLists.txt",
"""    if(APPLE)
        # Build the loader and MoltenVK revisions shipped by the pinned SDK.
""",
"""    if(APPLE AND NOT REXGLUE_PLATFORM_IOS)
        # iOS: no loader (vulkan-loader CMake is malformed for arm64 non-WIN32)
        # and no MoltenVK build - REXGLUE_IOS_MOLTENVK links the prebuilt
        # static libMoltenVK.a from the KhronosGroup release.
        # Build the loader and MoltenVK revisions shipped by the pinned SDK.
""", "thirdparty cmake: skip vulkan-loader and moltenvk on iOS",
     "if(APPLE AND NOT REXGLUE_PLATFORM_IOS)")

# --- fiber_switch.cpp: ELF-only assembler directives on Mach-O -------------
# Mach-O prefixes C symbols with '_' and has no .type/.size directives.
# clang's integrated assembler rejects them with "unknown directive", and a
# plain `rex_fiber_swap` label would not satisfy the linker's `_rex_fiber_swap`.
hunk("sdk/src/core/fiber_switch.cpp",
"""__asm__(
    ".text\\n"
    ".balign 4\\n"
    ".global rex_fiber_swap\\n"
    ".type rex_fiber_swap, %function\\n"
    "rex_fiber_swap:\\n"
""",
"""#if REX_PLATFORM_IOS
// Mach-O (iOS) prefixes C symbols with '_' and has no ELF .type/.size
// directives; clang's integrated assembler rejects both.
#define REX_FIBER_ASM_ALIGN ".p2align 2\\n"
#define REX_FIBER_ASM_PROLOG(name) ".globl _" #name "\\n" "_" #name ":\\n"
#define REX_FIBER_ASM_EPILOG(name) ""
#define REX_FIBER_ASM_TRAMPOLINE "_rex_fiber_trampoline"
#else
#define REX_FIBER_ASM_ALIGN ".balign 4\\n"
#define REX_FIBER_ASM_PROLOG(name) \\
    ".global " #name "\\n" ".type " #name ", %function\\n" #name ":\\n"
#define REX_FIBER_ASM_EPILOG(name) ".size " #name ", .-" #name "\\n"
#define REX_FIBER_ASM_TRAMPOLINE "rex_fiber_trampoline"
#endif

__asm__(
    ".text\\n"
    REX_FIBER_ASM_ALIGN
    REX_FIBER_ASM_PROLOG(rex_fiber_swap)
""", "fiber_switch: Mach-O symbol names and prologues",
     "Mach-O (iOS) prefixes C symbols with '_'")

hunk("sdk/src/core/fiber_switch.cpp",
"""    ".size rex_fiber_swap, .-rex_fiber_swap\\n"
    "\\n"
    ".balign 4\\n"
    ".global rex_fiber_entry\\n"
    ".type rex_fiber_entry, %function\\n"
    "rex_fiber_entry:\\n"
    "    mov x29, #0\\n"
    "    mov x30, #0\\n"
    "    b   rex_fiber_trampoline\\n"
    ".size rex_fiber_entry, .-rex_fiber_entry\\n");
""",
"""    REX_FIBER_ASM_EPILOG(rex_fiber_swap)
    "\\n"
    REX_FIBER_ASM_ALIGN
    REX_FIBER_ASM_PROLOG(rex_fiber_entry)
    "    mov x29, #0\\n"
    "    mov x30, #0\\n"
    "    b   " REX_FIBER_ASM_TRAMPOLINE "\\n"
    REX_FIBER_ASM_EPILOG(rex_fiber_entry)
);
""", "fiber_switch: Mach-O epilogues and trampoline symbol",
     "    REX_FIBER_ASM_EPILOG(rex_fiber_swap)")

# --- memory_posix.cpp: mach_vm.h is an #error stub in the iPhoneOS SDK -----
# iOS arm64 still has the 64-bit MIG call vm_region_64() (mach/vm_map.h), so
# QueryProtect keeps working instead of falling back to kNoAccess, which the
# MMIO fault handler relies on.
hunk("sdk/src/core/memory_posix.cpp",
"""#include <cerrno>
#include <cstddef>
""",
"""#include <rex/platform.h>  // before <mach/*>: the iOS branch needs REX_PLATFORM_IOS

#include <cerrno>
#include <cstddef>
""", "memory_posix: define platform macros before mach headers",
     "before <mach/*>: the iOS branch needs REX_PLATFORM_IOS")

hunk("sdk/src/core/memory_posix.cpp",
"""#if REX_PLATFORM_MAC
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/vm_region.h>
#endif  // REX_PLATFORM_MAC
""",
"""#if REX_PLATFORM_MAC
#include <mach/mach.h>
#include <mach/vm_region.h>
#if REX_PLATFORM_IOS
// iPhoneOS SDK ships mach_vm.h as an #error stub. vm_region_64() from
// mach/vm_map.h is the 64-bit MIG equivalent on arm64 iOS.
#include <mach/vm_map.h>
#else
#include <mach/mach_vm.h>
#endif
#endif  // REX_PLATFORM_MAC
""", "memory_posix: no mach_vm.h on iOS",
     "iPhoneOS SDK ships mach_vm.h as an #error stub")

hunk("sdk/src/core/memory_posix.cpp",
"""bool QueryProtect(void* base_address, size_t& length, PageAccess& access_out) {
#if REX_PLATFORM_MAC
  mach_vm_address_t address = reinterpret_cast<mach_vm_address_t>(base_address);
  mach_vm_size_t region_size = 0;
  vm_region_basic_info_data_64_t info;
  mach_msg_type_number_t info_count = VM_REGION_BASIC_INFO_COUNT_64;
  mach_port_t object_name;

  kern_return_t kr =
      mach_vm_region(mach_task_self(), &address, &region_size, VM_REGION_BASIC_INFO_64,
                     reinterpret_cast<vm_region_info_t>(&info), &info_count, &object_name);
  if (kr != KERN_SUCCESS) {
""",
"""bool QueryProtect(void* base_address, size_t& length, PageAccess& access_out) {
#if REX_PLATFORM_MAC && !REX_PLATFORM_IOS
  mach_vm_address_t address = reinterpret_cast<mach_vm_address_t>(base_address);
  mach_vm_size_t region_size = 0;
  vm_region_basic_info_data_64_t info;
  mach_msg_type_number_t info_count = VM_REGION_BASIC_INFO_COUNT_64;
  mach_port_t object_name;

  kern_return_t kr =
      mach_vm_region(mach_task_self(), &address, &region_size, VM_REGION_BASIC_INFO_64,
                     reinterpret_cast<vm_region_info_t>(&info), &info_count, &object_name);
#elif REX_PLATFORM_IOS
  vm_address_t address = reinterpret_cast<vm_address_t>(base_address);
  vm_size_t region_size = 0;
  vm_region_basic_info_data_64_t info;
  mach_msg_type_number_t info_count = VM_REGION_BASIC_INFO_COUNT_64;
  mach_port_t object_name = MACH_PORT_NULL;

  kern_return_t kr =
      vm_region_64(mach_task_self(), &address, &region_size, VM_REGION_BASIC_INFO_64,
                   reinterpret_cast<vm_region_info_t>(&info), &info_count, &object_name);
#endif

#if REX_PLATFORM_MAC
  if (kr != KERN_SUCCESS) {
""", "memory_posix: vm_region_64 fallback for iOS QueryProtect",
     "vm_region_64(mach_task_self()")

# --- let ResolveShaderLibrary() accept the compressed sibling --------------
hunk("app/src/ds2_app.h",
"""    auto usable = [](const std::filesystem::path& p) {
      std::error_code e;
      auto idx = p;
      idx += ".idx";
      return std::filesystem::is_regular_file(p, e) && std::filesystem::is_regular_file(idx, e);
    };
""",
"""    auto usable = [](const std::filesystem::path& p) {
      std::error_code e;
#if defined(REX_PLATFORM_IOS) && REX_PLATFORM_IOS
      // iOS ships DS2SPVZ1 instead of the 1.6 GiB raw pack. The loader in
      // spirv_library.cpp swaps in the .spvz sibling with the same stem.
      if (!std::filesystem::is_regular_file(p, e)) {
        auto spvz = p;
        spvz.replace_extension(".spvz");
        auto spvz_idx = std::filesystem::path(spvz.string() + ".idx");
        return std::filesystem::is_regular_file(spvz, e) &&
               std::filesystem::is_regular_file(spvz_idx, e);
      }
#endif
      auto idx = p;
      idx += ".idx";
      return std::filesystem::is_regular_file(p, e) && std::filesystem::is_regular_file(idx, e);
    };
""", "ds2_app.h: accept compressed shader pack", 'spvz.replace_extension(".spvz")')


# --- ds2_app.h: log overlay sink + writable log dir on iOS -----------------
hunk("app/src/ds2_app.h",
"""#include "ds2_ios/ios_paths.h"
#endif
""",
"""#include "ds2_ios/ios_paths.h"
#include "ds2_ios/ios_log_overlay.h"
#endif
""", "ds2_app.h: log overlay include", '#include "ds2_ios/ios_log_overlay.h"')

hunk("app/src/ds2_app.h",
"""  void OnPostInitLogging() override {
    SetIfDefault("gpu_plugin", "xenos");
""",
"""  void OnPostInitLogging() override {
#if defined(REX_PLATFORM_IOS) && REX_PLATFORM_IOS
    // Capture every log line for the on-device overlay (3 taps top-center)
    // and Documents/ds2_log.txt. Must run after logging init, otherwise
    // InitLogging() wipes g_extra_sinks.
    ds2_ios::InstallLogSink();
#endif
    SetIfDefault("gpu_plugin", "xenos");
""", "ds2_app.h: install log overlay sink", "ds2_ios::InstallLogSink()")

hunk("sdk/src/ui/rex_app.cpp",
"""#include <rex/logging.h>
#include <rex/ui/overlay/achievement_toast.h>
""",
"""#include <rex/logging.h>
#if defined(REX_PLATFORM_IOS) && REX_PLATFORM_IOS
#include "ds2_ios/ios_paths.h"
#endif
#include <rex/ui/overlay/achievement_toast.h>
""", "rex_app: iOS log path include", 'ds2_ios/ios_paths.h"')

hunk("sdk/src/ui/rex_app.cpp",
"""  if (log_file_cvar.empty()) {
    log_config.app_name = std::string(GetName());
    log_config.log_dir = (exe_dir / "logs").string();
  }
""",
"""  if (log_file_cvar.empty()) {
    log_config.app_name = std::string(GetName());
#if defined(REX_PLATFORM_IOS) && REX_PLATFORM_IOS
    // The .app bundle is read-only on iOS: rotating logs go to Documents,
    // where the runtime can actually create them.
    log_config.log_dir = (ds2_ios::DocumentsPath() / "logs").string();
#else
    log_config.log_dir = (exe_dir / "logs").string();
#endif
  }
""", "rex_app: rotating logs into Documents on iOS",
     "rotating logs go to Documents")

# --- ds2_app.h: the shader pack lives in Documents, not the .app bundle ----
# spirv_library.cpp resolves a RELATIVE cvar against GetExecutableFolder()
# (the .app bundle). ResolveShaderLibrary() checks Documents, so it must hand
# the loader an absolute path there or the 501 MiB pack is never found.
hunk("app/src/ds2_app.h",
"""    if (usable(wanted)) {
      rex::cvar::SetFlagByName("ds2_shader_library", name);
      return;
    }
""",
"""    if (usable(wanted)) {
#if defined(REX_PLATFORM_IOS) && REX_PLATFORM_IOS
      // The loader resolves relative names against the .app bundle; on iOS the
      // compressed pack lives in Documents, so pass an absolute path there.
      rex::cvar::SetFlagByName("ds2_shader_library", wanted.string());
#else
      rex::cvar::SetFlagByName("ds2_shader_library", name);
#endif
      return;
    }
""", "ds2_app.h: absolute shader pack path in Documents",
     "compressed pack lives in Documents")

hunk("app/src/ds2_app.h",
"""    const std::string found = best.filename().string();
    rex::cvar::SetFlagByName("ds2_shader_library", found);
""",
"""#if defined(REX_PLATFORM_IOS) && REX_PLATFORM_IOS
    const std::string found = best.string();
#else
    const std::string found = best.filename().string();
#endif
    rex::cvar::SetFlagByName("ds2_shader_library", found);
""", "ds2_app.h: versioned shader pack fallback stays absolute",
     "const std::string found = best.string();")

# --- ds2_watchdog.cpp: glibc mallinfo() does not exist on Darwin -----------
hunk("app/src/ds2_watchdog.cpp",
"""#include <ctime>
#include <malloc.h>
#include <deque>
""",
"""#include <ctime>
#if defined(__APPLE__)
#include <malloc/malloc.h>
// Darwin has no glibc mallinfo(). malloc_zone_statistics gives the same two
// numbers from the default zone (useful for jetsam monitoring on iPhone 11).
struct Ds2MallocStats {
  size_t arena;
  size_t uordblks;
};
static Ds2MallocStats Ds2Mallinfo() {
  Ds2MallocStats s{};
  malloc_statistics_t st{};
  malloc_zone_statistics(malloc_default_zone(), &st);
  s.arena = st.size_allocated;
  s.uordblks = st.size_in_use;
  return s;
}
#else
#include <malloc.h>
using Ds2MallocStats = struct mallinfo;
static Ds2MallocStats Ds2Mallinfo() { return mallinfo(); }
#endif
#include <deque>
""", "ds2_watchdog: Darwin malloc statistics", "Darwin has no glibc mallinfo()")

hunk("app/src/ds2_watchdog.cpp",
     "  const struct mallinfo mi = mallinfo();\n",
     "  const auto mi = Ds2Mallinfo();\n",
     "ds2_watchdog: use the portable mallinfo shim",
     "const auto mi = Ds2Mallinfo();", count=2)

# --- single static Mach-O: no embedded dylibs in a sideloaded .ipa --------
hunk("sdk/src/system/CMakeLists.txt",
"""if(REXGLUE_PLATFORM_SWITCH)
    # A NRO is a single static executable: no shared libraries, no dlopen.
    # rexglue_switch_configure_target links it whole into the title.
    add_library(rexruntime STATIC ${REXSYSTEM_SOURCES})
else()
""",
"""if(REXGLUE_PLATFORM_SWITCH OR REXGLUE_PLATFORM_IOS)
    # Switch NRO and sideloaded iOS app: one static executable, no embedded
    # dylibs. A .dylib would also force unresolved symbols (MoltenVK entry
    # points, the iOS input driver) to be satisfied at library link time.
    add_library(rexruntime STATIC ${REXSYSTEM_SOURCES})
else()
""", "system cmake: static rexruntime on iOS",
     "Switch NRO and sideloaded iOS app: one static executable")

hunk("sdk/src/graphics/CMakeLists.txt",
"""if(REXGLUE_PLATFORM_SWITCH)
    # Linked into the executable (gpu_plugin_loader.cpp calls rex_gpu_create
    # directly); a NRO cannot load plugins at run time.
    add_library(rexgpu-xenos STATIC ${REXGPU_XENOS_SOURCES})
else()
""",
"""if(REXGLUE_PLATFORM_SWITCH OR REXGLUE_PLATFORM_IOS)
    # Linked into the executable (gpu_plugin_loader.cpp calls rex_gpu_create
    # directly); neither an NRO nor a sideloaded iOS app loads plugins at runtime.
    add_library(rexgpu-xenos STATIC ${REXGPU_XENOS_SOURCES})
else()
""", "graphics cmake: static rexgpu-xenos on iOS",
     "neither an NRO nor a sideloaded iOS app loads plugins")

hunk("sdk/src/system/gpu_plugin_loader.cpp",
"""#if REX_PLATFORM_SWITCH
}  // namespace rex::system
""",
"""#if REX_PLATFORM_SWITCH || REX_PLATFORM_IOS
}  // namespace rex::system
""", "gpu_plugin_loader: direct rex_gpu_create on iOS",
     "#if REX_PLATFORM_SWITCH || REX_PLATFORM_IOS\n}  // namespace rex::system")

hunk("app/CMakeLists.txt",
"""    rexglue_configure_target(ds2 GPU_PLUGINS xenos)
    # Static MoltenVK exports vkGetInstanceProcAddr when linked directly;
""",
"""    rexglue_configure_target(ds2 GPU_PLUGINS xenos)
    # gpu_plugin_loader.cpp takes the static path on iOS, so the plugin archive
    # must be linked in (GPU_PLUGINS alone only stages loadable dylibs).
    target_link_libraries(ds2 PRIVATE rexgpu-xenos)
    # Static MoltenVK exports vkGetInstanceProcAddr when linked directly;
""", "app cmake: link the static GPU plugin archive on iOS",
     "gpu_plugin_loader.cpp takes the static path on iOS")

# --- SDL3 owns UIApplicationMain on iOS ------------------------------------
hunk("sdk/src/ui/windowed_app_main_sdl.cpp",
"""#include <rex/ui/windowed_app_context_sdl.h>

#if REX_PLATFORM_WIN32
""",
"""#include <rex/ui/windowed_app_context_sdl.h>

#if REX_PLATFORM_IOS
// SDL3-iOS owns UIApplicationMain. SDL_main.h renames our main() to SDL_main
// and supplies the real entry point that calls SDL_RunApp / UIKit startup.
#include <SDL3/SDL_main.h>
#endif

#if REX_PLATFORM_WIN32
""", "windowed_app_main_sdl: SDL3 iOS entry point",
     "SDL3-iOS owns UIApplicationMain")


hunk("app/CMakeLists.txt",
"""        "${CMAKE_CURRENT_SOURCE_DIR}/../ios_port/src/ios_paths.mm"
        "${CMAKE_CURRENT_SOURCE_DIR}/../ios_port/src/ios_touch_input_driver.cpp")
""",
"""        "${CMAKE_CURRENT_SOURCE_DIR}/../ios_port/src/ios_paths.mm"
        "${CMAKE_CURRENT_SOURCE_DIR}/../ios_port/src/ios_log_overlay.mm"
        "${CMAKE_CURRENT_SOURCE_DIR}/../ios_port/src/ios_touch_input_driver.cpp")
""", "app cmake: compile the log overlay", 'ios_log_overlay.mm')


# --- startup milestones: find where a black-screen launch stalls -----------
hunk("sdk/src/ui/windowed_app_main_sdl.cpp",
"""#if REX_PLATFORM_IOS
// SDL3-iOS owns UIApplicationMain. SDL_main.h renames our main() to SDL_main
// and supplies the real entry point that calls SDL_RunApp / UIKit startup.
#include <SDL3/SDL_main.h>
#endif
""",
"""#if REX_PLATFORM_IOS
// SDL3-iOS owns UIApplicationMain. SDL_main.h renames our main() to SDL_main
// and supplies the real entry point that calls SDL_RunApp / UIKit startup.
#include <SDL3/SDL_main.h>
#include "ds2_ios/ios_log_overlay.h"
#define DS2_MARK(stage) ds2_ios::DebugMark(stage)
#else
#define DS2_MARK(stage) ((void)0)
#endif
""", "windowed_app_main_sdl: overlay include + DS2_MARK", "#define DS2_MARK(stage)")

hunk("sdk/src/ui/windowed_app_main_sdl.cpp",
"""  auto remaining = rex::cvar::Init(argc, argv);
  rex::cvar::ApplyEnvironment();
  rex::InitLoggingEarly();

  int result;
  {
    rex::ui::SDLWindowedAppContext app_context;
    if (!app_context.Initialize()) {
      return EXIT_FAILURE;
    }
""",
"""  auto remaining = rex::cvar::Init(argc, argv);
  DS2_MARK("main: cvars ok");
  rex::cvar::ApplyEnvironment();
  rex::InitLoggingEarly();
  DS2_MARK("main: early logging ok");

  int result;
  {
    rex::ui::SDLWindowedAppContext app_context;
    DS2_MARK("main: initializing SDL context");
    if (!app_context.Initialize()) {
      return EXIT_FAILURE;
    }
    DS2_MARK("main: SDL context ready");
""", "windowed_app_main_sdl: milestones 1", '"main: SDL context ready"')

hunk("sdk/src/ui/windowed_app_main_sdl.cpp",
"""    std::unique_ptr<rex::ui::WindowedApp> app = rex::ui::GetWindowedAppCreator()(app_context);
""",
"""    std::unique_ptr<rex::ui::WindowedApp> app = rex::ui::GetWindowedAppCreator()(app_context);
    DS2_MARK("main: app object created");
""", "windowed_app_main_sdl: milestones 2", '"main: app object created"')

hunk("sdk/src/ui/windowed_app_main_sdl.cpp",
"""    result = app->OnInitialize() ? app_context.RunMainMessageLoop() : EXIT_FAILURE;
""",
"""    DS2_MARK("main: calling OnInitialize");
    result = app->OnInitialize() ? app_context.RunMainMessageLoop() : EXIT_FAILURE;
    DS2_MARK("main: message loop finished");
""", "windowed_app_main_sdl: milestones 3", '"main: calling OnInitialize"')

hunk("sdk/src/ui/rex_app.cpp",
"""  OnConfigurePaths(path_config);
""",
"""  DS2_MARK("rex_app: OnConfigurePaths begin");
  OnConfigurePaths(path_config);
  DS2_MARK("rex_app: OnConfigurePaths done");
""", "rex_app: OnConfigurePaths milestones", '"rex_app: OnConfigurePaths done"')

hunk("sdk/src/ui/rex_app.cpp",
"""  rex::InitLogging(log_config);
  rex::RegisterLogLevelCallback();
""",
"""  DS2_MARK("rex_app: InitLogging begin");
  rex::InitLogging(log_config);
  DS2_MARK("rex_app: InitLogging done");
  rex::RegisterLogLevelCallback();
""", "rex_app: InitLogging milestones", '"rex_app: InitLogging done"')

hunk("sdk/src/ui/rex_app.cpp",
"""  OnPostInitLogging();
""",
"""  DS2_MARK("rex_app: OnPostInitLogging done");
""", "rex_app: OnPostInitLogging milestone", '"rex_app: OnPostInitLogging done"')

hunk("app/src/ds2_app.h",
"""    if (paths.config_path.empty()) paths.config_path = ds2_ios::BundlePath() / "ds2.toml";
    return;
""",
"""    if (paths.config_path.empty()) paths.config_path = ds2_ios::BundlePath() / "ds2.toml";
    ds2_ios::DebugMark("ds2_app: OnConfigurePaths applied");
    return;
""", "ds2_app.h: OnConfigurePaths milestone", '"ds2_app: OnConfigurePaths applied"')

hunk("app/src/ds2_app.h",
"""    ds2_ios::InstallLogSink();
#endif
""",
"""    ds2_ios::InstallLogSink();
    ds2_ios::DebugMark("ds2_app: log sink installed");
#endif
""", "ds2_app.h: log sink milestone", '"ds2_app: log sink installed"')


hunk("sdk/src/ui/rex_app.cpp",
"""#include <rex/logging.h>
#if defined(REX_PLATFORM_IOS) && REX_PLATFORM_IOS
#include "ds2_ios/ios_paths.h"
#endif
""",
"""#include <rex/logging.h>
#if defined(REX_PLATFORM_IOS) && REX_PLATFORM_IOS
#include "ds2_ios/ios_paths.h"
#include "ds2_ios/ios_log_overlay.h"
#endif
""", "rex_app: DebugMark include", 'ds2_ios/ios_log_overlay.h"')

hunk("sdk/src/ui/rex_app.cpp",
     'DS2_MARK("rex_app:',
     'ds2_ios::DebugMark("rex_app:',
     "rex_app: DebugMark calls", 'ds2_ios::DebugMark("rex_app:', count=5)


# --- sdl context: fine-grained marks inside Initialize ---------------------
hunk("sdk/src/ui/windowed_app_context_sdl.cpp",
"""#include <rex/platform.h>
#include <rex/ui/flags.h>
#include <rex/ui/window_sdl.h>
""",
"""#include <rex/platform.h>
#include <rex/ui/flags.h>
#include <rex/ui/window_sdl.h>

#if defined(REX_PLATFORM_IOS) && REX_PLATFORM_IOS
#include "ds2_ios/ios_log_overlay.h"
#define DS2_CTX_MARK(stage) ds2_ios::DebugMark(stage)
#else
#define DS2_CTX_MARK(stage) ((void)0)
#endif
""", "sdlctx: DebugMark include", "#define DS2_CTX_MARK")

hunk("sdk/src/ui/windowed_app_context_sdl.cpp",
"""  if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
    REXLOG_ERROR("SDL_InitSubSystem(SDL_INIT_VIDEO) failed: {}", SDL_GetError());
    return false;
  }
  const char* video_driver_in_use = SDL_GetCurrentVideoDriver();
  REXLOG_INFO("SDL video driver: {}", video_driver_in_use ? video_driver_in_use : "unknown");
  uint32_t first = SDL_RegisterEvents(2);
  if (first == 0) {
    REXLOG_ERROR("SDL_RegisterEvents failed: {}", SDL_GetError());
    return false;
  }
  wakeup_event_type_ = first;
  paint_event_type_ = first + 1;
  if (!SDL_AddEventWatch(WatchEvent, this)) {
    REXLOG_ERROR("SDL_AddEventWatch failed: {}", SDL_GetError());
    return false;
  }
  event_watch_registered_ = true;
  return true;
}
""",
"""  DS2_CTX_MARK("sdlctx: calling SDL_InitSubSystem(VIDEO)");
  if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
    REXLOG_ERROR("SDL_InitSubSystem(SDL_INIT_VIDEO) failed: {}", SDL_GetError());
    return false;
  }
  const char* video_driver_in_use = SDL_GetCurrentVideoDriver();
  DS2_CTX_MARK(video_driver_in_use
                   ? ("sdlctx: video ok, driver=" + std::string(video_driver_in_use)).c_str()
                   : "sdlctx: video ok, driver=unknown");
  REXLOG_INFO("SDL video driver: {}", video_driver_in_use ? video_driver_in_use : "unknown");
  uint32_t first = SDL_RegisterEvents(2);
  if (first == 0) {
    REXLOG_ERROR("SDL_RegisterEvents failed: {}", SDL_GetError());
    return false;
  }
  DS2_CTX_MARK("sdlctx: events registered");
  wakeup_event_type_ = first;
  paint_event_type_ = first + 1;
  if (!SDL_AddEventWatch(WatchEvent, this)) {
    REXLOG_ERROR("SDL_AddEventWatch failed: {}", SDL_GetError());
    return false;
  }
  DS2_CTX_MARK("sdlctx: event watch ok");
  event_watch_registered_ = true;
  return true;
}
""", "sdlctx: fine marks in Initialize", 'DS2_CTX_MARK("sdlctx: calling SDL_InitSubSystem(VIDEO)")')


hunk("sdk/src/ui/windowed_app_context_sdl.cpp",
"""  DS2_CTX_MARK("sdlctx: calling SDL_InitSubSystem(VIDEO)");
""",
"""  {
    std::string drivers;
    for (int i = 0; i < SDL_GetNumVideoDrivers(); ++i) {
      const char* d = SDL_GetVideoDriver(i);
      if (d) drivers += (drivers.empty() ? "" : ",") + std::string(d);
    }
    DS2_CTX_MARK("sdlctx: compiled video drivers: " + drivers);
  }
  DS2_CTX_MARK("sdlctx: calling SDL_InitSubSystem(VIDEO)");
""", "sdlctx: compiled drivers list", 'sdlctx: compiled video drivers')

hunk("sdk/src/ui/windowed_app_context_sdl.cpp",
"""    REXLOG_ERROR("SDL_InitSubSystem(SDL_INIT_VIDEO) failed: {}", SDL_GetError());
    return false;
""",
"""    REXLOG_ERROR("SDL_InitSubSystem(SDL_INIT_VIDEO) failed: {}", SDL_GetError());
    DS2_CTX_MARK(std::string("sdlctx: VIDEO INIT FAILED: ") + SDL_GetError());
    return false;
""", "sdlctx: failure mark", 'VIDEO INIT FAILED')


hunk("sdk/src/ui/windowed_app_main_sdl.cpp",
"""    if (!app_context.Initialize()) {
      return EXIT_FAILURE;
    }
""",
"""    if (!app_context.Initialize()) {
      DS2_MARK("main: app_context.Initialize FAILED");
      return EXIT_FAILURE;
    }
""", "main: mark Initialize failure", 'app_context.Initialize FAILED')

hunk("sdk/src/ui/windowed_app_main_sdl.cpp",
"""    result = app->OnInitialize() ? app_context.RunMainMessageLoop() : EXIT_FAILURE;
""",
"""    result = app->OnInitialize() ? app_context.RunMainMessageLoop() : EXIT_FAILURE;
    DS2_MARK("main: RunMainMessageLoop returned");
""", "main: mark loop return", 'RunMainMessageLoop returned')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root", help="path to the DS2_switch clone")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    root = pathlib.Path(a.root)
    ok = skipped = failed = 0
    for h in HUNKS:
        p = root / h["rel"]
        if not p.exists():
            print(f"MISSING  {h['label']}: {h['rel']}")
            failed += 1
            continue
        text = p.read_text(encoding="utf-8")
        if h["marker"] in text:
            print(f"SKIP     {h['label']}")
            skipped += 1
            continue
        n = text.count(h["old"])
        if n != h["count"]:
            print(f"FAIL     {h['label']}: anchor found {n}x, expected {h['count']}x")
            failed += 1
            continue
        if a.dry_run:
            print(f"OK(dry)  {h['label']}")
            ok += 1
            continue
        backup = root / (h["rel"] + ".orig")
        if not backup.exists():
            shutil.copy2(p, backup)
        p.write_text(text.replace(h["old"], h["new"]), encoding="utf-8")
        print(f"APPLIED  {h['label']}")
        ok += 1
    print(f"\n{ok} applied, {skipped} already present, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
