# GPU readback lifetime repair

October 12, 2026. Repair built and exercised in gameplay; long-duration coverage
remains limited.

## Evidence

The normal launcher crashed at 17:16:39 with `DEVICE_HUNG`, about 88 seconds after
starting. Its log and abort report are preserved under
`analysis/crash-investigation/2026-10-12-171639`.
An earlier DRED run reported a GPU page fault with a buffer copy as its next
uncompleted operation. Breadcrumbs narrow the search but do not conclusively
identify the failing operation; see Microsoft's
[DRED documentation](https://learn.microsoft.com/en-us/windows/win32/direct3d12/use-dred).

## Identified source defect

`D3D12CommandProcessor::EvictOldReadbackBuffers` enforced its 256-entry cache limit
by immediately unmapping and releasing entries. It did not check the submissions
that still referenced their readback buffers. Command lists do not provide the
application's required lifetime management for those resources. A queued buffer
copy could therefore target an allocation released by cache eviction.

## Repair

- Skip eviction while either slot's last-write submission exceeds the GPU's
  completed submission. The cache limit is temporarily soft until writes finish.
- Track resolve readback writes too, and retire replaced resolve buffers through
  the existing fence-managed deletion queue.
- Correct the queue-fence HRESULT check: the old parentheses passed a Boolean
  expression to `SUCCEEDED` and could skip `SetEventOnCompletion` on a successful
  `Signal` call.
- Name shared memory, upload pages and readback resources; report existing and
  recently freed DRED allocations that match a page fault. Include breadcrumb
  nodes with zero completed operations and label buffer-copy operations.

## Validation so far

The Release build succeeded (`analysis/build_readback_lifetime.log`), and
`git diff --check` passed. Test `gpu-readback-lifetime-01.log` uses the normal
automatic renderer settings, existing shader cache, async compilation and DRED.
It omits frame capture, FPS sampling and thread-stack sampling.

The first run exercised the new guard before map testing: at 20:34:46.749,
last write submission **1734** exceeded completed submission **1733**, with a
cache size of **300**. Additional deferrals occurred with submissions 3101/3100
and cache sizes 264–266. These are concrete cases where the old eviction logic
would have freed resources still referenced by GPU work.

## Gameplay results

| Run | Settings | Observed result |
| --- | --- | --- |
| `gpu-readback-lifetime-02.log` | Normal automatic renderer, async enabled, DRED enabled | Ran 21:31:30–21:40:11 (8 min 41 s). Loaded The Giant and reloaded it later. User reported it stayed open. Normal window closure; no GPU-loss error. |
| `gpu-readback-lifetime-03-control.log` | Same renderer, async enabled, DRED disabled | Ran 21:41:19–21:42:45 (86 s). Loaded The Giant around 21:42:13. User reported playable gameplay. Normal window closure; no GPU-loss error. |

The control run was shorter than the requested five minutes and includes only
about half a minute after map asset loading. It is a short gameplay check, not
an extended stability result. The longer run includes loading and a map reload;
its duration is process lifetime, not measured continuous gameplay time.

Both runs exercised the repaired eviction path. The DRED run retained pending
writes with cache sizes up to 432; the control retained writes at sizes up to
374. That confirms the fix operates without breadcrumb instrumentation too.

The confirmed premature-release defect is repaired, and these runs passed.
This does not establish that all GPU crashes are eliminated or that all maps
remain stable in long sessions. Extended control testing on Shadows of Evil
and Der Eisendrache remains unverified. Existing performance and rendering
limitations are separate from this repair. No launcher renderer default, GPU
driver or TDR registry setting was changed.

Preserved evidence is under `analysis/crash-investigation/readback-lifetime-02`
and `readback-lifetime-03-control`. The rebuilt binary hashes are recorded in
`analysis/build_provenance.json`. Normal launches apply this source repair and
leave DRED, frame capture and thread sampling disabled.
