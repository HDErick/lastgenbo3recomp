# Native renderer performance investigation

October 13, 2026, branch `native-rendering-pc`.

## Run 27: visible depth geometry, inadequate rendering rate

The user's F9 screenshot of The Giant shows recognizable stairs, railings,
structures and zombies. World geometry reaches the selected native depth buffer.
This does not establish complete geometry, numerical depth parity, lighting or
final color output. Normal view is reported mostly black, with some content,
and too slow to inspect. The user closed the window; the complete log ends with
window closure/title termination rather than a fatal native backend report.
The supplied image is retained locally, excluded from Git, at
`runtime/logs/pc-native-backend27-depth-user.png`.

Three consecutive 30-presentation intervals near map entry/gameplay:

| Wall interval | Presented swaps/s | Queue/submission/fence scope | Texture scope | Pipeline creation | Shader translation | Submissions |
|---|---:|---:|---:|---:|---:|---:|
| 36.942 s | 0.81 | 13.437 s | 4.838 s | 11.233 s | 47.9 ms | 1,527 |
| 34.063 s | 0.88 | 16.013 s | 4.921 s | 5.199 s | 14.4 ms | 1,635 |
| 44.631 s | 0.67 | 19.966 s | 7.121 s | 6.323 s | 21.8 ms | 2,079 |

The first interval includes map transition. These are presentation rates across
user navigation, not a matched-scene benchmark or game simulation speed. Elapsed
scopes include waiting/scheduling and can nest; do not sum them or label queue
time pure GPU execution. Pipeline creation is a substantial cost; translation
alone does not explain the crawl. Many synchronous submissions and texture
processing also contribute. Separate guest threads may advance while rendering
falls behind; no simulation-rate measurement has been made.

The run reached the fixed 256-entry texture-cache cap and refused new textures.
Unsupported copy packets returned false and triggered repeated PM4 backend-error
messages despite being intentionally consumed. Native depth resolves, numeric
texture conversion and incomplete resolve coverage remain rendering gaps.

Evidence: `analysis/pc_native_backend_boot27_color_resolves.log`, especially the
timing reports at 20:01:24, 20:01:58 and 20:02:43 in the log's host clock.

## Batching and cache changes

- Validate export plans, source availability and destination permissions before
  flushing. Actual CPU export writes still wait for prior GPU work to retire.
- Preflight resolved-texture coverage before allocating draw descriptors/uploads.
- Batch supported color resolves and post-resolve clears with nearby draws.
  Keep image/attachment barriers and retire before presentation, command-ring
  boundaries, command-processor waits, readback and CPU exports.
- Keep rejected-draw uploads alive in their batch. Descriptor allocation attempts,
  including rejects, count toward the limit; reset pools and the upload arena
  only after retirement. Resolve descriptors have a bounded allocation count.
- Consume unsupported copies with rate-limited native counters, avoiding the
  parser's repeated failure logger. Unsupported output remains unavailable.
- Evict the least-recently used texture eligible for retirement. Current-batch
  images/samplers are protected; earlier batches have completed synchronously.
  The image-memory budget is unchanged.
- Canonicalize pipeline identity to static fields used by this backend. Dynamic
  raster/clip fields and disabled depth/stencil/blend state no longer multiply
  variants. Thirteen fixtures preserve meaningful changes to depth compare,
  culling, stencil, blending and clamp behavior.
- Add batch-boundary reasons, texture hits/misses, hash byte volume and eviction
  counts. Build 51 still hashes fresh guest bytes on each lookup.

Build 50 linked and all 88 CPU checks passed, but run 28 stalled after its first
presentation with a black window. The user closed it. No performance conclusion
can be drawn from that run. Build 51 restores original immediate startup
target-clear submissions and adds `PrepareForWait`/`OnPrimaryBufferEnd` retirement.
The exact stall cause is unconfirmed. Run 29 advances normally beyond startup;
early frontend intervals record 30–34 swaps/s, roughly 170 submissions per
30 swaps and zero translation/pipeline time after warming. The user subsequently
confirmed faster menus but still unplayable gameplay, with a dark scene and
apparent red trails as they move. F9 was not tested in this run. In-game reports
record 1.56 and 1.03 swaps/s; the latter includes 1,600 draw-limit flushes and
20,114 MiB hashed per 30 swaps. This is not a matched-scene FPS comparison.
The run ends with normal window closure. The normal-view screenshot is retained
locally at `runtime/logs/pc-native-backend29-normal-user.png`.

### Build 54: larger batches and watched texture reuse

