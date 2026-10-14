// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <rex/system/xmemory.h>

namespace bo3::native {
// Physical reads/writes use the SDK's direct physical mapping. Allocation,
// decommit and protection changes update this parent heap under its mutex.
// No permission cache survives between calls. The staged QueryRangeAccess
// checks commitment/protection under one lock and visits only requested pages.
// QueryRegionInfo scans to the end of the allocation, even for a tiny read.
inline bool PhysicalAccessible(rex::memory::BaseHeap& heap, uint32_t address,
                               uint32_t size, bool writable = false) {
  const uint64_t end = uint64_t(address) + size;
  if (!size || address < heap.heap_base() ||
      end > uint64_t(heap.heap_base()) + heap.heap_size()) return false;
  const auto access=heap.QueryRangeAccess(address,uint32_t(end-1));
  return access==rex::memory::PageAccess::kReadWrite ||
      (!writable && access==rex::memory::PageAccess::kReadOnly);
}
}  // namespace bo3::native
