# Performance measurements

October 12, 2026, America/New_York. FPS testing is paused for manual DLC tests.

## Findings

Async compilation is enabled. Background pipeline and Direct3D driver workers
consume substantial CPU time as new content appears. The user reports that
standing still improves performance and moving immediately reduces it. That
agrees with the measured compilation activity; other performance limits remain.

Release test: RTV, alternate resource binding (`d3d12_bindless=false`), async
enabled, automatic pipeline workers, DRED enabled, populated cache. Startup
translated 662 stored shaders in 20 ms and created 352 pipelines in 511 ms.
New pipeline variants continued to appear in gameplay.

| Ten-second interval starting | Guest swaps/s | 95th percentile interval | New pipeline starts | Pipeline + driver CPU, equivalent cores | GPU 3D activity |
| --- | ---: | ---: | ---: | ---: | ---: |
| 02:23:09 | 22.62 | 76.5 ms | 21 | 3.55 | 55.1% |
| 02:23:19 | 17.71 | 453.3 ms | 37 | 5.37 | 29.1% |
| 02:23:39 | 30.46 | 61.0 ms | 8 | 0.79 | 62.0% |
| 02:23:49 | 33.28 | 55.4 ms | 5 | 0.64 | 74.5% |
| 02:23:59 | 33.01 | 69.4 ms | 2 | 0.39 | 68.0% |

Game backend and server threads each used nearly one core in much of this run.
More work is needed to distinguish useful guest work from waiting/spin loops
and renderer overhead. GPU activity was not consistently saturated. This supports
CPU contention during compilation but does not identify every bottleneck.

The two-worker comparison crashed about 11 seconds after launch, before a valid
gameplay comparison. No performance default was changed.

## Measurement limits

Timing measures intervals between guest swap commands and CPU time inside
`IssueSwap`. It does not independently count displayed frames or measure GPU
execution timestamps. CSV logging is buffered; the timeline includes menus and
loading. DRED, debug logging and alternate binding can affect performance.
Pipeline-start logs do not record driver compilation completion.

## Evidence and tools

- `runtime/logs/perf-warm-bindful.log`: pipeline activity and normal closure.
- `analysis/performance/warm-bindful/frames.csv`: swap timing.
- `analysis/performance/warm-bindful/thread-cpu.csv`: read-only thread sampling.
- `analysis/performance/warm-bindful/gpu.csv`: Windows GPU-engine activity.
- `analysis/performance/warm-bindful/summary.json`: joined ten-second intervals.
- `scripts/measure-performance.ps1`: sampler; does not suspend game threads.
- `scripts/analyze-performance.py`: combines frame, CPU, GPU and pipeline data.
- `analysis/crash-investigation/bindful-two-workers`: latest abort/DRED evidence.

Optional `--gpu_frame_log <CSV path>` works in Release and is normally disabled.
Successful builds are recorded in `analysis/build_performance.log` and
`analysis/build_performance_shared.log`.
