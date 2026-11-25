@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem HOST-SAFE EXTRACT of SHOWCASE.BAT's [M-RICH-SHOWCASE] block. The real app
rem also uses draw/TUI/modal/webapp/gfx verbs (reported as SKIP by the gate);
rem this fixture runs ONLY the additive batch-language surfaces on the REAL
rem interpreter. No visual verb is faked here.
echo [M-RICH-SHOWCASE] str+map+forK+func
echo [M-DOS-SHOWCASE] screen=SHOWCASE-MASTER panels=6 head="DOS SHOWCASE" theme=ice bars=3 table=Y spark=2 status=Y
set SCW=showcase
str /rev SCW SR
str /count SCW c SCC
str /pad SCW 10 SP -
map /set SCFG batch 1
map /set SCFG tui 2
for /K %k in (SCFG) do echo [M-RICH-SHOWCASE-K] %k=%l
func :scsum 6 8
echo [M-RICH-SHOWCASE-F] total=%RESULT%
echo [M-RICH-SHOWCASE-S] rev=%SR% count=%SCC% pad=%SP%
rem ---- HOST CELL DASHBOARD: real draw verbs, one tui_frame core ----
rem draw verbs execute in the host batch runner against the ONE tui_frame cell core
draw box 1 1 80 3 double 11 1 "DOS SHOWCASE"
draw run 3 2 "SHOWCASE-MASTER / DOS SHOWCASE LIVE" /fg 15 /bg 1
draw box 2 4 38 9 single 11 1 "PANEL A"
draw run 4 6 "SHOWCASE / PANEL A" /fg 15 /bg 1
draw box 40 4 37 9 double 11 1 "PANEL B"
draw run 42 6 "SHOWCASE-MASTER / PANEL B" /fg 15 /bg 1
draw box 4 6 32 7 rounded 11 1 "PANEL D"
draw box 43 6 32 7 rounded 11 1 "PANEL E"
draw table 42 8 11 1 "ID|NAME|VAL" "01|SHOWCASE|56" "02|PANEL C|56"
draw bar 4 8 34 56 # . 15 1
draw bar 42 11 7 46 # . 15 1
draw bar 51 11 7 65 # . 15 1
draw bar 60 11 7 84 # . 15 1
draw bar 69 11 7 28 # . 15 1
draw box 2 15 76 8 single 11 1 "PANEL C"
draw run 4 17 "SHOWCASE-MASTER / MASTER LIVE" /fg 15 /bg 1
draw run 44 17 "F1 HELP  F2 NEXT  F9 EXIT" /fg 15 /bg 1
draw bar 4 19 32 56 # . 15 1
draw bar 42 19 32 56 # . 15 1
draw bar 6 9 26 56 # . 15 1
draw bar 45 9 26 56 # . 15 1
rem ---- END HOST CELL DASHBOARD ----
echo /raw [M-HOST-BATCH-DONE]
exit /b 0

:scsum
return %1+%2