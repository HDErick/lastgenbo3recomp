// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <rex/graphics/xenos.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/ui/vulkan/api.h>
#include <span>
#include <stdexcept>
#include <vector>

namespace bo3::native {
struct TextureUploadError : std::runtime_error { using std::runtime_error::runtime_error; };
struct TextureSlice {
  uint32_t mip, layer, width, height, depth, output_offset;
  uint32_t source_offset, x_blocks, y_blocks, z, row_pitch, z_rows;
  bool from_mips;
};
struct TextureUploadPlan {
  uint32_t kind, width, height, depth, layers, levels, min_level, max_level;
  uint32_t bytes_per_block, block_width, bc, output_bytes;
  uint32_t base_address, mip_address, base_bytes, mip_bytes;
  VkFormat format;
  rex::graphics::xenos::xe_gpu_texture_fetch_t fetch;
  std::vector<TextureSlice> slices;
};
bool TextureDimensionsCompatible(const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch, uint32_t kind);
// kind: 0=2D, 1=cube, 2=3D, matching the translated shader ABI.
TextureUploadPlan PlanTextureUpload(const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch,
                                    uint32_t kind, uint32_t limit);
std::vector<uint8_t> DecodeTextureUpload(const TextureUploadPlan& plan,
                                        std::span<const uint8_t> base,
                                        std::span<const uint8_t> mips);
}
