@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem =====================================================================
rem  PICS.BAT - BMP image reference app.
rem  Demonstrates the image surface everywhere: the full-color viewer
rem  (`view`), the 16-color TUI renderer (`draw image`), and the gfx
rem  canvas (`gfx image`), plus `image info` metadata.
rem  Needs PHOTO.BMP on the app drive (apps/push_assets.py). Markers: [M-PICS].
rem =====================================================================
setlocal
if "%P4_APP_FEATURES%"=="1" gosub %APPROOT%/APPLIB.BAT::probe
echo [M-PICS]
title "Pictures"
goto menu

:menu
draw clear screen
rem Layout: the shell publishes the grid (80x25 landscape, 80x97 portrait).
gosub %APPROOT%/APPLIB.BAT::page double "PICS ★"
draw text 5 2 "BMP image support  (%APPNAME% on %APPDRIVE%)" 14
list "Pictures" "View PHOTO.BMP (full color)" "Draw into TUI (16 color)" "Blit to gfx canvas" "Image info" "Back"
on %errorlevel%+1 goto :view,:tui,:canvas,:info,:back
goto back

:view
echo [M-PICS-VIEW]
view %APPDRIVE%/PHOTO.BMP
goto menu

:tui
draw clear screen
rem The image page frames the whole grid and scales the 16-colour draw to the
rem panel: 72x20 cells on the classic 25-row grid (the authored size), more
rem rows on a portrait panel.
if "%TUICOLS%"=="" set TUICOLS=80
if "%TUIROWS%"=="" set TUIROWS=25
set /a IW=%TUICOLS%-8 > NUL
set /a IH=%TUIROWS%-5 > NUL
gosub %APPROOT%/APPLIB.BAT::box single "TUI IMAGE"
draw image %APPDRIVE%/PHOTO.BMP 4 3 %IW% %IH%
list "TUI" "PHOTO.BMP drawn as 16-color blocks" "Back"
goto menu

:canvas
draw close
gfx init 320 240
rem [M-RICH-PICS] v1.6 canvas tour: gradient backdrop, a translucent
rem composite layer and a sprite-sheet blitmany /a stamp row.
echo [M-RICH-PICS] gradient + composite + atlas
gfx gradient 0 160 320 80 0x101820 0x3A6EA5 h
gfx image %APPDRIVE%/PHOTO.BMP 0 0 320 160
gfx load 0 %APPDRIVE%/BALL.BMP
if errorlevel 1 goto picsshow
gfx composite 0 280 170 128
gfx blitmany 0 /a:1,16,16 8 170 28 170 48 170
:picsshow
gfx show
list "gfx" "PHOTO.BMP blitted onto the canvas" "Back"
gfx close
goto menu

:info
image info %APPDRIVE%/PHOTO.BMP
pause
goto menu

:back
rem Leave the TUI: `draw clear screen` alone keeps TUI mode active and would
rem make the next gfx app refuse ("gfx: TUI mode is active").
draw close
return
