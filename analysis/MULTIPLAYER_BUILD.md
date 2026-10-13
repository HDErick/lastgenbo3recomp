# Native BO3 multiplayer target

Updated October 12, 2026 (America/New_York).

## Executable and translation

The new `bo3_mp` target selects `game:\default.xex`. Code generation uses the
staged base executable and its sibling `default.xexp`, applying version
0.0.0.5 -> 0.0.8.5. Manifest: `app/multiplayer/bo3_mp_manifest.toml`.

The initial translation registers 34,960 functions and emits 87 C++ files.
No unresolved branch/call placeholders were found in those generated sources.
Twenty float16 packing warnings remain, as in the original multiplayer probe.
Passing translation is not proof of runtime or graphics correctness.

## CRT verification

The multiplayer `setjmp` is at `0x82A60780`; `longjmp` is at `0x82A11500`.
Their generated PPC listings were compared with the verified Zombies routines:
longjmp matches all 197 instructions after normalizing branch addresses;
setjmp matches 179 instructions except its first two instructions loading an
executable-specific global hook. The jump-buffer layout is the same.
Evidence: `analysis/mp_crt_matches.json`.

## Native host

The multiplayer host inherits the BO3 content-installation and optional
diagnostic behavior from the existing host, and overrides the selected XEX and
application name. Its generated CPU code is linked into a separate executable.
The parent CMake project configures the SDK once for both targets.

Build:

```powershell
& C:\Windows\System32\cmd.exe /c "C:\bo3recomp\scripts\build.cmd bo3_mp"
```

Regenerate:

```powershell
& C:\Windows\System32\cmd.exe /c C:\bo3recomp\scripts\codegen-multiplayer.cmd
```

Launch:

```powershell
.\scripts\launch-multiplayer.ps1
```

The launcher uses a local signed-in profile and explicit game/update/user/cache
roots. Its normal renderer settings match the existing launcher. The recent
shared SDK coherency repairs are included; the separate color-flashing
investigation remains paused.

## Validation status

The initial native Release build succeeded (`analysis/build_multiplayer.log`).
The first boot applied the matching update and began loading core assets, but
aborted on an unregistered indirect callback at `0x82742828`, LR `0x827425C0`.
Evidence: `analysis/mp_boot01.log` and `analysis/mp_boot01_abort.txt`.

The loaded multiplayer image was saved locally to audit dispatch tables. The
manifest now includes 34 reviewed callback entries, including that runtime
target; 12 candidates in exception-metadata/landing-pad groups were excluded.
Evidence: `analysis/mp_indirect_function_candidates.json` and
`analysis/mp_indirect_function_review.json`. The corrected build succeeded
(`analysis/build_multiplayer_boot02.log`), registering 34,992 functions with no
unresolved branch/call placeholders. Boot check `runtime/logs/mp-boot02.log`
rendered the title screen (captured at 30 seconds). The user pressed Start/A;
profile loading then aborted on `0x822CB0A8`, LR `0x822DB0AC`.
Evidence: `analysis/mp_boot02.log` and `analysis/mp_boot02_abort.txt`.

That callback has 17 PPC instructions ending in `blr` at `0x822CB0E8`, with
padding before the next prologue. It is now seeded with the verified exclusive
end `0x822CB0EC`. Its function pointers occur in initialized `.data` at
`0x82B12DC0` and `0x82B12E50`; the original dispatch-table audit only scanned
`.rdata`. The audit now covers both sections. One additional four-instruction
virtual dispatch thunk at `0x828EE988` is also seeded. The 12 exception-metadata
candidates remain excluded. Rebuild log: `analysis/build_multiplayer_boot03.log`.

The profile-loading rebuild succeeded with 34,994 registered functions. In
boot03 the user reported substantially more menu progress before another abort:
`0x8257E598`, LR `0x8258E324`. Evidence: `analysis/mp_boot03.log` and
`analysis/mp_boot03_abort.txt`. This was an unregistered CPU callback, with no
GPU-loss report in that failure.

The new target is a two-instruction wrapper: `mr r6,r7`, followed by a tail
branch to the already registered `0x82584C58`. Its next function begins at
`0x8257E5A0`. It matches the previously required Zombies wrapper `0x8257CE30`,
allowing for the changed branch displacement. It is now seeded with that
exclusive end. An exact 112-byte counterpart to the previously needed Zombies
callback `0x82542368`, at MP `0x825431F8`, is also seeded with exclusive end
`0x82543268` (return at `0x82543260`, then padding). Its comparison evidence is
recorded in `analysis/mp_equivalent_callback_candidates.json`. The latter has
not yet been observed as a runtime failure. Rebuild log:
`analysis/build_multiplayer_boot04.log`. That build succeeded with 34,996
registered functions and no unresolved placeholders.

Boot04 used the same normal renderer settings as boot03. The user reported
"seems to play fine". The log references `mp_skyjacked` during two map-load
sequences, with a return to frontend content in between. It ran from 23:10:03
to 23:13:31 on October 12, 2026 (about 3 min 28 s total process lifetime), then
logged normal window closure and execution completion. No unregistered-call,
fatal-abort or device-loss messages were found. The readback-lifetime guard
also deferred pending-buffer evictions during the run. Archived evidence:
`analysis/mp_boot04.log`.

The native multiplayer build now has user-confirmed playable gameplay in this
short test. The map identification above comes from the log; the user did not
name the tested map or describe the match settings. Extended gameplay, all maps
and online/network play remain unverified. The log contains failed localized
asset probes; their visible or audible impact has not been assessed. Seamless
switching between executables is not implemented by adding the second target.
