# BO3 RexGlue progress and limitations

Updated October 13, 2026 (America/New_York).

## Current state

The `NX` branch contains Nintendo Switch groundwork. The installed devkitA64
and Mesa/libnx environment builds a diagnostic ARM64 NRO with startup at zero.
Its first console test passed application mode, three available cores, worker/
NEON execution and Vulkan instance/device creation. The user confirmed that +
exited normally. It does not load BO3. The BO3 compile
probe confirms missing Horizon platform/fiber support in the current desktop
SDK; a complete runtime port and playable performance are still unproven.
The user confirmed original Switch/OLED hardware and title takeover.
See [NX port plan](docs/NX_PORT.md) and `analysis/nx_environment.json`.
Hardware evidence: `analysis/nx_preflight_hardware01.log` and
`analysis/nx_preflight_result.json`. The first complete BO3 Zombies Horizon NRO
has linked and packaged, using the pinned Switch SDK and Vulkan Xenos backend.
Startup at zero, linked aliases and the custom exception handler were verified.
The user identified an incorrect NRO forwarder as the reason the initial launch
produced no log. After correcting it, the trace01 hardware run produced both
startup and application logs. Services, static constructors, Vulkan device and
1280x720 swapchain initialization, and guest-memory mapping are recorded. The
logs do not yet establish XEX loading or a rendered BO3 frame. Two earlier
Atmosphere reports show only `hbl` and cannot be attributed conclusively; the
console clock is March 7, so their filename dates do not exclude this test. See
[NX runtime build](docs/NX_RUNTIME.md).
The agreed first-log gate has passed. The next work is an isolated Windows
Vulkan baseline using the same SDK, then native renderer development on PC
before pursuing complete Switch gameplay/performance. Evidence is archived as
`analysis/nx_runtime_hardware01_startup.log` and `analysis/nx_runtime_hardware01.log`.

