@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem =====================================================================
rem  PLOT.BAT - `plot` coordinate-layer demo: scientific graphs, charts and
rem  world-coordinate drawings on the gfx canvas, then a TUI bar chart.
rem  Saves PLOT.BMP. Markers: [M-PLOT] / [M-PLOT-DONE].
rem =====================================================================
echo [M-PLOT]
calc /rad
gfx init 320 200
if errorlevel 1 goto :nofx
gfx clear 0
plot window -6.28 6.28 -1.6 1.6
plot axes /grid
plot func "sin(X)" 15
plot func "cos(X)" 11
plot line -6.28 0 6.28 0 8
gfx show
gfx save PLOT.BMP
gfx close
echo [M-PLOT-DONE]
return

:nofx
echo gfx: canvas unavailable (TUI active or out of memory)
echo [M-PLOT-DONE]
