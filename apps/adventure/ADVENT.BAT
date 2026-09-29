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
set room=courtyard
set ITEM[lantern]=0
set ITEM[key]=0
set ITEM[amulet]=0
set score=0
set moves=0
echo [M-ADV]
title "Adventure"
goto room_courtyard

:back_to_room
switch %room% hall:room_hall library:room_library tower:room_tower cellar:room_cellar garden:room_garden /d:room_courtyard

:room_courtyard
set room=courtyard
draw clear screen
draw box 1 1 80 3 double "COURTYARD"
draw text 5 5 "A quiet stone courtyard. A hall lies north, a garden east."
draw text 5 6 "Exits: n e"
goto prompt

:room_hall
set room=hall
draw clear screen
draw box 1 1 80 3 double "GREAT HALL"
draw text 5 5 "A torch-lit hall. Doors west and east, stairs up, south out."
draw text 5 6 "Exits: s w e u"
goto prompt

:room_library
set room=library
draw clear screen
draw box 1 1 80 3 double "LIBRARY"
if "!ITEM[key]!"=="0" (draw text 5 5 "Dusty shelves. A brass key hangs on a hook.") else (draw text 5 5 "Dusty shelves. The key hook stands empty.")
draw text 5 6 "Exits: e"
goto prompt

:room_tower
set room=tower
draw clear screen
draw box 1 1 80 3 double "TOWER"
if "!ITEM[amulet]!"=="0" (draw text 5 5 "Windy rooftop. A glowing amulet rests on a pedestal.") else (draw text 5 5 "Windy rooftop. The pedestal is empty.")
draw text 5 6 "Exits: d"
goto prompt

:room_cellar
if "!ITEM[lantern]!"=="1" (set room=cellar & draw clear screen & draw box 1 1 80 3 double "CELLAR" & draw text 5 5 "Your lantern lights a wine cellar. An iron chest sits here." & draw text 5 6 "Exits: w" & goto prompt) else (draw clear screen & draw text 5 5 "Pitch black below. You stumble back up to the hall." & delay 1200 & goto room_hall)

:room_garden
set room=garden
draw clear screen
draw box 1 1 80 3 double "GARDEN"
if "!ITEM[lantern]!"=="0" (draw text 5 5 "Moonlit garden. A lantern hangs by the gate.") else (draw text 5 5 "Moonlit garden. The lantern post is bare.")
draw text 5 6 "Exits: w"
goto prompt

:prompt
set /a moves+=1
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
draw box 1 10 80 9 single "Help"
draw text 5 12 "move: n s e w u d    look    inv    score"
draw text 5 13 "take key/lantern/amulet    use key/lantern/amulet"
draw text 5 14 "save    load    quit"
draw text 5 16 "Goal: open the cellar chest. The cellar is dark."
pause
goto back_to_room

:save
echo set room=%room% > ADVENT.SAV
echo set ITEM[lantern]=!ITEM[lantern]! >> ADVENT.SAV
echo set ITEM[key]=!ITEM[key]! >> ADVENT.SAV
echo set ITEM[amulet]=!ITEM[amulet]! >> ADVENT.SAV
echo set score=%score% >> ADVENT.SAV
echo set moves=%moves% >> ADVENT.SAV
echo [M-ADV-SAVED]
dialog "Adventure" "Saved." "OK"
goto prompt

:load
if not exist ADVENT.SAV dialog "Adventure" "No save yet." "OK" & goto prompt
call ADVENT.SAV
echo [M-ADV-LOADED]
dialog "Adventure" "Loaded." "OK"
goto back_to_room

:do_win
set /a score+=50
echo [M-ADV-WIN]
draw clear screen
draw box 1 8 80 5 double "VICTORY"
draw text 20 10 "The chest opens: gold! Score %score% in %moves% moves." 14
tone 784 150
tone 1046 300
pause
return

:do_quit
echo [M-ADV-QUIT]
draw clear screen
return