@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem HOST-SAFE EXTRACT of TCMD.BAT's [M-RICH-*] block. The real app also uses
rem hardware verbs (reported as SKIP by tools/host_apps_gate.py); this fixture
rem runs ONLY the additive batch-language surfaces on the REAL interpreter.
echo [M-RICH-TCMD] host language extract
echo [M-DOS-TCMD] screen=TCMD-DUALPANE panels=4 head="COMMANDER" theme=ice bars=0 table=N spark=0 status=Y
set W=commander
str /rev W RV
str /count W m RC
str /pad W 10 RP -
map /set RCFG host p4
map /set RCFG port 80
for /K %%k in (RCFG) do echo [M-RICH-TCMD-K] %%k=%%l
map /values RCFG VV
func :richsum 20 22
echo [M-RICH-TCMD-F] result=%RESULT%
echo [M-RICH-TCMD-S] rev=%RV% count=%RC% pad=%RP% vals=%VV[0]%,%VV[1]%
rem ---- HOST CELL DASHBOARD: real draw verbs, one tui_frame core ----
rem draw verbs execute in the host batch runner against the ONE tui_frame cell core
draw box 1 1 80 3 double 11 1 "COMMANDER"
draw run 3 2 "TCMD-DUALPANE / COMMANDER LIVE" /fg 15 /bg 1
draw box 2 4 38 10 single 11 1 "LEFT PANE"
draw run 4 6 "TCMD / LEFT PANE" /fg 15 /bg 1
draw box 40 4 37 10 double 11 1 "RIGHT PANE"
draw run 42 6 "TCMD-DUALPANE / RIGHT PANE" /fg 15 /bg 1
draw run 42 8 "TRANSFER=45 HOST=OK" /fg 15 /bg 1
draw box 2 15 76 8 single 11 1 "TRANSFER"
draw run 4 17 "TCMD-DUALPANE / DUAL PANE READY" /fg 15 /bg 1
draw run 44 17 "F1 HELP  F2 NEXT  F9 EXIT" /fg 15 /bg 1
draw bar 4 19 32 47 # . 15 1
draw bar 42 19 32 45 # . 15 1
rem ---- END HOST CELL DASHBOARD ----
trap /on errorlevel :richerr
trap /raise 4 "apps probe"
echo [M-RICH-TCMD] handler skipped WRONG
:richafter
trap /clear
echo /raw [M-HOST-BATCH-DONE]
exit /b 0

:richerr
echo [M-RICH-TCMD-T] code=%TRAP_CODE% msg=%TRAP_MSG%
goto :richafter

:richsum
return %1+%2
