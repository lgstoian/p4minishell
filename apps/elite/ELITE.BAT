@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem =====================================================================
rem  ELITE.BAT - turn-based space trader (batch adaptation of the DOS Elite
rem  idea: trade, travel, pirates; no real-time flight — the engine is
rem  blocking-input + full-label flushes, so combat is choice-driven).
rem  Systems/goods branch unrolled (no batch indirection); market renders
rem  as one draw table from %vars%; saves via echo-set/call (ADVENT.SAV
rem  pattern). Markers: [M-ELITE] / [M-ELITE-DOCKED] / [M-ELITE-SAVED] /
rem  [M-ELITE-DONE]. Foreground-only (draw/list refuse in bg, see :nogfx).
rem =====================================================================
setlocal
echo [M-ELITE]
if exist ELITE.SAV call ELITE.SAV
if "%credits%"=="" set credits=100
if "%sys%"=="" set sys=0
if "%c0%"=="" set c0=0
if "%c1%"=="" set c1=0
if "%c2%"=="" set c2=0
if "%hull%"=="" set hull=100
draw hold on
if errorlevel 1 goto nogfx
goto dock

:dock
if %sys% EQU 0 set sname=Lave
if %sys% EQU 1 set sname=Zaonce
if %sys% EQU 2 set sname=Diso
if %sys% EQU 3 set sname=Leesti
if %sys% EQU 0 set mult=8
if %sys% EQU 1 set mult=12
if %sys% EQU 2 set mult=5
if %sys% EQU 3 set mult=15
set /a p0=10 + %mult%
set /a p1=40 + %mult% * 2
set /a p2=120 - %mult% * 3
echo [M-ELITE-DOCKED] %sname%
draw clear
draw box 1 1 80 3 double ELITE
draw text 3 2 %sname% station
draw table 3 5 14 0 "Good|Price|Hold" "Food|%p0%|%c0%" "Textiles|%p1%|%c1%" "Computers|%p2%|%c2%"
draw text 3 14 Credits %credits%
draw text 3 15 Hull %hull%%
draw text 50 14 Buy low
draw text 50 15 sell high
draw refresh
list "Elite" "Buy" "Sell" "Travel" "Save" "Quit"
if errorlevel 255 goto done
if errorlevel 4 goto done
if errorlevel 3 goto save
if errorlevel 2 goto travel
if errorlevel 1 goto sell
goto buy

:buy
list "Buy" "Food %p0%" "Textiles %p1%" "Computers %p2%" "Back"
if errorlevel 255 goto dock
if errorlevel 3 goto dock
if errorlevel 2 goto buy2
if errorlevel 1 goto buy1
goto buy0
:buy2
set g=2
set pr=%p2%
goto buyask
:buy1
set g=1
set pr=%p1%
goto buyask
:buy0
set g=0
set pr=%p0%
goto buyask
:buyask
ask /v:qty "Tons" 1
set /a cost=%pr% * %qty%
if %cost% GTR %credits% goto broke
set /a credits-=cost
if %g% EQU 0 set /a c0+=%qty%
if %g% EQU 1 set /a c1+=%qty%
if %g% EQU 2 set /a c2+=%qty%
tone 660 80
goto dock

:sell
list "Sell" "Food %p0%" "Textiles %p1%" "Computers %p2%" "Back"
if errorlevel 255 goto dock
if errorlevel 3 goto dock
if errorlevel 2 goto sell2
if errorlevel 1 goto sell1
goto sell0
:sell2
set g=2
set pr=%p2%
goto sellask
:sell1
set g=1
set pr=%p1%
goto sellask
:sell0
set g=0
set pr=%p0%
goto sellask
:sellask
ask /v:qty "Tons" 1
if %g% EQU 0 set have=%c0%
if %g% EQU 1 set have=%c1%
if %g% EQU 2 set have=%c2%
if %qty% GTR %have% goto short
set /a gain=%pr% * %qty%
set /a credits+=gain
if %g% EQU 0 set /a c0-=%qty%
if %g% EQU 1 set /a c1-=%qty%
if %g% EQU 2 set /a c2-=%qty%
tone 520 80
goto dock

:broke
draw text 50 17 broke
draw refresh
delay 1200
goto dock

:short
draw text 50 17 none aboard
draw refresh
delay 1200
goto dock

:travel
list "Travel" "Lave" "Zaonce" "Diso" "Leesti" "Back"
if errorlevel 255 goto dock
if errorlevel 4 goto dock
set sys=%errorlevel%
set /a credits-=%sys% * 2 + 5
set /a ev=%RANDOM% / 9000
if %ev% EQU 0 goto pirate
tone 440 120
goto dock

:pirate
draw text 50 17 pirates
draw refresh
choice /C:fr "Fight or run?"
if errorlevel 2 goto flee
if %hull% LSS 40 goto dead
set /a hull-=25
set /a credits+=150
draw text 50 18 won +150
draw refresh
delay 1500
goto dock

:flee
set /a hull-=10
if %hull% LEQ 0 goto dead
draw text 50 18 escaped -10 hull
draw refresh
delay 1500
goto dock

:save
echo set credits=%credits% > ELITE.SAV
echo set sys=%sys% >> ELITE.SAV
echo set c0=%c0% >> ELITE.SAV
echo set c1=%c1% >> ELITE.SAV
echo set c2=%c2% >> ELITE.SAV
echo set hull=%hull% >> ELITE.SAV
echo [M-ELITE-SAVED]
draw text 50 18 saved
draw refresh
delay 1000
goto dock

:dead
draw text 50 18 DESTROYED
draw refresh
screenshot ELITE.BMP
echo [M-ELITE-DONE] destroyed credits=%credits%
goto end

:done
echo [M-ELITE-DONE] credits=%credits% sys=%sys%
goto end

:nogfx
echo ELITE needs the foreground display (no draw/list in background jobs)
goto end

:end
draw hold off
draw close
endlocal
