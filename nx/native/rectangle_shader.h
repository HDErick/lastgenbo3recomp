// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include <vector>
namespace bo3::native {
// Geometry expansion of three Xenos rectangle vertices into a four-vertex
// strip. Inputs/outputs contain only the actual VS interpolator locations.
std::vector<uint32_t> BuildRectangleShader(uint32_t interpolators);
}
