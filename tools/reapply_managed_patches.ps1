# Re-applies the hand-authored patches inside managed_components/
# (LVGL lv_async PSRAM/lock hardening + buttonmatrix button-area getter for the
#  `ui` touch verbs + port JD9165 swap_xy guard) after
# `idf.py update-dependencies` reinstalls them. The generated 384-glyph
# unscii_16 font is tracked in git; restore it from HEAD when the vendored
# LVGL minor version matches, then apply managed_patches.patch.
#
# LVGL 9.6 moved the public headers to include/lvgl/, so the buttonmatrix
# getter's declaration now lives in include/lvgl/widgets/lv_buttonmatrix.h and
# the component-root lv_version.h is only an include shim. The version guard
# below reads whichever version header the tree actually carries.
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
$fontMarker = "0x2500-0x257F"
$versionCandidates = @(
    "managed_components/lvgl__lvgl/include/lvgl/lv_version.h",
    "managed_components/lvgl__lvgl/lv_version.h"
)
$fontPath = Join-Path $root $fontRel
$fontOk = (Test-Path -LiteralPath $fontPath) -and `
    ((Get-Content -Raw -LiteralPath $fontPath) -match $fontMarker)

# Returns the LVGL minor version declared in a block of header text, or $null.
function Get-LvglMinor {
    param([string[]]$Text)
    foreach ($line in $Text) {
        if ($line -match "#define\s+LVGL_VERSION_MINOR\s+([0-9]+)") {
            return $Matches[1]
        }
    }
    return $null
}

if ($Check) {
    if (-not $fontOk) {
        Write-Error "Extended unscii font missing: restore $fontRel from a matching LVGL commit first"
    }
    git apply --check $patch
    if ($?) { Write-Output "managed patches apply cleanly" }
    return
}

if (-not $fontOk) {
    # Worktree version: the first candidate header that exists.
    $workMinor = $null
    foreach ($rel in $versionCandidates) {
        $abs = Join-Path $root $rel
        if (Test-Path -LiteralPath $abs) {
            $workMinor = Get-LvglMinor (Get-Content -LiteralPath $abs)
            if ($null -ne $workMinor) { break }
        }
    }

    # HEAD version: same order, skipping headers the committed tree lacks
    # (a pre-9.6 HEAD has no include/lvgl/lv_version.h, and a tree that is
    # mid-upgrade may have neither). `git cat-file` writes its diagnostic to
    # stderr, which is a terminating error under $ErrorActionPreference=Stop,
    # so run it in try/catch instead of testing $?.
    $headMinor = $null
    foreach ($rel in $versionCandidates) {
        $exists = $false
        try {
            git cat-file -e ("HEAD:" + $rel) 2>&1 | Out-Null
            $exists = ($LASTEXITCODE -eq 0)
        } catch {
            $exists = $false
        }
        if (-not $exists) { continue }
        $headMinor = Get-LvglMinor (git show ("HEAD:" + $rel))
        if ($null -ne $headMinor) { break }
    }

    if ($null -eq $workMinor) {
        Write-Error "Cannot determine LVGL minor version for font restore"
    }
    # Only refuse when HEAD can name its version: a worktree that is being
    # upgraded to a new LVGL has a version HEAD does not know yet, and the
    # staged font is the one to restore.
    if ($null -ne $headMinor -and $workMinor -ne $headMinor) {
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
