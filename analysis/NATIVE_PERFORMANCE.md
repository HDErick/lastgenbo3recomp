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
The user subsequently confirmed only marginal performance improvement and
unplayable gameplay. In normal view, a zombie barrier and the entire window
object are visibly drawn; most of the scene is still absent/dark. Their image
appears to persist/redraw as the camera moves. This may be a clear, overwrite,
compositing or ownership defect; no single cause is established. The full run
closed normally. Later samples fall to 1.87–2.88 swaps/s, with roughly 570–790
submissions per 30 swaps; a new-content interval includes 1.56 seconds of
pipeline creation, while most warmed intervals report zero compilation.

At swap 5160, skipped-path totals include 303,940 numeric-conversion texture
rejects, 226,428 texture-extent rejects, 11,096 resolve-number-format rejects,
3,475 depth-resolve rejects and 3,456 incomplete resolve reader rejects. These
are cumulative packet/path counts, not counts of uniquely missing objects.
They demonstrate that significant rendering support remains incomplete.

This native plugin owns Vulkan draws/resources but still executes BO3's original
GPU command stream and MW2-derived Xbox shader translations. Per-draw snapshots,
descriptors, barriers, synchronization and unoptimized translated shaders remain
costs. Native ownership alone does not guarantee higher FPS. CPU recompilation
and a complete, efficient renderer are separate requirements for the Switch goal.

Full evidence: `analysis/pc_native_backend_boot31_bounded_guards.log`.

## Build 56: follow the resolved guest frontbuffer

Normal presentation previously sampled `latest_`, the most recently drawn native
target. That can expose an intermediate image or a source after post-resolve
clearing. Supported resolves now exist, so presentation should follow the actual
guest frontbuffer address. Build 56 selects the newest matching, fully covered
RGBA8 resolve image with verified zero-endian display interpretation. It crops
aligned pitch/height padding via an eight-byte fragment push constant. Missing
or unsupported frontbuffer coverage retains the logged diagnostic fallback;
F8/F9 explicitly retain native target views.

This is a confirmed missing presentation path, but its contribution to the red
trails/dark scene has not been proven. Six address/format/endian/coverage fixtures
bring the total to 114 CPU checks. All five shaders structurally validate, and
compiled reflection checks the new presentation descriptor and push ABI. The
user-authorized run 32 is open for comparison. Remaining texture/conversion,
resolve, clear and alias support may still omit color passes; this build does
not establish a faster or complete renderer.

The live run 32 log confirms that resolved frontbuffer output is selected in
both frontend and early map-entry intervals. That establishes source selection
and submission, not correct pixels. Snapshot/provenance:
`analysis/pc_native_backend_boot32_resolved_frontbuffer_snapshot.log` and
`analysis/pc_native_provenance36_test.json`.

Evidence: `analysis/pc_native_backend_build56.log`,
`analysis/native_texture_upload_tests11.log`,
`analysis/native_frontbuffer_validation01.json`, and
`analysis/pc_native_provenance35_prepared.json`.

## Build 57: display channel mapping and working-set churn

Run 32 ended by normal window close. The user reports that red/blue appear
exchanged across menus and cutscenes, with FPS worsening over a few seconds.
Native frontbuffer presentation omitted the fetch swizzle. VdSwap copies the
actual display fetch into slot zero before its swap packet; the SDK's Vulkan
texture cache also uses that fetch's swizzle when requesting its swap image.
Build 57 checks its resource identity and applies all four channel selectors.
Run 33 confirms `fetchSwizzle=A0A`, meaning B,G,R,one, with `resolveSwapRB=true`.
The user confirms colors are normal in run 33; gamma ramps remain separate
unfinished support. FPS is still bad, and zombies are visible in normal view
when in front of a visible object. This does not prove a particular blend or
depth/compositing defect.

Run 32 provides a concrete repeated-resource cost, beyond initial compilation:

| Late interval (30 swaps) | Swaps/s | Texture misses | Upload MiB | Texture wall ms | Queue/fence ms | Shader ms |
|---|---:|---:|---:|---:|---:|---:|
| Map working set expands | 2.16 | 969 | 169.74 | 1493 | 5228 | 30.5 |
| Subsequent interval | 0.91 | 7513 | 1060.50 | 8303 | 14451 | 9.6 |
| Subsequent interval | 0.86 | 8438 | 1263.37 | 8250 | 17534 | 6.9 |

The corresponding cumulative texture evictions are 860, 8,336 and 16,744.
The prior report at swap 4320 records 256 cached textures and 225 MiB aggregate
images. The arbitrary entry cap evicts the active working set despite available
byte budget. Re-decoding/re-uploading then creates extra arena submissions.
Elapsed timing scopes overlap and are not additive CPU/GPU measurements; queue
waits are still a major cost and cache churn is not the sole remaining limit.

