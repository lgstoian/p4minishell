@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem HOST-SAFE EXTRACT of SVC.BAT's [M-RICH-*] block. The real app also uses
rem hardware verbs (reported as SKIP by tools/host_apps_gate.py); this fixture
rem runs ONLY the additive batch-language surfaces on the REAL interpreter.
echo [M-RICH-SVC] host language extract
set W=service
str /rev W RV
str /count W e RC
str /pad W 10 RP -
map /set RCFG host p4
map /set RCFG port 80
for /K %%k in (RCFG) do echo [M-RICH-SVC-K] %%k=%%l
map /values RCFG VV
func :richsum 20 22
echo [M-RICH-SVC-F] result=%RESULT%
echo [M-RICH-SVC-S] rev=%RV% count=%RC% pad=%RP% vals=%VV[0]%,%VV[1]%
trap /on errorlevel :richerr
trap /raise 4 "apps probe"
echo [M-RICH-SVC] handler skipped WRONG
:richafter
trap /clear
echo /raw [M-HOST-BATCH-DONE]
exit /b 0

:richerr
echo [M-RICH-SVC-T] code=%TRAP_CODE% msg=%TRAP_MSG%
goto :richafter

:richsum
return %1+%2
