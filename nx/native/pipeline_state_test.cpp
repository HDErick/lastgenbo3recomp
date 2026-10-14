// SPDX-License-Identifier: GPL-3.0-only
#include "pipeline_state.h"
#include <iostream>
#include <stdexcept>
using namespace bo3::native;
using namespace rex::graphics;
int main() {
 try {
  uint32_t checks=0;
  auto check=[&](bool value,const char* name){if(!value)throw std::runtime_error(name);++checks;};
  reg::RB_DEPTHCONTROL depth{};reg::PA_SU_SC_MODE_CNTL raster{};reg::RB_BLENDCONTROL blend{};
  auto key=[&](bool rectangle=false,bool color=true,bool clamp=false){return NativePipelineStateKey(depth,raster,blend,rectangle,false,color,clamp);};
  const auto empty=key();
  raster.poly_offset_front_enable=1;raster.poly_offset_back_enable=1;raster.msaa_enable=1;raster.vtx_window_offset_enable=1;
  check(key()==empty,"dynamic raster fields must reuse pipeline");
  raster.face=1;check(key()!=empty,"face orientation must remain static");raster.face=0;
  raster.cull_back=1;check(key()!=empty,"triangle culling must remain static");
  check(key(true)==empty,"rectangle culling must be ignored");raster.cull_back=0;
  depth.zfunc=xenos::CompareFunction(3);depth.z_write_enable=1;
  check(key()==empty,"disabled depth state must reuse pipeline");
  depth.z_enable=1;auto enabled=key();check(enabled!=empty,"enabled depth must remain static");
  depth.zfunc=xenos::CompareFunction(2);check(key()!=enabled,"depth compare must remain static");
  depth={};depth.stencilfunc=xenos::CompareFunction(3);depth.stencilfail=xenos::StencilOp(2);depth.backface_enable=1;
  check(key()==empty,"disabled stencil state must reuse pipeline");
  depth.stencil_enable=1;enabled=key();depth.stencilfail_bf=xenos::StencilOp(3);
  check(key()!=enabled,"enabled backface stencil must remain static");
  enabled=key(true);depth.stencilfail_bf=xenos::StencilOp(4);
  check(key(true)==enabled,"rectangle backface stencil must be ignored");
  depth={};blend.color_srcblend=xenos::BlendFactor(1);
  check(key()!=empty,"enabled color blend must remain static");
  check(key(false,false)==empty,"masked color blend must reuse pipeline");
  check(key(false,false,true)!=empty,"depth clamp must remain static");
  std::cout<<checks<<" pipeline state checks passed; no game or GPU rendering launched\n";return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