PC renderer development now has its own branch, `native-rendering-pc`. The
isolated Vulkan build using the Switch SDK sources linked successfully.
FBO failed with device loss on Intel Arc B580 in two controls. A third run loaded
The Giant and ended normally, with large white effect squares reported and
shown in the user's screenshot. The intended FSI setting was passed incorrectly
to late plugin cvars, so that third run also used FBO. The launcher is corrected
and a new build logs actual renderer selection. Confirmed FSI also loaded The
Giant and ended normally after about 3 minutes; an automatic gameplay capture
shows the same square effects. The defect therefore affects both paths.
A RenderDoc gameplay capture replayed successfully and localizes the square
pattern to the glow/bloom processing chain: the earlier scene image is clear,
while a 160x90 effect texture carries the same block pattern as the later output.
The exact shader/state/format defect remains unconfirmed. Menu playback does
not validate 3D rendering. See [Vulkan square investigation](analysis/VULKAN_WHITE_SQUARES.md).
Further capture analysis finds varied, finite FP16 EDRAM values becoming binary
UNORM values through the bloom resolve/upload chain. This investigation is
paused for native development. NFSMW-NX, Mass Effect and SA2 references were
reviewed; BO3-specific forwarding D3D/shader-discovery hooks passed user-confirmed
The Giant gameplay and normal-close tests. A new initialization/GPU capture
collected 2,897 unique shader containers and 455 GPU-loaded programs. The
offline Windows shader tools produce 2,843 validated SPIR-V modules;
vertex variant identity, FETCH declarations and several translator failures
remain unresolved. See [shader tools](docs/NATIVE_SHADER_TOOLS.md).
The BO2 native renderer is confirmed broken
and is not the renderer base. See [native foundation](analysis/BO3_NATIVE_FOUNDATION.md).
A separate BO3 native Vulkan prototype is now implemented in `nx/native`, using
its own draw backend and RexGlue command parsing/presentation. See
[PC native rendering work](docs/NATIVE_RENDERING_PC.md).
MW2's custom PM4-to-Vulkan renderer has also been reviewed. Its raw-buffer vertex
fetching is a useful candidate for the patched-VS problem, but its unsupported
texture-weight operations and renderer limits prevent wholesale adoption. The
offline comparison now covers 149 vertex and 306 pixel programs. The separate
BO3 prototype adds the observed GetWeights operation and typed 2D/cube/3D
texture bindings, and cleanly validates 453/455 loaded programs, including all
306 pixel programs. The 450 previously accepted modules retain the same
instructions apart from required binding decorations. Two vertex programs still
drop memory exports and remain blocked; actual native texture-view ownership
also needs implementation. A bounded GPU-thread probe captured 16 draw-state
samples of each remaining program: both use unsigned 16-bit exports, 8-in-16
endian conversion and no red/blue swap. EDRAM mode 4 is color/depth; vertex
export mode 7 suppresses rasterization. The visible repeat test closed normally,
confirmed by the user and shutdown log, with no new crash report. Its 32 states
fit a guarded offline copy-range model: contiguous for one program and strided
with bounds clipping for the other. Six model tests pass. Actual GPU output
bytes were subsequently compared against synchronous GPU readback: all eight
captures match, seven with nonzero writes. Both exact programs now have bounded,
state-checked CPU replacements in the native prototype; the actual C++ planner
passes all eight captured outputs. See [memory-export gate](analysis/BO3_MEMEXPORT.md).
The native prototype submits draws and the user reports visible frontend output
through the loading screen. Runs 06/07 were user-closed because they were too
slow to test further; an earlier reported loading crash remains unattributed.
No complete native 3D gameplay frame has been validated. Run 08 identified export
processing as the main measured slowdown (10.64 s of a 14.11 s interval), with
shader translation/pipeline creation taking milliseconds. Reduced copy planning
and snapshots plus BC texture decoding made menu options visible in run 09,
confirmed by the user. Performance still progressively worsens. Comparable
intervals fell from 13.33 to 3.04 swaps/s as export elapsed time grew despite
similar copied volume. Build 14 measures individual export phases and actual
command-thread CPU time. Run 10 isolated expensive Windows memory-region
permission queries: 11.75 s in guards versus about 11 ms copying over a 15.35 s
interval. Build 15 uses fresh committed physical-heap permission checks instead;
ten boundary/protection cases pass. Run 11 confirms much faster intro playback
and navigable menus. It reaches gameplay with visible HUD but a black 3D scene;
map previews may be invisible. Later menu slowdown is dominated by thousands
of synchronous submissions/fence waits, not shader compilation. Builds 16/17 add
bounded draw batches with distinct uploads/descriptors and explicit attachment
ordering. Recent run 12 intervals record about 20–24 swaps/s and substantially
fewer submissions; the user confirms improved performance. Gameplay still has
a black scene and visible HUD. The complete run 12 ends normally and reports
millions of omitted MSAA and polygon-mode/depth-bias draws. A covering pass is
unconfirmed; implementing the rejected geometry states and correct native target
resolves/presentation is the next rendering gate. This is not a controlled
gameplay benchmark.
Build 18 adds host-supported 2x/4x color/depth targets with diagnostic presentation
resolve, filled dual-face polygon mode and SDK-aligned dynamic depth bias.
Run 13 creates an 880x720 2x target on Arc, but the user reports a black scene
with HUD and worse in-game performance. The log shows newly reached stencil
and texture rejections, with many submissions. Builds 19/20 add stencil state,
canonical color aliases, prevalidate textures and discard empty GPU batches.
Run 14's optional MSAA scene-target view selected a black menu target and hid
the menu. Build 21 leaves normal presentation active initially; F8 toggles the
current-frame scene view after entering gameplay. In run 15 the user confirms
improved menus and visible map previews, but gameplay remains black with HUD;
F8 removes the HUD and the selected MSAA target stays black. Build 22 adds an
F9 depth view to check whether geometry reaches the depth buffer independently
of game shading. Its SPIR-V validates. Run 16 closed normally without an F9
comparison; in run 17 the user confirms F9 stays black, with the log confirming
an active current-frame depth view. It closed normally. Build 23 adds depth-only
draws without a fragment shader and shares matching depth surfaces across passes
and color targets. New color targets preserve an existing shared depth surface.
It also avoids repeated Vulkan object creation on unsupported texture views.
Build and source audit pass. Run 18 still shows black, including F9; it closes
normally. No `kDepthOnly` draws submitted, so that addition did not address the
scene. Thousands of color/depth draws submit while cube/volume/mip bindings
exclude many others. Build 26 implements full XYZ/W vertex format conversion
with SDK viewport/scissor normalization and a 48-byte push interface. It also
uses SDK analysis to omit pixel shaders only for passes with no PS side effects.
All 455 captured modules pass structural validation; the two dropped-export
modules remain excluded in favor of verified CPU copies. Run 19 is open with
accepted PS bypass draws; the user reports the same black output with no
noticeable regression. Run 19 closes normally. The native test launcher mutes
audio explicitly, so inaudible audio is not evidence of an audio failure.
Build 28 adds requested resolve-packet depth/stencil clears for matching shared
surfaces, preserving partial rectangles and replaying prior clears on new
images. Run 20 still shows black and closes normally. Its last periodic report
records zero resolve-clear requests/applications, so the new path was not
exercised. The main MSAA target received thousands of submitted draws, but many
cube/volume, mip and rectangle draws were rejected; visible sample writes are
unproven. Rejected rectangle-list draws/raster clears and color/depth readback
before final presentation are the next investigation, rather than assuming all
scene passes render successfully. Greater-depth prepasses still face initial
depth 1 without supported guest clears. Logs show no native backend failure.
Runs 21/22 add generated rectangle geometry shaders and bounded GPU coverage
queries. Run 21 lacked the enabled device feature; run 22 executes rectangles.
The main raster clear passes 1,267,200 samples, while world prepasses pass zero.
The user reports F9 shows a white top half and black bottom half in run 22.
Depth base 68 uses a 440x720 4x clear view and an 880x720 2x world view with the
same sample-row layout, but separate native depth images. Build 33 propagates
recognized XDK depth/stencil clears across those alias layouts and corrects
F9's selection to prefer scene depth. Eight mapping cases pass; run 23 is open,
and the user reports F9 fully black. It closed normally with zero recognized
clears, so the alias repair was not exercised. Run 24 identified half-pixel
edges; build 35 handles the observed disabled-MSAA-raster pixel-center coverage.
All 16 mapping/boundary tests pass. Run 25 remained black and identified VF95
address zero. The captured binary and translator metadata prove this exact
clear uses VF0 despite the saved text disassembly naming VF95. Build 36 corrects
the fetch ownership with metadata guards; run 26 is complete. Its log confirms
base 68 clear-to-zero propagation, and one sampled full-screen MSAA depth pass
now passes 1,267,200 samples instead of zero. Later gameplay prepasses also pass
nonzero samples, including 14,130, 23,735 and 63,767. Some sampled draws still pass
zero. The user reports normal gameplay still black with HUD; F9 shows a white
screen and faint gun/viewmodel. The depth display maps the new zero clear to
white, so this is partial visible depth output rather than a color game frame.
All 32 sampled scene draws omit the PS and color writes. The last report counts
112,455 cube/volume-view and 52,778 mip rejections; typed texture/mip backing and
native resolve ownership are the next color-rendering gates. A reported storage
selection overwrite warning is recorded separately with no established GPU
connection. The game was user-closed and the log ends with orderly shutdown.
Runs 24/25 also closed normally. BO2 was reviewed for ideas
without importing its broken D3D9 renderer. See
[BO2 reference notes](analysis/BO2_NATIVE_REFERENCE.md).
Textures/conversion, mips/cube views, raster/color clears,
EDRAM overlap/extent aliasing and native guest resolves remain incomplete.
See [MW2 assessment and work order](analysis/MW2_NATIVE_REFERENCE.md).

