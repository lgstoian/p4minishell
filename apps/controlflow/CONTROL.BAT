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
title "Control Flow Tour"
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
gosub :post
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

rem ---- additive tour (post-authoring features) -------------------------
rem switch dispatch, a while loop, a single-line if/else group, for /A over
rem an array, substring/replace expansion and delayed !VAR!. Appended after
rem the original constructs; the six original lines keep their order.
:post
set sw=diag
switch %sw% setup:sw_setup diag:sw_diag /d:sw_help
echo CONTROL: switch fell through WRONG
goto :sw_done
:sw_setup
echo CONTROL: switch setup
goto :sw_done
:sw_diag
echo CONTROL: switch diag
goto :sw_done
:sw_help
echo CONTROL: switch help
:sw_done
set w=0
while w LSS 3 do set /a w=w+1
echo CONTROL: while %w%
if 2 GTR 1 (echo CONTROL: if-group taken) else (echo CONTROL: if-group WRONG)
set ARR[0]=alpha
set ARR[1]=beta
for /A %%k in (ARR) do echo CONTROL: fora %%k=%%l
set NAME=abcdefgh
echo CONTROL: substr %NAME:~2,3% replace %NAME:a=A%
setlocal enabledelayedexpansion
set live=now
echo CONTROL: delayed !live!
endlocal
gosub :dosparity
return

rem ---- DOS-parity tour (redirection + newer language features) --------
rem  One pinned line per construct so a transcript diff fixes the exact
rem  behaviour: glued echo separators, `> NUL`/`> CON` devices, `1>`/`2>`
rem  handle merge, `type` reading redirected stdin, `shift /n`, and an
rem  explicit `eol=` override of the cmd.exe `;`-comment default.
:dosparity
echo [M-CONTROL-PARITY]
echo.Parity: dot ok
echo/Parity: slash ok
echo(Parity: paren ok
echo:Parity: colon ok
echo.
echo CONTROL: discarded > NUL
echo CONTROL: nul-ok
echo CONTROL: console-echo > CON
echo CONTROL: handle-merge 2> _p4parity_h.txt
type < _p4parity_h.txt
del /p _p4parity_h.txt > NUL
write _p4parity_in.txt "CONTROL: stdin-type ok"
type < _p4parity_in.txt
del /p _p4parity_in.txt > NUL
gosub :parity_shift a b c d
write _p4parity_eol.txt ";kept"
for /f "eol= tokens=*" %%e in (_p4parity_eol.txt) do echo CONTROL: eol-keep %%e
del /p _p4parity_eol.txt > NUL
return

:parity_shift
rem `shift /n` slides `%n`..`%9` onward, leaving earlier slots in place.
shift /2
echo CONTROL: shift-2 %1-%2
return
