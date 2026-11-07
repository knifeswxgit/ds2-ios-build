/* See guest_memory_switch.h for the design and the reason behind each decision. */

#if defined(__SWITCH__)

#include "guest_memory_switch.h"

#include <malloc.h>
#include <string.h>
#include <switch.h>

#include <map>
#include <set>
#include <atomic>
#include <mutex>
#include <vector>
#include <algorithm>
#include <iterator>

namespace {

constexpr size_t kPageSize  = 0x1000;
/*
 * Large chunks are requested 2 MB aligned, so the kernel can use block descriptors instead of a
 * page table.
 */
constexpr size_t kBlockSize = 0x200000;

/*
 * Commit granule (DS2 port). Committing exactly the requested range left hundreds of small chunks at
 * arbitrary 4 KB offsets; each one needs level-3 page tables in the heap (its backing), in the code alias
 * and in every view, and under hbloader those tables come from a kernel heap shared with the whole system
 * (2001-0103 for everybody once it is dry). With a 2 MB granule every chunk is one 2 MB-aligned 2 MB piece,
 * which the kernel can map with a single block descriptor and no page table. 0x1000 = the old behaviour.
 */
size_t g_commit_granule = 0x200000;

constexpr size_t AlignDown(size_t v, size_t a) { return v & ~(a - 1); }
constexpr size_t AlignUp(size_t v, size_t a) { return (v + a - 1) & ~(a - 1); }

/* A committed chunk: heap backing, its code alias (the shadow), and the span of the mapping it covers. */
struct Chunk {
    size_t offset;      /* offset in the mapping */
    size_t length;
    void*  backing;     /* from the heap. Inaccessible after mapping it as code */
    void*  shadow;      /* the codeAlias: always RW, a back door */
};

/* A view: a span of the window that mirrors the mapping from `offset`. */
struct View {
    uint8_t* base;
    size_t   offset;
    size_t   length;
};

/* A span already mapped in this operation, so it can be undone if another fails. */
struct Tramo {
    size_t   win_off;
    uint8_t* src;
    size_t   len;
};

/*
 * The window size, without the lock
 *
 * InGuestWindow() is called on every fault (tens of thousands per second in a race) and only needs
 * to know whether the address falls inside the window. Reading it from State meant taking the global
 * lock, the same one the mappings use, and in the profile the thread that faulted most spent 22.8%
 * of its time in svcArbitrateLock because of this.
 *
 * base and size are set once in RexGmInit and only change again in RexGmShutdown, so an atomic copy
 * is enough and cannot get out of sync.
 */
std::atomic<size_t> g_size_rapido{0};

/*
 * The base goes in its own atomic and is published after the size. That way, if a thread sees the
 * base, the size is already set: the other way round (as it used to be) the window could be seen
 * half-initialized, with a valid base and size 0, and then InGuestWindow said no and a legitimate
 * guest fault ended in a fatal stop.
 */
std::atomic<uint8_t*> g_base_rapido{nullptr};

struct State {
    /*
     * Exclusive lock, and it cannot be replaced by a reader/writer one.
     *
     * shared_mutex was tried and it hung the game. The idea was sound on paper: the three calls of
     * the fault path only read, so they could run concurrently. But the performance profiler suspends
     * threads to sample them (svcSetThreadActivity in switch_perf.cpp) and then takes this same lock
     * to read the memory counters.
     *
     * With shared_mutex that is fatal: libstdc++ gives writers preference, so if a thread holding the
     * shared lock is suspended, the first writer to arrive queues up, and all following readers queue
     * behind it. The whole process stops. It hung exactly like that in a test: all 17 threads idle and
     * the profiler unable to write a single report.
     *
     * What did pay off from that attempt is in exception_handler_switch.cpp: asking once per fault
     * instead of five times. That reduces lock traffic without changing how it blocks.
     */
    std::mutex mutex;
    uint8_t* base = nullptr;
    size_t   size = 0;
    VirtmemReservation* reservation = nullptr;

    std::vector<View> views;
    /* chunks sorted by offset: allows finding the one that covers an offset */
    std::map<size_t, Chunk> chunks;
    /*
     * Logical permissions per page of the window, keyed by offset from base.
     * Only pages that are not REX_GM_WRITE are stored; REX_GM_WRITE is the common
     * case, so the table stays small. A page with an entry here is unmapped from
     * the window, whether it is committed or not.
     */
    std::map<size_t, RexGmAccess> protection;
    /*
     * Which (view index, chunk offset) pairs are mapped. A chunk is not mapped into
     * all five views when it is committed: it enters each view the first time it is
     * accessed there. The real ceiling on Horizon is the process limit on mappable
     * memory, and mapping too much exhausts it.
     */
    std::set<std::pair<size_t, size_t>> view_mapped;

