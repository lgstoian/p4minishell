@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem HOST-SAFE EXTRACT of NET.BAT's [M-RICH-*] block. The real app also uses
rem hardware verbs (reported as SKIP by tools/host_apps_gate.py); this fixture
rem runs ONLY the additive batch-language surfaces on the REAL interpreter.
echo [M-RICH-NET] host language extract
echo [M-DOS-NET] screen=NET-TOOLS panels=3 head="NETWORK TOOLS" theme=ice bars=2 table=Y spark=0 status=Y
set W=network
str /rev W RV
str /count W e RC
str /pad W 10 RP -
map /set RCFG host p4
map /set RCFG port 80
for /K %%k in (RCFG) do echo [M-RICH-NET-K] %%k=%%l
map /values RCFG VV
func :richsum 20 22
echo [M-RICH-NET-F] result=%RESULT%
echo [M-RICH-NET-S] rev=%RV% count=%RC% pad=%RP% vals=%VV[0]%,%VV[1]%
rem ---- HOST CELL DASHBOARD: real draw verbs, one tui_frame core ----
rem draw verbs execute in the host batch runner against the ONE tui_frame cell core
draw box 1 1 80 3 double 11 1 "NETWORK TOOLS"
draw run 3 2 "NET-TOOLS / NETWORK TOOLS LIVE" /fg 15 /bg 1
draw box 2 4 36 9 single 11 1 "LINKS"
draw run 4 6 "NET / LINKS" /fg 15 /bg 1
draw box 40 4 39 9 double 11 1 "PORTS"
draw run 42 6 "NET-TOOLS / PORTS" /fg 15 /bg 1
draw table 42 8 11 1 "ID|NAME|VAL" "01|NET|49" "02|ROUTES|82"
draw bar 4 8 32 49 # . 15 1
draw box 2 15 76 8 single 11 1 "ROUTES"
draw run 4 17 "NET-TOOLS / NETWORK UP" /fg 15 /bg 1
draw run 44 17 "F1 HELP  F2 NEXT  F9 EXIT" /fg 15 /bg 1
rem ---- END HOST CELL DASHBOARD ----
trap /on errorlevel :richerr
trap /raise 4 "apps probe"
echo [M-RICH-NET] handler skipped WRONG
:richafter
trap /clear
echo /raw [M-HOST-BATCH-DONE]
exit /b 0

:richerr
echo [M-RICH-NET-T] code=%TRAP_CODE% msg=%TRAP_MSG%
goto :richafter

:richsum
return %1+%2
