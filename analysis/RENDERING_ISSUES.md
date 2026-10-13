# Rendering issues under investigation

Updated October 12, 2026 (America/New_York).

**Status: investigation paused at the user's request.** The color flashing is
unresolved. Resume only when the user brings this issue back into scope.

## The Giant: models flashing different colors

The user reports that apparently every 3D object on The Giant except terrain
flashes different colors: zombies, barriers, teleporter pads and buyable doors.
This broadens the investigation to data and shading shared by models, including
model lighting, vertex attributes and shader constants. It does not establish
which rendering path those objects use. The cause is unconfirmed.

### Latest user observation

The flashing becomes more frequent or more noticeable when zombies begin
spawning. The user suspects that increasing the number of models makes it worse.
This is an observed correlation, not a measured object-count threshold or a
confirmed cause. Keep this clue for later investigation of model lighting,
per-draw constants, resource reuse and rendering workload. Do not assume it is
shader compilation based on this observation.

### Comparison summary

| Change from the preceding comparison | User result |
| --- | --- |
| Disable fast memexport readback | Unchanged |
| Select RTV instead of automatic ROV | Improved, still flashing |
| Disable async shader compilation | Unchanged |
| Repair vertex-buffer validity checks | Unchanged |
| Disable bindless resource binding | Improved, still flashing |
| Repair frame-end page-validity snapshot | Unchanged |

These are qualitative user comparisons, not controlled measurements of artifact
frequency. Cache warming, view and scene population may differ between runs.

### Source findings

- Async pipeline compilation skips a draw while its pipeline is unavailable
  (`D3D12CommandProcessor::IssueDraw`). This explains temporarily missing draws;
  it does not establish the cause of persistent color cycling.
- The default `readback_memexport_fast=true` path copies a previous completed
  readback slot into guest CPU memory while scheduling the current GPU write.
  Fence completion ensures the older result is readable, but does not make it
  the current draw's result. Stale GPU-generated data is a plausible candidate.
- The fast-path cache key includes the first range address and total byte count,
  without storing and comparing the complete list of export ranges. Whether
  this game reuses that key for different range layouts is unverified.
- Shader translation, material/texture state and render-target behavior remain
  other candidates. The existing code-generation packing warnings have not
  been linked to this object or proved incorrect.
- The recent readback lifetime repair addresses resource release while GPU work
  is pending. It does not establish rendering correctness for the fast path.

### Comparisons

Compare the same objects with `--readback_memexport_fast=false`, preserving the
renderer and cache. A screenshot or clip is still useful for identifying whether
the effect is in lighting, textures or geometry.
This keeps readback enabled but waits for current GPU results. It may reduce
performance. A visual change would implicate this path, not prove an exact root
cause; no change would direct investigation toward shader/material/texture state.

After the user closed the baseline game, launched a comparison using the existing
executable with `--readback_memexport_fast=false`. Automatic render-target
selection, async compilation and the existing shader cache remain in use.
Log: `runtime/logs/model-colors-sync-readback-01.log`. The user reported the
flashing was **unchanged**. The process ran 22:07:18–22:09:25 and closed normally.
This reduces the likelihood that delayed fast readback causes this artifact; it
does not rule out other shared-memory or GPU-data correctness problems.
The flag applies only to this launch; the normal launcher default is unchanged.
No executable was rebuilt. FPS benchmarking remains paused.

The next comparison uses explicit RTV with synchronous memexport readback still
enabled, changing only render-target selection relative to the previous test.
Log: `runtime/logs/model-colors-rtv-sync-02.log`. The user reported **improved but
still flashing**. The process closed normally at 22:11:48. RTV changes shader
output and render-target handling, so this observation implicates that family of
paths without identifying a particular defect. The first RTV run also warms its
own pipeline cache; this was a qualitative comparison, not a frame-matched one.
This distinguishes behavior of the automatic Intel ROV path from RTV before
changing shader translation or guest model/lighting calculations.

A third run retained RTV and synchronous memexport readback, and disabled async
compilation. Log: `runtime/logs/model-colors-rtv-sync-noasync-03.log`. The user
reported **unchanged**. It ran 22:12:10–22:13:24 and closed normally. Pending async
draws therefore are unlikely to explain the persistent artifact in that view.

### Vertex-buffer coherency repair under test

