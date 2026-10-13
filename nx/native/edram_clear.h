// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include <stdexcept>
namespace bo3::native {
struct ClearRectangle { uint32_t x, y, width, height; };
// Pixel-center coverage under the top-left rule. Include a leading edge at
// .5, exclude a trailing edge at .5, after the SDK's 16.8 nearest-even rounding.
inline int32_t ClearPixelBoundary(int32_t fixed16p8) {
  const int64_t value = int64_t(fixed16p8) + 127;
  return int32_t(value >= 0 ? value / 256 : -((-value + 255) / 256));
}
inline uint32_t SampleX(uint32_t log2_samples) { return log2_samples >= 2 ? 2 : 1; }
inline uint32_t SampleY(uint32_t log2_samples) { return log2_samples >= 1 ? 2 : 1; }
inline ClearRectangle ToSampleRectangle(ClearRectangle r, uint32_t log2_samples) {
  if (log2_samples > 2) throw std::runtime_error("clear sample count");
  return {r.x * SampleX(log2_samples), r.y * SampleY(log2_samples),
          r.width * SampleX(log2_samples), r.height * SampleY(log2_samples)};
}
inline ClearRectangle FromSampleRectangle(ClearRectangle r, uint32_t sample_pitch,
                                         uint32_t pixel_pitch, uint32_t log2_samples) {
  if (log2_samples > 2 || uint64_t(pixel_pitch) * SampleX(log2_samples) != sample_pitch)
    throw std::runtime_error("clear alias pitch");
  const uint32_t sx = SampleX(log2_samples), sy = SampleY(log2_samples);
  if (r.x % sx || r.y % sy || r.width % sx || r.height % sy)
    throw std::runtime_error("clear alias partial sample");
  return {r.x / sx, r.y / sy, r.width / sx, r.height / sy};
}
}
