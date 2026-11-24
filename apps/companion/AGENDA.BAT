@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem =====================================================================
rem  AGENDA.BAT - agenda job: print today's and the next alarm, notify.
rem  Safe to run in the foreground (Services - Run agenda now) or in the
rem  background by scheduling it: alarm add DATE TIME /run:AGENDA.BAT
rem  (see command.md for the exact alarm add syntax).
rem  Markers: [M-AGENDA] / [M-AGENDA-DONE].
rem =====================================================================
echo [M-AGENDA] 
if "%P4_APP_FEATURES%"=="1" gosub %APPROOT%/APPLIB.BAT::probe
title "Agenda"
cal today
cal next
notify /t:3 "agenda checked"
echo [M-AGENDA-DONE]
rem ---- v1.6 richer-language showcase -----------------------------------
rem [M-RICH-AGENDA] headless str/map/forK/func tour; alarms are not changed.
echo [M-RICH-AGENDA] agenda language tour
set AW=agenda
str /rev AW WA
str /pad AW 8 AP .
map /set AGENDA next cal
for /K %%k in (AGENDA) do echo [M-RICH-AGENDA-K] %%k=%%l
func :agendanext 2 3
echo [M-RICH-AGENDA-F] slots=%RESULT%
echo [M-RICH-AGENDA-S] rev=%WA% pad=%AP%
goto :eof

:agendanext
return %1+%2
