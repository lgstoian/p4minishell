@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem HOST-SAFE EXTRACT of CHAT.BAT's [M-RICH-*] block. The real app also uses
rem hardware verbs (reported as SKIP by tools/host_apps_gate.py); this fixture
rem runs ONLY the additive batch-language surfaces on the REAL interpreter.
echo [M-RICH-CHAT] host language extract
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
