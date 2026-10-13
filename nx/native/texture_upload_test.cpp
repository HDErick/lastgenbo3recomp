// SPDX-License-Identifier: GPL-3.0-only
#include "texture_upload.h"
#include <algorithm>
#include <cstring>
#include <iostream>
using namespace bo3::native;
using namespace rex::graphics;
static uint32_t checks=0;
void Check(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); ++checks; }
xenos::xe_gpu_texture_fetch_t Fetch(uint32_t width,uint32_t height,uint32_t kind=0,uint32_t depth=1) {
  xenos::xe_gpu_texture_fetch_t f{}; f.type=xenos::FetchConstantType::kTexture;
  f.format=xenos::TextureFormat::k_8_8_8_8; f.base_address=0x100; f.pitch=2; f.swizzle=0x688;
  f.dimension=kind==1 ? xenos::DataDimension::kCube : kind==2 ? xenos::DataDimension::k3D : xenos::DataDimension::k2DOrStacked;
  if(kind==2) {f.size_3d.width=width-1; f.size_3d.height=height-1; f.size_3d.depth=depth-1;}
  else {f.size_2d.width=width-1; f.size_2d.height=height-1; f.size_2d.stack_depth=kind==1 ? 5 : 0;}
  return f;
}
// Fill a fixture using the SDK's guest tiling function. Golden checks below
// independently assert face/level/slice ownership and linear boundary addresses;
// this fixture does not independently prove the SDK's tiling algorithm.
void Fill(const TextureUploadPlan& p,std::vector<uint8_t>& base,std::vector<uint8_t>& mips) {
  for(const auto& s:p.slices) for(uint32_t z=0;z<s.depth;++z) for(uint32_t y=0;y<s.height;++y) for(uint32_t x=0;x<s.width;++x) {
    uint64_t relative;
    if(p.fetch.tiled) relative=p.kind==2 ? texture_util::GetTiledOffset3D(x+s.x_blocks,y+s.y_blocks,z+s.z,s.row_pitch/4,s.z_rows,2) : texture_util::GetTiledOffset2D(x+s.x_blocks,y+s.y_blocks,s.row_pitch/4,2);
    else relative=uint64_t(z+s.z)*s.z_rows*s.row_pitch+uint64_t(y+s.y_blocks)*s.row_pitch+uint64_t(x+s.x_blocks)*4;
    auto& source=s.from_mips ? mips : base;
    const uint64_t at=s.source_offset+relative;
    if(at+4>source.size()) throw std::runtime_error("fixture bounds");
    source[at]=uint8_t(s.mip+10); source[at+1]=uint8_t(s.layer+20); source[at+2]=uint8_t(z+30); source[at+3]=255;
  }
}
void Verify(const TextureUploadPlan& p,const std::vector<uint8_t>& pixels) {
  for(const auto& s:p.slices) for(uint32_t z=0;z<s.depth;++z) for(uint32_t y=0;y<s.height;++y) for(uint32_t x=0;x<s.width;++x) {
    const uint64_t at=s.output_offset+((uint64_t(z)*s.height+y)*s.width+x)*4;
    if(pixels[at]!=s.mip+10 || pixels[at+1]!=s.layer+20 || pixels[at+2]!=z+30 || pixels[at+3]!=255) throw std::runtime_error("face/mip/Z ownership or texel mismatch");
  }
  ++checks;
}
void Roundtrip(xenos::xe_gpu_texture_fetch_t f,uint32_t kind) {
  auto p=PlanTextureUpload(f,kind,16*1024*1024);
  std::vector<uint8_t> base(p.base_bytes),mips(p.mip_bytes);
  Fill(p,base,mips); Verify(p,DecodeTextureUpload(p,base,mips));
}
int main() {
 try {
  // Linear odd width/row pitch: second row begins at 256, not at width*4.
  auto f=Fetch(3,2); auto p=PlanTextureUpload(f,0,4096);
  Check(p.base_bytes==268 && p.output_bytes==24,"linear extent/pitch");
  std::vector<uint8_t> base(p.base_bytes); for(uint32_t b=0;b<12;++b){base[b]=uint8_t(b+1);base[256+b]=uint8_t(b+21);}
  auto out=DecodeTextureUpload(p,base,{});
  Check(out[0]==1 && out[11]==12 && out[12]==21 && out[23]==32,"linear row decode");
  const uint8_t endian_words[4][4]={{1,2,3,4},{2,1,4,3},{4,3,2,1},{3,4,1,2}};
  for(uint32_t e=0;e<4;++e) {
    f=Fetch(1,1); f.endianness=xenos::Endian(e); p=PlanTextureUpload(f,0,4096);
    out=DecodeTextureUpload(p,std::span(endian_words[e]),{});
    Check(out==std::vector<uint8_t>({1,2,3,4}),"endian word");
  }
  f=Fetch(1,1); f.format=xenos::TextureFormat::k_8; f.endianness=xenos::Endian::k8in32;
  p=PlanTextureUpload(f,0,4096);
  Check(p.base_bytes==4 && DecodeTextureUpload(p,std::span(endian_words[2]),{})==std::vector<uint8_t>({1}),"byte mip retains endian dword span");
  f=Fetch(4,4,1); p=PlanTextureUpload(f,1,1024*1024);
  Check(p.layers==6 && p.depth==1 && p.slices.size()==6 && p.slices[1].source_offset==8192,"cube face strides");
  Roundtrip(f,1);
  f=Fetch(8,8); f.mip_address=0x300; f.mip_max_level=3;
  p=PlanTextureUpload(f,0,1024*1024);
  Check(p.levels==4 && p.slices[1].source_offset==0 && p.slices[2].source_offset==8192 && p.slices[3].source_offset==16384,"separate unpacked mip allocation");
  Roundtrip(f,0);
  f=Fetch(64,64); f.tiled=1; f.packed_mips=1; f.mip_address=0x300; f.mip_max_level=6;
  p=PlanTextureUpload(f,0,1024*1024);
  Check(p.slices[2].source_offset==4096 && p.slices[2].x_blocks==16 && p.slices[3].x_blocks==8 && p.slices[6].y_blocks==4,"packed tail origins");
  Roundtrip(f,0);
  f=Fetch(64,64,1); f.tiled=1; f.packed_mips=1; f.mip_address=0x300; f.mip_max_level=6;
  p=PlanTextureUpload(f,1,1024*1024);
  Check(p.slices.size()==42 && p.slices[6].mip==1 && p.slices[12].source_offset==6*4096,"cube mip-major ordering");
  Roundtrip(f,1);
  f=Fetch(4,4,2,4); p=PlanTextureUpload(f,2,1024*1024);
  Check(p.layers==1 && p.depth==4 && p.base_bytes==25360 && p.slices[0].z_rows==32,"volume linear slice stride");
  Roundtrip(f,2);
  f=Fetch(32,32,2,8); f.tiled=1; f.packed_mips=1; f.mip_address=0x300; f.mip_max_level=5;
  Roundtrip(f,2);
  p=PlanTextureUpload(f,0,1024*1024);
  Check(p.kind==2 && p.depth==1 && p.levels==1 && p.mip_bytes==0,"3D-as-2D real volume slice layout");
  Roundtrip(f,0);
  Check(TextureDimensionsCompatible(f,0) && TextureDimensionsCompatible(f,2) && !TextureDimensionsCompatible(f,1),"volume descriptor compatibility");
  f=Fetch(8,8);
  Check(TextureDimensionsCompatible(f,0) && !TextureDimensionsCompatible(f,2) && !TextureDimensionsCompatible(f,1),"2D does not stand in for cube/3D");
  f=Fetch(16,16); f.base_address=0; f.mip_address=0x300; f.mip_min_level=1; f.mip_max_level=4; f.packed_mips=1;
  p=PlanTextureUpload(f,0,1024*1024);
  Check(p.base_bytes==0 && p.min_level==1 && p.slices.front().mip==1 && p.slices.size()==4,"base-less packed mip chain");
  Roundtrip(f,0);
  f=Fetch(8,8); f.mip_max_level=15;
  p=PlanTextureUpload(f,0,1024*1024);
  Check(p.levels==1 && p.mip_bytes==0,"missing mip address normalizes range");
  f=Fetch(3,2); f.format=xenos::TextureFormat::k_DXT1;
  p=PlanTextureUpload(f,0,4096); const uint8_t red_bc1[8]={0,248,224,7,0,0,0,0};
  out=DecodeTextureUpload(p,red_bc1,{});
  Check(out.size()==24 && out[0]==255 && out[1]==0 && out[2]==0 && out[23]==255,"BC1 partial block clip");
  const uint8_t transparent_bc1[8]={0,0,255,255,255,255,255,255};
  Check(DecodeTextureUpload(p,transparent_bc1,{})[3]==0,"BC1 transparent endpoint");
  f.force_bc_w_to_max=1; p=PlanTextureUpload(f,0,4096);
  Check(DecodeTextureUpload(p,transparent_bc1,{})[3]==255,"BC alpha override");
  bool truncated=false,mismatch=false,limit=false,physical=false;
  try {DecodeTextureUpload(p,std::span(red_bc1,7),{});} catch(const TextureUploadError&){truncated=true;}
  try {PlanTextureUpload(Fetch(8,8),1,4096);} catch(const TextureUploadError&){mismatch=true;}
  try {PlanTextureUpload(Fetch(64,64,1),1,4096);} catch(const TextureUploadError&){limit=true;}
  f=Fetch(64,64); f.base_address=0x1ffff;
  try {PlanTextureUpload(f,0,1024*1024);} catch(const TextureUploadError&){physical=true;}
  Check(truncated && mismatch && limit && physical,"unsafe source/view/upload accepted");
  f=Fetch(8,8); f.base_address=0;
  Check(!TextureDimensionsCompatible(f,0),"missing backing uses unbound semantics");
  std::cout<<checks<<" texture upload checks passed; no game or GPU rendering launched\n";
  return 0;
 } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
