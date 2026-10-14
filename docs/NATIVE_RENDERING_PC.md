# BO3 native rendering development on PC

October 13, 2026. Branch `native-rendering-pc`, based on `NX` after the first
Switch startup/application logs were received.

## First baseline

`nx/pc-vulkan` builds BO3 Zombies on Windows with the same pinned SDK source
used by the Switch build (`tools/nx-sdk`, NFSMW-NX revision
`be2d4c67ab42d6d629746a01dbf03cef824c8d2d`). Vulkan is enabled and D3D12 is
disabled. The default target uses the Xenos emulation renderer. A separate
native Vulkan GPU plugin is now available as an incomplete correctness prototype.

## Native plugin milestone (October 13)

`nx/native` builds `rexgpu-bo3native.dll`. Launch it with:

```powershell
scripts/nx/launch-pc-vulkan.ps1 -Native -Background -LogName pc-native.log
```

The plugin reuses RexGlue's PM4 parser, kernel integration and Vulkan presenter;
it owns its targets, shader modules, pipelines, uploads and submissions. It does
not instantiate the Xenos Vulkan command processor or its EDRAM draw backend.
Shaders use the MW2-derived BO3 ABI and shader-based vertex fetching. The
combined plugin has GPL-3.0-only provenance; its license is in `nx/native/LICENSE`.
The original SDK components retain their license notices.

Two exact memory-export programs now use bounded CPU copies. Eight captures
matched completed emulated GPU readback byte for byte, including untouched
destination gaps. Seven captures had nonzero writes. The actual C++ planner
also matches all eight captures and rejects altered offset/format/bounds state.
Program SHA-256, draw state, constants, byte order, physical bounds and overlap
are checked before writes. Prior native submissions retire before CPU writes;
source bytes are snapshotted and texture contents are rehashed at draw entry.
See [memory-export evidence](../analysis/BO3_MEMEXPORT.md).

Native runs 06 and 07 ended with orderly window closure. The user clarified that
the last two runs showed visible frontend output through the loading screen and
were closed because performance was too poor to test further. They reported an
earlier loading crash; it has not yet been tied to a captured fatal report.
The early zero-RGB readbacks sampled only the first five startup draws and do
not establish a persistent black screen. Visible native 3D gameplay is unproven.

Run 08's slow interval recorded 2.13 swaps/s, with 10.64 seconds spent in export
processing over 14.11 seconds, versus 2.6 ms shader translation and 8.8 ms
pipeline creation. This is frontend evidence, not an in-game FPS benchmark.
Texture uploads now share the draw submission; startup image readbacks require
`-NativeReadback`. Linear texture rows use dword endian conversion. The copy
planner now calculates contiguous exports directly, stops at export bounds and
snapshots only used source extents into reusable storage. Run 09 tests these
changes with BC1/BC2/BC3 texture decoding for the missing HUD textures. The user
confirmed that menu options are now visible, but performance starts better and
progressively worsens. Run 09 closed normally at the user's request. A comparable
interval fell from 13.33 to 3.04 swaps/s, with export elapsed time rising from
283 ms to 7,998 ms despite similar source volume. A later frontend interval
reached 1.17 swaps/s with increased draw/submission counts. These scopes measure
elapsed time, not CPU execution time; scheduling and memory waits are unresolved.
Build 14 splits export planning, permission guards, snapshotting and writes, and
records Windows thread CPU time to distinguish execution from elapsed waits.

Run 10 isolated an export bottleneck: in a 15.35-second interval, permission
guards spent 11,750 ms, versus 4.7 ms snapshotting and 6.0 ms writing; export
thread CPU time was 11,078 ms. The expensive path was repeated Windows
`VirtualQuery` region scanning, rather than shader compilation or copy volume.
Build 15 checks fresh, committed physical-heap allocation/protection regions
through RexGlue's locked page table instead. It caches no permissions, rejects
reserved/inaccessible ranges and keeps source snapshots and write bounds.
Ten tests cover unaligned boundaries, read-only pages, reserved pages, no-access
pages, overflow and the exclusive heap end. In run 11, the user confirmed much
faster intro playback, navigable menus despite slowing after profile loading,
and a working map-load cutscene. Map previews may be invisible; gameplay loads
with a black 3D scene and visible HUD. This does not validate native gameplay.
The run ended with orderly window closure. Permission guards now take roughly
one millisecond per 30 swaps. Later menu intervals still fall to roughly 5–6
swaps/s, with 4–6 seconds in queue/fence work and 2,250–2,520 submissions per
30 swaps. Shader/pipeline work is negligible in these warmed intervals.
Builds 16/17 batch up to 32 draws with distinct upload ranges and descriptor sets,
attachment dependencies, and completion fences before storage reuse. It flushes
before CPU memory exports, copies, presentation, readback, new-target clears or
upload exhaustion. Early unsupported packets do not flush an untouched batch.
Run 12 is the batching comparison. Recent warmed intervals record about 20–24
swaps/s, with roughly 400–500 submissions per 30 swaps rather than 2,250–2,520
in the earlier menu run. This is a presentation-rate observation across user
navigation, not a matched-scene benchmark. The user confirmed improved performance,
but gameplay still has a black scene with visible HUD. Run 12 closed normally.
At swap 2,760 the counters reported 5,390,953 multisample-target rejects and
1,791,130 polygon-mode/depth-bias rejects, against 84,391 accepted draws from
7,562,733 packets. Substantial geometry is therefore omitted. A covering pass
or incorrect diagnostic presentation target remains possible, but no occluding
draw has been identified. Next implement/validate the observed MSAA and raster
states, then follow native target resolves and the actual guest frontbuffer.

