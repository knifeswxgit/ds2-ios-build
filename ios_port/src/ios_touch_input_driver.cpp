#include "ds2_ios/ios_touch_input_driver.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#endif

#include <rex/input/input.h>
#include <rex/logging.h>

#include "ds2_ios/ios_paths.h"
#include "ds2_ios/ios_log_overlay.h"
#include "ds2_ios/ios_controls_layout.h"

namespace ds2_ios {
namespace {

using rex::input::DeviceId;
constexpr DeviceId kDeviceId = static_cast<DeviceId>(0x4453324900000001ull);  // "DS2I"

// X_INPUT_GAMEPAD_BUTTON values from sdk/include/rex/input/input.h.
enum : uint16_t {
  kDUp = 0x0001, kDDown = 0x0002, kDLeft = 0x0004, kDRight = 0x0008,
  kStart = 0x0010, kBack = 0x0020, kL3 = 0x0040, kR3 = 0x0080,
  kLB = 0x0100, kRB = 0x0200, kGuide = 0x0400,
  kA = 0x1000, kB = 0x2000, kX = 0x4000, kY = 0x8000,
};

// Layout shared with the drawn overlay (see ds2_ios/ios_controls_layout.h).

// Maps a raw SDL finger coordinate (0..1 over the whole window) into the
// safe-area-relative coordinate space the zones live in. The notch/cutout,
// Home indicator and swipe zones are excluded this way, so drawn buttons
// and hit zones stay in sync and never sit under system UI.
void MapToSafeArea(const SDL_Event* e, float* out_x, float* out_y) {
  float bx0 = 0, by0 = 0, bx1 = 1, by1 = 1;
  GetTouchAreaBounds(&bx0, &by0, &bx1, &by1);
  if (bx1 <= bx0 || by1 <= by0) { *out_x = e->tfinger.x; *out_y = e->tfinger.y; return; }
  *out_x = (e->tfinger.x - bx0) / (bx1 - bx0);
  *out_y = (e->tfinger.y - by0) / (by1 - by0);
}

struct StickCapture {
  uint64_t finger = 0;
  float origin_x = 0, origin_y = 0;
  bool active = false;
};

// Finger ids currently holding the analog triggers (zones with button == 0).
uint64_t left_trig_finger = 0;
uint64_t right_trig_finger = 0;

constexpr float kStickRadius = 0.11f;  // normalized
constexpr float kDeadZone = 0.12f;

int16_t Axis(float v) {
  const float mag = std::fabs(v);
  if (mag < kDeadZone) return 0;
  const float scaled = (mag - kDeadZone) / (1.0f - kDeadZone) * (v < 0 ? -1.0f : 1.0f);
  return static_cast<int16_t>(std::clamp(scaled, -1.0f, 1.0f) * 32767.0f);
}

uint16_t MapGamepadButton(uint32_t b) {
  switch (b) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return kA;
    case SDL_GAMEPAD_BUTTON_EAST: return kB;
    case SDL_GAMEPAD_BUTTON_WEST: return kX;
    case SDL_GAMEPAD_BUTTON_NORTH: return kY;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return kLB;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return kRB;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return kL3;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return kR3;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return kDUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return kDDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return kDLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return kDRight;
    case SDL_GAMEPAD_BUTTON_START: return kStart;
    case SDL_GAMEPAD_BUTTON_BACK: return kBack;
    case SDL_GAMEPAD_BUTTON_GUIDE: return kGuide;
    default: return 0;
  }
}

bool SDLCALL EventWatch(void* userdata, SDL_Event* event) {
  static_cast<IosTouchInputDriver*>(userdata)->OnSdlEvent(event);
  return true;  // never consume: SDLWindowedAppContext still needs the event
}

}  // namespace

