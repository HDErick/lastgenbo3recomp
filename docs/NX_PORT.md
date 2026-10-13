# BO3 Xbox 360 recompilation: Nintendo Switch port

Started October 12, 2026. Development branch: `NX`.
PC baseline: commit `4acffac349c9d748e5f8f2a5707fed1f0564ca04`.

## Current result

The attempt is technically credible: both updated BO3 executables already run
on PC, and another RexGlue project supplies a working Horizon platform layer.
Playable BO3 performance on Switch is **unproven**. A straightforward build of
the current desktop SDK is not sufficient, and there is no defensible FPS
prediction yet. A 30 FPS target means a 33.3 ms frame budget; it is a development
target, not a promised result.

Completed in this first stage:

- Created `NX` from the playable PC baseline.
- Read the NFSMW porting guide and its building, platform, renderer, shader,
  measurement, toolchain, driver and performance documentation. Checked the
  relevant platform code against the documentation.
- Downloaded the reference under ignored `tools/nfsmw-nx-reference`, pinned at
  `be2d4c67ab42d6d629746a01dbf03cef824c8d2d`.
- Inspected the Switch setup in `C:/T6ZM-Rebuild-NX` and `C:/KisakBlack-NX`.
- Recorded installed dependencies and source revisions in
  `analysis/nx_environment.json`.
- Built `build/nx-preflight/bo3_nx_preflight.nro` against installed libnx and
  Mesa/NVK. Verified ELF64, AArch64 and `_start` at address zero.
- Ran a compile-only probe of the first existing Zombies C++ chunk. The current
  SDK rejects the Switch OS. Build-local platform and alias adaptations proceed
  to a missing `ucontext.h` dependency. That probe did not produce a BO3 ARM64
  object; the subsequent full Horizon build is described in NX_RUNTIME.md.
  Evidence: `analysis/nx_cpu_probe.json` and the two associated compiler logs.

The environment NRO does not load game files, execute BO3, present a Vulkan
frame or measure gameplay. Its first hardware environment check passed on
October 13, 2026; details follow below.
The existing desktop SDK and generated BO3 sources were not altered by the
compile probe. Its adapted headers live only under ignored `build/`.

## Reference and reuse