Build 18 adds actual host-supported 2x/4x color/depth attachments, with sample
counts in target/pipeline identity and per-format capability checks. Multisample
diagnostic output is resolved to a separate single-sample image before sampling.
Filled dual-face polygon mode is accepted; non-filled modes remain unsupported.
Dynamic depth bias uses the SDK's preferred-face selection and depth/slope units;
independent per-face bias and float24 quantization are still not reproduced.
Run 13 logs Intel Arc sample masks `0xF` and creates an 880x720 2x target.
Per-target activity identifies the latest presented target. The user reported
the scene still black with HUD, with much worse in-game performance. The run
closed normally. Warmed intervals fell to 2.5–3.6 swaps/s, with around 1,600–1,760
submissions per 30 swaps. Stencil rejection became a major uncovered path;
textures, numeric conversion, mips/cube views and guest resolves remain gaps.

Builds 19/20 add stencil operations and dynamic per-face masks/references,
following the SDK's device fallback for separate masks. Storage format aliases
are canonicalized, allowing format 10 to use its underlying 10-bit UNORM target.
All texture bindings are inspected before uploads/descriptor allocation, and
command buffers with no GPU work are discarded without submitting or waiting.
An optional `-NativeSceneView` diagnostic selects a large multisampled target
only if it received an accepted draw in the current frame; otherwise it uses
the ordinary latest target. Its image is resolved before presentation. This
probe may omit HUD/final effects and is not guest frontbuffer/resolve support.
Run 14 selected a black 880x720 2x menu target (two accepted draws per frame),
obscuring the menu after profile loading and preventing the map comparison.
It closed normally. Build 21 keeps normal presentation initially and uses F8
to toggle this diagnostic while the game window is foreground. In run 15 the
user confirmed improved menus and visible map previews. Gameplay remains black
with HUD; F8 removes the HUD and the selected MSAA target is also black. The
toggle was logged with a current-frame target available, and shutdown was normal.
This makes a HUD-only cover insufficient to explain that target's black output;
missing geometry/shading and target ownership/selection remain unresolved. Clearing
and shared depth/stencil ownership are still incomplete, so accepting stencil
state alone does not establish correct stencil contents.

Build 22 adds an F9 depth view under `-NativeSceneView`. It samples depth sample
zero through a depth-only MSAA image view, without the game pixel shader,
textures, blending or HUD. Clear depth 1 displays black; values nearer zero
display progressively lighter grayscale with a nonlinear contrast curve.
Normal presentation remains active initially. The diagnostic presentation
modules pass `spirv-val --target-env vulkan1.2`. Run 16 closed normally; the user
did not try F9 and requested reopening. In run 17 the user reports that F9
remains black. The log confirms depth view ON with a current-frame scene
available. This run ended with normal window closure. Empty depth alone cannot
exclude geometry because depth-only draws, tests, write enables, clears and
shared depth ownership were incomplete.

Build 23 adds `kDepthOnly` draws with a vertex shader and no fragment/color
attachment. Matching depth base, format, dimensions and sample count now share
one depth/stencil surface across color targets and depth-only passes. Creating
a new color target no longer clears an existing depth surface. F9 can inspect
the last current-frame large MSAA depth surface even if only a depth pass drew
to it; normal/F8 presentation still selects color targets. Logs count accepted
depth-only draws and identify each target's pass type. Unsupported cube/volume
shader views are checked before Vulkan object creation, and common omitted
primitive/mode packets avoid exception unwinding. The build and repository audit
pass. In run 18 the user reports the view remains black, and F9 ON is logged
with a current-frame surface. The game closed normally. The log shows zero
`kDepthOnly` submissions: this scene uses color/depth mode instead. The 880x720
2x target did receive 51,612 accepted draws in one 120-swap interval, while
cube/volume and mip bindings excluded many others. The last 30-swap interval
was 1.83 swaps/s, with 7,780 ms queue/fence time and 1,274 submissions over
16,377 ms. Shader translation was 13.8 ms; this interval does not attribute the
slow frame rate to shader compilation. This is a bounded shared
surface implementation, not general EDRAM overlap/alias handling. Guest clears,
different-extent depth aliases and native resolves remain unsupported.

Build 26 uses the SDK's host viewport/scissor normalization and extends the VS
push interface from 32 to 48 bytes. The VS reconstructs W from reciprocal-W
when required, reverses pre-divided XY/Z, and applies all three host NDC scales
and offsets. This covers independently disabled viewport axes, negative scales,
window/half-pixel offsets and guest -W..W Z. The old path only changed XY and
always treated the exported fourth component as W. Explicit depth clamping,
user clip planes and exact float24 quantization remain gaps. SDK microcode
analysis now proves when a color/depth-mode prepass can omit its pixel shader;
kill, alpha coverage, depth output and memory-export side effects prevent that
bypass. Omitting a shader also disables color writes, and its unused textures
are not required. Logs identify the bypass and MSAA pipeline state.

