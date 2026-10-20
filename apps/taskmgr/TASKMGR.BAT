@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem =====================================================================
rem  TASKMGR.BAT - batch task manager for P4MiniShell.
rem  Drives the pool verbs (start/taskkill/suspend/resume/nice), the
rem  service registry, and the tmgr_* events through the existing batch
rem  surfaces only (list/menu/dialog/form/set /p, tui boxes): no new
rem  modal, no native code. Every action is also a plain verb, so the
rem  suite drives it over serial exactly like a user.
rem  Markers: [M-TMGR] / [M-TMGR-KILL] / [M-TMGR-SUSP] / [M-TMGR-RES] /
rem  [M-TMGR-NICE] / [M-TMGR-SVC] / [M-TMGR-EVENTS] / [M-TMGR-SAVED] /
rem  [M-TMGR-BACK].
rem =====================================================================

:main
if "%P4_APP_FEATURES%"=="1" gosub APPLIB.BAT::probe
title "Task Manager"
echo [M-TMGR]
tui fullscreen on
:refresh
draw clear screen
draw box 1 1 80 3 double "TASK MANAGER"
draw text 5 5 "Jobs, services and events. Pick an action:"
start
ps /O:C
service list
list "Tasks" "Refresh view" "Kill job" "Suspend job" "Resume job" "Job priority" "Services" "Events" "Save snapshot" "Back"
on %errorlevel%+1 goto :do_refresh,:tm_kill,:tm_susp,:tm_res,:tm_nice,:tm_svc,:tm_events,:tm_save,:tm_back
goto :tm_back

:do_refresh
goto :refresh

:tm_kill
echo [M-TMGR-KILL]
set /p TMJ=Job to kill (bg0..):
taskkill %TMJ%
dialog "Kill" "Stop requested (see transcript for the result)." "OK"
goto :refresh

:tm_susp
echo [M-TMGR-SUSP]
set /p TMJ=Job to suspend (bg0..):
suspend %TMJ%
dialog "Suspend" "Freeze requested (taskkill still applies on resume)." "OK"
goto :refresh

:tm_res
echo [M-TMGR-RES]
set /p TMJ=Job to resume (bg0..):
resume %TMJ%
dialog "Resume" "Thaw requested (see transcript for the result)." "OK"
goto :refresh

:tm_nice
echo [M-TMGR-NICE]
set /p TMJ=Job for priority (bg0..):
set /p TMP=Priority 1..3 (or +n/-n):
nice %TMJ% %TMP%
dialog "Priority" "Priority change requested (see transcript)." "OK"
goto :refresh

:tm_svc
echo [M-TMGR-SVC]
service list
list "Services" "Start service" "Stop service" "Back"
on %errorlevel%+1 goto :tm_svc,:svc_start,:svc_stop,:svc_back
goto :svc_back

:svc_start
set /p TMN=Service name to start:
service start %TMN%
dialog "Services" "Start requested (restart policy still applies)." "OK"
goto :tm_svc

:svc_stop
set /p TMN=Service name to stop:
service stop %TMN%
dialog "Services" "Stopped (admin stop: no restart)." "OK"
goto :tm_svc

:svc_back
goto :refresh

:tm_events
echo [M-TMGR-EVENTS]
dialog "Events" "tmgr_exit fires per job exit; tmgr_svc_restart per supervisor restart; tmgr_pool_full per refused launch. Wait on them with: wait <name>" "OK"
goto :refresh

:tm_save
start /b > TMGR_SNAP.TXT
ps /b >> TMGR_SNAP.TXT
service list >> TMGR_SNAP.TXT
echo [M-TMGR-SAVED]
dialog "Saved" "Snapshot saved to TMGR_SNAP.TXT" "OK"
goto :refresh

:tm_back
echo [M-TMGR-BACK]
tui fullscreen off
draw clear screen
goto :eof