    size_t   committed = 0;
    size_t   mapped = 0;
    uint32_t last_result = 0;
};

State& S() {
    static State s;
    return s;
}

Handle Proc() { return envGetOwnProcessHandle(); }

bool Intersect(size_t a, size_t al, size_t b, size_t bl, size_t* lo, size_t* hi) {
    const size_t l = a > b ? a : b;
    const size_t ea = a + al, eb = b + bl;
    const size_t h = ea < eb ? ea : eb;
    if (l >= h) return false;
    *lo = l;
    *hi = h;
    return true;
}

/* Finds the chunk that contains an offset of the mapping. */
Chunk* ChunkAt(State& s, size_t offset) {
    auto it = s.chunks.upper_bound(offset);
    if (it == s.chunks.begin()) return nullptr;
    --it;
    Chunk& c = it->second;
    return (offset >= c.offset && offset < c.offset + c.length) ? &c : nullptr;
}

/* The view that contains a window address. */
const View* ViewFor(State& s, uint64_t addr) {
    for (const View& v : s.views) {
        const uint64_t lo = reinterpret_cast<uint64_t>(v.base);
        if (addr >= lo && addr < lo + v.length) return &v;
    }
    return nullptr;
}

RexGmAccess AccessOf(State& s, size_t page_off) {
    auto it = s.protection.find(page_off);
    return it == s.protection.end() ? REX_GM_WRITE : it->second;
}

/* Is there committed backing behind this window address? */
bool IsCommittedWindowAddress(State& s, uint64_t addr) {
    const View* v = ViewFor(s, addr);
    if (!v) return false;
    const size_t off = v->offset + static_cast<size_t>(addr - reinterpret_cast<uint64_t>(v->base));
    return ChunkAt(s, off) != nullptr;
}

/*
 * Unmaps [win_off, win_off+len) from the window, skipping pages with logical
 * protection, which are already unmapped. `src` is the shadow address that
 * corresponds to win_off.
 *
 * Walk: `it` always points to the first protected page >= cur. Keys are
 * page-aligned and increasing, so skipping a protected page and advancing the
 * iterator keeps that invariant, and the loop always makes progress.
 */
/*
 * Read-only page: try it, and if it is refused, go back to unmapping
 *
 * Why. Watching writes by unmapping the page has an expensive side effect: the
 * guest cannot read from it either, and every read is an exception. Measured in
 * a race: 108,000 emulated reads per second at about 9 us each, which is
 * practically the whole core of the main thread.
 *
 * With the page read-only, watching works the same (the write faults and the
 * handler services it) but reads are free.
 *
 * Why it is not a given. svcSetProcessMemoryPermission only accepts memory whose
 * state allows permission changes. The shadow is mapped with
 * svcMapProcessCodeMemory, which does allow it (it is already used there, with
 * Perm_Rw). The views are mapped with svcMapProcessMemory, and whether Horizon
 * allows it there too is not documented anywhere.
 *
 * So it is tried once. The first page that needs watching is tried with
 * permissions; if the kernel refuses, that is recorded and from then on pages
 * are unmapped as before. Nothing is left half done: if the call fails, it has
 * not changed anything.
 */
enum class SoloLectura { kSinProbar, kSi, kNo };
SoloLectura g_solo_lectura = SoloLectura::kSinProbar;

/* Returns true if the page was protected through permissions. */
bool ProtegerConPermisos(uint8_t* pagina, bool solo_lectura) {
    if (g_solo_lectura == SoloLectura::kNo) return false;

    const Result rc = svcSetProcessMemoryPermission(
        Proc(), reinterpret_cast<u64>(pagina), kPageSize, solo_lectura ? Perm_R : Perm_Rw);
    if (R_FAILED(rc)) {
        if (g_solo_lectura == SoloLectura::kSinProbar) {
            g_solo_lectura = SoloLectura::kNo;  // no se vuelve a intentar
        }
        return false;
    }
    g_solo_lectura = SoloLectura::kSi;
    return true;
}
void UnmapWindowRange(State& s, size_t win_off, uint8_t* src, size_t len) {
    if (g_solo_lectura == SoloLectura::kSi) {
        /* No holes here: everything was mapped, watched or not. */
        if (R_SUCCEEDED(svcUnmapProcessMemory(s.base + win_off, Proc(),
                                              reinterpret_cast<u64>(src), len))) {
            s.mapped -= len;
        }
        return;
    }
    const size_t end = win_off + len;
    size_t cur = win_off;
    auto it = s.protection.lower_bound(cur);
    while (cur < end) {
        if (it != s.protection.end() && it->first == cur) {
            cur += kPageSize;
            ++it;
            continue;
        }
        const size_t stop = (it != s.protection.end() && it->first < end) ? it->first : end;
        if (R_SUCCEEDED(svcUnmapProcessMemory(s.base + cur, Proc(),
                                              reinterpret_cast<u64>(src + (cur - win_off)),
                                              stop - cur))) {
            s.mapped -= stop - cur;
        }
        cur = stop;
    }
}

/*
 * When watched pages are read-only instead of unmapped, the whole range is mapped
 * at once and the watched pages then get their permission lowered. Simpler and
 * faster than skipping them.
 */
bool MapWindowRangeSoloLectura(State& s, size_t win_off, uint8_t* src, size_t len) {
    const Result rc =
        svcMapProcessMemory(s.base + win_off, Proc(), reinterpret_cast<u64>(src), len);
    if (R_FAILED(rc)) {
        s.last_result = rc;
        return false;
    }
    s.mapped += len;
    for (auto it = s.protection.lower_bound(win_off);
         it != s.protection.end() && it->first < win_off + len; ++it) {
        ProtegerConPermisos(s.base + it->first, true);
    }
    return true;
}

/*
 * Maps [win_off, win_off+len) honoring the logical protection. If a call fails,
 * it undoes what this call had mapped and returns false: it never leaves a range
 * half mapped.
 */
bool MapWindowRange(State& s, size_t win_off, uint8_t* src, size_t len) {
    if (g_solo_lectura == SoloLectura::kSi) {
        return MapWindowRangeSoloLectura(s, win_off, src, len);
    }
    const size_t end = win_off + len;
    size_t cur = win_off;
    auto it = s.protection.lower_bound(cur);
    while (cur < end) {
        if (it != s.protection.end() && it->first == cur) {
            cur += kPageSize;
            ++it;
            continue;
        }
        const size_t stop = (it != s.protection.end() && it->first < end) ? it->first : end;
        const Result rc = svcMapProcessMemory(s.base + cur, Proc(),
                                              reinterpret_cast<u64>(src + (cur - win_off)),
                                              stop - cur);
        if (R_FAILED(rc)) {
            s.last_result = rc;
            /*
             * The unmap walks the same way as this loop, so it removes exactly what was
             * mapped, no more and no less.
             */
            UnmapWindowRange(s, win_off, src, cur - win_off);
            return false;
        }
        s.mapped += stop - cur;
        cur = stop;
    }
    return true;
}

/*
 * Maps the range [offset, offset+length) of a chunk into every view that covers
 * it. This is where the 360 mirrors come from.
 *
 * All or nothing: if it fails in one view, it undoes the previous ones. The
 * alternative (leaving a view unmapped and carrying on) would make every access
 * to it be emulated one by one by the exception handler: it would work, but at a
 * fraction of the speed and without any warning. That is exactly the bug the
 * first version had.
 */
bool ViewIndexFor(State& s, uint64_t addr, size_t* out) {
    for (size_t i = 0; i < s.views.size(); i++) {
        const uint64_t lo = reinterpret_cast<uint64_t>(s.views[i].base);
        if (addr >= lo && addr < lo + s.views[i].length) { *out = i; return true; }
    }
    return false;
}

/* Maps a chunk into one view: the view of the faulting access. */
bool MapChunkIntoView(State& s, size_t view_index, const Chunk& c) {
    const View& v = s.views[view_index];
    size_t lo, hi;
    if (!Intersect(c.offset, c.length, v.offset, v.length, &lo, &hi)) return false;
    const size_t win_off = static_cast<size_t>(v.base - s.base) + (lo - v.offset);
    uint8_t* src = static_cast<uint8_t*>(c.shadow) + (lo - c.offset);
    if (!MapWindowRange(s, win_off, src, hi - lo)) return false;
    s.view_mapped.insert({view_index, c.offset});
    return true;
}

void UnmapChunkFromViews(State& s, const Chunk& c) {
    for (size_t i = 0; i < s.views.size(); i++) {
        auto it = s.view_mapped.find({i, c.offset});
        if (it == s.view_mapped.end()) continue;  /* never entered this view */
        const View& v = s.views[i];
        size_t lo, hi;
        if (!Intersect(c.offset, c.length, v.offset, v.length, &lo, &hi)) continue;
        const size_t win_off = static_cast<size_t>(v.base - s.base) + (lo - v.offset);
        uint8_t* src = static_cast<uint8_t*>(c.shadow) + (lo - c.offset);
        UnmapWindowRange(s, win_off, src, hi - lo);
        s.view_mapped.erase(it);
    }
}

void ReleaseChunk(const Chunk& c) {
    svcUnmapProcessCodeMemory(Proc(), reinterpret_cast<u64>(c.shadow),
                              reinterpret_cast<u64>(c.backing), c.length);
    free(c.backing);
}

/*
 * DS2 (2026-10-20): every page a write watch unmaps and later maps back stays a separate 4 KB kernel memory
 * block in the view, because each svcMapProcessMemory call makes its own block. On the console the views of
 * DS2 reached 7,214 such blocks (979 MB) and the process ~10,000 blocks in total, and from there every
 * kernel mapping failed with 2001-0103 (resource exhausted) - GPU buffers, file writes, other processes.
 *
 * After a page is mapped back, the run of unprotected pages around it (inside the same chunk and view) is
 * unmapped and mapped again with one call, so it is one block again. Skipped when the run is already a
 * single block. On failure the run is mapped back piece by piece as before.
 */
size_t g_coalesce_runs = 0;
size_t g_coalesce_failures = 0;

void CoalesceViewRun(State& s, size_t page_off) {
    const uint64_t wbase = reinterpret_cast<uint64_t>(s.base);
    const View* v = ViewFor(s, wbase + page_off);
    if (!v) return;
    const size_t view_lo = static_cast<size_t>(v->base - s.base);
    const size_t off_mapeo = v->offset + (page_off - view_lo);
    Chunk* c = ChunkAt(s, off_mapeo);
    if (!c) return;
    size_t vi = 0;
    if (!ViewIndexFor(s, wbase + page_off, &vi) || s.view_mapped.count({vi, c->offset}) == 0) return;
    /* Window range of chunk ∩ view. */
    size_t lo, hi;
    if (!Intersect(c->offset, c->length, v->offset, v->length, &lo, &hi)) return;
    size_t run_lo = view_lo + (lo - v->offset);
    size_t run_hi = view_lo + (hi - v->offset);
    /* Clip at the nearest protected pages on each side. */
    auto it = s.protection.lower_bound(page_off);
    if (it != s.protection.end() && it->first < run_hi) run_hi = it->first;
    if (it != s.protection.begin()) {
        auto prev = std::prev(it);
        if (prev->first >= run_lo) run_lo = prev->first + kPageSize;
    }
    if (run_hi <= run_lo + kPageSize) return;
    MemoryInfo mi;
    u32 pi = 0;
    if (R_FAILED(svcQueryMemory(&mi, &pi, wbase + run_lo))) return;
    if (mi.addr <= wbase + run_lo && mi.addr + mi.size >= wbase + run_hi) return; /* already one block */
    const size_t len = run_hi - run_lo;
    uint8_t* src = static_cast<uint8_t*>(c->shadow) + (v->offset + (run_lo - view_lo) - c->offset);
    if (R_FAILED(svcUnmapProcessMemory(s.base + run_lo, Proc(), reinterpret_cast<u64>(src), len))) {
        ++g_coalesce_failures;
        return; /* nothing changed */
    }
    const Result rc = svcMapProcessMemory(s.base + run_lo, Proc(), reinterpret_cast<u64>(src), len);
    if (R_SUCCEEDED(rc)) {
        ++g_coalesce_runs;
        return;
    }
    ++g_coalesce_failures;
    s.last_result = rc;
    s.mapped -= len;
    /* Same result as before the attempt: the run mapped again, in pieces if need be. */
    if (MapWindowRange(s, run_lo, src, len)) return;
    /* Could not map it back: forget the chunk in this view so the next access faults it in again. */
    s.view_mapped.erase({vi, c->offset});
}

}  // namespace

