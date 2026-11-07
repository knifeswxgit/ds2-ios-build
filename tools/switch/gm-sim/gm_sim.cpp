// Host simulation of sdk/src/core/guest_memory_switch.cpp against a model of the Horizon address space.
// Checks that views always alias the right backing pages and estimates level-3 page tables the kernel needs
// (a 2 MB window needs one unless all 512 pages are mapped by one call, physically contiguous, 2 MB aligned
// in both VA and PA, with one permission). Replays a commit trace recorded on PC (REX_COMMIT_TRACE).
#include <switch.h>
#include "guest_memory_switch.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <random>
#include <set>
#include <vector>

namespace {
struct Ent { u32 type; u64 phys; u32 perm; u64 block; };
std::map<u64, Ent> g_pages;         // VA page -> mapping (only alias mappings; heap is identity)
std::set<u64> g_locked;             // heap pages used as code-memory source
u64 g_block = 1;
std::mt19937_64 g_rng(1234);
bool g_random_va = true;            // libnx returns any 4 KB-aligned address
u64 g_aslr_next = 0x8000000000ull, g_code_next = 0x9000000000ull;
u64 Pg(u64 a) { return a >> 12; }
u64 Resolve(u64 va) { auto it = g_pages.find(Pg(va)); return it == g_pages.end() ? Pg(va) : it->second.phys; }
void* Find(u64& next, size_t size) {
  u64 a = next + (g_random_va ? (g_rng() % 512) * 0x1000 : 0);
  next = (a + size + 0x10000000) & ~0xFFFull;
  return reinterpret_cast<void*>(a);
}
}  // namespace

Handle envGetOwnProcessHandle(void) { return 0xFFFF8001; }
void virtmemLock(void) {}
void virtmemUnlock(void) {}
void* virtmemFindAslr(size_t size, size_t) { return Find(g_aslr_next, size); }
void* virtmemFindCodeMemory(size_t size, size_t) { return Find(g_code_next, size); }
VirtmemReservation* virtmemAddReservation(void*, size_t) { return reinterpret_cast<VirtmemReservation*>(1); }
void virtmemRemoveReservation(VirtmemReservation*) {}

Result svcMapProcessCodeMemory(Handle, u64 dst, u64 src, u64 size) {
  for (u64 i = 0; i < size; i += 0x1000) {
    if (g_pages.count(Pg(dst + i)) || g_locked.count(Pg(src + i))) return 0xD401;
  }
  const u64 b = g_block++;
  for (u64 i = 0; i < size; i += 0x1000) {
    g_locked.insert(Pg(src + i));
    g_pages[Pg(dst + i)] = {MemType_ModuleCodeMutable, Pg(src + i), Perm_None, b};
  }
  return 0;
}
Result svcUnmapProcessCodeMemory(Handle, u64 dst, u64 src, u64 size) {
  for (u64 i = 0; i < size; i += 0x1000) {
    auto it = g_pages.find(Pg(dst + i));
    if (it == g_pages.end() || it->second.phys != Pg(src + i)) return 0xD401;
  }
  for (u64 i = 0; i < size; i += 0x1000) { g_pages.erase(Pg(dst + i)); g_locked.erase(Pg(src + i)); }
  return 0;
}
Result svcSetProcessMemoryPermission(Handle, u64 addr, u64 size, u32 perm) {
  for (u64 i = 0; i < size; i += 0x1000) {
    auto it = g_pages.find(Pg(addr + i));
    if (it == g_pages.end() || it->second.type != MemType_ModuleCodeMutable) return 0xD401;  // views: refused
  }
  for (u64 i = 0; i < size; i += 0x1000) g_pages[Pg(addr + i)].perm = perm;
  return 0;
}
Result svcMapProcessMemory(void* dstp, Handle, u64 src, u64 size) {
  const u64 dst = reinterpret_cast<u64>(dstp);
  for (u64 i = 0; i < size; i += 0x1000) {
    if (g_pages.count(Pg(dst + i))) return 0xD401;
    if (!g_pages.count(Pg(src + i))) return 0xD401;  // source must be the code alias here
  }
  const u64 b = g_block++;
  for (u64 i = 0; i < size; i += 0x1000) g_pages[Pg(dst + i)] = {MemType_ProcessMem, Resolve(src + i), Perm_Rw, b};
  return 0;
}
Result svcUnmapProcessMemory(void* dstp, Handle, u64 src, u64 size) {
  const u64 dst = reinterpret_cast<u64>(dstp);
  for (u64 i = 0; i < size; i += 0x1000) {
    auto it = g_pages.find(Pg(dst + i));
    if (it == g_pages.end() || it->second.type != MemType_ProcessMem || it->second.phys != Resolve(src + i))
      return 0xD401;
  }
  for (u64 i = 0; i < size; i += 0x1000) g_pages.erase(Pg(dst + i));
  return 0;
}
Result svcQueryMemory(MemoryInfo* mi, u32* pi, u64 addr) {
  *pi = 0;
  std::memset(mi, 0, sizeof(*mi));
  auto it = g_pages.find(Pg(addr));
  if (it == g_pages.end()) {
    auto nx = g_pages.upper_bound(Pg(addr));
    u64 lo = Pg(addr), hi = nx == g_pages.end() ? lo + 1 : nx->first;
    auto pv = g_pages.lower_bound(Pg(addr));
    u64 start = lo;
    if (pv != g_pages.begin()) { --pv; start = pv->first + 1; }
    mi->addr = start << 12; mi->size = (hi - start) << 12; mi->type = MemType_Unmapped;
    return 0;
  }
  // A block: neighbouring pages from the same call with the same permission.
  u64 lo = it->first, hi = it->first + 1;
  while (true) { auto p = g_pages.find(lo - 1); if (p == g_pages.end() || p->second.block != it->second.block || p->second.perm != it->second.perm) break; --lo; }
  while (true) { auto n = g_pages.find(hi); if (n == g_pages.end() || n->second.block != it->second.block || n->second.perm != it->second.perm) break; ++hi; }
  mi->addr = lo << 12; mi->size = (hi - lo) << 12; mi->type = it->second.type; mi->perm = it->second.perm;
  return 0;
}

