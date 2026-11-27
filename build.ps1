$ErrorActionPreference = "Stop"
$Root = $PSScriptRoot

Write-Host "==> Stopping any running launcher processes..." -ForegroundColor Cyan
Stop-Process -Name "GloriousRepublicModLoader" -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 300

$BuildDir = Join-Path $Root "build"
if (Test-Path $BuildDir) {
    Write-Host "==> Deleting $BuildDir..." -ForegroundColor Cyan
    Remove-Item -Recurse -Force $BuildDir
}

Write-Host "==> Building SvelteKit UI..." -ForegroundColor Cyan
Push-Location (Join-Path $Root "src\launcher-ui\web")
try {
    if (-not (Test-Path "node_modules")) {
        npm install
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
    npm run check
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    npm run build
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} finally {
    Pop-Location
}

$CmakeDir = Join-Path $BuildDir "cmake"
Write-Host "==> Building C++20 native launcher & runtime..." -ForegroundColor Cyan
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vcvars = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
    if (Test-Path $vcvars) {
        cmd /c "`"$vcvars`" && cmake -S `"$Root`" -B `"$CmakeDir`" -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build `"$CmakeDir`" -j"
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    } else {
        throw "MSVC x64 (cl.exe / vcvars64.bat) not found."
    }
} else {
    cmake -S $Root -B $CmakeDir -G Ninja -DCMAKE_BUILD_TYPE=Release
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    cmake --build $CmakeDir -j
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

Write-Host "==> Staging clean release output..." -ForegroundColor Cyan
$BinDir     = Join-Path $CmakeDir "bin"
$ReleaseDir = Join-Path $BuildDir "release"
New-Item -ItemType Directory -Force -Path $ReleaseDir | Out-Null

foreach ($f in @("GloriousRepublicModLoader.exe", "grml_runtime.dll", "grml_offsets.ini")) {
    $src = Join-Path $BinDir $f
    if (Test-Path $src) {
        Move-Item -Force $src (Join-Path $ReleaseDir $f)
    }
}

Write-Host "==> Clean build complete: $ReleaseDir" -ForegroundColor Green
Get-ChildItem $ReleaseDir | Select-Object Name, Length
