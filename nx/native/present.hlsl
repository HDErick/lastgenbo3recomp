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
