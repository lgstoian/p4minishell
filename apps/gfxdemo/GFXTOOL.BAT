@echo off
rem =====================================================================
rem  GFXTOOL.BAT - gfx toolkit demo: raster primitives + on-canvas text.
rem  Draws filled/outlined shapes, flood fill, spans and scaled text into
rem  one 320x200 RGB565 canvas, saves it as GFXTOOL.BMP, and exits.
rem  Markers: [M-GFXTOOL] / [M-GFXTOOL-DONE].
rem =====================================================================
echo [M-GFXTOOL]
gfx init 320 200
if errorlevel 1 goto :nofx
gfx clear 0x101820
gfx rect 4 4 312 192 0x3A6EA5
gfx triangle 20 30 140 30 20 150 0xFF5555 fill
gfx ellipse 185 95 65 50 0x55FF55 fill
gfx polygon 0x5599FF fill 215 150 300 150 257 40
gfx rect 230 12 72 40 0xFFFFFF
gfx fill 266 30 0xFFAA00
gfx hline 8 190 304 0xFFFFFF
gfx vline 158 20 120 0xFFFF00
gfx text /scale:2 18 150 0xFFFFFF Gfx Toolkit
gfx text 18 170 0x00FFFF tri ellipse poly fill text
gfx save GFXTOOL.BMP
gfx close
echo [M-GFXTOOL-DONE]
goto :eof

:nofx
echo gfx: canvas unavailable (TUI active or out of memory)
echo [M-GFXTOOL-DONE]
