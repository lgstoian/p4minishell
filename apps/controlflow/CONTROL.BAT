@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem =====================================================================
rem  CONTROL.BAT - batch control-flow tour: gosub / return / on.
rem
rem  A tiny, self-contained demo of the BASIC-flavoured verbs: gosub calls a
rem  local :label (with arguments) and returns; on <expr> goto|gosub dispatches
rem  on a computed index; a recursive gosub shows nesting. Every construct
rem  prints one line, so a transcript diff pins the exact execution order.
rem
rem  Markers: [M-CONTROL] / [M-CONTROL-DONE].
rem =====================================================================
setlocal
echo [M-CONTROL]
echo CONTROL: start
set n=2
call :banner
on %n% goto :one,:two,:three
echo CONTROL: on did not dispatch
goto :after

:one
echo CONTROL: branch one
goto :after

:two
echo CONTROL: branch two
goto :after

:three
echo CONTROL: branch three

:after
gosub :add 3 4
echo CONTROL: sum %sum%
gosub :wrap hi
echo CONTROL: wrapped %out%
on 1 gosub :tick,:tick
echo CONTROL: gosub dispatch done
gosub :count 3
echo [M-CONTROL-DONE]
goto :eof

:banner
echo CONTROL: banner
return

:add
set /a sum=%1+%2
echo CONTROL: add %1 %2
return 0

:wrap
set out=[%1]
return

:tick
echo CONTROL: tick
return

:count
if %1 LEQ 0 return
echo CONTROL: count %1
set /a next=%1-1
gosub :count %next%
return
