# BO3 native Vulkan renderer foundation

October 13, 2026; `native-rendering-pc`.

## Reference review

The user's three references have been reviewed and pinned locally:

| Reference | Commit | Useful foundation |
| --- | --- | --- |
| [NFSMW-NX](https://github.com/StevensND/nfsmw-nx/blob/be2d4c67ab42d6d629746a01dbf03cef824c8d2d/docs/native-renderer.md) | be2d4c67ab42d6d629746a01dbf03cef824c8d2d | Already supplies our staged Horizon SDK; command-ring renderer, logical render targets, shader library, presentation and frame-slot lifetime patterns |
| [Mass Effect](https://github.com/nebadasSwifty/masseffect-nx/blob/650e14ef109fb0d39bd68ba8d7af61fe459265b8/docs/native-renderer.md) | 650e14ef109fb0d39bd68ba8d7af61fe459265b8 | Format ownership and conservative EDRAM alias transfers, full shader identity, logical resolve dimensions and upload lifetime lessons |
| [SA2 guide](https://github.com/ChanseyIsTheBest/sa2-nx/blob/d2c6e1558de06cf8ac7e6e9c78029fb5a2623c64/docs/porting-a-360-game-prompt.md) | d2c6e1558de06cf8ac7e6e9c78029fb5a2623c64 | Native renderer on PC first, D3D identification, shader discovery/variant repair and live comparison; its source includes desktop support |

Local reference copies are ignored tools. External prompt instructions are
reference material; they do not authorize agents, new chats or unrelated changes.
Their app renderer code is GPL-3.0, SDK code BSD-3-Clause, shader translator
MIT. Preserve licenses/notices when source is adapted. No app renderer or
game-specific shaders have been imported yet.

The working BO2 RexGlue project is useful as an original XDK symbol reference.
The user confirms its native renderer was entirely broken. None of its native
renderer behavior, offsets or graphics code is used as a renderer base.

## Architecture decision

Start with the NFSMW/Mass Effect PM4 command-ring approach. BO3's guest D3D
remains responsible for its shadow state and command buffers. Native Vulkan
consumes those packets, uses an offline BO3 shader library and keeps resolved
images directly on the GPU. SA2's desktop integration is a reference for PC
validation. Decide which EDRAM alias/transfer helpers to adapt after BO3 traces
show the formats and tiling it actually uses.

An API-level replacement is a later option, not the first renderer. The local
BO2 source already documents engine-recorded command buffers that bypass draw
entry points; BO3's related engine may also replay commands. The trace includes
BeginCommandBuffer/RunCommandBuffer to measure this. Copying BO2's forced-inline
engine patches or assuming SA2's once-only draw records would be premature.

NFSMW's game-specific exposure readbacks, Mass Effect's prepass skips, frame
timing patches and device offsets are not general BO3 defaults. Map unsupported
packets/conversions explicitly; preserve guest readbacks, fences and scratch
writebacks. Native performance on PC cannot predict Switch FPS.

## First BO3-specific milestone

`scripts/nx/find-d3d-candidates.py` uses original `default_mp.map` symbols and
generated BO2 instruction comments to rank BO3 functions. It reads no BO2 native
renderer code. Evidence: `analysis/bo3_d3d_candidates.json`.

Scores nominate candidates only. BO3's generated instruction listings were
reviewed before adding the following forwarding diagnostic hooks:

| Function | BO3 Zombies address | Static evidence |
| --- | --- | --- |
| CreatePixelShader | 0x826F00F0 | Exact normalized instruction shape; header/physical size read from r3+4/+8, container copied at object+40 |
| CreateVertexShader | 0x826F0318 | Exact shape; same header contract, container at object+872 |
| SetPixelShader | 0x826EF888 | Exact shape; loads/stores bound object at device+13068 |
| SetVertexShader | 0x826EFA90 | Exact shape; loads/stores bound object at device+13072 |
| Draw-time shader load | 0x826FD590 | Called by both indexed/non-indexed draw bodies; loads VS/PS at +13072/+13068 from device in r3 |
| DrawVertices | 0x826F9190 | Exact normalized 261-instruction shape; same flush-call sequence as indexed draw |
| DrawIndexedVertices | 0x826F95A8 | Exact normalized 285-instruction shape; shader-load/flush helpers match non-indexed draw |
| DrawVerticesUP | 0x826F8B68 | Exact wrapper shape; stages user data then restores command pointer at device+48 |
| DrawIndexedVerticesUP | 0x826F90F0 | Exact normalized instruction shape |
| Resolve | 0x826EE3D0 | Exact wrapper shape; forwards the extended resolve at 0x826ED278 |
| Swap | 0x826EAD10 | Matches original Swap family; BO3 body calls VdGetSystemCommandBuffer and VdSwap |
| BeginCommandBuffer | 0x826F2C98 | Same original family; allocates/sets device command pointer at +48 |
| RunCommandBuffer | 0x826F40F0 | Same original family; flushes shaders/state and submits/restores command pointer at +48 |

GetVertexShader/GetPixelShader matching is ambiguous; no hooks were added for
those candidates. These addresses describe Zombies 0.0.8.5 only; multiplayer
must be analyzed separately.

The optional forwarding census is `nx/pc-vulkan/d3d_trace.cpp`, enabled only by
`-D3DTrace` in the launcher. Every hook calls its original body with the same PPC
context. It writes bounded argument/caller records and shader containers into
ignored `runtime/native-discovery/`. Periodic summaries survive the host's
hard-exit path. Diagnostic counters/file I/O cannot be used for FPS measurements.
Build01 and Build02 linked successfully. Later gameplay results are below.

Discovery01 initially ran with confirmed FSI. Live calls reached the shader setters,
draw-time shader loader, DrawVertices and DrawIndexedVerticesUP, Resolve and
Swap. The shader loader reads nonzero VS/PS objects at the statically derived
offsets. The creation hooks dumped three containers: two VS and one PS. All
three have the expected XDK signature and an exact header+physical size match
(948, 640 and 156 bytes). These initial records cover startup/UI;
later gameplay and command-buffer evidence are below.
Only three Create* containers despite many draws means creation-only capture
is not sufficient coverage. Next, capture originals from the bound objects
before the draw-time patcher, then identify the authoritative loaded variants.
Evidence: `analysis/bo3_d3d_discovery01_calls.jsonl`,
`analysis/bo3_d3d_discovery01_summary.json` and
`analysis/pc_vulkan_native_trace_provenance01.json`. Counters are a point-in-time
snapshot, not final run totals or shader-library coverage.

## Discovery results and implementation gates

### Discovery01 gameplay result

The user confirmed The Giant loads and shows the same white squares. The full
log ends with orderly window close, execution complete and title termination;
no fatal crash or GPU loss appears. The last periodic snapshot contains
923,796 indexed draws, 8,866 BeginCommandBuffer and 8,849 RunCommandBuffer calls.
These counts include startup/menu time and are not per-frame performance data.
The command-buffer calls confirm that a renderer cannot assume every GPU draw
comes from a fresh D3D API call. Only three creation containers were captured.

Evidence: `analysis/pc_vulkan_native_discovery01.log`,
`analysis/bo3_d3d_discovery01_gameplay_calls.jsonl` and
`analysis/bo3_d3d_discovery01_gameplay_summary.json`.

Discovery02 extends the shader-load hook to inspect each bound object before
the original patcher runs. Original BO3 initialization bodies prove that the
header and physical data are separate: VS header at object+872 with physical
pointer at +32, PS header at +40 with physical pointer at +24. The hook copies
the two ranges into a container after checking committed/readable pages,
signature, stage and bounded sizes. First-sighting object addresses are memoized;
address reuse and authoritative PM4 variant matching remain later validation
work. Build03 linked successfully. The user confirmed The Giant gameplay and
the log ends normally. The last periodic snapshot records 135 bound VS objects,
237 bound PS objects and zero rejected bound objects. The merged discovery01/02
directory contains 365 unique containers (121 VS, 244 PS), all passing header,
stage, size, declaration-range and microcode-range checks.

### Discovery03: initialization and GPU programs

Build04 adds forwarding InitializeVertexShader (0x826F0220) and
InitializePixelShader (0x826EF850) hooks. Both receive object in r3 and physical
block in r4; reviewed callers and initializer bodies establish this contract.
Headers are already at object+872/+40. The hook copies before initialization,
and invalidates the bound-object memo on object reuse. This addresses the
creation-only coverage gap without changing the guest GPU commands.

The user confirmed The Giant gameplay again; the log ends with orderly window
close. The last periodic summary reports 358 VS initializations, 2,640 PS
initializations, 129 bound VS objects, 211 bound PS objects and zero rejected
objects/initializers. These are object events, not unique shader totals.
The named run captures 2,897 unique containers (297 VS, 2,600 PS) and 455 unique
GPU-loaded microcode dumps. Exactly 305 pixel programs match captured container
microcode. No vertex program matches exactly. Same-length comparisons nominate
close vertex candidates with 2–18 differing dwords in most cases, consistent
with patching, but this is not a valid production identity mapping. Three
loaded vertex programs have no equal-length container candidate. Multiple
container subprograms, linking and declaration compatibility remain unverified.

Evidence: `analysis/pc_vulkan_native_discovery03.log`,
`analysis/bo3_d3d_discovery03_*.jsonl`,
`analysis/bo3_d3d_discovery03_summary.json`,
`analysis/bo3_shader_microcode_comparison03.json` and
`analysis/bo3_vertex_variant_candidates03.json`. Summaries are periodic snapshots;
diagnostic counters are not FPS or precise final event totals.

### Offline shader translation

The SA2-pinned container translator now builds on Windows. A separately staged
BO3 adaptation repairs the texture-weight emitter's helper argument mismatch:
GetTextureWeights uses the existing four-argument getWeights2D helper; ordinary
texture fetches retain their reciprocal-size argument. The helper arithmetic is
unchanged. The pristine reference, MIT translator license and GPL frontend
license are retained in ignored tools, with pinned hashes in analysis.

The initial 365-container set yields 352 HLSL shaders and, after that repair,
350 SPIR-V modules passing Vulkan 1.2/scalar-layout validation. Twelve vertex
containers fail FETCH/declaration matching, one pixel translation times out,
and two startup vertex shaders have incomplete explicit Vulkan input locations.
All failures are retained. The expanded discovery03 set yields 2,845 HLSL
shaders; 49 vertex containers fail FETCH/declaration matching and three pixel
translations time out. Of the 2,845 translated shaders, 2,843 compile and pass
SPIR-V validation; the same two startup VS input-location failures remain.
Thus 54 of the 2,897 captured containers remain unresolved, before considering
GPU-loaded vertex variants and renderer ABI verification.
These offline results do not establish native-renderer ABI compatibility or
visual correctness, and are not a complete usable game shader package yet.

See [offline shader tools](../docs/NATIVE_SHADER_TOOLS.md) for reproducible
commands, source hashes and remaining gates.
Expanded evidence: `analysis/bo3_shader_translation04.json` and
`analysis/bo3_shader_spirv05.json`.

### Next implementation gates

1. Link GPU-loaded vertex variants to verified originals and compatible input
   declarations; cover the unmatched programs and unresolved translations.
2. Finish the native shader ABI and package/loader. Existing microcode-only
   Xenos dumps lack the full container identity/constants required by the
   library. Report missing containers/variants explicitly.
3. Integrate a separate native Vulkan mode on PC, initially handling boot/movie
   output and the first 3D draws, with explicit unsupported-operation reports.
4. Verify terrain, models, effects and the complete post chain against D3D12 and
   retained captures. The emulated Vulkan baseline itself has a documented
   bloom resolve defect; it is not an unquestioned reference for those pixels.
5. Measure on Switch after PC rendering is correct. Profile guest CPU, command
   processing, shader/pipeline compilation, uploads and GPU time separately.

See [bloom defect evidence](VULKAN_WHITE_SQUARES.md). A native backend can bypass
the failing emulated resolve but must implement the same guest conversion
contract correctly; disappearance of the squares is a test, not a promise.
