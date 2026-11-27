# ============================================================================
# package_workshop.ps1 — Stage GRML release & Steam Workshop package
# ============================================================================
# Usage:
#   .\scripts\package_workshop.ps1
#   .\scripts\package_workshop.ps1 -Build
#   .\scripts\package_workshop.ps1 -Version 2.1.0-alpha.1 -Visibility 2
# ============================================================================
param(
    [string]$Version = "",
    [string]$PublishedFileId = "",
    [ValidateSet("0", "1", "2")]
    [string]$Visibility = "",
    [string]$Changenote = "",
    [switch]$Build
)

$ErrorActionPreference = "Stop"

function Convert-MarkdownToSteamBBCode {
    param([string]$Text)
    $t = $Text -replace '\*\*([^*]+)\*\*', '[b]$1[/b]'
    $t = $t -replace '`([^`]+)`', '[i]$1[/i]'
    $t = $t -replace '\[([^\]]+)\]\((https?://[^)]+)\)', '[url=$2]$1[/url]'
    return $t
}

function Get-GrmlChangelogData {
    param(
        [string]$RootDir,
        [string]$ResolvedVer
    )
    $changelogMd = Join-Path $RootDir "CHANGELOG.md"
    $latestPlain = @()
    $bbSections  = @()

    if (Test-Path $changelogMd) {
        $lines    = Get-Content $changelogMd -Encoding UTF8
        $sections = @()
        $curTitle = $null
        $curItems = @()
        foreach ($raw in $lines) {
            $line = $raw.Trim()
            if ($line -match '^##\s+(.+)$') {
                if ($curTitle) {
                    $sections += [pscustomobject]@{ title = $curTitle; items = $curItems }
                }
                $curTitle = ($Matches[1] -replace '^\[|\]$', '').Trim()
                $curItems = @()
            } elseif ($curTitle -and $line -match '^[-*]\s+(.+)$') {
                $curItems += $Matches[1].Trim()
            }
        }
        if ($curTitle) {
            $sections += [pscustomobject]@{ title = $curTitle; items = $curItems }
        }

        if ($sections.Count -gt 0) {
            $top = $sections[0]
            foreach ($item in $top.items) {
                $plain = $item -replace '\*\*([^*]+)\*\*', '$1' -replace '`([^`]+)`', '$1'
                $latestPlain += "- $plain"
            }

            $maxSections = [Math]::Min(3, $sections.Count)
            for ($i = 0; $i -lt $maxSections; $i++) {
                $sec = $sections[$i]
                $bb  = "[b]$($sec.title)[/b][list]"
                foreach ($item in $sec.items) {
                    $bb += "`n[*] $(Convert-MarkdownToSteamBBCode $item)"
                }
                $bb += "`n[/list]"
                $bbSections += $bb
            }
        }
    }

    if ($latestPlain.Count -eq 0) {
        $git = Get-Command git -ErrorAction SilentlyContinue
        if ($git -and (Test-Path (Join-Path $RootDir ".git"))) {
            Push-Location $RootDir
            $prevEap = $ErrorActionPreference
            $ErrorActionPreference = "Continue"
            try {
                $lastTag = (& git describe --tags --abbrev=0 2>$null | Select-Object -First 1)
                $range   = if ($LASTEXITCODE -eq 0 -and $lastTag) { "$lastTag..HEAD" } else { "-n 8" }
                $commits = & git log $range --no-merges --pretty=format:"- %s" 2>$null
                if ($LASTEXITCODE -eq 0 -and $commits) {
                    $latestPlain = @($commits)
                }
            } finally {
                $global:LASTEXITCODE = 0
                $ErrorActionPreference = $prevEap
                Pop-Location
            }
        }
    }

    $plainBlock = if ($latestPlain.Count -gt 0) {
        "Release v$ResolvedVer`n" + ($latestPlain -join "`n")
    } else {
        "Release v$ResolvedVer"
    }

    return [pscustomobject]@{
        changenote = $plainBlock
        bbSections = $bbSections
    }
}

$Root = Split-Path $PSScriptRoot -Parent

$ResolvedVer = $Version.Trim() -replace '^v', ''
if (-not $ResolvedVer) {
    $ResolvedVer = (Get-Content (Join-Path $Root "VERSION") -TotalCount 1).Trim()
}

