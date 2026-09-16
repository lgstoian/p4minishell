@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem =====================================================================
rem  COMPANION.BAT - entry point for the P4 Companion.
rem  Enhanced TUI with fullscreen, draw boxes, and status.
rem =====================================================================
setlocal
call LIB.BAT::load_settings

:main_menu
draw clear screen
draw box 1 1 80 3 double "P4 COMPANION"
draw text 22 2 "Pure-batch helper for P4MiniShell" 14
draw box 1 4 80 15 single "Main Menu"
draw text 5 6 "What would you like to do?"
draw text 5 8 "Live System  - dashboard & status"
draw text 5 9 "File Tools   - browse, view, notes"
draw text 5 10 "Network      - wifi, ping, fetch"
draw text 5 11 "Fun / Demos  - games, calc, RGB"
draw text 5 12 "Settings     - brightness, volume"
draw box 1 20 80 3 single
draw text 5 22 "Use list to select, ESC to cancel"
echo [C-MENU]
list "P4 Companion" "Live System" "File Tools" "Network Tools" "Fun / Demos" "Settings" "Exit"
if errorlevel 255 goto main_menu
if errorlevel 5 goto :opt_exit
if errorlevel 4 goto :opt_settings
if errorlevel 3 goto :opt_fun
if errorlevel 2 goto :opt_net
if errorlevel 1 goto :opt_files
if errorlevel 0 goto :opt_sys
goto main_menu

:opt_sys
call SYS.BAT::main
goto main_menu
:opt_files
call FILES.BAT::main
goto main_menu
:opt_net
call NET.BAT::main
goto main_menu
:opt_fun
call FUN.BAT::main
goto main_menu
:opt_settings
call SET.BAT::main
goto main_menu
:opt_exit
echo [C-EXIT]
draw clear screen
draw box 1 8 80 5 double "Goodbye"
draw text 27 10 "Thanks for using P4 Companion!" 14
delay 1000
draw clear screen
exit /b 0
