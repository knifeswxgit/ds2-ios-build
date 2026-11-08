// ds2 - DLC package installation, see ds2_dlc.h.

#include "ds2_dlc.h"

#include <rex/platform.h>
#if REX_PLATFORM_IOS
#include "ds2_ios/ios_paths.h"
#endif

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xam/install_progress.h>
#include <rex/system/xcontent.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/souls_style.h>

#include <imgui.h>
#include <imgui_internal.h>  // ImRect

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

REXCVAR_DEFINE_STRING(ds2_install_dlc, "", "DS2",
                      "STFS content packages to install at startup if missing, separated by ';' "
                      "(e.g. Compatibility Pack 4 or Calibration -GAME_0000). Relative paths are taken from the executable folder. "
                      "Empty: every file of <executable folder>/dlc/ is installed.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(ds2_resolution_prompt, true, "DS2",
                    "v107: ask for the internal resolution (720p / 540p) on the start screen; off = ds2_render_scale");
REXCVAR_DEFINE_STRING(ds2_calibration_file, "", "DS2",
                      "RC92: the game's online calibration (the regulation the 360 downloads into the save, file "
                      "USER_DATA15 of the -GAME_0000 save, e.g. Calibration 1.15 of the release). At start it is copied "
                      "into every profile's -GAME_0000 save whose USER_DATA15 differs; the old one is kept once as "
                      "USER_DATA15.before_calibration. Relative to the executable folder. Empty: off.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace ds2::dlc {

namespace {

bool ReadAll(const std::filesystem::path& p, std::vector<char>& out) {
  FILE* f = std::fopen(rex::path_to_utf8(p).c_str(), "rb");
  if (!f) return false;
  out.clear();
  char buf[65536];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
  std::fclose(f);
  return true;
}

bool WriteAll(const std::filesystem::path& p, const std::vector<char>& data) {
  const auto tmp = std::filesystem::path(rex::path_to_utf8(p) + ".tmp");
  FILE* f = std::fopen(rex::path_to_utf8(tmp).c_str(), "wb");
  if (!f) return false;
  const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
  if (std::fclose(f) != 0 || !ok) return false;
  std::error_code ec;
  std::filesystem::remove(p, ec);
  std::filesystem::rename(tmp, p, ec);
  return !ec;
}

// The calibration is a zlib-compressed regulation BND4 behind two big-endian sizes (compressed, uncompressed).
void ImportCalibration(const std::filesystem::path& exe_dir) {
  std::string name = REXCVAR_GET(ds2_calibration_file);
  if (name.empty()) return;
  std::filesystem::path src(name);
  if (src.is_relative()) src = exe_dir / src;
  std::vector<char> calib;
  if (!ReadAll(src, calib) || calib.size() < 16 || uint8_t(calib[8]) != 0x78) {
    REXLOG_WARN("[ds2 calibration] {}: missing or not a calibration file", rex::path_to_utf8(src));
    return;
  }
  std::error_code ec;
  int copied = 0, same = 0;
  for (const auto& profile : std::filesystem::directory_iterator(exe_dir / "user", ec)) {
    if (ec) break;
    const auto save = profile.path() / "465307E4" / "00000001" / "-GAME_0000";
    std::error_code ec2;
    if (!std::filesystem::is_directory(save, ec2)) continue;
    const auto target = save / "USER_DATA15";
    std::vector<char> current;
    if (ReadAll(target, current) && current == calib) {
      ++same;
      continue;
    }
    const auto backup = save / "USER_DATA15.before_calibration";
    if (!current.empty() && !std::filesystem::exists(backup, ec2)) WriteAll(backup, current);
    if (WriteAll(target, calib)) {
      ++copied;
      REXLOG_INFO("[ds2 calibration] {} -> {} ({} bytes; old one kept as {})", rex::path_to_utf8(src),
                  rex::path_to_utf8(target), calib.size(), rex::path_to_utf8(backup));
    } else {
      REXLOG_WARN("[ds2 calibration] could not write {}", rex::path_to_utf8(target));
    }
  }
  REXLOG_INFO("[ds2 calibration] {}: {} save(s) updated, {} already had it", rex::path_to_utf8(src), copied, same);
}

}  // namespace

std::atomic<uint32_t> g_package_index{0};
std::atomic<uint32_t> g_package_count{0};

void InstallConfigured(rex::Runtime* runtime) {
  using rex::X_RESULT;
  if (!runtime || !runtime->kernel_state()) {
    return;
  }
  const auto exe_dir = rex::filesystem::GetExecutableFolder();
  if (!exe_dir.empty()) ImportCalibration(exe_dir);
  std::string list = REXCVAR_GET(ds2_install_dlc);
#if defined(REX_PLATFORM_IOS) && REX_PLATFORM_IOS
  // On iOS the game data lives in Documents, not next to the executable (the
  // .app bundle is read-only), so the DLC packages are looked up there.
  const std::filesystem::path dlc_dir = ds2_ios::DocumentsPath() / "dlc";
#else
  const std::filesystem::path dlc_dir = exe_dir / "dlc";
#endif
  if (list.empty() && !dlc_dir.empty()) {
    // Portable layout (sdmc:/switch/ds2/dlc/ on the Switch): install whatever packages were copied there.
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dlc_dir, ec)) {
      if (ec) break;
      // Skip hidden files: macOS leaves "._<name>" AppleDouble copies next to every file it writes to a FAT card.
      if (e.path().filename().string().rfind('.', 0) == 0) continue;
      if (e.is_regular_file(ec)) list += (list.empty() ? "" : ";") + rex::path_to_utf8(e.path());
    }
  }
  if (list.empty()) {
    return;
  }
  auto* content = runtime->kernel_state()->content_manager();
  if (!content) {
    REXLOG_WARN("[ds2 dlc] no content manager");
    return;
  }
  {
    uint32_t n = 0;
    for (char c : list) n += c == ';';
    g_package_count.store(n + 1, std::memory_order_relaxed);
    g_package_index.store(0, std::memory_order_relaxed);
  }
  size_t start = 0;
  while (start <= list.size()) {
    size_t end = list.find(';', start);
    if (end == std::string::npos) end = list.size();
    const std::string item = list.substr(start, end - start);
    start = end + 1;
    if (item.empty()) continue;
    std::filesystem::path path(item);
    if (path.is_relative() && !exe_dir.empty()) path = exe_dir / path;
    const std::string name = path.filename().string();
    g_package_index.fetch_add(1, std::memory_order_relaxed);
    REXLOG_INFO("[ds2 dlc] installing {} (first boot only: the package is unpacked onto the SD card, which takes "
                "minutes on the console)", rex::path_to_utf8(path));
    rex::FlushLogging();
    const auto t0 = std::chrono::steady_clock::now();
    const auto result = content->InstallContent(path);
    if (result == X_ERROR_ALREADY_EXISTS) {
      REXLOG_INFO("[ds2 dlc] {} already installed", name);
    } else {
      REXLOG_INFO("[ds2 dlc] install {} -> {:08X} in {:.1f} s", rex::path_to_utf8(path), uint32_t(result),
                  std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    }
  }
}

