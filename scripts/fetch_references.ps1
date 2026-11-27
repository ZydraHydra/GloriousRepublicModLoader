# ============================================================================
# fetch_references.ps1 — Fetch gitignored upstream RML reference locally
# ============================================================================
$ErrorActionPreference = "Stop"
$Root = Split-Path $PSScriptRoot -Parent
$RefDir = Join-Path $Root "references\RML"

if (Test-Path $RefDir) {
    Write-Host "Updating existing reference in $RefDir..."
    git -C $RefDir pull --ff-only
} else {
    New-Item -ItemType Directory -Force -Path (Split-Path $RefDir -Parent) | Out-Null
    git clone --depth 1 https://github.com/Ultimate-Universe/WRSR-RepublicModLoader.git $RefDir
}
Write-Host "Reference ready at $RefDir" -ForegroundColor Green
