// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>
#include <utility>
namespace bo3::native {
// GPU resolve output unavailable in the CPU shadow. Native GPU readers must
// prove coverage separately. Intervals include SDK destination padding; verified
// CPU export writes release only their bytes, fault callbacks release none.
class UnavailableResolveRanges {
 public:
  struct Range { uint32_t begin, end; };
  const std::vector<Range>& ranges() const { return ranges_; }
  bool Intersects(uint32_t address, uint32_t size) const {
    const uint64_t end=uint64_t(address)+size;
    if (end>0x20000000) throw std::runtime_error("resolve ownership query bounds");
    if (!size) return false;
    return std::any_of(ranges_.begin(),ranges_.end(),[&](Range r){return address<r.end && end>r.begin;});
  }
  void Add(uint32_t address,uint32_t size) {
    const uint64_t end=uint64_t(address)+size;
    if (end>0x20000000) throw std::runtime_error("resolve ownership destination bounds");
    if (!size) return;
    std::vector<Range> next=ranges_; next.push_back({address,uint32_t(end)});
    std::sort(next.begin(),next.end(),[](Range a,Range b){return a.begin<b.begin;});
    std::vector<Range> merged;
    for (auto r:next) {
      if (!merged.empty() && merged.back().end>=r.begin) merged.back().end=std::max(merged.back().end,r.end);
      else merged.push_back(r);
    }
    if (merged.size()>512) throw std::runtime_error("resolve ownership range limit");
    ranges_=std::move(merged);
  }
  void CpuWritten(uint32_t address,uint32_t size) {
    const uint64_t end=uint64_t(address)+size;
    if (end>0x20000000) throw std::runtime_error("resolve ownership CPU write bounds");
    if (!size) return;
    std::vector<Range> next;
    for (auto r:ranges_) {
      if (address>=r.end || end<=r.begin) {next.push_back(r);continue;}
      if (r.begin<address) next.push_back({r.begin,address});
      if (end<r.end) next.push_back({uint32_t(end),r.end});
    }
    if (next.size()>512) throw std::runtime_error("resolve ownership range limit");
    ranges_=std::move(next);
  }
 private:
  std::vector<Range> ranges_;
};
}
