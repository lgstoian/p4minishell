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

rem ---- apply saved settings that can be set at launch ------------------
:load_settings
appconfig companion get brightness > comp_bright.txt
if errorlevel 1 goto :done
for /f "tokens=*" %%v in (comp_bright.txt) do brightness %%v
:done
del /p comp_bright.txt
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