Build 57 removes normal count-based eviction. Vulkan allocation requirements
drive pressure against the existing aggregate 512 MiB image cap; only retired
LRU textures may be released, including when targets/resolves need space.
Current-batch references are protected, oversized allocations rejected without
purging, failed placeholders removed, and metadata bounded independently at
4,096 keys. Per-interval logs report resident texture bytes and aggregate bytes.
Fetch/sampler variants still have separate backing; sharing is a next step if
the actual working set exceeds this budget. No Switch memory budget is inferred.

Ten residency fixtures cover an above-256 working set, allocation bounds and
recorded/retired resource eligibility. Ten new display fixtures cover identity,
red/blue exchange, constant channels and mismatched/reserved fetch state.
All 134 CPU cases and five shader modules pass, including the 16-byte display
push ABI. These do not prove GPU pixels or FPS improvements.

PC-first native PM4/Vulkan remains the chosen direction, consistent with the
NFSMW-NX and Mass Effect reference designs. This backend is still a serialized
correctness prototype with unsupported scene paths, per-draw snapshots and raw
fetch-key textures. Native ownership alone is not a performance guarantee.
Persistent shared resources, upload deduplication, changed-constant/descriptor
reuse, offline shader/pipeline reuse and safe frame-slot overlap are required
before assessing an original/OLED Switch port's performance on hardware.

Evidence: `analysis/pc_native_backend_boot32_resolved_frontbuffer.log`,
`analysis/pc_native_backend_build57.log`, `analysis/native_texture_upload_tests12.log`,
`analysis/native_display_residency_validation01.json`, and
`analysis/pc_native_provenance37_prepared.json`. Run 33 closed normally.

Early live run 33 confirms the count-cap repair: 334, 356 and then 562 textures
remain resident with zero evictions, using only 278–294 MiB aggregate images.
Those intervals still fall from 1.70 to 1.36 swaps/s. Texture uploads stay about
121–125 MiB per 30 swaps, substantially below late run 32 churn, but queue/fence
scopes still take 8.7–10.7 seconds with 883–984 submissions. This is an unmatched
live test, not a controlled speedup measurement. It proves count-based churn
is removed in the sample and that other substantial limits remain.

Next profiling work must separate vertex/constants uploads, draw recording and
actual GPU execution. Current draw reservation adds every CPU texture's worst-
case decoded size even if its cached content is unchanged, potentially forcing
premature arena flushes. Removing that over-reservation needs a safe preparation
and lifetime scheme so concurrent guest writes cannot overflow the upload arena
or evict descriptors prepared for a draw. Do not simply omit the reservation.
Snapshot: `analysis/pc_native_backend_boot33_display_residency_snapshot.log`;
binary/source hashes: `analysis/pc_native_provenance38_test.json`.

## Build 59: requested unlit visibility diagnostic

No `r_fullbright` string was found in the dumped patched BO3 Zombies image.
Rather than assuming an undocumented guest dvar works, F10 enables an unlit
geometry view in the native backend's existing diagnostic mode. The real VS,
its resources, and depth/stencil/culling remain; eligible MSAA scene pixel
shaders are replaced by bright per-shader solid colors, with PS material
textures, lighting, alpha kill and blend omitted. Unsupported PS textures can
therefore no longer reject those diagnostic draws; unsupported VS data still
can. Single-sample frontend/video, rectangles and depth-only draws retain their
paths. It does not display material textures or establish fullbright parity.

Separate RGBA8 diagnostic targets clear once per swap, share actual depth, and
are never selected for guest resolves. Direct presentation bypasses glow and
final composition, with no HUD. F10 returns to normal on the next fresh frames;
F9 remains a depth comparison. This is a diagnostic isolation experiment, not
a normal renderer fix or optimized performance benchmark.

142 CPU checks and all six shader modules pass; compiled unlit reflection
confirms no material image descriptors and the existing 48-byte draw push ABI.
Evidence: `analysis/pc_native_backend_build58.log` (initial diagnostic),
`analysis/pc_native_backend_build59.log` (independent target/gating final build),
`analysis/native_texture_upload_tests13.log`, `analysis/native_unlit_validation01.json`,
and `analysis/pc_native_provenance39_prepared.json`. Run 34 is open with F10 off
initially, awaiting user comparison.

Evidence: `analysis/pc_native_backend_boot30_texture_watch.log`,
`analysis/pc_native_backend_build55.log`, `analysis/native_texture_upload_tests10.log`,
`analysis/native_batching_validation04.json`, and
`analysis/pc_native_provenance33_prepared.json`.
