#pragma once
/**
 * iOS input driver for the DS2 port.
 *
 * Presents ONE XInput gamepad to the recompiled game. Sources:
 *   - touch  : SDL3 SDL_EVENT_FINGER_* -> virtual sticks / buttons / triggers
 *   - gamepad: SDL3 SDL_EVENT_GAMEPAD_* (MFi / Bluetooth via SDL's iOS backend)
 *
 * Events are observed with SDL_AddEventWatch, so the SDK's own
 * SDLWindowedAppContext keeps consuming them - we never steal from its loop.
 * The state is a small mutex-protected snapshot; GetDeviceState is called from
 * the XAM thread and never touches the UI thread.
 *
 * Layout is in ios_port/TOUCH_CONTROLS.md; the kZones table below must stay in
 * sync with the visual overlay in ios_paths.mm.
 */

#include <atomic>
#include <cstdint>
#include <mutex>

#include <rex/input/input_driver.h>

namespace ds2_ios {

class IosTouchInputDriver final : public rex::input::InputDriver {
 public:
  IosTouchInputDriver(rex::ui::Window* window, size_t window_z_order);
  ~IosTouchInputDriver() override;

  X_STATUS Setup() override;
  void EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) override;
  X_RESULT GetDeviceState(rex::input::DeviceId id, X_INPUT_STATE* out_state) override;
  X_RESULT GetDeviceCapabilities(rex::input::DeviceId id, uint32_t flags,
                                 X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT SetDeviceVibration(rex::input::DeviceId id, X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetDeviceKeystroke(rex::input::DeviceId id, uint32_t flags,
                              X_INPUT_KEYSTROKE* out_keystroke) override;

  /// Drop every held button/stick (backgrounding, memory warning).
  void Reset();
  /// Exposed for the platform setup hook: install overlay + memory handler.
  static void InstallPlatformHooks();

  /// Called from the SDL event watch (a free function, so this stays public).
  void OnSdlEvent(void* event);

 private:
  struct Snapshot {
    uint16_t buttons = 0;
    uint8_t lt = 0, rt = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
    bool changed = false;
  };
  std::mutex mutex_;
  Snapshot state_;
  uint32_t packet_number_ = 0;
  bool is_active_ = true;
  uint16_t vibration_[2] = {0, 0};
  void* watch_ = nullptr;  // SDL_EventWatch handle is managed by SDL_RemoveEventWatch
};

}  // namespace ds2_ios
