param(
    [string]$LogName = 'zombies.log',
    [string]$LogLevel = 'info',
    [string]$CommandLine = '',
    [string]$SampleSeconds = '',
    [ValidateSet(1, 2)][int]$SigninState = 1,
    [ValidateSet('auto', 'rtv', 'rov')][string]$RenderTargetPath = 'auto',
    [string]$CacheRoot = '',
    [switch]$CaptureFrames,
    [string]$DumpImagePath = '',
    [string[]]$RuntimeArguments = @(),
    [switch]$Background
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$executable = Join-Path $projectRoot 'build\bo3_zm.exe'
$logPath = Join-Path $projectRoot ('runtime\logs\' + $LogName)
if (-not $CacheRoot) { $CacheRoot = Join-Path $projectRoot 'runtime\cache' }
$arguments = @(
    '--game_data_root', (Join-Path $projectRoot 'runtime\game'),
    '--update_data_root', (Join-Path $projectRoot 'runtime\update'),
    '--user_data_root', (Join-Path $projectRoot 'runtime\user'),
    '--cache_root', $CacheRoot,
    '--gpu_plugin', 'xenos',
    '--xam_signin_state', $SigninState.ToString(),
    '--log_file', $logPath,
    '--log_level', $LogLevel
)
if ($CommandLine) { $arguments += @('--cl', $CommandLine) }
if ($RenderTargetPath -ne 'auto') {
    $arguments += @('--render_target_path_d3d12', $RenderTargetPath)
}
$arguments += $RuntimeArguments
$env:BO3_SAMPLE_SECS = $SampleSeconds
$env:BO3_CAPTURE_FRAMES = if ($CaptureFrames) { '1' } else { $null }
$env:BO3_DUMP_IMAGE = if ($DumpImagePath) { $DumpImagePath } else { $null }
if ($Background) {
    $quotedArguments = $arguments | ForEach-Object { '"' + $_.Replace('"', '\"') + '"' }
    $process = Start-Process -FilePath $executable -ArgumentList $quotedArguments -WorkingDirectory (Split-Path $executable) -WindowStyle Hidden -PassThru -RedirectStandardOutput ($logPath + '.stdout') -RedirectStandardError ($logPath + '.stderr')
    $process.Id | Set-Content (Join-Path $projectRoot 'runtime\logs\last_process_id.txt')
    Write-Output "Started BO3 Zombies PID $($process.Id), log $logPath"
} else {
    & $executable @arguments
    exit $LASTEXITCODE
}
