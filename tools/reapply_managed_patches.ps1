# Re-applies the working-tree patches inside managed_components/
# (BSP graceful-degrade + unscii_16 384-glyph font) after
# `idf.py update-dependencies` reinstalls them. See bugs.md M20/M28.
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

if ($Check) {
    git apply --check $patch
    if ($?) { Write-Output "managed patches apply cleanly" }
    return
}

git apply --check $patch
if (-not $?) {
    Write-Error "Patch does not apply cleanly - inspect managed_components state first"
}

git apply $patch
Write-Output "managed patches re-applied - rebuild to verify"
