# BO3 input and code generation results

October 12, 2026.

**Later implementation update:** The isolated native Zombies host now builds,
loads the update and four content packages, renders the Zombies map-selection
screen and enters Shadows of Evil gameplay (confirmed by the user). The current
prototype later fails with D3D12 `DEVICE_HUNG`. See
[native boot results](BOOT_RESULTS.md) and the project [README](../README.md).
The analysis below records the earlier code-generation-only stage.

**The provided inputs are suitable for a RexGlue prototype. Both executables load with their matching patches and pass code generation. Native compilation, booting the game, and reaching gameplay have not been tested.**

## Verified inputs

| Input | Result |
| --- | --- |
| Base executable identity | Both XEXs have title ID `4156091D`, media ID `33B8D0AD`, and header version `0.0.0.5` |
| Update container identity | Same title/media IDs as the base |
| Executable patch pairing | Both patches' source version and SHA-1 digest of the source XEX signature match their base executable |
| Updated executable version | SDK loader confirms `0.0.8.5` for both executables |
| Compatibility Pack 1 | All 620 extracted files match their original container contents by SHA-256 |
| Compatibility Pack 2 | All 115 extracted files match their original container contents by SHA-256 |
| The Giant | All 50 extracted files match their original container contents by SHA-256 |
| Awakening | All 195 extracted files match their original container contents by SHA-256 |
| Title update | All 429 extracted files match their original container contents by SHA-256 |

There were no missing, extra, size-mismatched, or hash-mismatched files in those five extracted packages. Their metadata consistently identifies BO3. The audit compares logical file bytes recovered through the STFS directory and block chains with the extracted files. It does not authenticate Microsoft signatures, validate an original disc image, or establish that this is the newest available title update. Keep the XEX header version separate from the title-update filename/number and the game's displayed version.

The supplied intake contains two XEXs and no `.map`, `.pdb`, or separate `.dll` files. Its base executables are region-free according to their XEX region masks. Their exception/function metadata is sufficient for the observed initial function discovery, even without BO2-style development symbols.

The complete intake contains 1,790 files totaling 8,065,903,548 bytes. There are 1,682 distinct relative file paths and 107 paths present in multiple input sets. These overlaps make asset precedence an actual setup requirement.

Raw metadata, file hashes, patch pairing results, and package directory listings are in [input_audit.json](input_audit.json). Content overlap details and code generation summaries are in [analysis_summary.json](analysis_summary.json).

## Why the compatibility packs are necessary

The base contains the executables, core assets, sound data, and disc XPaks. Its root fastfile listing contains `mp_gfx.ff` but lacks the main multiplayer and Zombies map fastfiles.

Compatibility Pack 1 supplies `zm_zod.ff`, `zm_frontend.ff`, the Zombies common fastfiles, the main multiplayer map fastfiles, and additional sound data. Compatibility Pack 2 supplies `zm_patch.ff`, `mp_patch.ff`, and several map patches. The title update adds another patch layer, frontend files, graphics fastfiles, and sound replacements.

The Giant package supplies `zm_factory.ff`; Awakening supplies `zm_castle.ff` and four multiplayer maps. These DLC identities were read from package metadata; internal map names were observed in the extracted files.

The tiny `.id` files are present and exactly match the package originals. The 11-byte `compatpack2.ff` contains `CompatPack2`; its small size is also identical to the original. Those small files are not evidence of a failed extraction.

## RexGlue probes

The probes used the existing BO2 generator and runtime binaries, launched from separate analysis directories. Copies of only the two XEXs and their two sibling patches were placed in [loader_probe](loader_probe). No guest game code was launched. The SDK ran in tool mode without a GPU; this establishes loading and translation, not game boot.

| Probe | Zombies | Multiplayer |
| --- | --- | --- |
| Guest executable | `default_zm.xex` | `default.xex` |
| Patch application | Successful | Successful |
| Patched PE timestamp reported by loader | April 20, 2016, 21:10:23 UTC | April 20, 2016, 21:07:03 UTC |
| Final codegen exit status | 0 | 0 |
| Generated function mappings | 34,805 | 34,960 |
| Generated C++ files | 87 | 87 |
| Generated output bytes | 179,217,277 | 180,146,582 |
| Remaining unresolved-call/branch fatal placeholders | 0 | 0 |
| Float16 packing warnings | 20 | 20 |

Function-mapping counts include generated helper/import entries and should not be read as an exact count of original engine functions. Passing code generation does not prove every indirect call, exception path, or instruction has correct runtime behavior.

### Analysis hints that were needed

The initial scan stopped on one unresolved tail-branch target per executable. Registering that target as a function entry let analysis proceed without `--force`. The first emitted output then contained four conditional-branch fatal placeholders per executable, despite returning exit status 0. Their target labels were visibly present in a merged region. Separating the corresponding leaf routine with explicit bounds removed the placeholders.

| Hint | Zombies | Multiplayer |
| --- | --- | --- |
| Tail-branch function entry | `0x828552B8` | `0x82862C38` |
| Tail-branch call site | `0x8285550C` | `0x82862E8C` |
| Leaf routine with explicit bounds | `[0x824DDF60, 0x824DE008)` | `[0x824DEDD0, 0x824DEE78)` |

The bounded routines end in `blr` at the final instruction in those ranges, as shown in the emitted PPC listing. These are analysis hints in the probe manifests; the original game executable bytes remain intact. The final generated source was checked for the unresolved-call/branch fatal placeholders. Initial and intermediate logs are preserved alongside final logs.

