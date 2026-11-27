<#
.SYNOPSIS
    Capture real PrintWindow screenshots of every P4Sync drop-down in both themes.

.DESCRIPTION
    Launches the shipped P4SyncGui.exe in --mock mode, switches theme through the
    app's own toggle button, finds each ComboBox with UI Automation, expands it,
    then captures the *popup window* with PrintWindow (not CopyFromScreen: this
    desktop is shared, and screen grabs pick up a different app).

    Printed for each capture: popup window class, size, dominant background RGB,
    farthest text RGB and WCAG contrast ratio. Exit code 1 if any pair is
    unreadable (ratio < 4.5) or a page has no ComboBox when one is expected.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File apps\p4sync_gui\capture_dropdowns.ps1 -Probe
    powershell -ExecutionPolicy Bypass -File apps\p4sync_gui\capture_dropdowns.ps1
#>
param(
    [string]$Exe = "src\P4SyncGui\bin\Release\net10.0-windows\P4SyncGui.exe",
    [string]$OutDir = "docs",
    [string[]]$Pages = @("devices", "dashboard", "files", "screenshot", "pim", "apps", "shell", "plan", "settings"),
    [string[]]$Themes = @("Dark", "Light"),
    [switch]$Probe,
    [switch]$SaveMain
)

$ErrorActionPreference = "Stop"
# Allow both `-Pages apps settings` and `-Pages apps,settings` from powershell -File.
$Pages = @($Pages | ForEach-Object { $_ -split ',' })
$Themes = @($Themes | ForEach-Object { $_ -split ',' })
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

Add-Type @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class P4SyncDropdownWin {
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }
    public delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder sb, int max);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder sb, int max);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
}
"@

function Get-WindowsOfProcess([int]$pidWanted) {
    $list = New-Object System.Collections.Generic.List[object]
    $cb = [P4SyncDropdownWin+EnumProc] {
        param($h, $l)
        [uint32]$p = 0
        [void][P4SyncDropdownWin]::GetWindowThreadProcessId($h, [ref]$p)
        if ($p -eq $pidWanted) {
            $class = New-Object System.Text.StringBuilder 256
            [void][P4SyncDropdownWin]::GetClassName($h, $class, 256)
            $title = New-Object System.Text.StringBuilder 256
            [void][P4SyncDropdownWin]::GetWindowText($h, $title, 256)
            $rect = New-Object P4SyncDropdownWin+RECT
            [void][P4SyncDropdownWin]::GetWindowRect($h, [ref]$rect)
            $list.Add([pscustomobject]@{
                Handle = $h
                Class = $class.ToString()
                Title = $title.ToString()
                Visible = [P4SyncDropdownWin]::IsWindowVisible($h)
                Rect = $rect
            })
        }
        return $true
    }
    [void][P4SyncDropdownWin]::EnumWindows($cb, [IntPtr]::Zero)
    return $list
}

function Get-Contrast([System.Drawing.Bitmap]$bmp) {
    # Dominant colour = popup background; farthest-luminance colour = glyph ink.
    $counts = @{}
    for ($y = 2; $y -lt $bmp.Height - 2; $y += 2) {
        for ($x = 2; $x -lt $bmp.Width - 2; $x += 2) {
            $c = $bmp.GetPixel($x, $y)
            $key = "{0},{1},{2}" -f $c.R, $c.G, $c.B
            if ($counts.ContainsKey($key)) { $counts[$key]++ } else { $counts[$key] = 1 }
        }
    }
    $bgKey = ($counts.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 1).Key
    $bg = $bgKey.Split(','); $br = [int]$bg[0]; $bgG = [int]$bg[1]; $bb = [int]$bg[2]
    function Chan([int]$v) {
        $x = $v / 255.0
        if ($x -le 0.03928) { return $x / 12.92 }
        return [math]::Pow(($x + 0.055) / 1.055, 2.4)
    }
    function Lum([int]$r, [int]$g, [int]$b) {
        return 0.2126 * (Chan $r) + 0.7152 * (Chan $g) + 0.0722 * (Chan $b)
    }
    $bgLum = Lum $br $bgG $bb
    # Ignore the highlighted first item and the popup border: count glyph pixels
    # only in the lower part of the list, and require a real cluster of them.
    # Picking the single farthest pixel would report a false green from a border.
    $inkCounts = @{}
    $startY = [math]::Max(3, [int]($bmp.Height * 0.5))
    for ($y = $startY; $y -lt $bmp.Height - 8; $y++) {
        for ($x = 8; $x -lt $bmp.Width - 14; $x++) {
            $c = $bmp.GetPixel($x, $y)
            $key = "{0},{1},{2}" -f $c.R, $c.G, $c.B
            if ($inkCounts.ContainsKey($key)) { $inkCounts[$key]++ } else { $inkCounts[$key] = 1 }
        }
    }
    # Pick the most contrasting *cluster* (at least 5 pixels) so a single stray
    # border pixel cannot fake a pass, and antialiasing near the background
    # cannot hide real dark text either.
    $farLum = $bgLum; $farKey = $bgKey; $bestDiff = 0.0
    foreach ($k in $inkCounts.Keys) {
        $v = $k.Split(','); $l = Lum ([int]$v[0]) ([int]$v[1]) ([int]$v[2])
        $diff = [math]::Abs($l - $bgLum)
        if ($diff -gt $bestDiff -and $inkCounts[$k] -ge 5) {
            $farLum = $l; $farKey = $k; $bestDiff = $diff
        }
    }
    $hi = [math]::Max($bgLum, $farLum); $lo = [math]::Min($bgLum, $farLum)
    $ratio = ($hi + 0.05) / ($lo + 0.05)
    return [pscustomobject]@{ Background = $bgKey; Ink = $farKey; Ratio = [math]::Round($ratio, 2) }
}

