@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem =====================================================================
rem  BOUNCE.BAT - an INTERACTIVE gfx paddle game in the DOS key/frame style
rem  of SNAKE.BAT. One `choice /C:... /T:...,1` read per tick feeds ONE
rem  dispatcher (:dispatch via `on`); ONE renderer (:render) paints every
rem  screen. The ball is a 16x16 sprite stamped with `gfx blitmany` (loaded
rem  once from BALL.BMP, needs apps/push_assets.py). Panels, HUD and status
rem  bar are drawn on the canvas with gfx rect/hline/text using the DOS
rem  palette (cyan frame, ice-blue fill, white ink) so the gfx surface keeps
rem  the 80x25 CP437 dashboard look instead of becoming a raw demo loop.
rem  Hit sparks use the shipped `gfx particles seed|emit|step|draw` pool.
rem  Markers: [M-BOUNCE] / [M-BOUNCE-FRAME] / [M-BOUNCE-DONE] plus the new
rem  [M-BOUNCE-UI|KEY|HIT|LIFE|OVER|RESTART] control-state lines.
rem  The render loop never blocks on I/O (a `screenshot` in the loop froze
rem  the animation for seconds): the BMP capture runs once at the end.
rem  Foreground-only: the canvas is a shared display surface, so `gfx`
rem  refuses inside `start` background jobs (like modal verbs).
rem =====================================================================
setlocal
if "%P4_APP_FEATURES%"=="1" gosub %APPROOT%/APPLIB.BAT::probe
echo [M-BOUNCE]
echo [M-DOS-BOUNCE] screen=BOUNCE-HEADLESS panels=0 head="NONE" theme=mono bars=0 table=N spark=0 status=N
title "Bounce"
gfx init 240 180
if errorlevel 1 goto nogfx
gfx load 0 %APPDRIVE%/BALL.BMP
rem The v1.7 particle pool replaces hand-stamped spark pixels on hits.
gfx particles seed 4242
rem [M-RICH-BOUNCE] additive v1.6 gfx tour: smooth primitives, a linear
rem gradient background, a translucent composite layer and an atlas blitmany.
echo [M-RICH-BOUNCE] smooth gfx + language tour
gfx gradient 0 0 240 180 0x101820 0x3A6EA5 h
gfx lineaa 0 179 239 0 0x00FFFF
gfx circleaa 120 90 40 0xFFFFFF
gfx rectaa 4 4 236 176 0x555555
gfx composite 0 8 8 128
gfx blitmany 0 /a:1,16,16 0 8
func :bouncev 3 2
echo [M-RICH-BOUNCE-F] speed=%RESULT%
rem ---- game state: 3 lives, score per paddle hit, level = speed tier ----
set score=0
set lives=3
set level=1
set x=120
set y=90
set dx=3
set dy=2
set n=0
set paddle=112
set spark=0
set hx=120
set hy=160
set tx1=120
set ty1=90
set tx2=120
set ty2=90
set UIBACK=start
set UI=start
gfx stats reset
echo [M-BOUNCE-UI] state=start score=0 lives=3 level=1
goto loop

rem ---- ONE input dispatcher; every choice uses the same key ordering -----
rem choice errorlevel 1..8 maps to: a d p n q h r s. /T supplies an idle
rem default per screen, so a headless/untouched session still advances.
:dispatch
on %errorlevel% goto :k_left,:k_right,:k_pause,:k_none,:k_quit,:k_help,:k_restart,:k_start
goto loop

:k_left
echo [M-BOUNCE-KEY] key=a state=%UI%
if "%UI%"=="play" set /a paddle=%paddle%-12
if %paddle% LSS 24 set paddle=24
goto loop

:k_right
echo [M-BOUNCE-KEY] key=d state=%UI%
if "%UI%"=="play" set /a paddle=%paddle%+12
set PMAX=216
if %paddle% GTR %PMAX% set paddle=%PMAX%
goto loop

:k_pause
echo [M-BOUNCE-KEY] key=p state=%UI%
if "%UI%"=="play" gosub :setstate pause
if "%UI%"=="pause" gosub :setstate play
goto loop

