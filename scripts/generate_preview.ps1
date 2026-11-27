# ============================================================================
# generate_preview.ps1 — Generate standardized 700x700 GRML previewimage.png
# ============================================================================
# Renders steamcmd/previewimage.png (< 1 MB) in the Soviet-industrial
# dark-gold-crimson suite style, embedding the GRML logo and launcher preview.
#
# Usage: .\scripts\generate_preview.ps1
# ============================================================================
$ErrorActionPreference = "Stop"
$Root    = Split-Path $PSScriptRoot -Parent
$OutPath = Join-Path $Root "steamcmd\previewimage.png"
$LogoPath = Join-Path $Root "src\launcher\ui\static\logo.png"
$ShotPath = Join-Path $Root "docs\screenshots\01_mods.png"

Add-Type -AssemblyName System.Drawing

function New-Rect {
    param([float]$X, [float]$Y, [float]$W, [float]$H)
    return (New-Object System.Drawing.RectangleF $X, $Y, $W, $H)
}

function New-RoundedRectPath {
    param([float]$X, [float]$Y, [float]$W, [float]$H, [float]$R)
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $R * 2
    $path.AddArc($X, $Y, $d, $d, 180, 90)
    $path.AddArc($X + $W - $d, $Y, $d, $d, 270, 90)
    $path.AddArc($X + $W - $d, $Y + $H - $d, $d, $d, 0, 90)
    $path.AddArc($X, $Y + $H - $d, $d, $d, 90, 90)
    $path.CloseFigure()
    return $path
}

$W = 700
$H = 700
$bmp = New-Object System.Drawing.Bitmap $W, $H
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::ClearTypeGridFit

# Palette
$cBg          = [System.Drawing.Color]::FromArgb(255, 18, 17, 13)
$cOuterBorder = [System.Drawing.Color]::FromArgb(255, 200, 154, 58)
$cCrimson     = [System.Drawing.Color]::FromArgb(255, 152, 32, 28)
$cWhite       = [System.Drawing.Color]::FromArgb(255, 245, 242, 232)
$cMuted       = [System.Drawing.Color]::FromArgb(255, 170, 162, 138)
$cGold        = [System.Drawing.Color]::FromArgb(255, 214, 166, 69)
$cPillBg      = [System.Drawing.Color]::FromArgb(255, 26, 24, 18)
$cWinBg       = [System.Drawing.Color]::FromArgb(255, 22, 20, 15)

# Background & Outer Gold Frame
$g.Clear($cBg)
$outerPen = New-Object System.Drawing.Pen $cOuterBorder, 2
$g.DrawRectangle($outerPen, 8, 8, $W - 17, $H - 17)

# Fonts
$fBadge    = New-Object System.Drawing.Font "Segoe UI", 10.5, ([System.Drawing.FontStyle]::Bold)
$fTitle    = New-Object System.Drawing.Font "Segoe UI", 27, ([System.Drawing.FontStyle]::Bold)
$fSubtitle = New-Object System.Drawing.Font "Segoe UI", 11.5, ([System.Drawing.FontStyle]::Regular)
$fPill     = New-Object System.Drawing.Font "Segoe UI", 9.5, ([System.Drawing.FontStyle]::Bold)
$fTagline  = New-Object System.Drawing.Font "Segoe UI", 13, ([System.Drawing.FontStyle]::Bold)
$fFooter   = New-Object System.Drawing.Font "Segoe UI", 10, ([System.Drawing.FontStyle]::Regular)

$sfCenter = New-Object System.Drawing.StringFormat
$sfCenter.Alignment = [System.Drawing.StringAlignment]::Center
$sfCenter.LineAlignment = [System.Drawing.StringAlignment]::Center

$sfLeft = New-Object System.Drawing.StringFormat
$sfLeft.Alignment = [System.Drawing.StringAlignment]::Near
$sfLeft.LineAlignment = [System.Drawing.StringAlignment]::Center

$bCrimson = New-Object System.Drawing.SolidBrush $cCrimson
$bWhite   = New-Object System.Drawing.SolidBrush $cWhite
$bMuted   = New-Object System.Drawing.SolidBrush $cMuted
$bGold    = New-Object System.Drawing.SolidBrush $cGold
$bPillBg  = New-Object System.Drawing.SolidBrush $cPillBg
$bWinBg   = New-Object System.Drawing.SolidBrush $cWinBg
$penGold1 = New-Object System.Drawing.Pen $cGold, 1.4