namespace {

// v101: progress of the first-boot unpacking, centred on the screen. The bar shows only while a package is really
// being extracted (an installed package is skipped in about a second without one).
class InstallDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit InstallDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    namespace souls = rex::ui::souls;
    auto& progress = rex::system::xam::install_progress();
    const uint64_t total = progress.bytes_total.load(std::memory_order_relaxed);
    if (!total) return;
    const uint64_t done = std::min(progress.bytes_done.load(std::memory_order_relaxed), total);
    const float fraction = float(double(done) / double(total));
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x * 0.6f, 0.0f), ImGuiCond_Always);
    souls::PushWindowStyle();
    const ImGuiWindowFlags flags = souls::kWindowFlags | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin("##ds2_install", nullptr, flags)) {
      souls::DrawFrame();
      // v109: Dark Souls look (rex/ui/souls_style.h).
      souls::CenteredTitle("Installing Game Data", 1.0f, souls::kParchment);
      char line[64];
      std::snprintf(line, sizeof(line), "Package %u of %u", g_package_index.load(std::memory_order_relaxed),
                    g_package_count.load(std::memory_order_relaxed));
      souls::CenteredTitle(line, 0.6f, souls::kAsh);
      souls::Divider();
      char label[32];
      std::snprintf(label, sizeof(label), "%.1f%%", double(fraction) * 100.0);
      ImGui::PushStyleColor(ImGuiCol_PlotHistogram, souls::kEmber);
      ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.10f, 0.08f, 0.06f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_Border, souls::kBronze);
      ImGui::PushStyleColor(ImGuiCol_Text, souls::kParchment);
      ImGui::SetWindowFontScale(2.0f);
      ImGui::ProgressBar(fraction, ImVec2(-1.0f, 0.0f), label);
      ImGui::SetWindowFontScale(1.0f);
      ImGui::PopStyleColor(4);
      std::snprintf(line, sizeof(line), "%.1f / %.1f MB", double(done) / 1048576.0, double(total) / 1048576.0);
      souls::CenteredTitle(line, 0.55f, souls::kAsh);
    }
    ImGui::End();
    souls::PopWindowStyle();
  }
};