The updated Xbox 360 Zombies executable builds as `build/bo3_zm.exe` and reaches
playable Shadows of Evil, confirmed by the user. The user also confirmed that
The Giant and Der Eisendrache both load successfully. Earlier builds lost the
D3D12 device. A confirmed readback-buffer lifetime defect is now repaired. The
patched build passed an 8 min 41 s run with DRED and a shorter 86 s control
without DRED, both loading The Giant and ending normally. Long-session and
all-map stability remain unverified. FPS testing remains paused.
Investigation of model color flashing on The Giant is also paused at the user's
request so other project work can take priority.
A separate native multiplayer target for the updated `default.xex` has compiled.
Its first boot loaded the update and core assets, then reached a missing indirect
callback. Reviewed multiplayer dispatch entries have been added; the corrected
build rendered the title screen. Pressing Start/A reached profile loading and
another missing callback; verified entries from initialized `.data` allowed
further menu progress in boot03. That run reached another missing callback at
`0x8257E598`; its verified wrapper and one equivalent Zombies callback are now
included in the successful 34,996-function build. In boot04 the user reported
playable multiplayer. Its log references `mp_skyjacked`, runs for about 3 min
28 s total and ends normally, without a fatal abort or GPU-loss report.
Extended multiplayer stability, all maps and online play remain unverified.
See [Multiplayer build](analysis/MULTIPLAYER_BUILD.md).

## Completed

- Audited the extracted base game, update, compatibility packs and two DLC
  packages. All 1,409 extracted package files matched original STFS payload hashes.
- Verified title-update source matching and translated both updated executable
  probes. Created a native Zombies host.
- Copied the working BO2 RexGlue SDK and dependencies into this project,
  preserving original modifications. Built with isolated LLVM 20.1.8.
- Staged separate base/update roots, applied sibling XEXP patches, installed
  four packages with original identities and license masks, and observed mounts.
- Fixed missing function-discovery entries and configured verified native
  `setjmp`/`longjmp` handling. Registered 34,840 recompiled functions.
