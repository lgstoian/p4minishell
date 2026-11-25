@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem HOST-SAFE EXTRACT of MOOD.BAT's [M-RICH-*] block. The real app also uses
rem hardware verbs (reported as SKIP by tools/host_apps_gate.py); this fixture
rem runs ONLY the additive batch-language surfaces on the REAL interpreter.
echo [M-RICH-MOOD] host language extract
echo [M-DOS-MOOD] screen=MOOD-LIVE panels=3 head="MOOD" theme=green bars=2 table=N spark=1 status=Y
set W=mood
str /rev W RV
str /count W o RC
str /pad W 10 RP -
map /set RCFG host p4
map /set RCFG port 80
for /K %%k in (RCFG) do echo [M-RICH-MOOD-K] %%k=%%l
map /values RCFG VV
func :richsum 20 22
echo [M-RICH-MOOD-F] result=%RESULT%
echo [M-RICH-MOOD-S] rev=%RV% count=%RC% pad=%RP% vals=%VV[0]%,%VV[1]%
rem ---- HOST CELL DASHBOARD: real draw verbs, one tui_frame core ----
rem draw verbs execute in the host batch runner against the ONE tui_frame cell core
draw box 1 1 80 3 double 10 0 "MOOD"
draw run 3 2 "MOOD-LIVE / MOOD LIVE" /fg 14 /bg 0
draw box 2 4 40 11 single 10 0 "MOOD TREND"
draw run 4 6 "MOOD / MOOD TREND" /fg 14 /bg 0
draw box 40 4 35 11 double 10 0 "HEAP"
draw run 42 6 "MOOD-LIVE / HEAP" /fg 14 /bg 0
draw run 42 8 "SAMPLES=62 HOST=OK" /fg 14 /bg 0
draw bar 4 8 36 62 # . 14 0
draw bar 42 11 7 42 # . 14 0
draw bar 51 11 7 61 # . 14 0
draw bar 60 11 7 80 # . 14 0
draw box 2 15 76 8 single 10 0 "SAMPLES"
draw run 4 17 "MOOD-LIVE / MOOD LIVE" /fg 14 /bg 0
draw run 44 17 "F1 HELP  F2 NEXT  F9 EXIT" /fg 14 /bg 0
rem ---- END HOST CELL DASHBOARD ----
trap /on errorlevel :richerr
trap /raise 4 "apps probe"
echo [M-RICH-MOOD] handler skipped WRONG
:richafter
trap /clear
echo /raw [M-HOST-BATCH-DONE]
exit /b 0

:richerr
echo [M-RICH-MOOD-T] code=%TRAP_CODE% msg=%TRAP_MSG%
goto :richafter

:richsum
return %1+%2
