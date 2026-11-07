// ds2 - installs Xbox 360 DLC packages (STFS: PIRS/LIVE/CON files) for the game at startup.
//
// DS2 SotFS refuses to leave the title screen without "DARK SOULS II Compatibility Pack 4" (content type 0x2). The
// SDK's content manager enumerates DLC as extracted folders under <user_data_root>/0000000000000000/<title>/00000002/
// and already has ContentManager::InstallContent (STFS -> folder + .header with the license mask), but nothing calls
// it. --ds2_install_dlc=<package>[;<package>...] calls it once per package that is not installed yet.

#pragma once

#include <functional>

namespace rex {
class Runtime;
namespace ui {
class ImGuiDrawer;
}
}

namespace ds2::dlc {

void InstallConfigured(rex::Runtime* runtime);

// v101: InstallConfigured on a worker thread with a progress bar in the centre of the screen (ImGui), then `then`
// on the UI thread. Called from the UI thread.
void InstallWithProgress(rex::Runtime* runtime, rex::ui::ImGuiDrawer* drawer,
                         std::function<void(std::function<void()>)> post_to_ui, std::function<void()> then);

// v107: start screen with two buttons (720p / 540p internal resolution), then `then` on the UI thread. Without
// ds2_resolution_prompt, `then` at once (ds2_render_scale from ds2.toml). Called from the UI thread.
void ChooseResolution(rex::ui::ImGuiDrawer* drawer, std::function<void(std::function<void()>)> post_to_ui,
                      std::function<void()> then);

}  // namespace ds2::dlc
