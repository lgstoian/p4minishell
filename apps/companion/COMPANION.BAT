@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem =====================================================================
rem  COMPANION.BAT - entry point for the P4 Companion.
rem  Enhanced TUI with fullscreen, draw boxes, and status.
rem =====================================================================
setlocal
if "%P4_APP_FEATURES%"=="1" gosub %APPROOT%/APPLIB.BAT::probe
title "P4 Companion"
gosub %APPROOT%/LIB.BAT::load_settings
gosub :rich_companion
goto :main_menu

rem ---- v1.6 richer-language showcase ------------------------------------
rem [M-RICH-COMPANION] exercises map /set + for /K, str /pad and func/return
rem through the real interpreter; no menu state is changed.
:rich_companion
echo [M-RICH-COMPANION] menu language tour
map /set CENTRY system SYS
map /set CENTRY files FILES
for /K %%k in (CENTRY) do echo [M-RICH-COMPANION-K] %%k=%%l
set CW=companion
str /pad CW 12 CP .
func :csplit 20 4
echo [M-RICH-COMPANION-F] value=%RESULT%
echo [M-RICH-COMPANION-S] pad=%CP%
return

:csplit
return %1/%2

:main_menu
draw clear screen
rem Layout: the shell publishes the grid (80x25 landscape, 80x97 portrait).
rem The Main Menu panel takes the extra rows; the hint bar stays on the
rem bottom rows (20..22) and its text inside it (row 22).
gosub %APPROOT%/APPLIB.BAT::bar 1 3 double "P4 COMPANION ★"
draw text 22 2 "Pure-batch helper for P4MiniShell (%APPDRIVE%)" 14
gosub %APPROOT%/APPLIB.BAT::panel 4 15 single "Main Menu"
if "%TUIROWS%"=="" set TUIROWS=25
rem Spread the five menu lines down a tall panel; the step is 1 on the
rem classic 25-row grid, keeping rows 8..12 exactly where they were.
set /a EX=%TUIROWS%-25 > NUL
set /a MS=1+%EX%/4 > NUL
set /a R6=6 > NUL
set /a R8=8 > NUL
set /a R9=8+%MS% > NUL
set /a R10=8+2*%MS% > NUL
set /a R11=8+3*%MS% > NUL
set /a R12=8+4*%MS% > NUL
draw text 5 %R6% "What would you like to do?"
draw text 5 %R8% "Live System  - dashboard & status"
draw text 5 %R9% "File Tools   - browse, view, notes"
draw text 5 %R10% "Network      - wifi, ping, fetch"
draw text 5 %R11% "Fun / Demos  - games, calc, RGB"
draw text 5 %R12% "Settings     - brightness, volume"
gosub %APPROOT%/APPLIB.BAT::footer 20 3 single
if "%TUIROWS%"=="" set TUIROWS=25
set /a FY=%TUIROWS%-3 > NUL
draw text 5 %FY% "Use list to select, ESC to cancel"
draw run 55 2 "[M-RICH-COMPANION]" /fg 11 /bg 0
echo [C-MENU]
list "P4 Companion" "Live System" "File Tools" "Network Tools" "Fun / Demos" "Settings" "Exit"
on %errorlevel%+1 goto :opt_sys,:opt_files,:opt_net,:opt_fun,:opt_settings,:opt_exit
goto main_menu

:opt_sys
gosub %APPROOT%/SYS.BAT::main
goto main_menu
:opt_files
gosub %APPROOT%/FILES.BAT::main
goto main_menu
:opt_net
gosub %APPROOT%/NET.BAT::main
goto main_menu
:opt_fun
gosub %APPROOT%/FUN.BAT::main
goto main_menu
:opt_settings
gosub %APPROOT%/SET.BAT::main
goto main_menu
:opt_exit
echo [C-EXIT]
draw clear screen
rem The goodbye panel keeps its authored size and centres itself on the live
rem grid (rows 8..12 / text 10 on the classic 25-row grid).
if "%TUIROWS%"=="" set TUIROWS=25
set /a GY=%TUIROWS%/2-4 > NUL
set /a GT=%GY%+2 > NUL
gosub %APPROOT%/APPLIB.BAT::bar %GY% 5 double "Goodbye"
draw text 27 %GT% "Thanks for using P4 Companion!" 14
delay 1000
draw clear screen
exit /b 0
