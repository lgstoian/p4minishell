@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem =====================================================================
rem  PICS.BAT - BMP image reference app.
rem  Demonstrates the image surface everywhere: the full-color viewer
rem  (`view`), the 16-color TUI renderer (`draw image`), and the gfx
rem  canvas (`gfx image`), plus `image info` metadata.
rem  Needs PHOTO.BMP from apps/push_assets.py (SD root). Markers: [M-PICS].
rem =====================================================================
setlocal
echo [M-PICS]
goto menu

:menu
draw clear screen
draw box 1 1 80 3 double "PICS"
draw text 5 2 "BMP image support" 14
list "Pictures" "View PHOTO.BMP (full color)" "Draw into TUI (16 color)" "Blit to gfx canvas" "Image info" "Back"
if errorlevel 255 goto back
if errorlevel 4 goto back
if errorlevel 3 goto info
if errorlevel 2 goto canvas
if errorlevel 1 goto tui
goto view

:view
view PHOTO.BMP
goto menu

:tui
draw clear screen
draw box 1 1 80 25 single "TUI IMAGE"
draw image PHOTO.BMP 4 3 72 20
list "TUI" "PHOTO.BMP drawn as 16-color blocks" "Back"
goto menu

:canvas
draw close
gfx init 320 240
gfx image PHOTO.BMP 0 0 320 160
gfx show
list "gfx" "PHOTO.BMP blitted onto the canvas" "Back"
gfx close
goto menu

:info
image info PHOTO.BMP
pause
goto menu

:back
rem Leave the TUI: `draw clear screen` alone keeps TUI mode active and would
rem make the next gfx app refuse ("gfx: TUI mode is active").
draw close
return
