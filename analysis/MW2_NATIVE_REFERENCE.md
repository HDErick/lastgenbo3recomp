# MW2 renderer reference and revised BO3 work order

October 13, 2026. Reviewed public `PaulCombal/mw2-recompiled` master at
`1d7171213c87b532039b234e61af29d94010262a`. Selected docs and sources are pinned
under ignored `tools/mw2-recompiled-reference/`. No MW2 implementation has
been imported into BO3 or used to draw BO3 yet.

## What it implements

MW2 has a custom PM4-to-Vulkan renderer, independent of RexGlue. Guest D3D9
keeps producing commands; the runtime parses those commands, translates actual
loaded Xenos microcode directly to SPIR-V and renders to Vulkan images. It
does not emulate a byte-addressed EDRAM buffer. This is relevant to our chosen
command-ring architecture, but its runtime interfaces differ from RexGlue.

Primary sources at the reviewed commit:

- [Renderer and limitations](https://github.com/PaulCombal/mw2-recompiled/blob/1d7171213c87b532039b234e61af29d94010262a/docs/rendering.md)
- [Shader interface, vertex fetch and unsupported operations](https://github.com/PaulCombal/mw2-recompiled/blob/1d7171213c87b532039b234e61af29d94010262a/docs/shaders.md)
- [Command stream and completion handshake](https://github.com/PaulCombal/mw2-recompiled/blob/1d7171213c87b532039b234e61af29d94010262a/docs/d3d9-seam.md)
- [Translator implementation](https://github.com/PaulCombal/mw2-recompiled/blob/1d7171213c87b532039b234e61af29d94010262a/runtime/gpu/vulkan/shader_translator.cpp)

The source license is GPL-3.0-only. Keep its copyright/license when adapting
implementation. BO3's translated shaders and captures remain local data.

## Useful for BO3

**Vertex fetching from shader code.** Vertex shaders read fetch constants and
uploaded raw buffers themselves, including endian conversion, format decoding,
stride and mini-fetch reuse. The Vulkan pipeline has no conventional vertex
input layout. This is a promising alternative to requiring an original shader
container declaration to match every patched vertex program. It does not prove
correctness for BO3 or remove the need to track fetch constants and buffer life.

**GPU-loaded shader identity.** Translation consumes the microcode from IM_LOAD
or IM_LOAD_IMMEDIATE, so a patched program has its own exact identity. This is
directly relevant to our 150 loaded VS programs, none of which matches captured
container microcode exactly. Similarity matching is not sufficient identity.

**Guest completion and resource lifetime.** The renderer snapshots/uploads data
before notifying the guest that prior work completed. Command-buffer replay,
scratch writebacks, pending texture reads and guest memory reuse are treated
explicitly. These patterns can inform a RexGlue integration without replacing
our working kernel/XAM/game runtime.

**Compilation and caching.** Shader libraries may be prepared when shader
objects initialize, while patched variants still appear at their first draw.
Driver and application caches are separate. Treat pipeline-library support as
an optional capability until verified on both PC and Switch.

## Limits preventing wholesale reuse

- The translator rejects `get_weights`, computed LOD and gradient-setting
  fetch opcodes. BO3 already uses GetTextureWeights: the initial unpatched
  HLSL compile failed for 97 pixel shaders at that helper. Those shaders now
  validate with our helper-argument repair. Switching translators indiscriminately
  would reintroduce a coverage gap.
- Pixel depth export is rejected. Shader memory exports can be dropped/countable;
  MW2 implements its known image-pool copy through a game-specific CPU path.
  Such omissions must become explicit BO3 unsupported-operation reports.
- Only color target zero is attached. General MRT rendering, CPU resolve
  writeback, console sample locations and alpha-to-coverage are not implemented.
  BO3's needs must be established from actual command/state traces.
- Its image ownership assumes MW2's surface/tiling behavior. Do not apply that
  assumption to BO3; preserve alias/format ownership and the bloom conversion
  contract, including the observed resolve exponent bias +11.
- The raw-fetch ABI differs from our existing SA2-derived modules. Mixing the
  two shader sets needs a deliberate descriptor/constant interface adaptation.
- The documented default frame arena is 512 MB and a physical-memory shadow
  adds another 512 MB. Those allocations cannot be adopted as a Switch budget.
  Vertex pulling and general dispatcher-style shader control flow also need
  Switch GPU measurements; desktop behavior does not establish playable FPS.

## Next steps in order

1. **Compare vertex translation offline.** Build a small isolated driver for
   MW2's translator and run the 455 captured GPU-loaded programs through it,
   checking SPIR-V and recording unsupported operations. Focus on the 150 VS
   programs. Compare its raw vertex-fetch approach with adapting our current
   translator. This requires no new gameplay capture initially.
2. **Fix the native shader interface.** Choose one consistent constant, texture
   and vertex-buffer binding contract; preserve full loaded-program identities.
   Resolve the 54 current container failures where relevant to live draws, and
   account for shaders absent from capture. The 2,843 validated modules are a
   useful corpus, not evidence of a complete renderer/library.
3. **Implement a separate PC native Vulkan mode.** Keep RexGlue's working guest
   services. Integrate command consumption, target/texture ownership, uploads,
   completion/fences, required readbacks and presentation. First milestone is
   a real 3D frame with native draw counters and explicit unsupported reports.
   Video-only menus cannot satisfy that milestone.
4. **Validate a complete The Giant frame.** Terrain, models, skinning, weapon,
   UI, glow/particles, shadows and post-processing; compare against D3D12 and
   retained captures. The emulated Vulkan bloom path is a known regression,
   so its broken squares are not expected native-renderer output.
5. **Move the verified path to NX.** Measure memory, command/guest CPU cost,
   upload bandwidth and GPU time, then optimize for original Switch/OLED.
   Start from bounded allocations and supported features; do not predict FPS
   from these reference projects or prioritize final optimization before a
   correct native frame exists.
