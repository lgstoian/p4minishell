<#
.SYNOPSIS
    Capture real WPF P4Sync GUI window screenshots (mock mode, no board).

.DESCRIPTION
    Launches the shipped P4SyncGui.exe with --mock --page <key>, waits for the
    window, and copies the actual window rectangle to a PNG. These are real
    captures of the shipped app, not illustrations or a fake backend.

    Run from the repository root after `dotnet build apps/p4sync_gui/P4SyncGui.sln`.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools/p4sync/capture_gui_shots.ps1
#>
param(
    [string]$Exe = "apps\p4sync_gui\src\P4SyncGui\bin\Release\net10.0-windows\P4SyncGui.exe",
    [string]$OutDir = "docs\assets\p4sync",
    [string[]]$Pages = @("devices", "dashboard", "files", "screenshot", "pim", "apps", "shell", "plan", "settings")
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

# Primary action button per page; invoked through UI Automation so the captured
# window shows real bridge data rather than the initial empty state.
$ActionButtons = @{
    devices    = "Discover devices"
    dashboard  = "Refresh capability snapshot"
    screenshot = "Capture a device screenshot"
    files      = "Run a dry-run plan"
    pim        = "Build the full PIM image"
}

Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class P4SyncWindow {
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
}
"@

if (-not (Test-Path $Exe)) { throw "Build the GUI first: dotnet build apps/p4sync_gui/P4SyncGui.sln -c Release" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$exeFull = (Resolve-Path $Exe).Path
$outFull = (Resolve-Path $OutDir).Path

foreach ($page in $Pages) {
    $proc = Start-Process -FilePath $exeFull -ArgumentList @("--mock", "--page", $page) -PassThru
    try {
        $handle = [IntPtr]::Zero
        for ($i = 0; $i -lt 60; $i++) {
            Start-Sleep -Milliseconds 250
            $proc.Refresh()
            if ($proc.MainWindowHandle -ne [IntPtr]::Zero) { $handle = $proc.MainWindowHandle; break }
        }
        if ($handle -eq [IntPtr]::Zero) { throw "No window for page '$page'" }

        [void][P4SyncWindow]::ShowWindow($handle, 9)
        [void][P4SyncWindow]::SetForegroundWindow($handle)
        Start-Sleep -Milliseconds 1200

        if ($ActionButtons.ContainsKey($page)) {
            try {
                $root = [System.Windows.Automation.AutomationElement]::FromHandle($handle)
                $condName = New-Object System.Windows.Automation.PropertyCondition(
                    [System.Windows.Automation.AutomationElement]::NameProperty,
                    $ActionButtons[$page])
                $condType = New-Object System.Windows.Automation.PropertyCondition(
                    [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
                    [System.Windows.Automation.ControlType]::Button)
                $cond = New-Object System.Windows.Automation.AndCondition -ArgumentList @($condName, $condType)
                $btn = $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $cond)
                if ($null -ne $btn) {
                    $invoke = $btn.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)
                    $invoke.Invoke()
                    Write-Output ("  invoked '{0}' on {1}" -f $ActionButtons[$page], $page)
                    # The mock bridge needs time to answer before PrintWindow.
                    Start-Sleep -Milliseconds 2500
                } else {
                    Write-Output ("  (no '{0}' button found on {1})" -f $ActionButtons[$page], $page)
                }
            } catch {
                Write-Output ("  (UI Automation on {0} skipped: {1})" -f $page, $_.Exception.Message)
            }
        }

        $rect = New-Object P4SyncWindow+RECT
        if (-not [P4SyncWindow]::GetWindowRect($handle, [ref]$rect)) { throw "GetWindowRect failed for '$page'" }
        $w = $rect.Right - $rect.Left
        $h = $rect.Bottom - $rect.Top
        if ($w -lt 200 -or $h -lt 200) { throw "Implausible window size ${w}x${h} for '$page'" }

        # PrintWindow renders THIS window even when another app overlaps the
        # screen; CopyFromScreen captured the wrong app on a shared desktop.
        $bmp = New-Object System.Drawing.Bitmap($w, $h)
        $gfx = [System.Drawing.Graphics]::FromImage($bmp)
        try {
            $hdc = $gfx.GetHdc()
            try {
                if (-not [P4SyncWindow]::PrintWindow($handle, $hdc, 2)) {
                    throw "PrintWindow failed for '$page'"
                }
            } finally { $gfx.ReleaseHdc($hdc) }
            $path = Join-Path $outFull ("p4sync-" + $page + ".png")
            $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
            Write-Output ("captured {0} ({1}x{2}) -> {3}" -f $page, $w, $h, $path)
        } finally {
            $gfx.Dispose(); $bmp.Dispose()
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
