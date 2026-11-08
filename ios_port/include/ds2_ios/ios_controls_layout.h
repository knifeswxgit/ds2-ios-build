#pragma once

// Single source of truth for the on-screen controls.
//
// The default zone table lives in ios_controls.mm. The layout is
// runtime-mutable (drag-to-move in edit mode) and persisted in
// NSUserDefaults. The input driver hit-tests GetTouchZones(), the overlay
// in ios_controls.mm draws exactly the same rectangles, so what is drawn
// is what is pressed.
//
// Coordinates are normalized 0..1, landscape, origin top-left.
// Button masks are X_INPUT_GAMEPAD_* values (sdk/include/rex/input/input.h).
// A zone with button == 0 is an analog trigger zone (left one = LT,
// right one = RT).

namespace ds2_ios {

struct TouchZone {
  float x0, y0, x1, y1;
  unsigned short button;
  const char* label;
};

// Defaults, kept for ResetTouchZones() and for validating saved layouts.
extern const TouchZone kDefaultTouchZones[];
extern const int kDefaultTouchZoneCount;

// Current (possibly user-moved) layout. Lazily loads persisted overrides.
const TouchZone* GetTouchZones();
int GetTouchZoneCount();

// The interactive touch area in normalized window coordinates: the iPhone
// safe area (notch/cutout, Home indicator, system swipe zones) is excluded.
// Zone coordinates are relative to this rectangle; the input driver maps
// finger coordinates through it before hit-testing.
void GetTouchAreaBounds(float* x0, float* y0, float* x1, float* y1);

// Move/resize one zone (clamped to the screen, kept non-degenerate).
void SetTouchZoneRect(int index, float x0, float y0, float x1, float y1);
void ResetTouchZones();

// Installs the drawn buttons + drag recognizers + the single settings
// button ("SET") with its panel: move buttons, reset layout, show logs,
// display scaling (Base / Stretch / Manual). Idempotent.
void InstallControlsOverlay();

// Re-fit zone frames to the current root bounds (rotation/resize).
void RelayoutZoneViews();

// Programmatic scaling of the rendered game view (Manual mode).
void SetGameDisplayScale(float sx, float sy);

}  // namespace ds2_ios
