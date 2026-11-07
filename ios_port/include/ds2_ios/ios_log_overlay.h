#pragma once
/**
 * iOS log overlay: every log line is captured into a ring buffer (and into
 * Documents/ds2_log.txt). Three taps on the top-center of the screen toggle a
 * scrollable overlay showing them, so a black screen can be diagnosed on the
 * device without a PC.
 */

namespace ds2_ios {

// Register the spdlog sink that feeds the overlay + ds2_log.txt. Call once,
// after logging init (OnPostInitLogging). Idempotent.
void InstallLogSink();

// Install the triple-tap top-center gesture and the overlay view. Needs a key
// window; retries on the main queue until one exists. Idempotent.
void InstallLogOverlay();

}  // namespace ds2_ios
