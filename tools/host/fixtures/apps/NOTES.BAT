@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem HOST-SAFE EXTRACT of NOTES.BAT's [M-RICH-*] block. The real app also uses
rem hardware verbs (reported as SKIP by tools/host_apps_gate.py); this fixture
rem runs ONLY the additive batch-language surfaces on the REAL interpreter.
echo [M-RICH-NOTE] host language extract
echo [M-DOS-NOTES] screen=NOTES-ZETTEL panels=2 head="ZETTELKASTEN" theme=amber bars=0 table=N spark=0 status=Y
set W=zettel
str /rev W RV
str /count W t RC
str /pad W 10 RP -
map /set RCFG host p4
map /set RCFG port 80
for /K %%k in (RCFG) do echo [M-RICH-NOTE-K] %%k=%%l
map /values RCFG VV
func :richsum 20 22
echo [M-RICH-NOTE-F] result=%RESULT%
echo [M-RICH-NOTE-S] rev=%RV% count=%RC% pad=%RP% vals=%VV[0]%,%VV[1]%
rem ---- HOST CELL DASHBOARD: real draw verbs, one tui_frame core ----
rem draw verbs execute in the host batch runner against the ONE tui_frame cell core
draw box 1 1 80 3 double 14 0 "ZETTELKASTEN"
draw run 3 2 "NOTES-ZETTEL / ZETTELKASTEN LIVE" /fg 12 /bg 0
draw box 2 4 36 11 single 14 0 "CARDS"
draw run 4 6 "NOTES / CARDS" /fg 12 /bg 0
draw box 40 4 39 11 double 14 0 "TAGS"
draw run 42 6 "NOTES-ZETTEL / TAGS" /fg 12 /bg 0
draw run 42 8 "RECENT=69 HOST=OK" /fg 12 /bg 0
draw box 2 15 76 8 single 14 0 "STATUS"
draw run 4 17 "ZETTEL READY / NOTES-ZETTEL" /fg 12 /bg 0
rem ---- END HOST CELL DASHBOARD ----
trap /on errorlevel :richerr
trap /raise 4 "apps probe"
echo [M-RICH-NOTE] handler skipped WRONG
:richafter
trap /clear
echo /raw [M-HOST-BATCH-DONE]
exit /b 0

:richerr
echo [M-RICH-NOTE-T] code=%TRAP_CODE% msg=%TRAP_MSG%
goto :richafter

:richsum
return %1+%2
