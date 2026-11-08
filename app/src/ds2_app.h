// ds2 - host application class. Starts from the `rexglue init` template; game-specific hooks go here.

#pragma once

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/platform.h>
#if REX_PLATFORM_IOS
#include "ds2_ios/ios_paths.h"
#include "ds2_ios/ios_log_overlay.h"
#endif
#include <rex/logging.h>
#include <rex/rex_app.h>

#include "ds2_capture.h"
#include "ds2_dlc.h"
#include "ds2_input_script.h"
#include "ds2_mode_overlay.h"
#include "ds2_pgo.h"
#include "ds2_watchdog.h"
#include "ds2_func_counts.h"

#include <filesystem>
#include <string>
#include <system_error>

class Ds2App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Ds2App>(new Ds2App(ctx, "ds2", PPCImageConfig));
  }

 protected:
  // Portable layout, as nfsmw-nx does (app/src/nfsmw_app.h): with no --game_data_root / --user_data_root, look
  // next to the executable. On the Switch that is the NRO folder, sdmc:/switch/ds2/:
  //   game_root/   the game folder (Default.xex, GameData*.bdt, ...)
  //   user/        saves and installed content (created by the runtime)
  // Runs before ds2.toml is read, so only the command line overrides it (SDK ordering, sdk/src/ui/rex_app.cpp).
  void OnConfigurePaths(rex::PathConfig& paths) override {
#if REX_PLATFORM_IOS
    // 5.4 GiB of game data is copied into Documents via Files, not bundled.
    // ds2.toml and the compressed shader pack stay in the .app bundle.
    const auto docs = ds2_ios::DocumentsPath();
    if (paths.game_data_root.empty()) paths.game_data_root = docs / "game_root";
    if (paths.user_data_root.empty()) paths.user_data_root = docs / "user";
    if (paths.cache_root.empty()) paths.cache_root = docs / "user" / "cache";
    if (paths.config_path.empty()) {
      // The config stays editable without a rebuild: Documents/ds2.toml wins
      // and the bundled copy is only the template copied on the first launch.
      const std::filesystem::path docs_cfg = docs / "ds2.toml";
      const std::filesystem::path bundle_cfg = ds2_ios::BundlePath() / "ds2.toml";
      std::error_code cec;
      if (!std::filesystem::exists(docs_cfg, cec)) {
        std::error_code cperr;
        std::filesystem::copy_file(bundle_cfg, docs_cfg,
                                   std::filesystem::copy_options::overwrite_existing, cperr);
      }
      paths.config_path = docs_cfg;
    }
    ds2_ios::DebugMark("ds2_app: OnConfigurePaths applied");
    return;
#endif
    const auto dir = rex::filesystem::GetExecutableFolder();
    if (dir.empty()) return;
    std::error_code ec;
    if (paths.game_data_root.empty() && std::filesystem::is_directory(dir / "game_root", ec))
      paths.game_data_root = dir / "game_root";
    if (rex::cvar::GetFlagByName("user_data_root").empty()) {
      paths.user_data_root = dir / "user";
      if (rex::cvar::GetFlagByName("cache_root").empty()) paths.cache_root = dir / "user" / "cache";
    }
  }

  // Without a GPU plugin the runtime drops the game's graphics ("no GPU emulation loaded"); default to xenos
  // unless the command line or ds2.toml chose otherwise.
  void OnPostInitLogging() override {
#if defined(REX_PLATFORM_IOS) && REX_PLATFORM_IOS
    // Capture every log line for the on-device overlay (3 taps top-center)
    // and Documents/ds2_log.txt. Must run after logging init, otherwise
    // InitLogging() wipes g_extra_sinks.
    ds2_ios::InstallLogSink();
    ds2_ios::DebugMark("ds2_app: log sink installed");
#endif
    SetIfDefault("gpu_plugin", "xenos");
#if defined(__SWITCH__)
    // Switch defaults proven on the console (docs/handoff/11-switch-level-load-diag.md, tests 20-21):
    // 16 MB host stacks for ~44 guest threads exhausted the heap (PC measurement: <= 3.6 MB used).
    SetIfDefault("host_thread_stack_mb", "6");
#endif
    ResolveShaderLibrary();
    ds2::watchdog::ApplyPlatformSwitches();
    ds2::func_counts::Configure();
    ds2::mode_overlay::ApplyConfig();
  }

  /*
   * v121: the shader pack keeps one name, ds2_shaders.spvlib (+ .idx), so an update replaces only ds2.nro and
   * ds2.toml. If the file named in ds2.toml is not next to the NRO, the versioned pack of an older build
   * (ds2_shaders_vNNN.spvlib, the newest number) is used instead, so nothing has to be renamed when updating.
   * Done here, before the renderer opens the pack; the pack loader itself is untouched (its sources are part of
   * the translator signature).
   */
  static void ResolveShaderLibrary() {
    if (rex::cvar::GetFlagInfo("ds2_shader_library") == nullptr) return;
#if defined(REX_PLATFORM_IOS) && REX_PLATFORM_IOS
    // The 501 MiB compressed shader pack is copied into Documents via Files,
    // together with game_root/. It never goes through GitHub or the .ipa.
    const auto dir = ds2_ios::DocumentsPath();
#else
    const auto dir = rex::filesystem::GetExecutableFolder();
#endif
    if (dir.empty()) return;
    std::error_code ec;
    auto usable = [](const std::filesystem::path& p) {
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
    std::string name = rex::cvar::GetFlagByName("ds2_shader_library");
    if (name.empty()) name = "ds2_shaders.spvlib";
    std::filesystem::path wanted(name);
    if (wanted.is_relative()) wanted = dir / wanted;
    if (usable(wanted)) {
#if defined(REX_PLATFORM_IOS) && REX_PLATFORM_IOS
      // The loader resolves relative names against the .app bundle; on iOS the
      // compressed pack lives in Documents, so pass an absolute path there.
      rex::cvar::SetFlagByName("ds2_shader_library", wanted.string());
#else
      rex::cvar::SetFlagByName("ds2_shader_library", name);
#endif
      return;
    }
    std::filesystem::path best;
    int best_version = -1;
    for (auto it = std::filesystem::directory_iterator(dir, ec); !ec && it != std::filesystem::directory_iterator();
         it.increment(ec)) {
      const std::string file = it->path().filename().string();
      int version = -1;
      if (file == "ds2_shaders.spvlib") {
        version = 1 << 30;
      } else if (file.rfind("ds2_shaders_v", 0) == 0 && file.size() > 20 &&
                 file.compare(file.size() - 7, 7, ".spvlib") == 0) {
        const std::string digits = file.substr(13, file.size() - 20);
        if (!digits.empty() && digits.find_first_not_of("0123456789") == std::string::npos && digits.size() < 9)
          version = std::stoi(digits);
      }
      if (version > best_version && usable(it->path())) {
        best_version = version;
        best = it->path();
      }
    }
    if (best.empty()) {
      REXLOG_WARN("ds2: shader pack {} not found next to the NRO (no ds2_shaders*.spvlib + .idx either)", name);
      return;
    }
#if defined(REX_PLATFORM_IOS) && REX_PLATFORM_IOS
    const std::string found = best.string();
#else
    const std::string found = best.filename().string();
#endif
    rex::cvar::SetFlagByName("ds2_shader_library", found);
    REXLOG_INFO("ds2: shader pack {} not found, using {}", name, found);
  }

  static void SetIfDefault(const char* name, const char* value) {
    if (rex::cvar::GetFlagInfo(name) == nullptr || rex::cvar::HasNonDefaultValue(name)) return;
    if (rex::cvar::SetFlagByName(name, value)) REXLOG_INFO("ds2: default {} = {}", name, value);
  }

  // The SDK starts game:\default.xex and checks it on the host file system before the (case-insensitive) VFS
  // sees it (sdk/src/ui/rex_app.cpp). The DS2 disc has it as Default.xex, so on a case-sensitive host
  // (Linux, Switch SD card) the lowercase name is not found.
  void OnLoadXexImage(std::string& xex_image) override { xex_image = "game:\\Default.xex"; }

  // Automated tests: scripted virtual pad (off unless --ds2_input_script is given), see ds2_input_script.h.
  void OnPreSetup(rex::RuntimeConfig& config) override { ds2::input_script::Wrap(config); }

  // DLC packages (--ds2_install_dlc), installed once the XEX is loaded so the title id is known.
  void OnPostSetup() override { ds2::input_script::ApplyTimeScalar(); }

  // v101: the packages are unpacked on a worker thread while the UI thread draws a progress bar (on the first boot
  // this takes minutes and the screen used to stay black); the game is launched when it is done.
  // v107: first the start screen with the internal resolution (720p / 540p).
  void LaunchModule() override {
    auto post = [this](std::function<void()> f) { app_context().CallInUIThreadDeferred(std::move(f)); };
    ds2::dlc::ChooseResolution(imgui_drawer(), post, [this, post]() {
      ds2::dlc::InstallWithProgress(runtime(), imgui_drawer(), post, [this]() { rex::ReXApp::LaunchModule(); });
    });
  }

  // Diagnostic frame capture (off unless --ds2_capture_dir is given), see ds2_capture.h.
  void OnPostLaunchModule(rex::system::XThread* thread) override {
    (void)thread;
    ds2::capture::Start(runtime());
    ds2::watchdog::Start(runtime());
    ds2::pgo::Start();  // v108: DS2_PGO=GEN builds only
    ds2::mode_overlay::Start(imgui_drawer(), [this](std::function<void()> f) {
      app_context().CallInUIThreadDeferred(std::move(f));
    });
  }
  void OnShutdown() override {
    ds2::pgo::Stop();
    ds2::mode_overlay::Stop();
    ds2::watchdog::Stop();
    ds2::capture::Stop();
  }
};
