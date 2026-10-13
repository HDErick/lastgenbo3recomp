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
Build01 and Build02 linked successfully. Live map validation is pending.

Discovery01 is running with confirmed FSI. Live calls reach the shader setters,
draw-time shader loader, DrawVertices and DrawIndexedVerticesUP, Resolve and
Swap. The shader loader reads nonzero VS/PS objects at the statically derived
offsets. The creation hooks dumped three containers: two VS and one PS. All
three have the expected XDK signature and an exact header+physical size match
(948, 640 and 156 bytes). The original game continues running. These are
startup/UI observations; gameplay and command-buffer coverage are pending.
Only three Create* containers despite many draws means creation-only capture
is not sufficient coverage. Next, capture originals from the bound objects
before the draw-time patcher, then identify the authoritative loaded variants.
Evidence: `analysis/bo3_d3d_discovery01_calls.jsonl`,
`analysis/bo3_d3d_discovery01_summary.json` and
`analysis/pc_vulkan_native_trace_provenance01.json`. Counters are a point-in-time
snapshot, not final run totals or shader-library coverage.

## Next implementation gates

1. Verify live shader/create/draw/command-buffer calls on The Giant; validate
   container headers and compare loaded variants with their originals.
2. Build BO3's shader translation/package tools. Existing microcode-only Xenos
   dumps lack the complete container identity/constants required by the native
   library. Add missing-container and patched-VS discovery counters.
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
