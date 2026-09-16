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
cal today
cal next
notify "agenda checked" /t:3
echo [M-AGENDA-DONE]
