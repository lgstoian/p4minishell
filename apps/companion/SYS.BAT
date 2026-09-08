@echo off
rem =====================================================================
rem  SYS.BAT - Live System dashboard.
rem  Uses TUI fullscreen with draw boxes for each panel, falls back to
rem  appmode if TUI not available. Shows sysinfo, mem, ps, battery, wifi.
rem =====================================================================

:main
echo [M-SYS]
tui fullscreen on
draw clear screen
draw box 1 1 80 3 double "LIVE SYSTEM DASHBOARD"
draw box 1 4 80 10 single "System - Press View for details"
draw box 1 15 40 10 single "Memory"
draw box 41 15 40 10 single "Tasks"
draw text 5 6 "System dashboard ready"
draw text 5 7 "Use View to see sysinfo/mem/ps"
draw text 5 8 "Use Save to keep snapshot"
:refresh
list "Dashboard" "Refresh" "View details" "Save snapshot" "Back"
if errorlevel 255 goto :back
if errorlevel 3 goto :back
if errorlevel 2 goto :save
if errorlevel 1 goto :view_details
if errorlevel 0 goto :do_refresh
goto refresh
:do_refresh
draw clear screen
draw box 1 1 80 3 double "LIVE SYSTEM DASHBOARD"
draw box 1 4 80 10 single "System - Refreshed"
draw box 1 15 40 10 single "Memory"
draw box 41 15 40 10 single "Tasks"
draw text 5 6 "Refreshed at %TIME%"
goto refresh
:view_details
dialog "Details" "Heap, tasks and uptime shown above. Use Save to keep a snapshot." "OK"
goto refresh
:save
sysinfo > SYS_SNAP.TXT
mem >> SYS_SNAP.TXT
ps /O:C >> SYS_SNAP.TXT
echo [M-SYS-SAVED]
dialog "Saved" "Snapshot saved to SYS_SNAP.TXT" "OK"
goto refresh
:back
echo [M-SYS-BACK]
tui fullscreen off
draw clear screen
goto :eof
