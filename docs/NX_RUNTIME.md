# BO3 NX: first Horizon runtime build

October 13, 2026. Branch `NX`.

## Source and build layout

The first game build uses the Horizon runtime from NFSMW-NX revision
`be2d4c67ab42d6d629746a01dbf03cef824c8d2d`, staged at `tools/nx-sdk`.
`scripts/nx/prepare-runtime.py` reconstructs it from the pinned reference and
the repository's vendored dependency sources. Reference dependency overrides
are applied last. This is not a claim that every dependency matches the
upstream v0.10 release pins: the source provenance records the actual inputs.

The reference SDK's BSD-3-Clause license and per-file notices are preserved in
the staged tree. Its app sources, game hook addresses, generated translations,
PGO profiles and shader libraries are not inputs to the BO3 build.

The existing BO3 Zombies translation is copied into `build/nx-generated/zombies`.
Its instructions, discovery coverage, image descriptor and native XDK
`setjmp`/`longjmp` implementation are retained. The copied header gets explicit
C linkage for GCC's weak aliases. A sampled compiled object contains strong
`__imp__sub_82140000` and weak `sub_82140000` symbols, as required for hooks.
Register localization, direct-call rewriting, LTO and PGO are not enabled in
this initial correctness build.
The target uses the code generator's `sources.cmake`, which includes a large
registration wrapper. GCC's O3 optimization of its thousands of repeated virtual
calls consumed minutes and about 1.8 GB; only that wrapper is compiled with O0.
The game's code remains at O3. The entrypoint runtime registers the full
`PPCImageConfig.func_mappings` table from `bo3_zm_init.cpp` instead of calling
that optional wrapper. An earlier statement that the original source list
excluded it was incorrect; both original and NX lists retain it.

The Switch host is `nx/runtime/main.cpp`. It selects `game:\default_zm.xex`,
configures a local signed-in profile, and installs the same four content headers
with the original package identities and license masks. The Horizon SDK provides
guest-memory aliases, pooled exception handling, ARM fibers, threads, libnx
audio/input/UI, static plugin linkage and Vulkan presentation.

The generic shared-memory validity refresh in the reference SDK already uses a
full GPU-written-validity snapshot under the critical-region lock. The desktop
D3D12 readback repair is not compiled into this Vulkan build. The Vulkan vertex
fetch cache retains the reference behavior; this build does not claim to fix
the paused Giant model-color issue.

## Build

```powershell
cd C:\bo3recomp
.\scripts\nx\build-runtime.ps1
.\scripts\nx\package-runtime.ps1
```

Native Windows CMake and Ninja are pinned to Visual Studio Build Tools, and
host Python is supplied explicitly. The initial configure failed because
glslang selected MSYS Python, which misinterpreted Windows paths. Configure02
passed after selecting native Python. Third-party directory checks accept
vendored sources without requiring live submodule `.git` files.

Build output:

- `build/nx-runtime/bo3_zm_nx`: unstripped ELF, retained for crash symbolization.
- `build/nx-runtime/bo3_zm_nx.nro`: game test executable.
- `build/nx-runtime/bo3_zm_nx.map`: link map.
- `build/nx-package/bo3recomp`: deployment files and instructions.

`-SkipPrepare` supports incremental builds after the staged sources are prepared.
Re-running preparation reapplies the pinned sources and tracked BO3 patches;
persist future SDK changes under `nx/sdk-patches/` rather than editing only the
ignored staging tree. Host/ARM build logs and source provenance live in
`analysis/nx_runtime_*`.
Preparation merges the dependency/reference/patch inputs before writing, and
writes transformed headers only when their contents change. Repeating it after
the successful build produced `ninja: no work to do` in a dry build check.

## SD layout

```text
sdmc:/switch/bo3recomp/
    bo3_zm_nx.nro
    bo3_zm_nx.toml
    game/       contents of runtime/game, including default_zm.xex + .xexp
    update/     contents of runtime/update
    user/
        0000000000000000/   common packages from runtime/user
    cache/      created/used locally on Switch
```

The NRO and configuration are packaged by default. Add `-IncludeGameFiles` to
the package script to copy the staged game/update/common content into the local
deployment folder before transferring it. This can take substantial disk space.
Alternatively copy those three content directories directly to the SD card.
Preserve package directory names and keep the game/update roots separate.
The matching title update and compatibility packs are required for gameplay.

Launch through **title takeover**. The initial test should establish whether
startup reaches the title screen and whether Start/A reaches the Zombies menu.
Then attempt Shadows of Evil if the menu works. The environment-only preflight
needed no game files; this game build requires them.

The configuration selects Xenos emulation through Vulkan's `fbo` host-framebuffer
path and disables the NFSMW exact-range file cache for an initial BO3 baseline.
Offline BO3 shaders, a native renderer and performance optimizations are future
stages. Any low FPS from this first build must be measured before its cause is
assigned.

## Evidence to return

- `sdmc:/switch/bo3recomp/bo3_zm_nx.log`.
- `sdmc:/switch/bo3recomp/logs/rex/`, including crash/stderr files if produced.
- The Atmosphere report if a system error occurs.
- Visible screen, last successful action and whether the app stayed responsive.

Keep the matching ELF and build provenance with each tested NRO. SDK crash
addresses can then be resolved with devkitA64 `addr2line`/`nm`. This stage does
not validate all maps, audio, multiplayer or playable performance.

