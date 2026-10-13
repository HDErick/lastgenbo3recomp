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
The environment preflight passed on hardware; **this BO3 game NRO has not yet
been tested on hardware**. First boot, menu interaction, image correctness,
audio, map loading, memory behavior and performance remain to be established.