- Reached menu, map loading and playable Shadows of Evil with a local profile.
- Added crash reporting, asset-open logging and optional Release frame timing.
  Async compilation and persistent shader/pipeline caching are enabled.

## Limitations

**Validated lifetime repair:** readback cache eviction could release buffers
whose GPU writes were pending. The rebuilt runtime now protects those entries
with submission fences. Actual unsafe evictions were prevented with cache sizes
of 300–432 entries. Both gameplay checks passed; the normal-settings control was
shorter than five minutes, so extended stability is still open.
See [GPU crash repair](analysis/GPU_CRASH_FIX.md).

- **GPU stability before the repair:** `DEVICE_HUNG` occurred with automatic ROV, explicit RTV,
  and RTV with alternate resource binding. Short successful runs do not prove
  stability. The earlier two-worker experiment crashed about 11 seconds after
  launch. DRED showed an unfinished `CopyBufferRegion` operation and page-fault
  VA `0x0000B80227368000`. That log did not identify the exact allocation;
  the subsequent confirmed lifetime defect and repair are described above.
- **Performance:** a compilation-heavy interval averaged 17.7 guest swaps/s,
  versus 30–33/s in later intervals with less compilation. Pipeline and driver
  workers consumed significant CPU time. The user reports improvement standing
  still and immediate drops moving. Compilation contributes substantially;
  the remaining performance limit is not fully identified. The two-worker
  comparison crashed before establishing a benefit; the normal default remains.
- **DLC maps:** The user confirmed that The Giant and Der Eisendrache both load
  successfully. The log shows The Giant assets from `dlc1:` and Der Eisendrache
  assets from `dlc4:` with its title-update patches. This run ended normally;
  extended gameplay stability on these maps has not been established. Evidence:
  `runtime/logs/dlc-map-test.log`.
- BO3 online services and seamless mode switching are not implemented. The native
  multiplayer host passed a short user-confirmed gameplay test; extended sessions
  and all maps remain unverified. Audio and controller behavior were not
  separately validated.
- Generator float16 packing warnings, further indirect-function targets and
  rendering artifacts require investigation when encountered.
- **The Giant rendering:** the user reports models flashing different colors,
  including zombies, barriers, doors and teleporter pads, with terrain unaffected.
  The cause is unconfirmed; model lighting/vertex data, stale GPU readback data
  and shader/material/texture state are candidates. Disabling fast GPU readback
  left the flashing unchanged. RTV improved it but did not eliminate it;
  disabling async compilation then made no further difference. Source inspection
  found a vertex-buffer cache that skipped memory-validity checks when addresses
  and sizes matched. A coherency repair built successfully, but the user reported
  no visual improvement. Captures show model tint/brightness changes while
  geometry and texture details remain recognizable. Alternate resource binding
  improved the flashing but did not eliminate it. A second page-validity coherency
  repair also built successfully and left the flashing unchanged. The user
  observes it gets worse when zombies spawn and suspects more models worsen it;
  that connection is unmeasured. The cause remains unconfirmed, and investigation
  is paused at the user's request. Both coherency repairs remain in the local
  build; their performance and extended stability impact are not separately
  established.
  See [Rendering issues](analysis/RENDERING_ISSUES.md).

No GPU driver or TDR registry changes were made. The original BO2 project is
unchanged. The normal launcher retains automatic render-target selection;
RTV, alternate binding and GPU diagnostics are explicit test settings.

## Detailed records

- [Launch and rebuild](README.md)
- [Boot fixes and runtime evidence](analysis/BOOT_RESULTS.md)
- [Measured performance](analysis/PERFORMANCE.md)
- [Rendering issues](analysis/RENDERING_ISSUES.md)
- [Multiplayer build](analysis/MULTIPLAYER_BUILD.md)
- [Initial feasibility](FEASIBILITY.md)
- [Input verification](analysis/RESULTS.md)
- [SDK changes](sdk_snapshot/BO3_LOCAL_CHANGES.md)
- [Binary and toolchain hashes](analysis/build_provenance.json)

## Next work

The project is being versioned in the private repository
`Wiiucoop/lastgenbo3recomp`. Game content, credentials, caches and compiled tools
are excluded; source, scripts and progress records are retained.

1. Verify extended normal-settings gameplay stability on the supplied maps.
2. Investigate any remaining GPU failure using named DRED resource evidence.
3. Resume FPS comparisons when requested, separating new content compilation
   from repeated views with a populated cache.
4. Resume The Giant model-color investigation when requested, retaining the
   observed connection with zombie spawning/model population and the recorded
   comparison results.
