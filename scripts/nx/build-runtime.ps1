param([int]$Jobs = 4, [switch]$ConfigureOnly, [switch]$SkipPrepare)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$python = 'C:/Users/Erick/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'
if (-not $SkipPrepare) {
    & $python "$PSScriptRoot/prepare-runtime.py"
    if ($LASTEXITCODE -ne 0) { throw 'NX runtime source preparation failed' }
}
$cmakeRoot = 'C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake'
$cmake = "$cmakeRoot/CMake/bin/cmake.exe"
$ninja = "$cmakeRoot/Ninja/ninja.exe"
$env:DEVKITPRO = 'C:/devkitPro'
$env:DEVKITA64 = 'C:/devkitPro/devkitA64'
& $cmake -S "$projectRoot/nx/runtime" -B "$projectRoot/build/nx-runtime" -G Ninja `
    "-DCMAKE_TOOLCHAIN_FILE=$projectRoot/nx/cmake/devkitA64.cmake" `
    "-DCMAKE_MAKE_PROGRAM=$ninja" "-DPYTHON_EXECUTABLE=$python" "-DPython3_EXECUTABLE=$python" -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { throw 'NX runtime configure failed' }
if ($ConfigureOnly) { return }
& $cmake --build "$projectRoot/build/nx-runtime" --target bo3_zm_nx --parallel $Jobs
if ($LASTEXITCODE -ne 0) { throw 'NX runtime build failed' }
$start = & "$env:DEVKITA64/bin/aarch64-none-elf-nm.exe" "$projectRoot/build/nx-runtime/bo3_zm_nx" |
    Select-String '^[0-9a-fA-F]+ [Tt] _start$'
if (-not $start -or $start.Line -notmatch '^0000000000000000 ') { throw 'NX startup entry is not zero' }
Write-Output 'Built bo3_zm_nx.nro; verified startup entry at zero'