std::unique_ptr<InstallDialog> g_dialog;
std::thread g_install_thread;

// v107: start screen with the internal resolution of the scene (ds2_render_scale): two buttons in the centre of the
// screen, driven with the controller (D-pad + A) or the touch screen.
// v116: the choices of the start screen, kept on the SD card for the next start.
struct StartPrefs {
  bool valid = false;
  double scale = 1.0;
  int fps = 60;
  int language = 0;  // 0 = not saved
  bool sharpen = true;
};
constexpr const char* kStartPrefsPath = "sdmc:/switch/ds2/start_screen.cfg";

StartPrefs LoadStartPrefs() {
  StartPrefs p;
  FILE* f = std::fopen(kStartPrefsPath, "r");
  if (!f) return p;
  char line[64];
  while (std::fgets(line, sizeof(line), f)) {
    double scale;
    int v;
    if (std::sscanf(line, "scale=%lf", &scale) == 1) {
      p.scale = scale < 1.0 ? 0.75 : 1.0;
      p.valid = true;
    } else if (std::sscanf(line, "fps=%d", &v) == 1) {
      p.fps = v == 30 ? 30 : 60;
    } else if (std::sscanf(line, "lang=%d", &v) == 1) {
      p.language = v;
    } else if (std::sscanf(line, "sharpen=%d", &v) == 1) {
      p.sharpen = v != 0;
    }
  }
  std::fclose(f);
  return p;
}

void SaveStartPrefs(const StartPrefs& p) {
  FILE* f = std::fopen(kStartPrefsPath, "w");
  if (!f) return;
  std::fprintf(f, "scale=%.2f\nfps=%d\nlang=%d\nsharpen=%d\n", p.scale, p.fps, p.language, p.sharpen ? 1 : 0);
  std::fclose(f);
}

#ifndef DS2_VERSION_STRING
#define DS2_VERSION_STRING ""
#endif

