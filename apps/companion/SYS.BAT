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
list "Dashboard" "Refresh" "View details" "Save snapshot" "Services" "Packages" "Back"
if errorlevel 255 goto :back
if errorlevel 5 goto :back
if errorlevel 4 goto :packages
if errorlevel 3 goto :services
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

rem ---- Background services: run the service loop, inspect the alarm
rem      scheduler, and fire the agenda job. ----
:services
echo [M-SYS-SVC]
list "Services" "Status" "Start background service" "Stop service" "List alarms" "Run agenda now" "Back"
if errorlevel 255 goto :svc_back
if errorlevel 5 goto :svc_back
if errorlevel 4 goto :svc_agenda
if errorlevel 3 goto :svc_list
if errorlevel 2 goto :svc_stop
if errorlevel 1 goto :svc_start
goto :svc_status

:svc_status
echo [M-SVC-STATUS]
tasks
alarm status
cal next
dialog "Services" "Tasks, alarm status and the next alarm are shown above." "OK"
goto :services

:svc_start
echo [M-SVC-START]
start SVC
dialog "Services" "Background service started on bg0. Use Stop service or taskkill bg0." "OK"
goto :services

:svc_stop
echo [M-SVC-STOP]
taskkill bg0
dialog "Services" "Stop requested for bg0." "OK"
goto :services

:svc_list
echo [M-SVC-LIST]
alarm list
cal next
dialog "Services" "Alarms listed above. Schedule more with: alarm add <date> <time> /run:AGENDA.BAT" "OK"
goto :services

:svc_agenda
echo [M-SVC-AGENDA]
call AGENDA.BAT
dialog "Services" "Agenda job finished." "OK"
goto :services

:svc_back
goto :refresh

rem ---- Packaged SD apps: list/verify/install/remove via `pkg`. Bundles are
rem      pushed to PKGS/<APP>/ by apps/push_pkgs.py. ----
:packages
echo [M-SYS-PKG]
pkg list
list "Packages" "Verify all installed" "App info" "Install from PKGS" "Remove app" "Back"
if errorlevel 255 goto :refresh
if errorlevel 4 goto :refresh
if errorlevel 3 goto :pkg_remove
if errorlevel 2 goto :pkg_install
if errorlevel 1 goto :pkg_info
goto :pkg_verify

:pkg_verify
echo [M-PKG-VERIFY]
pkg check
dialog "Packages" "Every installed app was CRC-checked above." "OK"
goto :packages

:pkg_info
echo [M-PKG-INFO]
set /p PA=App name:
pkg info %PA%
pkg verify %PA%
dialog "Packages" "Info and verification shown above." "OK"
goto :packages

:pkg_install
echo [M-PKG-INSTALL]
set /p PA=App name to install:
pkg install %PA%
dialog "Packages" "Install finished (see transcript for CRCs)." "OK"
goto :packages

:pkg_remove
echo [M-PKG-REMOVE]
set /p PA=App name to remove:
pkg remove %PA%
dialog "Packages" "Remove finished (see transcript)." "OK"
goto :packages
