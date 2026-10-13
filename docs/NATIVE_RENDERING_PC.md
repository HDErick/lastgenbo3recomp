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
to toggle this diagnostic while the game window is foreground. Run 15 tests
normal menu navigation followed by the in-map F8 comparison; feedback is pending. Clearing
and shared depth/stencil ownership are still incomplete, so accepting stencil
state alone does not establish correct stencil contents.

Remaining gates include stencil/clear behavior, resolves and their exponent
conversion, native-produced texture ownership, target aliasing, MRT/MSAA parity,
rectangle primitives, mips, cube/volume views, queries and complete GPU semantics.
Presentation currently samples the latest drawn native target as a diagnostic;
it does not yet follow guest frontbuffer resolves. Unsupported paths are counted
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
