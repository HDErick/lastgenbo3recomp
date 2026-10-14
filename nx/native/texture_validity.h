// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
namespace bo3::native {
struct TextureValidity {
  uint32_t base=0,base_bytes=0,mips=0,mip_bytes=0;
  bool clean=false;
  bool watched=false;
  void Invalidate(uint32_t address,uint32_t bytes) {
    const auto intersects=[&](uint32_t start,uint32_t length) {
      return bytes && length && address<uint64_t(start)+length && uint64_t(address)+bytes>start;
    };
    if(intersects(base,base_bytes) || intersects(mips,mip_bytes)){clean=false;watched=false;}
  }
};
}
