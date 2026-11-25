@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem HOST-SAFE EXTRACT of ELITE.BAT's [M-RICH-*] block. The real app also uses
rem hardware verbs (reported as SKIP by tools/host_apps_gate.py); this fixture
rem runs ONLY the additive batch-language surfaces on the REAL interpreter.
echo [M-RICH-ELITE] host language extract
echo [M-DOS-ELITE] screen=ELITE-MARKET panels=3 head="ELITE" theme=amber bars=2 table=Y spark=0 status=Y
set W=trader
str /rev W RV
str /count W r RC
str /pad W 10 RP -
map /set RCFG host p4
map /set RCFG port 80
for /K %%k in (RCFG) do echo [M-RICH-ELITE-K] %%k=%%l
map /values RCFG VV
func :richsum 20 22
echo [M-RICH-ELITE-F] result=%RESULT%
echo [M-RICH-ELITE-S] rev=%RV% count=%RC% pad=%RP% vals=%VV[0]%,%VV[1]%
rem ---- HOST CELL DASHBOARD: real draw verbs, one tui_frame core ----
rem draw verbs execute in the host batch runner against the ONE tui_frame cell core
draw box 1 1 80 3 double 14 0 "ELITE"
draw run 3 2 "ELITE-MARKET / ELITE LIVE" /fg 12 /bg 0
draw box 2 4 40 10 single 14 0 "MARKET"
draw run 4 6 "ELITE / MARKET" /fg 12 /bg 0
draw box 40 4 35 10 double 14 0 "ORDERS"
draw run 42 6 "ELITE-MARKET / ORDERS" /fg 12 /bg 0
draw table 42 8 14 0 "ID|NAME|VAL" "01|ELITE|52" "02|WATCH|21"
draw bar 4 8 36 52 # . 12 0
draw box 2 15 76 8 single 14 0 "WATCH"
draw run 4 17 "ELITE-MARKET / ELITE TRADING" /fg 12 /bg 0
draw run 44 17 "F1 HELP  F2 NEXT  F9 EXIT" /fg 12 /bg 0
rem ---- END HOST CELL DASHBOARD ----
trap /on errorlevel :richerr
trap /raise 4 "apps probe"
echo [M-RICH-ELITE] handler skipped WRONG
:richafter
trap /clear
echo /raw [M-HOST-BATCH-DONE]
exit /b 0

:richerr
echo [M-RICH-ELITE-T] code=%TRAP_CODE% msg=%TRAP_MSG%
goto :richafter

:richsum
return %1+%2
