param(
    [string]$LogName = 'pc-vulkan.log',
    [ValidateSet('fbo', 'fsi')][string]$RenderTargetPath = 'fbo',
    [switch]$CaptureFrames,
    [switch]$DumpShaders,
    [switch]$Background
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$executable = Join-Path $projectRoot 'build/pc-vulkan/bo3_zm_vulkan.exe'
$logPath = Join-Path $projectRoot ('runtime/logs/' + $LogName)
$cacheRoot = Join-Path $projectRoot ('runtime/cache-pc-vulkan-' + $RenderTargetPath)
if (-not (Test-Path -LiteralPath $executable)) { throw 'Build the PC Vulkan baseline first' }
New-Item -ItemType Directory -Path $cacheRoot -Force | Out-Null
$arguments = @(
    '--game_data_root', (Join-Path $projectRoot 'runtime/game'),
    '--update_data_root', (Join-Path $projectRoot 'runtime/update'),
    '--user_data_root', (Join-Path $projectRoot 'runtime/user'),
    '--cache_root', $cacheRoot,
    '--gpu_plugin', 'xenos',
    # GPU-plugin cvars register after CLI parsing; separated values are not
    # replayed by SDK 0.10. Use --name=value for those late registrations.
    "--render_target_path_vulkan=$RenderTargetPath",
    '--xam_signin_state', '1',
    '--log_file', $logPath,
    '--log_level', 'info',
    '--audio_mute'
)
if ($DumpShaders) {
    $arguments += ('--dump_shaders=' + (Join-Path $projectRoot 'runtime/shader-dumps/pc-vulkan'))
}
$env:BO3_SAMPLE_SECS = $null
$env:BO3_CAPTURE_FRAMES = if ($CaptureFrames) { '1' } else { $null }
$env:BO3_DUMP_IMAGE = $null
if ($Background) {
    $quotedArguments = $arguments | ForEach-Object { '"' + $_.Replace('"', '\"') + '"' }
    $process = Start-Process -FilePath $executable -ArgumentList $quotedArguments `
        -WorkingDirectory (Split-Path $executable) -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput ($logPath + '.stdout') -RedirectStandardError ($logPath + '.stderr')
    $process.Id | Set-Content (Join-Path $projectRoot 'runtime/logs/pc-vulkan-process-id.txt')
    Write-Output "Started PC Vulkan PID $($process.Id), log $logPath"
} else {
    & $executable @arguments
    exit $LASTEXITCODE
}