Offline validation caught an overlapping push member in the first attempt;
the staged declaration now advances float4 members by 16 bytes. The corrected
453 raster modules and both structurally valid dropped-export modules pass
`spirv-val`; those two memexport modules remain excluded from native pipelines.
Build 26 links, and run 19 is open for normal/F9 comparison. Early logs confirm
accepted PS bypass draws and no backend failure. In run 19 the user reports
the same black output and no noticeable regression; F9 activation is logged
with a current-frame surface. It closed normally. Audio was reported inaudible,
but the diagnostic launcher explicitly supplies `--audio_mute`; this run does
not establish an audio defect, and rendering remains the priority.
The live run 19 logs MSAA prepasses with `RB_DEPTHCONTROL=0x002002C7`, a greater
depth comparison with writes enabled, followed by `0x002002E3` greater-equal
color passes. The current initial depth is 1 and guest resolve clears are still
ignored. This is a concrete correctness gap and the next investigation target:
decode the requested depth/stencil clear and its covered rectangle, preserve
partial clears, and apply it to matching shared surfaces. Do not substitute a
per-frame full clear or change the depth comparison as a rendering fix.

Build 28 implements depth/stencil clears requested by `RB_COPY_CONTROL` in
resolve packets. Physical VF0 readability and finite coordinates are checked
before the SDK resolve decoder. Its scissor/window/8-pixel rectangle is recovered
from tile-relative coordinates; packed D24/float24 depth and low-byte stencil
are decoded, and out-of-range float24 clears are rejected. `vkCmdClearAttachments`
clears only the covered portion of matching shared images, once per image.
Clears arriving before image creation are replayed from bounded per-surface
history; newer covering rectangles replace older ones. Resolving to guest
textures is still rejected separately. Rasterized clear primitives, color
clears, general EDRAM overlap ownership and float24 values above 1 are not covered
by this change. Build 28 links. In run 20 the user reports the same black output;
F9 ON is logged with a current-frame surface, followed by normal window closure.
The last periodic report (swap 3120) still records zero resolve-clear requests
and applications, so this run did not exercise the new clear path. It counts
50,088 cube/volume, 15,926 mip, 9,063 rectangle and 182,391 point rejections
cumulatively; these are packet counters, not counts of visible objects. The
main 880x720 2x target received 36,365 submitted draws in the last 120-swap
interval, but submitting draws does not prove samples passed depth/stencil or
that visible color was written. A 30-swap interval took 20,339 ms, including
11,194 ms queue/fence time and 1,560 submissions; shader translation was 3.4 ms.
No native backend/presentation failure is logged. The next investigation is
rejected rectangle-list state/shaders and raster clear behavior, followed by
pixel/depth readback before final presentation. Greater-depth prepasses with
initial depth 1 and no supported guest clears remain a concrete correctness
gap; rectangle clears are a hypothesis until their actual draws are identified.
F9 may see cleared depth if a
resolve-clear happens after the scene draw; its uniform output alone does not
prove whether the scene wrote depth earlier in that frame.

### Rectangle draws and depth-clear alias investigation (runs 21–23)

BO3 rectangle lists now use a generated geometry shader to expand three input
vertices to four, with the SDK's longest-edge rule and only the VS's available
interpolators. Four output-interface masks pass SPIR-V validation. Run 21 still
rejected rectangles because the presentation-only Vulkan device profile had
not enabled geometry shaders. Run 22 enables the SDK's full graphics feature
profile; it still uses the separate BO3 command processor. Logs confirm actual
rectangle execution and precise occlusion-query results.

The optional `-NativeDrawProbe` records the first four draws of each rectangle
or MSAA pipeline after the existing submission fence. Run 22's main clear passes
1,267,200 samples, but world prepasses pass zero. The user reports that F9 shows
the top half white and bottom half black. The selected depth-only clear surface
is not proof of world geometry; run 23 prefers the current scene color target's
depth surface instead.

Depth base 68 is cleared through a 440x720 4x view and rendered through an
880x720 2x view. Both have 880 samples per row and 1,440 sample rows, but the
cache held separate images. Build 33 recognizes the reviewed XDK constant-depth
clear VS and optional pass-through PS by SHA-256, checks their state and uploaded
vertices, and propagates depth/stencil clears between matching sample layouts.
Partial rectangles are preserved; prior clears are replayed when an alias image
is created. Eight mapping/rejection cases pass. This does not implement general
EDRAM overlap, color clear aliasing or arbitrary shader approximation. Run 23
closed normally with zero recognized clears. The user reports F9 fully black.
The guard prevented the repair from running, so build 34 adds a bounded
diagnostic of the observed clear program's failed guard and transformed edges.
Run 24 closed normally and remained black. It identified half-pixel edges near
0.5: the SDK enables the D3D pixel-center adjustment. Build 35 uses SDK 16.8
rounding and the top-left pixel-center rule for the observed clear's disabled
MSAA raster coverage. Per-sample raster coverage is explicitly refused by this
whole-pixel clear path. Sixteen mapping/boundary cases pass.

