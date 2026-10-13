# BO3 native rendering development on PC

October 13, 2026. Branch `native-rendering-pc`, based on `NX` after the first
Switch startup/application logs were received.

## First baseline

`nx/pc-vulkan` builds BO3 Zombies on Windows with the same pinned SDK source
used by the Switch build (`tools/nx-sdk`, NFSMW-NX revision
`be2d4c67ab42d6d629746a01dbf03cef824c8d2d`). Vulkan is enabled and D3D12 is
disabled. This first target still uses the Xenos emulation renderer; a BO3
native renderer is not implemented yet.

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