extern "C" {

uint8_t* RexGmInit(size_t size) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.base) return s.base;

    /*
     * virtmemFind*'s second argument is a guard size, not an alignment: libnx returns any 4 KB-aligned
     * address. A window at a random 4 KB offset can never be mapped with 2 MB block descriptors, so every
     * view mapping took level-3 page tables (DS2 port). Ask for 2 MB more and align inside it.
     */
    virtmemLock();
    void* found = virtmemFindAslr(size + kBlockSize, 0);
    s.base = found ? reinterpret_cast<uint8_t*>(AlignUp(reinterpret_cast<size_t>(found), kBlockSize)) : nullptr;
    if (s.base) s.reservation = virtmemAddReservation(s.base, size);
    virtmemUnlock();

    if (!s.base || !s.reservation) {
        s.base = nullptr;
        return nullptr;
    }
    s.size = size;
    g_size_rapido.store(size, std::memory_order_release);
    g_base_rapido.store(s.base, std::memory_order_release);
    return s.base;
}

void RexGmShutdown(void) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);

    for (auto& kv : s.chunks) {
        UnmapChunkFromViews(s, kv.second);
        ReleaseChunk(kv.second);
    }
    s.chunks.clear();
    s.views.clear();
    s.protection.clear();
    s.committed = 0;
    s.mapped = 0;

    if (s.reservation) {
        virtmemLock();
        virtmemRemoveReservation(s.reservation);
        virtmemUnlock();
        s.reservation = nullptr;
    }
    s.base = nullptr;
    s.size = 0;
    /*
     * Reverse order from startup: the base is cleared first, so nobody gets in with
     * a size that is no longer valid.
     */
    g_base_rapido.store(nullptr, std::memory_order_release);
    g_size_rapido.store(0, std::memory_order_release);
}

