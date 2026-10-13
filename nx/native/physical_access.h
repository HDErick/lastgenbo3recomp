// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <rex/system/xmemory.h>

namespace bo3::native {
// Physical reads/writes use the SDK's direct physical mapping. Allocation,
// decommit and protection changes update this parent heap under its mutex.
// No permission cache survives between calls. Region sizes start at the first
// containing page, so subtract the initial byte offset before advancing.
inline bool PhysicalAccessible(rex::memory::BaseHeap& heap, uint32_t address,
                               uint32_t size, bool writable = false) {
  const uint64_t end = uint64_t(address) + size;
  if (!size || address < heap.heap_base() ||
      end > uint64_t(heap.heap_base()) + heap.heap_size()) return false;
  uint64_t at = address;
  while (at < end) {
    rex::memory::HeapAllocationInfo info{};
    if (!heap.QueryRegionInfo(uint32_t(at), &info) ||
        !(info.state & rex::memory::kMemoryAllocationCommit) ||
        !(info.protect & rex::memory::kMemoryProtectRead) ||
        (writable && !(info.protect & (rex::memory::kMemoryProtectWrite |
                                      rex::memory::kMemoryProtectWriteCombine)))) return false;
    const uint64_t page_start = heap.heap_base() +
        ((at - heap.heap_base()) / heap.page_size()) * heap.page_size();
    const uint64_t next = page_start + info.region_size;
    if (next <= at) return false;
    at = next;
  }
  return true;
}
}  // namespace bo3::native
