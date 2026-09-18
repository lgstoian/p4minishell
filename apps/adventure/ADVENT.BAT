@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem =====================================================================
rem  ADVENT.BAT - a tiny pure-batch text adventure for P4MiniShell.
rem  Reference app: rooms, inventory, score, save/load, all in batch.
rem  Type lowercase commands. Markers: [M-ADV] / [M-ADV-WIN] / [M-ADV-QUIT].
rem =====================================================================
setlocal
set room=courtyard
set has_lantern=0
set has_key=0
set has_amulet=0
set score=0
set moves=0
echo [M-ADV]
goto room_courtyard

:back_to_room
if "%room%"=="hall" goto room_hall
if "%room%"=="library" goto room_library
if "%room%"=="tower" goto room_tower
if "%room%"=="cellar" goto room_cellar
if "%room%"=="garden" goto room_garden
goto room_courtyard

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
if "%has_key%"=="1" draw text 5 5 "Dusty shelves. The key hook stands empty."
if "%has_key%"=="0" draw text 5 5 "Dusty shelves. A brass key hangs on a hook."
draw text 5 6 "Exits: e"
goto prompt

:room_tower
set room=tower
draw clear screen
draw box 1 1 80 3 double "TOWER"
if "%has_amulet%"=="1" draw text 5 5 "Windy rooftop. The pedestal is empty."
if "%has_amulet%"=="0" draw text 5 5 "Windy rooftop. A glowing amulet rests on a pedestal."
draw text 5 6 "Exits: d"
goto prompt

:room_cellar
if not "%has_lantern%"=="1" goto cellar_dark
set room=cellar
draw clear screen
draw box 1 1 80 3 double "CELLAR"
draw text 5 5 "Your lantern lights a wine cellar. An iron chest sits here."
draw text 5 6 "Exits: w"
goto prompt
:cellar_dark
draw clear screen
draw text 5 5 "Pitch black below. You stumble back up to the hall."
delay 1200
goto room_hall

:room_garden
set room=garden
draw clear screen
draw box 1 1 80 3 double "GARDEN"
if "%has_lantern%"=="1" draw text 5 5 "Moonlit garden. The lantern post is bare."
if "%has_lantern%"=="0" draw text 5 5 "Moonlit garden. A lantern hangs by the gate."
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
if "%cmd%%room%"=="ncourtyard" goto room_hall
if "%cmd%%room%"=="shall" goto room_courtyard
if "%cmd%%room%"=="ecourtyard" goto room_garden
if "%cmd%%room%"=="ehall" goto room_cellar
if "%cmd%%room%"=="elibrary" goto room_hall
if "%cmd%%room%"=="whall" goto room_library
if "%cmd%%room%"=="wcellar" goto room_hall
if "%cmd%%room%"=="wgarden" goto room_courtyard
if "%cmd%%room%"=="uhall" goto room_tower
if "%cmd%%room%"=="dtower" goto room_hall
echo You cannot go that way.
goto prompt

:t_key
if not "%room%"=="library" goto deny
if "%has_key%"=="1" goto deny
set has_key=1
set /a score+=10
echo You take the brass key. [+10]
goto prompt
:t_lantern
if not "%room%"=="garden" goto deny
if "%has_lantern%"=="1" goto deny
set has_lantern=1
set /a score+=10
echo You take the lantern. [+10]
goto prompt
:t_amulet
if not "%room%"=="tower" goto deny
if "%has_amulet%"=="1" goto deny
set has_amulet=1
set /a score+=20
echo You take the glowing amulet. [+20]
goto prompt
:deny
echo Can't do that here.
goto prompt

:u_key
if not "%has_key%"=="1" goto deny
if not "%room%"=="cellar" goto deny
goto do_win
:u_lantern
if not "%has_lantern%"=="1" goto deny
echo The lantern burns steady. The dark holds no fear now.
goto prompt
:u_amulet
if not "%has_amulet%"=="1" goto deny
echo The amulet hums warm in your hand.
set /a score+=5
goto prompt

:inv
echo You carry:
set /a inv_n=%has_lantern% + %has_key% + %has_amulet%
if "%has_lantern%"=="1" echo " - lantern"
if "%has_key%"=="1" echo " - brass key"
if "%has_amulet%"=="1" echo " - glowing amulet"
if %inv_n% EQU 0 echo " - nothing yet"
goto prompt

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
echo set has_lantern=%has_lantern% >> ADVENT.SAV
echo set has_key=%has_key% >> ADVENT.SAV
echo set has_amulet=%has_amulet% >> ADVENT.SAV
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
