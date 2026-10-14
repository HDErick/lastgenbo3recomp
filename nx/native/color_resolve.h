// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <rex/graphics/util/draw.h>
#include <rex/ui/vulkan/api.h>
#include <vector>
#include <stdexcept>
namespace bo3::native {
struct ColorResolveError : std::runtime_error { using std::runtime_error::runtime_error; };
struct ResolveRect { uint32_t x,y,width,height; };
struct ColorResolvePlan {
  ResolveRect source, destination;
  uint32_t width,height,address,extent_start,extent_bytes,guest_format,endian,samples;
  int32_t exponent;
  bool swap_rb;
  VkFormat format;
};
ColorResolvePlan PlanColorResolve(const rex::graphics::draw_util::ResolveInfo& info,
                                  uint32_t surface_pitch,uint32_t raw_destination);
ResolveRect ColorResolveSourceRectangle(const rex::graphics::draw_util::ResolveInfo& info);
bool ResolveRectContains(ResolveRect outer,ResolveRect inner);
// Coverage is semantic: zero-initialized padding is never declared guest data.
void AddResolveCoverage(std::vector<ResolveRect>& coverage,ResolveRect rectangle);
}