Run 25 remained black and closed normally. Its diagnostic found three identical
positions and VF95 address zero. A direct review of the captured binary proves
that this exact canonical shader uses VF0, stride seven, despite the saved SDK
text disassembly naming VF95. The native translator metadata independently
agrees. Build 36 corrects the fetch ownership and verifies that metadata before
reading the uploaded snapshot. `analysis/native_clear_shader_review01.json`
records the hashes and slot evidence. Run 26 ended after the user closed it. Its log confirms
base 68 is cleared to zero across sample layouts. The sampled full-screen
BE68717E4DF4E8D8 depth pass now passes 1,267,200 samples, versus zero in runs
22–25; the subsequent sampled 03EEAA01C3B394BF pass still passes zero. This is
actual GPU coverage evidence. Later gameplay prepasses with depth state
0x087087C7 also pass nonzero samples (observed counts include 14,130, 23,735 and
63,767), versus zero before the repair. This establishes accepted depth-tested
coverage, but still does not prove the final color or diagnostic view is correct.
The user reports normal gameplay remains black with HUD, while F9 gives a white
screen with a faint viewmodel/gun. The display shader maps depth zero to white
(`pow(1-depth, 0.2)`), explaining the bright background after the reverse-depth
clear repair. This is partial visible depth output, not a native color frame.
The complete log is `analysis/pc_native_backend_boot26_clear_vf0.log`; the older
snapshot is retained separately.

All 32 sampled 880x720 scene draws in this run omit the PS and have color mask
zero; 20 have nonzero GPU coverage. The final periodic report counts 112,455
cube/volume-view rejections and 52,778 mip rejections. Color/material draws are
still blocked by these resource gates, with guest resolves also unsupported.
Implement typed texture views and their real backing/upload/mip ownership next;
do not substitute an arbitrary 2D or black image for a required cube/3D texture.
These are cumulative packet counts, not counts of visible models, and do not
prove that resource support alone will finish the renderer.

The user also reports a host-looking storage-selection overwrite warning and
closed the game. No causal link to this GPU change is established. The log ends
with window closure and title termination; no native backend failure is logged.

Evidence: `analysis/pc_native_backend_boot21_rectangles_coverage.log`,
`analysis/pc_native_backend_boot22_rectangles_enabled.log`,
`analysis/native_rectangle_validation01.json`,
`analysis/native_edram_clear_tests01.log`, and provenance manifests 21–23.
The BO2 renderer was reviewed for useful ideas; no code was imported. See
[BO2 native renderer assessment](../analysis/BO2_NATIVE_REFERENCE.md).

### Prepared texture/resource build 43 (historical; game launch deferred)

The user requested further implementation without opening the game. The native
runtime now supplies the ABI's 2D, cube and 3D bindings, with real six-face cube
images and volume images. Image allocation, format capability checks, image
views and barriers include every mip/array layer. Upload reservation covers
decoded faces, volume slices and aligned subresources.

`texture_upload.cpp` uses the pinned SDK's guest subresource/layout helpers for
separate base/mip allocations, non-packed levels, packed tails and 3D tiling.
It retains BC decoding and the linear-video row path. Both allocation hashes
are checked on cache hits. The layout cache stores immutable arithmetic only;
physical permission checks and content hashes remain fresh. Sampler filtering
uses the fetch settings and normalized LOD bounds. LOD bias remains in the
translated shader, avoiding a second application in the sampler.

The SDK's dimension rules govern mixed bindings. 2D access to a real 3D texture
gets a separate base-level, slice-zero image decoded with the **3D** layout.
Unbound or dimension-incompatible bindings get a correctly typed zero descriptor,
following the SDK's null-view behavior. A compatible texture with unsupported
format, signed conversion, stacked layout, border size or volume-filter semantics
is still rejected; these zero descriptors do not substitute for missing assets.

Resolve ownership now records the SDK's potential destination byte ranges,
including tiled/partial aliases. Texture, vertex/index and CPU-export readers
reject intersections with unresolved output. Verified CPU exports release only
the bytes they actually overwrite. This is a conservative availability gate,
not completed resolve conversion: shader readers of native output, exponent
conversion, depth resolves and guest CPU writeback still require implementation.
Arbitrary guest CPU writes/reallocation are not tracked yet, so reused ranges
may remain unavailable unnecessarily. This may expose additional skipped draws
or missing menu effects when testing resumes.

F9 now maps reverse-depth clear zero to black and nearer values brighter; this
corrects the diagnostic display's previous white background. It does not add
color rendering.

Build 43 links. The final offline fixtures pass 30 texture checks and 13 ownership
checks, with no game or GPU rendering launched. They cover row padding, endian
orders, separate/packed mip storage, cube face/level ordering, linear/tiled volume
slices, 3D-as-2D access, BC clipping, missing bases, physical/upload bounds and
partial ownership release, sub-dword endian mip extents and the BC alpha override.
Tiled fixture creation uses SDK helpers, so these
tests do not independently verify that SDK's hardware tiling parity. No in-game
visual improvement or performance claim is established by this prepared build.

Evidence: `analysis/native_texture_upload_tests06.log`,
`analysis/pc_native_backend_build43.log`,
`analysis/native_texture_validation01.json`, and
`analysis/pc_native_provenance27_prepared.json`.
Reproduce the CPU tests with `scripts/nx/test-native-texture-upload.cmd` after
the normal PC build. The following stage implements native resolve conversion
and reader ownership.

### Native color resolve build 48

The native backend now implements supported color resolves into its own GPU
images. An unchanged single-sample format uses an image copy preserving bits;
conversion uses an image-only pixel shader with exponent scaling, optional R/B
swap and destination-format quantization. Multisample input averages **all**
samples (2x or 4x); individual/subset selections are refused because host sample
ordering has not been mapped to Xenos. The implementation copies before applying
supported post-resolve color and depth clears.

