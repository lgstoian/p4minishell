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

:main_menu
draw clear screen
rem Layout: the shell publishes the grid (80x25 landscape, 80x97 portrait).
rem The Main Menu panel takes the extra rows; the hint bar stays on the
rem bottom rows (20..22) and its text inside it (row 22).
gosub %APPROOT%/APPLIB.BAT::bar 1 3 double "P4 COMPANION ★"
draw text 22 2 "Pure-batch helper for P4MiniShell (%APPDRIVE%)" 14
gosub %APPROOT%/APPLIB.BAT::panel 4 15 single "Main Menu"
draw text 5 6 "What would you like to do?"
draw text 5 8 "Live System  - dashboard & status"
draw text 5 9 "File Tools   - browse, view, notes"
draw text 5 10 "Network      - wifi, ping, fetch"
draw text 5 11 "Fun / Demos  - games, calc, RGB"
draw text 5 12 "Settings     - brightness, volume"
gosub %APPROOT%/APPLIB.BAT::footer 20 3 single
if "%TUIROWS%"=="" set TUIROWS=25
set /a FY=%TUIROWS%-3 > NUL
draw text 5 %FY% "Use list to select, ESC to cancel"
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
