@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem =====================================================================
rem  SVC.BAT - resident background service demo. Install once with
rem  `service install svc "SVC" /restart` (autostart persists across
rem  reboots in sd:/SERVICES.INI); stop with `service stop svc`.
rem  Killing it with `taskkill` counts as unexpected: the supervisor
rem  restarts it (that is the TSR contract - `service stop` is the off
rem  switch, not taskkill).
rem  Runs on a background worker, so it uses only headless-safe verbs (no
rem  draw/modal/choice/pause/gfx - those refuse in background jobs). Shows
rem  how a batch app can keep time-based work, calendar checks and DB
rem  maintenance running while the shell stays live.
rem  Markers: [M-SVC] / [M-SVC-TICK] / [M-SVC] stopped.
rem =====================================================================
echo [M-SVC]  service started
echo [M-DOS-SVC] screen=SVC-HEADLESS panels=0 head="NONE" theme=mono bars=0 table=N spark=0 status=N
title "Companion Service"
notify /t:3 "background service running"
rem ---- v1.6 richer-language showcase -----------------------------------
rem [M-RICH-SVC] one-shot headless str/map/forK/func tour; the tick loop is
rem unchanged and no display verb is used in this background service.
echo [M-RICH-SVC] headless language tour
set SW=service
str /rev SW WS
str /pad SW 9 SP .
map /set SVCJOBS tick 1
for /K %%k in (SVCJOBS) do echo [M-RICH-SVC-K] %%k=%%l
func :svcrate 20 3
echo [M-RICH-SVC-F] rate=%RESULT%
echo [M-RICH-SVC-S] rev=%WS% pad=%SP%
:loop
rem Each tick: report the next scheduled alarm and keep the record store
rem warm. Output lands in the shared transcript (bg-safe deferred writes).
cal next
echo [M-SVC-TICK] checked schedule at %TIME%
delay 20000
if errorlevel 1 goto stopped
goto loop
:stopped
echo [M-SVC] service stopped
goto :eof

:svcrate
return %1/%2
