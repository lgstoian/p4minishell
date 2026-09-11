@echo off
rem =====================================================================
rem  SVC.BAT - background service demo. Launch with `start SVC`; stop with
rem  `taskkill bg0`. Runs on the background worker, so it uses only
rem  headless-safe verbs (no draw/modal/choice/pause/gfx - those refuse in
rem  background jobs). Shows how a batch app can keep time-based work,
rem  calendar checks and DB maintenance running while the shell stays live.
rem  Markers: [M-SVC] / [M-SVC-TICK] / [M-SVC] stopped.
rem =====================================================================
echo [M-SVC] service started
notify "background service running" /t:3
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
