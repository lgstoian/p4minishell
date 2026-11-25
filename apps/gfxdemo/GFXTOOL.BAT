@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem =====================================================================
rem  GFXTOOL.BAT - INTERACTIVE gfx toolkit workbench. The shipped v1.6
rem  toolkit tour is still drawn verbatim as the "before" state (all gfx
rem  primitives, atlas blitmany, translucent composite and scaled image),
rem  then a bounded key loop lets the player select a primitive, tune its
rem  size live (+/-), flip a before/after preview (V) and save a snapshot
rem  (S). The v1.7 tile map and alpha layer are bound from the SAME sprite
rem  sheet through `gfx tile create|set|draw` and `gfx layer bind|dirty|
rem  present|visible`, still on one canvas. ONE dispatcher (`on` over the
rem  choice errorlevel) and ONE renderer (:wt_render) paint every screen; the
rem  canvas keeps the DOS palette and framed-panel/status-bar layout.
rem  Markers: [M-GFXTOOL] / [M-GFXTOOL-IMG|NOIMG] / [M-GFXTOOL-DONE] plus
rem  [M-GFXTOOL-UI|KEY|SELECT|PARAM|VIEW|SNAP]. Needs PHOTO.BMP and BALL.BMP
rem  (apps/push_assets.py).
rem =====================================================================
echo [M-GFXTOOL]
echo [M-DOS-GFXTOOL] screen=GFXTOOL-HEADLESS panels=0 head="NONE" theme=mono bars=0 table=N spark=0 status=N
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
rem AFTER the stamps so a sprite can never cover the title or the key legend.
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
rem v1.7 tile map + alpha layer: the SAME sheet, no second raster engine.
gfx tile create 0 GRID 0 4 1 16 16 /z:1
gfx tile set 0 0 0 0 /f:r
gfx tile draw 20 112 0
gfx layer bind 0 0 BADGE 8 8 128 /z:2
gfx layer dirty 0
gfx layer visible 0 on
gfx layer present 0
echo [M-RICH-GFXTOOL-TILELAYER] tile map + alpha layer stamped
rem Scaled BMP blit below the text and above the hline.
gfx image %APPDRIVE%/PHOTO.BMP 232 154 80 32
if errorlevel 1 echo [M-GFXTOOL-NOIMG]
if not errorlevel 1 echo [M-GFXTOOL-IMG]
rem ---- workbench state: one selected primitive + one live parameter ----
set PRIM=line
set PARAM=32
set VIEW=after
set STEPS=0
echo [M-GFXTOOL-UI] state=menu prim=line param=32 view=after
goto wt_loop

rem ---- ONE input dispatcher (choice keys l r c e p f t g a m + - v s q) --
:wt_dispatch
on %errorlevel% goto :sel_line,:sel_rect,:sel_circle,:sel_ellipse,:sel_poly,:sel_fill,:sel_text,:sel_gradient,:sel_atlas,:sel_composite,:param_up,:param_down,:toggle_view,:snap,:wt_done
goto wt_loop

:sel_line
gosub :select l line
goto wt_loop
:sel_rect
gosub :select r rect
goto wt_loop
:sel_circle
gosub :select c circle
goto wt_loop
:sel_ellipse
gosub :select e ellipse
goto wt_loop
:sel_poly
gosub :select p polygon
goto wt_loop
:sel_fill
gosub :select f fill
goto wt_loop
:sel_text
gosub :select t text
goto wt_loop
:sel_gradient
gosub :select g gradient
goto wt_loop
:sel_atlas
gosub :select a atlas
goto wt_loop
:sel_composite
gosub :select m composite
goto wt_loop

:select
set PRIM=%2
echo [M-GFXTOOL-KEY] key=%1 prim=%PRIM% param=%PARAM%
echo [M-GFXTOOL-SELECT] prim=%PRIM%
gosub :wt_ui
return

:param_up
set /a PARAM=%PARAM%+4
if %PARAM% GTR 96 set PARAM=96
echo [M-GFXTOOL-KEY] key=+ prim=%PRIM% param=%PARAM%
echo [M-GFXTOOL-PARAM] prim=%PRIM% param=%PARAM%
gosub :wt_ui
goto wt_loop

:param_down
set /a PARAM=%PARAM%-4
if %PARAM% LSS 8 set PARAM=8
echo [M-GFXTOOL-KEY] key=- prim=%PRIM% param=%PARAM%
echo [M-GFXTOOL-PARAM] prim=%PRIM% param=%PARAM%
gosub :wt_ui
goto wt_loop

:toggle_view
if "%VIEW%"=="before" goto view_after
set VIEW=before
goto view_done
:view_after
set VIEW=after
:view_done
echo [M-GFXTOOL-KEY] key=v prim=%PRIM% param=%PARAM%
echo [M-GFXTOOL-VIEW] view=%VIEW%
gosub :wt_ui
goto wt_loop

