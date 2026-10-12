// Unhandled-exception reporter and on-demand stack sampler.
//
// Recompiled code is ~30,000 generated functions named sub_<guest address>, so a
// symbolized host stack is the fastest way to turn a crash -- or a hang -- into a
// guest address worth investigating. The SDK's VEH returns
// EXCEPTION_CONTINUE_SEARCH for anything it doesn't handle itself, so an
// unhandled-exception filter still gets the fatal ones.

#pragma once

namespace bo3 {

// Installs the unhandled-exception filter and initializes the symbol handler.
// Idempotent.
void InstallCrashReporter();

// Walks every other thread in the process and writes a symbolized stack per
// thread to stacks_<reason>.txt next to the exe. Safe to call while the guest
// runs: threads are suspended only long enough to capture return addresses, and
// symbolization happens after everything is resumed.
void DumpAllThreadStacks(const char* reason);

// Starts a watchdog that calls DumpAllThreadStacks at each offset listed in the
// BO3_SAMPLE_SECS environment variable (comma-separated seconds, e.g.
// "90,180,300"). Does nothing when the variable is unset. This is the tool for
// "it stopped making progress and nothing is in the log".
void StartStackSampler();

}  // namespace bo3

