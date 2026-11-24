@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem =====================================================================
rem  ADVENT.BAT - a tiny pure-batch text adventure for P4MiniShell.
rem  Reference app: the move map is one `switch %cmd%%room%` statement
rem  (glued single-token match values), rooms use single-line `if (true)
rem  else (false)` groups, and inventory lives in an ITEM[name] array
rem  walked with `for` + delayed `!ITEM[%1]!` reads. Multi-word verbs
rem  (take key) stay an if-chain: switch matches are split on spaces.
rem  Type lowercase commands. Markers: [M-ADV] / [M-ADV-WIN] / [M-ADV-QUIT].
rem =====================================================================
setlocal enabledelayedexpansion
if "%P4_APP_FEATURES%"=="1" gosub %APPROOT%/APPLIB.BAT::probe
set room=courtyard
set ITEM[lantern]=0
set ITEM[key]=0
set ITEM[amulet]=0
set score=0
set moves=0
rem Layout: the room prose and the help block are authored for the 80x25
rem grid. Shifting them by half the extra rows keeps the authored rows 5/6
rem and 12-16 exactly on landscape while centring them on an 80x97 panel.
if "%TUICOLS%"=="" set TUICOLS=80
if "%TUIROWS%"=="" set TUIROWS=25
set /a AO=(%TUIROWS%-25)/2 > NUL
set /a RT=5+%AO% > NUL
set /a EX=6+%AO% > NUL
set /a H12=12+%AO% > NUL
set /a H13=13+%AO% > NUL
set /a H14=14+%AO% > NUL
set /a H16=16+%AO% > NUL
echo [M-ADV]
title "Adventure"
draw hold on
draw fullscreen on
gosub :rich_adv
goto room_courtyard

rem ---- v1.6 richer-language showcase -----------------------------------
rem [M-RICH-ADV] proves str /rev|/count|/pad, map /set + for /K and
rem func/return publish %RESULT% using the real interpreter. Game state is
rem untouched: every value below is a local of this gosub frame.
:rich_adv
echo [M-RICH-ADV] language tour
draw text 5 3 "[M-RICH-ADV] str+map+forK+func live" 11
set RWORD=lantern
str /rev RWORD RV
str /count RWORD n CN
str /pad RWORD 10 RP .
map /set ROOMMAP hall north
map /set ROOMMAP garden east
for /K %%k in (ROOMMAP) do echo [M-RICH-ADV-K] %%k=%%l
func :advscore 10 20
echo [M-RICH-ADV-F] result=%RESULT%
echo [M-RICH-ADV-S] rev=%RV% count=%CN% pad=%RP%
return

:back_to_room
switch %room% hall:room_hall library:room_library tower:room_tower cellar:room_cellar garden:room_garden /d:room_courtyard

:room_courtyard
set room=courtyard
draw clear screen
gosub %APPROOT%/APPLIB.BAT::page double "COURTYARD ⚔"
draw text 5 %RT% "A quiet stone courtyard. A hall lies north, a garden east." 15
draw text 5 %EX% "Exits: n e    (%APPNAME% on %APPDRIVE%)"
goto prompt

:room_hall
set room=hall
draw clear screen
gosub %APPROOT%/APPLIB.BAT::page double "GREAT HALL ⚔"
draw text 5 %RT% "A torch-lit hall. Doors west and east, stairs up, south out." 15
draw text 5 %EX% "Exits: s w e u"
goto prompt

:room_library
set room=library
draw clear screen
gosub %APPROOT%/APPLIB.BAT::page double "LIBRARY ⚔"
rem Keep multi-word prose out of the if-group: this engine's group parser
rem takes only the first token of a quoted string inside `( ... )`.
if "!ITEM[key]!"=="0" (set desc=Dusty shelves. A brass key hangs on a hook.) else (set desc=Dusty shelves. The key hook stands empty.)
draw text 5 %RT% "%desc%" 15
draw text 5 %EX% "Exits: e"
goto prompt

:room_tower
set room=tower
draw clear screen
gosub %APPROOT%/APPLIB.BAT::page double "TOWER ⚔"
if "!ITEM[amulet]!"=="0" (set desc=Windy rooftop. A glowing amulet rests on a pedestal.) else (set desc=Windy rooftop. The pedestal is empty.)
draw text 5 %RT% "%desc%" 15
draw text 5 %EX% "Exits: d"
goto prompt

:room_cellar
if not "!ITEM[lantern]!"=="1" goto cellar_dark
set room=cellar
draw clear screen
gosub %APPROOT%/APPLIB.BAT::page double "CELLAR ⚔"
draw text 5 %RT% "Your lantern lights a wine cellar. An iron chest sits here." 15
draw text 5 %EX% "Exits: w"
goto prompt
:cellar_dark
draw clear screen
draw text 5 %RT% "Pitch black below. You stumble back up to the hall." 15
delay 1200
goto room_hall

:room_garden
set room=garden
draw clear screen
gosub %APPROOT%/APPLIB.BAT::page double "GARDEN ⚔"
if "!ITEM[lantern]!"=="0" (set desc=Moonlit garden. A lantern hangs by the gate.) else (set desc=Moonlit garden. The lantern post is bare.)
draw text 5 %RT% "%desc%" 15
draw text 5 %EX% "Exits: w"
goto prompt

