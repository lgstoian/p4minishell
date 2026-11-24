@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem =====================================================================
rem  SNAKE.BAT - TUI snake on packed coordinates (v = y*128+x). The body
rem  lives in the S[] array; `for /L` seeds the slots, `for /A` iterates
rem  the prefix (index to %%k, live value to %%l) for drawing, food
rem  collision and the shift, and each slot is a gosub routine so `%1`
rem  re-expands per call (the fresh-pipeline-per-call idiom). Segments
rem  stay opaque packed numbers: split (q=y, t=x) happens only for wall
rem  checks and drawing.
rem  Markers: [M-SNAKE] / [M-SNAKE-EAT] / [M-SNAKE-DONE].
rem  Foreground-only: draw refuses in background jobs (see :nogfx).
rem  Grid: x 3..42, y 4..(TUIROWS-4) maps to a live packed max; sentinel
rem  (99,99) fills the spare slots and never matches a live head; guarded
rem  draws keep it off-screen.
rem =====================================================================
setlocal
if "%P4_APP_FEATURES%"=="1" gosub %APPROOT%/APPLIB.BAT::probe
echo [M-SNAKE]
title "Snake"
gosub :rich_snake
draw hold on
if errorlevel 1 goto nogfx
rem Full screen: reclaim the header and on-screen keyboard rows so the whole
rem 44x22 playfield and the score panel are visible (TCMD does the same).
draw fullscreen on
rem Layout: the shell publishes the grid (80x25 landscape, 80x97 portrait).
rem The frame spans the grid AND the playfield grows into the extra rows:
rem x 3..42, y 4..(TUIROWS-4). The packed coordinate stride and the food range
rem are derived from the live grid, so the board is not a small strip at the
rem top of a tall portrait panel (the v1.4.3 gallery bug).
if "%TUICOLS%"=="" set TUICOLS=80
if "%TUIROWS%"=="" set TUIROWS=25
set /a SH=%TUIROWS%-3 > NUL
if %SH% LSS 22 set SH=22
set /a YMAX=%TUIROWS%-4 > NUL
set /a YSPAN=%TUIROWS%-9 > NUL
if %YSPAN% LSS 18 set YSPAN=18
set /a MID=%TUIROWS%/2 > NUL
set /a MID2=%MID%+2 > NUL
set /a FOOT=%TUIROWS%-6 > NUL
set /a FOOT2=%FOOT%+2 > NUL
draw box 1 2 %TUICOLS% %SH% double "SNAKE ★"
if errorlevel 1 goto nogfx
set dir=1
set dc=d
set len=3
set score=0
rem Seed all twelve slots with the guaranteed-miss sentinel, then lay the
rem 3-cell start: `for /L` precomputes the index (%%i -> n) so set S[n]=
rem writes plain numbered slots.
for /L %%i in (0,1,11) do set S[%%i]=20000
set /a hh=%MID%*128+20 > NUL
set S[0]=%hh%
set /a S[1]=%hh%-1 > NUL
set /a S[2]=%hh%-2 > NUL
goto newfood

rem ---- v1.6 richer-language showcase -----------------------------------
rem [M-RICH-SNAKE]: func/return packs a board coordinate, str /pad formats
rem the HUD and map /incr tracks food. The 12-slot game state is untouched.
:rich_snake
echo [M-RICH-SNAKE] board language tour
func :snakepack 12 20
echo [M-RICH-SNAKE-F] coord=%RESULT%
set SHUD=score
str /pad SHUD 10 SH .
map /set FOODS eaten 0
map /incr FOODS eaten 3
for /K %%k in (FOODS) do echo [M-RICH-SNAKE-K] %%k=%%l
echo [M-RICH-SNAKE-S] pad=%SH%
return

:snakepack
return %1*128+%2

