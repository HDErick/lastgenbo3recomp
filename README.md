# BO3 Xbox 360 / RexGlue prototype

See [PROGRESS.md](PROGRESS.md) for current progress, limitations and next work.

This project builds the updated Xbox 360 Zombies executable (`default_zm.xex` +
`default_zm.xexp`) as `build/bo3_zm.exe`, using an isolated copy of the working
BO2 project's RexGlue SDK and its Xenos GPU plugin. It is a development prototype.
Shadows of Evil reached playable gameplay, confirmed by the user during testing.
Earlier builds crashed with D3D12 `DEVICE_HUNG`. The current build repairs a
readback-buffer lifetime defect and passed two The Giant tests, including an
8 min 41 s run with DRED and a short control without it. Long-session stability
remains unverified. See [GPU repair](analysis/GPU_CRASH_FIX.md) for details.
See [boot results](analysis/BOOT_RESULTS.md) for the verified runtime state.

## Launch

In PowerShell:

```powershell
cd C:\bo3recomp
.\scripts\launch-zombies.ps1
```

The launcher configures the game, update, user content and cache roots explicitly.
It selects the Zombies executable directly. BO2's `+zm` mode switch is not required
to select this executable.

The separate multiplayer target selects `default.xex` directly:

```powershell
.\scripts\launch-multiplayer.ps1
```

Multiplayer reached playable gameplay in a short user-confirmed test and closed
normally. Extended stability and all maps remain unverified.
Build it with `scripts/build.cmd bo3_mp`; see
[multiplayer build status](analysis/MULTIPLAYER_BUILD.md) for validation results.

The launcher defaults to a local signed-in profile (`--xam_signin_state 1`). The
copied BO2 SDK defaults to a Live signed-in profile, which caused this prototype
to display "Connecting to Online Service" while polling unimplemented APIs.
Use `-SigninState 2` only when investigating that path. Online service support
has not been implemented for BO3.

For a diagnostic launch:

```powershell
.\scripts\launch-zombies.ps1 -LogName diagnostic.log -LogLevel debug -SampleSeconds '15,40' -CaptureFrames -Background
```

Logs are in `runtime/logs`. Optional thread samples and crash reports are beside
the executable in `build`. Optional guest frame captures are PPM files in
`runtime/cache`. A patched memory image can be captured with `-DumpImagePath`.
Frame capture includes short gameplay bursts around 90 seconds and two minutes
after module launch, allowing comparisons of changing object colors.
Before the lifetime repair, both instrumented runs and a run without capture/sampling lost the graphics
device. Normal launches leave these optional diagnostics disabled. Their absence
did not resolve the earlier GPU hang.

Async shader compilation and the persistent shader/pipeline cache are enabled
by default. Pending pipelines can cause brief missing effects; some shader work
still runs synchronously. Both automatic ROV and explicit RTV have produced
`DEVICE_HUNG`. Use `-RenderTargetPath rtv` or `rov` for controlled comparisons;
the default remains `auto`. See the boot results for individual test outcomes.
Frame timing and compiler CPU activity are recorded in
[performance results](analysis/PERFORMANCE.md).

## Rebuild

```powershell
& C:\Windows\System32\cmd.exe /c C:\bo3recomp\scripts\build.cmd
```

`scripts/codegen_incremental.py` is an alternative generator wrapper that keeps
timestamps of byte-identical outputs. Adding function declarations still causes
the shared generated header's dependents to rebuild.

Uses portable Clang 20.1.8 from `tools/llvm-20.1.8`, Visual Studio 2022 Build Tools,
Windows SDK 10.0.26100.0, native CMake and Ninja. No BO2 build cache is reused.
The build targets x86-64-v3 (including AVX2).
Generated C++ is already present. After changing the manifest:

```powershell
& C:\Windows\System32\cmd.exe /c C:\bo3recomp\scripts\codegen.cmd
& C:\Windows\System32\cmd.exe /c C:\bo3recomp\scripts\build.cmd
```

## Content preparation

The supplied inputs remain in `game_files` and the original containers remain at
the project root. `scripts/prepare_content.py` creates derived copies under
`runtime` after checking the existing audit's package verification results.
Run `analysis/analyze_inputs.py --verify` again if replacing any inputs.

```powershell
& 'C:\Users\Erick\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe' C:\bo3recomp\scripts\prepare_content.py
```

The runtime applies the sibling XEXP patch, mounts the update as `update:`, and
uses common marketplace content under:

`runtime/user/0000000000000000/4156091D/00000002/<original-package-filename>/`

At startup the app writes each package's SDK metadata header with its original
display name and license mask. Compatibility Pack 1, Compatibility Pack 2, The
Giant and Awakening retain separate identities and directories. Game-driven
enumeration of all four packages and their `dlc1:` through `dlc4:` mounts were
observed during boot. The user confirmed successful loading of The Giant and
Der Eisendrache, and playable Shadows of Evil. Extended stability remains open.

## Repository contents

The private source repository is `Wiiucoop/lastgenbo3recomp`. It includes the
Zombies app and generated C++, the modified SDK source and dependency sources,
build/staging scripts, and progress/analysis records. Supplied game assets,
installed content, profiles, caches, executables, portable tools, credentials,
raw memory dumps and redundant generated translation probes are excluded.

A fresh clone still needs your own extracted game/update/DLC inputs under
`game_files`, the portable LLVM 20.1.8 toolchain under `tools/llvm-20.1.8`, and the
Visual Studio Build Tools/Windows SDK noted above. The generated Zombies C++ is
included, so rebuilding it does not require running code generation first.
The original generator bootstrap is required only by the regeneration script.

Local portable GitHub commands use `scripts/github.ps1`; its CLI binary is under
`tools/github-cli` and its configuration is under ignored `.local/github`.

## Directory map

| Directory | Purpose |
| --- | --- |
| `app` | Zombies app, manifest, diagnostics and generated C++ |
| `thirdparty/rexglue-sdk` | Isolated SDK source, initialized dependencies and build outputs |
| `sdk_snapshot` | Original source hashes, revisions and local changes |
| `tools` | Portable LLVM and original generator bootstrap |
| `runtime` | Derived game/update files, installed packages, saves, caches and logs |
| `analysis` | Input verification, translation probes and boot evidence |
| `scripts` | Build, code generation, staging and launch helpers |

The original `C:\BO2recomp\Project BO2` source and builds are unchanged.
