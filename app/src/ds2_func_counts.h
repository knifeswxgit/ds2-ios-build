// ds2 - per-function call counters of the recompiled game code (cvar ds2_func_counts, off by default).
//
// Every guest function sub_XXXXXXXX counts its calls (REX_FUNC_COUNT, rex/ppc/func.h). Every
// ds2_func_counts_every_s seconds the watchdog thread appends the non-zero counters to logs/ds2_llamadas.bin and
// zeroes them, so the file holds one window per period: which game functions ran and how often. Two windows
// compared (normal play / a hang) show the code that runs only in one of them.
// Decoder: tools/analysis/ds2_func_counts.py. Cost: ~19 MB of counters, one increment per guest call.
//
// Also applies ds2_msr_per_thread (the mfmsr/mtmsrd regions, see the codegen pch template), which must be set
// before the first guest instruction runs: both are configured from OnPostInitLogging.

#pragma once

namespace ds2::func_counts {
void Configure();
// Called once per second from the watchdog thread.
void Tick(int seconds);
void Flush();
}  // namespace ds2::func_counts
