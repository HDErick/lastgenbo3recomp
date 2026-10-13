# BO3 offline native shader tools

October 13, 2026; `native-rendering-pc`.

This is preparation for a native Vulkan renderer. The game test executable
still uses Xenos emulation. Generated containers, HLSL and SPIR-V are local
game-derived data under ignored `runtime/` and `build/`; they are not uploaded.

## Sources and build

The container translator is pinned to
[SA2 commit d2c6e15](https://github.com/ChanseyIsTheBest/sa2-nx/tree/d2c6e1558de06cf8ac7e6e9c78029fb5a2623c64/shaders).
XenosRecomp is MIT; its frontend is covered by the reference repository's
GPL-3.0 license. Preparation retains the licenses and creates a separate BO3
copy, leaving the reference unchanged. Source hashes are in
`analysis/native_shader_tool_sources.json` and
`analysis/native_shader_tool_staged_sources.json`.

The BO3 patch fixes the emitted GetTextureWeights argument list. The inherited
emitter supplied five arguments to a four-argument getWeights2D helper. Only
ordinary tfetch2D receives the reciprocal-size fifth argument; the texture-weight
helper retains its original dimensions query and arithmetic.

Portable DXC is Microsoft's official
[v1.9.2609 release](https://github.com/microsoft/DirectXShaderCompiler/releases/tag/v1.9.2609).
The archive is verified against the SHA256 published in the GitHub asset API;
the local executable also passed Microsoft signature verification. The Windows
SDK's existing DXC advertised `-spirv` in help but rejected it with
"SPIR-V CodeGen not available". That failed control is not a shader defect.
The portable compiler and licenses stay under ignored tools. Provenance:
`analysis/native_dxc_provenance.json`.

SPIRV-Tools' validator is built from the already staged SDK dependencies.
Dependency tree hashes, validator hash and build evidence are recorded in
`analysis/native_shader_dependencies.json`. Builds use local LLVM 20.1.8 and
Visual Studio BuildTools; preparing the staged SDK is a prerequisite.

```powershell
$python = 'C:\Users\Erick\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe'
& $python scripts/nx/prepare-shader-tools.py
& $python scripts/nx/prepare-dxc.py
& C:\Windows\System32\cmd.exe /d /c scripts\nx\build-shader-tools.cmd
& C:\Windows\System32\cmd.exe /d /c scripts\nx\build-spirv-validator.cmd
```

## Capture and translate

Use a new named capture directory so container and GPU dumps describe the
same run. Initialization hooks capture header+physical data before the original
initializer executes; draw-time hooks capture first-seen bound objects. All
original guest bodies still execute. GPU dumps come from the SDK command path.

```powershell
.\scripts\nx\launch-pc-vulkan.ps1 -RenderTargetPath fsi -D3DTrace -DumpShaders -DiscoveryRun native-discovery03 -Background -LogName pc-vulkan-native-discovery03.log
```

After normal game close, use new or empty output directories:

```powershell
& $python scripts/nx/translate-native-shaders.py --input runtime/native-discovery03/containers --output build/native-shaders/discovery04 --jobs 8
& $python scripts/nx/compile-native-shaders.py --input build/native-shaders/discovery04 --output build/native-shaders/spirv05 --jobs 8
& $python scripts/nx/compare-native-shader-dumps.py --containers runtime/native-discovery03/containers --gpu-dumps runtime/shader-dumps/native-discovery03 --output analysis/bo3_shader_microcode_comparison03.json --same-run
```

Translation validates the outer header, stage, sizes, declarations and program
range. Every container runs in a separate process with a 60-second limit; a
failure stays in the manifest. DXC uses shader model 6.6, HLSL 2021, Vulkan 1.2,
DirectX constant layout and vertex Y inversion. Every compiled module must pass
`spirv-val --target-env vulkan1.2 --scalar-block-layout`. Missing/failed shaders
are reported; exit code 2 indicates incomplete coverage. The manifests record
tool, input and output hashes. No incomplete package is silently called complete.

## Results and remaining work

Discovery02's 365 containers passed structural checks. 352 translated, and 350
compiled/validated after the argument repair. Twelve VS containers failed
FETCH/declaration matching, one PS timed out, and two startup VS shaders failed
partial explicit Vulkan input-location assignment. These failures remain open.

Discovery03 passed user-confirmed The Giant gameplay and closed normally. It
captures 2,897 unique containers: 297 VS and 2,600 PS. Of these, 2,845 translate;
49 VS fail FETCH/declaration matching and three PS hit the translation limit.
Of the 2,845 HLSL shaders, 2,843 compile and pass validation; the same two startup
VS input-location errors remain. That leaves 54 unresolved containers overall.
Evidence: `analysis/bo3_shader_translation04.json` and
`analysis/bo3_shader_spirv05.json`.

The same run has 455 GPU-loaded programs. Exactly 305 pixel programs match
captured container microcode; none of the loaded VS matches exactly. Close
same-length VS candidates are diagnostic evidence of possible patching, not a
safe lookup key. Other subprograms, vertex linking/input remaps and command-buffer
replays need verified associations before these modules can render the game.

Structural SPIR-V validation does not prove shader semantics, renderer descriptor
layouts, constant-bank contents or correct pixels. The native renderer and
package/loader remain to be implemented. The emulated Vulkan bloom regression
and older model-color flashing remain documented separately.
