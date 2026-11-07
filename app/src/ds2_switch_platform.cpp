#include "ds2_switch_platform.h"

#if defined(__SWITCH__)
#include <cstddef>
#include <malloc.h>
#include <switch.h>
extern "C" {
size_t RexGmCommittedBytes(void);
size_t RexGmMappedBytes(void);
void RexSwitchSaltyNxHabilitar(int habilitado);
uint32_t RexGmLastResult(void);
uint8_t* RexGmBase(void);
size_t RexGmCoalescedRuns(void);
size_t RexGmCoalesceFailures(void);
extern u32 __nx_nv_transfermem_size;
size_t RexGmSize(void);
void RexGmSetCommitGranule(size_t granule);
size_t RexGmCommitGranule(void);
size_t RexGmChunkCount(void);
}
#include <algorithm>
#include <cstdio>
#include <map>
#include <tuple>
#include <vector>
#endif

namespace ds2::platform {

bool HostMemory(uint64_t* used, uint64_t* total) {
#if defined(__SWITCH__)
  u64 u = 0, t = 0;
  if (R_FAILED(svcGetInfo(&u, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0)) ||
      R_FAILED(svcGetInfo(&t, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0)))
    return false;
  *used = u;
  *total = t;
  return true;
#else
  (void)used;
  (void)total;
  return false;
#endif
}

uint64_t GuestCommitted() {
#if defined(__SWITCH__)
  return RexGmCommittedBytes();
#else
  return 0;
#endif
}

uint64_t GuestMapped() {
#if defined(__SWITCH__)
  return RexGmMappedBytes();
#else
  return 0;
#endif
}

KernelStats QueryKernelStats() {
  KernelStats k;
#if defined(__SWITCH__)
  u64 v = 0;
  if (R_SUCCEEDED(svcGetInfo(&v, InfoType_SystemResourceSizeUsed, CUR_PROCESS_HANDLE, 0))) k.sysres_used = v;
  if (R_SUCCEEDED(svcGetInfo(&v, InfoType_SystemResourceSizeTotal, CUR_PROCESS_HANDLE, 0))) k.sysres_total = v;
  u64 addr = 0;
  u64 last_mapped_w = ~0ull, last_split_w = ~0ull;
  for (int guard = 0; guard < 1000000; ++guard) {
    MemoryInfo mi{};
    u32 pi = 0;
    if (R_FAILED(svcQueryMemory(&mi, &pi, addr))) break;
    ++k.blocks;
    if (mi.type != MemType_Unmapped && (mi.perm & Perm_Rw) == 0) ++k.blocks_inaccessible;
    if (mi.type != MemType_Unmapped && mi.size) {
      // 2 MB windows (one level-3 page table each when not mapped as a 2 MB block).
      const u64 a = mi.addr, e = mi.addr + mi.size;
      u64 w0 = a >> 21;
      const u64 w1 = (e - 1) >> 21;
      if (last_mapped_w != ~0ull && w0 <= last_mapped_w) w0 = last_mapped_w + 1;
      if (w0 <= w1) k.pt_windows_mapped += w1 - w0 + 1;
      if (w1 > last_mapped_w || last_mapped_w == ~0ull) last_mapped_w = w1;
      auto split = [&](u64 w) {
        if (last_split_w == ~0ull || w > last_split_w) {
          ++k.pt_windows_split;
          last_split_w = w;
        }
      };
      if (a & 0x1FFFFF) split(a >> 21);
      if (e & 0x1FFFFF) split((e - 1) >> 21);
    }
    const u64 next = mi.addr + mi.size;
    if (next <= addr || next == 0) break;
    addr = next;
  }
  k.last_gm = RexGmLastResult();
  k.coalesced = RexGmCoalescedRuns();
  k.chunks = RexGmChunkCount();
  k.coalesce_failures = RexGmCoalesceFailures();
  k.valid = true;
#endif
  return k;
}

std::string KernelBlockHistogram(size_t max_groups) {
#if defined(__SWITCH__)
  const u64 gb = reinterpret_cast<u64>(RexGmBase()), ge = gb + RexGmSize();
  std::map<std::tuple<int, u32, u32, u32>, std::pair<u64, u64>> groups;  // -> (blocks, bytes)
  u64 addr = 0;
  for (int guard = 0; guard < 1000000; ++guard) {
    MemoryInfo mi{};
    u32 pi = 0;
    if (R_FAILED(svcQueryMemory(&mi, &pi, addr))) break;
    const int in_window = (gb && mi.addr >= gb && mi.addr < ge) ? 1 : 0;
    auto& g = groups[{in_window, mi.type & 0xFF, mi.perm, mi.attr}];
    g.first += 1;
    g.second += mi.size;
    const u64 next = mi.addr + mi.size;
    if (next <= addr || next == 0) break;
    addr = next;
  }
  std::vector<std::pair<std::tuple<int, u32, u32, u32>, std::pair<u64, u64>>> v(groups.begin(), groups.end());
  std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
  std::string out;
  char buf[160];
  for (size_t i = 0; i < v.size() && i < max_groups; ++i) {
    const auto& [key, val] = v[i];
    std::snprintf(buf, sizeof(buf), "%s%s type=0x%02X perm=%u attr=0x%X: %llu blocks %llu MB", i ? "; " : "",
                  std::get<0>(key) ? "window" : "other", std::get<1>(key), std::get<2>(key), std::get<3>(key),
                  (unsigned long long)val.first, (unsigned long long)(val.second >> 20));
    out += buf;
  }
  return out;
#else
  (void)max_groups;
  return {};
#endif
}

uint32_t ProbeKernelHeadroom(uint32_t max_pages, uint32_t* fail_rc) {
  if (fail_rc) *fail_rc = 0;
#if defined(__SWITCH__)
  constexpr size_t kScratch = 8u << 20;
  static uint8_t* scratch = static_cast<uint8_t*>(memalign(2u << 20, kScratch));
  if (!scratch) return 0;
  const uint32_t pages = std::min<uint32_t>(max_pages, kScratch / 0x2000);
  uint32_t ok = 0;
  armDCacheFlush(scratch, kScratch);
  for (uint32_t i = 0; i < pages; ++i) {
    const Result rc = svcSetMemoryAttribute(scratch + size_t(i) * 0x2000, 0x1000, 8, 8);
    if (R_FAILED(rc)) {
      if (fail_rc) *fail_rc = rc;
      break;
    }
    ++ok;
  }
  // Back to normal in one call: the blocks merge again.
  svcSetMemoryAttribute(scratch, kScratch, 8, 0);
  return ok;
#else
  (void)max_pages;
  return 0;
#endif
}

uint32_t ProbePageTableHeadroom(uint32_t max_tables, uint32_t* fail_rc) {
  if (fail_rc) *fail_rc = 0;
#if defined(__SWITCH__)
  // Each probe maps one 4 KB heap page into its own empty 2 MB window of the stack region with svcMapMemory: the
  // kernel must take a fresh level-3 page table for every window. A process without its own system resource takes
  // page tables from the kernel's page-table heap that it shares with every system process, so this measures how
  // much of that shared heap is left. Everything is unmapped again right away.
  constexpr uint32_t kMax = 128;
  static uint8_t* src = static_cast<uint8_t*>(memalign(0x1000, kMax * 0x1000));
  static uint8_t* dst = nullptr;
  static bool tried = false;
  if (!tried) {
    tried = true;
    const size_t span = size_t(kMax) << 21;
    virtmemLock();
    void* p = virtmemFindStack(span + (2u << 20), 0);
    if (p) {
      uint8_t* aligned = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(p) + 0x1FFFFF) & ~uintptr_t(0x1FFFFF));
      if (virtmemAddReservation(p, span + (2u << 20))) dst = aligned;
    }
    virtmemUnlock();
  }
  if (!src || !dst) {
    if (fail_rc) *fail_rc = 0xFFFFFFFF;
    return 0;
  }
  const uint32_t n = std::min<uint32_t>(max_tables, kMax);
  uint32_t ok = 0;
  for (uint32_t i = 0; i < n; ++i) {
    const Result rc = svcMapMemory(dst + (size_t(i) << 21), src + size_t(i) * 0x1000, 0x1000);
    if (R_FAILED(rc)) {
      if (fail_rc) *fail_rc = rc;
      break;
    }
    ++ok;
  }
  for (uint32_t i = 0; i < ok; ++i) svcUnmapMemory(dst + (size_t(i) << 21), src + size_t(i) * 0x1000, 0x1000);
  return ok;
