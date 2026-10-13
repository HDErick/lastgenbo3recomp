# MW2 renderer reference and revised BO3 work order

October 13, 2026. Reviewed public `PaulCombal/mw2-recompiled` master at
`1d7171213c87b532039b234e61af29d94010262a`. Selected docs and sources are pinned
under ignored `tools/mw2-recompiled-reference/`. The later BO3 prototype adapts
this pinned translator, with GPL-3.0-only provenance, in a separate Vulkan draw
backend. The user confirms visible frontend/menu options; native 3D gameplay is
not validated. The review below records the earlier assessment and work order.
See [current status](../docs/NATIVE_RENDERING_PC.md).

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
directly relevant to our 149 loaded VS programs, none of which matches captured
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
   checking SPIR-V and recording unsupported operations. Focus on the 149 VS
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

## Offline comparison completed

The isolated Windows driver builds the pinned, unmodified MW2 translator without
its runtime, Vulkan presentation or game. It accepts explicit shader stage and
the SDK's little-endian microcode dumps. Each program is translated in a separate
process and every emitted module is checked with spirv-val for Vulkan 1.2/scalar
layout. Source hashes, tool hashes, metadata and all failures are retained.

The 455-program corpus contains **149 VS and 306 PS**. The prior 150-VS statement
was an incorrect inference from 455 minus the 305 exact pixel-container matches:
one additional pixel program also lacked a container match.

| Translator | Clean VS validation | Clean PS validation | Remaining cases |
| --- | ---: | ---: | --- |
| Pinned MW2, unchanged | 147 | 188 | 117 PS reject GetWeights; 1 PS fails SPIR-V image-query typing; 2 VS drop memory exports |
| BO3 GetWeights prototype (comparison02) | 147 | 303 | 3 PS require mixed 2D/3D texture views; 2 VS drop memory exports |
| BO3 typed-view prototype (comparison03) | 147 | 306 | 2 VS drop memory exports; live view ownership still needed |

The prototype implements only the GetWeights profile observed in this capture:
141 instructions across the 117 rejected pixel programs, all normalized 2D,
linear mag/min filtering, no computed/register LOD and only X/Y weight results.
It derives guest size from the 2D fetch constant, retains predicate/swizzle
handling, and uses the staged SDK/Xenia level-zero fraction/rounding rules.
Broader profiles are refused. General mip-LOD/filter semantics, other maps and
numerical GPU parity are unverified.

The first prototype refused mixed-dimensional image slots explicitly. The original
translator assumes one image dimensionality per slot and lets the final fetch
overwrite that choice. BO3 program `7A64BBF119546D8B` uses slot 13 for tfetch2D
and tfetch3D; the original generated a 2-component image-size result for a 3D
image. Two further mixed-slot programs (`B1398134025C11C2`, `BA9B59848E82E5E5`)
were masked by their earlier GetWeights rejection. These need a texture-view
binding design; changing the vector size alone would not implement their intent.

The next adaptation supplies separate typed variables per slot/kind, with stable
cross-stage bindings: 2D = slot, cube = slot+32, 3D = slot+64. The three programs
now validate, using bindings 13 and 77 for their two slot-13 views. Reflection
checks all 455 emitted modules against metadata and image types. Maximum observed
texture declarations per stage are 0 in VS and 11 in PS, so runtime layouts can
be sparse. Real texture views, compatible dimensional conversions and ownership
have not been implemented in a native cache. This is a shader-side repair only.

Two VS (`5B9B7484417FB9B6`, `A4827826E2589916`) respectively drop 20 and 10
memory exports. They are flagged separately and cannot be treated as correct
native replacements. Their guest memory-write behavior needs implementation or
a verified equivalent copy path. No MW2-specific image-copy hooks were copied.

**All 335 modules that cleanly validated in the original comparison remain
byte-identical in the prototype.** The change newly validates 115 pixel programs;
450 of 455 programs validated without dropped exports in comparison02. The typed
view adaptation increases that to **453/455**, covering all 306 PS. Of the prior
450 clean modules, 217 remain byte-identical and 233 differ only in their texture
binding decorations. No other SPIR-V instruction changes. Pixel metadata in
this capture only reports color output zero, but that does not establish
whole-game MRT/depth requirements.

Evidence: `analysis/bo3_mw2_comparison01.json`,
`analysis/bo3_mw2_comparison02.json`, `analysis/bo3_mw2_comparison_delta02.json`,
`analysis/bo3_getweights_census03.json`,
`analysis/mw2_shader_tool_sources.json`,
`analysis/mw2_shader_tool_staged_sources.json` and the two tool build logs.
Raw programs/SPIR-V stay under ignored build/runtime directories.
New evidence: `analysis/bo3_mw2_comparison03.json`,
`analysis/bo3_texture_view_validation03.json`, `analysis/mw2_comparison_results03.log`.
The staged-source manifest records both adaptation helpers and preparation hashes.

### Decision for the next implementation gate

Use exact GPU-loaded programs and shader-based vertex fetching for the first
native correctness prototype. This avoids making original container declarations
a prerequisite for the 147 observed raster vertex programs. It does not resolve
the remaining two memory-export VS or prove software fetching will be fast on
Maxwell; common fetches can be optimized after correctness is measured.

Before live native rendering, define a consistent binding contract for guest
float/fetch/bool/loop registers, buffer snapshots and mixed texture views. The
existing SA2-derived modules use a different interface and cannot simply be
paired with these vertex modules. Preserve both libraries as comparison corpora.
The typed shader interface is now defined and checked offline. A bounded probe
has been added at emulated Vulkan IssueDraw, after VS analysis, to capture actual
memory-export registers and loaded microcode. See [memory exports](BO3_MEMEXPORT.md).
Next, implement compatible texture views, memory-export behavior and the native command
backend, retaining explicit unsupported reports. There is still no native game
frame, driver-execution validation or Switch performance result from this test.