:newfood
set /a fx=%RANDOM% / 820 + 3
set /a fy=%RANDOM% %% %YSPAN% + 4
set /a nh=%fy% * 128 + %fx%
set col=0
rem Body check: for /A hands the live segment value to %%l, so no dynamic
rem name lookups are needed. Every slot is either a segment or the 20000
rem sentinel (which can never equal a real cell).
for /A %%k in (S) do if %nh%==%%l set col=1
if %col%==1 goto newfood
goto frame

:frame
draw clear
draw box 1 2 %TUICOLS% %SH% double "SNAKE ★"
draw text 48 4 "Score %score%"
draw run 48 2 "[M-RICH-SNAKE] v1.6" /fg 11 /bg 0
draw text 48 6 "Length %len%"
draw text 48 8 "wasd move"
draw text 48 9 "Enter on serial"
draw text 48 %FOOT% "p snap  q quit"
draw text 48 %FOOT2% "1s per step"
draw text %fx% %fy% *
rem Draw the body once per frame: head gets the @, the rest get O.
for /A %%k in (S) do if %%k LSS %len% call :drawseg %%k %%l
draw refresh
if %dir% EQU 0 set dc=w
if %dir% EQU 1 set dc=d
if %dir% EQU 2 set dc=s
if %dir% EQU 3 set dc=a
choice /C:wasdqp /N /T:%dc%,1 Move?
if errorlevel 6 goto snap
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
if %dir% EQU 0 set /a nh=%hh% - 128
if %dir% EQU 1 set /a nh=%hh% + 1
if %dir% EQU 2 set /a nh=%hh% + 128
if %dir% EQU 3 set /a nh=%hh% - 1
set /a q=%nh% / 128
set /a t=%nh% - %q% * 128
if %q% LSS 4 goto dead
if %q% GTR %YMAX% goto dead
if %t% LSS 3 goto dead
if %t% GTR 42 goto dead
set col=0
for /A %%k in (S) do if %nh%==%%l set col=1
if %col%==1 goto dead
set /a q=%fy% * 128 + %fx%
if %q%==%nh% goto eat
rem Shift the body down one slot (S[k]=S[k-1]) so the new head lands in
rem S[0]. `for /A` visits slots ascending and the previous slot's OLD value
rem is carried in %prev%, so no slot overwrites data it still needs.
set prev=%nh%
for /A %%k in (S) do call :shiftseg %%k %%l
set hh=%nh%
goto frame

:shiftseg
rem %1 = index text, %2 = that slot's current value.
rem Only the first `len` slots are the body: without this guard every one of
rem the 12 slots fills up after 12 moves and the snake renders as a 13-cell
rem streak even though the HUD says Length 3 (the v1.4.3 gallery bug).
if %1 GEQ %len% return
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
rem %1 = index, %2 = packed segment value (y*128+x)
set /a q=%2 / 128
set /a t=%2 - %q% * 128
if %1 EQU 0 draw text %t% %q% @
if not %1 EQU 0 draw text %t% %q% O
return

:snap
rem Documented snap key (p): save the live playfield for the docs gallery.
screenshot %APPDRIVE%/SNAKE.BMP
echo [M-SNAKE-SNAP]
goto frame

:dead
draw text 48 %MID% GAME OVER
draw text 48 %MID2% Score %score%
draw refresh
screenshot %APPDRIVE%/SNAKE.BMP
echo [M-SNAKE-DONE] score=%score% len=%len%
goto end

:win
draw text 48 %MID% YOU WIN
draw text 48 %MID2% Score %score%
draw refresh
screenshot %APPDRIVE%/SNAKE.BMP
echo [M-SNAKE-DONE] score=%score% len=%len% WIN
goto end

:quit
draw text 48 %MID% QUIT
draw text 48 %MID2% Score %score%
draw refresh
echo [M-SNAKE-DONE] score=%score% len=%len% QUIT
goto end

:nogfx
echo SNAKE needs the foreground display (no draw in background jobs)
goto end

:end
draw hold off
draw close
draw fullscreen off
endlocal