:k_none
echo [M-BOUNCE-KEY] key=n state=%UI%
goto loop

:k_quit
echo [M-BOUNCE-KEY] key=q state=%UI%
gosub :setstate quit
goto loop

:k_help
echo [M-BOUNCE-KEY] key=h state=%UI%
if "%UI%"=="help" goto helpback
set UIBACK=%UI%
gosub :setstate help
goto loop
:helpback
gosub :setstate %UIBACK%
goto loop

:k_restart
echo [M-BOUNCE-KEY] key=r state=%UI%
if not "%UI%"=="over" goto loop
gosub :resetgame
echo [M-BOUNCE-RESTART]
gosub :setstate play
goto loop

:k_start
echo [M-BOUNCE-KEY] key=s state=%UI%
if "%UI%"=="help" goto helpback
if not "%UI%"=="start" goto loop
gosub :resetgame
gosub :setstate play
goto loop

:setstate
set UI=%1
echo [M-BOUNCE-UI] state=%UI% score=%score% lives=%lives% level=%level%
return

:resetgame
set score=0
set lives=3
set level=1
set x=120
set y=90
set dx=3
set dy=2
set paddle=112
set spark=0
set tx1=120
set ty1=90
set tx2=120
set ty2=90
return

rem ---- main loop: bounded per tick (choice /T), bounded overall (n) -----
:loop
if "%UI%"=="quit" goto finish
if %n% GEQ 240 goto finish
if "%UI%"=="play" gosub :step
gosub :render
rem 24 ms keeps the original 30 fps present pacing between bounded key reads.
if "%UI%"=="play" delay 24
set DEF=n
set TO=1
if "%UI%"=="start" set DEF=s
if "%UI%"=="start" set TO=10
if "%UI%"=="help" set DEF=s
if "%UI%"=="help" set TO=10
if "%UI%"=="pause" set DEF=n
if "%UI%"=="pause" set TO=10
if "%UI%"=="over" set DEF=q
if "%UI%"=="over" set TO=10
choice /C:adpnqhrs /N /T:%DEF%,%TO% "Keys"
goto dispatch

rem ---- ball physics: one discrete step per key tick ---------------------
:step
set /a n+=1
set tx2=%tx1%
set ty2=%ty1%
set tx1=%x%
set ty1=%y%
set /a x+=%dx%
set /a y+=%dy%
if %x% GTR 224 gosub :sidehit
if %x% LSS 16 gosub :sidehit
if %y% LSS 28 gosub :tophit
set /a PL=%paddle%-24
set /a PR=%paddle%+24
if %y% LSS 160 goto step_spark
if %x% LSS %PL% goto miss
if %x% GTR %PR% goto miss
gosub :paddlehit
goto step_spark
:miss
set /a lives=%lives%-1
echo [M-BOUNCE-LIFE] lives=%lives%
if %lives% LEQ 0 goto gameover
set x=120
set y=90
set dx=3
set dy=2
:step_spark
if %spark% GTR 0 set /a spark=%spark%-1
echo [M-BOUNCE-FRAME] %n%
return

:sidehit
set /a dx=0-%dx%
set hx=%x%
set hy=%y%
set spark=3
gfx particles emit 4 %x% %y% 4 0x00FFFF
echo [M-BOUNCE-HIT] kind=wall score=%score% lives=%lives%
return

:tophit
set /a dy=0-%dy%
set hx=%x%
set hy=%y%
set spark=3
gfx particles emit 4 %x% %y% 4 0x00FFFF
echo [M-BOUNCE-HIT] kind=wall score=%score% lives=%lives%
return

:paddlehit
set /a score=%score%+10
set /a level=%level%+1
set /a dy=0-%dy%
set /a dx=%dx%+1
set NEG8=-8
if %dx% GTR 8 set dx=8
if %dx% LSS %NEG8% set dx=%NEG8%
set y=158
set hx=%x%
set hy=160
set spark=4
gfx particles emit 8 %x% 158 6 0xFFAA00
echo [M-BOUNCE-HIT] kind=paddle score=%score% lives=%lives%
tone 660 40
return

