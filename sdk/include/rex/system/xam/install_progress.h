#pragma once
// Progress of ContentManager::InstallContent (DS2 v101: progress bar while the first boot unpacks packages).
// Kept out of content_manager.h, which the generated code's precompiled header includes.

#include <atomic>
#include <cstdint>

namespace rex::system::xam {

struct InstallProgress {
  std::atomic<uint64_t> bytes_done{0};
  std::atomic<uint64_t> bytes_total{0};  // 0 until a package is known to need extracting
};

InstallProgress& install_progress();

}  // namespace rex::system::xam
