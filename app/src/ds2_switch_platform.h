// ds2 - small Switch-only queries kept out of the files that include rex headers (switch.h typedefs clash).
#pragma once
#include <cstdint>
#include <string>

namespace ds2::platform {
// Process memory as Horizon counts it (svcGetInfo UsedMemorySize / TotalMemorySize). false off the Switch.
bool HostMemory(uint64_t* used, uint64_t* total);
// Guest backing committed / mapped by the SDK's Switch guest memory (0 off the Switch).
uint64_t GuestCommitted();
uint64_t GuestMapped();
// Kernel-side accounting that can run out before memory does (2001-0103 "resource exhausted"):
//   blocks       memory blocks in this process's address space (svcQueryMemory walk; each separately mapped or
//                protected range is one block, and the kernel's block objects are a limited resource)
//   sysres_*     InfoType_SystemResourceSizeUsed/Total (page tables and memory management of the process)
//   last_gm      last result code of the SDK's guest-memory mapper (RexGmLastResult)
struct KernelStats {
  uint64_t blocks = 0, blocks_inaccessible = 0, sysres_used = 0, sysres_total = 0;
  uint32_t last_gm = 0;
  uint64_t coalesced = 0, coalesce_failures = 0;  // SDK view-run merges (guest_memory_switch.cpp)
  // 2 MB windows of the address space that hold any mapping (upper bound of level-3 page tables) and windows cut by
  // a block edge (they need a page table whatever the physical layout: lower bound).
  uint64_t pt_windows_mapped = 0, pt_windows_split = 0;
  uint64_t chunks = 0;  // committed guest-memory chunks (each: heap backing + code alias + lazy view mappings)
  bool valid = false;
};
KernelStats QueryKernelStats();
// The same walk, as text: blocks grouped by (inside/outside the guest window, memory type, permission, attribute),
// largest groups first. For the 10 s snapshots, to see which kind of mapping multiplies.
std::string KernelBlockHistogram(size_t max_groups);

// How many more kernel memory blocks this process can still create right now: changes the cache attribute of
// every other 4 KB page of a private 8 MB scratch buffer, one call each, until a call fails or max_pages is
// reached, then restores the whole buffer with one call. Each changed page costs the kernel what any small
// mapping change costs (memory blocks, and a page table for the region). Returns the pages that succeeded;
// *fail_rc gets the first failure (0 if none). Switch only; 0 elsewhere.
uint32_t ProbeKernelHeadroom(uint32_t max_pages, uint32_t* fail_rc);

// How many more page tables the kernel can still hand to this process right now (up to max_tables, at most 128):
// maps single pages into separate empty 2 MB windows, then unmaps them. Without a system resource of its own
// (sysres 0/0, the case under hbloader) the process shares that page-table heap with all system processes, and
// running it dry fails every mapping system-wide with 2001-0103. *fail_rc: first failure, 0 if none.
uint32_t ProbePageTableHeadroom(uint32_t max_tables, uint32_t* fail_rc);

// Process resource limits as "name cur/max" pairs (memory in MB), or "" if unavailable.
std::string ResourceLimits();
// Guest memory commit granule (RexGmSetCommitGranule). Must run before the guest memory is created. Returns the
// granule in effect (0 off the Switch).
size_t SetCommitGranule(size_t bytes);
// libnx's transfer memory for nvservices (__nx_nv_transfermem_size): the area where nvdrv keeps the bookkeeping of
// every GPU memory object. mb <= 0 leaves libnx's default (8 MB). Must run before Vulkan is created. Returns MB now.
uint32_t SetNvTransferMemMb(int mb);

// SDK bridge that publishes FPS to SaltyNX overlays (sdk/src/ui/switch_saltynx.cpp). No-op off the Switch.
void SetSaltyNxBridge(bool enabled);
}  // namespace ds2::platform