uint8_t* RexGmBase(void) { return g_base_rapido.load(std::memory_order_acquire); }

size_t RexGmSize(void) { return g_size_rapido.load(std::memory_order_acquire); }

bool RexGmWindowToOffset(uint64_t window_address, size_t* offset_out) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.base) return false;
    const View* v = ViewFor(s, window_address);
    if (!v) return false;
    if (offset_out) {
        *offset_out =
            v->offset + static_cast<size_t>(window_address - reinterpret_cast<uint64_t>(v->base));
    }
    return true;
}

size_t RexGmCommittedBytes(void) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.committed;
}

size_t RexGmMappedBytes(void) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.mapped;
}

int RexGmModoProteccion(void) {
    switch (g_solo_lectura) {
        case SoloLectura::kSi: return 1;
        case SoloLectura::kNo: return 2;
        default: return 0;
    }
}

size_t RexGmCoalescedRuns(void) { return g_coalesce_runs; }

void RexGmSetCommitGranule(size_t granule) {
    if (granule < kPageSize || (granule & (granule - 1)) != 0) granule = kPageSize;
    g_commit_granule = granule;
}

size_t RexGmCommitGranule(void) { return g_commit_granule; }

size_t RexGmChunkCount(void) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.chunks.size();
}
size_t RexGmCoalesceFailures(void) { return g_coalesce_failures; }

