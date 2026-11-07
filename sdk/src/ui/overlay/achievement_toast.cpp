/**
 * @file        ui/overlay/achievement_toast.cpp
 * @brief       Achievement toast implementation. See achievement_toast.h for details.
 *
 * @copyright   Copyright (c) 2026 Rien Gupta <rgupta9@scu.edu>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/ui/overlay/achievement_toast.h>
#include <imgui.h>

#include <algorithm>

#include <rex/ui/immediate_drawer.h>
#include <rex/ui/souls_style.h>

namespace rex::ui {

AchievementToastDialog::AchievementToastDialog(ImGuiDrawer* drawer,
                                               ImmediateDrawer* immediate_drawer,
                                               rex::Runtime* runtime)
    : AchievementNotificationDialog(drawer), icon_cache_(immediate_drawer, runtime) {}

AchievementToastDialog::~AchievementToastDialog() {}

void AchievementToastDialog::Push(const rex::system::AchievementEvent& event) {
  std::lock_guard<std::mutex> lock(mutex_);
  queue_.push_back({event, std::chrono::steady_clock::now()});
}

void AchievementToastDialog::OnDraw(ImGuiIO& io) {
  auto now = std::chrono::steady_clock::now();

  PendingToast toast;
  float age = 0.0f;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!queue_.empty()) {
      if (!queue_.front().visible_since) {
        queue_.front().visible_since = now;
      }
      age = std::chrono::duration<float>(now - *queue_.front().visible_since).count();
      if (age >= kDisplaySeconds) {
        queue_.pop_front();
      } else {
        break;
      }
    }
    if (queue_.empty()) {
      return;
    }
    toast = queue_.front();
  }

  float alpha = 1.0f;
  if (age < 0.3f)
    alpha = age / 0.3f;
  else if (age > kDisplaySeconds - 0.5f)
    alpha = (kDisplaySeconds - age) / 0.5f;
  alpha = std::max(0.0f, std::min(1.0f, alpha));

  // v109 (DS2 port): Dark Souls look, rex/ui/souls_style.h.
  namespace souls = rex::ui::souls;
  const float pad = 16.0f;
  const float w = 380.0f, h = 92.0f;
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - w - pad, io.DisplaySize.y - h - pad), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
  const ToastStyle& s = imgui_drawer()->style().toast;

  ImGuiWindowFlags flags = souls::kWindowFlags | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
                           ImGuiWindowFlags_NoFocusOnAppearing;
  // Colors carry the fade envelope; the style holds them at full opacity.
  auto faded = [alpha](ImVec4 c) { return ImVec4(c.x, c.y, c.z, c.w * alpha); };

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f, 14.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_Text, faded(s.text));
  if (ImGui::Begin("##ach_toast", nullptr, flags)) {
    souls::DrawFrame(alpha * s.background_alpha);
    ImmediateTexture* icon = icon_cache_.GetIcon(toast.event.achievement);
    if (icon) {
      ImGui::Image(reinterpret_cast<ImTextureID>(icon), ImVec2(s.icon_size, s.icon_size));
      ImGui::SameLine(0.0f, 12.0f);
    }

    ImGui::BeginGroup();
    souls::PushTitleFont(0.62f);
    ImGui::PushStyleColor(ImGuiCol_Text, faded(s.title));
    ImGui::TextUnformatted("Achievement Unlocked");
    ImGui::PopStyleColor();
    souls::PopTitleFont();
    souls::PushTitleFont(0.52f);
    ImGui::TextUnformatted(toast.event.achievement.label.c_str());
    souls::PopTitleFont();
    ImGui::PushStyleColor(ImGuiCol_Text, faded(souls::kAsh));
    ImGui::SetWindowFontScale(1.6f);
    ImGui::Text("%dG", static_cast<int>(toast.event.achievement.gamerscore));
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor();
    ImGui::EndGroup();
  }
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
}

}  // namespace rex::ui
