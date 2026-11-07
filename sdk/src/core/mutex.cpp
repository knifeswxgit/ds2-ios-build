/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/thread/mutex.h>

namespace rex::thread {

std::recursive_mutex& global_critical_region::mutex() {
  static std::recursive_mutex global_mutex;
  return global_mutex;
}

}  // namespace rex::thread

// See REX_FUNC_COUNT (rex/ppc/func.h) and REX_ENTER_GLOBAL_LOCK (codegen pch template).
extern "C" uintptr_t rex_func_counts_biased = 0;
extern "C" bool rex_msr_global_lock = true;
extern "C" uint64_t rex_fpcr_switches = 0;
extern "C" bool rex_fpcr_ftz_fixed = false;
