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

## Native renderer work order

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
