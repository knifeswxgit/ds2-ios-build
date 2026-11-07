// ds2 - diagnostic frame capture, see ds2_capture.h.

#include "ds2_capture.h"

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/ui/presenter.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>

REXCVAR_DEFINE_STRING(ds2_capture_dir, "", "DS2",
                      "Diagnostic: folder where guest output frames are saved as PPM (empty = off)");
REXCVAR_DEFINE_INT32(ds2_capture_every_ms, 2000, "DS2", "Diagnostic: interval between captured frames, ms");
REXCVAR_DEFINE_INT32(ds2_capture_max, 60, "DS2", "Diagnostic: maximum number of captured frames");
REXCVAR_DEFINE_BOOL(ds2_capture_save, true, "DS2",
                    "Diagnostic: write the PPM files (false = only log size and non-black share of each capture)");

namespace ds2::capture {
namespace {

std::thread g_thread;
std::atomic<bool> g_stop{false};

bool WritePpm(const std::filesystem::path& path, const rex::ui::RawImage& img) {
  FILE* f = std::fopen(path.string().c_str(), "wb");
  if (!f) {
    return false;
  }
  std::fprintf(f, "P6\n%u %u\n255\n", img.width, img.height);
  std::string row(size_t(img.width) * 3, '\0');
  for (uint32_t y = 0; y < img.height; ++y) {
    const uint8_t* src = img.data.data() + y * img.stride;
    for (uint32_t x = 0; x < img.width; ++x) {
      row[x * 3 + 0] = char(src[x * 4 + 0]);
      row[x * 3 + 1] = char(src[x * 4 + 1]);
      row[x * 3 + 2] = char(src[x * 4 + 2]);
    }
    std::fwrite(row.data(), 1, row.size(), f);
  }
  std::fclose(f);
  return true;
}

// Fraction of pixels that are not black: a quick check that the frame has content.
double NonBlackRatio(const rex::ui::RawImage& img) {
  if (!img.width || !img.height) {
    return 0.0;
  }
  size_t lit = 0;
  for (uint32_t y = 0; y < img.height; ++y) {
    const uint8_t* src = img.data.data() + y * img.stride;
    for (uint32_t x = 0; x < img.width; ++x) {
      if (src[x * 4 + 0] > 8 || src[x * 4 + 1] > 8 || src[x * 4 + 2] > 8) {
        ++lit;
      }
    }
  }
  return double(lit) / (double(img.width) * img.height);
}

void Loop(rex::Runtime* runtime, std::filesystem::path dir) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  int saved = 0;
  int attempt = 0;
  const auto every = std::chrono::milliseconds(std::max(100, REXCVAR_GET(ds2_capture_every_ms)));
  while (!g_stop.load() && saved < REXCVAR_GET(ds2_capture_max)) {
    std::this_thread::sleep_for(every);
    ++attempt;
    auto* gs = runtime ? runtime->graphics_system() : nullptr;
    auto* presenter = gs ? gs->presenter() : nullptr;
    if (!presenter) {
      REXLOG_INFO("[ds2 capture] attempt {}: no presenter (GPU plugin not loaded?)", attempt);
      continue;
    }
    rex::ui::RawImage img;
    if (!presenter->CaptureGuestOutput(img) || img.data.empty()) {
      REXLOG_INFO("[ds2 capture] attempt {}: no guest output yet", attempt);
      continue;
    }
    char name[64];
    std::snprintf(name, sizeof(name), "frame_%04d.ppm", saved);
    if (!REXCVAR_GET(ds2_capture_save)) {
      REXLOG_INFO("[ds2 capture] attempt {}: {}x{} non-black {:.1f}% (not saved)", attempt, img.width, img.height,
                  100.0 * NonBlackRatio(img));
      ++saved;
      continue;
    }
    const bool ok = WritePpm(dir / name, img);
    REXLOG_INFO("[ds2 capture] attempt {}: {} {}x{} non-black {:.1f}% -> {}", attempt, ok ? "saved" : "FAILED",
                img.width, img.height, 100.0 * NonBlackRatio(img), (dir / name).string());
    ++saved;
  }
}

}  // namespace

void Start(rex::Runtime* runtime) {
  const std::string dir = REXCVAR_GET(ds2_capture_dir);
  if (dir.empty() || g_thread.joinable()) {
    return;
  }
  g_stop = false;
  std::filesystem::path path(dir);
  // Relative to the executable folder, so ds2.toml on the SD card can say "frames" (sdmc:/switch/ds2/frames).
  if (path.is_relative()) {
    const auto exe_dir = rex::filesystem::GetExecutableFolder();
    if (!exe_dir.empty()) path = exe_dir / path;
  }
  g_thread = std::thread(Loop, runtime, path);
}

void Stop() {
  g_stop = true;
  if (g_thread.joinable()) {
    g_thread.join();
  }
}

}  // namespace ds2::capture