:gameover
echo [M-BOUNCE-OVER] score=%score% level=%level%
gosub :setstate over
return

rem ---- ONE renderer: DOS frame + HUD + per-state panel + playfield -----
:render
gfx clear 0x101820
gfx rect 2 2 236 176 0x00FFFF
gfx rect 4 4 232 172 0x3A6EA5
gfx hline 4 18 232 0x00FFFF
gfx text 8 6 0xFFFFFF Bounce / Paddle
gfx text 150 6 0x00FFFF v1.6 gfx tour
gfx text 8 22 0xFFFFFF SCORE %score%   LIVES %lives%   LEVEL %level%
if not "%UI%"=="start" goto rend_help
gfx rect 34 48 172 84 0x00FFFF
gfx rect 36 50 168 80 0x101820
gfx hline 36 62 168 0x3A6EA5
gfx text 44 54 0xFFFFFF BOUNCE / PADDLE
gfx text 44 68 0x00FFFF Keep the ball off the floor.
gfx text 44 78 0xFFFFFF A/D move   P pause
gfx text 44 88 0xFFFFFF H help     Q quit
gfx text 44 102 0xFFAA00 S to start - auto in 10s
:rend_help
if not "%UI%"=="help" goto rend_pause
gfx rect 22 40 196 100 0x00FFFF
gfx rect 24 42 192 96 0x101820
gfx hline 24 54 192 0x3A6EA5
gfx text 32 46 0xFFFFFF HELP - KEYS AND RULES
gfx text 32 60 0x00FFFF A / D     move the paddle
gfx text 32 70 0xFFFFFF P         pause / resume
gfx text 32 80 0xFFFFFF H         this help
gfx text 32 90 0xFFFFFF R         restart after game over
gfx text 32 100 0xFFAA00 Q         quit to the transcript
gfx text 32 118 0x3A6EA5 3 lives - 10 points per hit - faster each hit
:rend_pause
if not "%UI%"=="pause" goto rend_over
gfx rect 70 64 100 52 0x00FFFF
gfx rect 72 66 96 48 0x101820
gfx text 88 74 0xFFFFFF PAUSED
gfx text 80 88 0x00FFFF P resume   Q quit
:rend_over
if not "%UI%"=="over" goto rend_play
gfx rect 46 44 148 88 0x00FFFF
gfx rect 48 46 144 84 0x101820
gfx hline 48 58 144 0x3A6EA5
gfx text 70 50 0xFFFFFF GAME OVER
gfx text 60 64 0xFFAA00 SCORE %score%  LEVEL %level%
gfx text 60 78 0x00FFFF R restart   Q quit
gfx text 60 96 0x3A6EA5 auto-quit in 10s
:rend_play
if not "%UI%"=="play" goto rend_status
rem fading trail (two prior centres) then impact particles
gfx rect %tx2% %ty2% 4 4 0x1B3A5C fill
gfx rect %tx1% %ty1% 6 6 0x3A6EA5 fill
if %spark% EQU 0 goto rend_ball
gfx particles step 1
gfx particles draw
:rend_ball
gfx circleaa %x% %y% 10 0x00FFFF
set /a bx=%x%-8
set /a by=%y%-8
gfx blitmany 0 %bx% %by% transparent
set /a PX=%paddle%-24
gfx rect %PX% 162 48 4 0x55FF55 fill
gfx hline %PX% 162 48 0xFFFFFF
:rend_status
gfx rect 4 168 232 8 0x3A6EA5 fill
gfx text 8 169 0xFFFFFF A/D move  P pause  H help  R restart  Q quit
gfx show
return

:finish
rem Capture the final frame once, off the animation loop.
screenshot %APPDRIVE%/BOUNCE.BMP
rem Report frame pacing from the canvas stats (frames/min/avg/max/jitter).
gfx stats
gfx close
echo [M-BOUNCE-DONE] frames=%n%
endlocal
rem Stop here: a label body that follows is still reachable by fall-through.
goto :eof

:nogfx
echo BOUNCE needs the foreground display (no gfx in background jobs)
echo [M-BOUNCE-DONE] frames=0
endlocal
goto :eof

:bouncev
return %1*%1+%2*%2
