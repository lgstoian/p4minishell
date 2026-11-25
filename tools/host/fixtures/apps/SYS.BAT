@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem HOST-SAFE EXTRACT of SYS.BAT's [M-RICH-*] block. The real app also uses
rem hardware verbs (reported as SKIP by tools/host_apps_gate.py); this fixture
rem runs ONLY the additive batch-language surfaces on the REAL interpreter.
echo [M-RICH-SYS] host language extract
echo [M-DOS-SYS] screen=SYS-DASHBOARD panels=4 head="SYSTEM DASHBOARD" theme=green bars=2 table=Y spark=1 status=Y
set W=sysinfo
str /rev W RV
str /count W s RC
str /pad W 10 RP -
map /set RCFG host p4
map /set RCFG port 80
for /K %%k in (RCFG) do echo [M-RICH-SYS-K] %%k=%%l
map /values RCFG VV
func :richsum 20 22
echo [M-RICH-SYS-F] result=%RESULT%
echo [M-RICH-SYS-S] rev=%RV% count=%RC% pad=%RP% vals=%VV[0]%,%VV[1]%
rem ---- HOST CELL DASHBOARD: real draw verbs, one tui_frame core ----
rem draw verbs execute in the host batch runner against the ONE tui_frame cell core
draw box 1 1 80 3 double 10 0 "SYSTEM DASHBOARD"
draw run 3 2 "SYS-DASHBOARD / SYSTEM DASHBOARD LIVE" /fg 14 /bg 0
draw box 2 4 40 12 single 10 0 "CPU"
draw run 4 6 "SYS / CPU" /fg 14 /bg 0
draw box 40 4 35 12 double 10 0 "MEMORY"
draw run 42 6 "SYS-DASHBOARD / MEMORY" /fg 14 /bg 0
draw table 42 8 10 0 "ID|NAME|VAL" "01|SYS|32" "02|TASKS|53"
draw bar 4 8 36 32 # . 14 0
draw bar 42 11 7 66 # . 14 0
draw bar 51 11 7 85 # . 14 0
draw bar 60 11 7 29 # . 14 0
draw box 2 15 76 8 single 10 0 "TASKS"
draw run 4 17 "SYS-DASHBOARD / SYSTEM NOMINAL" /fg 14 /bg 0
draw run 44 17 "F1 HELP  F2 NEXT  F9 EXIT" /fg 14 /bg 0
draw bar 4 19 32 53 # . 14 0
draw bar 42 19 32 32 # . 14 0
rem ---- END HOST CELL DASHBOARD ----
trap /on errorlevel :richerr
trap /raise 4 "apps probe"
echo [M-RICH-SYS] handler skipped WRONG
:richafter
trap /clear
echo /raw [M-HOST-BATCH-DONE]
exit /b 0

:richerr
echo [M-RICH-SYS-T] code=%TRAP_CODE% msg=%TRAP_MSG%
goto :richafter

:richsum
return %1+%2
