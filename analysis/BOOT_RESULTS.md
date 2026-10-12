# Native BO3 Zombies bootstrap results

Date: October 12, 2026. Host: Windows x64, Intel Arc B580, Xenos/D3D12.

## Completed in the proposed order

1. **Isolated SDK and toolchain.** Copied the working BO2 SDK's initialized source
   tree, including local XAM/session/XMA changes, into `thirdparty/rexglue-sdk`.
   Recorded original source hashes and dependency revisions in `sdk_snapshot`.
   Extracted portable LLVM 20.1.8 in the project. Original BO2 sources/builds and
   original BO3 inputs were left unchanged.
2. **Native Zombies build.** Generated and compiled the patched `default_zm.xex`
   into `build/bo3_zm.exe`, with the independently compiled runtime and Xenos
   plugin beside it. Native CMake/Ninja, Clang 20.1.8 and the installed MSVC/Windows
   SDK were used. Build exit codes and logs are retained in `analysis`.
3. **Content staging.** Created separate game, update and common marketplace
   content trees under `runtime`. Both compatibility packs and both optional DLC
   packages keep their original names, metadata and license masks. The app writes
   SDK-format metadata headers using `ContentManager::WriteContentHeaderFile`.
4. **Boot, tracing and initial fixes.** Applied the runtime XEXP patch, resolved
   missing indirect function entries reached during startup/UI interaction,
   configured the matching CRT jump routines, and captured native stacks and
   rendered guest frames.

## Verified runtime evidence

- Runtime applied `default_zm.xexp`: **0.0.0.5 -> 0.0.8.5**.
- The game called `XamContentCreateEnumerator` and received **four packages**.
- The game mounted the packages itself. The observed mount order in boot 06 was:

| Guest mount | Package | Successful identifier read |
| --- | --- | --- |
| `dlc1:` | The Giant | `dlc0zm.id` |
| `dlc2:` | Compatibility Pack 2 | `dlccompat2.id` |
| `dlc3:` | Compatibility Pack 1 | `dlccompat1.id` |
| `dlc4:` | Awakening | `dlc1.id` |

Mount numbering is an observed enumeration order, not a hardcoded requirement.

The successful asset-open traces in `runtime/logs/boot06-local-map.log` establish
that BO3 reads its content across the intended roots, including:

```text
update:\core_common.ff
update:\zm_gfx.ff
dlc3:\zm_common.ff
dlc2:\zm_patch.ff
update:\zm_patch2.ff
dlc3:\zm_frontend.ff
update:\zm_frontend_patch.ff
dlc3:\zm_zod.xpak
dlc3:\zm_zod.ff
```

No flattened game/update/DLC overlay was necessary for this menu path.

The Xenos backend rendered a 1280x720 title screen and the **Zombies map-selection
screen**, showing Shadows of Evil, The Giant and Der Eisendrache. The captured UI
shows some horizontal artifacts, so rendering correctness is not complete.

Screenshots:

- `boot03-10s.png`: title screen, "Press A to Start".
- `boot03-45s.png`: online connection prompt with the copied SDK's Live-profile default.
- `boot05-30s.png`: Zombies map selection with all three supplied maps listed.
- `boot06-45s.png`: Shadows of Evil loading cinematic after successful map-file opens.

## Fixes and their scope

- Added discovery roots for the two analysis-time boundary failures and reviewed
  `.rdata` dispatch-table entries omitted by discovery. Kept their original PPC
  instructions; the manifest is the source of these hints.
- Registered CRT `setjmp` at `0x82A52740` and `longjmp` at `0x82A03720` after
  matching their jump-buffer save/restore layout. Generated calls now use
  `ppc_setjmp`/`ppc_longjmp` for native control-flow restoration.
- Added runtime-reached entries `0x827A9198`, `0x8257CE30` and `0x82542368` with
  verified boundaries. `0x827351A8` is included in the reviewed table entries.
- The launcher uses the existing SDK setting `--xam_signin_state 1` for a local
  profile. The BO2 SDK's default of 2 entered "Connecting to Online Service" and
  repeatedly queried unimplemented Live APIs. BO3 online backend support is not
  established by copying BO2's service/session code.
- Added diagnostics for unregistered calls and successful asset opens. Details
  of SDK changes are in `sdk_snapshot/BO3_LOCAL_CHANGES.md`.

## Limits and next implementation gate

**Menu boot, content loading and entry into a map-loading cinematic are
demonstrated; the user also confirmed playable Shadows of Evil during boot 08.**
A diagnostic startup command
requested `+devmap zm_zod`; an early probe of `D:\zm_zod.ff` failed before package
enumeration/mounting completed. Later, boot 06 successfully opened `dlc3:\zm_zod.ff`
and `dlc3:\zm_zod.xpak` and displayed the Shadows of Evil loading cinematic.
The process terminated at about 46 seconds. Windows Application events recorded
`ucrtbase.dll`, exception `0xC0000409`, fail-fast parameter 7 (fatal application
exit). This event alone does not establish a buffer overrun. Reproduction in boot
07 with abort-stack capture and redirected stderr identified:

```text
FATAL ERROR: Graphics device lost (probably due to an internal error)
```

The abort stack ends in `rex::ui::Presenter::PaintFromUIThread`, window painting
and the SDL message loop. The current failure is a Xenos/D3D12 graphics-device
loss during map loading. Its specific GPU cause has not been established.
These two runs included optional frame capture and thread sampling. Boot 08
disabled both and omitted the guest `developer`/`logfile` diagnostics. It survived
longer, continued compiling map graphics pipelines, and the user reported
"Playable Shadows of Evil" in response to the visual verification question.
At 01:47:47, about 174 seconds after launch, that run also terminated. Its log
records the more specific failure:

```text
D3D12 device removed: HRESULT 0x887A0006 - DEVICE_HUNG (TDR - GPU command took too long)
```

It is followed by failed `PM4_DRAW_INDX`/`PM4_DRAW_INDX_2` backend draws and the
same fatal graphics-device-loss message. The abort stack again comes from the
presentation thread. Capture and sampling are therefore not required to trigger
the failure. No TDR registry settings or GPU drivers were changed.

Evidence: `runtime/logs/boot08-control.log`, its `.stderr` file and
`analysis/boot08/abort.txt`. The prior captured runs retain their frames/stacks
under `analysis/boot03` through `analysis/boot07` as applicable.

### RTV and async compilation follow-up (October 12)

The Intel adapter is an Arc B580. The SDK's automatic render-target selection
chooses ROV for Intel adapters based on an older UHD workaround. Explicit RTV
also failed with `DEVICE_HUNG`: `gpu-rtv-warm.log` crashed about 60 seconds after
launch. RTV alone is therefore not an established fix, and the launcher retains
`auto` as its default.

Two RTV runs with DRED enabled ended normally rather than reproducing the hang.
The latest, `gpu-rtv-dred-repro.log`, ran from 02:04:16 to 02:08:09. The user
reported that gameplay remained running. The log records normal window closure
and title termination. This limited test does not establish a stable workaround;
instrumentation can change timing. DRED was available despite the missing D3D12
debug layer. Logs are preserved under `analysis/crash-investigation`.

Async shader compilation is already enabled by default. D3D12 pipeline workers
translate shaders and create pipelines in the background, skipping draws while
their pipelines are pending. Draws without a pixel shader still use synchronous
compilation, and microcode analysis remains on the command processor thread.
The automatic worker count uses three quarters of the logical processors.
Cached startup work in the latest run translated 558 shaders in 20 ms and
created 410 pipelines in 124 ms, but new pipelines continued to appear during
gameplay. No frame-time measurements yet establish how much stutter comes from
compilation versus other work. The persistent cache remains enabled.

The next implementation gate is **GPU stability during gameplay**: investigate
the Xenos/D3D12 work that leads to `DEVICE_HUNG`, using the SDK's GPU debug/DRED
support and targeted shader/command tracing. BO3 now reaches gameplay with the
installed update and compatibility packs, but the current prototype is unstable.

The subsequent RTV/alternate-binding test reached gameplay and ended normally.
A warmed-cache performance run also ended normally. A comparison with two
pipeline workers crashed about 11 seconds after launch, at 02:28:13.993.
DRED recorded 26 of 29 completed operations; the next operation was type 7
(`CopyBufferRegion`) and a GPU page fault occurred at `0x0000B80227368000`.
The responsible resource and error are not yet identified. Alternate binding
and DRED do not establish stability. Evidence is preserved under
`analysis/crash-investigation/bindful-two-workers`.

The user requested DLC tests before further FPS comparisons. Current progress
is consolidated in `PROGRESS.md`; measurements are in `analysis/PERFORMANCE.md`.

The user subsequently confirmed that **The Giant and Der Eisendrache both load
successfully**. `dlc-map-test.log` records The Giant (`zm_factory`) loading from
`dlc1:` around 02:30:12 and Der Eisendrache (`zm_castle`) from `dlc4:` around
02:32:08, including `update:` castle patches. The process closed normally at
02:33:21; no device-removal error appears in this run. This confirms map loading,
not extended gameplay stability.

Remaining issues include additional constructed function-pointer targets, online
APIs, rendering artifacts and mode switching. The generator's existing 20
`float16_4` packing warnings remain; successful menu rendering does not establish
correctness for every use in gameplay. Audio playback and controller behavior
have not been separately validated. BO3 multiplayer has translation probes but
no native host target in this bootstrap.

Use `scripts/launch-zombies.ps1` for the local-profile prototype. The full directory
map, rebuild/staging commands and diagnostics are in the project `README.md`.
`analysis/build_provenance.json` records hashes of the current binaries, compiler,
manifest and runtime XEX/XEXP pair.