// v109: the start screen (v107 resolution, v113 frame rate lock, v114 language; v116: remembered choices with a
// countdown, ember background, version, button glyphs, sharpening for 540p).
class ResolutionDialog final : public rex::ui::ImGuiDialog {
 public:
  ResolutionDialog(rex::ui::ImGuiDrawer* drawer, std::function<void(const StartPrefs&)> on_choice)
      : ImGuiDialog(drawer), on_choice_(std::move(on_choice)) {
    const StartPrefs saved = LoadStartPrefs();
    REXLOG_INFO("ds2: start screen: remembered choice {} (scale {:.2f}, {} FPS lock, language {}, sharpening {})",
                saved.valid ? "found" : "none", saved.scale, saved.fps, saved.language, saved.sharpen);
    toml_language_ = TomlLanguage();
    language_ = (toml_language_ == 1 || OtherLanguageName(toml_language_)) ? toml_language_ : 12;
    if (saved.language == 1 || saved.language == 12 || OtherLanguageName(saved.language)) {
      language_ = saved.language;
      if (OtherLanguageName(saved.language)) toml_language_ = saved.language;
    }
    fps_ = saved.fps;
    sharpen_ = saved.sharpen;
    remembered_ = saved.valid;
    scale_ = saved.valid ? saved.scale : 1.0;
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
    namespace souls = rex::ui::souls;
    if (chosen_) return;
    time_ += io.DeltaTime;
    DrawBackground(io);
    // Countdown: any button or touch stops it.
    // v118: only a new press stops it, after a short grace: the A that launched the port can still be held
    // when the screen appears (v117 stopped the countdown in its first frame), and stick noise is ignored.
    if (counting_ && time_ > 0.6f) {
      for (int k = ImGuiKey_GamepadStart; k <= ImGuiKey_GamepadR3; ++k) {
        if (ImGui::IsKeyPressed(ImGuiKey(k), false)) counting_ = false;
      }
      for (int k = ImGuiKey_GamepadLStickLeft; k <= ImGuiKey_GamepadRStickDown; ++k) {
        if (ImGui::IsKeyPressed(ImGuiKey(k), false) && ImGui::GetKeyData(ImGuiKey(k))->AnalogValue > 0.5f) {
          counting_ = false;
        }
      }
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) counting_ = false;
    }
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    if (first_frame_) ImGui::SetNextWindowFocus();
    souls::PushWindowStyle();
    double choice = 0.0;
    if (ImGui::Begin("##ds2_resolution", nullptr, souls::kWindowFlags | ImGuiWindowFlags_AlwaysAutoResize)) {
      souls::DrawFrame();
      // Language at the top left, on the title's row.
      const float title_y = ImGui::GetCursorPosY();
      {
        const ImVec2 lang(io.DisplaySize.x * 0.06f, io.DisplaySize.y * 0.052f);
        if (Toggle("##rus", "RUS", language_ == 12, lang, 0.6f)) language_ = 12;
        ImGui::SameLine(0.0f, 8.0f);
        if (Toggle("##eng", "ENG", language_ == 1, lang, 0.6f)) language_ = 1;
        if (const char* other = OtherLanguageName(toml_language_)) {
          ImGui::SameLine(0.0f, 8.0f);
          if (Toggle("##toml_lang", other, language_ == toml_language_, lang, 0.6f)) language_ = toml_language_;
        }
      }
      ImGui::SetCursorPosY(title_y);
      souls::CenteredTitle("Choose Resolution", 1.0f, souls::kParchment);
      souls::Divider();
      // The two resolutions; the remembered one has the focus.
      const ImVec2 size(io.DisplaySize.x * 0.24f, io.DisplaySize.y * 0.27f);
      const float gap = 24.0f;
      ImGui::PushStyleColor(ImGuiCol_Border, souls::kBronze);
      ImGui::PushStyleColor(ImGuiCol_Text, souls::kParchment);
      ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.5f);
      if (first_frame_ && scale_ >= 1.0) ImGui::SetKeyboardFocusHere();
      if (Choice("##720", "720p", "Native, sharp", size, false)) choice = 1.0;
      ImRect card720(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
      if (scale_ >= 1.0) ImGui::SetItemDefaultFocus();
      ImGui::SameLine(0.0f, gap);
      if (first_frame_ && scale_ < 1.0) ImGui::SetKeyboardFocusHere();
      if (Choice("##540", "540p", "More FPS, softer", size, true)) choice = 0.75;
      ImRect card540(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
      if (scale_ < 1.0) ImGui::SetItemDefaultFocus();
      ImGui::PopStyleVar();
      ImGui::PopStyleColor(2);
      const ImVec2 after_cards = ImGui::GetCursorScreenPos();
      // v117: sharpening of the 540p upscale, inside the 540p card (it belongs to that mode).
      const ImVec2 small(io.DisplaySize.x * 0.075f, io.DisplaySize.y * 0.065f);
      {
        const float pad = 18.0f;
        const ImVec2 bar(card540.GetWidth() - 2.0f * pad, card540.GetHeight() * 0.2f);
        ImGui::SetCursorScreenPos(ImVec2(card540.Min.x + pad, card540.Max.y - bar.y - pad));
        if (Toggle("##sharpen", sharpen_ ? "Sharpening: On" : "Sharpening: Off", sharpen_, bar, 0.55f)) {
          sharpen_ = !sharpen_;
          counting_ = false;
        }
      }
      // v117: the countdown shown on the remembered card: an ember bar draining along its top edge.
      if (counting_) {
        const ImRect& card = scale_ >= 1.0 ? card720 : card540;
        const float frac = std::clamp((kCountdown - time_) / kCountdown, 0.0f, 1.0f);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float y = card.Min.y + 5.0f;
        dl->AddRectFilled(ImVec2(card.Min.x + 6.0f, y), ImVec2(card.Max.x - 6.0f, y + 4.0f),
                          souls::Col(souls::kBronzeDim));
        dl->AddRectFilled(ImVec2(card.Min.x + 6.0f, y),
                          ImVec2(card.Min.x + 6.0f + (card.GetWidth() - 12.0f) * frac, y + 4.0f),
                          souls::Col(souls::kEmber));
      }
      ImGui::SetCursorScreenPos(after_cards);
      // Frame rate lock at the right.
      ImGui::Dummy(ImVec2(0.0f, 6.0f));
      {
        const float row_w = small.x * 2.0f + 12.0f;
        const float row_y = ImGui::GetCursorPosY();
        const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
        const float x30 = right - row_w;
        souls::PushTitleFont(0.55f);
        const char* label = "Frame Rate Lock";
        const ImVec2 label_size = ImGui::CalcTextSize(label);
        ImGui::SetCursorPos(ImVec2(x30 - label_size.x - 16.0f, row_y + (small.y - label_size.y) * 0.5f));
        ImGui::PushStyleColor(ImGuiCol_Text, souls::kAsh);
        ImGui::TextUnformatted(label);
        ImGui::PopStyleColor();
        souls::PopTitleFont();
        ImGui::SetCursorPos(ImVec2(x30, row_y));
        if (Toggle("##fps30", "30", fps_ == 30, small)) fps_ = 30;
        ImGui::SetCursorPos(ImVec2(x30 + small.x + 12.0f, row_y));
        if (Toggle("##fps60", "60", fps_ == 60, small)) fps_ = 60;
        ImGui::SetCursorPosY(row_y + small.y);
      }
      ImGui::Dummy(ImVec2(0.0f, 6.0f));
      // Countdown with the remembered choice, or the button hints.
      const float left = counting_ ? kCountdown - time_ : 0.0f;
      if (counting_ && left <= 0.0f) choice = scale_;
      if (counting_) {
        char text[64];
        std::snprintf(text, sizeof(text), "Auto start in %d s  -  any button to stay", int(left) + 1);
        souls::CenteredTitle(text, 0.75f, souls::kEmber);
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
      }
      DrawHints();  // v121: also under the countdown, so the touch hint is always visible
      first_frame_ = false;
    }
    ImGui::End();
    souls::PopWindowStyle();
    if (choice > 0.0) {
      chosen_ = true;
      StartPrefs p;
      p.valid = true;
      p.scale = choice;
      p.fps = fps_;
      p.language = language_;
      p.sharpen = sharpen_;
      SaveStartPrefs(p);
      on_choice_(p);
    }
  }

