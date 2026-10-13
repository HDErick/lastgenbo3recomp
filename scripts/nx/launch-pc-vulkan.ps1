param(
    [string]$LogName = 'pc-vulkan.log',
    [ValidateSet('fbo', 'fsi')][string]$RenderTargetPath = 'fbo',
    [switch]$CaptureFrames,
    [switch]$DumpShaders,
    [switch]$GpuCapture,
    [switch]$D3DTrace,
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
$captureEnvironment = @{}
foreach ($name in 'BO3_SAMPLE_SECS','BO3_CAPTURE_FRAMES','BO3_DUMP_IMAGE',
    'BO3_GPU_CAPTURE_DIR','BO3_RENDERDOC_DLL','VK_LAYER_PATH','VK_INSTANCE_LAYERS',
    'ENABLE_VULKAN_RENDERDOC_CAPTURE','VK_ADD_IMPLICIT_LAYER_PATH','BO3_D3D_TRACE_DIR') {
    $captureEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
try {
$env:BO3_SAMPLE_SECS = $null
$env:BO3_CAPTURE_FRAMES = if ($CaptureFrames) { '1' } else { $null }
$env:BO3_DUMP_IMAGE = $null
$env:BO3_GPU_CAPTURE_DIR = $null
$env:BO3_RENDERDOC_DLL = $null
$env:BO3_D3D_TRACE_DIR = if ($D3DTrace) { Join-Path $projectRoot 'runtime/native-discovery' } else { $null }
if ($GpuCapture) {
    $renderDocRoot = Join-Path $projectRoot 'tools/renderdoc/RenderDoc_1.46_64'
    if (-not (Test-Path -LiteralPath (Join-Path $renderDocRoot 'renderdoc.dll'))) {
        throw 'Portable RenderDoc 1.46 is missing under tools/renderdoc'
    }
    $captureRoot = Join-Path $projectRoot 'build/draw-capture'
    New-Item -ItemType Directory -Path $captureRoot -Force | Out-Null
    $env:BO3_GPU_CAPTURE_DIR = $captureRoot
    $env:BO3_RENDERDOC_DLL = Join-Path $renderDocRoot 'renderdoc.dll'
    # Discover the capture manifest as an implicit layer, as RenderDoc expects.
    # Preserve normal layer search paths; no system-wide registration needed.
    $env:VK_ADD_IMPLICIT_LAYER_PATH = $renderDocRoot
    $env:ENABLE_VULKAN_RENDERDOC_CAPTURE = '1'
    $arguments += '--gpu_debug_markers'
}
$launchExecutable = $executable
$launchArguments = $arguments
if ($GpuCapture) {
    $launchExecutable = Join-Path $renderDocRoot 'renderdoccmd.exe'
    $launchArguments = @('capture', '-d', (Split-Path $executable), '-c',
        (Join-Path $captureRoot 'white-squares'), '--opt-capture-callstacks',
        '--opt-capture-callstacks-only-actions', $executable) + $arguments
}
if ($Background) {
    $quotedArguments = $launchArguments | ForEach-Object { '"' + $_.Replace('"', '\"') + '"' }
    $launchTime = Get-Date
    $process = Start-Process -FilePath $launchExecutable -ArgumentList $quotedArguments `
        -WorkingDirectory (Split-Path $executable) -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput ($logPath + '.stdout') -RedirectStandardError ($logPath + '.stderr')
    if ($GpuCapture) {
        for ($attempt = 0; $attempt -lt 30; $attempt++) {
            $target = Get-Process -Name bo3_zm_vulkan -ErrorAction SilentlyContinue |
                Where-Object { $_.StartTime -ge $launchTime } | Select-Object -First 1
            if ($target) { $process = $target; break }
            Start-Sleep -Milliseconds 100
        }
    }
    $process.Id | Set-Content (Join-Path $projectRoot 'runtime/logs/pc-vulkan-process-id.txt')
    Write-Output "Started PC Vulkan PID $($process.Id), log $logPath"
} else {
    & $launchExecutable @launchArguments
    exit $LASTEXITCODE
}
} finally {
    foreach ($name in $captureEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable($name, $captureEnvironment[$name], 'Process')
    }
}