uint32_t RexGmLastResult(void) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.last_result;
}

bool RexGmAddView(uint8_t* base, size_t offset, size_t length) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.base) return false;
    if (base < s.base || static_cast<size_t>(base - s.base) + length > s.size)
        return false;  /* outside the reserved window */

    const View v{base, offset, length};

    /*
     * Nothing is mapped here: what is already committed enters this view the first
     * time it is touched.
     */
    s.views.push_back(v);
    return true;
}

bool RexGmCommit(size_t offset, size_t length, RexGmAccess access) {
    /*
     * Memory is always committed accessible. Restricting permissions is the job of
     * RexGmProtect, which works on window addresses and not on mapping offsets (and
     * it has to, because the same offset can be mirrored in several windows with
     * different permissions: the 360 does exactly that).
     *
     * `access` stays in the signature because the natural caller is
     * AllocFixed(kCommit, access), and removing it would invite forgetting to apply
     * it. Pages that already had logical protection keep it.
     */
    (void)access;
    if (length == 0) return true;

    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.base) return false;

    const size_t gran  = g_commit_granule > kPageSize ? g_commit_granule : kPageSize;
    const size_t begin = AlignDown(offset, gran);
    const size_t end   = std::min(AlignUp(offset + length, gran), s.size);

    size_t cur = begin;
    while (cur < end) {
        if (Chunk* existente = ChunkAt(s, cur)) {
            cur = existente->offset + existente->length;
            continue;
        }

        /* Where the gap ends: the next chunk, or the end. */
        size_t hueco_fin = end;
        auto sig = s.chunks.upper_bound(cur);
        if (sig != s.chunks.end() && sig->second.offset < hueco_fin)
            hueco_fin = sig->second.offset;

        /* Granule mode: one chunk per granule, so views still map lazily, 2 MB at a time. */
        const size_t tam = gran > kPageSize ? std::min(hueco_fin - cur, gran - (cur & (gran - 1)))
                                            : hueco_fin - cur;
        hueco_fin = cur + tam;
        const size_t align = (tam >= kBlockSize && (cur & (kBlockSize - 1)) == 0)
                                 ? kBlockSize : kPageSize;

        void* backing = memalign(align, tam);
        if (!backing) {
            s.last_result = REX_GM_ERR_NOMEM;
            return false;
        }
        memset(backing, 0, tam);

        virtmemLock();
        /* As for the window: the second argument of virtmemFind* is a guard, so align by hand. */
        void* shadow = virtmemFindCodeMemory(tam + (align > kPageSize ? align : 0), 0);
        if (shadow && align > kPageSize)
            shadow = reinterpret_cast<void*>(AlignUp(reinterpret_cast<size_t>(shadow), align));
        Result rc = shadow ? svcMapProcessCodeMemory(Proc(), reinterpret_cast<u64>(shadow),
                                                     reinterpret_cast<u64>(backing), tam)
                           : 0;
        virtmemUnlock();

        if (!shadow) {
            free(backing);
            s.last_result = REX_GM_ERR_NOCODEVA;
            return false;
        }
        if (R_FAILED(rc)) {
            free(backing);
            s.last_result = rc;
            return false;
        }

        /* The code alias starts without write access; it has to be opened. */
        rc = svcSetProcessMemoryPermission(Proc(), reinterpret_cast<u64>(shadow), tam, Perm_Rw);
        if (R_FAILED(rc)) {
            svcUnmapProcessCodeMemory(Proc(), reinterpret_cast<u64>(shadow),
                                      reinterpret_cast<u64>(backing), tam);
            free(backing);
            s.last_result = rc;
            return false;
        }

        /*
         * Not mapped into the views: each view gets it on first access
         * (RexGmFaultIn). Mapping it into all five 360 views multiplies by five the
         * use of the process limit on mappable memory.
         */
        const Chunk c{cur, tam, backing, shadow};
        s.chunks[cur] = c;
        s.committed += tam;

        cur = hueco_fin;
    }
    return true;
}

