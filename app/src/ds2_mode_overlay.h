// ds2 - on-screen label of the GPU diagnostic mode (Switch). The modes are the A/B lap of the SDK profiler
// (sdk/src/ui/switch_perf.cpp, table kModes): MODE 0 = normal rendering, the others leave out one kind of GPU
// work. L+R+ZL runs the lap (10 s per mode); edram_diagnostic_mode in ds2.toml fixes one mode for the session.
#pragma once

#include <functional>

namespace rex::ui {
class ImGuiDrawer;
}

namespace ds2::mode_overlay {
// Reads edram_diagnostic_mode and applies it (before the game starts). An invalid number is reported in the log
// and on screen and MODE 0 stays in effect.
void ApplyConfig();
// Shows the label whenever a lap runs, a non-zero mode is in effect, the setting was invalid, or the mode just
// changed. The ImGui window exists only while shown: a registered dialog changes how frames are presented.
// post_to_ui runs a function on the UI thread (ImGui dialogs are created and destroyed there).
void Start(rex::ui::ImGuiDrawer* drawer, std::function<void(std::function<void()>)> post_to_ui);
void Stop();
}  // namespace ds2::mode_overlay
