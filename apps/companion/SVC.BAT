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
echo [M-SVC] service started
title "Companion Service"
notify /t:3 "background service running"
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