if ($Build) {
    & (Join-Path $Root "build.ps1")
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed"
    }
}

$ReleaseBinDir = Join-Path $Root "build\release"
$CoreFiles = @("GloriousRepublicModLoader.exe", "grml_runtime.dll", "grml_offsets.ini")
foreach ($f in $CoreFiles) {
    $p = Join-Path $ReleaseBinDir $f
    if (-not (Test-Path $p) -or (Get-Item $p).Length -le 0) {
        throw "Missing or empty build output: $p — run .\build.ps1 (or pass -Build)"
    }
}

$MetaPath = Join-Path $Root "steamcmd\workshop.json"
$meta = if (Test-Path $MetaPath) {
    Get-Content $MetaPath -Raw -Encoding UTF8 | ConvertFrom-Json
} else {
    [pscustomobject]@{
        appId           = "784150"
        publishedFileId = "0"
        workshopTitle   = "Glorious Republic Mod Loader (GRML)"
        visibility      = "2"
    }
}

$resolvedId = $PublishedFileId.Trim()
if (-not $resolvedId -and $env:WORKSHOP_PUBLISHED_FILE_ID) {
    $resolvedId = $env:WORKSHOP_PUBLISHED_FILE_ID.Trim()
}
if (-not $resolvedId -and $meta.publishedFileId) {
    $resolvedId = ([string]$meta.publishedFileId).Trim()
}
if (-not $resolvedId) {
    $resolvedId = "0"
}

$resolvedVis = if ($Visibility) { $Visibility } elseif ($meta.visibility) { [string]$meta.visibility } else { "2" }
$workshopTitle = if ($meta.workshopTitle) { [string]$meta.workshopTitle } else { "Glorious Republic Mod Loader (GRML)" }

$StageDir   = Join-Path $Root "release\GRML_v${ResolvedVer}"
$ContentDir = Join-Path $StageDir "content"