 private:
  static constexpr float kCountdown = 20.0f;  // v121: 20 s (was 10)

  // Dark screen with embers rising from a glow at the bottom, and the port version in the corner.
  void DrawBackground(ImGuiIO& io) {
    namespace souls = rex::ui::souls;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ImVec2 d = io.DisplaySize;
    dl->AddRectFilled(ImVec2(0, 0), d, IM_COL32(0, 0, 0, 255));  // v117: black
    const ImU32 clear = souls::Col(souls::kEmber, 0.0f);
    const ImU32 glow = souls::Col(souls::kEmber, 0.035f + 0.01f * std::sin(time_ * 1.3f));
    dl->AddRectFilledMultiColor(ImVec2(0, d.y * 0.75f), d, clear, clear, glow, glow);
    for (int i = 0; i < kEmbers; ++i) {
      // Deterministic per ember: start, speed, sway and size from its index.
      const float h1 = Hash(i * 3 + 1), h2 = Hash(i * 3 + 2), h3 = Hash(i * 3 + 3);
      const float speed = 18.0f + 42.0f * h2;
      const float travel = d.y * 0.85f;
      const float period = travel / speed;
      const float t = std::fmod(time_ + h1 * period, period) / period;  // 0 at the bottom, 1 at the top
      const float x = d.x * (0.1f + 0.8f * h1) + std::sin(time_ * (0.6f + h3) + h2 * 6.28f) * (12.0f + 20.0f * h3);
      const float y = d.y + 10.0f - t * travel;
      const float fade = std::sin(t * 3.14159f) * (0.55f + 0.45f * std::sin(time_ * (3.0f + 4.0f * h3) + h1 * 9.0f));
      const float r = 1.2f + 2.2f * h3;
      dl->AddCircleFilled(ImVec2(x, y), r * 3.5f, souls::Col(souls::kEmber, 0.07f * fade));
      dl->AddCircleFilled(ImVec2(x, y), r, souls::Col(ImVec4(1.0f, 0.72f, 0.35f, 1.0f), 0.9f * fade));
    }
    if (DS2_VERSION_STRING[0]) {
      souls::PushTitleFont(0.57f);  // v118: larger
      const ImVec2 t = ImGui::CalcTextSize(DS2_VERSION_STRING);
      dl->AddText(ImVec2(d.x - t.x - 18.0f, d.y - t.y - 12.0f), souls::Col(souls::kAsh, 0.8f), DS2_VERSION_STRING);
      souls::PopTitleFont();
    }
  }
  static float Hash(int n) {
    uint32_t x = uint32_t(n) * 0x9E3779B1u;
    x ^= x >> 15;
    x *= 0x85EBCA77u;
    x ^= x >> 13;
    return float(x & 0xFFFF) / 65535.0f;
  }
  static constexpr int kEmbers = 70;