// --- driver ---------------------------------------------------------------------------------------------
struct ViewDef { u64 vstart, vend, target; };
static const ViewDef kViews[] = {
    {0x00000000, 0x3FFFFFFF, 0x0}, {0x40000000, 0x7EFFFFFF, 0x40000000}, {0x7F000000, 0x7FFFFFFF, 0x100000000},
    {0x80000000, 0x8FFFFFFF, 0x80000000}, {0x90000000, 0x9FFFFFFF, 0x80000000}, {0xA0000000, 0xBFFFFFFF, 0x100000000},
    {0xC0000000, 0xDFFFFFFF, 0x100000000}, {0xE0000000, 0xFFFFFFFF, 0x100001000}, {0x100000000, 0x11FFFFFFF, 0x100000000}};

struct Stats { u64 tables = 0, windows = 0, blocks = 0, chunks = 0; };
static Stats Count() {
  Stats st;
  std::map<u64, std::vector<std::pair<u64, Ent>>> win;
  for (auto& [pg, e] : g_pages) win[pg >> 9].push_back({pg, e});
  for (auto& [w, v] : win) {
    ++st.windows;
    bool block = v.size() == 512;
    if (block) {
      const Ent& f = v.front().second;
      block = (f.phys & 511) == 0;
      for (size_t i = 0; block && i < v.size(); ++i)
        block = v[i].second.phys == f.phys + i && v[i].second.perm == f.perm && v[i].second.block == f.block;
    }
    if (!block) {
      ++st.tables;
      if (std::getenv("GM_SIM_BREAKDOWN")) {
        const u64 va = w << 21;
        const u64 b = reinterpret_cast<u64>(RexGmBase());
        const char* where = "code alias";
        if (va >= b && va < b + 0x120000000ull) {
          const u64 g = va - b;
          where = g < 0x7F000000 ? "virt views" : g < 0x80000000 ? "7F view" : g < 0xA0000000 ? "xex views" :
                  g < 0xC0000000 ? "A0 view" : g < 0xE0000000 ? "C0 view" : g < 0x100000000 ? "E0 view" : "raw phys";
        }
        static std::map<std::string, int> cnt; cnt[where]++;
        static bool reg = false;
        if (!reg) { reg = true; std::atexit([] { for (auto& [k, c] : cnt) std::printf("  table: %-10s %d\n", k.c_str(), c); }); }
      }
    }
  }
  u64 last_block = 0, last_pg = 0;
  for (auto& [pg, e] : g_pages) { if (e.block != last_block || pg != last_pg + 1) ++st.blocks; last_block = e.block; last_pg = pg; }
  return st;
}

