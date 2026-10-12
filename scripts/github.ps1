# Portable GitHub CLI with project-local configuration (ignored by Git).
$projectRoot = Split-Path -Parent $PSScriptRoot
$metadata = Get-Content -LiteralPath (Join-Path $projectRoot 'tools\github-cli\provenance.json') -Raw | ConvertFrom-Json
$oldConfig = $env:GH_CONFIG_DIR
try {
    $env:GH_CONFIG_DIR = Join-Path $projectRoot '.local\github'
    & $metadata.executable @args
    $githubExit = $LASTEXITCODE
} finally {
    $env:GH_CONFIG_DIR = $oldConfig
}
exit $githubExit