bool RexGmFaultIn(uint64_t window_address) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.base) return false;

    size_t vi = 0;
    if (!ViewIndexFor(s, window_address, &vi)) return false;

    /*
     * Page with logical protection: it is unmapped on purpose (write watching, MMIO,
     * read-only). Mapping the chunk would not bring it in (protected pages are
     * skipped), so answering yes would leave the fault repeating forever. The next
     * steps of the handler deal with it.
     */
    const size_t win_off =
        static_cast<size_t>(window_address - reinterpret_cast<uint64_t>(s.base));
    if (s.protection.find(AlignDown(win_off, kPageSize)) != s.protection.end()) return false;

    const size_t off = s.views[vi].offset +
                       static_cast<size_t>(window_address -
                                           reinterpret_cast<uint64_t>(s.views[vi].base));
    Chunk* c = ChunkAt(s, off);
    if (!c) return false;                                       /* sin confirmar */
    if (s.view_mapped.count({vi, c->offset}) != 0) {
        /*
         * Already mapped in this view. Since CoalesceViewRun (RexGmProtect) briefly unmaps and remaps a
         * run of pages, a thread can fault inside that gap and get here once the lock is free: if the page
         * is mapped and writable now, retrying the instruction is correct. Checked with the kernel so a
         * real hole can never turn into an endless retry.
         */
        MemoryInfo mi;
        u32 pi = 0;
        const u64 page = AlignDown(window_address, kPageSize);
        return R_SUCCEEDED(svcQueryMemory(&mi, &pi, page)) && mi.type != MemType_Unmapped &&
               (mi.perm & Perm_Rw) == Perm_Rw;
    }
    return MapChunkIntoView(s, vi, *c);
}