# Top Badges
$pLeftBadge = New-RoundedRectPath 28 24 164 28 6
$g.FillPath($bCrimson, $pLeftBadge)
$g.DrawString("WRSR 1.2.0.9+", $fBadge, $bWhite, (New-Rect 28 24 164 28), $sfCenter)

$rx = $W - 208
$pRightBadge = New-RoundedRectPath $rx 24 180 28 6
$g.FillPath($bCrimson, $pRightBadge)
$g.DrawString("F9 IN-GAME HUB", $fBadge, $bWhite, (New-Rect $rx 24 180 28), $sfCenter)

# Logo + Title Header
$titleX = 28
if (Test-Path $LogoPath) {
    $logoImg = [System.Drawing.Image]::FromFile($LogoPath)
    try {
        $g.DrawImage($logoImg, 28, 66, 68, 68)
        $titleX = 108
    } finally {
        $logoImg.Dispose()
    }
}

$g.DrawString("GLORIOUS REPUBLIC", $fTitle, $bWhite, (New-Rect $titleX 64 ($W - $titleX - 24) 36), $sfLeft)
$g.DrawString("MOD LOADER (GRML)", $fTitle, $bGold, (New-Rect $titleX 98 ($W - $titleX - 24) 36), $sfLeft)
$g.DrawString("Mod Manager  |  Media VFS  |  Save Inspector  |  CrashGuard", $fSubtitle, $bMuted, (New-Rect 28 140 ($W - 56) 24), $sfLeft)

# 3 Feature Pills
$pills = @("RML + TESMIO COMPATIBLE", "VISUAL INI & F9 UI", "DIRECT SAVE LAUNCH")
$pillW = 198
$pillGap = 19
$pillX = 28
$pillY = 172
for ($i = 0; $i -lt 3; $i++) {
    $x = $pillX + $i * ($pillW + $pillGap)
    $pp = New-RoundedRectPath $x $pillY $pillW 28 6
    $g.FillPath($bPillBg, $pp)
    $g.DrawPath($penGold1, $pp)
    $g.DrawString($pills[$i], $fPill, $bGold, (New-Rect $x $pillY $pillW 28), $sfCenter)
}

# Framed Launcher Screenshot Preview
$winX = 28; $winY = 216; $winW = $W - 56; $winH = 372
$g.FillRectangle($bWinBg, $winX, $winY, $winW, $winH)
if (Test-Path $ShotPath) {
    $shotImg = [System.Drawing.Image]::FromFile($ShotPath)
    try {
        $ratio = [Math]::Min(($winW - 4) / [double]$shotImg.Width, ($winH - 4) / [double]$shotImg.Height)
        $drawW = [int]($shotImg.Width * $ratio)
        $drawH = [int]($shotImg.Height * $ratio)
        $drawX = $winX + [int](($winW - $drawW) / 2)
        $drawY = $winY + [int](($winH - $drawH) / 2)
        $g.DrawImage($shotImg, $drawX, $drawY, $drawW, $drawH)
    } finally {
        $shotImg.Dispose()
    }
}
$g.DrawRectangle($penGold1, $winX, $winY, $winW, $winH)

# Bottom Tagline & Footer
$g.DrawString("ONE LOADER FOR ALL YOUR WRSR MODS & OVERRIDES", $fTagline, $bGold, (New-Rect 28 602 ($W - 56) 32), $sfCenter)
$g.DrawString("Windows x64  |  21 Languages  |  github.com/ZydraHydra/GloriousRepublicModLoader", $fFooter, $bMuted, (New-Rect 28 638 ($W - 56) 28), $sfCenter)

$dir = Split-Path $OutPath -Parent
if (-not (Test-Path $dir)) {
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
}
$bmp.Save($OutPath, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose()
$bmp.Dispose()

$bytes = (Get-Item $OutPath).Length
Write-Host "Generated 700x700 previewimage.png ($bytes bytes) -> $OutPath" -ForegroundColor Green
