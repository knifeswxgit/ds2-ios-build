/**
 * @file        rex/ui/souls_style.h
 *
 * @brief       DS2 port: the look of the port's own windows (start screen, install progress, achievement toast):
 *              dark bronze frame with ember accents and a Cinzel title font.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once
#include <imgui.h>

namespace rex::ui::souls {

// Palette.
inline constexpr ImVec4 kParchment{0.93f, 0.87f, 0.74f, 1.00f};   // text
inline constexpr ImVec4 kAsh{0.62f, 0.56f, 0.46f, 1.00f};         // secondary text
inline constexpr ImVec4 kEmber{0.96f, 0.56f, 0.16f, 1.00f};       // accent (bonfire orange)
inline constexpr ImVec4 kEmberDeep{0.62f, 0.30f, 0.07f, 1.00f};   // pressed / filled
inline constexpr ImVec4 kBronze{0.66f, 0.47f, 0.22f, 1.00f};      // frame
inline constexpr ImVec4 kBronzeDim{0.36f, 0.26f, 0.12f, 1.00f};   // inner frame
inline constexpr ImVec4 kCharTop{0.075f, 0.064f, 0.052f, 0.96f};   // background, top
inline constexpr ImVec4 kCharBottom{0.030f, 0.025f, 0.020f, 0.96f};  // background, bottom

// Cinzel (SIL OFL 1.1, sdk/src/ui/fonts/), loaded by ImGuiDrawer; null before that or if it failed.
inline ImFont*& TitleFont() {
  static ImFont* font = nullptr;
  return font;
}

inline ImU32 Col(ImVec4 c, float alpha = 1.0f) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * alpha)); }

inline void Diamond(ImDrawList* dl, ImVec2 c, float r, ImU32 fill) {
  dl->AddQuadFilled(ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - r, c.y), fill);
}

// The window frame: gradient background, double bronze border, ember corner pieces and a faint glow along the
// bottom edge, like an ember-lit stone tablet. Call right after ImGui::Begin of a window without its own
// background (ImGuiWindowFlags_NoBackground) and without a border.
inline void DrawFrame(float alpha = 1.0f) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 a = ImGui::GetWindowPos();
  const ImVec2 b(a.x + ImGui::GetWindowSize().x, a.y + ImGui::GetWindowSize().y);
  dl->PushClipRect(a, b, false);
  dl->AddRectFilledMultiColor(a, b, Col(kCharTop, alpha), Col(kCharTop, alpha), Col(kCharBottom, alpha),
                              Col(kCharBottom, alpha));
  // Ember glow rising from the bottom edge.
  const float glow = 18.0f;
  dl->AddRectFilledMultiColor(ImVec2(a.x, b.y - glow), b, Col(kEmber, 0.0f), Col(kEmber, 0.0f),
                              Col(kEmber, 0.16f * alpha), Col(kEmber, 0.16f * alpha));
  // Double border.
  dl->AddRect(a, b, Col(kBronze, alpha), 0.0f, 0, 2.0f);
  const float in = 6.0f;
  dl->AddRect(ImVec2(a.x + in, a.y + in), ImVec2(b.x - in, b.y - in), Col(kBronzeDim, alpha), 0.0f, 0, 1.0f);
  // Corner pieces: an L of ember on the inner border and a diamond on the outer corner.
  const float len = 22.0f, t = 2.0f;
  const ImU32 ember = Col(kEmber, alpha);
  const ImVec2 corners[4] = {a, ImVec2(b.x, a.y), ImVec2(a.x, b.y), b};
  for (int i = 0; i < 4; ++i) {
    const float sx = (i & 1) ? -1.0f : 1.0f, sy = (i & 2) ? -1.0f : 1.0f;
    const ImVec2 p(corners[i].x + sx * in, corners[i].y + sy * in);
    dl->AddLine(p, ImVec2(p.x + sx * len, p.y), ember, t);
    dl->AddLine(p, ImVec2(p.x, p.y + sy * len), ember, t);
    Diamond(dl, corners[i], 5.0f, ember);
  }
  // A small diamond in the middle of the top and bottom edges.
  Diamond(dl, ImVec2((a.x + b.x) * 0.5f, a.y), 4.0f, Col(kBronze, alpha));
  Diamond(dl, ImVec2((a.x + b.x) * 0.5f, b.y), 4.0f, Col(kBronze, alpha));
  dl->PopClipRect();
}

// A divider: a line fading out from the centre with a diamond in the middle.
inline void Divider(float alpha = 1.0f) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float w = ImGui::GetContentRegionAvail().x;
  const float y = p.y + 6.0f, cx = p.x + w * 0.5f;
  dl->AddRectFilledMultiColor(ImVec2(p.x, y - 0.75f), ImVec2(cx, y + 0.75f), Col(kBronze, 0.0f),
                              Col(kBronze, alpha), Col(kBronze, alpha), Col(kBronze, 0.0f));
  dl->AddRectFilledMultiColor(ImVec2(cx, y - 0.75f), ImVec2(p.x + w, y + 0.75f), Col(kBronze, alpha),
                              Col(kBronze, 0.0f), Col(kBronze, 0.0f), Col(kBronze, alpha));
  Diamond(dl, ImVec2(cx, y), 4.0f, Col(kEmber, alpha));
  ImGui::Dummy(ImVec2(w, 12.0f));
}

// Size the title font is baked at (the renderer uses a static atlas; other sizes scale these glyphs).
inline constexpr float kTitleFontSize = 32.0f;

// Pushes the title font at kTitleFontSize * scale (or the default font scaled up when it is missing).
inline void PushTitleFont(float scale) {
  if (ImFont* font = TitleFont()) {
    ImGui::PushFont(font, kTitleFontSize * scale);
  } else {
    ImGui::PushFont(nullptr, 0.0f);
    ImGui::SetWindowFontScale(2.4f * scale);
  }
}
inline void PopTitleFont() {
  if (!TitleFont()) ImGui::SetWindowFontScale(1.0f);
  ImGui::PopFont();
}

// Centred text in the title font, in the given color.
inline void CenteredTitle(const char* text, float scale, ImVec4 color, float alpha = 1.0f) {
  PushTitleFont(scale);
  const float w = ImGui::CalcTextSize(text).x;
  const float avail = ImGui::GetContentRegionAvail().x;
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - w) * 0.5f);
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(color.x, color.y, color.z, color.w * alpha));
  ImGui::TextUnformatted(text);
  ImGui::PopStyleColor();
  PopTitleFont();
}

// Window flags and padding for a framed window.
inline constexpr ImGuiWindowFlags kWindowFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground;
inline void PushWindowStyle() {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28.0f, 22.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
}
inline void PopWindowStyle() { ImGui::PopStyleVar(3); }

}  // namespace rex::ui::souls
