// SPDX-License-Identifier: GPL-3.0-only
#include "color_resolve.h"
#include <rex/graphics/pipeline/texture/util.h>
#include <iostream>
#include <limits>
using namespace bo3::native;
using namespace rex::graphics;
static uint32_t checks=0;
void Check(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);++checks;}
draw_util::ResolveInfo Fixture() {
  draw_util::ResolveInfo i{};
  i.color_original_base=68;i.color_edram_info.base_tiles=68;i.color_edram_info.pitch_tiles=11;
  i.color_edram_info.msaa_samples=xenos::MsaaSamples::k1X;
  i.coordinate_info.width_div_8=10;i.height_div_8=2;
  i.copy_dest_coordinate_info.pitch_aligned_div_32=28;
  i.copy_dest_coordinate_info.height_aligned_div_32=1;
  i.copy_dest_coordinate_info.copy_sample_select=xenos::CopySampleSelect::k0;
  i.copy_dest_info.copy_dest_format=xenos::ColorFormat(6);
  i.copy_dest_base=i.copy_dest_extent_start=0x100000;i.copy_dest_extent_length=8192;
  return i;
}
bool Refuses(draw_util::ResolveInfo i,uint32_t pitch=880,uint32_t raw=0x100000) {
  try {PlanColorResolve(i,pitch,raw);return false;}catch(const ColorResolveError&){return true;}
}
int main() {
 try {
  auto i=Fixture();auto p=PlanColorResolve(i,880,0x100000);
  Check(p.width==896 && p.height==32 && p.source.x==0 && p.source.y==0 && p.source.width==80 && p.source.height==16 && p.format==VK_FORMAT_R8G8B8A8_UNORM,"full resolve dimensions");
  i.color_edram_info.base_tiles=68+11+2;i.coordinate_info.edram_offset_x_div_8=1;
  auto r=ColorResolveSourceRectangle(i);
  Check(r.x==168 && r.y==16,"32bpp tile and pixel origin");
  i=Fixture();i.color_edram_info.pitch_tiles=22;i.color_edram_info.format_is_64bpp=1;i.color_edram_info.base_tiles=68+22+4;
  r=ColorResolveSourceRectangle(i);Check(r.x==160 && r.y==16,"64bpp horizontal tile pairs");
  i.color_edram_info.msaa_samples=xenos::MsaaSamples::k4X;
  r=ColorResolveSourceRectangle(i);Check(r.x==80 && r.y==8,"4x sample coordinates become pixels");
  i.color_edram_info.msaa_samples=xenos::MsaaSamples::k2X;
  r=ColorResolveSourceRectangle(i);Check(r.x==160 && r.y==8,"2x has vertical samples only");
  i.color_edram_info.base_tiles++;Check(Refuses(i),"half 64bpp tile rejected");
  i=Fixture();i.color_edram_info.msaa_samples=xenos::MsaaSamples::k2X;i.copy_dest_coordinate_info.copy_sample_select=xenos::CopySampleSelect::k01;
  Check(PlanColorResolve(i,880,0x100000).samples==2,"two-sample average accepted");
  i.copy_dest_coordinate_info.copy_sample_select=xenos::CopySampleSelect::k0;Check(Refuses(i),"single host MSAA sample mapping not assumed");
  i.color_edram_info.msaa_samples=xenos::MsaaSamples::k4X;i.copy_dest_coordinate_info.copy_sample_select=xenos::CopySampleSelect::k0123;
  Check(PlanColorResolve(i,880,0x100000).samples==4,"four-sample average accepted");
  i.copy_dest_coordinate_info.copy_sample_select=xenos::CopySampleSelect::k23;Check(Refuses(i),"sample subset rejected");
  i=Fixture();i.color_edram_info.base_tiles+=2;i.copy_dest_coordinate_info.offset_x_div_8=4;
  i.copy_dest_base=0x100000+texture_util::GetTiledOffset2D(128,0,896,2);
  Check(PlanColorResolve(i,880,0x100000).destination.x==160,"nonzero destination anchor matched");
  i.copy_dest_base+=4;Check(Refuses(i),"mismatched destination anchor rejected");
  i=Fixture();i.copy_dest_info.copy_dest_exp_bias=-3;i.copy_dest_info.copy_dest_swap=1;
  p=PlanColorResolve(i,880,0x100000);Check(p.exponent==-3 && p.swap_rb,"conversion flags retained");
  i=Fixture();i.copy_dest_info.copy_dest_array=1;Check(Refuses(i),"array destination rejected");
  i=Fixture();i.rb_copy_control.copy_src_select=4;Check(Refuses(i),"depth resolve rejected");
  i=Fixture();i.copy_dest_info.copy_dest_endian=xenos::Endian128(4);Check(Refuses(i),"unsupported endian width rejected");
  i=Fixture();i.copy_dest_extent_start=0x1ffff000;i.copy_dest_extent_length=8192;Check(Refuses(i),"physical heap boundary rejected");
  i=Fixture();Check(Refuses(i,64),"source pitch bound enforced");
  i.copy_dest_coordinate_info.pitch_aligned_div_32=2;Check(Refuses(i),"destination pitch bound enforced");
  i=Fixture();i.height_div_8=257;Check(Refuses(i),"height allocation bound enforced");
  i=Fixture();Check(Refuses(i,880,0x100004),"unaligned base rejected");
  std::vector<ResolveRect> coverage;
  AddResolveCoverage(coverage,{0,0,80,16});AddResolveCoverage(coverage,{8,0,8,8});
  Check(coverage.size()==1,"contained update does not expand validity");
  Check(!ResolveRectContains(coverage[0],{0,0,81,16}) && !ResolveRectContains(coverage[0],{0,0,80,17}),"padding never becomes valid");
  AddResolveCoverage(coverage,{80,0,80,16});
  Check(coverage.size()==2 && !ResolveRectContains(coverage[0],{0,0,160,16}) && !ResolveRectContains(coverage[1],{0,0,160,16}),"disjoint records are not invented full coverage");
  AddResolveCoverage(coverage,{0,0,160,16});Check(coverage.size()==1 && ResolveRectContains(coverage[0],{0,0,160,16}),"covering update replaces old regions");
  Check(!ResolveRectContains({0,0,16,16},{std::numeric_limits<uint32_t>::max(),0,2,1}),"coverage addition cannot overflow");
  p=PlanColorResolve(Fixture(),880,0x100000);
  std::vector<ResolveRect> display{{0,0,80,16}};
  Check(ResolveCanPresent(p,display,0x100000,80,16),"covered logical frontbuffer accepted despite padding");
  Check(!ResolveCanPresent(p,display,0x101000,80,16),"unrelated frontbuffer address refused");
  Check(!ResolveCanPresent(p,display,0x100000,80,32),"zero padding is not valid display output");
  p.endian=2;Check(!ResolveCanPresent(p,display,0x100000,80,16),"unverified display endian refused");
  p.endian=0;p.guest_format=30;Check(!ResolveCanPresent(p,display,0x100000,80,16),"non-display alias refused");
  p.guest_format=6;Check(!ResolveCanPresent(p,{},0x100000,80,16),"invalidated frontbuffer refused");
  std::cout<<checks<<" color resolve checks passed; no game or GPU rendering launched\n";return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
