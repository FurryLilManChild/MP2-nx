// mp2-nx: memory below 4 GiB for Echoes' REL module arena on Horizon.
//
// The REL runtime (extern/echoes/platform/rel.cpp) relocates module images with the
// GameCube's 32-bit addresses, so the arena that holds them must sit below 4 GiB. On
// the PC it asks mmap or VirtualAlloc for that. Horizon has neither: a homebrew
// process gets its heap wherever the kernel put the heap region. What it can do is map
// heap pages a second time at an address of its choosing, the way libnx's Jit and
// Alek's mod loader do: svcMapProcessCodeMemory on our own process handle places the
// pages at a free address we picked, and svcSetProcessMemoryPermission makes them
// read-write. The heap copy is unusable while mapped, so it is only kept to unmap.
#include <switch.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace {
struct Mapping {
  uintptr_t dst;
  void* src;
  size_t size;
};
std::mutex g_mutex;
std::vector<Mapping> g_mappings;

constexpr uint64_t kFourGiB = 0x100000000ull;
constexpr uint64_t kLowest = 0x10000000ull;  // nothing below 256 MiB, as the PC fallback
constexpr uint64_t kAlign = 0x200000ull;      // 2 MiB: Horizon's large-page size

// Whether [addr, addr + size) is one unmapped region.
bool range_free(uint64_t addr, uint64_t size) {
  MemoryInfo info{};
  u32 page = 0;
  if (R_FAILED(svcQueryMemory(&info, &page, addr))) {
    return false;
  }
  return (info.type & MemState_Type) == MemType_Unmapped && info.addr <= addr &&
         info.addr + info.size >= addr + size;
}

// The lowest free kAlign-aligned gap of `size` bytes that ends below 4 GiB, or 0.
uint64_t find_low_gap(uint64_t size) {
  uint64_t addr = kLowest;
  while (addr + size <= kFourGiB) {
    MemoryInfo info{};
    u32 page = 0;
    if (R_FAILED(svcQueryMemory(&info, &page, addr))) {
      return 0;
    }
    const uint64_t end = info.addr + info.size;
    if ((info.type & MemState_Type) == MemType_Unmapped) {
      const uint64_t start = (addr + kAlign - 1) & ~(kAlign - 1);
      if (start + size <= end && start + size <= kFourGiB) {
        return start;
      }
    }
    if (end <= addr) {
      return 0;
    }
    addr = end;
  }
  return 0;
}
} // namespace

extern "C" void* mp2nx_low_arena_alloc(size_t size, uint32_t preferredBase) {
  if (size == 0) {
    return nullptr;
  }
  size = (size + 0xFFF) & ~static_cast<size_t>(0xFFF);
  const Handle self = envGetOwnProcessHandle();
  void* src = std::aligned_alloc(0x1000, size);
  if (src == nullptr) {
    std::fprintf(stderr, "mp2-nx: REL arena: no heap for %zu bytes\n", size);
    return nullptr;
  }
  std::memset(src, 0, size);

  std::lock_guard lock{g_mutex};
  virtmemLock();
  uint64_t dst = 0;
  if (preferredBase != 0 && (preferredBase & 0xFFF) == 0 && preferredBase + size <= kFourGiB &&
      range_free(preferredBase, size)) {
    dst = preferredBase;
  } else {
    dst = find_low_gap(size);
  }
  Result rc = dst != 0 ? svcMapProcessCodeMemory(self, dst, reinterpret_cast<uint64_t>(src), size) : MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
  if (R_SUCCEEDED(rc)) {
    rc = svcSetProcessMemoryPermission(self, dst, size, Perm_Rw);
    if (R_FAILED(rc)) {
      svcUnmapProcessCodeMemory(self, dst, reinterpret_cast<uint64_t>(src), size);
    }
  }
  virtmemUnlock();
  if (R_FAILED(rc)) {
    std::fprintf(stderr, "mp2-nx: REL arena: could not map %zu bytes below 4 GiB at 0x%llx (rc 0x%x)\n", size,
                 static_cast<unsigned long long>(dst), static_cast<unsigned>(rc));
    std::free(src);
    return nullptr;
  }
  g_mappings.push_back({static_cast<uintptr_t>(dst), src, size});
  std::fprintf(stderr, "mp2-nx: REL arena: %zu bytes at 0x%llx\n", size, static_cast<unsigned long long>(dst));
  return reinterpret_cast<void*>(dst);
}

extern "C" void mp2nx_low_arena_free(void* base) {
  std::lock_guard lock{g_mutex};
  for (auto it = g_mappings.begin(); it != g_mappings.end(); ++it) {
    if (it->dst != reinterpret_cast<uintptr_t>(base)) {
      continue;
    }
    const Handle self = envGetOwnProcessHandle();
    // Back to the code-static state the unmap expects, then hand the pages back to the heap.
    svcSetProcessMemoryPermission(self, it->dst, it->size, Perm_R);
    if (R_SUCCEEDED(svcUnmapProcessCodeMemory(self, it->dst, reinterpret_cast<uint64_t>(it->src), it->size))) {
      std::free(it->src);
    }
    g_mappings.erase(it);
    return;
  }
}
