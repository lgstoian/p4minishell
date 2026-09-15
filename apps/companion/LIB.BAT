@echo off
rem =====================================================================
rem  LIB.BAT - shared routines for the P4 Companion.
rem  Every routine is reached with `call LIB.BAT::<routine> [args]`.
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
:tui_banner
draw clear screen
draw box 1 1 80 3 single
draw text 2 2 "P4 COMPANION - Pure-batch helper for P4MiniShell" 14
goto :eof

rem ---- tui header with title ------------------------------------------
:tui_header
rem %1 = title
draw clear screen
draw box 1 1 80 3 single
draw text 30 2 "%~1" 14
goto :eof

rem ---- thin rule ------------------------------------------------------
:hr
ansi 36m  -------------------------------------------------------------
goto :eof

rem ---- tui rule -------------------------------------------------------
:tui_hr
draw line 1 4 80 4
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
:status
rem %1 = left, %2 = right
draw text 1 25 "%~1" 7
draw text 60 25 "%~2" 7
goto :eof

rem ---- selftest: %~ modifiers -------------------------------------------
rem %1 = value to dissect. Exercises the `%~[fdpnx]N` expansion on the
rem live engine: dequote, name, extension, dir, full. Callable headless:
rem   call LIB.BAT::selftest_tilde "sd:/DIR/FILE.TXT"
:selftest_tilde
echo SELFTEST raw=[%1]
echo SELFTEST stripped=[%~1]
echo SELFTEST name=[%~n1] ext=[%~x1] dir=[%~p1] full=[%~f1]
goto :eof

rem ---- selftest: for /f command form ------------------------------------
rem Iterates command output instead of a file (no temp file needed).
rem Callable headless: call LIB.BAT::selftest_forf_cmd
:selftest_forf_cmd
for /f "tokens=*" %%v in ('echo hello-forf') do echo SELFTEST forf-cmd got=[%%v]
goto :eof

rem ---- selftest: defined/empty observation -------------------------------
rem Reports (not judges) the `set NAME=` + `if defined` semantics so the
rem board session can confirm cmd parity. Callable headless:
rem   call LIB.BAT::selftest_defined
:selftest_defined
set SELFTEST_X=
if defined SELFTEST_X echo SELFTEST defined-after-clear=1
if not defined SELFTEST_X echo SELFTEST defined-after-clear=0
set SELFTEST_X=1
if defined SELFTEST_X echo SELFTEST defined-after-set=1
set SELFTEST_X=
goto :eof

rem ---- selftest: all ------------------------------------------------------
rem Runs every selftest routine above. Callable headless:
rem   call LIB.BAT::selftest_all
:selftest_all
call LIB.BAT::selftest_tilde "sd:/DIR/FILE.TXT"
call LIB.BAT::selftest_forf_cmd
call LIB.BAT::selftest_defined
echo SELFTEST done
goto :eof