  // "[D-pad] Select    (A) Start   or   [finger] Tap" with drawn glyphs (v121: the touch hint).
  void DrawHints() {
    namespace souls = rex::ui::souls;
    souls::PushTitleFont(0.62f);
    const char* select = "Select";
    const char* start = "Start";
    const char* or_text = "or";
    const char* tap = "Tap";
    const ImVec2 ts = ImGui::CalcTextSize(select), tt = ImGui::CalcTextSize(start);
    const ImVec2 to = ImGui::CalcTextSize(or_text), tp = ImGui::CalcTextSize(tap);
    const float icon = ts.y * 1.1f, space = 10.0f, group_gap = 40.0f, or_gap = 28.0f;
    const float total = icon + space + ts.x + group_gap + icon + space + tt.x + or_gap + to.x + or_gap +
                        icon * 1.2f + space + tp.x;
    const float x0 = ImGui::GetCursorScreenPos().x + (ImGui::GetContentRegionAvail().x - total) * 0.5f;
    const float y0 = ImGui::GetCursorScreenPos().y;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 ink = souls::Col(souls::kAsh);
    // D-pad: a cross.
    {
      const float c = icon * 0.5f, arm = icon * 0.18f;
      const ImVec2 m(x0 + c, y0 + ts.y * 0.5f);
      dl->AddRectFilled(ImVec2(m.x - c, m.y - arm), ImVec2(m.x + c, m.y + arm), ink, 2.0f);
      dl->AddRectFilled(ImVec2(m.x - arm, m.y - c), ImVec2(m.x + arm, m.y + c), ink, 2.0f);
    }
    dl->AddText(ImVec2(x0 + icon + space, y0), ink, select);
    // A button: a circle with the letter.
    const float ax = x0 + icon + space + ts.x + group_gap;
    {
      const ImVec2 m(ax + icon * 0.5f, y0 + ts.y * 0.5f);
      dl->AddCircle(m, icon * 0.5f, souls::Col(souls::kEmber), 24, 1.6f);
      const ImVec2 la = ImGui::CalcTextSize("A");
      dl->AddText(ImVec2(m.x - la.x * 0.5f, m.y - la.y * 0.5f), souls::Col(souls::kParchment), "A");
    }
    dl->AddText(ImVec2(ax + icon + space, y0), ink, start);
    // "or", dimmer.
    const float ox = ax + icon + space + tt.x + or_gap;
    dl->AddText(ImVec2(ox, y0), souls::Col(souls::kAsh, 0.6f), or_text);
    // Hand pointing with the index finger (a silhouette), turned 90 degrees left so the finger points left:
    // index finger out, the other three fingers curled as knuckles, thumb folded; two tap arcs at the fingertip.
    // Drawn upright in local coordinates and rotated: (dx, dy) -> (dy, -dx) around the icon centre.
    const float fx = ox + to.x + or_gap;
    const float hand_w = icon * 1.2f;  // the upright hand's height becomes the width
    {
      const float h = icon * 1.2f;
      const ImVec2 c(fx + hand_w * 0.5f, y0 + ts.y * 0.5f);
      // Upright layout, relative to the centre (y grows down).
      const float top = -h * 0.5f;
      const float fw = icon * 0.20f;
      const float fl = -icon * 0.26f + icon * 0.0f;
      auto rot = [&](float dx, float dy) { return ImVec2(c.x + dy, c.y - dx); };
      auto rect = [&](float x0, float y0r, float x1, float y1, float rounding, ImDrawFlags flags) {
        const ImVec2 p0 = rot(x0, y0r), p1 = rot(x1, y1);
        dl->AddRectFilled(ImVec2(std::min(p0.x, p1.x), std::min(p0.y, p1.y)),
                          ImVec2(std::max(p0.x, p1.x), std::max(p0.y, p1.y)), ink, rounding, flags);
      };
      const float tip_y = top + h * 0.06f;
      // index finger (its rounded top end becomes the left end)
      rect(fl, tip_y, fl + fw, top + h * 0.60f, fw * 0.5f, ImDrawFlags_RoundCornersLeft);
      // palm
      rect(fl, top + h * 0.50f, icon * 0.36f, top + h, icon * 0.12f, 0);
      // curled fingers
      const float kr = icon * 0.085f;
      for (int k = 0; k < 3; ++k) {
        dl->AddCircleFilled(rot(fl + fw + kr * (1.1f + 2.0f * float(k)), top + h * 0.50f), kr, ink, 12);
      }
      // thumb
      rect(fl - icon * 0.14f, top + h * 0.62f, fl + fw * 0.6f, top + h * 0.80f, icon * 0.08f, 0);
      // tap arcs around the fingertip (angles turned by -90 degrees with the hand)
      const ImVec2 tip = rot(fl + fw * 0.5f, tip_y);
      const ImU32 ember = souls::Col(souls::kEmber);
      const float a0 = 3.14159f * 0.65f, a1 = 3.14159f * 1.35f;
      dl->PathArcTo(tip, fw * 1.4f, a0, a1, 10);
      dl->PathStroke(ember, 0, 1.4f);
      dl->PathArcTo(tip, fw * 2.3f, a0, a1, 12);
      dl->PathStroke(ember, 0, 1.4f);
    }
    dl->AddText(ImVec2(fx + hand_w + space, y0), ink, tap);
    souls::PopTitleFont();
    ImGui::Dummy(ImVec2(total, ts.y));
  }

