#include "ds2_mode_overlay.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>

REXCVAR_DEFINE_INT32(edram_diagnostic_mode, 0, "DS2",
                     "Switch DIAGNOSTIC: GPU work left out for the whole session, by mode number as shown on screen "
                     "(MODE 0 = normal rendering; see docs/handoff/13-switch-diag-modes.md). An unknown number "
                     "is reported and ignored")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#if defined(__SWITCH__)
extern "C" {
int RexSwitchPerfModeCount(void);
const char* RexSwitchPerfModeName(int index);
unsigned RexSwitchPerfModeMask(int index);
int RexSwitchPerfCurrentMode(void);
int RexSwitchPerfFixedMode(void);
bool RexSwitchPerfLapActive(void);
unsigned long long RexSwitchPerfModeChanges(void);
double RexSwitchPerfFps(void);
bool RexSwitchPerfSetFixedMode(int index);
}
#endif

namespace ds2::mode_overlay {
namespace {

// Short English labels by what the mode leaves out (ImGui's default font has no Cyrillic). Keyed by the skip
// mask, so the numbers always come from the SDK table.
const char* LabelForMask(unsigned mask) {
  switch (mask) {
    case 0: return "normal rendering";
    case 1: return "no draws";
    case 2: return "no RT transfers (all)";
    case 4: return "no resolves";
    case 8: return "no texture loads";
    case 16: return "no present effect";
    case 32: return "no tiled resolve dispatch";
    case 64: return "no stencil-bit transfer passes";
    case 128: return "no depth/stencil transfers";
    case 256: return "no color transfers";
    case 63: return "no GPU work";
    default: return "?";
  }
}

std::atomic<int> g_invalid_value{0};
std::atomic<bool> g_invalid{false};

#if defined(__SWITCH__)
class ModeDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit ModeDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    const int mode = RexSwitchPerfCurrentMode();
    const bool lap = RexSwitchPerfLapActive();
    const double fps = RexSwitchPerfFps();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 18.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.65f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
                                   ImGuiWindowFlags_NoFocusOnAppearing;
    if (ImGui::Begin("##ds2_mode", nullptr, flags)) {
      ImGui::SetWindowFontScale(3.0f);
      ImGui::TextColored(mode == 0 ? ImVec4(0.6f, 1.0f, 0.6f, 1.0f) : ImVec4(1.0f, 0.85f, 0.2f, 1.0f),
                         "MODE %d", mode);
      ImGui::SameLine();
      ImGui::Text("  %.1f FPS", fps);
      ImGui::SetWindowFontScale(1.6f);
      ImGui::Text("%s%s", LabelForMask(RexSwitchPerfModeMask(mode)),
                  lap ? "   [A/B lap, 10 s per mode]"
                      : (RexSwitchPerfFixedMode() != 0 ? "   [fixed by ds2.toml]" : ""));
      if (g_invalid.load()) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                           "edram_diagnostic_mode = %d is not a mode (0..%d): MODE 0 used",
                           g_invalid_value.load(), RexSwitchPerfModeCount() - 1);
      }
    }
    ImGui::End();
  }
};

rex::ui::ImGuiDrawer* g_drawer = nullptr;
std::function<void(std::function<void()>)> g_post;
std::thread g_thread;
std::atomic<bool> g_run{false};
std::mutex g_dialog_mutex;  // g_dialog is created and destroyed on the UI thread only
std::unique_ptr<ModeDialog> g_dialog;
bool g_shown = false;  // poll thread's view

void SetShown(bool show) {
  if (show == g_shown || !g_drawer || !g_post) return;
  g_shown = show;
  rex::ui::ImGuiDrawer* drawer = g_drawer;
  g_post([drawer, show]() {
    std::lock_guard<std::mutex> lock(g_dialog_mutex);
    if (show && !g_dialog) {
      g_dialog = std::make_unique<ModeDialog>(drawer);
    } else if (!show) {
      g_dialog.reset();
    }
  });
}

void Poll() {
  using clock = std::chrono::steady_clock;
  unsigned long long last_changes = RexSwitchPerfModeChanges();
  int last_mode = RexSwitchPerfCurrentMode();
  const auto start = clock::now();
  auto last_change_at = start;
  while (g_run.load()) {
    const auto now = clock::now();
    const unsigned long long changes = RexSwitchPerfModeChanges();
    const int mode = RexSwitchPerfCurrentMode();
    if (changes != last_changes || mode != last_mode) {
      REXLOG_INFO("[ds2] MODE {} ({}){}", mode, LabelForMask(RexSwitchPerfModeMask(mode)),
                  RexSwitchPerfLapActive() ? " - A/B lap" : "");
      last_changes = changes;
      last_mode = mode;
      last_change_at = now;
    }
    const bool recent_change = now - last_change_at < std::chrono::seconds(6);
    const bool invalid_banner = g_invalid.load() && now - start < std::chrono::seconds(60);
    // v104: no banner for MODE 0 (normal rendering) at startup or when a mode returns to 0.
    SetShown(RexSwitchPerfLapActive() || mode != 0 || invalid_banner || (recent_change && mode != 0));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
}
#endif

}  // namespace

void ApplyConfig() {
  const int requested = REXCVAR_GET(edram_diagnostic_mode);
#if defined(__SWITCH__)
  const int count = RexSwitchPerfModeCount();
  if (!RexSwitchPerfSetFixedMode(requested)) {
    g_invalid_value.store(requested);
    g_invalid.store(true);
    REXLOG_ERROR("[ds2] edram_diagnostic_mode = {} is not a mode (valid: 0..{}); MODE 0 (normal rendering) stays",
                 requested, count - 1);
    return;
  }
  for (int i = 0; i < count; ++i) {
    REXLOG_INFO("[ds2] diagnostic MODE {} = {} (mask {})", i, LabelForMask(RexSwitchPerfModeMask(i)),
                RexSwitchPerfModeMask(i));
  }
  REXLOG_INFO("[ds2] edram_diagnostic_mode = {}: MODE {} ({}) from the start", requested, requested,
              LabelForMask(RexSwitchPerfModeMask(requested)));
#else
  if (requested != 0) {
    REXLOG_WARN("[ds2] edram_diagnostic_mode = {} ignored: the diagnostic modes exist only on the Switch", requested);
  }
#endif
}

void Start(rex::ui::ImGuiDrawer* drawer, std::function<void(std::function<void()>)> post_to_ui) {
#if defined(__SWITCH__)
  if (!drawer || g_run.exchange(true)) return;
  g_drawer = drawer;
  g_post = std::move(post_to_ui);
  g_thread = std::thread(Poll);
#else
  (void)drawer;
  (void)post_to_ui;
#endif
}

void Stop() {
#if defined(__SWITCH__)
  if (!g_run.exchange(false)) return;
  if (g_thread.joinable()) g_thread.join();
  {
    // Before the ImGui drawer goes away (OnShutdown runs ahead of the app's teardown).
    std::lock_guard<std::mutex> lock(g_dialog_mutex);
    g_dialog.reset();
  }
  g_drawer = nullptr;
  g_post = nullptr;
#endif
}

}  // namespace ds2::mode_overlay
