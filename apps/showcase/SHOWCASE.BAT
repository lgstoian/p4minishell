@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem =====================================================================
rem  SHOWCASE.BAT - flagship DOS dashboard for P4MiniShell.
rem
rem  One 80x25 CP437 screen tiles the batch, TUI, modal, webapp and gfx
rem  families into a single coherent panel layout. Every primitive comes
rem  from APPLIB.BAT (one renderer, one DOS palette decision point) and
rem  every verb below is shipped firmware: draw box/line/text/run/bar/
rem  table/list, tui dump, mpick/mprog/dialog, webapp dump/css, gfx status.
rem  Hardware-only surfaces stay behind the 1..5 menu keys so a host lane
rem  can run the language extract honestly instead of faking a render.
rem
rem  Markers: [M-SHOWCASE] / [M-RICH-SHOWCASE] / [M-SHOWCASE-DONE].
rem =====================================================================
setlocal
if "%P4_APP_FEATURES%"=="1" gosub %APPROOT%/APPLIB.BAT::probe
echo [M-SHOWCASE]
title "DOS Showcase"
set DOS_THEME=ice
echo [M-DOS-SHOWCASE] screen=SHOWCASE-MASTER panels=6 head="DOS SHOWCASE" theme=ice bars=3 table=Y spark=2 status=Y
gosub :rich_showcase
goto :screen

rem ---- additive language proof (same APPLIB, no second engine) ---------
:rich_showcase
echo [M-RICH-SHOWCASE] str+map+forK+func
gosub %APPROOT%/APPLIB.BAT::rich SHOWCASE
return

rem ---- the flagship screen --------------------------------------------
:screen
draw clear screen
if "%TUICOLS%"=="" set TUICOLS=80
if "%TUIROWS%"=="" set TUIROWS=25
gosub %APPROOT%/APPLIB.BAT::header "DOS SHOWCASE" "[M-RICH-SHOWCASE] one screen / five surfaces"
gosub %APPROOT%/APPLIB.BAT::tile 1 5 39 7 double "BATCH - LANGUAGE"
gosub %APPROOT%/APPLIB.BAT::table 3 8 "Verb|Proof|State" "str|rev/pad|OK" "map|for /K|OK" "func|RESULT|OK"
gosub %APPROOT%/APPLIB.BAT::tile 42 5 38 7 single "TUI - CELL GRID"
gosub %APPROOT%/APPLIB.BAT::sparkline 45 9 3 30 55 42 78 60 90
gosub %APPROOT%/APPLIB.BAT::meter 45 12 32 86 "FRAME 86%"
gosub %APPROOT%/APPLIB.BAT::tile 1 13 26 6 double "MODAL - INPUT"
draw text 4 16 "1..5 probe    q quit" 15
gosub %APPROOT%/APPLIB.BAT::tile 29 13 25 6 single "WEBAPP - DOM"
draw text 32 16 "webapp dump /a" 15
gosub %APPROOT%/APPLIB.BAT::tile 56 13 24 6 single "GFX - CANVAS"
gosub %APPROOT%/APPLIB.BAT::themebox 59 15 18
draw text 58 14 "tile/pcl/layer" 11
gosub %APPROOT%/APPLIB.BAT::statusbar 21 "80x25 CP437  ICE  panels=6" "1 batch 2 tui 3 modal 4 web 5 gfx  q quit"
draw refresh
echo [M-SHOWCASE-READY]
list "DOS Showcase" "1 Batch language" "2 TUI cell grid" "3 Modal input" "4 Webapp DOM" "5 GFX canvas" "q Back"
on %errorlevel%+1 goto :sc_batch,:sc_tui,:sc_modal,:sc_web,:sc_gfx,:sc_done
goto :screen

:sc_batch
echo [M-SHOWCASE-BATCH]
gosub %APPROOT%/APPLIB.BAT::rich SHOWCASE-BATCH
pause
goto :screen

:sc_tui
echo [M-SHOWCASE-TUI]
tui stats
tui dump 1 6 /a
pause
goto :screen

:sc_modal
echo [M-SHOWCASE-MODAL]
mpick /items Batch^|TUI^|Modal^|Web^|GFX /title "Surface picker" /v:SCPICK
mprog /run "Painting cells" /total 3 /step 1 /t:2 /v:SCPROG
dialog "Modal" "Picked %SCPICK% on the ONE modal runtime." "OK"
goto :screen

:sc_web
echo [M-SHOWCASE-WEB]
if not exist %APPDRIVE%/WEB/RICH.HTML goto :sc_web_skip
webapp open /no %APPDRIVE%/WEB/RICH.HTML
webapp dump 1 8 /a
webapp css
goto :screen
:sc_web_skip
echo [M-SHOWCASE-WEB-SKIP] RICH.HTML not deployed
pause
goto :screen

:sc_gfx
echo [M-SHOWCASE-GFX]
echo [M-SHOWCASE-GFX-TOOLS] lineaa rectaa circleaa gradient composite blitmany image tile particles layer
gfx status
gfx stats
pause
goto :screen

:sc_done
echo [M-SHOWCASE-DONE]
draw fullscreen off
endlocal
exit /b 0