if (Test-Path $StageDir) {
    Remove-Item $StageDir -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $ContentDir | Out-Null

# 1. Copy core binaries, offsets & LICENSE into content/
foreach ($f in $CoreFiles) {
    Copy-Item (Join-Path $ReleaseBinDir $f) $ContentDir -Force
}
Copy-Item (Join-Path $Root "LICENSE") $ContentDir -Force

# 2. Generate/copy previewimage.png (< 1 MB required by Steam)
$PreviewStage   = Join-Path $StageDir "previewimage.png"
$PreviewContent = Join-Path $ContentDir "previewimage.png"
$PreviewSrc     = Join-Path $Root "steamcmd\previewimage.png"
$GenScript      = Join-Path $PSScriptRoot "generate_preview.ps1"
if (Test-Path $GenScript) {
    $regen = (-not (Test-Path $PreviewSrc)) -or ((Get-Item $GenScript).LastWriteTimeUtc -gt (Get-Item $PreviewSrc).LastWriteTimeUtc)
    if ($regen) {
        & $GenScript
    }
}
if (-not (Test-Path $PreviewSrc)) {
    throw "Preview image not found at $PreviewSrc"
}
Copy-Item $PreviewSrc $PreviewStage -Force
Copy-Item $PreviewSrc $PreviewContent -Force

$previewSize = (Get-Item $PreviewStage).Length
if ($previewSize -gt 1048576) {
    throw "previewimage.png exceeds Steam's 1 MB limit ($previewSize bytes)"
}

# 3. Copy workshopconfig.ini into content/
$CfgSrc = Join-Path $Root "steamcmd\workshopconfig.ini"
if (Test-Path $CfgSrc) {
    $cfgText = Get-Content $CfgSrc -Raw -Encoding UTF8
    if ($resolvedId -and $resolvedId -ne "0") {
        $cfgText = $cfgText -replace '(?m)^\$ITEM_ID\s+\S+', "`$ITEM_ID $resolvedId"
    }
    $cfgText = $cfgText -replace '(?m)^\$VISIBILITY\s+\S+', "`$VISIBILITY $resolvedVis"
    $cfgText = $cfgText -replace '(?m)^\$ITEM_NAME\s+".*"', "`$ITEM_NAME `"$workshopTitle`""
    $cfgText = $cfgText -replace '\{VERSION\}', $ResolvedVer
    [System.IO.File]::WriteAllText((Join-Path $ContentDir "workshopconfig.ini"), $cfgText, (New-Object System.Text.UTF8Encoding $false))
}

# 4. Render clean workshop_item.vdf OUTSIDE content/
$changelogData = Get-GrmlChangelogData -RootDir $Root -ResolvedVer $ResolvedVer
$note = if ($Changenote) { $Changenote } else { $changelogData.changenote }
$VdfSrc = Join-Path $Root "steamcmd\workshop_item.vdf"
if (Test-Path $VdfSrc) {
    $contentSlash = $ContentDir -replace '\\', '/'
    $previewSlash = $PreviewStage -replace '\\', '/'
    $vdf = Get-Content $VdfSrc -Raw -Encoding UTF8
    $vdf = $vdf -replace '\{WORKSHOP_TITLE\}', $workshopTitle
    $vdf = $vdf -replace '\{VERSION\}', $ResolvedVer
    $vdf = $vdf -replace '\{PUBLISHED_FILE_ID\}', $resolvedId
    $vdf = $vdf -replace '\{CONTENT_DIR\}', $contentSlash
    $vdf = $vdf -replace '\{PREVIEW_FILE\}', $previewSlash
    $vdf = $vdf -replace '\{VISIBILITY\}', $resolvedVis
    $vdf = $vdf -replace '\{CHANGENOTE\}', ($note -replace '"', "'")

    # Steam Workshop caps ISteamUGC::SetItemDescription at 8,000 characters.
    $maxDescChars = 7900
    $baseDescLen  = 0
    if ($vdf -match '(?s)"description"\s+"(.*?)"\s+"language"') {
        $baseDescLen = ($Matches[1] -replace '\{CHANGELOG_SECTION\}', '').Length
    }
    $fittedChangelog = ""
    $sectionsList = @($changelogData.bbSections)
    if ($sectionsList.Count -gt 0 -and $baseDescLen -gt 0) {
        $header    = "`n`n[h1]Recent Changelog[/h1]"
        $candidate = $header
        $added     = 0
        foreach ($secBb in $sectionsList) {
            $cleanSec = $secBb -replace '"', "'"
            $next = if ($added -eq 0) { $candidate + $cleanSec } else { $candidate + "`n" + $cleanSec }
            if (($baseDescLen + $next.Length) -le $maxDescChars) {
                $candidate = $next
                $added++
            } else {
                break
            }
        }
        if ($added -gt 0) {
            $fittedChangelog = $candidate
        }
    }
    $vdf = $vdf -replace '\{CHANGELOG_SECTION\}', $fittedChangelog

    if ($vdf -match '(?s)"description"\s+"(.*?)"\s+"language"') {
        $finalLen = $Matches[1].Length
        if ($finalLen -gt 8000) {
            throw "Steam Workshop description exceeds 8,000-char limit ($finalLen chars)"
        }
        Write-Host "  Description : $finalLen / 8000 chars"
    }

    [System.IO.File]::WriteAllText((Join-Path $StageDir "workshop_item.vdf"), $vdf, (New-Object System.Text.UTF8Encoding $false))
}

# 5. Create clean standalone ZIP for GitHub Releases / manual install
$ZipStage = Join-Path $StageDir "zip_tmp"
New-Item -ItemType Directory -Force -Path $ZipStage | Out-Null
foreach ($f in $CoreFiles) {
    Copy-Item (Join-Path $ReleaseBinDir $f) $ZipStage -Force
}
Copy-Item (Join-Path $Root "LICENSE") $ZipStage -Force
$ZipPath = Join-Path $StageDir "GRML-v${ResolvedVer}-win64.zip"
Compress-Archive -Path "$ZipStage\*" -DestinationPath $ZipPath -Force
Remove-Item $ZipStage -Recurse -Force

Write-Host "Staged GRML v$ResolvedVer -> $StageDir" -ForegroundColor Green
Write-Host "  Workshop ID : $resolvedId"
Write-Host "  Visibility  : $resolvedVis (0=Public, 1=Friends, 2=Hidden)"
Write-Host "  Archive zip : $ZipPath"
