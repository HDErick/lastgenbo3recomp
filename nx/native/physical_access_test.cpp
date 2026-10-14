// SPDX-License-Identifier: GPL-3.0-only
#include "physical_access.h"
#include <cstdio>

// Exercise the actual staged SDK QueryRangeAccess, including bounded extents,
// against committed, reserved, read-only and inaccessible page entries.
class HeapFixture : public rex::memory::BaseHeap {
 public:
  HeapFixture() {
    Initialize(nullptr, nullptr, rex::memory::HeapType::kGuestPhysical, 0, 0x4000, 0x1000);
    for (auto& page : page_table_) {
      page.qword = 0; page.base_address = 0; page.region_page_count = 4;
      page.state = rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit;
      page.current_protect = rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite;
    }
  }
  void ReadOnly() { page_table_[1].current_protect = rex::memory::kMemoryProtectRead; }
  void Reserved() { page_table_[2].state = rex::memory::kMemoryAllocationReserve; }
  void NoAccess() { page_table_[3].current_protect = 0; }
};
int main() {
  using bo3::native::PhysicalAccessible;
  HeapFixture heap;
  int failures = 0;
  auto check = [&](bool value, const char* name) { if (!value) { std::printf("FAIL: %s\n", name); ++failures; } };
  check(PhysicalAccessible(heap, 0xfff, 2, true), "unaligned committed boundary");
  check(PhysicalAccessible(heap, 0x3fff, 1, true), "last valid byte");
  check(!PhysicalAccessible(heap, 0x4000, 1), "exclusive heap end");
  check(!PhysicalAccessible(heap, 0xffffffff, 2), "overflow");
  check(!PhysicalAccessible(heap, 0, 0), "empty span");
  heap.ReadOnly();
  check(PhysicalAccessible(heap, 0xfff, 2), "read-only boundary permits reads");
  check(!PhysicalAccessible(heap, 0xfff, 2, true), "unaligned boundary rejects writes");
  heap.Reserved();
  check(!PhysicalAccessible(heap, 0x1fff, 2), "reserved page with protection still rejects reads");
  heap.NoAccess();
  check(!PhysicalAccessible(heap, 0x3000, 1), "inaccessible committed page");
  check(PhysicalAccessible(heap, 0, 0x1000, true), "exact span excludes following read-only page");
  std::printf("Physical allocation guards: %s (10 cases)\n", failures ? "FAILED" : "PASS");
  return failures ? 1 : 0;
}
