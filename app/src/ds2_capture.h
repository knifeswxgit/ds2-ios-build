// ds2 - diagnostic frame capture (PC only): periodically saves the guest output image as PPM files.
// Off unless --ds2_capture_dir is set. Uses ui::Presenter::CaptureGuestOutput, so it needs a GPU plugin.

#pragma once

namespace rex {
class Runtime;
}

namespace ds2::capture {

// Starts the capture thread if ds2_capture_dir is set; safe to call once.
void Start(rex::Runtime* runtime);
// Stops and joins the capture thread.
void Stop();

}  // namespace ds2::capture