:prompt
rem A `snap` command re-enters the current room and takes the BMP here, before
rem the input modal covers the page (the page is freshly presented).
if "%wantsnap%"=="1" goto snap_now
set /a moves+=1
rem Present the room before the input modal: the modal is a surface over the
rem committed TUI page, so the docs capture (and a player) sees where they are.
draw refresh
rem Literal dashes (not a Markdown rule): /raw keeps the bytes identical.
echo /raw ---
ask /v:cmd "What now? (help)"
if errorlevel 1 goto prompt
if "%cmd%"=="quit" goto do_quit
if "%cmd%"=="help" goto help
if "%cmd%"=="look" goto back_to_room
if "%cmd%"=="inv" goto inv
if "%cmd%"=="score" goto show_score
if "%cmd%"=="save" goto save
if "%cmd%"=="load" goto load
if "%cmd%"=="snap" goto do_snap
if "%cmd%"=="n" goto move
if "%cmd%"=="s" goto move
if "%cmd%"=="e" goto move
if "%cmd%"=="w" goto move
if "%cmd%"=="u" goto move
if "%cmd%"=="d" goto move
if "%cmd%"=="take key" goto t_key
if "%cmd%"=="take lantern" goto t_lantern
if "%cmd%"=="take amulet" goto t_amulet
if "%cmd%"=="use key" goto u_key
if "%cmd%"=="use lantern" goto u_lantern
if "%cmd%"=="use amulet" goto u_amulet
echo I do not understand "%cmd%".
goto prompt

:move
switch %cmd%%room% ncourtyard:room_hall shall:room_courtyard ecourtyard:room_garden ehall:room_cellar elibrary:room_hall whall:room_library wcellar:room_hall wgarden:room_courtyard uhall:room_tower dtower:room_hall
echo You cannot go that way.
goto prompt

:t_key
if not "%room%"=="library" goto deny
if "!ITEM[key]!"=="1" goto deny
set ITEM[key]=1
set /a score+=10
echo You take the brass key. [+10]
goto prompt
:t_lantern
if not "%room%"=="garden" goto deny
if "!ITEM[lantern]!"=="1" goto deny
set ITEM[lantern]=1
set /a score+=10
echo You take the lantern. [+10]
goto prompt
:t_amulet
if not "%room%"=="tower" goto deny
if "!ITEM[amulet]!"=="1" goto deny
set ITEM[amulet]=1
set /a score+=20
echo You take the glowing amulet. [+20]
goto prompt
:deny
echo Can't do that here.
goto prompt

:u_key
if not "!ITEM[key]!"=="1" goto deny
if not "%room%"=="cellar" goto deny
goto do_win
:u_lantern
if not "!ITEM[lantern]!"=="1" goto deny
echo The lantern burns steady. The dark holds no fear now.
goto prompt
:u_amulet
if not "!ITEM[amulet]!"=="1" goto deny
echo The amulet hums warm in your hand.
set /a score+=5
goto prompt

:inv
echo You carry:
set /a inv_n=ITEM[lantern]+ITEM[key]+ITEM[amulet]
for %%i in (lantern key amulet) do call :invcheck %%i
if %inv_n% EQU 0 echo " - nothing yet"
goto prompt
:invcheck
rem %1 = item name; delayed !ITEM[%1]! resolves the per-name slot and the
rem nested if-groups map the slot name to its display label.
if "!ITEM[%1]!"=="1" (if "%1"=="key" (echo " - brass key") else (if "%1"=="amulet" (echo " - glowing amulet") else (echo " - lantern")))
return

:show_score
echo Score: %score% in %moves% moves.
goto prompt

:help
gosub %APPROOT%/APPLIB.BAT::panel 10 9 single "Help"
draw text 5 %H12% "move: n s e w u d    look    inv    score"
draw text 5 %H13% "take key/lantern/amulet    use key/lantern/amulet"
draw text 5 %H14% "save    load    quit"
draw text 5 %H16% "Goal: open the cellar chest. The cellar is dark."
pause
goto back_to_room

:save
echo set room=%room% > %APPDRIVE%/ADVENT.SAV
echo set ITEM[lantern]=!ITEM[lantern]! >> %APPDRIVE%/ADVENT.SAV
echo set ITEM[key]=!ITEM[key]! >> %APPDRIVE%/ADVENT.SAV
echo set ITEM[amulet]=!ITEM[amulet]! >> %APPDRIVE%/ADVENT.SAV
echo set score=%score% >> %APPDRIVE%/ADVENT.SAV
echo set moves=%moves% >> %APPDRIVE%/ADVENT.SAV
echo [M-ADV-SAVED]
dialog "Adventure" "Saved." "OK"
goto prompt

:load
if not exist %APPDRIVE%/ADVENT.SAV dialog "Adventure" "No save yet." "OK" & goto prompt
call %APPDRIVE%/ADVENT.SAV
echo [M-ADV-LOADED]
dialog "Adventure" "Loaded." "OK"
goto back_to_room

:do_snap
rem Documented debug/docs aid. The room routine is re-entered so the page is
rem freshly painted; :prompt takes the BMP before the input modal opens.
set wantsnap=1
goto back_to_room

:snap_now
set wantsnap=
rem Capture immediately: with draw-hold off the shell repaints the transcript
rem over a presented page after a short delay, so a pause here captures black.
draw refresh
delay 300
screenshot %APPDRIVE%/ADVENT.BMP
echo [M-ADV-SNAP]
goto prompt

:do_win
set /a score+=50
echo [M-ADV-WIN]
draw clear screen
rem The victory panel keeps its authored 5 rows and centres itself on the live
rem grid (rows 8..12 / text 10 on the classic 25-row grid).
if "%TUIROWS%"=="" set TUIROWS=25
set /a VY=%TUIROWS%/2-4 > NUL
set /a VT=%VY%+2 > NUL
gosub %APPROOT%/APPLIB.BAT::bar %VY% 5 double "VICTORY"
draw text 20 %VT% "The chest opens: gold! Score %score% in %moves% moves." 14
tone 784 150
tone 1046 300
pause
return

:do_quit
echo [M-ADV-QUIT]
draw clear screen
draw hold off
draw fullscreen off
draw close
return