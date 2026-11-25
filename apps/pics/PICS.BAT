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
set DOS_THEME=mono
gosub %APPROOT%/APPLIB.BAT::header "PICS" "[M-RICH-PICS] BMP to CP437 cells"
gosub %APPROOT%/APPLIB.BAT::tile 2 5 50 12 double "IMAGE / CELL PREVIEW"
gosub %APPROOT%/APPLIB.BAT::tile 54 5 25 12 single "PALETTE"
draw text 56 8 "BMP -> 80x25 grid" 7
gosub %APPROOT%/APPLIB.BAT::statusbar 21 "viewer=cell  source=BMP" "1 view  2 next  q quit"
echo [M-DOS-PICS] screen=PICS-VIEWER panels=2 head="PICS" theme=green bars=0 table=N spark=0 status=Y
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
if errorlevel 1 goto menu
rem [M-RICH-PICS] v1.6 canvas tour: gradient backdrop, a translucent
rem composite layer and a sprite-sheet blitmany /a stamp row. Bounded keys
rem now toggle image fit (F) and the alpha composite layer (A); ONE renderer
rem (:pics_canvas) and ONE dispatcher serve every preview.
echo [M-RICH-PICS] gradient + composite + atlas
set FIT=1
set ALPHA=128
set CSTEPS=0
goto pics_loop

:pics_dispatch
on %errorlevel% goto :pics_fit,:pics_alpha,:pics_back
goto pics_loop

:pics_fit
if %FIT% EQU 1 goto fit_off
set FIT=1
goto fit_done
:fit_off
set FIT=0
:fit_done
echo [M-PICS-KEY] key=f fit=%FIT% alpha=%ALPHA%
goto pics_loop

:pics_alpha
if %ALPHA% EQU 128 goto alpha_off
set ALPHA=128
goto alpha_done
:alpha_off
set ALPHA=0
:alpha_done
echo [M-PICS-KEY] key=a fit=%FIT% alpha=%ALPHA%
goto pics_loop

:pics_canvas
gfx clear 0x101820
gfx gradient 0 160 320 80 0x101820 0x3A6EA5 h
if %FIT% EQU 1 goto pics_fit_on
gfx image %APPDRIVE%/PHOTO.BMP 40 16 240 120
goto pics_img_done
:pics_fit_on
gfx image %APPDRIVE%/PHOTO.BMP 0 0 320 160
:pics_img_done
gfx load 0 %APPDRIVE%/BALL.BMP
if errorlevel 1 goto picsshow
if %ALPHA% EQU 128 gfx composite 0 280 170 128
gfx blitmany 0 /a:1,16,16 8 170 28 170 48 170
:picsshow
gfx show
echo [M-PICS-CANVAS] fit=%FIT% alpha=%ALPHA%
return

:pics_loop
if %CSTEPS% GEQ 12 goto pics_back
set /a CSTEPS=%CSTEPS%+1
gosub :pics_canvas
choice /C:faq /N /T:q,10 "Canvas"
goto pics_dispatch

:pics_back
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
