// SPDX-License-Identifier: GPL-3.0-only
// BC1/2/3 decode for the initial native CPU upload path. Format reference:
// https://learn.microsoft.com/en-us/windows/win32/direct3d10/d3d10-graphics-programming-guide-resources-block-compression
#pragma once
#include <array>
#include <cstdint>
namespace bo3::native {
inline void DecodeBC(const uint8_t* block, uint32_t kind, uint8_t* rgba) {
  const uint8_t* color = block + (kind == 1 ? 0 : 8);
  uint32_t c0 = color[0] | (uint32_t(color[1]) << 8), c1 = color[2] | (uint32_t(color[3]) << 8);
  std::array<std::array<uint8_t, 4>, 4> palette{};
  auto rgb565 = [](uint32_t c) { uint32_t r = c >> 11, g = (c >> 5) & 63, b = c & 31; return std::array<uint8_t, 4>{uint8_t((r << 3) | (r >> 2)), uint8_t((g << 2) | (g >> 4)), uint8_t((b << 3) | (b >> 2)), 255}; };
  palette[0] = rgb565(c0); palette[1] = rgb565(c1);
  if (kind != 1 || c0 > c1) {
    for (uint32_t c = 0; c < 3; ++c) { palette[2][c] = uint8_t((2 * palette[0][c] + palette[1][c]) / 3); palette[3][c] = uint8_t((palette[0][c] + 2 * palette[1][c]) / 3); }
    palette[2][3] = palette[3][3] = 255;
  } else {
    for (uint32_t c = 0; c < 3; ++c) palette[2][c] = uint8_t((palette[0][c] + palette[1][c]) / 2);
    palette[2][3] = 255; palette[3] = {0, 0, 0, 0};
  }
  uint32_t indices = color[4] | (uint32_t(color[5]) << 8) | (uint32_t(color[6]) << 16) | (uint32_t(color[7]) << 24);
  std::array<uint8_t, 8> alpha{}; uint64_t alpha_indices = 0;
  if (kind == 3) {
    alpha[0] = block[0]; alpha[1] = block[1];
    if (alpha[0] > alpha[1]) for (uint32_t a = 2; a < 8; ++a) alpha[a] = uint8_t(((8 - a) * alpha[0] + (a - 1) * alpha[1]) / 7);
    else { for (uint32_t a = 2; a < 6; ++a) alpha[a] = uint8_t(((6 - a) * alpha[0] + (a - 1) * alpha[1]) / 5); alpha[6] = 0; alpha[7] = 255; }
    for (uint32_t b = 0; b < 6; ++b) alpha_indices |= uint64_t(block[2 + b]) << (8 * b);
  }
  for (uint32_t pixel = 0; pixel < 16; ++pixel) {
    auto value = palette[(indices >> (2 * pixel)) & 3];
    if (kind == 2) value[3] = uint8_t(((block[pixel / 2] >> ((pixel & 1) * 4)) & 15) * 17);
    if (kind == 3) value[3] = alpha[(alpha_indices >> (3 * pixel)) & 7];
    for (uint32_t c = 0; c < 4; ++c) rgba[pixel * 4 + c] = value[c];
  }
}
}  // namespace bo3::native