int main(int argc, char** argv) {
  if (argc < 3) { std::fprintf(stderr, "usage: gm_sim <trace> <granule_kb> [aligned_va 0/1]\n"); return 2; }
  const size_t gran = size_t(std::atoi(argv[2])) << 10;
  g_random_va = argc > 3 ? std::atoi(argv[3]) == 0 : true;
  RexGmSetCommitGranule(gran);
  const size_t kSize = 0x120000000ull;
  uint8_t* base = RexGmInit(kSize);
  assert(base);
  for (auto& v : kViews) RexGmAddView(base + v.vstart, v.target, v.vend - v.vstart + 1);
  // Replay the PC commit trace (host address = 0x100000000 + guest address on PC).
  FILE* f = std::fopen(argv[1], "r");
  unsigned long a, l; int n = 0, fails = 0;
  std::vector<std::pair<u64, u64>> commits;
  while (std::fscanf(f, "%lx %lx", &a, &l) == 2) {
    const u64 g = a - 0x100000000ull;
    if (g >= kSize) continue;
    size_t off = 0;
    if (!RexGmWindowToOffset(reinterpret_cast<u64>(base) + g, &off)) { ++fails; continue; }
    if (!RexGmCommit(off, l, REX_GM_WRITE)) ++fails;
    commits.push_back({g, l});
    ++n;
  }
  std::fclose(f);
  // The game touches its memory: fault in every committed page of every commit through the view used.
  u64 faults = 0;
  for (auto& [g, l] : commits)
    for (u64 p = g & ~0xFFFull; p < g + l; p += 0x1000)
      if (!g_pages.count(Pg(reinterpret_cast<u64>(base) + p)) && RexGmFaultIn(reinterpret_cast<u64>(base) + p)) ++faults;
  // Physical memory is also touched through the other physical views (A0/C0/E0) for a random sample.
  for (int i = 0; i < 20000; ++i) {
    const u64 g = 0xA0000000ull + (g_rng() % 0x60000000ull);
    const u64 va = reinterpret_cast<u64>(base) + (g & ~0xFFFull);
    if (!g_pages.count(Pg(va)) && RexGmFaultIn(va)) ++faults;
  }
  // Write watches: protect and release 3000 random 4-64 KB ranges in A0.
  int prot_fail = 0;
  for (int i = 0; i < 3000; ++i) {
    const u64 g = 0xA0000000ull + ((g_rng() % 0x1FF00000ull) & ~0xFFFull);
    const size_t len = (1 + g_rng() % 16) * 0x1000;
    if (!RexGmProtect(base + g, len, REX_GM_READ, nullptr)) ++prot_fail;
    if (i % 3 == 0 && !RexGmProtect(base + g, len, REX_GM_WRITE, nullptr)) ++prot_fail;
  }
  // Invariant: every mapped view page maps the backing that RexGmShadowFor reports for it.
  u64 bad = 0, checked = 0;
  for (auto& [pg, e] : g_pages) {
    if (e.type != MemType_ProcessMem) continue;
    void* sh = RexGmShadowFor(pg << 12);
    if (!sh || Resolve(reinterpret_cast<u64>(sh)) != e.phys) ++bad;
    ++checked;
  }
  const Stats st = Count();
  std::printf("granule=%zuKB aligned_va=%d commits=%d fails=%d faults=%llu prot_fail=%d chunks=%zu committed=%zuMB "
              "mapped=%zuMB | view pages checked=%llu bad=%llu | 2MB windows=%llu need_table=%llu (%.1f MB) blocks=%llu\n",
              gran >> 10, !g_random_va, n, fails, (unsigned long long)faults, prot_fail, RexGmChunkCount(),
              RexGmCommittedBytes() >> 20, RexGmMappedBytes() >> 20, (unsigned long long)checked,
              (unsigned long long)bad, (unsigned long long)st.windows, (unsigned long long)st.tables,
              st.tables * 4.0 / 1024, (unsigned long long)st.blocks);
  // Decommit a few virtual ranges, then shut down: everything must be unmapped and unlocked again.
  for (size_t i = 0; i < commits.size(); i += 7) {
    size_t off = 0;
    if (commits[i].first < 0x7F000000 && RexGmWindowToOffset(reinterpret_cast<u64>(base) + commits[i].first, &off))
      RexGmDecommit(off & ~(gran - 1), (commits[i].second + gran) & ~(gran - 1));
  }
  RexGmShutdown();
  std::printf("after shutdown: mapped pages=%zu locked=%zu\n", g_pages.size(), g_locked.size());
  return (bad || !g_pages.empty() || !g_locked.empty()) ? 1 : 0;
}
