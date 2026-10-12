# BO3 Xbox 360 RexGlue feasibility assessment

Initial assessment date: October 11, 2026. BO3 files were not present during the initial inspection below.

**Native implementation update:** An isolated Zombies host is built in this
workspace. The game reads the supplied update and compatibility packs, renders
the map-selection menu and reaches Shadows of Evil gameplay, confirmed by the
user. A later D3D12 `DEVICE_HUNG` crash makes the prototype unstable. See [native boot results](analysis/BOOT_RESULTS.md)
and [launch/build instructions](README.md).

**October 12 update:** The supplied update matches both base executables, all 1,409 extracted package files match their original container contents by SHA-256, and both updated executables now pass RexGlue code generation with two analysis hints each. See [the input and code generation results](analysis/RESULTS.md) for the current assessment, remaining runtime work, and reproducible probes. The original assessment below records the earlier investigation.

**Verdict: technically plausible and worth a controlled prototype. The existing BO2 RexGlue project is a useful runtime and tooling reference. Playable BO3 remains unproven until its executable, matching title update, and compatibility packs are examined together.**

The update requirement is compatible with static recompilation. The key requirement is to generate C++ from the effective updated executable and load that same effective image at runtime. Compatibility assets and platform services need their own validation.

## What was inspected

The active reference is [rexglue-bo2](<C:/BO2recomp/Project BO2/rexglue-bo2>), particularly its CMake files, manifest, app, hooks, generated SDK integration, renderer, and existing build cache. The SDK reference is [rexglue-sdk](<C:/BO2recomp/Project BO2/rexglue-sdk>), particularly its code generation loader, XEX patch loader, content manager, module loader, and local Git changes.

The parent README describes an earlier XenonRecomp project and is unsuitable as the build guide for this RexGlue application. The parent directory contains unrelated work and backup projects.

An existing `bo2mp.exe` is present under `rexglue-bo2/out/build/win-amd64-release`. Its gameplay status is based on the user's description; this assessment did not launch it or rebuild it. No BO2 source or assets were intentionally changed.

## Reuse assessment

| Component | Value for BO3 | Required adaptation |
| --- | --- | --- |
| RexGlue CMake integration and app lifecycle | Strong starting point | Create a BO3 target, manifest, paths, and generated code |
| Crash reporter and stack sampler | Mostly reusable tooling | Rename application settings and verify symbols for the BO3 build |
| Modified SDK XAM/network/session support | Useful platform reference | Audit assumptions and select changes supported by BO3 behavior |
| Modified SDK XMA audio support | Candidate reusable platform fix | Verify BO3's register usage and thread behavior |
| BO2 engine, session, streaming, sound, and input hooks | Useful reverse engineering examples | Identify BO3 functions, layouts, and calling conventions afresh |
| BO2 D3D9 renderer | Potential later reference | Reidentify graphics entry points and state layouts; generate BO3 shader coverage |
| BO2 generated C++ and address tables | BO2-specific | Regenerate from BO3 |
| BO2 shader caches and modified fastfiles | BO2-specific | Use BO3 assets and shader data |

The BO2 manifest targets `rfazio/t6/default_mp.xex` and explicitly identifies `setjmp`/`longjmp`, mid-function hooks, and function boundaries. Its comments repeatedly use `default_mp.map` for symbols. The hook file has 1,844 lines; much of it depends on exact BO2 addresses. The presence of BO2 development symbols is a substantial advantage that a retail BO3 dump may not provide. Lack of symbols increases analysis effort but does not alone rule out recompilation.

The BO2 renderer is optional. Its app retains the Xenos GPU plugin unless a separate detachment setting is enabled. Renderer comments describe the detached path as unfinished, with GPU synchronization dependencies. Start a BO3 prototype with the SDK's Xenos backend; reassess native graphics interception after establishing a functional baseline. RexGlue recompiles the guest CPU code while supplying Xbox platform behavior through its runtime and backends; generating C++ does not automatically resolve graphics or kernel compatibility. [Upstream description](https://github.com/rexglue/rexglue-sdk).

