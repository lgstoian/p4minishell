@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem =====================================================================
rem  PLOT.BAT - `plot` coordinate-layer demo: scientific graphs, charts and
rem  world-coordinate drawings on the gfx canvas, then a TUI bar chart.
rem  Saves PLOT.BMP. Markers: [M-PLOT] / [M-PLOT-DONE].
rem =====================================================================
echo [M-PLOT]
if "%P4_APP_FEATURES%"=="1" gosub %APPROOT%/APPLIB.BAT::probe
title "Plot"
calc /rad
gfx init 320 200
if errorlevel 1 goto :nofx
gfx clear 0
rem [M-RICH-PLOT] v1.6 canvas tour: a vertical gradient backdrop under the
rem plot plus a func/return-computed scale (%%RESULT%%).
echo [M-RICH-PLOT] gradient backdrop + computed scale
gfx gradient 0 0 320 200 0x000000 0x101820 v
func :plotscale 6 8
echo [M-RICH-PLOT-F] scale=%RESULT%
rem Nudge the x window just past 2 full periods: at exactly +/-6.28 the
rem sine endpoints land on the rightmost/leftmost tick labels ("6"/"-6")
rem and overdraw them. Ending at +/-6.8 lifts the endpoints off the axis.
plot window -6.8 6.8 -1.6 1.6
plot axes /grid
plot func "sin(X)" 15
plot func "cos(X)" 11
plot line -6.28 0 6.28 0 8
rem Parametric scatter: 48 samples near the sine curve, each point computed
rem with `calc` (angle mode already /rad) in a gosub fed by a for /L index;
rem plot point then places the world coordinate on the canvas.
for /L %%i in (1,1,48) do call :scatter %%i
gfx show
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
rem %1 = 1..48: x sweeps -6..6 in 0.25 steps, y rides around sin(x).
calc WX = -6 + 0.25*%1
calc WY = SIN(WX) + 0.04*SIN(WX*5)
plot point %WX% %WY% 12
return

:nofx
echo gfx: canvas unavailable (TUI active or out of memory)
echo [M-PLOT-DONE]