Source lookup requires an existing drawn color image with exact base, storage
format, pitch and sample count. Source rectangles account for tile offsets,
64-bit tile pairs and MSAA sample coordinates. Destination anchor arithmetic is
checked against the SDK's decoded tiled address. The CPU-shadow availability
journal remains conservative; successful GPU output does not make CPU bytes
fresh. Covered GPU regions are tracked separately from zero-initialized padding.
Supported destination formats are R8/RG8/RGBA8 UNORM (including storage aliases),
packed 10/10/10/2 UNORM, and R/RG/RGBA 16- or 32-bit floats.

Compatible tiled 2D fetches can read these images. Each requested mip must have
matching backing address, pitch, host format, endian mode and complete recorded
rectangle coverage. Copies crop padded resolve allocations into the sampler's
logical image. Content epochs prevent stale cache reuse. Mixed CPU/GPU mip
chains, resolved cubes/volumes, depth/array output and 64/128-bit endian modes
remain refused. Different destination layouts invalidate overlapping old views.

The SDK physical-memory invalidation callbacks enqueue guest writes/reallocation
without issuing Vulkan calls on guest threads; the GPU thread clears intersecting
coverage. Verified native exports also invalidate explicitly because direct
physical writes can bypass virtual-alias faults. Partial writes do not release
unwritten CPU-shadow bytes. Full arbitrary CPU reuse may consequently still be
rejected; CPU readback/data providers and complete EDRAM ownership transfers are
unfinished. Post-resolve FP16 clears currently accept only zero/one endpoints;
their wider format/word-order parity is unproven. Presentation still uses the
latest drawn target rather than following the guest frontbuffer resolve.

Offline shader reflection found that the shared source file's
`combinedImageSampler` annotations were leaking into resolve and depth entries
at the same binding. Entries now compile with isolated resource declarations.
The two resolve modules and depth diagnostic use actual image-only descriptors;
resolve push offsets are checked as 0/8/16/20/24 in a 32-byte range. This repairs
an interface mismatch, but previous F9 observations cannot establish numerical
depth correctness under the old descriptor declaration.

Build 48 links the executable and native plugin. Fixtures pass 36 texture,
13 availability-ownership and 26 resolve planning/coverage checks. All five
presentation/resolve modules pass Vulkan 1.1 structural validation. CPU fixtures
and reflection do not prove GPU numerical output, visible world rendering or
performance. The user authorized the next game launch after implementation;
the comparison result is pending.

Evidence: `analysis/pc_native_backend_build48.log`,
`analysis/native_texture_upload_tests07.log`,
`analysis/native_resolve_validation01.json`, and
`analysis/pc_native_provenance28_prepared.json`.

Run 27 was opened after the user authorized launch, with normal native
presentation and the F9 diagnostic available. An early live report records
2,164 completed color resolves and 767 resolved-texture reads. Vulkan creation
and submission succeeded for those operations; their numerical output and
visual correctness remain unconfirmed. This is a partial live-run observation,
not a complete gameplay result. Unsupported copies currently return failure
to the PM4 parser, causing repeated backend-failure log messages; preserving
consumed-packet handling while keeping unsupported counters is a follow-up.
Evidence: `analysis/pc_native_backend_boot27_color_resolves_snapshot.log` and
`analysis/pc_native_provenance29_test.json`.

Run 27 subsequently reached The Giant. The user's F9 screenshot shows
recognizable world structures and zombies in the native depth buffer. Normal
color view is mostly black with some content, and gameplay is too slow to
inspect. Three nearby 30-swap reports show 0.67–0.88 swaps/s, with 13.4–20.0 s
in submission/fence scopes, 4.8–7.1 s in texture scopes and 5.2–11.2 s in pipeline
creation. Translation itself takes only 14–48 ms in those intervals. The run
ends with user window closure. This establishes depth geometry progress, not
complete native gameplay or correct color/depth parity.

Builds 49–51 reduce unnecessary batch flushes, preflight resolve coverage,
protect current-batch images during texture eviction and canonicalize static
pipeline state. They add flush/hash/cache counters. Run 28 stalled after one
swap and was closed; build 51 restores startup target-clear submissions and
adds command-ring/wait retirement. The exact stall cause is unconfirmed. Run 29
is advancing past startup, with early frontend reports around 30–34 swaps/s;
in-game improvement remains unverified. All 88 CPU checks and five structural
shader validations pass. Driver behavior and scene regression remain test gates.
See [native performance evidence](../analysis/NATIVE_PERFORMANCE.md).

The user confirmed faster menus in run 29 but still unplayable gameplay. Its
normal view contains faint scene detail and apparent red trails when moving;
a missing clear/overwrite/compositing operation or target ownership is plausible
but unconfirmed. F9 was not tested. In-game counters remain 1.03–1.56 swaps/s,
with roughly 15–20 GiB hashed and 1,500–1,800 submissions per 30 swaps. The run
closed normally.

