param([switch]$IncludeGameFiles)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$output = Join-Path $projectRoot 'build/nx-package/bo3recomp'
$nro = Join-Path $projectRoot 'build/nx-runtime/bo3_zm_nx.nro'
if (-not (Test-Path -LiteralPath $nro)) { throw 'Build bo3_zm_nx.nro first' }
New-Item -ItemType Directory -Path $output -Force | Out-Null
Copy-Item -LiteralPath $nro -Destination $output
Copy-Item -LiteralPath "$projectRoot/nx/runtime/bo3_zm_nx.toml" -Destination $output
foreach ($folder in 'game','update','user','cache') {
    New-Item -ItemType Directory -Path (Join-Path $output $folder) -Force | Out-Null
}
if ($IncludeGameFiles) {
    foreach ($folder in 'game','update') {
        Get-ChildItem -LiteralPath "$projectRoot/runtime/$folder" | ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $output $folder) -Recurse -Force
        }
    }
    # Common content holds the two compatibility packs and two DLC packages.
    Copy-Item -LiteralPath "$projectRoot/runtime/user/0000000000000000" `
        -Destination (Join-Path $output 'user') -Recurse -Force
}
@'
BO3 Zombies NX development build

Copy this bo3recomp folder to sdmc:/switch/bo3recomp.
Use title takeover. This is the first game build and has not booted on hardware.

Required game content, using the already staged PC runtime:
runtime/game/*    -> sdmc:/switch/bo3recomp/game/
runtime/update/*  -> sdmc:/switch/bo3recomp/update/
runtime/user/0000000000000000/ -> sdmc:/switch/bo3recomp/user/0000000000000000/

The game root must contain default_zm.xex and default_zm.xexp.
Keep the update root separate and preserve all package directory names.
The optional -IncludeGameFiles packaging argument copies these into this folder.

Logs to return after testing:
sdmc:/switch/bo3recomp/bo3_zm_nx.log
sdmc:/switch/bo3recomp/logs/rex/ (crash and stderr reports, when present)
An Atmosphere crash report, if the system reports an error.

Record the visible screen and whether Start/A reaches the menu.
'@ | Set-Content -LiteralPath (Join-Path $output 'READ-ME.txt') -Encoding utf8
Write-Output "Prepared $output"
