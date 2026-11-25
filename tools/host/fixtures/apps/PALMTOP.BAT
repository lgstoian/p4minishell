@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem HOST-SAFE EXTRACT of PALMTOP.BAT's [M-RICH-*] block. The real app also uses
rem hardware verbs (reported as SKIP by tools/host_apps_gate.py); this fixture
rem runs ONLY the additive batch-language surfaces on the REAL interpreter.
echo [M-RICH-PALM] host language extract
echo [M-DOS-PALMTOP] screen=PALM-DESK panels=3 head="PALMTOP" theme=ice bars=0 table=Y spark=0 status=Y
set W=palmtop
str /rev W RV
str /count W p RC
str /pad W 10 RP -
map /set RCFG host p4
map /set RCFG port 80
for /K %%k in (RCFG) do echo [M-RICH-PALM-K] %%k=%%l
map /values RCFG VV
func :richsum 20 22
echo [M-RICH-PALM-F] result=%RESULT%
echo [M-RICH-PALM-S] rev=%RV% count=%RC% pad=%RP% vals=%VV[0]%,%VV[1]%
rem ---- HOST CELL DASHBOARD: real draw verbs, one tui_frame core ----
rem draw verbs execute in the host batch runner against the ONE tui_frame cell core
draw box 1 1 80 3 double 11 1 "PALMTOP"
draw run 3 2 "PALM-DESK / PALMTOP LIVE" /fg 15 /bg 1
draw box 2 4 38 12 single 11 1 "DESK"
draw run 4 6 "PALMTOP / DESK" /fg 15 /bg 1
draw box 40 4 37 12 double 11 1 "APPS"
draw run 42 6 "PALM-DESK / APPS" /fg 15 /bg 1
draw table 42 8 11 1 "ID|NAME|VAL" "01|PALMTOP|86" "02|CLOCK|79"
draw box 2 15 76 8 single 11 1 "CLOCK"
draw run 4 17 "PALM-DESK / PALMTOP READY" /fg 15 /bg 1
draw run 44 17 "F1 HELP  F2 NEXT  F9 EXIT" /fg 15 /bg 1
rem ---- END HOST CELL DASHBOARD ----
trap /on errorlevel :richerr
trap /raise 4 "apps probe"
echo [M-RICH-PALM] handler skipped WRONG
:richafter
trap /clear
echo /raw [M-HOST-BATCH-DONE]
exit /b 0

:richerr
echo [M-RICH-PALM-T] code=%TRAP_CODE% msg=%TRAP_MSG%
goto :richafter

:richsum
return %1+%2