// OnSdlEvent is declared private but called from the free function above;
// friendship is granted through the static trampoline below.
void IosTouchInputDriver::OnSdlEvent(void* raw) {
  auto* e = static_cast<SDL_Event*>(raw);
  static StickCapture left_stick, right_stick;
  static std::map<uint64_t, uint16_t> touch_buttons;

  std::lock_guard<std::mutex> lock(mutex_);
  auto mark = [&] { state_.changed = true; };

  switch (e->type) {
    case SDL_EVENT_FINGER_DOWN: {
      float x, y;
      MapToSafeArea(e, &x, &y);
      const uint64_t id = static_cast<uint64_t>(e->tfinger.fingerID);
      // Buttons take priority: the right-stick area overlaps the face buttons,
      // and previously taps on A/B/X/Y were swallowed as stick captures.
      uint16_t mask = 0;
      bool in_zone = false;
      const int zone_count = GetTouchZoneCount();
      const TouchZone* zones = GetTouchZones();
      for (int i = 0; i < zone_count; ++i) {
        const TouchZone& z = zones[i];
        if (x >= z.x0 && x < z.x1 && y >= z.y0 && y < z.y1) {
          in_zone = true;
          if (z.button == 0) {
            // analog trigger zone: left half = LT, right half = RT
            if (x < 0.5f) { left_trig_finger = id; state_.lt = 255; }
            else { right_trig_finger = id; state_.rt = 255; }
          } else {
            mask = z.button;
            touch_buttons[id] = mask;
            state_.buttons |= mask;
          }
          mark();
          break;
        }
      }
      if (!in_zone) {
        if (x < 0.35f && y > 0.45f) {
          left_stick = {id, x, y, true};
        } else if (x > 0.65f && y > 0.45f) {
          right_stick = {id, x, y, true};
        }
      }
      break;
    }
    case SDL_EVENT_FINGER_MOTION: {
      const uint64_t id = static_cast<uint64_t>(e->tfinger.fingerID);
      float x, y;
      MapToSafeArea(e, &x, &y);
      auto apply = [&](StickCapture& s, int16_t& ox, int16_t& oy) {
        if (!s.active || s.finger != id) return;
        const float dx = (x - s.origin_x) / kStickRadius;
        const float dy = (y - s.origin_y) / kStickRadius;
        const float len = std::hypot(dx, dy);
        const float k = len > 1.0f ? 1.0f / len : 1.0f;
        ox = Axis(dx * k);
        oy = Axis(-dy * k);  // SDL y grows down, XInput LY grows up
        mark();
      };
      apply(left_stick, state_.lx, state_.ly);
      apply(right_stick, state_.rx, state_.ry);
      break;
    }
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED: {
      const uint64_t id = static_cast<uint64_t>(e->tfinger.fingerID);
      auto it = touch_buttons.find(id);
      if (it != touch_buttons.end()) {
        state_.buttons &= ~it->second;
        // Any other finger still holding the same bit keeps it down.
        for (const auto& kv : touch_buttons)
          if (kv.first != id) state_.buttons |= kv.second;
        touch_buttons.erase(it);
        mark();
      }
      if (left_stick.active && left_stick.finger == id) {
        left_stick.active = false; state_.lx = state_.ly = 0; mark();
      }
      if (right_stick.active && right_stick.finger == id) {
        right_stick.active = false; state_.rx = state_.ry = 0; mark();
      }
      // Release triggers when their finger leaves.
      if (left_trig_finger == id) { left_trig_finger = 0; state_.lt = 0; mark(); }
      if (right_trig_finger == id) { right_trig_finger = 0; state_.rt = 0; mark(); }
      break;
    }
    case SDL_EVENT_GAMEPAD_ADDED:
      SDL_OpenGamepad(e->gdevice.which);
      break;
    case SDL_EVENT_GAMEPAD_REMOVED: {
      SDL_Gamepad* gp = SDL_GetGamepadFromID(e->gdevice.which);
      if (gp) SDL_CloseGamepad(gp);
      Reset();
      break;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
      const uint16_t mask = MapGamepadButton(e->gbutton.button);
      if (!mask) break;
      if (e->gbutton.down) state_.buttons |= mask; else state_.buttons &= ~mask;
      mark();
      break;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
      const int16_t v = e->gaxis.value;
      switch (e->gaxis.axis) {
        case SDL_GAMEPAD_AXIS_LEFTX: state_.lx = v; break;
        case SDL_GAMEPAD_AXIS_LEFTY: state_.ly = static_cast<int16_t>(-v); break;
        case SDL_GAMEPAD_AXIS_RIGHTX: state_.rx = v; break;
        case SDL_GAMEPAD_AXIS_RIGHTY: state_.ry = static_cast<int16_t>(-v); break;
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
          state_.lt = static_cast<uint8_t>(std::max<int16_t>(0, v) >> 7); break;
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
          state_.rt = static_cast<uint8_t>(std::max<int16_t>(0, v) >> 7); break;
        default: break;
      }
      mark();
      break;
    }
    case SDL_EVENT_DID_ENTER_BACKGROUND:
      Reset();
      break;
    default:
      break;
  }
}

IosTouchInputDriver::IosTouchInputDriver(rex::ui::Window* window, size_t z)
    : rex::input::InputDriver(window, z) {}

IosTouchInputDriver::~IosTouchInputDriver() {
  if (watch_) {
    // SDL3 removes by filter+userdata.
    SDL_RemoveEventWatch(&EventWatch, this);
  }
}

