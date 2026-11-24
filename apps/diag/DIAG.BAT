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
rem  Output report: DIAG.TXT on the app drive (overwritten per run).
rem =====================================================================
setlocal
if "%P4_APP_FEATURES%"=="1" gosub %APPROOT%/APPLIB.BAT::probe
set REPORT=%APPDRIVE%/DIAG.TXT
echo [M-DIAG]
title "P4 Diagnostics"
echo P4 Diagnostics report > %REPORT%
echo ===================== >> %REPORT%

rem %TUICOLS%/%TUIROWS% are the shell-published layout grid; the 80x25
rem fallback keeps the app runnable outside the shell launcher.
if "%TUICOLS%"=="" set TUICOLS=80
if "%TUIROWS%"=="" set TUIROWS=25
rem The eight menu lines are authored at rows 7..14 of the 80x25 grid. Step
rem them down by the extra panel rows on a tall grid; the step is 1 on the
rem classic 25-row panel so the authored landscape layout is unchanged.
set /a EX=%TUIROWS%-25 > NUL
set /a DS=1+%EX%/8 > NUL
set /a R7=7 > NUL
set /a R8=7+%DS% > NUL
set /a R9=7+2*%DS% > NUL
set /a R10=7+3*%DS% > NUL
set /a R11=7+4*%DS% > NUL
set /a R12=7+5*%DS% > NUL
set /a R13=7+6*%DS% > NUL
set /a R14=7+7*%DS% > NUL

:menu
draw clear screen
gosub %APPROOT%/APPLIB.BAT::bar 1 3 double "P4 DIAGNOSTICS ★"
draw text 3 2 "System health, capabilities and frame-rate benchmark"
gosub %APPROOT%/APPLIB.BAT::panel 5 15 single "Diagnostics"
draw text 5 %R7% "Full report"
draw text 5 %R8% "Identity          software / board / build"
draw text 5 %R9% "System            heap + PSRAM, CPU, tasks, clock"
draw text 5 %R10% "Storage           SD card, disk, recycle bin"
draw text 5 %R11% "Network           Wi-Fi, DNS, sockets"
draw text 5 %R12% "Devices           audio, LED, USB, battery"
draw text 5 %R13% "Benchmark         graphical frame rate + jitter"
draw text 5 %R14% "Capabilities      feature checklist"
gosub %APPROOT%/APPLIB.BAT::footer 21 3 single
rem The footer prompt rides inside its bar: row 22 on the 25-row grid, the
rem matching row (TUIROWS-3) on a taller panel.
set /a FY=%TUIROWS%-3 > NUL
draw text 3 %FY% "Select a diagnostic; Exit returns to the shell"
echo [M-DIAG-MENU]
list "P4 Diagnostics" "Full report" "Identity" "System" "Storage" "Network" "Devices" "Benchmark" "Capabilities" "Exit"
on %errorlevel%+1 goto :do_full,:do_identity,:do_system,:do_storage,:do_network,:do_devices,:do_bench,:do_cap,:do_exit
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
echo (mem reports internal heap, DMA and the PSRAM pool)
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
sync status
wifi status >> %REPORT%
ipconfig >> %REPORT%
netstat >> %REPORT%
sync status >> %REPORT%
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
gosub :probe "markdown" "markdown %REPORT%"
gosub :probe "network" "wifi status"
gosub :probe "usb" "usb status"
gosub :probe "sync" "sync status"
gosub :probe "events" "net status"
rem ---- v1.6 richer-language capability tour ----------------------------
rem [M-RICH-DIAG] runs the new batch surfaces through the SAME interpreter;
rem the hardware-only surfaces are listed as capabilities above, not faked.
echo [M-RICH-DIAG] v1.6 language capabilities
map /set RICHCAP str str
map /set RICHCAP map map
map /set RICHCAP func func
for /K %%k in (RICHCAP) do echo [M-RICH-DIAG-K] %%k=%%l
set DSTEP=diagnostics
str /count DSTEP i DC
str /pad DSTEP 14 DP .
func :diagsum 100 58
echo [M-RICH-DIAG-F] total=%RESULT%
echo [M-RICH-DIAG-S] i=%DC% pad=%DP%
echo capabilities checked
return

rem %1 = feature label (quoted), %2 = command line (quoted). Runs it, records
rem the resulting errorlevel to the report. The command is executed live.
:diagsum
return %1+%2

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
rem The menu drew into the TUI grid; close it before opening the gfx canvas
rem (gfx init refuses while TUI mode is active). The menu redraws on return.
draw hold off
draw close
gfx init 320 200
if errorlevel 1 goto no_bench
set bx=20
set by=20
set bdx=4
set bdy=3
set n=0
gfx stats reset
while n LSS 150 do call :bench_frame
screenshot %APPDRIVE%/DIAG_BENCH.BMP
echo --- frame pacing (%n% frames) ---
gfx stats
gfx stats >> %REPORT%
gfx close
return
:no_bench
echo gfx unavailable in this context
return

:bench_frame
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
