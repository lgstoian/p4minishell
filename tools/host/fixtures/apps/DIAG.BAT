@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem HOST-SAFE EXTRACT of DIAG.BAT's [M-RICH-*] block. The real app also uses
rem hardware verbs (reported as SKIP by tools/host_apps_gate.py); this fixture
rem runs ONLY the additive batch-language surfaces on the REAL interpreter.
echo [M-RICH-DIAG] host language extract
echo [M-DOS-DIAG] screen=DIAG-HEALTH panels=4 head="P4 DIAGNOSTICS" theme=green bars=0 table=Y spark=1 status=Y
set W=diagnostics
str /rev W RV
str /count W i RC
str /pad W 10 RP -
map /set RCFG host p4
map /set RCFG port 80
for /K %%k in (RCFG) do echo [M-RICH-DIAG-K] %%k=%%l
map /values RCFG VV
func :richsum 20 22
echo [M-RICH-DIAG-F] result=%RESULT%
echo [M-RICH-DIAG-S] rev=%RV% count=%RC% pad=%RP% vals=%VV[0]%,%VV[1]%
rem ---- HOST CELL DASHBOARD: real draw verbs, one tui_frame core ----
rem draw verbs execute in the host batch runner against the ONE tui_frame cell core
draw box 1 1 80 3 double 10 0 "P4 DIAGNOSTICS"
draw run 3 2 "DIAG-HEALTH / P4 DIAGNOSTICS LIVE" /fg 14 /bg 0
draw box 2 4 38 9 single 10 0 "HEALTH"
draw run 4 6 "DIAG / HEALTH" /fg 14 /bg 0
draw box 40 4 37 9 double 10 0 "COUNTERS"
draw run 42 6 "DIAG-HEALTH / COUNTERS" /fg 14 /bg 0
draw table 42 8 10 0 "ID|NAME|VAL" "01|DIAG|35" "02|LOG|63"
draw bar 42 11 7 64 # . 14 0
draw bar 51 11 7 83 # . 14 0
draw bar 60 11 7 27 # . 14 0
draw box 2 15 76 8 single 10 0 "LOG"
draw run 4 17 "DIAG-HEALTH / DIAG OK" /fg 14 /bg 0
draw run 44 17 "F1 HELP  F2 NEXT  F9 EXIT" /fg 14 /bg 0
draw bar 4 19 32 63 # . 14 0
draw bar 42 19 32 35 # . 14 0
rem ---- END HOST CELL DASHBOARD ----
trap /on errorlevel :richerr
trap /raise 4 "apps probe"
echo [M-RICH-DIAG] handler skipped WRONG
:richafter
trap /clear
echo /raw [M-HOST-BATCH-DONE]
exit /b 0

:richerr
echo [M-RICH-DIAG-T] code=%TRAP_CODE% msg=%TRAP_MSG%
goto :richafter

:richsum
return %1+%2