### Remaining vector warnings

All 20 warnings in each image correspond to `vpkd3d128` with type 5 (four half floats), packing selector 3, and shift 0. The inspected RexGlue emitter warns when the selector is not 2, while emitting a 64-bit result in this case.

In the local Xenia reference, selectors 2 and 3 use the same permutation for shift 0. The observed operand combination is therefore valid and appears compatible with the generated lane placement. The generated code has no vector `u16` indices beyond 7. These warnings alone are not a demonstrated translation blocker. Numeric packing accuracy and register alias behavior still need targeted verification if rendering exposes a problem.

Reference locations: [RexGlue vector emitter](<C:/BO2recomp/Project BO2/rexglue-sdk/src/codegen/builders/vector.cpp:1325>) and [Xenia packing implementation](<C:/BO2recomp/Project BO2/tools/xenia/src/xenia/cpu/ppc/ppc_emit_altivec.cc:2054>).

## Runtime work established by the analysis

1. **Content installation and lookup.** The SDK's content manager uses the user data root and enumerates unpacked packages below `0000000000000000/4156091D/00000002/<package-name>/` for common marketplace content. BO3 imports `XamContentCreateEnumerator`, `XamContentCreateEx`, and `XamContentClose`. Preserve package identities and prepare the SDK's content metadata; copying an STFS header verbatim is not the same as writing its `XCONTENT_AGGREGATE_DATA` header format. Confirm actual guest mount names and successful map opens in runtime logs.
2. **Update asset path.** `--update_data_root` mounts a separate `update:` device in the inspected SDK. It does not automatically overlay all files onto `game:`. Also, XEX patch detection looks for a sibling of the loaded executable, so configuring only an update root is insufficient to establish the same patched executable used by codegen. A runtime game tree needs the matching XEX/XEXP pairing and the update asset lookup BO3 expects.
3. **Mode switching.** Both executables are title modules, start at guest image base `0x82000000`, and overlap in memory. They should initially be separate native entrypoints. The SDK supports separately compiled DLL modules but rejects overlapping module ranges. BO3 imports `XamLoaderLaunchTitle`; the inspected handler records a launch path and terminates the current title, while the default app quits after its main guest thread exits. Switching modes needs host relaunch/reinitialization handling or separate launches. The loader probe successfully loads the Zombies image directly, but its standalone gameplay behavior remains untested.
4. **Runtime API coverage.** Zombies imports 276 APIs and multiplayer 279. Comparing those names with the local SDK source finds 51 and 54 explicit stub exports respectively, including HTTP/network services, selected crypto routines, and some UI/audio queries. Import presence does not prove a stub is exercised by the selected mode. Trace calls during boot and map loading; repair the paths actually needed. The existing BO2 service/session changes do not establish BO3 backend compatibility.
5. **CRT and exceptions.** No BO3 `setjmp`/`longjmp` addresses have been identified or registered yet. The BO2 manifest documents why these need correct native handling. Identify any corresponding BO3 routines before trusting error recovery and map loading. Native exception behavior and indirect-call coverage also remain runtime concerns.
6. **Host build.** No BO3 host executable was compiled or linked. The inspected SDK requires Clang 18 or newer. The prebuilt generator works, but the usual LLVM compiler locations checked on this machine contain no `clang-cl.exe`/`clang++.exe`. Establish the required compiler and dependency configuration before treating the probe as a reproducible native build. A complete toolchain inventory or installation was outside this analysis.

The launch command supplied for BO2 uses `--game_data_root`, `--cl "+zm"`, and `--gpu_plugin xenos`. That is the reference for its existing launch configuration. For the initial BO3 Zombies prototype, use a wrapper selecting `game:\default_zm.xex`, the Xenos backend, a prepared game root, and explicit update/user roots. Whether BO3 also needs a mode command line will be established by its own startup behavior.

## Reproduction

The generator reports `ReXGlue v0.9.0.0-dev.unknown`. The SDK source baseline is recorded in the original [feasibility assessment](../FEASIBILITY.md); these prebuilt binary hashes identify exactly what the probes ran:

```text
rexglue.exe:
759F855B63F16A43B30710AC56F662B4A8D6DE9D3077237926C505C07229A18C
rexruntime.dll:
52D779AD39A1C6DEC3E72B17F8940EEDEDFBCCBEF121D1945198791B96357487
```

Rerun the input audit with the bundled Windows Python (the default `python` on PATH is an MSYS installation):

```powershell
& 'C:\Users\Erick\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe' 'C:\bo3recomp\analysis\analyze_inputs.py' --verify
```

The code generation manifests and final logs are [Zombies manifest](zm_probe/bo3_zm_probe_manifest.toml), [Zombies log](zm_probe/console.log), [multiplayer manifest](mp_probe/bo3_mp_probe_manifest.toml), and [multiplayer log](mp_probe/console.log). Example command:

```powershell
& 'C:\BO2recomp\Project BO2\rexglue-sdk\out\win-amd64\rexglue.exe' codegen 'C:\bo3recomp\analysis\zm_probe\bo3_zm_probe_manifest.toml'
```

The summary tool is [summarize_analysis.py](summarize_analysis.py). Generated code is retained under each probe directory for subsequent implementation work.

**Recommended next implementation milestone:** create an isolated BO3 Zombies host target using the verified patched image, arrange the compatibility/update mounts, and capture the first boot/map-load traces with Xenos. The supplied inputs and successful translation make this a concrete project candidate; gameplay and service compatibility are the remaining feasibility gates.
