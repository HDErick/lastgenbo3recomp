param(
    [string]$LogName = 'multiplayer.log',
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
# Share the explicit content roots and diagnostics of the existing launcher.
if (-not $PSBoundParameters.ContainsKey('LogName')) {
    $PSBoundParameters['LogName'] = $LogName
}
& (Join-Path $PSScriptRoot 'launch-zombies.ps1') -ExecutableName bo3_mp @PSBoundParameters