bool RexGmDecommit(size_t offset, size_t length) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.base) return false;

    const size_t begin = AlignDown(offset, kPageSize);
    const size_t end   = AlignUp(offset + length, kPageSize);

    for (auto it = s.chunks.begin(); it != s.chunks.end();) {
        const Chunk c = it->second;
        /*
         * Only chunks entirely contained in the range are freed: splitting a chunk
         * would also mean splitting its backing, and the backing is a single heap
         * allocation. Partial chunks stay committed, which is conservative but correct.
         */
        if (c.offset >= begin && c.offset + c.length <= end) {
            UnmapChunkFromViews(s, c);
            ReleaseChunk(c);
            s.committed -= c.length;
            it = s.chunks.erase(it);
        } else {
            ++it;
        }
    }
    return true;
}

bool RexGmProtect(uint8_t* address, size_t length, RexGmAccess access, RexGmAccess* out_old) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.base) return false;

    const uint64_t addr  = reinterpret_cast<uint64_t>(address);
    const uint64_t wbase = reinterpret_cast<uint64_t>(s.base);
    if (addr < wbase || addr + length > wbase + s.size) return false;

    const size_t begin = AlignDown(static_cast<size_t>(addr - wbase), kPageSize);
    const size_t end   = AlignUp(static_cast<size_t>(addr - wbase) + length, kPageSize);

    if (out_old) *out_old = AccessOf(s, begin);

    const bool debe_estar = (access == REX_GM_WRITE);

    /*
     * Fast path, and the common one: asking for full access on a range that has no
     * protected page. AllocFixed(kCommit) does it for every allocation of the game
     * heap, and without this it would walk the range page by page (131,072
     * iterations for the 512 MB of physical memory) to do nothing.
     */
    if (debe_estar) {
        auto it = s.protection.lower_bound(begin);
        if (it == s.protection.end() || it->first >= end) return true;
    }

    std::vector<size_t> remapped; /* pages mapped back in this call, see CoalesceViewRun */

    /*
     * DS2 port: consecutive pages that need the same kernel operation (all mapped back, or all unmapped) and
     * whose sources are consecutive in the same chunk's shadow go in ONE svcMapProcessMemory /
     * svcUnmapProcessMemory call instead of one per 4 KB page. A lifted write watch can cover up to 4 MB, which
     * used to be 1,024 calls (and 1,024 separate kernel mappings) at once. If the batched call fails, the run is
     * retried page by page as before, and on a page failure the logical protection of the pages not done is
     * put back, so the table never says "mapped" for a page that is not.
     */
    struct Pending {
        bool active = false;
        bool map = false;      /* true: map back (estaba == false); false: unmap */
        size_t win = 0;        /* window offset of the first page */
        u64 src = 0;           /* shadow address of the first page */
        size_t len = 0;
        std::vector<std::pair<size_t, RexGmAccess>> old;  /* page -> access before this call */
    } run;
    auto restore_from = [&](size_t idx) {
        for (size_t k = idx; k < run.old.size(); ++k) {
            const auto& [pg, acc] = run.old[k];
            if (acc == REX_GM_WRITE) s.protection.erase(pg);
            else s.protection[pg] = acc;
        }
    };
    auto flush = [&]() -> bool {
        if (!run.active) return true;
        run.active = false;
        const Result rc = run.map ? svcMapProcessMemory(s.base + run.win, Proc(), run.src, run.len)
                                  : svcUnmapProcessMemory(s.base + run.win, Proc(), run.src, run.len);
        if (R_SUCCEEDED(rc)) {
            if (run.map) {
                s.mapped += run.len;
                remapped.push_back(run.win);
            } else {
                s.mapped -= run.len;
            }
            return true;
        }
        for (size_t k = 0; k < run.len / kPageSize; ++k) {
            const size_t pg = run.win + k * kPageSize;
            const u64 sp = run.src + k * kPageSize;
            const Result r1 = run.map ? svcMapProcessMemory(s.base + pg, Proc(), sp, kPageSize)
                                      : svcUnmapProcessMemory(s.base + pg, Proc(), sp, kPageSize);
            if (R_FAILED(r1)) {
                s.last_result = r1;
                restore_from(k);
                return false;
            }
            if (run.map) {
                s.mapped += kPageSize;
                remapped.push_back(pg);
            } else {
                s.mapped -= kPageSize;
            }
        }
        return true;
    };

    for (size_t p = begin; p < end; p += kPageSize) {
        const RexGmAccess antes = AccessOf(s, p);
        const bool estaba = (antes == REX_GM_WRITE);

        if (estaba != debe_estar) {
            /*
             * The view is looked up per page: a range can cross the boundary
             * between two views with different offsets.
             */
            if (const View* v = ViewFor(s, wbase + p)) {
                const size_t off_mapeo =
                    v->offset + (p - static_cast<size_t>(v->base - s.base));
                size_t vi = 0;
                Chunk* c = ChunkAt(s, off_mapeo);
                /*
                 * If the chunk has not entered this view yet, there is nothing to map
                 * or unmap: the permission is recorded and the first access will bring
                 * the chunk in with the right permission already.
                 */
                const bool en_vista = c != nullptr && ViewIndexFor(s, wbase + p, &vi) &&
                                      s.view_mapped.count({vi, c->offset}) != 0;
                if (en_vista) {
                    const u64 src = reinterpret_cast<u64>(
                        static_cast<uint8_t*>(c->shadow) + (off_mapeo - c->offset));
                    /*
                     * Permissions are tried first, which keeps the page readable. If the
                     * kernel refuses, it falls back to the usual unmap. ProtegerConPermisos
                     * remembers whether it works.
                     */
                    if (!ProtegerConPermisos(s.base + p, estaba)) {
                        const bool map = !estaba;
                        const bool extends = run.active && run.map == map && run.win + run.len == p &&
                                             run.src + run.len == src;
                        if (!extends) {
                            if (!flush()) return false;
                            run.active = true;
                            run.map = map;
                            run.win = p;
                            run.src = src;
                            run.len = 0;
                            run.old.clear();
                        }
                        run.len += kPageSize;
                        run.old.emplace_back(p, antes);
                    }
                }
                /*
                 * Not committed: nothing to map, but the permission is still recorded
                 * and RexGmCommit will honor it on commit.
                 */
            }
        }

        if (debe_estar) s.protection.erase(p);
        else s.protection[p] = access;
    }
    if (!flush()) return false;
    /* Pages of the same run would repeat the work: once per run is enough (CoalesceViewRun skips runs that
     * are already one block). */
    for (size_t p : remapped) CoalesceViewRun(s, p);
    return true;
}

