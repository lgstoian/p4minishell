@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem =====================================================================
rem  GFXRICH.BAT - board-facing reference for the bounded game toolkit:
rem  a named sprite-sheet tilemap with per-tile flags, a deterministic
rem  particle emitter and a keyed/alpha layer present. Draws one 160x120
rem  RGB565 canvas and presents it.
rem  Markers: [M-GFXRICH] / [M-GFXRICH-TILE] / [M-GFXRICH-PARTICLE] /
rem  [M-GFXRICH-LAYER] / [M-GFXRICH-DONE].
rem  Host checks only assert that every surface is exercised here; the exact
rem  pixels are asserted by the pure Unity cases (test/main/test_gfx.c).
rem =====================================================================
echo [M-GFXRICH]
gfx init 160 120
if errorlevel 1 goto :done
gfx clear 0x101820
gfx load 0 %APPDRIVE%/BALL.BMP
if errorlevel 1 goto :show
echo [M-GFXRICH-TILE]
gfx tile create 0 terrain 0 2 2 16 16 /z:1
if errorlevel 1 goto :show
gfx tile set 0 0 0 0 /f:h
gfx tile set 0 1 1 1 /f:rv
gfx tile draw 8 8
gfx tile list
echo [M-GFXRICH-PARTICLE]
gfx particles seed 12345
gfx particles emit 8 40 60 4 0xFFAA00
gfx particles step
gfx particles draw 0 0
echo [M-GFXRICH-LAYER]
gfx layer bind 0 cloud 0 20 20 128 /z:0
gfx layer dirty 0
gfx layer visible 0 on
gfx layer present all
:show
gfx show
echo [M-GFXRICH-DONE]
:done
