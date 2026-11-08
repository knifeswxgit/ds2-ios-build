#pragma once
/**
 * iOS log overlay: every log line is captured into a ring buffer (and into
 * Documents/ds2_log.txt). The overlay is toggled from the controls settings
 * panel (SET -> Show logs), so a black screen can be diagnosed on the
 * device without a PC.
 */

namespace ds2_ios {

// Register the spdlog sink that feeds the overlay + ds2_log.txt. Call once,
// after logging init (OnPostInitLogging). Idempotent.
void InstallLogSink();

// Install the log overlay infrastructure (no UI by default; logs are toggled
// from the controls settings panel via ToggleLogOverlay). Idempotent.
void InstallLogOverlay();

// Show/hide the on-device log panel. Call from any thread.
void ToggleLogOverlay();

// Append "[mark] <stage>" to ds2_log.txt + the overlay buffer. Safe to call
// from anywhere at any time - it is how we find where startup stalls.
void DebugMark(const char* stage);

}  // namespace ds2_ios
