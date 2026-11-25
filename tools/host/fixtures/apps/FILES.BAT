@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem HOST-SAFE EXTRACT of FILES.BAT's [M-RICH-*] block. The real app also uses
rem hardware verbs (reported as SKIP by tools/host_apps_gate.py); this fixture
rem runs ONLY the additive batch-language surfaces on the REAL interpreter.
echo [M-RICH-FILES] host language extract
echo [M-DOS-FILES] screen=FILES-TOOLS panels=3 head="FILE TOOLS" theme=ice bars=0 table=N spark=0 status=Y
set W=files
str /rev W RV
str /count W e RC
str /pad W 10 RP -
map /set RCFG host p4
map /set RCFG port 80
for /K %%k in (RCFG) do echo [M-RICH-FILES-K] %%k=%%l
map /values RCFG VV
func :richsum 20 22
echo [M-RICH-FILES-F] result=%RESULT%
echo [M-RICH-FILES-S] rev=%RV% count=%RC% pad=%RP% vals=%VV[0]%,%VV[1]%
rem ---- HOST CELL DASHBOARD: real draw verbs, one tui_frame core ----
rem draw verbs execute in the host batch runner against the ONE tui_frame cell core
draw box 1 1 80 3 double 11 1 "FILE TOOLS"
draw run 3 2 "FILES-TOOLS / FILE TOOLS LIVE" /fg 15 /bg 1
draw box 2 4 38 10 single 11 1 "TREE"
draw run 4 6 "FILES / TREE" /fg 15 /bg 1
draw box 40 4 37 10 double 11 1 "DETAILS"
draw run 42 6 "FILES-TOOLS / DETAILS" /fg 15 /bg 1
draw run 42 8 "OPS=66 HOST=OK" /fg 15 /bg 1
draw box 2 15 76 8 single 11 1 "OPS"
draw run 4 17 "FILES-TOOLS / FILES READY" /fg 15 /bg 1
draw run 44 17 "F1 HELP  F2 NEXT  F9 EXIT" /fg 15 /bg 1
rem ---- END HOST CELL DASHBOARD ----
trap /on errorlevel :richerr
trap /raise 4 "apps probe"
echo [M-RICH-FILES] handler skipped WRONG
:richafter
trap /clear
echo /raw [M-HOST-BATCH-DONE]
exit /b 0

:richerr
echo [M-RICH-FILES-T] code=%TRAP_CODE% msg=%TRAP_MSG%
goto :richafter

:richsum
return %1+%2
