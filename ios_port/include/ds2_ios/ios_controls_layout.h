#pragma once

// Single source of truth for the on-screen controls: the input driver uses
// these normalized rectangles for hit testing, ios_paths.mm draws them.
// Landscape coordinates, 0..1, origin top-left. X_INPUT_GAMEPAD_* bit values
// from sdk/include/rex/input/input.h.
namespace ds2_ios {

struct TouchZone {
  float x0, y0, x1, y1;
  unsigned short button;
  const char* label;
};

static constexpr TouchZone kTouchZones[] = {
    {0.86f, 0.44f, 0.93f, 0.58f, 0x1000, "A"},       // kA
    {0.93f, 0.32f, 1.00f, 0.46f, 0x2000, "B"},       // kB
    {0.79f, 0.32f, 0.86f, 0.46f, 0x4000, "X"},       // kX
    {0.86f, 0.20f, 0.93f, 0.34f, 0x8000, "Y"},       // kY
    {0.00f, 0.30f, 0.09f, 0.40f, 0x0100, "LB"},      // kLB
    {0.91f, 0.30f, 1.00f, 0.40f, 0x0200, "RB"},      // kRB
    {0.00f, 0.16f, 0.09f, 0.26f, 0, "LT"},           // analog trigger
    {0.91f, 0.16f, 1.00f, 0.26f, 0, "RT"},           // analog trigger
    {0.44f, 0.00f, 0.56f, 0.08f, 0x0010, "START"},   // kStart
    {0.56f, 0.00f, 0.64f, 0.08f, 0x0020, "BACK"},    // kBack
    {0.00f, 0.00f, 0.06f, 0.07f, 0x0001, "^"},       // kDUp
    {0.00f, 0.07f, 0.06f, 0.14f, 0x0002, "v"},       // kDDown
    {0.06f, 0.00f, 0.10f, 0.07f, 0x0004, "<"},       // kDLeft
    {0.06f, 0.07f, 0.10f, 0.14f, 0x0008, ">"},       // kDRight
};

static constexpr int kTouchZoneCount =
    static_cast<int>(sizeof(kTouchZones) / sizeof(kTouchZones[0]));

}  // namespace ds2_ios
