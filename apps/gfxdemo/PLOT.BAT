@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem =====================================================================
rem  PLOT.BAT - `plot` coordinate-layer demo: scientific graphs, charts and
rem  world-coordinate drawings on the gfx canvas, then a TUI bar chart.
rem  Saves PLOT.BMP. Markers: [M-PLOT] / [M-PLOT-DONE].
rem =====================================================================
echo [M-PLOT]
title "Plot"
calc /rad
gfx init 320 200
if errorlevel 1 goto :nofx
gfx clear 0
plot window -6.28 6.28 -1.6 1.6
plot axes /grid
plot func "sin(X)" 15
plot func "cos(X)" 11
plot line -6.28 0 6.28 0 8
rem Parametric scatter: 48 samples near the sine curve, each point computed
rem with `calc` (angle mode already /rad) in a gosub fed by a for /L index;
rem plot point then places the world coordinate on the canvas.
for /L %%i in (1,1,48) do call :scatter %%i
gfx show
gfx save PLOT.BMP
gfx close
echo [M-PLOT-DONE]
return

:scatter
rem %1 = 1..48: x sweeps -6..6 in 0.25 steps, y rides around sin(x).
calc WX = -6 + 0.25*%1
calc WY = SIN(WX) + 0.04*SIN(WX*5)
plot point %WX% %WY% 12
return

:nofx
echo gfx: canvas unavailable (TUI active or out of memory)
echo [M-PLOT-DONE]
