// SPDX-License-Identifier: GPL-3.0-only
#include "texture_upload.h"
#include "bc_decode.h"
#include <algorithm>
#include <bit>
#include <cstring>
namespace bo3::native {
using namespace rex::graphics;
bool TextureDimensionsCompatible(const xenos::xe_gpu_texture_fetch_t& fetch, uint32_t kind) {
  // Same compatibility rules as VulkanTextureCache::AreDimensionsCompatible.
  if (uint32_t(fetch.type)!=2) return false;
  if (!(fetch.base_address & 0x1ffff) && !(fetch.mip_address & 0x1ffff)) return false;
  if (kind==0) return fetch.dimension!=xenos::DataDimension::kCube;
  if (kind==1) return fetch.dimension==xenos::DataDimension::kCube;
  if (kind==2) return fetch.dimension==xenos::DataDimension::k3D;
  return false;
}
TextureUploadPlan PlanTextureUpload(const xenos::xe_gpu_texture_fetch_t& fetch, uint32_t kind, uint32_t limit) {
  const bool volume_as_2d=kind==0 && fetch.dimension==xenos::DataDimension::k3D;
  TextureUploadPlan p{}; p.fetch = fetch; p.kind = volume_as_2d ? 2 : kind;
  if (uint32_t(fetch.type) != 2 || fetch.stacked || kind > 2) throw TextureUploadError("texture_dimension_or_type");
  if ((kind == 0 && fetch.dimension != xenos::DataDimension::k2DOrStacked && !volume_as_2d) ||
      (kind == 1 && fetch.dimension != xenos::DataDimension::kCube) ||
      (kind == 2 && fetch.dimension != xenos::DataDimension::k3D)) throw TextureUploadError("texture_view_dimension_mismatch");
  if (uint32_t(fetch.sign_x) || uint32_t(fetch.sign_y) || uint32_t(fetch.sign_z) || uint32_t(fetch.sign_w) || fetch.num_format)
    throw TextureUploadError("texture_numeric_conversion");
  if (fetch.border_size) throw TextureUploadError("texture_border_size");
  if (kind == 1 && (fetch.size_2d.stack_depth != 5 || fetch.size_2d.width != fetch.size_2d.height)) throw TextureUploadError("texture_cube_extent");
  if (kind == 2 && (fetch.vol_mag_filter != uint32_t(fetch.mag_filter == xenos::TextureFilter::kLinear) ||
                    fetch.vol_min_filter != uint32_t(fetch.min_filter == xenos::TextureFilter::kLinear)))
    throw TextureUploadError("texture_volume_filter");
  if (uint32_t(fetch.mag_filter) > 1 || uint32_t(fetch.min_filter) > 1 || uint32_t(fetch.mip_filter) > 2) throw TextureUploadError("texture_filter");
  if (kind != 1 && (uint32_t(fetch.clamp_x) > 2 || uint32_t(fetch.clamp_y) > 2 || (kind == 2 && uint32_t(fetch.clamp_z) > 2))) throw TextureUploadError("texture_clamp");
  for (uint32_t c = 0; c < 4; ++c) if (((fetch.swizzle >> (3*c)) & 7) >= 6) throw TextureUploadError("texture_swizzle");
  switch (uint32_t(fetch.format)) {
    case 2: case 8: case 9: p.bytes_per_block=1; p.format=VK_FORMAT_R8_UNORM; break;
    case 10: p.bytes_per_block=2; p.format=VK_FORMAT_R8G8_UNORM; break;
    case 6: case 14: case 50: p.bytes_per_block=4; p.format=VK_FORMAT_R8G8B8A8_UNORM; break;
    case 7: case 54: p.bytes_per_block=4; p.format=VK_FORMAT_A2B10G10R10_UNORM_PACK32; break;
    case 30: p.bytes_per_block=2; p.format=VK_FORMAT_R16_SFLOAT; break;
    case 31: p.bytes_per_block=4; p.format=VK_FORMAT_R16G16_SFLOAT; break;
    case 32: p.bytes_per_block=8; p.format=VK_FORMAT_R16G16B16A16_SFLOAT; break;
    case 36: p.bytes_per_block=4; p.format=VK_FORMAT_R32_SFLOAT; break;
    case 37: p.bytes_per_block=8; p.format=VK_FORMAT_R32G32_SFLOAT; break;
    case 38: p.bytes_per_block=16; p.format=VK_FORMAT_R32G32B32A32_SFLOAT; break;
    case 18: p.bytes_per_block=8; p.bc=1; p.format=VK_FORMAT_R8G8B8A8_UNORM; break;
    case 19: p.bytes_per_block=16; p.bc=2; p.format=VK_FORMAT_R8G8B8A8_UNORM; break;
    case 20: p.bytes_per_block=16; p.bc=3; p.format=VK_FORMAT_R8G8B8A8_UNORM; break;
    default: throw TextureUploadError("texture_format_" + std::to_string(uint32_t(fetch.format)));
  }
  p.block_width = p.bc ? 4 : 1;
  uint32_t w,h,d,base_page,mip_page;
  texture_util::GetSubresourcesFromFetchConstant(fetch,&w,&h,&d,&base_page,&mip_page,&p.min_level,&p.max_level);
  p.width=w+1; p.height=h+1; p.depth=kind==2 ? d+1 : 1; p.layers=kind==1 ? 6 : 1;
  if (volume_as_2d) {
    // The SDK's 3D-as-2D wrapper samples Z slice zero of the base level only.
    // Read the real volume layout, never reinterpret its tiled data as 2D.
    if (!base_page || p.min_level) throw TextureUploadError("texture_3d_as_2d_missing_base");
    mip_page=0; p.max_level=0;
  }
  p.levels=p.max_level+1;
  if (p.width>4096 || p.height>4096 || p.depth>1024) throw TextureUploadError("texture_extent");
  if (!base_page && !mip_page) throw TextureUploadError("texture_missing_backing");
  p.base_address=base_page<<12; p.mip_address=mip_page<<12;
  const auto layout=texture_util::GetGuestTextureLayout(fetch.dimension,fetch.pitch,p.width,p.height,p.depth,
      fetch.tiled,fetch.format,fetch.packed_mips,base_page!=0,p.max_level);
  p.base_bytes=base_page ? layout.base.level_data_extent_bytes : 0;
  p.mip_bytes=mip_page ? layout.mips_total_extent_bytes : 0;
  // Endian addressing may select the last byte of its containing dword even
  // for a 1-byte mip. Reserve that dword instead of truncating its source span.
  if (uint32_t(fetch.endianness)) {
    p.base_bytes=(p.base_bytes+3)&~3u; p.mip_bytes=(p.mip_bytes+3)&~3u;
  }
  if (uint64_t(p.base_address)+p.base_bytes>0x20000000 || uint64_t(p.mip_address)+p.mip_bytes>0x20000000)
    throw TextureUploadError("texture_physical_range");
  // Bound guest reads separately from the decoded upload, including tiled padding.
  if (uint64_t(p.base_bytes)+p.mip_bytes > uint64_t(limit)*4) throw TextureUploadError("texture_source_limit");
  uint64_t output=0;
  for (uint32_t mip=p.min_level; mip<=p.max_level; ++mip) {
    const bool from_mips=mip!=0;
    const uint32_t stored=std::min(mip,layout.packed_level);
    const auto& level=from_mips ? layout.mips[stored] : layout.base;
    uint32_t x=0,y=0,z=0;
    if (fetch.packed_mips) texture_util::GetPackedMipOffset(p.width,p.height,p.depth,fetch.format,mip,x,y,z);
    const uint32_t width=std::max(p.width>>mip,1u), height=std::max(p.height>>mip,1u), depth=std::max(p.depth>>mip,1u);
    for (uint32_t layer=0; layer<p.layers; ++layer) {
      output=(output+15)&~uint64_t(15);
      const uint64_t bytes=uint64_t(width)*height*depth*(p.bc ? 4 : p.bytes_per_block);
      if (output+bytes>limit) throw TextureUploadError("texture_upload_limit");
      const uint64_t source_offset=uint64_t(from_mips ? layout.mip_offsets_bytes[stored] : 0)+uint64_t(layer)*level.array_slice_stride_bytes;
      if (source_offset>= (from_mips ? p.mip_bytes : p.base_bytes)) throw TextureUploadError("texture_layout_bounds");
      p.slices.push_back({mip,layer,width,height,depth,uint32_t(output),uint32_t(source_offset),x,y,z,level.row_pitch_bytes,level.z_slice_stride_block_rows,from_mips});
      output+=bytes;
    }
  }
  p.output_bytes=uint32_t(output); return p;
}
std::vector<uint8_t> DecodeTextureUpload(const TextureUploadPlan& p, std::span<const uint8_t> base, std::span<const uint8_t> mips) {
  if (base.size()<p.base_bytes || mips.size()<p.mip_bytes) throw TextureUploadError("texture_source_bounds");
  std::vector<uint8_t> pixels(p.output_bytes);
  const uint32_t endian=uint32_t(p.fetch.endianness), xor_byte=endian==1 ? 1 : endian==2 ? 3 : endian==3 ? 2 : 0;
  for (const auto& s:p.slices) {
    const auto source=s.from_mips ? mips : base;
    const uint32_t pitch=s.row_pitch/p.bytes_per_block, log_bytes=std::countr_zero(p.bytes_per_block);
    auto address=[&](uint32_t x,uint32_t y,uint32_t z) -> uint64_t {
      x+=s.x_blocks; y+=s.y_blocks; z+=s.z;
      int64_t relative=p.fetch.tiled ? (p.kind==2 ? texture_util::GetTiledOffset3D(x,y,z,pitch,s.z_rows,log_bytes) : texture_util::GetTiledOffset2D(x,y,pitch,log_bytes)) :
          int64_t(z)*s.z_rows*s.row_pitch+int64_t(y)*s.row_pitch+int64_t(x)*p.bytes_per_block;
      if (relative<0) throw TextureUploadError("texture_source_bounds");
      return s.source_offset+uint64_t(relative);
    };
    auto read=[&](uint64_t offset) {
      const uint64_t at=offset^xor_byte;
      if (at>=source.size()) throw TextureUploadError("texture_source_bounds");
      return source[at];
    };
    if (!p.bc && !p.fetch.tiled && !(s.width*p.bytes_per_block & 3) && !(s.x_blocks*p.bytes_per_block & 3)) {
      const uint32_t row_bytes=s.width*p.bytes_per_block;
      for (uint32_t z=0;z<s.depth;++z) for (uint32_t y=0;y<s.height;++y) {
        const uint64_t begin=address(0,y,z);
        if (begin+row_bytes>source.size()) throw TextureUploadError("texture_source_bounds");
        auto* out=pixels.data()+s.output_offset+(uint64_t(z)*s.height+y)*row_bytes;
        if (!endian) std::memcpy(out,source.data()+begin,row_bytes);
        else for (uint32_t at=0;at<row_bytes;at+=4) for (uint32_t b=0;b<4;++b) out[at+b]=read(begin+at+b);
      }
      continue;
    }
    for (uint32_t z=0;z<s.depth;++z) for (uint32_t y=0;y<(s.height+p.block_width-1)/p.block_width;++y)
      for (uint32_t x=0;x<(s.width+p.block_width-1)/p.block_width;++x) {
        const auto at=address(x,y,z);
        if (p.bc) {
          uint8_t block[16]{}, rgba[64];
          for (uint32_t b=0;b<p.bytes_per_block;++b) block[b]=read(at+b);
          DecodeBC(block,p.bc,rgba);
          if (p.fetch.force_bc_w_to_max) for (uint32_t pixel=0;pixel<16;++pixel) rgba[pixel*4+3]=255;
          for (uint32_t row=0;row<4 && y*4+row<s.height;++row)
            std::memcpy(pixels.data()+s.output_offset+((uint64_t(z)*s.height+y*4+row)*s.width+x*4)*4,rgba+row*16,std::min(4u,s.width-x*4)*4);
        } else for (uint32_t b=0;b<p.bytes_per_block;++b)
          pixels[s.output_offset+((uint64_t(z)*s.height+y)*s.width+x)*p.bytes_per_block+b]=read(at+b);
      }
  }
  return pixels;
}
}