  // A small toggle; the selected one is lit in ember, the other dimmed.
  static bool Toggle(const char* id, const char* text, bool selected, ImVec2 size, float text_scale = 0.8f) {
    namespace souls = rex::ui::souls;
    ImGui::PushStyleColor(ImGuiCol_Button, selected ? ImVec4(0.42f, 0.23f, 0.07f, 0.95f)
                                                    : ImVec4(0.10f, 0.08f, 0.06f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_Border, selected ? souls::kEmber : souls::kBronzeDim);
    const bool pressed = ImGui::Button(id, size);
    ImGui::PopStyleColor(2);
    const bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    souls::PushTitleFont(text_scale);
    const ImVec2 t = ImGui::CalcTextSize(text);
    const ImVec4 color = selected ? souls::kParchment : souls::kAsh;
    dl->AddText(ImVec2((a.x + b.x - t.x) * 0.5f, (a.y + b.y - t.y) * 0.5f),
                souls::Col(color, selected || hot ? 1.0f : 0.6f), text);
    souls::PopTitleFont();
    if (selected) souls::Diamond(dl, ImVec2((a.x + b.x) * 0.5f, b.y), 4.0f, souls::Col(souls::kEmber));
    return pressed;
  }

  // A button with the resolution in the title font and a short line under it.
  static bool Choice(const char* id, const char* title, const char* note, ImVec2 size, bool overlap) {
    namespace souls = rex::ui::souls;
    if (overlap) ImGui::SetNextItemAllowOverlap();  // v117: the sharpening toggle sits inside the 540p card
    const bool pressed = ImGui::Button(id, size);
    const bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // v118: both cards breathe in ember while the screen waits for a choice; the focused one more strongly.
    // v121: much stronger (the v118 thin border was not visible on the console): the whole card is a full
    // ember gradient that swells and fades (~1.6 s cycle), with a thick border and a wide halo.
    {
      const float pulse = 0.5f + 0.5f * std::sin(float(ImGui::GetTime()) * 3.9f);
      const float strength = hot ? 0.45f + 0.55f * pulse : 0.15f + 0.85f * pulse;
      const float peak = hot ? 0.85f : 0.65f;
      const ImU32 fill_top = souls::Col(souls::kEmber, strength * peak * 0.15f);
      const ImU32 fill_bottom = souls::Col(souls::kEmber, strength * peak);
      dl->AddRectFilledMultiColor(a, b, fill_top, fill_top, fill_bottom, fill_bottom);
      for (int i = 8; i >= 1; --i) {
        const float g = float(i) * 3.0f;
        dl->AddRect(ImVec2(a.x - g, a.y - g), ImVec2(b.x + g, b.y + g),
                    souls::Col(souls::kEmber, strength * 0.55f / float(i)), 0.0f, 0, 3.0f);
      }
      dl->AddRect(a, b, souls::Col(souls::kEmber, 0.4f + 0.6f * strength), 0.0f, 0, hot ? 3.5f : 2.5f);
    }
    souls::PushTitleFont(1.3f);
    ImVec2 t = ImGui::CalcTextSize(title);
    const float cx = (a.x + b.x) * 0.5f;
    const float ty = a.y + (b.y - a.y) * 0.24f - t.y * 0.5f;
    dl->AddText(ImVec2(cx - t.x * 0.5f, ty), souls::Col(hot ? souls::kEmber : souls::kParchment), title);
    souls::PopTitleFont();
    souls::PushTitleFont(0.55f);
    const ImVec2 n = ImGui::CalcTextSize(note);
    dl->AddText(ImVec2(cx - n.x * 0.5f, a.y + (b.y - a.y) * 0.48f - n.y * 0.5f), souls::Col(souls::kAsh), note);
    souls::PopTitleFont();
    if (hot) {
      souls::Diamond(dl, ImVec2(a.x, (a.y + b.y) * 0.5f), 6.0f, souls::Col(souls::kEmber));
      souls::Diamond(dl, ImVec2(b.x, (a.y + b.y) * 0.5f), 6.0f, souls::Col(souls::kEmber));
    }
    return pressed;
  }

  // Xbox 360 language ids of the European disc other than English and Russian.
  static const char* OtherLanguageName(int id) {
    switch (id) {
      case 3: return "DEU";
      case 4: return "FRA";
      case 5: return "ESP";
      case 6: return "ITA";
      case 9: return "POR";
      case 11: return "POL";
      default: return nullptr;
    }
  }
  static int TomlLanguage() {
    try {
      return std::stoi(rex::cvar::GetFlagByName("user_language"));
    } catch (...) {
      return 12;
    }
  }

  std::function<void(const StartPrefs&)> on_choice_;
  int fps_ = 60;
  int toml_language_ = 12;
  int language_ = 12;
  bool sharpen_ = true;
  double scale_ = 1.0;   // remembered resolution (focus and countdown)
  bool remembered_ = false;
  bool counting_ = false;  // only with a remembered choice (StartCountdown)
  float time_ = 0.0f;
  bool chosen_ = false;
  bool first_frame_ = true;

 public:
  void StartCountdown() { counting_ = remembered_; }
};

std::unique_ptr<ResolutionDialog> g_resolution_dialog;

}  // namespace

