// SPDX-License-Identifier: GPL-3.0-only
#include "color_resolve.h"
#include <rex/graphics/pipeline/texture/util.h>
#include <algorithm>
#include <bit>
namespace bo3::native {
using namespace rex::graphics;
bool ResolveRectContains(ResolveRect a,ResolveRect b) {
  return b.x>=a.x && b.y>=a.y && uint64_t(b.x)+b.width<=uint64_t(a.x)+a.width && uint64_t(b.y)+b.height<=uint64_t(a.y)+a.height;
}
void AddResolveCoverage(std::vector<ResolveRect>& coverage,ResolveRect r) {
  if(!r.width || !r.height) return;
  if(std::any_of(coverage.begin(),coverage.end(),[&](ResolveRect a){return ResolveRectContains(a,r);})) return;
  std::erase_if(coverage,[&](ResolveRect a){return ResolveRectContains(r,a);});
  if(coverage.size()>=64) throw ColorResolveError("resolve_coverage_limit");
  coverage.push_back(r);
}
ResolveRect ColorResolveSourceRectangle(const draw_util::ResolveInfo& info) {
  const auto edram=info.color_edram_info;
  if(info.IsCopyingDepth() || !edram.pitch_tiles || uint32_t(edram.msaa_samples)>2) throw ColorResolveError("resolve_color_source_state");
  const uint32_t offset=(edram.base_tiles+xenos::kEdramTileCount-info.color_original_base)%xenos::kEdramTileCount;
  const uint32_t tile_x=offset%edram.pitch_tiles;
  if(edram.format_is_64bpp && (tile_x&1)) throw ColorResolveError("resolve_half_color_tile");
  const uint32_t sx=uint32_t(edram.msaa_samples==xenos::MsaaSamples::k4X),sy=uint32_t(edram.msaa_samples!=xenos::MsaaSamples::k1X);
  return {((tile_x>>edram.format_is_64bpp)*xenos::kEdramTileWidthSamples>>sx)+info.coordinate_info.edram_offset_x_div_8*8,
    ((offset/edram.pitch_tiles)*xenos::kEdramTileHeightSamples>>sy)+info.coordinate_info.edram_offset_y_div_8*8,
    uint32_t(info.coordinate_info.width_div_8)*8,info.height_div_8*8};
}
ColorResolvePlan PlanColorResolve(const draw_util::ResolveInfo& info,uint32_t pitch,uint32_t raw) {
  if(info.IsCopyingDepth()) throw ColorResolveError("native_depth_resolve_unimplemented");
  if(info.copy_dest_info.copy_dest_array) throw ColorResolveError("resolve_array_unimplemented");
  if(info.copy_dest_info.copy_dest_number!=xenos::SurfaceNumberFormat::kUnsignedRepeatingFraction) throw ColorResolveError("resolve_number_format");
  if(raw&4095) throw ColorResolveError("resolve_unaligned_destination");
  if(uint32_t(info.copy_dest_info.copy_dest_endian)>3) throw ColorResolveError("resolve_endian128_unimplemented");
  const auto edram=info.color_edram_info;
  if(!edram.pitch_tiles || uint32_t(edram.msaa_samples)>2 || !pitch) throw ColorResolveError("resolve_source_pitch");
  const uint32_t samples=1u<<uint32_t(edram.msaa_samples);
  const auto select=info.copy_dest_coordinate_info.copy_sample_select;
  // Host sample numbering is not assumed to match Xenos. Averaging all samples
  // is invariant under that mapping; subset/single-sample MSAA needs more work.
  if((samples==1 && select!=xenos::CopySampleSelect::k0) ||
     (samples==2 && select!=xenos::CopySampleSelect::k01) ||
     (samples==4 && select!=xenos::CopySampleSelect::k0123)) throw ColorResolveError("resolve_sample_subset_unimplemented");
  ColorResolvePlan p{}; p.samples=samples; p.address=raw;
  p.guest_format=uint32_t(info.copy_dest_info.copy_dest_format); p.endian=uint32_t(info.copy_dest_info.copy_dest_endian);
  p.exponent=info.copy_dest_info.copy_dest_exp_bias; p.swap_rb=info.copy_dest_info.copy_dest_swap;
  uint32_t bytes=0;
  switch(p.guest_format) {
    case 2: case 8: case 9: p.format=VK_FORMAT_R8_UNORM;bytes=1;break;
    case 10: p.format=VK_FORMAT_R8G8_UNORM;bytes=2;break;
    case 6: case 14: case 50: p.format=VK_FORMAT_R8G8B8A8_UNORM;bytes=4;break;
    case 7: case 54: p.format=VK_FORMAT_A2B10G10R10_UNORM_PACK32;bytes=4;break;
    case 30: p.format=VK_FORMAT_R16_SFLOAT;bytes=2;break;
    case 31: p.format=VK_FORMAT_R16G16_SFLOAT;bytes=4;break;
    case 32: p.format=VK_FORMAT_R16G16B16A16_SFLOAT;bytes=8;break;
    case 36: p.format=VK_FORMAT_R32_SFLOAT;bytes=4;break;
    case 37: p.format=VK_FORMAT_R32G32_SFLOAT;bytes=8;break;
    case 38: p.format=VK_FORMAT_R32G32B32A32_SFLOAT;bytes=16;break;
    default: throw ColorResolveError("resolve_destination_format_"+std::to_string(p.guest_format));
  }
  p.source=ColorResolveSourceRectangle(info);
  p.destination=p.source;
  p.width=info.copy_dest_coordinate_info.pitch_aligned_div_32*32;
  p.height=std::max(uint32_t(info.copy_dest_coordinate_info.height_aligned_div_32)*32,(p.destination.y+p.destination.height+31)&~31u);
  if(!p.source.width || !p.source.height || uint64_t(p.source.x)+p.source.width>pitch ||
     !p.width || p.width>4096 || p.height>2048 || uint64_t(p.destination.x)+p.destination.width>p.width) throw ColorResolveError("resolve_extent");
  const uint32_t local_x=info.copy_dest_coordinate_info.offset_x_div_8*8,local_y=info.copy_dest_coordinate_info.offset_y_div_8*8;
  if(p.destination.x<local_x || p.destination.y<local_y) throw ColorResolveError("resolve_destination_origin");
  const int32_t tiled=texture_util::GetTiledOffset2D(p.destination.x-local_x,p.destination.y-local_y,p.width,std::countr_zero(bytes));
  if(tiled<0 || uint64_t(raw)+uint32_t(tiled)!=info.copy_dest_base) throw ColorResolveError("resolve_destination_anchor");
  p.extent_start=info.copy_dest_extent_start; p.extent_bytes=info.copy_dest_extent_length;
  if(!p.extent_bytes || p.extent_start<raw || uint64_t(p.extent_start)+p.extent_bytes>0x20000000) throw ColorResolveError("resolve_destination_bounds");
  return p;
}
}
