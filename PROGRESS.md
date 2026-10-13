# BO3 RexGlue progress and limitations

Updated October 12, 2026 (America/New_York).

## Current state

On branch `NX`, Nintendo Switch groundwork has started. The installed devkitA64
and Mesa/libnx environment builds a diagnostic ARM64 NRO with startup at zero.
It has not been tested on the console and does not load BO3. The BO3 compile
probe confirms missing Horizon platform/fiber support in the current desktop
SDK; a complete runtime port and playable performance are still unproven.
The user confirmed original Switch/OLED hardware and title takeover.
See [NX port plan](docs/NX_PORT.md) and `analysis/nx_environment.json`.

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
