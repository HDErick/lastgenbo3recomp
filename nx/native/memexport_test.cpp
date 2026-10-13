// SPDX-License-Identifier: GPL-3.0-only
// Exercise the actual native planner against captured GPU byte output.
#include "memexport_copy.h"
#include "bc_decode.h"
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
static std::vector<uint8_t> Read(const char* name) {
  std::ifstream f(name, std::ios::binary); if (!f) throw std::runtime_error("input missing");
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
int main(int argc, char** argv) {
  try {
    if (argc == 1) {
      uint8_t block[16]{}, rgba[64]{};
      block[0] = 0; block[1] = 0xf8; block[2] = 0xe0; block[3] = 7; block[4] = 0xe4;
      bo3::native::DecodeBC(block, 1, rgba);
      const uint8_t colors[]{255,0,0,255, 0,255,0,255, 170,85,0,255, 85,170,0,255};
      if (std::memcmp(rgba, colors, 16)) return 1;
      block[0] = block[1] = 0; block[2] = block[3] = 255; block[4] = 255;
      bo3::native::DecodeBC(block, 1, rgba); if (rgba[3] != 0) return 1;
      for (uint32_t i = 0; i < 8; ++i) block[i] = uint8_t((i * 2 + 1) * 16 + i * 2);
      block[8] = 0; block[9] = 0xf8; block[10] = 0xe0; block[11] = 7;
      block[12] = block[13] = block[14] = block[15] = 0;
      bo3::native::DecodeBC(block, 2, rgba);
      for (uint32_t i = 0; i < 16; ++i) if (rgba[i * 4] != 255 || rgba[i * 4 + 3] != i * 17) return 1;
      uint64_t selectors = 0; for (uint32_t i = 0; i < 16; ++i) selectors |= uint64_t(i & 7) << (i * 3);
      for (uint32_t i = 0; i < 6; ++i) block[i + 2] = uint8_t(selectors >> (i * 8));
      block[0] = 255; block[1] = 0;
      bo3::native::DecodeBC(block, 3, rgba);
      const uint8_t alpha7[]{255,0,218,182,145,109,72,36};
      for (uint32_t i = 0; i < 16; ++i) if (rgba[i * 4 + 3] != alpha7[i & 7]) return 1;
      block[0] = 0; block[1] = 255;
      bo3::native::DecodeBC(block, 3, rgba);
      const uint8_t alpha5[]{0,255,51,102,153,204,0,255};
      for (uint32_t i = 0; i < 16; ++i) if (rgba[i * 4 + 3] != alpha5[i & 7]) return 1;
      std::cout << "BC1 color/transparency, BC2 explicit alpha and both BC3 alpha modes passed\n";
      return 0;
    }
    if (argc != 7) return 2;
    auto regs_bytes = Read(argv[1]), input = Read(argv[2]), before = Read(argv[3]), output = Read(argv[4]);
    if (regs_bytes.size() != 0x5003 * 4) return 2;
    std::vector<uint32_t> regs(0x5003); std::memcpy(regs.data(), regs_bytes.data(), regs_bytes.size());
    auto plan = bo3::native::PlanMemexport(regs.data(), argv[5], 1, std::stoul(argv[6]), false, false);
    if (input.size() != plan.source_bytes || before.size() != plan.destination_bytes || output.size() != before.size()) return 2;
    for (auto range : plan.ranges) std::memcpy(before.data() + range.destination, input.data() + range.source, range.bytes);
    if (before != output) { std::cerr << "Native copy differs from completed GPU output\n"; return 1; }
    auto refuses = [&](uint32_t index, uint32_t value) { auto changed = regs; changed[index] = value; try { bo3::native::PlanMemexport(changed.data(), argv[5], 1, std::stoul(argv[6]), false, false); } catch (const std::exception&) { return true; } return false; };
    if (!refuses(0x2102, 1) || !refuses(0x4002, 0) || !refuses(0x4800 + 95 * 2 + 1, 2)) return 1;
    std::cout << "Native copy matches GPU bytes; offset/format/bounds rejection passed\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 2; }
}
