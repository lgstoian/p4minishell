@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem HOST-SAFE EXTRACT of ADVENT.BAT's [M-RICH-*] block. The real app also uses
rem hardware verbs (reported as SKIP by tools/host_apps_gate.py); this fixture
rem runs ONLY the additive batch-language surfaces on the REAL interpreter.
echo [M-RICH-ADV] host language extract
set W=lantern
str /rev W RV
str /count W n RC
str /pad W 10 RP -
map /set RCFG host p4
map /set RCFG port 80
for /K %%k in (RCFG) do echo [M-RICH-ADV-K] %%k=%%l
map /values RCFG VV
func :richsum 20 22
echo [M-RICH-ADV-F] result=%RESULT%
echo [M-RICH-ADV-S] rev=%RV% count=%RC% pad=%RP% vals=%VV[0]%,%VV[1]%
trap /on errorlevel :richerr
trap /raise 4 "apps probe"
echo [M-RICH-ADV] handler skipped WRONG
:richafter
trap /clear
echo /raw [M-HOST-BATCH-DONE]
exit /b 0

:richerr
echo [M-RICH-ADV-T] code=%TRAP_CODE% msg=%TRAP_MSG%
goto :richafter

:richsum
return %1+%2
