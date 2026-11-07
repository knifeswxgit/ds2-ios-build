#pragma once
/** iOS platform helpers for the DS2 port. Implemented in ios_paths.mm (ObjC++). */

#include <filesystem>

namespace ds2_ios {

/// App's Documents directory. Game data (game_root/, user/, dlc/) lives here,
/// copied in through Files - it is NOT bundled into the .ipa.
std::filesystem::path DocumentsPath();

/// Path of ds2.app (ds2_ios.toml, ds2_shaders.spvz, ds2_shaders.spvz.idx).
std::filesystem::path BundlePath();

/// Register a callback for UIApplicationDidReceiveMemoryWarningNotification.
/// The callback should drop the texture cache and the SpvzReader LRU blocks.
void SetLowMemoryCallback(void (*callback)());
void InstallMemoryWarningHandler();

/// Add the semi-transparent touch control overlay to the key window.
/// Call once, from the main thread, after the SDL window is up.
void InstallTouchOverlay();

}  // namespace ds2_ios
