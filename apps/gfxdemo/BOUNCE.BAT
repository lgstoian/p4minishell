@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem =====================================================================
rem  BOUNCE.BAT - a gfx bouncing-ball demo: pixel canvas + batch math +
rem  a killable frame loop. The ball is a 16x16 sprite stamped each frame
rem  with `gfx blitmany` (loaded once from BALL.BMP, needs
rem  apps/push_assets.py), and the frame loop is a `while` that calls a
rem  :frame gosub (hits flip dx/dy in place, no cross-frame goto hops).
rem  Reference app for `gfx` raster verbs + blitmany.
rem  Markers: [M-BOUNCE] / [M-BOUNCE-FRAME] / [M-BOUNCE-DONE].
rem  The render loop never blocks on I/O (a `screenshot` in the loop froze
rem  the animation for seconds): the BMP capture runs once at the end.
rem  Foreground-only: the canvas is a shared display surface, so `gfx`
rem  refuses inside `start` background jobs (like modal verbs).
rem =====================================================================
setlocal
echo [M-BOUNCE]
title "Bounce"
gfx init 240 180
if errorlevel 1 goto nogfx
gfx load 0 BALL.BMP
set x=120
set y=90
set dx=3
set dy=2
set n=0
gfx stats reset
while n LSS 240 do call :frame
rem Capture the final frame once, off the animation loop.
screenshot BOUNCE.BMP
rem Report frame pacing from the canvas stats (frames/min/avg/max/jitter).
gfx stats
gfx close
echo [M-BOUNCE-DONE] frames=%n%
goto end
:nogfx
echo BOUNCE needs the foreground display (no gfx in background jobs)
:end
endlocal

:frame
set /a n+=1
set /a x+=%dx%
set /a y+=%dy%
if %x% GTR 226 set dx=-3
if %x% LSS 14 set dx=3
if %y% GTR 166 set dy=-2
if %y% LSS 14 set dy=2
rem Stamp centered on (x,y): the sprite is 16x16, so the slot corner sits 8
rem px up-left of the tracked center.
set /a bx=%x%-8
set /a by=%y%-8
gfx clear 1
gfx blitmany 0 %bx% %by% transparent
gfx show
echo [M-BOUNCE-FRAME] %n%
delay 33
return