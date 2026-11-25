@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem HOST-SAFE EXTRACT of CHAT.BAT's [M-RICH-*] block. The real app also uses
rem hardware verbs (reported as SKIP by tools/host_apps_gate.py); this fixture
rem runs ONLY the additive batch-language surfaces on the REAL interpreter.
echo [M-RICH-CHAT] host language extract
echo [M-DOS-CHAT] screen=CHAT-INBOX panels=3 head="P4 CHAT" theme=ice bars=2 table=N spark=0 status=Y
set W=chat
str /rev W RV
str /count W a RC
str /pad W 10 RP -
map /set RCFG host p4
map /set RCFG port 80
for /K %%k in (RCFG) do echo [M-RICH-CHAT-K] %%k=%%l
map /values RCFG VV
func :richsum 20 22
echo [M-RICH-CHAT-F] result=%RESULT%
echo [M-RICH-CHAT-S] rev=%RV% count=%RC% pad=%RP% vals=%VV[0]%,%VV[1]%
rem ---- HOST CELL DASHBOARD: real draw verbs, one tui_frame core ----
rem draw verbs execute in the host batch runner against the ONE tui_frame cell core
draw box 1 1 80 3 double 11 1 "P4 CHAT"
draw run 3 2 "CHAT-INBOX / P4 CHAT LIVE" /fg 15 /bg 1
draw box 2 4 36 12 single 11 1 "INBOX"
draw run 4 6 "CHAT / INBOX" /fg 15 /bg 1
draw box 40 4 39 12 double 11 1 "THREAD"
draw run 42 6 "CHAT-INBOX / THREAD" /fg 15 /bg 1
draw run 42 8 "PEERS=79 HOST=OK" /fg 15 /bg 1
draw bar 4 8 32 79 # . 15 1
draw box 2 15 76 8 single 11 1 "PEERS"
draw run 4 17 "CHAT-INBOX / CHAT ONLINE" /fg 15 /bg 1
draw run 44 17 "F1 HELP  F2 NEXT  F9 EXIT" /fg 15 /bg 1
rem ---- END HOST CELL DASHBOARD ----
trap /on errorlevel :richerr
trap /raise 4 "apps probe"
echo [M-RICH-CHAT] handler skipped WRONG
:richafter
trap /clear
echo /raw [M-HOST-BATCH-DONE]
exit /b 0

:richerr
echo [M-RICH-CHAT-T] code=%TRAP_CODE% msg=%TRAP_MSG%
goto :richafter

:richsum
return %1+%2
