@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem HOST-SAFE EXTRACT of PICS.BAT's [M-RICH-*] block. The real app also uses
rem hardware verbs (reported as SKIP by tools/host_apps_gate.py); this fixture
rem runs ONLY the additive batch-language surfaces on the REAL interpreter.
echo [M-RICH-PICS] host language extract
echo [M-DOS-PICS] screen=PICS-VIEWER panels=2 head="PICS" theme=green bars=0 table=N spark=0 status=Y
set W=picture
str /rev W RV
str /count W c RC
str /pad W 10 RP -
map /set RCFG host p4
map /set RCFG port 80
for /K %%k in (RCFG) do echo [M-RICH-PICS-K] %%k=%%l
map /values RCFG VV
func :richsum 20 22
echo [M-RICH-PICS-F] result=%RESULT%
echo [M-RICH-PICS-S] rev=%RV% count=%RC% pad=%RP% vals=%VV[0]%,%VV[1]%
rem ---- HOST CELL DASHBOARD: real draw verbs, one tui_frame core ----
rem draw verbs execute in the host batch runner against the ONE tui_frame cell core
draw box 1 1 80 3 double 10 0 "PICS"
draw run 3 2 "PICS-VIEWER / PICS LIVE" /fg 14 /bg 0
draw box 2 4 40 9 single 10 0 "VIEWER"
draw run 4 6 "PICS / VIEWER" /fg 14 /bg 0
draw box 40 4 35 9 double 10 0 "THUMBS"
draw run 42 6 "PICS-VIEWER / THUMBS" /fg 14 /bg 0
draw run 42 8 "META=42 HOST=OK" /fg 14 /bg 0
draw box 2 15 76 8 single 10 0 "STATUS"
draw run 4 17 "PICS READY / PICS-VIEWER" /fg 14 /bg 0
rem ---- END HOST CELL DASHBOARD ----
trap /on errorlevel :richerr
trap /raise 4 "apps probe"
echo [M-RICH-PICS] handler skipped WRONG
:richafter
trap /clear
echo /raw [M-HOST-BATCH-DONE]
exit /b 0

:richerr
echo [M-RICH-PICS-T] code=%TRAP_CODE% msg=%TRAP_MSG%
goto :richafter

:richsum
return %1+%2
