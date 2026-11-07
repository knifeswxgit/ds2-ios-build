// ds2 - scripted virtual pad, see ds2_input_script.h.

#include <rex/chrono/clock.h>

#include "ds2_input_script.h"

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

REXCVAR_DEFINE_STRING(ds2_input_script, "", "DS2",
                      "Tests: virtual pad script in seconds since start, e.g. \"60:start,70:a,80-82:down\" "
                      "(T:button = short press, T1-T2:button = hold). Buttons: a b x y lb rb lt rt up down "
                      "left right start back lstick_up lstick_down lstick_left lstick_right rstick_up rstick_down rstick_left "
                      "rstick_right. Times are guest seconds (= real seconds unless ds2_guest_time_scalar is set)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_DOUBLE(ds2_guest_time_scalar, 1.0, "DS2",
                      "Tests on a slow PC: guest clock speed (0.05 = the game sees 20x less time pass). With software "
                      "Vulkan at ~1.5 frames/s, 0.05 makes each frame look like ~1/30 s to the game")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(ds2_guest_time_scalar_at, 0, "DS2",
                     "Tests: apply ds2_guest_time_scalar only from this many guest seconds on (menus at full speed, "
                     "then slow the world down). Needs ds2_input_script")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace ds2::input_script {

void ApplyTimeScalar() {
  const double scalar = REXCVAR_GET(ds2_guest_time_scalar);
  if (REXCVAR_GET(ds2_guest_time_scalar_at) > 0) return;  // applied later by the script pad
  if (scalar > 0.0 && scalar != 1.0) {
    rex::chrono::Clock::set_guest_time_scalar(scalar);
    REXLOG_INFO("[ds2 input] guest time scalar {}", scalar);
  }
}

namespace {

using rex::X_RESULT;
using rex::X_STATUS;
using Clock = std::chrono::steady_clock;
namespace in = rex::input;

constexpr in::DeviceId kScriptPad = in::DeviceId(0x4453325343525054ull);  // "DS2SCRPT"
constexpr double kPress = 0.25;  // seconds of a single press

// X_INPUT_GAMEPAD bits (triggers in bits 16/17, left stick directions in 18-21) and the XInputGetKeystroke VK_PAD code.
struct Button {
  std::string_view name;
  uint32_t bits;
  uint16_t key;
};
constexpr Button kButtons[] = {
    {"a", 0x1000, 0x5800},       {"b", 0x2000, 0x5801},        {"x", 0x4000, 0x5802},
    {"y", 0x8000, 0x5803},       {"rb", 0x0200, 0x5804},       {"lb", 0x0100, 0x5805},
    {"lt", 1u << 16, 0x5806},    {"rt", 1u << 17, 0x5807},     {"up", 0x0001, 0x5810},
    {"down", 0x0002, 0x5811},    {"left", 0x0004, 0x5812},     {"right", 0x0008, 0x5813},
    {"start", 0x0010, 0x5814},   {"back", 0x0020, 0x5815},     {"lstick_up", 1u << 18, 0x5820},
    {"lstick_down", 1u << 19, 0x5821}, {"lstick_right", 1u << 20, 0x5822}, {"lstick_left", 1u << 21, 0x5823},
    {"rstick_up", 1u << 22, 0x5830},   {"rstick_down", 1u << 23, 0x5831}, {"rstick_right", 1u << 24, 0x5832},
    {"rstick_left", 1u << 25, 0x5833},
};

struct Step {
  double begin;
  double end;
  uint32_t bits;
  // Polls that saw this step held: a step stays held past its end until the game has read it twice, so a slow
  // game (a few frames per second on a software-Vulkan PC) never misses a press.
  uint32_t polls = 0;
};

bool Number(std::string_view text, double& value) {
  const std::string copy(text);
  char* end = nullptr;
  value = std::strtod(copy.c_str(), &end);
  return !copy.empty() && end == copy.c_str() + copy.size() && value >= 0.0;
}

std::vector<Step> Parse(std::string_view text) {
  std::vector<Step> steps;
  size_t i = 0;
  const auto sep = [](char c) { return c == ' ' || c == ',' || c == ';'; };
  while (i < text.size()) {
    while (i < text.size() && sep(text[i])) ++i;
    size_t j = i;
    while (j < text.size() && !sep(text[j])) ++j;
    if (j == i) break;
    const std::string_view step = text.substr(i, j - i);
    i = j;
    const size_t colon = step.find(':');
    const std::string_view times = step.substr(0, colon);
    const std::string_view name = colon == std::string_view::npos ? std::string_view() : step.substr(colon + 1);
    const Button* button = nullptr;
    for (const Button& b : kButtons) {
      if (b.name == name) button = &b;
    }
    const size_t dash = times.find('-');
    double begin = 0.0, end = 0.0;
    const bool ok = button && Number(times.substr(0, dash), begin) &&
                    (dash == std::string_view::npos ? (end = begin + kPress, true)
                                                    : (Number(times.substr(dash + 1), end) && end > begin));
    if (!ok) {
      REXLOG_WARN("[ds2 input] invalid step \"{}\", ignored", step);
      continue;
    }
    steps.push_back({begin, end, button->bits});
  }
  return steps;
}

class ScriptPad final : public in::InputDriver {
 public:
  explicit ScriptPad(std::vector<Step> steps)
      : InputDriver(nullptr, 0), steps_(std::move(steps)), start_(Clock::now()) {}

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }

  void EnumerateDevices(std::vector<in::DeviceInfo>& out) override {
    in::DeviceInfo info;
    info.id = kScriptPad;
    info.name = "DS2 test script pad";
    info.synthetic = true;
    out.push_back(info);
  }

  X_RESULT GetDeviceState(in::DeviceId id, in::X_INPUT_STATE* out_state) override {
    if (id != kScriptPad) return X_ERROR_DEVICE_NOT_CONNECTED;
    uint32_t packet = 0;
    const uint32_t bits = Update(packet);
    if (out_state) {
      std::memset(out_state, 0, sizeof(*out_state));
      out_state->packet_number = packet;
      out_state->gamepad.buttons = uint16_t(bits & 0xFFFF);
      out_state->gamepad.left_trigger = ((bits >> 16) & 1) ? 0xFF : 0;
      out_state->gamepad.right_trigger = ((bits >> 17) & 1) ? 0xFF : 0;
      out_state->gamepad.thumb_ly = ((bits >> 18) & 1) ? 30000 : ((bits >> 19) & 1) ? -30000 : 0;
      out_state->gamepad.thumb_lx = ((bits >> 20) & 1) ? 30000 : ((bits >> 21) & 1) ? -30000 : 0;
      out_state->gamepad.thumb_ry = ((bits >> 22) & 1) ? 30000 : ((bits >> 23) & 1) ? -30000 : 0;
      out_state->gamepad.thumb_rx = ((bits >> 24) & 1) ? 30000 : ((bits >> 25) & 1) ? -30000 : 0;
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT GetDeviceCapabilities(in::DeviceId id, uint32_t flags, in::X_INPUT_CAPABILITIES* out_caps) override {
    (void)flags;
    if (id != kScriptPad) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (out_caps) {
      std::memset(out_caps, 0, sizeof(*out_caps));
      out_caps->type = 0x01;
      out_caps->sub_type = 0x01;
      out_caps->gamepad.buttons = 0xFFFF;
      out_caps->gamepad.left_trigger = 0xFF;
      out_caps->gamepad.right_trigger = 0xFF;
      out_caps->gamepad.thumb_lx = static_cast<int16_t>(0x7FFF);
      out_caps->gamepad.thumb_ly = static_cast<int16_t>(0x7FFF);
      out_caps->gamepad.thumb_rx = static_cast<int16_t>(0x7FFF);
      out_caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT SetDeviceVibration(in::DeviceId id, in::X_INPUT_VIBRATION* vibration) override {
    (void)vibration;
    return id == kScriptPad ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }

  X_RESULT GetDeviceKeystroke(in::DeviceId id, uint32_t flags, in::X_INPUT_KEYSTROKE* out_keystroke) override {
    (void)flags;
    if (id != kScriptPad) return X_ERROR_DEVICE_NOT_CONNECTED;
    uint32_t packet = 0;
    Update(packet);
    std::lock_guard<std::mutex> lock(mutex_);
    if (keys_.empty()) return X_ERROR_EMPTY;
    if (out_keystroke) *out_keystroke = keys_.front();
    keys_.pop_front();
    return X_ERROR_SUCCESS;
  }

 private:
  uint32_t Update(uint32_t& packet) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Guest seconds since the first poll: the same as real seconds unless ds2_guest_time_scalar slows the game
    // down (PC runs), so a script written for normal speed still lines up with what the game does.
    const double t = double(rex::chrono::Clock::QueryGuestUptimeMillis()) / 1000.0;
    if (!scalar_applied_ && REXCVAR_GET(ds2_guest_time_scalar_at) > 0 && t >= REXCVAR_GET(ds2_guest_time_scalar_at)) {
      scalar_applied_ = true;
      rex::chrono::Clock::set_guest_time_scalar(REXCVAR_GET(ds2_guest_time_scalar));
      REXLOG_INFO("[ds2 input] guest time scalar {} from {:.1f} s", REXCVAR_GET(ds2_guest_time_scalar), t);
    }
    uint32_t bits = 0;
    for (Step& s : steps_) {
      if (t >= s.begin && (t < s.end || s.polls < 2)) {
        bits |= s.bits;
        ++s.polls;
      }
    }
    if (bits != current_) {
      for (const Button& b : kButtons) {
        const bool before = (current_ & b.bits) != 0;
        const bool now = (bits & b.bits) != 0;
        if (before == now) continue;
        in::X_INPUT_KEYSTROKE key{};
        key.virtual_key = b.key;
        key.flags = uint16_t(now ? in::X_INPUT_KEYSTROKE_KEYDOWN : in::X_INPUT_KEYSTROKE_KEYUP);
        if (keys_.size() < 64) keys_.push_back(key);
        REXLOG_INFO("[ds2 input] {} {} at {:.1f} s", b.name, now ? "pressed" : "released", t);
      }
      current_ = bits;
      ++packet_;
    }
    packet = packet_;
    return bits;
  }

  std::vector<Step> steps_;
  const Clock::time_point start_;
  std::mutex mutex_;
  uint32_t current_ = 0;
  uint32_t packet_ = 1;
  bool scalar_applied_ = false;
  std::deque<in::X_INPUT_KEYSTROKE> keys_;
};

}  // namespace

void Wrap(rex::RuntimeConfig& config) {
  const std::string script = REXCVAR_GET(ds2_input_script);
  if (script.empty() || !config.input_factory) return;
  std::vector<Step> steps = Parse(script);
  if (steps.empty()) {
    REXLOG_WARN("[ds2 input] \"{}\" has no valid steps", script);
    return;
  }
  auto base = config.input_factory;
  config.input_factory = [base, steps](bool tool_mode) -> std::unique_ptr<rex::system::IInputSystem> {
    auto system = base(tool_mode);
    if (system && !tool_mode) {
      // The default factory is CreateDefaultInputSystem (sdk/src/ui/rex_app.cpp), a rex::input::InputSystem.
      static_cast<in::InputSystem*>(system.get())->AddDriver(std::make_unique<ScriptPad>(steps));
      REXLOG_INFO("[ds2 input] script pad with {} steps", steps.size());
    }
    return system;
  };
}

}  // namespace ds2::input_script