Build 54 allows 128 draws per batch and reuses watched CPU texture contents
within a command-ring chunk. Every lookup still checks memory permissions;
base/mip write faults, allocation changes, native exports and coherency actions
invalidate reuse. Ring ends expire clean reuse for direct PM4 writers. Ten
actual SDK write-watch fixtures bring the total to 98 CPU checks; shader
validation still passes all five modules. Run 30 reduced measured hash traffic
to around 415–418 MiB and submissions to 452–480 per 30 in-game swaps, but
presentation still falls around 1.4–1.6 swaps/s. The user confirms F9 geometry
still looks correct, normal view remains faulty and gameplay unplayable. They
closed the test. These samples are not a matched-scene performance comparison.

Build 55 replaces whole-allocation permission-region scans with locked checks
of only the pages needed by each input span. The staged SDK's bounded
`QueryRangeAccess` now rejects uncommitted pages as well as inaccessible ones;
this adaptation is reproduced by `prepare-runtime.py`. Fresh checks remain
on every lookup, with no permission cache. Ten guard fixtures bring the total
to 108 CPU checks, and new logs measure guest-access guard time. Run 31's guard
cost is tens of milliseconds per roughly 150,000–225,000 checks, but gameplay
remains unplayable (about 1.9–4.8 swaps/s across sampled gameplay intervals).
The user sees a barrier and its window object in normal view, with persistent
redraw/trails as the camera moves. Most scene color remains missing. The run
closed normally; incomplete texture and resolve paths still reject many draws.

Build 56 corrects a missing presentation path: normal view prefers the covered
RGBA8 resolve image at the actual guest frontbuffer address, cropping its
padded storage to logical dimensions. Previously it always sampled the latest
native target, potentially an intermediate or a post-resolve-cleared source.
Unsupported/missing coverage retains a reported diagnostic fallback; F8/F9
still select diagnostic targets. The new presentation push range is eight
bytes and structurally reflected. Six new coverage/address/format fixtures
bring CPU checks to 114. Run 32 closed normally, but the user reported red/blue
exchange across menus/cutscenes and increasingly unplayable gameplay.

Build 57 follows the actual frontbuffer fetch that VdSwap places in slot zero.
It checks the fetch address, dimensions, format, endian, type and valid channel
selectors against the selected resolve; the presentation shader applies its
swizzle through a sixteen-byte push block. Run 33 logs `A0A` (B,G,R,one) and a
resolve with red/blue swap enabled, establishing the missing display mapping.
This correction is independent of the still-incomplete scene color passes.

The old 256-texture limit churned the map working set even while aggregate image
memory was well below its 512 MiB limit. Build 57 evicts retired LRU textures
when Vulkan allocation bytes require space; recorded resources remain protected.
An independent 4,096-entry metadata ceiling prevents unlimited key growth.
Failed allocations remove their empty map entries. Texture and total-image bytes
are reported with timing counters. Raw fetch keys still duplicate backing for
sampler/view variants, so shared backing remains needed. The aggregate budget
is a PC prototype setting, not a measured Switch budget.

134 CPU fixtures and all five structural shader modules pass for build 57.
Run 33 closed normally. The user confirms display colors are normal again;
FPS remains bad, and a zombie is visible only when in front of a visible object.
This does not identify the missing color/depth/compositing behavior.

Build 59 adds an F10 unlit geometry diagnostic when launched with
`-Native -NativeSceneView`. Hold F10 for roughly a second and release after
entering gameplay. It replaces eligible MSAA, depth-tested, target-zero scene
pixel shading with bright solid colors, omitting PS textures/lighting/alpha kill
and blending. The real translated VS, VS textures, depth/stencil and culling
remain. Menu/video, depth-only and rectangle clear/postprocess draws retain
their original path. No reliable `r_fullbright` setting was identified in the
patched image; this is not BO3's textured fullbright mode.

Diagnostic color targets have a separate identity, clear once per swap and
cannot become guest resolve sources. Direct scene presentation avoids bloom
and the normal final compositing chain. F10 again restores normal selection;
allow fresh frames to redraw it. F9 can still inspect shared real depth. The
unlit view omits the HUD and loses alpha cutouts/material appearance, so it
cannot establish full normal-renderer correctness or production FPS.
All 142 CPU cases and six structurally checked shaders pass for build 59,
including eight draw-selection checks and the 48-byte unlit push ABI.
Run 34 closed normally. The user and attached screenshot confirm extensive
world/viewmodel geometry in F10, while FPS remains poor. This establishes a
working diagnostic vertex/raster/depth route for the visible surfaces. It does
not validate texture sampling, ordinary pixel shaders, alpha kill, blend,
lighting or final composition. Several systems are bypassed together, so the
image cannot identify lighting alone as the cause of normal black output.
The normal-path counters still include 30,668 texture numeric-mode rejects,
26,424 texture-extent rejects, 6,120 resolve-number-format rejects, 3,117
incomplete resolve-reader rejects and 1,093 missing depth resolves. These are
cumulative attempts, not unique missing objects. Next work should localize
the failures by VS/PS and fetch state, implement supported numeric/resource
semantics, and compare normal pre-composition scene color against the baseline.
Full log: `analysis/pc_native_backend_boot34_unlit_geometry.log`.

### Direction toward Switch

