# BO3 RexGlue progress and limitations

Updated October 12, 2026 (America/New_York).

## Current state

The updated Xbox 360 Zombies executable builds as `build/bo3_zm.exe` and reaches
playable Shadows of Evil, confirmed by the user. The user also confirmed that
The Giant and Der Eisendrache both load successfully. Gameplay remains unstable:
multiple tests lost the D3D12 device. No tested GPU configuration is an
established fix. FPS tests are paused at the user's request while testing DLC
maps from the menu.

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

- **GPU stability:** `DEVICE_HUNG` occurred with automatic ROV, explicit RTV,
  and RTV with alternate resource binding. Short successful runs do not prove
  stability. The latest two-worker experiment crashed about 11 seconds after
  launch. DRED showed an unfinished `CopyBufferRegion` operation and page-fault
  VA `0x0000B80227368000`; the responsible resource/error is unidentified.
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
- BO3 online services, a multiplayer host and seamless mode switching are not
  implemented. Audio and controller behavior were not separately validated.
- Generator float16 packing warnings, further indirect-function targets and
  rendering artifacts require investigation when encountered.

No GPU driver or TDR registry changes were made. The original BO2 project is
unchanged. The normal launcher retains automatic render-target selection;
RTV, alternate binding and GPU diagnostics are explicit test settings.

## Detailed records

- [Launch and rebuild](README.md)
- [Boot fixes and runtime evidence](analysis/BOOT_RESULTS.md)
- [Measured performance](analysis/PERFORMANCE.md)
- [Initial feasibility](FEASIBILITY.md)
- [Input verification](analysis/RESULTS.md)
- [SDK changes](sdk_snapshot/BO3_LOCAL_CHANGES.md)
- [Binary and toolchain hashes](analysis/build_provenance.json)

## Next work

The project is being versioned in the private repository
`Wiiucoop/lastgenbo3recomp`. Game content, credentials, caches and compiled tools
are excluded; source, scripts and progress records are retained.

1. Investigate DRED buffer-copy/page-fault evidence to identify the GPU failure.
2. Verify extended gameplay stability on the supplied maps.
3. Resume FPS comparisons when requested, separating new content compilation
   from repeated views with a populated cache.