:snap
gfx save %APPDRIVE%/GFXTOOL.BMP
echo [M-GFXTOOL-KEY] key=s prim=%PRIM% param=%PARAM%
echo [M-GFXTOOL-SNAP]
goto wt_loop

:wt_ui
echo [M-GFXTOOL-UI] state=menu prim=%PRIM% param=%PARAM% view=%VIEW%
return

rem ---- bounded workbench loop: 24 key turns, each choice /T-bounded ----
:wt_loop
if %STEPS% GEQ 24 goto wt_done
set /a STEPS=%STEPS%+1
gosub :wt_render
choice /C:lrcepftgam+-vsq /N /T:q,10 "Tool"
goto wt_dispatch

rem ---- ONE renderer: framed DOS workbench + live primitive preview -----
:wt_render
gfx clear 0x101820
gfx rect 2 2 316 196 0x00FFFF
gfx rect 4 4 312 192 0x3A6EA5
gfx hline 4 20 312 0x00FFFF
gfx text 8 6 0xFFFFFF GFx WORKBENCH
gfx text 224 6 0x00FFFF PRIM %PRIM%  P=%PARAM%
rem Preview panel. "before" pins the original line/size-32 tour state.
gfx rect 12 26 196 124 0x3A6EA5
gfx text 16 28 0xFFFFFF PREVIEW / %VIEW%
set VPRIM=%PRIM%
set VP=%PARAM%
if "%VIEW%"=="before" set VPRIM=line
if "%VIEW%"=="before" set VP=32
set /a EY=%VP%/2
set /a CX=110
set /a CY=88
if "%VPRIM%"=="line" goto prev_line
if "%VPRIM%"=="rect" goto prev_rect
if "%VPRIM%"=="circle" goto prev_circle
if "%VPRIM%"=="ellipse" goto prev_ellipse
if "%VPRIM%"=="polygon" goto prev_poly
if "%VPRIM%"=="fill" goto prev_fill
if "%VPRIM%"=="text" goto prev_text
if "%VPRIM%"=="gradient" goto prev_gradient
if "%VPRIM%"=="atlas" goto prev_atlas
if "%VPRIM%"=="composite" goto prev_composite
goto prev_end
:prev_line
gfx lineaa 20 140 %VP% 40 0x00FFFF
gfx pixel 20 140 0xFFFFFF
gfx pixel %VP% 40 0xFFFFFF
goto prev_end
:prev_rect
gfx rect 40 44 %VP% 64 0x3A6EA5
gfx rectaa 40 44 %VP% 64 0xFFFFFF
goto prev_end
:prev_circle
gfx circleaa %CX% %CY% %VP% 0xFFFFFF
gfx lineaa %CX% %CY% %CX% %CY% 0xFFAA00
goto prev_end
:prev_ellipse
gfx ellipse %CX% %CY% %VP% %EY% 0x55FF55
goto prev_end
:prev_poly
gfx polygon 0x5599FF fill 30 140 %VP% 140 %CX% 44
goto prev_end
:prev_fill
gfx rect 40 44 %VP% %VP% 0xFFFFFF
gfx fill %CX% %CY% 0xFFAA00
goto prev_end
:prev_text
gfx text /scale:2 32 80 0xFFFFFF P4 %VP%
goto prev_end
:prev_gradient
gfx gradient 20 44 %VP% 80 0x101820 0x3A6EA5 h
gfx rectaa 20 44 %VP% 80 0x00FFFF
goto prev_end
:prev_atlas
gfx blitmany 0 /a:1,16,16 30 96 %VP% 96
goto prev_end
:prev_composite
gfx composite 0 %VP% 88 128
goto prev_end
:prev_end
rem Key legend panel (one renderer, no second UI engine).
gfx rect 214 26 102 124 0x3A6EA5
gfx text 218 30 0xFFFFFF PRIMITIVES
gfx text 218 42 0x00FFFF L line  R rect  C circle
gfx text 218 54 0x00FFFF E ellipse P poly
gfx text 218 66 0x00FFFF F fill   T text  G grad
gfx text 218 78 0x00FFFF A atlas  M composite
gfx text 218 94 0xFFAA00 +/- live param  V before/after
gfx text 218 106 0xFFAA00 S save snap  Q done
gfx rect 4 176 312 20 0x3A6EA5 fill
gfx text 8 178 0xFFFFFF one toolkit / one dispatcher / bounded keys
rem One sheet drives tile cells and the alpha layer; both use the ONE path.
gfx tile draw 20 112 0
gfx layer present all
gfx show
return

:wt_done
set UI=done
echo [M-GFXTOOL-UI] state=done prim=%PRIM% param=%PARAM% view=%VIEW%
rem Present and capture the final panel, then save the raw canvas as before.
gfx show
screenshot %APPDRIVE%/GFXTOOL_SCREEN.BMP
gfx save %APPDRIVE%/GFXTOOL.BMP
gfx close
echo [M-GFXTOOL-DONE]
return

rem Stop here: a label body that follows is still reachable by fall-through.
goto :eof

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
