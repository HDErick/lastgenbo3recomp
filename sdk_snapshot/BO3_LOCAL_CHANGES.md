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

Game-specific discovery boundaries and native CRT jump handling are configured
in `app/bo3_zm_manifest.toml`. Package headers and optional frame/image capture are
implemented in the application. The launcher's local-profile setting uses the
SDK's existing `xam_signin_state` option.
