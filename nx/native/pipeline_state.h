// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <rex/graphics/registers.h>
#include <array>
namespace bo3::native {
// This prototype uses only these static fields. Viewport, bias values, stencil
// masks/references and clip transforms are supplied dynamically per draw.
inline std::array<uint32_t,4> NativePipelineStateKey(
    rex::graphics::reg::RB_DEPTHCONTROL depth,
    rex::graphics::reg::PA_SU_SC_MODE_CNTL raster,
    rex::graphics::reg::RB_BLENDCONTROL blend,
    bool rectangle,bool no_cull,bool color_writes,bool depth_clamp) {
  using namespace rex::graphics;
  reg::RB_DEPTHCONTROL d{};
  d.z_enable=depth.z_enable;
  if(depth.z_enable){d.z_write_enable=depth.z_write_enable;d.zfunc=depth.zfunc;}
  d.stencil_enable=depth.stencil_enable;
  if(depth.stencil_enable) {
    d.stencilfunc=depth.stencilfunc;d.stencilfail=depth.stencilfail;
    d.stencilzpass=depth.stencilzpass;d.stencilzfail=depth.stencilzfail;
    d.backface_enable=!rectangle && depth.backface_enable;
    if(d.backface_enable) {
      d.stencilfunc_bf=depth.stencilfunc_bf;d.stencilfail_bf=depth.stencilfail_bf;
      d.stencilzpass_bf=depth.stencilzpass_bf;d.stencilzfail_bf=depth.stencilzfail_bf;
    }
  }
  reg::PA_SU_SC_MODE_CNTL s{};
  s.face=raster.face;
  if(!rectangle && !no_cull){s.cull_front=raster.cull_front;s.cull_back=raster.cull_back;}
  reg::RB_BLENDCONTROL b{};
  if(color_writes) {
    b.color_srcblend=blend.color_srcblend;b.color_destblend=blend.color_destblend;b.color_comb_fcn=blend.color_comb_fcn;
    b.alpha_srcblend=blend.alpha_srcblend;b.alpha_destblend=blend.alpha_destblend;b.alpha_comb_fcn=blend.alpha_comb_fcn;
  }
  return {d.value,s.value,b.value,uint32_t(depth_clamp)};
}
}
