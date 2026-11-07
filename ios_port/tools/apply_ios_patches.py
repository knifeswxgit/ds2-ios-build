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
#include "ds2_ios/spvz_reader.h"
#endif
""", "spirv_library.h: compressed reader include", "ds2_ios/spvz_reader.h")

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
"""    const auto dir = rex::filesystem::GetExecutableFolder();
    if (dir.empty()) return;
    std::error_code ec;
""",
"""#if defined(REX_PLATFORM_IOS) && REX_PLATFORM_IOS
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