X_STATUS IosTouchInputDriver::Setup() {
  // Bluetooth / MFi controllers: SDL3 routes GCController (MFi, PS, Xbox)
  // gamepads through the same SDL_EVENT_GAMEPAD_* events handled below.
  if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
    REXLOG_WARN("ds2-ios: SDL_INIT_GAMEPAD failed: {} (touch only)", SDL_GetError());
  }
  if (!SDL_AddEventWatch(&EventWatch, this)) {
    REXLOG_ERROR("ds2-ios: SDL_AddEventWatch failed: {}", SDL_GetError());
    return X_STATUS_UNSUCCESSFUL;
  }
  watch_ = this;
  InstallPlatformHooks();
  REXLOG_INFO("ds2-ios: touch + gamepad input driver ready");
  return X_STATUS_SUCCESS;
}

void IosTouchInputDriver::EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) {
  rex::input::DeviceInfo info;
  info.id = kDeviceId;
  info.ordinal = 0;  // assigned by InputSystem
  info.name = "DS2 Touch / MFi Gamepad";
  info.guid = "ds2-ios-touch";
  info.synthetic = false;
  out.push_back(std::move(info));
}

X_RESULT IosTouchInputDriver::GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) {
  if (id != kDeviceId) return X_ERROR_DEVICE_NOT_CONNECTED;
  std::lock_guard<std::mutex> lock(mutex_);
  const bool active = is_active();
  if (state_.changed) {
    ++packet_number_;
    state_.changed = false;
  }
  out_state->packet_number = packet_number_;
  if (!active) {
    std::memset(&out_state->gamepad, 0, sizeof(out_state->gamepad));
    return X_ERROR_SUCCESS;
  }
  out_state->gamepad.buttons = state_.buttons;
  out_state->gamepad.left_trigger = state_.lt;
  out_state->gamepad.right_trigger = state_.rt;
  out_state->gamepad.thumb_lx = state_.lx;
  out_state->gamepad.thumb_ly = state_.ly;
  out_state->gamepad.thumb_rx = state_.rx;
  out_state->gamepad.thumb_ry = state_.ry;
  return X_ERROR_SUCCESS;
}

X_RESULT IosTouchInputDriver::GetDeviceCapabilities(DeviceId id, uint32_t,
                                                    X_INPUT_CAPABILITIES* out_caps) {
  if (id != kDeviceId) return X_ERROR_DEVICE_NOT_CONNECTED;
  std::memset(out_caps, 0, sizeof(*out_caps));
  out_caps->type = rex::input::XINPUT_DEVTYPE_GAMEPAD;
  out_caps->sub_type = 1;  // XINPUT_DEVSUBTYPE_GAMEPAD
  out_caps->flags = rex::input::X_INPUT_CAPS_FFB_SUPPORTED | rex::input::X_INPUT_CAPS_WIRELESS;
  out_caps->gamepad.buttons = 0xF3FF;
  out_caps->gamepad.left_trigger = out_caps->gamepad.right_trigger = 255;
  out_caps->gamepad.thumb_lx = out_caps->gamepad.thumb_ly = 32767;
  out_caps->gamepad.thumb_rx = out_caps->gamepad.thumb_ry = 32767;
  return X_ERROR_SUCCESS;
}

X_RESULT IosTouchInputDriver::SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* v) {
  if (id != kDeviceId) return X_ERROR_DEVICE_NOT_CONNECTED;
  std::lock_guard<std::mutex> lock(mutex_);
  vibration_[0] = uint16_t(v->left_motor_speed);
  vibration_[1] = uint16_t(v->right_motor_speed);
  // CoreHaptics / GCController haptics can be driven from these values.
  return X_ERROR_SUCCESS;
}

X_RESULT IosTouchInputDriver::GetDeviceKeystroke(DeviceId, uint32_t, X_INPUT_KEYSTROKE*) {
  return X_ERROR_EMPTY;
}

void IosTouchInputDriver::Reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  state_ = Snapshot{};
  state_.changed = true;
  left_trig_finger = 0;
  right_trig_finger = 0;
}

void IosTouchInputDriver::InstallPlatformHooks() {
  static std::once_flag once;
  std::call_once(once, [] {
    ds2_ios::InstallMemoryWarningHandler();
    ds2_ios::InstallLogOverlay();  // infra only; toggled via SET -> Show logs
    dispatch_async(dispatch_get_main_queue(), ^{
      ds2_ios::InstallTouchOverlay();  // -> InstallControlsOverlay()
    });
  });
}

}  // namespace ds2_ios
