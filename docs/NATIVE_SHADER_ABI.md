# Initial BO3 native shader interface

October 13, 2026. This records the tested offline prototype's interface;
the BO3 native Vulkan backend is not implemented. Mixed texture views and
memory exports remain implementation gates, so this is not a finalized ABI.

## Program identity

Identify a program by shader stage and its exact GPU-loaded microcode. Keep a
canonical big-endian representation for hashes and byte comparisons. SDK dumps
from this Windows run are little-endian dwords and must be converted explicitly.
Original shader-container similarity is diagnostic evidence, not a lookup key.

The initial correctness path uses shader-based vertex fetching. The translated
VS reads fetch constants, stride, format and endian state and pulls data from a
read-only upload arena. There is no conventional Vulkan vertex-input declaration.
The 147 observed raster VS validate without original container declarations.
This has not been measured on Switch; common cases may later use cheaper paths.

## Current descriptor interface

| Set | Binding | Contents |
| --- | --- | --- |
| 0 | 0 | VS float-constant window: 256 float4 |
| 0 | 1 | Fetch constants: 96 pairs, each padded to uint4 |
| 0 | 2 | All 256 boolean constants: 2 uint4 |
| 0 | 3 | 32 loop constants: 8 uint4 |
| 0 | 4 | PS float-constant window: 256 float4 |
| 1 | N | Combined image/sampler for fetch slot N, 0..31 |
| 2 | 0 | Upload arena as read-only uint dwords |

Float windows come from the actual SQ_VS_CONST/SQ_PS_CONST register bases.
Populate fetch constants from the complete guest fetch-register window, not from
container declarations. Preserve texture fetch dwords. Rewrite only vertex-buffer
addresses when relocating their data into the arena; the shader addresses dwords.
GetWeights reads guest 2D size-minus-one fields from fetch dword 2.

Push constants at bytes 0..15 hold pixel alpha function/reference, color exponent
scale and gamma-target flag. Bytes 16..31 hold vertex XY scale/offset for window
coordinates. Initial render scale is 1; the optional scaled-target interface is
not part of this first prototype.

## Required upload and lifetime behavior

Snapshot mutable buffers/constants while consuming each draw. Reuse an upload
only when the same guest content/version is established, and keep it alive until
its GPU fence retires. Before guest completion/writebacks permit memory reuse,
finish reading every input owed to earlier draws. Do not adopt MW2's fixed
512-MB arena/shadow allocations as a Switch memory budget.

## Remaining binding and execution gates

- Three PS programs sample slot 13 as both 2D and 3D. The current one-view-per-slot
  interface refuses them. Define and implement separate compatible descriptor
  views plus their texture-cache ownership rules; a type cast cannot fix them.
- Two VS programs export to guest memory. They validate structurally but drop
  writes in MW2's implementation. A native backend must refuse those modules
  until their writes or a verified BO3 equivalent copy path are supported.
- The SA2-derived HLSL library uses another descriptor/constant interface. Pairing
  those PS modules with these VS requires an explicit adaptation; matching stage
  outputs alone is insufficient.
- Implement BO3 target formats/aliasing, required resolves/readbacks, queries,
  synchronization and presentation before claiming a native game frame.
- Validate numerical output and actual Vulkan driver execution. Offline SPIR-V
  validation does not verify register contents, texture signs/filter behavior,
  blend/depth state or correct geometry.

See [comparison evidence](../analysis/MW2_NATIVE_REFERENCE.md) and
[tool commands](NATIVE_SHADER_TOOLS.md).
