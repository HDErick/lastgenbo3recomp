// SPDX-License-Identifier: GPL-3.0-only
// Bounded copy profiles for exact BO3 programs, validated against GPU output.
#pragma once
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <vector>
namespace bo3::native {
struct CopyRange { uint32_t source, destination, bytes; };
struct CopyPlan { uint32_t source = 0, source_bytes = 0, destination = 0, destination_bytes = 0; std::vector<CopyRange> ranges; };
inline CopyPlan PlanMemexport(const uint32_t* r, std::string_view sha, uint32_t primitive, uint32_t count, bool indexed, bool major) {
  auto require = [](bool valid, const char* reason) { if (!valid) throw std::runtime_error(reason); };
  uint32_t blocks = 0, stride = 0, multiplier = 0;
  if (sha == "0c9ced036fbfff79ae9c6c9d83eb256c58aa57e98d7c504e1699f0430182e490") { blocks = 4; stride = 64; multiplier = 32; }
  if (sha == "d4991999678dd7e26c2c876e06b078cc87b875f5addf173bfdb1beb64eb00bf5") { blocks = 2; stride = 16; multiplier = 8; }
  require(blocks != 0, "unreviewed_memexport_program");
  require(primitive == 1 && !indexed && !major, "memexport_primitive_or_indexing");
  require(((r[0x21fc] >> 6) & 3) == 2 && ((r[0x21fc] >> 8) & 3) == 0, "memexport_draw_initiator");
  require(count && count <= 65535 && (r[0x21fc] >> 16) == count, "memexport_count");
  require(r[0x2102] == 0 && r[0x2101] == 0 && r[0x2100] >= count - 1, "memexport_index_offset_or_clamp");
  require(((r[0x2180] >> 24) & 7) == 7 && (r[0x2307] & 511) == 0, "memexport_program_state");
  const uint32_t* c0 = r + 0x4000;
  require((c0[0] >> 30) == 1 && c0[1] == 0x4b000000 && (c0[2] & 0xffff3f07) == 0x4b021a01 && (c0[3] >> 23) == 0x96, "memexport_stream_profile");
  const uint32_t* c255 = r + 0x4000 + 255 * 4;
  require(c255[0] == 0 && c255[1] == 0x3f800000 && c255[2] == 0x40800000 && c255[3] == 0x41000000, "memexport_index_constants");
  if (blocks == 4) require(r[0x4000 + 254 * 4] == 0x42000000 && r[0x4000 + 254 * 4 + 1] == 0x41400000, "memexport_block_constants");
  const uint32_t vf0 = r[0x4800 + 95 * 2], vf1 = r[0x4800 + 95 * 2 + 1];
  require(((vf0 & 3) == 2 || (vf0 & 3) == 3) && (vf1 & 3) == 2, "memexport_vertex_fetch_profile");
  CopyPlan plan; plan.source = vf0 & ~3u; plan.source_bytes = ((vf1 >> 2) & 0xffffff) * 4; plan.destination = c0[0] << 2;
  uint32_t elements = c0[3] & 0x7fffff; plan.destination_bytes = elements * 8;
  require(uint64_t(plan.source) + plan.source_bytes <= 0x20000000 && uint64_t(plan.destination) + plan.destination_bytes <= 0x20000000, "memexport_physical_range");
  if (blocks == 2) {
    // These two 32-byte blocks cover a contiguous 64 bytes per invocation.
    // Apply export bounds once rather than walking every element.
    uint32_t bytes = uint32_t(std::min(uint64_t(count) * 64, uint64_t(elements) * 8));
    require(bytes <= plan.source_bytes, "memexport_source_bounds");
    if (bytes) plan.ranges.push_back({0, 0, bytes});
  } else {
  const uint32_t active = std::min(count, (elements + multiplier - 1) / multiplier);
  for (uint32_t invocation = 0; invocation < active; ++invocation) for (uint32_t block = 0; block < blocks; ++block) {
    const uint32_t element = invocation * multiplier + block * 4;
    const uint32_t length = element < elements ? std::min(4u, elements - element) * 8 : 0;
    if (!length) continue;
    uint32_t source = invocation * stride * 4 + block * 32, target = element * 8;
    require(uint64_t(source) + length <= plan.source_bytes, "memexport_source_bounds");
    if (!plan.ranges.empty() && plan.ranges.back().source + plan.ranges.back().bytes == source && plan.ranges.back().destination + plan.ranges.back().bytes == target) plan.ranges.back().bytes += length;
    else plan.ranges.push_back({source, target, length});
  }
  }
  if (!plan.ranges.empty()) {
    auto last = plan.ranges.back();
    require(last.source + last.bytes <= 16 * 1024 * 1024 && last.destination + last.bytes <= 16 * 1024 * 1024, "memexport_snapshot_budget");
    require(uint64_t(plan.source) + last.source + last.bytes <= plan.destination || uint64_t(plan.destination) + last.destination + last.bytes <= plan.source, "memexport_overlap");
  }
  return plan;
}
}  // namespace bo3::native