bool RexGmQueryProtect(uint8_t* address, size_t* length, RexGmAccess* out_access) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.base) return false;

    const uint64_t addr  = reinterpret_cast<uint64_t>(address);
    const uint64_t wbase = reinterpret_cast<uint64_t>(s.base);
    if (addr < wbase || addr >= wbase + s.size) return false;

    const size_t begin = AlignDown(static_cast<size_t>(addr - wbase), kPageSize);

    /*
     * An uncommitted page is reported as having no access, even if its logical
     * permission is the default one.
     *
     * This matters a lot: after a fault, the SDK's MMIO handler queries this, and if
     * it sees the page as accessible it concludes that another thread already
     * unprotected it and retries the instruction. If the page does not really exist,
     * the retry faults again... forever. A silent hang.
     */
    if (!IsCommittedWindowAddress(s, wbase + begin)) {
        if (out_access) *out_access = REX_GM_NONE;
        if (length) *length = kPageSize;
        return true;
    }

    const RexGmAccess a = AccessOf(s, begin);
    if (out_access) *out_access = a;

    /*
     * On input *length is the maximum to walk (0 = one page); on output, the size of
     * the committed range with the same permission.
     */
    size_t maximo = (length && *length) ? *length : kPageSize;
    if (begin + maximo > s.size) maximo = s.size - begin;

    size_t p = begin;
    const size_t tope = begin + maximo;
    while (p < tope && AccessOf(s, p) == a && IsCommittedWindowAddress(s, wbase + p))
        p += kPageSize;
    if (length) *length = p - begin;
    return true;
}

void* RexGmShadowFor(uint64_t window_address) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.base) return nullptr;

    const View* v = ViewFor(s, window_address);
    if (!v) return nullptr;

    const size_t off_mapeo =
        v->offset + static_cast<size_t>(window_address - reinterpret_cast<uint64_t>(v->base));
    Chunk* c = ChunkAt(s, off_mapeo);
    if (!c) return nullptr;

    return static_cast<uint8_t*>(c->shadow) + (off_mapeo - c->offset);
}

}  // extern "C"

#endif /* __SWITCH__ */
