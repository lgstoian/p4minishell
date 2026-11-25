@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem HOST-SAFE EXTRACT of SET.BAT's [M-RICH-*] block. The real app also uses
rem hardware verbs (reported as SKIP by tools/host_apps_gate.py); this fixture
rem runs ONLY the additive batch-language surfaces on the REAL interpreter.
echo [M-RICH-SET] host language extract
echo [M-DOS-SET] screen=SET-PREFS panels=2 head="PREFERENCES" theme=ice bars=2 table=Y spark=0 status=Y
set W=settings
str /rev W RV
str /count W t RC
str /pad W 10 RP -
map /set RCFG host p4
map /set RCFG port 80
for /K %%k in (RCFG) do echo [M-RICH-SET-K] %%k=%%l
map /values RCFG VV
func :richsum 20 22
echo [M-RICH-SET-F] result=%RESULT%
echo [M-RICH-SET-S] rev=%RV% count=%RC% pad=%RP% vals=%VV[0]%,%VV[1]%
rem ---- HOST CELL DASHBOARD: real draw verbs, one tui_frame core ----
rem draw verbs execute in the host batch runner against the ONE tui_frame cell core
draw box 1 1 80 3 double 11 1 "PREFERENCES"
draw run 3 2 "SET-PREFS / PREFERENCES LIVE" /fg 15 /bg 1
draw box 2 4 36 12 single 11 1 "GENERAL"
draw run 4 6 "SET / GENERAL" /fg 15 /bg 1
draw box 40 4 39 12 double 11 1 "DISPLAY"
draw run 42 6 "SET-PREFS / DISPLAY" /fg 15 /bg 1
draw table 42 8 11 1 "ID|NAME|VAL" "01|SET|39" "02|KEYS|27"
draw bar 4 8 32 39 # . 15 1
draw box 2 15 76 8 single 11 1 "STATUS"
draw run 4 17 "PREFS SAVED / SET-PREFS" /fg 15 /bg 1
rem ---- END HOST CELL DASHBOARD ----
trap /on errorlevel :richerr
trap /raise 4 "apps probe"
echo [M-RICH-SET] handler skipped WRONG
:richafter
trap /clear
echo /raw [M-HOST-BATCH-DONE]
exit /b 0

:richerr
echo [M-RICH-SET-T] code=%TRAP_CODE% msg=%TRAP_MSG%
goto :richafter

:richsum
return %1+%2
