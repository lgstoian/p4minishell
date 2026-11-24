@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem =====================================================================
rem  GFXTOOL.BAT - gfx toolkit demo: raster primitives + on-canvas text,
rem  plus a scaled BMP blit (`gfx image`) of PHOTO.BMP in the free
rem  lower-right corner. Draws one 320x200 RGB565 canvas, saves it as
rem  GFXTOOL.BMP, and exits. Needs PHOTO.BMP (apps/push_assets.py).
rem  Markers: [M-GFXTOOL] / [M-GFXTOOL-IMG|NOIMG] / [M-GFXTOOL-DONE].
rem =====================================================================
echo [M-GFXTOOL]
if "%P4_APP_FEATURES%"=="1" gosub %APPROOT%/APPLIB.BAT::probe
title "GFX Tool"
gfx init 320 200
if errorlevel 1 goto :nofx
gfx clear 0x101820
rem [M-RICH-GFXTOOL] v1.6 toolkit tour: smooth edges, a linear gradient,
rem a keyed translucent composite and a sprite-sheet blitmany /a atlas.
echo [M-RICH-GFXTOOL] smooth toolkit tour
gfx gradient 0 0 320 200 0x101820 0x1B3A5C h
gfx lineaa 4 196 316 4 0x00FFFF
gfx circleaa 160 100 70 0x555555
gfx rectaa 2 2 318 198 0x3A6EA5
func :gfxsum 12 30
echo [M-RICH-GFXTOOL-F] area=%RESULT%
gfx rect 4 4 312 192 0x3A6EA5
gfx triangle 20 30 140 30 20 150 0xFF5555 fill
gfx ellipse 185 95 65 50 0x55FF55 fill
gfx polygon 0x5599FF fill 215 150 300 150 257 40
gfx rect 230 12 72 40 0xFFFFFF
gfx fill 266 30 0xFFAA00
gfx hline 8 190 304 0xFFFFFF
gfx vline 158 20 120 0xFFFF00
rem Sprite stamp row (blitmany): load the 16x16 BALL sprite once and stamp a
rem diagonal row of copies, rotating each step 90 degrees. The text is drawn
rem AFTER the stamps so a sprite can never cover the title or the key legend
rem (the v1.4.3 gallery image had two balls stamped over both lines).
gfx load 0 %APPDRIVE%/BALL.BMP
set /a sx=24
set /a sy=160
for /L %%s in (0,1,3) do call :stamp %%s
gfx text /scale:2 18 150 0xFFFFFF Gfx Toolkit
gfx text 18 170 0x00FFFF tri ellipse poly fill text
echo [M-GFXTOOL-BLIT]
rem v1.6 atlas stamp row (sheet cells stamped in ONE command, no rotation).
gfx blitmany 0 /a:1,16,16 24 40 40 40 56 40
gfx composite 0 270 100 96
echo [M-RICH-GFXTOOL-ATLAS] sheet+composite stamped
rem Scaled BMP blit below the text and above the hline: 232..312 x 154..186
rem is outside every primitive/text region checked by gfx_toolkit_test.py.
gfx image %APPDRIVE%/PHOTO.BMP 232 154 80 32
if errorlevel 1 echo [M-GFXTOOL-NOIMG]
if not errorlevel 1 echo [M-GFXTOOL-IMG]
rem Present the canvas, then capture the whole panel for the docs gallery
rem (GFXTOOL.BMP below stays the raw 320x200 canvas that gfx_toolkit_test.py
rem checks; the SCREEN file is the 1024x600 surface).
gfx show
screenshot %APPDRIVE%/GFXTOOL_SCREEN.BMP
gfx save %APPDRIVE%/GFXTOOL.BMP
gfx close
echo [M-GFXTOOL-DONE]
return

:gfxsum
return %1*%2

:stamp
rem %1 = 0..3; %so% = the rotation for this step, sx/sy slide across the row.
set /a so=%1*90
gfx blitmany 0 /r:%so% %sx% %sy% transparent
set /a sx+=34
set /a sy-=14
return

:nofx
echo gfx: canvas unavailable (TUI active or out of memory)
echo [M-GFXTOOL-FAIL]
echo [M-GFXTOOL-DONE]