void ChooseResolution(rex::ui::ImGuiDrawer* drawer, std::function<void(std::function<void()>)> post_to_ui,
                      std::function<void()> then) {
  if (!drawer || !post_to_ui || !REXCVAR_GET(ds2_resolution_prompt)) {
    then();
    return;
  }
  g_resolution_dialog = std::make_unique<ResolutionDialog>(drawer, [post_to_ui, then](const StartPrefs& p) {
    // From inside the dialog's OnDraw: the dialog is destroyed later, on the UI thread, before the game starts.
    post_to_ui([p, then]() {
      // v114: game language, read by the game through XamGetLanguage after it starts.
      rex::cvar::SetFlagByName("user_language", std::to_string(p.language));
      rex::cvar::SetFlagByName("ds2_render_scale", p.scale >= 1.0 ? "1.0" : "0.75");
      // v113: frame rate lock (ds2_fps_cap): 30, or 60. v116: sharpening of the 540p upscale.
      rex::cvar::SetFlagByName("ds2_fps_cap", p.fps == 30 ? "30" : "60");
      rex::cvar::SetFlagByName("ds2_sharpen", p.sharpen ? "true" : "false");
      REXLOG_INFO("ds2: start screen: {}, {} FPS lock, language {}, sharpening {}",
                  p.scale >= 1.0 ? "720p" : "540p", p.fps, p.language, p.sharpen ? "on" : "off");
      g_resolution_dialog.reset();
      then();
    });
  });
  g_resolution_dialog->StartCountdown();
}


void InstallWithProgress(rex::Runtime* runtime, rex::ui::ImGuiDrawer* drawer,
                         std::function<void(std::function<void()>)> post_to_ui, std::function<void()> then) {
  if (!drawer || !post_to_ui) {
    InstallConfigured(runtime);
    then();
    return;
  }
  g_dialog = std::make_unique<InstallDialog>(drawer);
  g_install_thread = std::thread([runtime, post_to_ui, then]() {
    InstallConfigured(runtime);
    post_to_ui([then]() {
      // The dialog must be gone before the game starts: while a dialog is registered the presenter paints
      // from the UI thread (see ReXApp::LaunchModule).
      g_dialog.reset();
      if (g_install_thread.joinable()) g_install_thread.join();
      then();
    });
  });
}

}  // namespace ds2::dlc