## Validation status

The first full BO3 ARM64 build **linked and packaged successfully**. The final
log is `analysis/nx_runtime_build03.log`; builds 01/02 were manually interrupted
during GCC's O3 registration-wrapper pass, before switching that file to O0.

The NRO is 67,815,621 bytes (about 64.7 MiB). Its ELF is AArch64, ELF64, a static
PIE with entrypoint `_start` at zero. Linked sample weak aliases have the same
addresses as their strong implementations. The Horizon exception handler is
present as a strong symbol. Text is 66,513,900 bytes, data 1,268,840 bytes and BSS
6,063,712 bytes. These section sizes are not a total runtime-memory estimate.
Binary/input hashes are saved in `analysis/nx_runtime_provenance.json`.
All 34,840 image function mappings are retained. The prepared package was checked
against all 1,796 source content paths and sizes, and its NRO hash matches the
built binary. Evidence: `analysis/nx_runtime_package.json`.

The package's game, update and common content total about 7.52 GiB, plus the NRO.
The environment preflight passed on hardware. The first BO3 game NRO hardware
launch initially failed with no application log reported; the user later
identified an incorrect NRO forwarder. With the forwarder corrected, the trace
build produced startup and application logs. First boot into BO3,
menu interaction, image correctness,
audio, map loading, memory behavior and performance remain to be established.

### Supplied Atmosphere reports

On October 13 the user supplied these reports in `build/nx-package/bo3recomp`:

- `01772862935_05bf5a68d54b9000.log`
- `01772866382_056622cebac95000.log`

Both report `0x202 (2002-0001)`, a User Break at `hbl + 0x4040`, one thread,
and only the `hbl` module with ID
`20C7654671295934AB14F14187501B8BFE957327000000000000000000000000`.
Neither contains the BO3 module/build ID
`c00c58c3d7e9e8ed1cad0874c32ba31d2e2bcf14` or BO3 stack addresses.
They establish a loader failure, but do not identify the requested NRO or prove
that either report came from this BO3 launch. A loader failure during a BO3
launch is still possible; absence of a BO3 module alone cannot rule that out.
The filename timestamps decode to March 7, 2026, 05:55:35 and 06:53:02 UTC,
assuming the console clock was correct. The new application log also uses March
7, confirming the clock differs from the October 13 client date; those filename
dates cannot establish that the earlier reports are unrelated. The user initially
suspected unrelated reports and later reported correcting the NRO forwarder.
These files are not being used as evidence for a BO3 runtime repair.

### Startup trace01 test

Build with `scripts/nx/build-runtime.ps1 -StartupTrace`, then package normally.
This diagnostic overrides libnx's weak `__appInit` to initialize SM, FS and mount
SD before applet/HID/time/window initialization. Those services and libnx's
optional hooks are retained. The changed service order is diagnostic and is not
a confirmed crash fix. It can be disabled by rebuilding without `-StartupTrace`.

The first file is `sdmc:/switch/bo3recomp/bo3_nx_startup.log`, rewritten per
launch. Each stage closes its file before continuing. Markers bracket the
window initialization, all static constructors and entry into the existing
SDK `main`. This avoids depending on the C++ logger or game configuration to
record progress. Failure before SM/FS/SD initialization can still prevent a
file; those service failures retain their raw error code on screen.

The original failed NRO/ELF/map/provenance are preserved locally under
`build/nx-runtime-baseline01`. The trace build linked, retained `_start=0`, and
disassembly confirmed `__libnx_init` calls the diagnostic `__appInit` and wraps
the constructor call; crt0 branches to the main wrapper. Evidence:
`analysis/nx_runtime_startup_trace01_build.log` and
`analysis/nx_runtime_startup_trace01_provenance.json`.

The startup-log gate has passed on hardware. The agreed subsequent work
order is an isolated PC emulated-Vulkan baseline using the Horizon SDK source,
followed by BO3 shader/command capture and a native Vulkan renderer on PC, then
Switch integration and measured optimization. Full Switch gameplay is not a
prerequisite for beginning that PC renderer work.

### Runtime hardware01: first log gate passed

The user corrected the NRO forwarder and returned `bo3_nx_startup.log` and
`bo3_zm_nx.log`. The startup trace reaches `main` after successful applet, HID,
time, window and static-constructor initialization. The application log records
Vulkan initialization on Tegra X1, driverInfo **Mesa 26.2.1
(git-fe913078f1)**, a 1280x720 handheld swapchain with immediate presentation,
and mapped guest virtual/physical arenas. This identifies the driver version
more precisely than the earlier raw version integer; it does not identify all
local driver patches.

The final line warns about fewer than six processor cores. Source inspection
shows this is a nonfatal once-only scheduling warning in `XThread::SetAffinity`;
it is not proof of the reason execution stopped. The supplied log does not
record XEX loading, translated guest execution, gameplay, or a shutdown/crash
reason. No SDK crash report was supplied with these two logs.

Archived evidence: `analysis/nx_runtime_hardware01_startup.log` and
`analysis/nx_runtime_hardware01.log`. The tested package currently matches
trace01's SHA256 `ddbd0396617dbe76ea143d0091fc93def481b9cfc15d9151e74dff62ca750c1d`.
The user reported the forwarder correction, so successful logging cannot be
attributed specifically to the diagnostic service-order change.
