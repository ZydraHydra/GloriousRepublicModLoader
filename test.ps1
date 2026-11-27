param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$LauncherArgs
)

$ErrorActionPreference = "Stop"
$Root = $PSScriptRoot
$BuildScript = Join-Path $Root "build.ps1"
$ReleaseDir = Join-Path $Root "build\release"
$LauncherExe = Join-Path $ReleaseDir "GloriousRepublicModLoader.exe"

Write-Host "==> Running full rebuild before launching..." -ForegroundColor Cyan
& $BuildScript
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

if (-not (Test-Path $LauncherExe)) {
    throw "Launcher executable not found after build: $LauncherExe"
}

Write-Host "==> Starting launcher: $LauncherExe" -ForegroundColor Green
if ($LauncherArgs -and $LauncherArgs.Count -gt 0) {
    Start-Process -FilePath $LauncherExe -WorkingDirectory $ReleaseDir -ArgumentList $LauncherArgs
} else {
    Start-Process -FilePath $LauncherExe -WorkingDirectory $ReleaseDir
}
