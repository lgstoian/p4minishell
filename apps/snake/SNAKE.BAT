@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem =====================================================================
rem  SNAKE.BAT - TUI snake on packed coordinates (v = y*64+x). The body
rem  lives in the S[] array; `for /L` seeds the slots, `for /A` iterates
rem  the prefix (index to %%k, live value to %%l) for drawing, food
rem  collision and the shift, and each slot is a gosub routine so `%1`
rem  re-expands per call (the fresh-pipeline-per-call idiom). Segments
rem  stay opaque packed numbers: split (q=y, t=x) happens only for wall
rem  checks and drawing.
rem  Markers: [M-SNAKE] / [M-SNAKE-EAT] / [M-SNAKE-DONE].
rem  Foreground-only: draw refuses in background jobs (see :nogfx).
rem  Grid: x 3..42, y 4..21 maps to packed max 21*64+42=1386. Sentinel 6435
rem  (99,99) fills the spare slots and never matches a live head; guarded
rem  draws keep it off-screen.
rem =====================================================================
setlocal
echo [M-SNAKE]
title "Snake"
draw hold on
if errorlevel 1 goto nogfx
draw box 1 2 44 22 double SNAKE
if errorlevel 1 goto nogfx
set hh=596
set dir=1
set dc=d
set len=3
set score=0
rem Seed all twelve slots with the guaranteed-miss sentinel, then lay the
rem 3-cell start: `for /L` precomputes the index (%%i -> n) so set S[n]=
rem writes plain numbered slots.
for /L %%i in (0,1,11) do set S[%%i]=6435
set S[0]=596
set S[1]=595
set S[2]=594
goto newfood

:newfood
set /a fx=%RANDOM% / 820 + 3
set /a fy=%RANDOM% / 1821 + 4
set /a nh=%fy% * 64 + %fx%
set col=0
rem Body check: for /A hands the live segment value to %%l, so no dynamic
rem name lookups are needed. Every slot is either a segment or the 6435
rem sentinel (which can never equal a real cell).
for /A %%k in (S) do if %nh%==%%l set col=1
if %col%==1 goto newfood
goto frame

:frame
draw clear
draw box 1 2 44 22 double SNAKE
draw text 48 4 Score %score%
draw text 48 6 Length %len%
draw text 48 8 wasd move
draw text 48 9 Enter on serial
draw text 48 10 q quit
draw text 48 12 1s per step
draw text %fx% %fy% *
rem Draw the body once per frame: head gets the @, the rest get O.
for /A %%k in (S) do if %%k LSS %len% call :drawseg %%k %%l
draw refresh
if %dir% EQU 0 set dc=w
if %dir% EQU 1 set dc=d
if %dir% EQU 2 set dc=s
if %dir% EQU 3 set dc=a
choice /C:wasdq /N /T:%dc%,1 Move?
if errorlevel 5 goto quit
if errorlevel 4 goto go_d
if errorlevel 3 goto go_s
if errorlevel 2 goto go_a
goto go_w
:go_d
if %dir% EQU 3 goto move
set dir=1
goto move
:go_a
if %dir% EQU 1 goto move
set dir=3
goto move
:go_s
if %dir% EQU 0 goto move
set dir=2
goto move
:go_w
if %dir% EQU 2 goto move
set dir=0
goto move

:move
if %dir% EQU 0 set /a nh=%hh% - 64
if %dir% EQU 1 set /a nh=%hh% + 1
if %dir% EQU 2 set /a nh=%hh% + 64
if %dir% EQU 3 set /a nh=%hh% - 1
set /a q=%nh% / 64
set /a t=%nh% - %q% * 64
if %q% LSS 4 goto dead
if %q% GTR 21 goto dead
if %t% LSS 3 goto dead
if %t% GTR 42 goto dead
set col=0
for /A %%k in (S) do if %nh%==%%l set col=1
if %col%==1 goto dead
set /a q=%fy% * 64 + %fx%
if %q%==%nh% goto eat
rem Shift the body down one slot (S[k]=S[k-1]) so the new head lands in
rem S[0]. `for /A` visits slots ascending and the previous slot's OLD value
rem is carried in %prev%, so no slot overwrites data it still needs.
set prev=%nh%
for /A %%k in (S) do call :shiftseg %%k %%l
set hh=%nh%
goto frame

:shiftseg
rem %1 = index text, %2 = that slot's current value
set S[%1]=%prev%
set prev=%2
return

:eat
set /a score+=10
set /a len+=1
echo [M-SNAKE-EAT] %score%
if %len% GEQ 12 goto win
tone 880 80
goto newfood

:drawseg
rem %1 = index, %2 = packed segment value (y*64+x)
set /a q=%2 / 64
set /a t=%2 - %q% * 64
if %1 EQU 0 draw text %t% %q% @
if not %1 EQU 0 draw text %t% %q% O
return

:dead
draw text 48 13 GAME OVER
draw text 48 14 Score %score%
draw refresh
screenshot SNAKE.BMP
echo [M-SNAKE-DONE] score=%score% len=%len%
goto end

:win
draw text 48 13 YOU WIN
draw text 48 14 Score %score%
draw refresh
screenshot SNAKE.BMP
echo [M-SNAKE-DONE] score=%score% len=%len% WIN
goto end

:quit
draw text 48 13 QUIT
draw text 48 14 Score %score%
draw refresh
echo [M-SNAKE-DONE] score=%score% len=%len% QUIT
goto end

:nogfx
echo SNAKE needs the foreground display (no draw in background jobs)
goto end

:end
draw hold off
draw close
endlocal