## SDK state and reproducibility

The local SDK is based on commit `cb58065c793429aa92895d778af58d12e9d26d8f`, dated July 30, 2026, with commit subject `Release v0.9.0`.

It is modified. Git reports changes in 12 tracked SDK files, totaling 808 insertions and 53 deletions at inspection, plus two untracked session implementation files:

```text
include/rex/audio/xma/decoder.h
include/rex/audio/xma/register_table.inc
src/audio/xma_decoder.cpp
src/kernel/CMakeLists.txt
src/kernel/xam/apps/xgi_app.cpp
src/kernel/xam/apps/xlivebase_app.cpp
src/kernel/xam/xam_enum.cpp
src/kernel/xam/xam_misc.cpp
src/kernel/xam/xam_net.cpp
src/kernel/xam/xam_user.cpp
src/system/CMakeLists.txt
src/system/xsocket.cpp

Untracked:
include/rex/system/xsession.h
src/system/xsession.cpp
```

Examples inspected include XMA context locking and synthetic local service/title-server enumeration. Those service responses do not establish a working BO3 backend. Several dependency submodules are also reported as modified; the meaning of those changes was not audited in this assessment.

A stock SDK checkout is therefore not an exact reproduction of the reference. Before development, preserve the selected SDK changes and session files in a separate BO3 dependency snapshot or patch set and record dependency revisions. Avoid coupling BO3 experiments to changes in the working BO2 SDK.

The copied BO2 build cache still references `C:/Project BO2/rexglue-bo2` and `C:/Project BO2/rexglue-sdk`, rather than the current source locations. BO3 needs a fresh configure and build directory. BO2's CMake also reaches outside `rexglue-bo2` for `assets/prebuilt/bo2_sm3_shader_cache.cpp` and the zstd sources under `tools/XenosRecomp`. Copying only the two named folders would not reproduce its full custom renderer build.

## Title updates: a concrete supported route

The inspected local SDK automatically looks for a sibling patch named by appending `p` to the XEX filename. For example:

```text
default.xex       + default.xexp
default_mp.xex    + default_mp.xexp
```

[UserModule::LoadFromFile](<C:/BO2recomp/Project BO2/rexglue-sdk/src/system/user_module.cpp:53>) loads the sibling and invokes [XexModule::ApplyPatch](<C:/BO2recomp/Project BO2/rexglue-sdk/src/system/xex_module.cpp:189>) before completing the executable load. [Project code generation](<C:/BO2recomp/Project BO2/rexglue-sdk/src/codegen/project_recompiler.cpp:193>) loads the image through the same runtime in tool mode. This supports analyzing the patched image and running against that same image.