The draw allocation limit increases from 32 to 128; the arena remains 16 MiB,
with bounds-based retirement and immutable per-draw uploads. CPU texture cache
hits may skip a repeat hash within a primary command-ring chunk only while their
base/mip source is clean. Fresh committed/readable permission checks remain on
every lookup. Watches are armed before hashing and queued invalidations drained
after capture; writes and allocation changes dirty intersecting resources.
Explicit native physical export writes invalidate without relying on a fault.
TC coherency actions and each primary-buffer end expire clean reuse conservatively
for PM4/direct writers. Existing armed watches are retained across these expiry
points until an actual invalidation requires rearming.

The actual SDK memory/write-watch fixture passes ten cases: fault callbacks,
one-shot behavior, separate resource invalidation, mip writes, rearming,
physical-mapping bypass, explicit native invalidation, non-overlap, crossed
boundaries and allocation release. Compiler barriers reload callback state
after faulting test stores; early fixture failures came from optimized cached
handler-state reads, rather than evidence of incorrect SDK invalidation.
All 98 CPU checks and five shader validations pass. No game assets are loaded
by these fixtures. In run 30, hash traffic fell to roughly 415–418 MiB and
submissions to 452–480 per 30 swaps in sampled in-game intervals. Texture scopes
fell to roughly 1.1–1.4 seconds, and warmed pipeline creation was milliseconds.
Gameplay still records 1.4–1.6 swaps/s. Different scenes, draw counts and player
actions prevent assigning a controlled FPS benefit to these changes. The user
confirmed gameplay remains unplayable, F9 geometry still looks correct and
normal view remains faulty. The run was closed by the user.

Apparent red trails may indicate a missing clear/overwrite/compositing operation
or incorrect target ownership. Do not infer that flushing more submissions will
repair it, and do not blindly clear every target each frame: some targets hold
intermediate or persistent data. Resolve/clear alias support and final color
passes still need investigation after performance permits useful inspection.

Evidence: `analysis/pc_native_backend_build49.log` through `build51.log`,
`analysis/native_texture_upload_tests08.log`,
`analysis/native_batching_validation02.json`,
`analysis/pc_native_provenance31_prepared.json`, and
`analysis/pc_native_backend_boot28_batching.log`.
Further evidence: `analysis/pc_native_backend_boot29_batching_waits.log`,
`analysis/pc_native_backend_build54.log`, `analysis/native_texture_upload_tests09.log`,
`analysis/native_batching_validation03.json`, and
`analysis/pc_native_provenance32_prepared.json`.

### Build 55: bounded fresh permission checks

Source inspection found another repeated cost in `PhysicalAccessible`:
`QueryRegionInfo` scans forward to the end of a matching allocation region,
even when the requested read is just a small index/vertex span. Repeating that
scan on many draws can visit far more pages than the GPU input actually needs.
Its old contribution was not separately timed, so it is a confirmed inefficient
algorithm, not a measured attribution of all remaining frame time.

The helper now uses the SDK's locked `QueryRangeAccess` over the requested
inclusive page range. A reproducible adaptation in `prepare-runtime.py` adds
commitment checks to that API: reserved pages can retain protection metadata and
must still be refused. No permission cache is introduced. Ten guard fixtures
pass for empty/overflow/end bounds, unaligned spans, read-only/reserved/no-access
pages and exact ranges. All 108 CPU checks and five shader validations pass.
New guest-access counters report check count and elapsed guard time. The
user-authorized run 31 is open; performance and regression results are pending.

Early run 31 frontend reports exceed 50 swaps/s. One mixed map-entry interval
reports 15.34 swaps/s and 18,174 guest-access checks in 2.3 ms; it contains both
linear and tiled texture uploads. This verifies low guard cost in that sample,
not matched-scene gameplay performance. See
`analysis/pc_native_backend_boot31_bounded_guards_snapshot.log` and
`analysis/pc_native_provenance34_test.json` for this partial live observation.
Later warmed in-game reports show 3.21 and 4.80 swaps/s. They record about
157,000 guest checks taking 25–28 ms per 30 swaps, zero shader/pipeline time,
roughly 363–420 ms texture time and 2.1–4.9 s in submission/fence scopes.
Performance remains low; player actions and scene complexity are not controlled.
Normal-view/F9 confirmation for run 31 is pending.

Evidence: `analysis/pc_native_backend_boot30_texture_watch.log`,
`analysis/pc_native_backend_build55.log`, `analysis/native_texture_upload_tests10.log`,
`analysis/native_batching_validation04.json`, and
`analysis/pc_native_provenance33_prepared.json`.
