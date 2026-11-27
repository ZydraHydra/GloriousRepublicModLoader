# ============================================================================
# publish_workshop.ps1 — Build, stage, and upload GRML to Steam Workshop
# ============================================================================
# Usage:
#   .\scripts\publish_workshop.ps1 -DryRun
#   .\scripts\publish_workshop.ps1 -Visibility 2
#   .\scripts\publish_workshop.ps1 -SkipBuild -Visibility 0
# ============================================================================
param(
    [string]$Version = "",
    [string]$PublishedFileId = "",
    [ValidateSet("0", "1", "2")]
    [string]$Visibility = "2",
    [string]$Changenote = "",
    [string]$SteamUser = "",
    [switch]$SkipBuild,
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"
$Root = Split-Path $PSScriptRoot -Parent

$envFile = Join-Path $Root ".env.local"
$envMap  = @{}
if (Test-Path $envFile) {
    Get-Content $envFile | ForEach-Object {
        if ($_ -match "^\s*([A-Z0-9_]+)\s*=\s*(.+)$") {
            $envMap[$Matches[1]] = $Matches[2].Trim()
        }
    }
}

if (-not $SteamUser) {
    $SteamUser = if ($env:STEAM_USERNAME) { $env:STEAM_USERNAME } elseif ($envMap.ContainsKey("STEAM_USERNAME")) { $envMap["STEAM_USERNAME"] } else { "" }
}

if (-not $PublishedFileId -and $envMap.ContainsKey("WORKSHOP_PUBLISHED_FILE_ID")) {
    $PublishedFileId = $envMap["WORKSHOP_PUBLISHED_FILE_ID"]
}

$steamCmd = if ($env:STEAMCMD_EXE) { $env:STEAMCMD_EXE } elseif ($envMap.ContainsKey("STEAMCMD_EXE")) { $envMap["STEAMCMD_EXE"] } else { "" }
if (-not $steamCmd) {
    $cmd = Get-Command steamcmd -ErrorAction SilentlyContinue
    if ($cmd) {
        $steamCmd = $cmd.Source
    } else {
        $candidates = @("C:\steamcmd\steamcmd.exe", "D:\steamcmd\steamcmd.exe", "C:\Tools\steamcmd\steamcmd.exe")
        $wingetPkg  = "$env:LOCALAPPDATA\Microsoft\WinGet\Packages"
        if (Test-Path $wingetPkg) {
            $found = Get-ChildItem $wingetPkg -Recurse -Filter "steamcmd.exe" -ErrorAction SilentlyContinue |
                     Select-Object -First 1 -ExpandProperty FullName
            if ($found) { $candidates = @($found) + $candidates }
        }
        foreach ($c in $candidates) {
            if (Test-Path $c) { $steamCmd = $c; break }
        }
    }
}

$ResolvedVer = $Version.Trim() -replace '^v', ''
if (-not $ResolvedVer) {
    $ResolvedVer = (Get-Content (Join-Path $Root "VERSION") -TotalCount 1).Trim()
}

$pkgParams = @{
    Version         = $ResolvedVer
    PublishedFileId = $PublishedFileId
    Visibility      = $Visibility
    Changenote      = $Changenote
    Build           = (-not $SkipBuild)
}
& (Join-Path $PSScriptRoot "package_workshop.ps1") @pkgParams
if ($LASTEXITCODE -ne 0) {
    throw "Packaging failed"
}

$VdfPath = Join-Path $Root "release\GRML_v${ResolvedVer}\workshop_item.vdf"
if (-not (Test-Path $VdfPath)) {
    throw "Staged VDF not found at $VdfPath"
}

if ($DryRun) {
    Write-Host "`n[DRY RUN] (GRML v$ResolvedVer) VDF: $VdfPath" -ForegroundColor Yellow
    Get-Content $VdfPath | Write-Host
    $userDisplay = if ($SteamUser) { $SteamUser } else { "<STEAM_USERNAME>" }
    $cmdDisplay  = if ($steamCmd) { $steamCmd } else { "steamcmd.exe" }
    Write-Host "[DRY RUN] Command: `"$cmdDisplay`" +login $userDisplay +workshop_build_item `"$VdfPath`" +quit" -ForegroundColor Yellow
    return
}

if (-not $steamCmd -or -not (Test-Path $steamCmd)) {
    throw "steamcmd.exe not found. Set STEAMCMD_EXE in .env.local or install SteamCMD."
}
if (-not $SteamUser) {
    throw "Steam username not set. Pass -SteamUser <name> or set STEAM_USERNAME in .env.local."
}

Write-Host "`n=== Uploading GRML v$ResolvedVer to Steam Workshop ===" -ForegroundColor Cyan
& $steamCmd "+login" $SteamUser "+workshop_build_item" $VdfPath "+quit"
if ($LASTEXITCODE -ne 0) {
    throw "SteamCMD exited with code $LASTEXITCODE"
}

$vdfAfter = Get-Content $VdfPath -Raw
if ($vdfAfter -match '"publishedfileid"\s+"(\d+)"') {
    $newId = $Matches[1]
    if ($newId -ne "0") {
        Write-Host "Workshop upload succeeded! PublishedFileId = $newId" -ForegroundColor Green
        $metaPath = Join-Path $Root "steamcmd\workshop.json"
        if (Test-Path $metaPath) {
            $metaObj = Get-Content $metaPath -Raw | ConvertFrom-Json
            if ($metaObj.publishedFileId -ne $newId) {
                $metaObj.publishedFileId = $newId
                $jsonOut = $metaObj | ConvertTo-Json -Depth 10
                [System.IO.File]::WriteAllText($metaPath, $jsonOut + "`n", (New-Object System.Text.UTF8Encoding $false))
                Write-Host "  Updated $metaPath with PublishedFileId=$newId" -ForegroundColor Cyan
            }
        }
    }
}
