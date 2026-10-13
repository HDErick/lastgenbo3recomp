// SPDX-License-Identifier: GPL-3.0-only
struct Vertex { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Vertex VS(uint index : SV_VertexID) {
  Vertex o;
  o.uv = float2((index << 1) & 2, index & 2);
  o.position = float4(o.uv * 2 - 1, 0, 1);
  return o;
}
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] Texture2D<float4> source;
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] SamplerState filtering;
float4 PS(Vertex input) : SV_Target0 { return source.Sample(filtering, input.uv); }
[[vk::binding(0, 0)]] Texture2DMS<float> sceneDepth;
float4 DepthPS(Vertex input) : SV_Target0 {
  uint width, height, samples;
  sceneDepth.GetDimensions(width, height, samples);
  uint2 pixel = min(uint2(input.uv * float2(width, height)), uint2(width - 1, height - 1));
  float depth = sceneDepth.Load(pixel, 0);
  // Clear depth 1 is black; a nonlinear display makes geometry near 1 visible.
  float shade = pow(saturate(1.0 - depth), 0.2);
  return float4(shade, shade, shade, 1);
}
