@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem =====================================================================
rem  LIB.BAT - shared routines for the P4 Companion.
rem  Every routine is reached with `call %APPROOT%/LIB.BAT::<routine> [args]`.
rem  Routine calls run in an isolated environment scope, so temporary
rem  variables here never leak into the caller.
rem =====================================================================

rem ---- banner ---------------------------------------------------------
:banner
ansi 1;36m ============================================================
ansi 1;36m    P4 COMPANION  -  a pure-batch helper for P4MiniShell
ansi 1;36m ============================================================
goto :eof

rem ---- tui banner (draw fullscreen box with title) --------------------
rem TUICOLS/TUIROWS are the shell-published layout grid (the 80x25 fallback
rem keeps a headless call working). Horizontal text stays on the authored
rem 80-column positions: the grid keeps 80 columns on every supported panel.
:tui_banner
if "%TUICOLS%"=="" set TUICOLS=80
draw clear screen
draw box 1 1 %TUICOLS% 3 single
draw text 2 2 "P4 COMPANION - Pure-batch helper for P4MiniShell" 14
goto :eof

rem ---- tui header with title ------------------------------------------
:tui_header
rem %1 = title
if "%TUICOLS%"=="" set TUICOLS=80
draw clear screen
draw box 1 1 %TUICOLS% 3 single
draw text 30 2 "%~1" 14
goto :eof

rem ---- thin rule ------------------------------------------------------
:hr
ansi 36m  -------------------------------------------------------------
goto :eof

rem ---- tui rule -------------------------------------------------------
:tui_hr
if "%TUICOLS%"=="" set TUICOLS=80
draw line 1 4 %TUICOLS% 4
goto :eof

rem ---- settings restore ----------------------------------------------
rem Firmware settings persist in CONFIG.SYS and are applied by boot
rem (brightness/volume/theme/font/header/...). The companion keeps no
rem shadow settings file, so there is nothing to re-apply here. The routine
rem stays for COMPANION.BAT compatibility.
:load_settings
goto :eof

rem ---- pause ----------------------------------------------------------
:wait
pause
goto :eof

rem ---- draw status bar ------------------------------------------------
rem %1 = left, %2 = right. Both ride the grid's bottom row, the right label
rem on the last 20 columns (col 60 on the 80-column grid).
:status
if "%TUICOLS%"=="" set TUICOLS=80
if "%TUIROWS%"=="" set TUIROWS=25
set /a SX=%TUICOLS%-20 > NUL
draw text 1 %TUIROWS% "%~1" 7
draw text %SX% %TUIROWS% "%~2" 7
goto :eof

rem ---- selftest: %~ modifiers -------------------------------------------
rem %1 = value to dissect. Exercises the `%~[fdpnx]N` expansion on the
rem live engine: dequote, name, extension, dir, full. Callable headless:
rem   call %APPROOT%/LIB.BAT::selftest_tilde "sd:/DIR/FILE.TXT"
:selftest_tilde
echo SELFTEST raw=[%1]
echo SELFTEST stripped=[%~1]
echo SELFTEST name=[%~n1] ext=[%~x1] dir=[%~p1] full=[%~f1]
goto :eof

rem ---- selftest: for /f command form ------------------------------------
rem Iterates command output instead of a file (no temp file needed).
rem Callable headless: call %APPROOT%/LIB.BAT::selftest_forf_cmd
:selftest_forf_cmd
for /f "eol= tokens=*" %%v in ('echo hello-forf') do echo SELFTEST forf-cmd got=[%%v]
goto :eof

rem ---- selftest: defined/empty observation -------------------------------
rem Reports (not judges) the `set NAME=` + `if defined` semantics so the
rem board session can confirm cmd parity. Callable headless:
rem   call %APPROOT%/LIB.BAT::selftest_defined
:selftest_defined
set SELFTEST_X=
if defined SELFTEST_X echo SELFTEST defined-after-clear=1
if not defined SELFTEST_X echo SELFTEST defined-after-clear=0
set SELFTEST_X=1
if defined SELFTEST_X echo SELFTEST defined-after-set=1
set SELFTEST_X=
goto :eof

rem ---- selftest: substring/replace + for /A ------------------------------
rem Exercises the `%VAR:~%` / `%VAR:old=new%` expansions and the associative
rem `for /A` binding (index in %%k, live slot value in the next letter).
rem Callable headless: call %APPROOT%/LIB.BAT::selftest_expansion
:selftest_expansion
set SELFTEST_WORD=cloudpilot
echo SELFTEST substr=[%SELFTEST_WORD:~0,4%]
echo SELFTEST replace=[%SELFTEST_WORD:pilot=P4%]
set SELFTEST_A[0]=alpha
set SELFTEST_A[1]=beta
for /A %%k in (SELFTEST_A) do echo SELFTEST fora %%k=%%l
set SELFTEST_A[0]=
set SELFTEST_A[1]=
set SELFTEST_WORD=
goto :eof

rem ---- selftest: all ------------------------------------------------------
rem Runs every selftest routine above. Callable headless:
rem   call LIB.BAT::selftest_all
rem ---- v1.6 richer-language showcase ------------------------------------
rem Callable headless: call LIB.BAT::rich
rem [M-RICH-LIB-COMPANION] proves str /rev|/count|/pad, map /set + for /K
rem and func/return publish %RESULT% through the ONE interpreter.
:rich
echo [M-RICH-LIB-COMPANION] str/map/forK/func
set LWORD=companion
str /rev LWORD LR
str /count LWORD n LN
str /pad LWORD 12 LP -
map /set LMAP lib LIB
map /set LMAP menu COMPANION
for /K %%k in (LMAP) do echo [M-RICH-LIB-COMPANION-K] %%k=%%l
func :libhalf 24
echo [M-RICH-LIB-COMPANION-F] half=%RESULT%
echo [M-RICH-LIB-COMPANION-S] rev=%LR% n=%LN% pad=%LP%
goto :eof

:libhalf
return %1/2

:selftest_all
rem Library calls keep an app's APPROOT, but a headless shell call has none:
rem the selftests are read-only, so SD is a safe fallback for the recursion.
if "%APPROOT%"=="" set APPROOT=sd:
call %APPROOT%/LIB.BAT::selftest_tilde "sd:/DIR/FILE.TXT"
call %APPROOT%/LIB.BAT::selftest_forf_cmd
call %APPROOT%/LIB.BAT::selftest_defined
call %APPROOT%/LIB.BAT::selftest_expansion
echo SELFTEST done
goto :eof
