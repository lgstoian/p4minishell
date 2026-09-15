@echo off
rem =====================================================================
rem  BOUNCE.BAT - a gfx bouncing-ball demo: pixel canvas + batch math +
rem  a killable frame loop. Reference app for `gfx` raster verbs.
rem  Markers: [M-BOUNCE] / [M-BOUNCE-FRAME] / [M-BOUNCE-DONE].
rem  The render loop never blocks on I/O (a `screenshot` in the loop froze
rem  the animation for seconds): the BMP capture runs once at the end.
rem  Foreground-only: the canvas is a shared display surface, so `gfx`
rem  refuses inside `start` background jobs (like modal verbs).
rem =====================================================================
setlocal
echo [M-BOUNCE]
gfx init 240 180
if errorlevel 1 goto nogfx
set x=120
set y=90
set dx=3
set dy=2
set n=0
:loop
set /a n+=1
set /a x+=%dx%
set /a y+=%dy%
if %x% GTR 226 goto hit_r
if %x% LSS 14 goto hit_l
if %y% GTR 166 goto hit_b
if %y% LSS 14 goto hit_t
goto bounce_draw
:hit_r
set dx=-3
goto loop
:hit_l
set dx=3
goto loop
:hit_b
set dy=-2
goto loop
:hit_t
set dy=2
goto loop
:bounce_draw
gfx clear 1
gfx circle %x% %y% 12 14 fill
gfx circle %x% %y% 5 15 fill
gfx show
echo [M-BOUNCE-FRAME] %n%
delay 33
if %n% GEQ 240 goto done
goto loop
:done
rem Capture the final frame once, off the animation loop.
screenshot BOUNCE.BMP
gfx close
echo [M-BOUNCE-DONE] frames=%n%
goto end
:nogfx
echo BOUNCE needs the foreground display (no gfx in background jobs)
:end
endlocal
