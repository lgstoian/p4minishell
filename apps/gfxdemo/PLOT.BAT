@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem =====================================================================
rem  PLOT.BAT - `plot` coordinate-layer demo: scientific graphs, charts and
rem  world-coordinate drawings on the gfx canvas, then a TUI bar chart.
rem  The v1.6 drawing is unchanged; bounded keys now tune the live preview
rem  (1/2/3 function, G grid, +/- sample count, Q done) through ONE key
rem  dispatcher and ONE renderer (:plot_draw). Saves PLOT.BMP.
rem  Markers: [M-PLOT] / [M-PLOT-DONE] (+ additive [M-PLOT-KEY|DRAW]).
rem =====================================================================
echo [M-PLOT]
echo [M-DOS-PLOT] screen=PLOT-HEADLESS panels=0 head="NONE" theme=mono bars=0 table=N spark=0 status=N
if "%P4_APP_FEATURES%"=="1" gosub %APPROOT%/APPLIB.BAT::probe
title "Plot"
calc /rad
gfx init 320 200
if errorlevel 1 goto :nofx
gfx clear 0
rem [M-RICH-PLOT] v1.6 canvas tour: a vertical gradient backdrop under the
rem plot plus a func/return-computed scale (%%RESULT%%).
echo [M-RICH-PLOT] gradient backdrop + computed scale
func :plotscale 6 8
echo [M-RICH-PLOT-F] scale=%RESULT%
rem Interactive plot parameters (all bounded: <=8 key turns, choice /T:q,10).
set PFUNC=both
set PGRID=1
set PN=48
set PSTEPS=0
gosub :plot_draw
goto plot_loop

rem ---- ONE key dispatcher for the plot parameters ------------------------
:plot_dispatch
on %errorlevel% goto :pf_sin,:pf_cos,:pf_both,:pf_grid,:pf_up,:pf_down,:plot_finish
goto plot_loop

:pf_sin
set PFUNC=sin
echo [M-PLOT-KEY] key=1 func=sin grid=%PGRID% samples=%PN%
gosub :plot_draw
goto plot_loop

:pf_cos
set PFUNC=cos
echo [M-PLOT-KEY] key=2 func=cos grid=%PGRID% samples=%PN%
gosub :plot_draw
goto plot_loop

:pf_both
set PFUNC=both
echo [M-PLOT-KEY] key=3 func=both grid=%PGRID% samples=%PN%
gosub :plot_draw
goto plot_loop

:pf_grid
if %PGRID% EQU 1 goto grid_off
set PGRID=1
goto grid_done
:grid_off
set PGRID=0
:grid_done
echo [M-PLOT-KEY] key=g func=%PFUNC% grid=%PGRID% samples=%PN%
gosub :plot_draw
goto plot_loop

:pf_up
set /a PN=%PN%+12
if %PN% GTR 48 set PN=48
echo [M-PLOT-KEY] key=+ func=%PFUNC% grid=%PGRID% samples=%PN%
gosub :plot_draw
goto plot_loop

:pf_down
set /a PN=%PN%-12
if %PN% LSS 12 set PN=12
echo [M-PLOT-KEY] key=- func=%PFUNC% grid=%PGRID% samples=%PN%
gosub :plot_draw
goto plot_loop

:plot_loop
if %PSTEPS% GEQ 8 goto plot_finish
set /a PSTEPS=%PSTEPS%+1
choice /C:123g+-q /N /T:q,10 "Plot"
goto plot_dispatch

rem ---- ONE renderer: gradient backdrop, axes, func(s), scatter -----------
:plot_draw
gfx clear 0
gfx gradient 0 0 320 200 0x000000 0x101820 v
rem Nudge the x window just past 2 full periods so sine endpoints do not
rem overdraw the rightmost/leftmost tick labels.
plot window -6.8 6.8 -1.6 1.6
if %PGRID% EQU 1 goto axes_grid
plot axes
goto axes_done
:axes_grid
plot axes /grid
:axes_done
if "%PFUNC%"=="both" goto pdf_both
if "%PFUNC%"=="sin" goto pdf_sin
plot func "cos(X)" 11
goto pdf_end
:pdf_sin
plot func "sin(X)" 15
goto pdf_end
:pdf_both
plot func "sin(X)" 15
plot func "cos(X)" 11
:pdf_end
plot line -6.28 0 6.28 0 8
for /L %%i in (1,1,48) do call :scatter %%i
gfx show
echo [M-PLOT-DRAW] func=%PFUNC% grid=%PGRID% samples=%PN%
return

:plot_finish
rem Panel capture for the docs gallery; PLOT.BMP below remains the raw
rem 320x200 canvas that plot_test.py checks.
screenshot %APPDRIVE%/PLOT_SCREEN.BMP
gfx save %APPDRIVE%/PLOT.BMP
gfx close
echo [M-PLOT-DONE]
return

:plotscale
return %1*%2

:scatter
rem %1 = 1..48; PN is the live sample cap (12..48), so +/- changes density.
if %1 GTR %PN% return
calc WX = -6 + 0.25*%1
calc WY = SIN(WX) + 0.04*SIN(WX*5)
plot point %WX% %WY% 12
return

:nofx
echo gfx: canvas unavailable (TUI active or out of memory)
echo [M-PLOT-DONE]