function Invoke-ButtonByName([System.Windows.Automation.AutomationElement]$root, [string]$name) {
    $cond = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::NameProperty, $name)
    $type = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
        [System.Windows.Automation.ControlType]::Button)
    $and = New-Object System.Windows.Automation.AndCondition -ArgumentList @($cond, $type)
    $btn = $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $and)
    if ($null -eq $btn) { return $false }
    $btn.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
    return $true
}

# Runtime UIA audit: only these pages are expected to own a drop-down. Any new
# combo on another page changes this count and fails the guard until reviewed.
$ExpectedCombos = @{
    devices = 0; dashboard = 0; files = 0; screenshot = 0; pim = 0
    apps = 1; shell = 0; plan = 0; settings = 1
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$exeFull = Join-Path $PSScriptRoot $Exe
$outFull = Join-Path $PSScriptRoot $OutDir
if (-not (Test-Path $exeFull)) { throw "Build the GUI first: dotnet build apps/p4sync_gui/P4SyncGui.sln -c Release -> $exeFull" }
New-Item -ItemType Directory -Force -Path $outFull | Out-Null

$failures = New-Object System.Collections.Generic.List[string]
foreach ($theme in $Themes) {
    foreach ($page in $Pages) {
        $proc = Start-Process -FilePath $exeFull -ArgumentList @("--mock", "--page", $page) -PassThru
        try {
            $handle = [IntPtr]::Zero
            for ($i = 0; $i -lt 80; $i++) {
                Start-Sleep -Milliseconds 250
                $proc.Refresh()
                if ($proc.MainWindowHandle -ne [IntPtr]::Zero) { $handle = $proc.MainWindowHandle; break }
            }
            if ($handle -eq [IntPtr]::Zero) { throw "No window for page '$page'" }
            [void][P4SyncDropdownWin]::ShowWindow($handle, 9)
            [void][P4SyncDropdownWin]::SetForegroundWindow($handle)
            Start-Sleep -Milliseconds 700

            $root = [System.Windows.Automation.AutomationElement]::FromHandle($handle)
            # Switch theme with the app's own control; assert the resulting label.
            $labelName = "$theme mode"
            for ($t = 0; $t -lt 2; $t++) {
                $labelCond = New-Object System.Windows.Automation.PropertyCondition(
                    [System.Windows.Automation.AutomationElement]::NameProperty,
                    $labelName)
                $labelBtn = $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $labelCond)
                if ($null -ne $labelBtn) { break }
                if (-not (Invoke-ButtonByName $root "Toggle light or dark theme")) {
                    throw "Theme toggle button not found for $theme"
                }
                Start-Sleep -Milliseconds 350
                $root = [System.Windows.Automation.AutomationElement]::FromHandle($handle)
            }
            if ($null -eq $labelBtn) { throw "Could not switch theme to $theme (button label never matched)" }

            $comboCond = New-Object System.Windows.Automation.PropertyCondition(
                [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
                [System.Windows.Automation.ControlType]::ComboBox)
            $combos = $root.FindAll([System.Windows.Automation.TreeScope]::Descendants, $comboCond)
            $expectedCount = $ExpectedCombos[$page]
            if ($combos.Count -ne $expectedCount) {
                $failures.Add("$theme/${page}: expected $expectedCount ComboBox(es), found $($combos.Count)")
            }
            if ($combos.Count -eq 0) {
                Write-Output ("audited {0}/{1}: no ComboBox (expected {2})" -f $theme, $page, $expectedCount)
            }

            for ($ci = 0; $ci -lt $combos.Count; $ci++) {
                $combo = $combos.Item($ci)
                $name = $combo.Current.Name
                if ([string]::IsNullOrWhiteSpace($name)) { $name = "combobox-$ci" }
                $slug = ($name -replace '[^a-zA-Z0-9]+', '-').Trim('-').ToLowerInvariant()
                if ($Probe) {
                    Write-Output ("  COMBO {0}/{1}: name='{2}' enabled={3} offscreen={4} rect={5}" -f `
                        $theme, $page, $name, $combo.Current.IsEnabled, $combo.Current.IsOffscreen, $combo.Current.BoundingRectangle)
                }
                $combo.SetFocus()
                Start-Sleep -Milliseconds 200
                $expand = $combo.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern)
                $expand.Expand()
                Start-Sleep -Milliseconds 600
                if ($Probe) {
                    Write-Output ("  EXPAND {0}/{1}/{2}: state={3}" -f $theme, $page, $name, $expand.Current.ExpandCollapseState)
                }

                $popups = @()
                for ($attempt = 0; $attempt -lt 4 -and $popups.Count -eq 0; $attempt++) {
                    $windows = Get-WindowsOfProcess $proc.Id
                    if ($Probe) {
                        foreach ($w in $windows) {
                            Write-Output ("  PROBE {0}/{1} try{4}: h={2} class='{3}' title='{5}' visible={6} rect={7},{8} {9}x{10}" -f `
                                $theme, $page, $w.Handle, $w.Class, $attempt, $w.Title, $w.Visible, `
                                $w.Rect.Left, $w.Rect.Top, ($w.Rect.Right - $w.Rect.Left), ($w.Rect.Bottom - $w.Rect.Top))
                        }
                    }
                    $popups = @($windows | Where-Object {
                        $_.Handle -ne $handle -and $_.Visible -and
                        ($_.Rect.Right - $_.Rect.Left) -ge 60 -and
                        ($_.Rect.Bottom - $_.Rect.Top) -ge 30 -and
                        ($_.Class -eq 'Popup' -or $_.Title -eq '')
                    })
                    if ($popups.Count -eq 0) {
                        Start-Sleep -Milliseconds 400
                        $expand.Collapse(); Start-Sleep -Milliseconds 150; $expand.Expand()
                        Start-Sleep -Milliseconds 400
                    }
                }
                if ($popups.Count -eq 0) { $failures.Add("$theme/$page/${name}: no popup window found"); continue }
                $popup = $popups[-1]
                $w = $popup.Rect.Right - $popup.Rect.Left
                $h = $popup.Rect.Bottom - $popup.Rect.Top
                $bmp = New-Object System.Drawing.Bitmap($w, $h)
                $gfx = [System.Drawing.Graphics]::FromImage($bmp)
                try {
                    $hdc = $gfx.GetHdc()
                    try {
                        if (-not [P4SyncDropdownWin]::PrintWindow($popup.Handle, $hdc, 2)) {
                            throw "PrintWindow failed on popup class '$($popup.Class)'"
                        }
                    } finally { $gfx.ReleaseHdc($hdc) }
                    $path = Join-Path $outFull ("dropdown-{0}-{1}-{2}.png" -f $theme.ToLowerInvariant(), $page, $slug)
                    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
                    $contrast = Get-Contrast $bmp
                    Write-Output ("captured {0}/{1}/{2}: class={3} {4}x{5} bg={6} ink={7} ratio={8} -> {9}" -f `
                        $theme, $page, $name, $popup.Class, $w, $h, $contrast.Background, $contrast.Ink, $contrast.Ratio, $path)
                    if ($contrast.Ratio -lt 4.5) {
                        $failures.Add("$theme/$page/${name}: contrast ratio $($contrast.Ratio) < 4.5 (bg=$($contrast.Background) ink=$($contrast.Ink))")
                    }
                } finally { $gfx.Dispose(); $bmp.Dispose() }

                if ($SaveMain) {
                    $mr = New-Object P4SyncDropdownWin+RECT
                    [void][P4SyncDropdownWin]::GetWindowRect($handle, [ref]$mr)
                    $mw = $mr.Right - $mr.Left; $mh = $mr.Bottom - $mr.Top
                    $mbmp = New-Object System.Drawing.Bitmap($mw, $mh)
                    $mg = [System.Drawing.Graphics]::FromImage($mbmp)
                    try {
                        $hdc = $mg.GetHdc()
                        try { [void][P4SyncDropdownWin]::PrintWindow($handle, $hdc, 2) }
                        finally { $mg.ReleaseHdc($hdc) }
                        $mpath = Join-Path $outFull ("window-{0}-{1}-{2}.png" -f $theme.ToLowerInvariant(), $page, $slug)
                        $mbmp.Save($mpath, [System.Drawing.Imaging.ImageFormat]::Png)
                        Write-Output ("captured main {0}/{1}: {2}x{3} -> {4}" -f $theme, $page, $mw, $mh, $mpath)
                    } finally { $mg.Dispose(); $mbmp.Dispose() }
                }
                $expand.Collapse()
                Start-Sleep -Milliseconds 150
            }
        } finally {
            if (-not $proc.HasExited) {
                $proc.CloseMainWindow() | Out-Null
                Start-Sleep -Milliseconds 400
                if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
            }
            Start-Sleep -Milliseconds 300
        }
    }
}

if ($failures.Count -gt 0) {
    foreach ($f in $failures) { Write-Output ("FAIL: " + $f) }
    exit 1
}
Write-Output "ALL DROPDOWNS READABLE (contrast >= 4.5:1)"
exit 0
