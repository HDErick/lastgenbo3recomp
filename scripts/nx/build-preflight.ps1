param(
    [string]$DevkitPro = 'C:/devkitPro',
    [string]$MesaSdk = '',
    [int]$Jobs = 4
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (-not $MesaSdk) { $MesaSdk = "$DevkitPro/portlibs/switch" }
$cmake = 'C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
$ninja = 'C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'
$output = Join-Path $projectRoot 'build/nx-preflight'
$env:DEVKITPRO = $DevkitPro
$env:DEVKITA64 = "$DevkitPro/devkitA64"
# Use native Windows CMake and the same Ninja for configure and build.
& $cmake -S "$projectRoot/nx/preflight" -B $output -G Ninja `
    "-DCMAKE_TOOLCHAIN_FILE=$projectRoot/nx/cmake/devkitA64.cmake" `
    "-DDEVKITPRO=$DevkitPro" "-DBO3_NX_MESA_SDK=$MesaSdk" `
    "-DCMAKE_MAKE_PROGRAM=$ninja" -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { throw "NX preflight configure failed: $LASTEXITCODE" }
& $cmake --build $output --parallel $Jobs
if ($LASTEXITCODE -ne 0) { throw "NX preflight build failed: $LASTEXITCODE" }
$start = & "$DevkitPro/devkitA64/bin/aarch64-none-elf-nm.exe" $output/bo3_nx_preflight |
    Select-String '^[0-9a-fA-F]+ [Tt] _start$'
if (-not $start -or $start.Line -notmatch '^0000000000000000 ') {
    throw "NX startup symbol is not at zero: $start"
}
Write-Output "Verified _start at zero: $output/bo3_nx_preflight.nro"
