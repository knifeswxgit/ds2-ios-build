#include "ds2_func_counts.h"

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <vector>

REXCVAR_DEFINE_BOOL(ds2_func_counts, false, "DS2",
                    "Diagnostic: count the calls of every game function and append them to logs/ds2_llamadas.bin "
                    "every ds2_func_counts_every_s seconds (decoder: tools/analysis/ds2_func_counts.py)");
REXCVAR_DEFINE_INT32(ds2_func_counts_every_s, 10, "DS2", "Seconds per ds2_func_counts window");
REXCVAR_DEFINE_BOOL(ds2_msr_per_thread, false, "DS2",
                    "mfmsr/mtmsrd regions as per-thread state instead of the process-wide mutex (read at start; "
                    "PORTING.md 5.1 of the Dead Space 2 Switch port)");

extern "C" uintptr_t rex_func_counts_biased;
extern "C" bool rex_msr_global_lock;
extern "C" uint64_t rex_fpcr_switches;
extern "C" bool rex_fpcr_ftz_fixed;

REXCVAR_DEFINE_BOOL(ds2_fpcr_ftz_fixed, false, "DS2",
                    "Keep FPCR flush-to-zero on for scalar code too (read at start; the guest code then never "
                    "rewrites FPCR). Changes denormal results of scalar float code");

namespace ds2::func_counts {
namespace {
// Recompiled code range of this XEX (REX_CODE_BASE / REX_CODE_SIZE in the generated ds2_pch.h).
constexpr uint32_t kCodeBase = 0x82240000u;
constexpr uint32_t kCodeSize = 0x011F8264u;
constexpr uint32_t kCount = (kCodeSize + 3) / 4;
uint32_t* g_counts = nullptr;
FILE* g_file = nullptr;
int g_last = 0;

void Dump(int seconds) {
  if (!g_counts || !g_file) return;
  std::vector<uint32_t> out;
  out.reserve(65536);
  uint64_t total = 0;
  for (uint32_t i = 0; i < kCount; ++i) {
    const uint32_t c = g_counts[i];
    if (!c) continue;
    g_counts[i] = 0;  // racing increments may be lost: a count, not an exact trace
    out.push_back(kCodeBase + i * 4);
    out.push_back(c);
    total += c;
  }
  const uint32_t header[3] = {0x31574E44u /* "DNW1" */, uint32_t(seconds), uint32_t(out.size() / 2)};
  std::fwrite(header, sizeof(header), 1, g_file);
  if (!out.empty()) std::fwrite(out.data(), sizeof(uint32_t), out.size(), g_file);
  std::fflush(g_file);
  static uint64_t fpcr_before = 0;
  const uint64_t fpcr = rex_fpcr_switches;
  REXLOG_INFO("[llamadas] t={}s: {} funciones, {:.1f} M llamadas en la ventana; cambios de FPCR {:.1f} k",
              seconds, out.size() / 2, double(total) / 1e6, double(fpcr - fpcr_before) / 1e3);
  fpcr_before = fpcr;
}
}  // namespace

void Configure() {
  rex_msr_global_lock = !REXCVAR_GET(ds2_msr_per_thread);
  rex_fpcr_ftz_fixed = REXCVAR_GET(ds2_fpcr_ftz_fixed);
  REXLOG_INFO("[ds2] FPCR flush-to-zero: {}", rex_fpcr_ftz_fixed ? "always on" : "switched by the guest code");
  REXLOG_INFO("[ds2] mtmsrd regions: {}", rex_msr_global_lock ? "process-wide mutex" : "per-thread state");
  if (!REXCVAR_GET(ds2_func_counts)) return;
  g_counts = static_cast<uint32_t*>(std::calloc(kCount, sizeof(uint32_t)));
  if (!g_counts) {
    REXLOG_ERROR("[llamadas] no memory for {} counters", kCount);
    return;
  }
  const auto dir = rex::filesystem::GetExecutableFolder();
  std::error_code ec;
  std::filesystem::create_directories(dir / "logs", ec);
  const auto path = dir / "logs" / "ds2_llamadas.bin";
  g_file = std::fopen(path.string().c_str(), "wb");
  if (!g_file) {
    REXLOG_ERROR("[llamadas] cannot open {}", path.string());
    std::free(g_counts);
    g_counts = nullptr;
    return;
  }
  rex_func_counts_biased = reinterpret_cast<uintptr_t>(g_counts) - kCodeBase;
  REXLOG_INFO("[llamadas] counting calls of every game function ({} MB), window {} s -> {}",
              (kCount * 4) >> 20, REXCVAR_GET(ds2_func_counts_every_s), path.string());
}

void Tick(int seconds) {
  const int every = std::max(1, REXCVAR_GET(ds2_func_counts_every_s));
  if (!g_counts || seconds - g_last < every) return;
  g_last = seconds;
  Dump(seconds);
}

void Flush() {
  if (g_file) std::fflush(g_file);
}

}  // namespace ds2::func_counts
