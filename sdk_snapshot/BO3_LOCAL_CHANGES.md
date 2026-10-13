# BO3 SDK changes after snapshot

The baseline manifest records the source copied from the BO2 SDK, including its
existing uncommitted modifications and initialized submodules. These additional
changes are confined to the copy in `thirdparty/rexglue-sdk`:

- `thirdparty/CMakeLists.txt`: permit the recorded source snapshot without `.git`
  pointers into the original checkout. CMake still checks the dependency source
  files while configuring and compiling.
- `src/system/function_dispatcher.cpp`: log the target, LR, SP, r3 and r4 before
  aborting an unregistered indirect call. Dispatch behavior is unchanged.
- `src/kernel/xboxkrnl/xboxkrnl_io.cpp`: log successful `.ff`, `.xpak` and `.id`
  opens at debug level so the boot trace distinguishes normal failed probes
  from available content.
- `src/graphics/command_processor.cpp`: add optional `gpu_frame_log` CSV timing
  for guest swap intervals and CPU time inside `IssueSwap`, including Release
  builds. It is disabled by default. The CSV uses host performance-counter ticks
  to align frames with the read-only external CPU sampler. This is not hardware
  GPU timing or a count of independently displayed frames.
- `src/graphics/d3d12/command_processor.cpp`: fence-protect readback cache
  eviction, track resolve readback writes, defer release of resized resolve
  buffers, and correct the queue-operation HRESULT/event check. Add readback
  resource names and more complete DRED allocation/breadcrumb logging.
- `src/graphics/d3d12/shared_memory.cpp` and
  `src/ui/d3d12/d3d12_upload_buffer_pool.cpp`: name GPU allocations for DRED.
  See `analysis/GPU_CRASH_FIX.md` for evidence and validation status.
- `src/graphics/d3d12/command_processor.cpp` and its header: remove vertex-buffer
  address/size and fetch-bit shortcuts that bypassed shared-memory validity
  checks. Batch used vertex ranges through `RequestRanges` on every draw so
  unchanged addresses can still receive updated CPU data. The build succeeded;
  the user reported unchanged color flashing in the comparison. The source
  coherency defect is repaired but has not explained that symptom.
  See `analysis/RENDERING_ISSUES.md`.
- `src/graphics/shared_memory.cpp` and its header: snapshot the full GPU-written
  page-validity table under the shared critical region at frame end. Remove
  dirty-block/early-return shortcuts that could retain stale validity or race
  with CPU invalidation. The build succeeded, but the user reported unchanged
  model color flashing. Performance and extended stability remain unverified;
  this investigation is paused. See `analysis/RENDERING_ISSUES.md`.

Game-specific discovery boundaries and native CRT jump handling are configured
in `app/bo3_zm_manifest.toml`. Package headers and optional frame/image capture are
implemented in the application. The launcher's local-profile setting uses the
SDK's existing `xam_signin_state` option.