Keep the PC-first command-ring-to-native-Vulkan approach. NFSMW-NX and Mass
Effect retain the game's Direct3D layer and consume its PM4 stream with a native
backend. Their optimized renderers also use resource caching and overlapping
frame slots; NFSMW avoids repeated vertex uploads and reuses changed constants.
See the reference [native renderer](https://github.com/StevensND/nfsmw-nx/blob/main/docs/native-renderer.md).
These are architectural references, not evidence of BO3 performance on Switch.

Next gates are: verify display colors and residency in the current test; finish
missing color/resolve/alias behavior against captured reference frames; share
texture backing and reuse uploads/constants/descriptors; use offline shaders
and persistent pipeline caches; replace per-batch synchronous waits with frame
slots while preserving guest completion/readback lifetimes; then measure a
small correct scene on original Switch hardware. The current incomplete shader
and resource support cannot be benchmarked as an optimized Switch renderer.

Remaining gates include stencil/clear behavior, remaining resolves and CPU
writeback, general native-produced texture ownership, target aliasing, MRT/MSAA parity,
remaining primitive modes, unsupported texture conversions/filter modes, queries
and complete GPU semantics.
Presentation now follows supported, covered guest RGBA8 frontbuffer resolves;
missing/unsupported output retains a diagnostic fallback. Unsupported paths are counted
and skipped, so a visible menu does not establish a complete renderer. Batches
still wait synchronously at their boundaries; overlapping frame slots and Switch
performance are unproven.

Sources and test evidence: `analysis/bo3_memexport_parity01.json`,
`analysis/bo3_native_memexport_tests02.json`, `analysis/native_bc_tests01.log`,
`analysis/pc_native_backend_boot06.log` through `boot09.log`, and
`analysis/pc_native_provenance09.json`. Profiling evidence:
`analysis/pc_native_backend_boot10.log`, `analysis/native_physical_access_tests01.log`,
`analysis/pc_native_backend_boot11.log`, `analysis/pc_native_provenance11.json`,
`analysis/pc_native_provenance12.json`,
`analysis/pc_native_backend_boot12_snapshot.log` (partial live-run snapshot),
`analysis/pc_native_backend_boot12.log` (complete run).
Build 18 inputs: `analysis/pc_native_provenance13.json`.
Further inputs: `analysis/pc_native_provenance14.json` and
`analysis/pc_native_provenance15.json`. Complete diagnostic logs:
`analysis/pc_native_backend_boot13.log`, `analysis/pc_native_backend_boot14_scene.log`.
Further logs and inputs: `analysis/pc_native_backend_boot15.log`,
`analysis/pc_native_backend_boot16_depth.log`, `analysis/pc_native_provenance16.json`,
`analysis/pc_native_provenance17.json`, `analysis/pc_native_backend_boot17_depth.log`,
`analysis/pc_native_backend_build23.log`, `analysis/pc_native_provenance18.json`,
`analysis/pc_native_backend_boot18_depth_prepass.log`,
`analysis/pc_native_backend_build26.log`, `analysis/pc_native_provenance19.json`,
`analysis/native_position_validation02.json` and its validation log,
`analysis/pc_native_backend_boot19_position_format.log`,
`analysis/pc_native_backend_build28.log`, `analysis/pc_native_provenance20.json`,
`analysis/pc_native_backend_boot20_depth_clears.log`.

Run `scripts/nx/prepare-runtime.py` if the ignored SDK/generated staging is
missing, then `scripts/nx/build-pc-vulkan.cmd`. The executable is
`build/pc-vulkan/bo3_zm_vulkan.exe`; SDK DLLs and the Xenos plugin are staged
beside it. The original desktop build uses its separate vendored SDK/build.
The prepared guest header supplies explicit weak aliases required by SDK 0.10.
The translated guest instructions and function coverage are retained.

The host retains BO3's existing local-profile/content installation hooks and
Windows crash reporter. Use separate Vulkan caches and logs, explicit game,
update and user roots, and the `fbo` render-target path to match the initial
Switch configuration. Initial acceptance is title/menu and one map, followed
by DLC coverage. Neither a successful compile nor Vulkan setup alone proves
renderer correctness or gameplay.

Launch with `scripts/nx/launch-pc-vulkan.ps1`. It selects explicit content roots,
a local signed-in profile, muted audio and separate caches per render-target
path. `-RenderTargetPath fsi` selects the interlock path; `fbo` is the default
for comparison with Switch. Optional `-CaptureFrames` and `-DumpShaders` are
diagnostics, not required for ordinary play. Close the test before rebuilding.

### Initial PC validation

Build01 linked the executable, runtime and Xenos plugin successfully. On Intel
Arc B580, FBO boot01 and the minimal boot02 control both reported a graphics
device loss after loading the updated XEX and the four content packages.
Boot01 used frame capture and shader dumping; boot02 used neither, so those
diagnostics are not required for the failure. No cause has been established.
The Vulkan loader also reported RTSS and OBS implicit layers; their involvement
has not been tested. This is not evidence of a Switch NVK driver fault.

Boot03 remained running, and the user loaded The Giant and supplied a game-over
screenshot after round 1. Large pale squares with circular shading obscure the
scene; the user reports the scene behind them otherwise looks correct. Particle
alpha/texture sampling, blending or another translucent/light pass are candidates,
not confirmed causes. The run ended with orderly window close/guest exit after
about 3 minutes. FSI requires fragment shader interlock, which this PC advertises.
A later build02
attempt was unable to relink the executable while boot03 held it open; no source
compile error was reported. The build script now checks for a running test.
Build03 subsequently linked successfully with the test closed.

**Comparison correction:** boot03 was launched with an intended FSI setting,
but its pipeline cache was named `.fbo.vk.xpso`. Inspection of SDK `cvar::Init`
shows late plugin settings only accept `--name=value`; a separated
`--name value` is replayed as `name=true` with the value left positional.
Thus boot01/02/03 used FBO; their results cannot establish an FSI benefit.
Shader dumping in boot01 similarly wrote into the build folder `true/`.
The launcher now uses equals syntax for late GPU settings, a standalone mute
flag and a diagnostic that logs the requested and actual render-target path.
Build04 includes that diagnostic and is the corrected FSI comparison.
Boot04 logs `requested='fsi', selected=fsi, FSI supported=true`, confirming the
actual backend. Shader dumping now writes microcode and disassembly into the
intended ignored runtime directory, and scheduled guest-frame captures work.
The automatic 120-second capture shows The Giant gameplay with the same pale
square effects under confirmed FSI. Terrain, weapon and HUD remain recognizable.
Thus the defect is present in both FBO and FSI; changing render-target paths did
not remove it. The user also observes squares at windows, consistent with glow
or other light sprites as well as particles. A shared effect shader, texture
sampling/alpha or draw-state issue is plausible; the responsible draw is not
identified. Do not infer a general lighting failure or hide the effects as a fix.
Boot04 ended with orderly window close and guest exit after about 3 minutes.
Binary/source hashes
are in `analysis/pc_vulkan_provenance04.json`; the log snapshot is
`analysis/pc_vulkan_boot04_fsi_snapshot.log`.
The complete run is `analysis/pc_vulkan_boot04_fsi.log`; the inspected capture
is kept locally at `runtime/logs/pc-vulkan-boot04-fsi-frame120.png`. The second
user screenshot is `runtime/logs/pc-vulkan-white-squares-window.png`.

Evidence: `analysis/pc_vulkan_build01.log`, `analysis/pc_vulkan_boot01.log`,
`analysis/pc_vulkan_boot02.log`, their stderr reports and
`analysis/pc_vulkan_provenance01.json`. The complete boot03 log is archived as
`analysis/pc_vulkan_boot03_requested_fsi_actual_fbo.log`. Its screenshot is kept
locally at `runtime/logs/pc-vulkan-boot03-fsi-screenshot.png`. Corrected FSI test
log: `runtime/logs/pc-vulkan-boot04-fsi.log`.

## Native renderer work order

### Draw capture

Portable RenderDoc 1.46 is installed locally under
`tools/renderdoc/RenderDoc_1.46_64`. Its official archive SHA256 is
`9ca4d09ecaba2cc791168660d6fc2a7e3d70fe67146e87ec05c5f8dea70772f7`;
the DLL has a valid Baldur Scott Karlsson signature. The Android helper has been
disabled locally because its automatic detection generated recurring adb errors.

Launch a diagnostic test:

```powershell
.\scripts\nx\launch-pc-vulkan.ps1 -RenderTargetPath fsi -GpuCapture -Background
```

When positioned at the defect, use the capture hotkey or create
`build/draw-capture/request.txt`. The host writes the saved path into
`build/draw-capture/status.txt`. `scripts/nx/inspect-gpu-capture.py` runs through
RenderDoc's embedded Python (`qrenderdoc --python`) and exports an action/resource
inventory into ignored build output. System Python cannot load this portable
package's embedded RenderDoc module. Raw captures, shader exports and textures
are excluded from Git.

The first replay localizes the squares to the glow/bloom chain, including a
160x90 effect texture carrying the same block pattern. The precise corruption
mechanism is still under investigation. See
[capture findings](../analysis/VULKAN_WHITE_SQUARES.md).

### Implementation sequence

The first BO3-specific foundation is recorded in
[native foundation and reference review](../analysis/BO3_NATIVE_FOUNDATION.md).
Run `launch-pc-vulkan.ps1 -RenderTargetPath fsi -D3DTrace -Background` to collect
the forwarding XDK census and full shader containers. This build still draws
through Xenos emulation; the census is preparation for the native backend.
The initialization and bound-object capture passed user-confirmed The Giant
gameplay. The offline translator/validator now builds on Windows; see
[shader tools and unresolved variants](NATIVE_SHADER_TOOLS.md).

1. Establish the emulated Vulkan baseline on PC and retain frame captures.
2. Capture BO3 shaders, patched variants, command packets and resource usage.
3. Identify and verify BO3's XDK shader/draw/flip functions and device offsets.
4. Adapt a native Vulkan command-ring renderer with reported unsupported
   shaders/packets, required readbacks and correct guest GPU synchronization.
5. Compare boot, menus, models, effects and maps against the baseline.
6. Integrate into Switch and measure CPU, GPU and memory costs on hardware.

The [SA2 porting guide](https://github.com/ChanseyIsTheBest/sa2-nx/blob/main/docs/porting-a-360-game-prompt.md)
recommends validating a native PM4-ring Vulkan renderer on PC before Switch.
Its game-specific hooks, shader library and XDK offsets cannot be assumed to
describe BO3. Third-party renderer licensing and provenance must be recorded
when importing code. No such native app renderer has been imported here yet.

The Giant's existing color flashing is a known baseline limitation and remains
paused; the new path does not imply a fix. PC timing cannot establish Switch
performance. BO3's guest CPU work, GPU emulation overhead, shader/pipeline
compilation, resource uploads and synchronization need separate measurements.
