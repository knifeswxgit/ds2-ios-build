// Host-side stand-in for the parts of libnx that sdk/src/core/guest_memory_switch.cpp uses, backed by a simulated
// Horizon address space (gm_sim.cpp). Only for testing the guest-memory logic off the console.
#pragma once
#include <cstddef>
#include <cstdint>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64; typedef int64_t s64;
typedef u32 Result; typedef u32 Handle;
#define R_FAILED(r) ((r) != 0)
#define R_SUCCEEDED(r) ((r) == 0)
enum { Perm_None = 0, Perm_R = 1, Perm_W = 2, Perm_X = 4, Perm_Rw = 3, Perm_Rx = 5 };
enum { MemType_Unmapped = 0, MemType_Heap = 5, MemType_ModuleCodeMutable = 9, MemType_ProcessMem = 0x0F };
typedef struct { u64 addr, size; u32 type, attr, perm, ipc_refcount, device_refcount, padding; } MemoryInfo;
typedef struct VirtmemReservation VirtmemReservation;
Handle envGetOwnProcessHandle(void);
Result svcMapProcessCodeMemory(Handle proc, u64 dst, u64 src, u64 size);
Result svcUnmapProcessCodeMemory(Handle proc, u64 dst, u64 src, u64 size);
Result svcSetProcessMemoryPermission(Handle proc, u64 addr, u64 size, u32 perm);
Result svcMapProcessMemory(void* dst, Handle proc, u64 src, u64 size);
Result svcUnmapProcessMemory(void* dst, Handle proc, u64 src, u64 size);
Result svcQueryMemory(MemoryInfo* mi, u32* pi, u64 addr);
void virtmemLock(void); void virtmemUnlock(void);
void* virtmemFindAslr(size_t size, size_t guard);
void* virtmemFindCodeMemory(size_t size, size_t guard);
VirtmemReservation* virtmemAddReservation(void* mem, size_t size);
void virtmemRemoveReservation(VirtmemReservation* rv);
