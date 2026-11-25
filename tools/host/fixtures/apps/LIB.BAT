@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem HOST-SAFE EXTRACT of LIB.BAT's [M-RICH-*] block. The real app also uses
rem hardware verbs (reported as SKIP by tools/host_apps_gate.py); this fixture
rem runs ONLY the additive batch-language surfaces on the REAL interpreter.
echo [M-RICH-LIB-COMPANION] host language extract
echo [M-DOS-LIB-COMPANION] screen=LIB-COMPANION panels=2 head="COMPANION LIB" theme=ice bars=0 table=N spark=0 status=Y
set W=companion
str /rev W RV
str /count W a RC
str /pad W 10 RP -
map /set RCFG host p4
map /set RCFG port 80
for /K %%k in (RCFG) do echo [M-RICH-LIB-COMPANION-K] %%k=%%l
map /values RCFG VV
func :richsum 20 22
echo [M-RICH-LIB-COMPANION-F] result=%RESULT%
echo [M-RICH-LIB-COMPANION-S] rev=%RV% count=%RC% pad=%RP% vals=%VV[0]%,%VV[1]%
trap /on errorlevel :richerr
trap /raise 4 "apps probe"
echo [M-RICH-LIB-COMPANION] handler skipped WRONG
:richafter
trap /clear
echo /raw [M-HOST-BATCH-DONE]
exit /b 0

:richerr
echo [M-RICH-LIB-COMPANION-T] code=%TRAP_CODE% msg=%TRAP_MSG%
goto :richafter

:richsum
return %1+%2