Source inspection found an address/size cache in D3D12 `IssueDraw` that bypassed
`SharedMemory::RequestRange`. Its per-frame fetch bits were reset at swap, but
the cached addresses/sizes persisted. A matching address and size bypassed the
page-validity check on later frames too. Neither CPU physical-memory invalidation
nor frame-end shared-memory page invalidation invalidated these address entries.
Even within a frame, the fetch-bit shortcut could skip checking changed data.

This is a coherency defect: buffer location and size can stay unchanged while its
contents change. It is a plausible cause for incorrect model attributes, but has
not yet been tied to the flashing by a gameplay result.

The repair removes both vertex-fetch shortcuts and checks all used vertex ranges
through `SharedMemory::RequestRanges` on every draw. The ranges are batched in one
call; valid pages avoid redundant uploads, and GPU-written pages retain existing
shared-memory ownership behavior. The code uses the existing validity mechanism
rather than forcing unconditional CPU copies over GPU data.

The baseline GPU plugin and edited source files are preserved locally under
`.local/model-color-baseline`. Build log: `analysis/build_vertex_coherency.log`.
The Release build succeeded. Test `model-colors-vertex-coherency-04.log` ran,
retaining RTV, synchronous memexport readback and synchronous shader compilation
from the preceding comparison. The user reported **unchanged**, with no new
geometry problem mentioned. It closed normally at 22:19:02. This source defect
was repaired, but that repair did not resolve the reported flashing. Rebuilt binary hashes
are recorded in `analysis/build_provenance.json`. Renderer defaults have not been
changed as a result of these comparisons.

### Visual capture

The optional application capture now includes consecutive one-second gameplay
frames at 90–95 and 120–125 seconds after module launch, in addition to its earlier
10/30/45-second frames. The renderer is otherwise unchanged for capture run
`model-colors-capture-05.log`. The user was asked to face an affected object and
keep the view stationary. The user requested a restart, which produced
`model-colors-capture-06.log`. Both bursts completed in the restarted run, and
the game closed normally at 22:26:28. These captures are stored under the ignored
runtime cache and are not included in the source repository.

Inspection of the first run's 90–95-second frames shows the teleporter panel
changing from gray to gold, nearly black and red while the surrounding world
remains similar. The weapon and hands also change brightness. Geometry and
surface texture details remain recognizable; large model tint/brightness changes
are the prominent artifact. Model shading, lighting constants and resource
bindings are therefore the next targets, without an exact cause yet identified.
The restarted run's later burst includes moving zombies, attacks and camera
movement, so it is not a completely stationary pixel comparison.

A generated-code scan found 90 `vpkd3d128` instructions and no instances where
source and destination vector registers are identical. Destructive aliasing in
that packing operation is therefore not a candidate for these generated sites.

### Resource-binding comparison

Run `model-colors-bindful-07.log` disables `d3d12_bindless`, retaining RTV,
synchronous memexport readback, synchronous shader compilation and the capture
schedule. This changes shader texture/buffer descriptor binding, a candidate for
incorrect model shading inputs. The user reported **improved but still flashing**.
The run closed normally at 22:30:06. These diagnostic flags remain launch-specific;
no renderer flag default has been changed.

### Frame-end page-validity coherency repair

Further source inspection found that the page-validity snapshot recycled an old
active table but updated only dirty blocks, leaving other blocks with older
validity bits. It also read GPU-written ownership and swapped tables without the
lock used by CPU invalidation. The refresh could skip entirely when GPU ownership
was unchanged, despite CPU-uploaded validity bits needing a frame-end reset.

The repair snapshots the complete GPU-written ownership table under the shared
critical region before swapping tables. It preserves GPU-owned data and resets
CPU-uploaded validity through the existing page-state mechanism. Dirty-block and
early-return shortcuts were removed. This changes metadata, not an unconditional
GPU-to-CPU or CPU-to-GPU copy of all game memory.

The Release build succeeded (`analysis/build_page_coherency.log`). Test
`model-colors-page-coherency-08.log` retained RTV, alternate resource binding,
synchronous readback, synchronous shader compilation and optional frame capture.
The user reported **unchanged** and noted the stronger artifact when zombies
spawn. The run closed normally at 22:37:37.

The current local build includes both coherency repairs. Neither has resolved
the reported flashing; their performance and extended stability impact have not
been separately established. The original source and pre-investigation GPU
plugin are retained under `.local/model-color-baseline` for future comparison.
No additional rendering experiments are scheduled while the issue is paused.