The online [configuration wiki](https://github.com/rexglue/rexglue-sdk/wiki/rexglue-CLI-Configuration-File) documents explicit patch path options. The local [legacy migration code](<C:/BO2recomp/Project BO2/rexglue-sdk/src/rexglue/commands/legacy_config.cpp:180>) says those options were removed in favor of sibling detection. Follow the inspected SDK source when preparing this project's configuration.

After receiving the files:

1. Identify the base XEX's title ID, media ID, version, language/region, and hash, and inventory every executable/module.
2. Extract the provided update and verify it matches that base. Identify executable patches and replacement assets separately.
3. Prepare a derived game tree with the matching sibling patches and the required update files in their expected locations.
4. Run code generation against the updated image. Record hashes of inputs and the effective executable's code/data image after patching.
5. Load that same prepared tree at runtime. Check the effective image identity before debugging game behavior.

Generating from version 1.0 and subsequently mounting an update can leave native functions inconsistent with loaded guest code/data. Even unchanged addresses can contain changed instructions. A future update that changes executable code requires regeneration and may require retargeting hooks.

No specific TU number is selected yet. The user's requirement for the latest update is the initial target; package metadata and behavior must establish the exact compatible version.

## Compatibility packs and the menu gate

Treat the reported version 1.0 menu restriction as the starting observation. A [historical Xenia report](https://github.com/xenia-project/issue-graveyard/issues/436) also describes an update gate. The [main compatibility issue](https://github.com/xenia-project/game-compatibility/issues/258) contains an older report of getting past the update page and subsequently encountering missing/damaged content. These reports identify useful failure categories; they do not demonstrate current RexGlue gameplay compatibility.

The local [content manager](<C:/BO2recomp/Project BO2/rexglue-sdk/src/system/xam/content_manager.cpp:139>) enumerates package directories below a content root organized by profile/XUID, title ID, and content type. It can read accompanying header metadata. [ContentPackage](<C:/BO2recomp/Project BO2/rexglue-sdk/src/system/xam/content_manager.cpp:43>) mounts an unpacked package through a host-path device. STFS container support exists elsewhere in the SDK, but that alone does not show that this content enumeration route discovers raw compatibility-pack container files.

Consequently, merely dropping raw packs into a folder, or flattening their contents into the base game, may be insufficient. Inventory the original packages and reproduce the enumeration, mount names, metadata, language selection, and asset precedence BO3 actually uses. Supply update assets and compatibility assets as one coherent installed content set. Disabling a menu check cannot supply missing maps, scripts, fastfiles, or sound banks.

After content is available, a remaining gate could involve profile state, online service results, session creation, or a game-specific version/hotfix check. Trace those separately. The BO2 local backend setup cannot be presumed compatible with BO3's protocol or services. Official online play is a separate, substantially less certain objective.

## First prototype and decision points

| Stage | Evidence required to advance |
| --- | --- |
| Input qualification | Matching base/update/packs, complete file inventory, selected mode and executable |
| Effective executable load | Successful patch application; recorded effective version and image identity |
| Code generation and build | Resolve analysis gaps, relevant indirect calls, CRT jumps, and required modules; produce a native executable |
| Updated menu | Visible stable menus, correct content discovery, actionable logs for any remaining gate |
| One gameplay map | Load a selected Zombies or multiplayer map, enter gameplay, and sustain rendering/input/audio |
| Broader support | Additional maps, reloads, saves, and multiplayer paths tested individually |

The first meaningful playability milestone is one sustained gameplay map after the installed content checks. A compiling executable or menu screenshot alone is insufficient. If BO3 uses separate launchers or switches executables, evaluate each executable and transition; support for guest DLL modules does not by itself prove executable relaunch works.

The Xbox 360 release contains Multiplayer and Zombies and omits campaign, as [Activision documents](https://support.activision.com/content/atvi/support/web/fr/call-of-duty--black-ops-iii/articles/details-for-the-playstation-3-and-xbox-360-versions-of-call-of-duty-black-ops-iii.html). Select the initial gameplay mode from the supplied content and intended goal.

An updated menu prototype is a reasonable research target. Playable local gameplay is a conditional target with significant unknowns in content loading, retail binary analysis, graphics synchronization, and services. A reliable effort estimate requires the first code generation and runtime traces.

## Files to provide for the next assessment

Keep the original inputs separate, preserving their directory layout. A suggested intake layout is:

```text
C:/bo3recomp/game_files/base/             complete extracted Xbox 360 game
C:/bo3recomp/game_files/title_update/     original matching title-update package
C:/bo3recomp/game_files/compatibility/    original compatibility-pack packages
C:/bo3recomp/game_files/optional_dlc/     content needed for additional target maps
```

Include all XEX files and other modules, fastfiles, sound banks, localized data, and any installation data present in the dump. Original packages are useful even if an extracted or merged copy is also supplied, because they preserve identity and provenance. Include any matching BO3 symbols if available; BO2 symbols are reference material only. A known working console/emulator installation and its exact update/content versions would strengthen the baseline comparison.

This assessment created documentation only. BO3 code generation, patch application against BO3, and runtime behavior await those inputs.
