# Initial BO3 native shader interface

October 13, 2026. This records the tested offline prototype's interface;
the BO3 native Vulkan backend is not implemented. Typed texture declarations
now validate offline; live view ownership and memory exports remain gates.
This is prototype ABI 2, not a finalized runtime interface.

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
| 1 | N | Combined 2D image/sampler for slot N (also Xenos 1D), 0..31 |
| 1 | N + 32 | Combined cube image/sampler for slot N |
| 1 | N + 64 | Combined 3D image/sampler for slot N |
| 2 | 0 | Upload arena as read-only uint dwords |

Float windows come from the actual SQ_VS_CONST/SQ_PS_CONST register bases.
Populate fetch constants from the complete guest fetch-register window, not from
container declarations. Preserve texture fetch dwords. Rewrite only vertex-buffer
addresses when relocating their data into the arena; the shader addresses dwords.
GetWeights reads guest 2D size-minus-one fields from fetch dword 2.

`texture_view_masks[N]` names each declared view: bit 0 = 2D, bit 1 = cube,
bit 2 = 3D. `texture_kinds[N]` remains a single-view summary and is 255 for
mixed slots; do not use it to allocate bindings. Merge VS/PS masks when building
a pipeline layout so both stages use the same binding for a given slot/kind.
Allocate only the union of used bindings, not 96 descriptors. This capture's
maximum is 11 texture descriptors in a PS and none in a VS; other programs and
actual device descriptor limits still require checks.

The three mixed PS programs declare 2D slot 13 at binding 13 and 3D slot 13 at
binding 77. Each instruction loads its correctly typed variable. All 455 modules
pass descriptor/image-type reflection and structural validation; two still drop
memory exports and must remain blocked. The previously accepted 450 modules have
identical SPIR-V after normalizing the changed binding numbers.

Separate descriptors do not establish correct backing images. The staged SDK
explicitly handles 1D/2D sampling of a 3D texture via a special compatible view
(`VulkanTextureCache::GetActiveBindingOrNullImageView`). Reproduce and verify
guest texture layout and conversion semantics; do not bind an arbitrary view
type or assume an inactive predicate makes an incompatible descriptor legal.
Texture ownership, upload, transitions, sampler overrides and typed fallback
images must be implemented in the native cache.

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

- The three mixed PS programs translate using separate typed bindings. Implement
  their compatible backing views and texture-cache ownership rules at runtime.
- Two VS programs export to guest memory. They validate structurally but drop
  writes in MW2's implementation. A native backend must refuse those modules
  until their writes or a verified BO3 equivalent copy path are supported. The
  native plugin now handles these two exact programs through state-checked CPU
  copies tested against eight synchronous GPU output captures; it still refuses
  their dropped-export modules and arbitrary memory-export programs.
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
