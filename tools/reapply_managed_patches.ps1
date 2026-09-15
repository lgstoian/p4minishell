# Re-applies the hand-authored patches inside managed_components/
# (LVGL lv_async PSRAM/lock hardening + buttonmatrix button-area getter for the
#  `ui` touch verbs + port JD9165 swap_xy guard) after
# `idf.py update-dependencies` reinstalls them. The generated 384-glyph
# unscii_16 font is tracked in git; restore it from HEAD when the vendored
# LVGL minor version matches, then apply managed_patches.patch.
param(
    [switch]$Check
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$patch = Join-Path $PSScriptRoot "managed_patches.patch"

if (-not (Test-Path -LiteralPath $patch)) {
    Write-Error "Patch not found: $patch"
}

Set-Location -LiteralPath $root

$fontRel = "managed_components/lvgl__lvgl/src/font/lv_font_unscii_16.c"
$versionRel = "managed_components/lvgl__lvgl/lv_version.h"
$fontMarker = "0x2500-0x257F"
$fontPath = Join-Path $root $fontRel
$fontOk = (Test-Path -LiteralPath $fontPath) -and `
    ((Get-Content -Raw -LiteralPath $fontPath) -match $fontMarker)

if ($Check) {
    if (-not $fontOk) {
        Write-Error "Extended unscii font missing: restore $fontRel from a matching LVGL commit first"
    }
    git apply --check $patch
    if ($?) { Write-Output "managed patches apply cleanly" }
    return
}

if (-not $fontOk) {
    $workMatch = Select-String -Path (Join-Path $root $versionRel) `
        -Pattern "#define LVGL_VERSION_MINOR ([0-9]+)" | Select-Object -First 1
    $headVersion = git show ("HEAD:" + $versionRel)
    if (-not $?) {
        Write-Error "Cannot read HEAD version for font restore"
    }
    $headMatch = $headVersion | Select-String `
        -Pattern "#define LVGL_VERSION_MINOR ([0-9]+)" | Select-Object -First 1
    if ($null -eq $workMatch -or $null -eq $headMatch) {
        Write-Error "Cannot determine LVGL minor version for font restore"
    }
    $workMinor = $workMatch.Matches.Groups[1].Value
    $headMinor = $headMatch.Matches.Groups[1].Value
    if ($workMinor -ne $headMinor) {
        Write-Error ("HEAD LVGL minor ({0}) does not match worktree ({1}); " +
            "regenerate the extended unscii font for this LVGL version") -f $headMinor, $workMinor
    }
    git checkout -- $fontRel
    if (-not $?) {
        Write-Error "Failed to restore extended unscii font from HEAD"
    }
}

git apply --check $patch
if (-not $?) {
    Write-Error "Patch does not apply cleanly - inspect managed_components state first"
}

git apply $patch
Write-Output "managed patches re-applied - rebuild to verify"
