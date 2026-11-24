@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem =====================================================================
rem  GFXAA.BAT - board-facing reference for the additive gfx toolkit:
rem  smooth primitives, linear gradients, a keyed/alpha layer composite and
rem  a sprite-sheet blitmany. Draws one 160x120 RGB565 canvas and presents it.
rem  Markers: [M-GFXAA] / [M-GFXAA-DONE].
rem  Host-side checks only verify that every new verb is exercised here; the
rem  pixels are asserted by the host Unity lane (test/main/test_gfx.c).
rem =====================================================================
echo [M-GFXAA]
gfx init 160 120
if errorlevel 1 goto :done
gfx clear 0x101820
rem Smooth line, circle outline and rectangle outline (originals stay exact).
gfx lineaa 4 4 150 100 0xFFFFFF
gfx circleaa 80 60 30 0x00FFFF
gfx rectaa 8 8 144 104 0x3A6EA5
rem Horizontal and vertical linear gradients.
gfx gradient 20 70 120 30 0x0000FF 0xFF0000
gfx gradient 100 20 30 40 0x00FF00 0xFF00FF v
rem Sprite sheet: one slot loaded once, three cells stamped in one command.
gfx load 0 %APPDRIVE%/BALL.BMP
if errorlevel 1 goto :show
gfx composite 0 60 40 128 0x000000
gfx blitmany 0 /a:1,16,16 0 110 16 0 126 16 0 142 16
:show
gfx show
echo [M-GFXAA-DONE]
:done
