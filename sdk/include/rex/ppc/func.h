/**
 * @file        ppc/func.h
 * @brief       Recompiled PPC function signature, mapping table, and definition macros
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Split out of rex/ppc/context.h so callers that only need the
 *              PPCFunc type do not pull in PPCContext and its SIMD headers.
 */

#pragma once

#include <cstddef>
#include <cstdint>

//=============================================================================
// PPCFunc Type Definition
//=============================================================================
// Function signature for recompiled PPC functions.
// All recompiled functions take a context reference and memory base pointer.

// Forward declaration of the PPC execution context
struct PPCContext;

// Function signature for recompiled PPC functions
using PPCFunc = void(PPCContext& ctx, uint8_t* base);

namespace rex::runtime {
PPCFunc* ResolveIndirectFunction(uint32_t guest_address);
}  // namespace rex::runtime

//=============================================================================
// PPC Function Macros
//=============================================================================

#define REX_JOIN(x, y) x##y
#define REX_XSTRINGIFY(x) #x
#define REX_STRINGIFY(x) REX_XSTRINGIFY(x)
// REX_FUNC is the bare signature; the declaring site supplies linkage, either
// by writing extern "C" itself or by using REX_EXTERN.
#define REX_FUNC(x) void x([[maybe_unused]] PPCContext& __restrict ctx, uint8_t* base)
#define REX_EXTERN(x) extern "C" REX_FUNC(x)

// Per-function call counters (DS2 port diagnostic, cvar ds2_func_counts): the generated code calls
// REX_FUNC_COUNT(guest address) at the top of every function. rex_func_counts_biased is 0 when counting is off
// (one load and a not-taken branch per call); when on it is (counter array - code base), so the counter of the
// function at guest address A is the uint32 at rex_func_counts_biased + A (A is 4-aligned).
extern "C" uintptr_t rex_func_counts_biased;
#define REX_FUNC_COUNT(addr)                                         \
  do {                                                               \
    const uintptr_t rex_fc_ = rex_func_counts_biased;                \
    if (__builtin_expect(rex_fc_ != 0, 0)) ++*(uint32_t*)(rex_fc_ + (addr)); \
  } while (0)

//=============================================================================
// Function Mapping
//=============================================================================

struct PPCFuncMapping {
  size_t guest;
  PPCFunc* host;
};

extern PPCFuncMapping PPCFuncMappings[];
