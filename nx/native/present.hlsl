// SPDX-License-Identifier: GPL-3.0-only
struct Vertex { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Vertex VS(uint index : SV_VertexID) {
  Vertex o;
  o.uv = float2((index << 1) & 2, index & 2);
  o.position = float4(o.uv * 2 - 1, 0, 1);
  return o;
}
#if defined(BO3_PRESENT_COLOR)
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] Texture2D<float4> source;
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] SamplerState filtering;
float4 PS(Vertex input) : SV_Target0 { return source.Sample(filtering, input.uv); }
#endif
#if defined(BO3_PRESENT_DEPTH)
[[vk::binding(0, 0)]] Texture2DMS<float> sceneDepth;
float4 DepthPS(Vertex input) : SV_Target0 {
  uint width, height, samples;
  sceneDepth.GetDimensions(width, height, samples);
  uint2 pixel = min(uint2(input.uv * float2(width, height)), uint2(width - 1, height - 1));
  float depth = sceneDepth.Load(pixel, 0);
  // BO3 scene depth is reversed: clear zero is black, nearer surfaces brighter.
  // Nonlinear contrast exposes the small values of distant geometry.
  float shade = pow(saturate(depth), 0.2);
  return float4(shade, shade, shade, 1);
}
#endif
#if defined(BO3_RESOLVE_COLOR)
struct ResolveConstants { int2 sourceOrigin; int2 destinationOrigin; float exponentScale; uint swapRB; uint2 padding; };
[[vk::push_constant]] ConstantBuffer<ResolveConstants> resolveConstants;
#if !defined(BO3_RESOLVE_MSAA)
[[vk::binding(0, 0)]] Texture2D<float4> resolveSource;
#else
[[vk::binding(0, 0)]] Texture2DMS<float4> resolveSourceMS;
#endif
float4 ResolveConvert(float4 color) {
  color *= resolveConstants.exponentScale;
  if (resolveConstants.swapRB) color.rb = color.br;
  return color;
}
#if !defined(BO3_RESOLVE_MSAA)
float4 ResolvePS(Vertex input) : SV_Target0 {
  int2 pixel = int2(input.position.xy) - resolveConstants.destinationOrigin + resolveConstants.sourceOrigin;
  return ResolveConvert(resolveSource.Load(int3(pixel, 0)));
}
#else
float4 ResolveMSPS(Vertex input) : SV_Target0 {
  int2 pixel = int2(input.position.xy) - resolveConstants.destinationOrigin + resolveConstants.sourceOrigin;
  uint width, height, samples;
  resolveSourceMS.GetDimensions(width, height, samples);
  float4 color = 0;
  for (uint i = 0; i < samples; ++i) color += resolveSourceMS.Load(pixel, i);
  return ResolveConvert(color / float(samples));
}
#endif
#endif