The [NFSMW porting guide](https://github.com/StevensND/nfsmw-nx/blob/be2d4c67ab42d6d629746a01dbf03cef824c8d2d/docs/porting-another-game.md)
provides the order: establish PC correctness, adapt the platform, obtain first
frames, prepare shaders, measure on the console, then optimize. BO3 has reached
the PC gameplay milestone, with limited coverage and known rendering defects.

| Component | Reuse assessment | BO3-specific work |
|---|---|---|
| Horizon memory and fault handling | Strong candidate in NFSMW `sdk/src/core/*_switch.*` | Validate BO3 aliases, MMIO, write watches and concurrent faults |
| Threads, fibers, clocks, crash reporting | Reusable platform foundation | Map guest priorities; check worker contention, TLS, waits and shutdown |
| Controller/audio/window support | NFSMW SDK has libnx implementations | Confirm BO3 input mappings and XMA behavior; preserve content/profile setup |
| Vulkan/NVK driver | Installed static SDK exists | Query features on hardware, verify driver revision, then assess reference patches |
| Native command-ring renderer | NFSMW is a reference implementation | BO3 formats, shader identities, passes, resolves, readbacks and synchronization need their own validation |
| Shader translation tools | XenosRecomp and DXC workflow are useful | Extract BO3 shaders from its actual updated assets and identify runtime variants |
| Native engine replacements | Method is reusable | Every address, structure, replacement and guard must be established for BO3 |
| T6ZM/Kisak runtime bridges | Their build practices and diagnostics are useful | PC D3D9/D3D11 bridges do not implement Xbox 360 Xenos command processing |

The [reference notices](https://github.com/StevensND/nfsmw-nx/blob/be2d4c67ab42d6d629746a01dbf03cef824c8d2d/THIRD_PARTY_NOTICES.md)
place the Horizon SDK additions under BSD-3-Clause, shader/driver changes under
their project licenses, and the app under GPL-3.0. Preserve notices and identify
the applicable license when importing code. The current preflight uses our own
host code and the installed dependencies; it does not import NFSMW app sources.

## Installed environment

- devkitPro: `C:/devkitPro`; devkitA64 GCC **16.1.0**.
- libnx: headers, static library, linker specs and packaging tools present.
- Mesa/NVK: `C:/devkitPro/portlibs/switch/lib/libvulkan.a` present, as are
  Vulkan headers/configuration, libelf, expat, zstd and zlib.
- FFmpeg codec, utility and resampling archives present. Presence does not prove
  the codec configuration is suitable for RexGlue XMA decoding.
- The installed Vulkan CMake config reports **26.2.0**. The Kisak project refers
  to a 26.2.3 build and records another library hash. The installed archive's
  SHA-256 differs from that lock file. Treat driver identity as unresolved until
  hardware reports its properties; a configured package version is not proof of
  the actual compiled driver version or patches.
- Native Windows CMake 3.31 from Visual Studio Build Tools is usable. The initial
  native-CMake/MSYS-Ninja combination failed because the MSYS shell interpreted
  Windows backslashes. The working script pins native Windows Ninja from the
  same Visual Studio installation for this separate build directory.
- Inherited `DEVKITPRO=/opt/devkitpro` is a shell path, unsuitable for native
  Windows CMake. Our script supplies absolute `C:/devkitPro` paths explicitly.

The user confirmed original Switch/OLED hardware with **title takeover**.

### First console test: October 13, 2026

Archived log: `analysis/nx_preflight_hardware01.log`. Structured results:
`analysis/nx_preflight_result.json`.

| Check | Result |
|---|---|
| Applet mode | 0, `AppletType_Application` in installed libnx; appropriate for title takeover |
| Available core mask | `0x7`, three application CPU cores |
| System timer | 19,200,000 Hz; this is not the CPU clock |
| Worker creation/start | Both succeeded |
| Worker NEON result | 28, matching the expected value |
| Vulkan instance/device | Both succeeded (`VK_SUCCESS`) |
| GPU | NVIDIA Tegra X1 / GM20B, NVK |
| Reported Vulkan API | 1.3.354 |
| Raw driver version | `0x06802001`; exact driver patches remain unidentified |
| GPU heap | 2,391 MiB capacity, not free memory |
| Queue | One queue in family 0, flags `0x7` (graphics, compute, transfer) |
| Queried shader features | int64, geometry and tessellation supported |
| Timestamp period | 1 ns reported; calibration remains pending |
| Input/exit | User confirmed + exits normally without an error |

The reported total process memory is 3,343,908,864 bytes (3,189 MiB).
UsedMemorySize includes the startup heap allocation and cannot be subtracted
from that total to estimate usable game memory. GPU frame presentation, BO3
execution, full renderer feature coverage and playable performance are not
established by this test. The environment milestone has passed; the next work
is Horizon runtime adaptation.
That work is now underway; [NX_RUNTIME.md](NX_RUNTIME.md) describes the isolated
SDK, BO3 host, build scripts and first game-test package.

## Runtime migration decision

BO3 uses SDK v0.9.0 with local fixes. The NFSMW Switch SDK is based on v0.10.0
(`c94f5ebdcb3c9d1a460ca48e04f9758448f8d518`). Its platform work cannot simply be
switched on by changing one CMake option in the BO3 tree.

Next implementation should use a separately staged Switch SDK based on that
reference, then reconcile BO3's required local changes and generated interface.
This avoids making an unreviewed replacement of the validated desktop SDK.
The source inventory records differences excluding line-ending changes. Some
differences are upstream changes and some are port changes; a file count alone
does not classify them.

Required work includes static runtime/plugin linkage, Horizon memory and fiber
implementations, libnx input/audio/UI, ARM fault handling, and newlib-compatible
dependencies. The host also needs a Switch crash-reporting path: current
`crash_reporter.cpp`, `dbghelp` and linker `/DEBUG` options are Windows-specific.
Normal generated functions use weak aliases; GCC requires the correct C
linkage in the generated macro. The diagnostic overlay contains that proposed
adaptation, but the sample stops in a dependency header before validating all
generated function bodies.

Keep host pointers 64-bit. RexGlue stores Xbox pointers as guest addresses and
translates them against the guest-memory base. T6ZM's ILP32 guest ABI is specific
to its recovered PC structures and is not the starting ABI for this port.

Retain the exact updated BO3 images, native jump-buffer configuration, discovery
hints, four package identities/licenses and local profile behavior. BO3 needs
the matching title update and compatibility content to proceed into gameplay;
NFSMW's base-disc-only package layout is insufficient. Initially build separate
Zombies and multiplayer hosts, as on PC, starting with Zombies.
If migrating the code generator, first compare the new translation and run it
on PC with BO3's verified configuration. Confirm callback coverage, native
jump-buffer behavior and map loading before diagnosing it on Horizon.

## Graphics and shader feasibility

The [reference renderer](https://github.com/StevensND/nfsmw-nx/blob/be2d4c67ab42d6d629746a01dbf03cef824c8d2d/docs/native-renderer.md)
was written after measuring a CPU limit in its original Xenos backend. This is
evidence that a Switch build can require substantial renderer work; it does not
predict BO3's speed. Our initial correctness backend should be Vulkan, followed
by a measured decision about a BO3 native command-ring path.

A native path must honor BO3 packet types, formats and synchronization. Color,
depth, MSAA, memexport, CPU-visible resolves and resource coherency are coverage
questions, not features we can discard by assumption. Missing packets need
diagnostic reporting or a validated fallback. Terrain rendering alone would not
prove that models, animation, skinning or effects work.

The [shader workflow](https://github.com/StevensND/nfsmw-nx/blob/be2d4c67ab42d6d629746a01dbf03cef824c8d2d/docs/shaders.md)
uses offline microcode translation and pipeline warm-up. BO3 needs its own
container/fastfile investigation, extraction and manifest. NFSMW's 2005 shader
container converter, identifiers and library are not BO3 assets. Vertex shader
microcode may be patched by guest Direct3D; lookup must preserve those variants.
Record shader hashes and unknown-shader counts by map, model/effect and update.
Offline SPIR-V removes one stage of compilation; NVK pipeline creation can still
stutter unless the actual pipelines are cached and warmed.

The Giant's model color flashing remains unresolved on PC and paused at the
user's request. Record it as a correctness limitation of the baseline. A new
backend may behave differently, but no fix is implied by this port.

## Memory and scheduling

The [platform notes](https://github.com/StevensND/nfsmw-nx/blob/be2d4c67ab42d6d629746a01dbf03cef824c8d2d/docs/platform-notes.md)
identify Horizon mapping limits, shared exception-state races, thread scheduling
and costly GPU allocation/submission paths. Validate those behaviors against the
actual installed libraries. The reference's current guest-memory source uses
2 MiB backing blocks while its guide describes 4 MiB chunks; use the pinned code
as the implementation reference and tune only after measurements.

BO3 requires a large sparse guest address space, not a physical allocation of
that entire space. Account separately for physical backing, alias mapping
charges, executable/data image, stacks, exception slots, GPU resources, upload
buffers, shader caches and staging peaks. Never count the same physical mirror
as extra RAM, or ignore the kernel's separate mapping resource charge. The
preflight reports raw process memory information, explicitly not a free-heap
estimate; real allocator and GPU budget accounting belongs in the runtime.

Use the available application cores without starting one busy host worker per
Xbox hardware thread. Identify spinning guest GPU/frame waits before adding
compilation workers. Memory watchers, DMA/readbacks and cache invalidation must
remain correct across ARM's ordering rules and GPU cache maintenance.

## Ordered milestones and acceptance evidence

| Stage | Work | Evidence required |
|---|---|---|
| 0: environment — passed | Supplied preflight ran in title takeover | Hardware01 log, Vulkan/device success, three cores, worker result and user-confirmed clean exit |
| 1: runtime | Stage/reconcile Horizon SDK; compile both image interfaces | Full ARM64 build and retained ELF; no placeholder linkage or fake service success |
| 2: boot | Preserve update/content mounts and local profile | BO3 startup log, title/menu frame and controller interaction |
| 3: gameplay | Load Shadows of Evil; then Giant and Der Eisendrache | Gameplay, content mounts, input/audio and repeated load/exit checks |
| 4: coverage | Trace command ring and capture shaders/pipelines | Packet, format, shader and render-pass coverage; frame comparisons |
| 5: baseline | Measure warm/cold sessions on the console | Per-thread CPU, calibrated GPU, memory, draw/upload counts and frame-time distribution |
| 6: optimize | Apply one measured change at a time | Within-session A/B controls, correctness guards and reproducible logs |
| 7: multiplayer | Bring across its verified separate image configuration | Local map gameplay and stability; online services remain separate work |

CPU build candidates from the [toolchain guide](https://github.com/StevensND/nfsmw-nx/blob/be2d4c67ab42d6d629746a01dbf03cef824c8d2d/docs/toolchain.md):
safe register localization with split-function sharing, direct calls that preserve
hooks, LTO, then console-generated PGO and profile-based function ordering.
Audit read-before-write and tail/indirect branches before register localization.
Retain required link-register behavior and jump-buffer semantics. Validate
`_start` at zero on every release, and keep an unstripped ELF and hashes.
NFSMW profiles and hook-address lists cannot train or describe BO3.

Measured rendering candidates include fewer redundant uploads, batched
suballocation, cheaper state/pipeline lookup, necessary resolves only, optimized
shader constants, visibility/LOD and shadow/effect cost controls. Native engine
replacements need known BO3 addresses and comparisons against original results.
No whole-game fast-math setting or unverified packet removal is justified.

Follow the [measurement guide](https://github.com/StevensND/nfsmw-nx/blob/be2d4c67ab42d6d629746a01dbf03cef824c8d2d/docs/measuring.md):
stock-clock console sessions, thread CPU rather than busy wall time, controlled
A/B intervals, and slow-frame percentages. Recalibrate GPU timestamps for the
chosen driver; the guide's 1.627 correction is a reference observation, not a
universal constant to hardcode. Report docked/handheld mode and actual clocks.
PC timings can locate work but cannot establish a Switch frame budget.

## Build and run the environment probe

```powershell
cd C:\bo3recomp
.\scripts\nx\build-preflight.ps1
```

The script defaults to installed Mesa, or accepts `-DevkitPro` and `-MesaSdk`.
Copy `build/nx-preflight/bo3_nx_preflight.nro` to
`sdmc:/switch/bo3recomp/bo3_nx_preflight.nro`. Launch through title takeover.
It displays the checks, writes `sdmc:/switch/bo3recomp/nx-preflight.log`, and
exits with **+**. Return that log for the runtime implementation stage.
No game files are needed for this environment probe.

Host-side repeatable analysis:

```powershell
& 'C:\Users\Erick\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe' scripts/nx/inventory.py
& 'C:\Users\Erick\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe' scripts/nx/probe-recompiled-code.py
```

The CPU probe's compiler failures are recorded diagnostic results. They are
not a successful BO3 build. Logs, provenance and sources belong in Git; game
content, local caches, credentials and compiled NRO/ELF artifacts stay excluded.
