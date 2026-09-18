@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem =====================================================================
rem  DIAG.BAT - P4 Diagnostics: a pure-batch system health, capability
rem  and performance console. Demonstrates the full batch surface at once:
rem  TUI (draw), modal selection (list), redirection, errorlevel branching,
rem  a gfx animation loop with firmware-measured frame pacing (gfx stats),
rem  screenshots, and a written report file.
rem
rem  Markers (used by tools/suites):
rem    [M-DIAG]                 app started
rem    [M-DIAG-SECTION] <name>  a section began
rem    [M-DIAG-DONE]            app finished
rem
rem  Output report: DIAG.TXT on the SD root (overwritten per run).
rem =====================================================================
setlocal
set REPORT=DIAG.TXT
echo [M-DIAG]
echo P4 Diagnostics report > %REPORT%
echo ===================== >> %REPORT%

:menu
draw clear screen
draw box 1 1 80 3 double "P4 DIAGNOSTICS"
draw text 3 2 "System health, capabilities and frame-rate benchmark"
draw box 1 5 80 15 single "Diagnostics"
draw text 5 7  "Full report"
draw text 5 8  "Identity          software / board / build"
draw text 5 9  "System            memory, CPU, tasks, clock"
draw text 5 10 "Storage           SD card, disk, recycle bin"
draw text 5 11 "Network           Wi-Fi, DNS, sockets"
draw text 5 12 "Devices           audio, LED, USB, battery"
draw text 5 13 "Benchmark         graphical frame rate + jitter"
draw text 5 14 "Capabilities      feature checklist"
draw box 1 21 80 3 single
draw text 3 22 "Select a diagnostic; Exit returns to the shell"
echo [M-DIAG-MENU]
list "P4 Diagnostics" "Full report" "Identity" "System" "Storage" "Network" "Devices" "Benchmark" "Capabilities" "Exit"
if errorlevel 255 goto menu
if errorlevel 8 goto do_exit
if errorlevel 7 goto do_cap
if errorlevel 6 goto do_bench
if errorlevel 5 goto do_devices
if errorlevel 4 goto do_network
if errorlevel 3 goto do_storage
if errorlevel 2 goto do_system
if errorlevel 1 goto do_identity
if errorlevel 0 goto do_full
goto menu

rem ---- dispatch wrappers (call the section, then return to the menu) ---
:do_identity
gosub :identity
goto menu
:do_system
gosub :system
goto menu
:do_storage
gosub :storage
goto menu
:do_network
gosub :network
goto menu
:do_devices
gosub :devices
goto menu
:do_cap
gosub :cap
goto menu
:do_bench
gosub :bench
goto menu
:do_full
gosub :identity
gosub :system
gosub :storage
gosub :network
gosub :devices
gosub :cap
gosub :bench
gosub :help
echo.
echo Full report written to %REPORT%
pause
goto menu

rem ---- Identity -------------------------------------------------------
:identity
echo [M-DIAG-SECTION] identity
echo.
echo ==== IDENTITY ==== >> %REPORT%
about
version
about >> %REPORT%
version >> %REPORT%
return

rem ---- System ---------------------------------------------------------
:system
echo [M-DIAG-SECTION] system
echo.
echo ==== SYSTEM ==== >> %REPORT%
mem
ps /O:C
date
time
timezone
mem >> %REPORT%
ps /O:C >> %REPORT%
date >> %REPORT%
time >> %REPORT%
timezone >> %REPORT%
return

rem ---- Storage --------------------------------------------------------
:storage
echo [M-DIAG-SECTION] storage
echo.
echo ==== STORAGE ==== >> %REPORT%
sd info
disk detail
chkdsk
trash info
sd info >> %REPORT%
disk detail >> %REPORT%
chkdsk >> %REPORT%
trash info >> %REPORT%
return

rem ---- Network --------------------------------------------------------
:network
echo [M-DIAG-SECTION] network
echo.
echo ==== NETWORK ==== >> %REPORT%
wifi status
ipconfig
netstat
wifi status >> %REPORT%
ipconfig >> %REPORT%
netstat >> %REPORT%
return

rem ---- Devices --------------------------------------------------------
:devices
echo [M-DIAG-SECTION] devices
echo.
echo ==== DEVICES ==== >> %REPORT%
audio
rgb status
usb status
power
i2c scan
audio >> %REPORT%
rgb status >> %REPORT%
usb status >> %REPORT%
power >> %REPORT%
i2c scan >> %REPORT%
return

rem ---- Capabilities ---------------------------------------------------
rem Runs representative read-only commands and records the errorlevel so the
rem report is a real availability matrix, not a claim.
:cap
echo [M-DIAG-SECTION] capabilities
echo.
echo ==== CAPABILITIES ==== >> %REPORT%
gosub :probe "filesystem" "dir /b"
gosub :probe "batch" "echo ok"
gosub :probe "tui" "tui status"
gosub :probe "gfx-canvas" "gfx status"
gosub :probe "gfx-stats" "gfx stats"
gosub :probe "tui-stats" "tui stats"
gosub :probe "database" "db list"
gosub :probe "alarms" "alarm status"
gosub :probe "archive" "archive list"
gosub :probe "crypt" "crypt status"
gosub :probe "csv" "csv rows"
gosub :probe "markdown" "markdown DIAG.TXT"
gosub :probe "network" "wifi status"
gosub :probe "usb" "usb status"
echo capabilities checked
return

rem %1 = feature label (quoted), %2 = command line (quoted). Runs it, records
rem the resulting errorlevel to the report. The command is executed live.
:probe
echo --- %~1 ---
%~2
echo %~1 errorlevel=%ERRORLEVEL% >> %REPORT%
return

rem ---- Benchmark ------------------------------------------------------
rem Animated canvas loop, then the REAL present timing the firmware measured
rem (frames/min/avg/max/jitter/dropped) via `gfx stats`. The screenshot runs
rem once after the loop (a capture inside the loop stalls it for seconds).
:bench
echo [M-DIAG-SECTION] benchmark
echo.
echo ==== BENCHMARK ==== >> %REPORT%
gfx init 320 200
if errorlevel 1 goto no_bench
set bx=20
set by=20
set bdx=4
set bdy=3
set n=0
gfx stats reset
:bench_loop
set /a n+=1
set /a bx+=%bdx%
set /a by+=%bdy%
if %bx% GEQ 300 set bdx=-4
if %bx% LEQ 20 set bdx=4
if %by% GEQ 180 set bdy=-3
if %by% LEQ 20 set bdy=3
gfx clear 0
gfx rect 0 0 320 200 1
gfx circle %bx% %by% 16 14 fill
gfx circle %bx% %by% 6 15 fill
gfx show
if %n% GEQ 150 goto bench_done
goto bench_loop
:bench_done
screenshot DIAG_BENCH.BMP
echo --- frame pacing (%n% frames) ---
gfx stats
gfx stats >> %REPORT%
gfx close
return
:no_bench
echo gfx unavailable in this context
return

:help
echo.
echo P4 Diagnostics:
echo   Full report   runs every section and writes %REPORT%.
echo   Benchmark     animated gfx loop + firmware-measured frame stats.
echo   Capabilities  probes representative commands and records errorlevels.
return

:do_exit
echo [M-DIAG-DONE]
draw clear screen
exit /b 0
