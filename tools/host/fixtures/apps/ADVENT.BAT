@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem HOST-SAFE EXTRACT of ADVENT.BAT's [M-RICH-*] block. The real app also uses
rem hardware verbs (reported as SKIP by tools/host_apps_gate.py); this fixture
rem runs ONLY the additive batch-language surfaces on the REAL interpreter.
echo [M-RICH-ADV] host language extract
echo [M-DOS-ADVENT] screen=ADVENT-MAP panels=3 head="COURTYARD" theme=amber bars=1 table=N spark=0 status=Y
set W=lantern
str /rev W RV
str /count W n RC
str /pad W 10 RP -
map /set RCFG host p4
map /set RCFG port 80
for /K %%k in (RCFG) do echo [M-RICH-ADV-K] %%k=%%l
map /values RCFG VV
func :richsum 20 22
echo [M-RICH-ADV-F] result=%RESULT%
echo [M-RICH-ADV-S] rev=%RV% count=%RC% pad=%RP% vals=%VV[0]%,%VV[1]%
rem ---- HOST CELL DASHBOARD: real draw verbs, one tui_frame core ----
rem draw verbs execute in the host batch runner against the ONE tui_frame cell core
draw box 1 1 80 3 double 14 0 "COURTYARD"
draw run 3 2 "ADVENT-MAP / COURTYARD LIVE" /fg 12 /bg 0
draw box 2 4 36 9 single 14 0 "MAP"
draw run 4 6 "ADVENT / MAP" /fg 12 /bg 0
draw box 40 4 39 9 double 14 0 "INVENTORY"
draw run 42 6 "ADVENT-MAP / INVENTORY" /fg 12 /bg 0
draw run 42 8 "EXITS=28 HOST=OK" /fg 12 /bg 0
draw bar 4 8 32 28 # . 12 0
draw box 2 15 76 8 single 14 0 "EXITS"
draw run 4 17 "ADVENT-MAP / COURTYARD LIVE" /fg 12 /bg 0
draw run 44 17 "F1 HELP  F2 NEXT  F9 EXIT" /fg 12 /bg 0
rem ---- END HOST CELL DASHBOARD ----
trap /on errorlevel :richerr
trap /raise 4 "apps probe"
echo [M-RICH-ADV] handler skipped WRONG
:richafter
trap /clear
echo /raw [M-HOST-BATCH-DONE]
exit /b 0

:richerr
echo [M-RICH-ADV-T] code=%TRAP_CODE% msg=%TRAP_MSG%
goto :richafter

:richsum
return %1+%2