#else
  (void)max_tables;
  return 0;
#endif
}

std::string ResourceLimits() {
#if defined(__SWITCH__)
  u64 v = 0;
  if (R_FAILED(svcGetInfo(&v, InfoType_ResourceLimit, INVALID_HANDLE, 0)) || !v) return {};
  const Handle h = static_cast<Handle>(v);
  static const struct {
    LimitableResource r;
    const char* name;
    int shift;
  } kRes[] = {{LimitableResource_Memory, "memory_mb", 20},
              {LimitableResource_Threads, "threads", 0},
              {LimitableResource_Events, "events", 0},
              {LimitableResource_TransferMemories, "transfer_mems", 0},
              {LimitableResource_Sessions, "sessions", 0}};
  std::string out;
  char buf[64];
  for (const auto& r : kRes) {
    s64 cur = 0, max = 0;
    if (R_SUCCEEDED(svcGetResourceLimitCurrentValue(&cur, h, r.r)) &&
        R_SUCCEEDED(svcGetResourceLimitLimitValue(&max, h, r.r))) {
      std::snprintf(buf, sizeof(buf), "%s%s %lld/%lld", out.empty() ? "" : ", ", r.name,
                    (long long)(cur >> r.shift), (long long)(max >> r.shift));
      out += buf;
    }
  }
  svcCloseHandle(h);
  return out;
#else
  return {};
#endif
}

size_t SetCommitGranule(size_t bytes) {
#if defined(__SWITCH__)
  RexGmSetCommitGranule(bytes);
  return RexGmCommitGranule();
#else
  (void)bytes;
  return 0;
#endif
}

uint32_t SetNvTransferMemMb(int mb) {
#if defined(__SWITCH__)
  if (mb > 0) __nx_nv_transfermem_size = static_cast<u32>(mb) << 20;
  return __nx_nv_transfermem_size >> 20;
#else
  (void)mb;
  return 0;
#endif
}

void SetSaltyNxBridge(bool enabled) {
#if defined(__SWITCH__)
  RexSwitchSaltyNxHabilitar(enabled ? 1 : 0);
#else
  (void)enabled;
#endif
}

}  // namespace ds2::platform
