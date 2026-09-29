@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem =====================================================================
rem  ELITE.BAT - turn-based space trader (batch adaptation of the DOS Elite
rem  idea: trade, travel, pirates; no real-time flight — the engine is
rem  blocking-input + full-label flushes, so combat is choice-driven).
rem  Systems and goods live in SYSNAME[]/MULT[]/PRICE[]/HOLD[] arrays; the
rem  buy/sell lists return the good index straight into `set /a g=%%err`,
rem  prices/holds read via set /a live-name lookups (PRICE[%%g%%]...), and
rem  the dock menu dispatches through one `on` statement. Saves echo the
rem  array slots in set form (call ADVENT.SAV pattern). Markers:
rem  [M-ELITE] / [M-ELITE-DOCKED] / [M-ELITE-SAVED] / [M-ELITE-DONE].
rem  Foreground-only (draw/list refuse in bg, see :nogfx).
rem =====================================================================
setlocal enabledelayedexpansion
echo [M-ELITE]
title "Elite"
if exist ELITE.SAV call ELITE.SAV
if not defined credits set credits=100
if not defined sys set sys=0
if not defined HOLD[0] set HOLD[0]=0
if not defined HOLD[1] set HOLD[1]=0
if not defined HOLD[2] set HOLD[2]=0
if not defined hull set hull=100
set SYSNAME[0]=Lave
set SYSNAME[1]=Zaonce
set SYSNAME[2]=Diso
set SYSNAME[3]=Leesti
set MULT[0]=8
set MULT[1]=12
set MULT[2]=5
set MULT[3]=15
draw hold on
if errorlevel 1 goto nogfx
goto dock

:dock
set sname=!SYSNAME[%sys%]!
set /a mult=MULT[%sys%]
set /a PRICE[0]=10+%mult%
set /a PRICE[1]=40+%mult%*2
set /a PRICE[2]=120-%mult%*3
echo [M-ELITE-DOCKED] %sname%
draw clear
draw box 1 1 80 3 double ELITE
draw text 3 2 %sname% station
draw table 3 5 14 0 "Good|Price|Hold" "Food|%PRICE[0]%|%HOLD[0]%" "Textiles|%PRICE[1]%|%HOLD[1]%" "Computers|%PRICE[2]%|%HOLD[2]%"
draw text 3 14 Credits %credits%
draw text 3 15 Hull %hull%%
draw text 50 14 Buy low
draw text 50 15 sell high
draw refresh
list "Elite" "Buy" "Sell" "Travel" "Save" "Quit"
on %errorlevel%+1 goto :buy,:sell,:travel,:save,:done
goto done

:buy
list "Buy" "Food %PRICE[0]%" "Textiles %PRICE[1]%" "Computers %PRICE[2]%" "Back"
if %errorlevel% GEQ 3 goto dock
set /a g=%errorlevel%
set /a pr=PRICE[%g%]
goto buyask
:buyask
ask /v:qty "Tons" 1
set /a cost=%pr% * %qty%
if %cost% GTR %credits% goto broke
set /a credits-=cost
set /a HOLD[%g%]+=%qty%
tone 660 80
goto dock

:sell
list "Sell" "Food %PRICE[0]%" "Textiles %PRICE[1]%" "Computers %PRICE[2]%" "Back"
if %errorlevel% GEQ 3 goto dock
set /a g=%errorlevel%
set /a have=HOLD[%g%]
set /a pr=PRICE[%g%]
goto sellask
:sellask
ask /v:qty "Tons" 1
if %qty% GTR %have% goto short
set /a gain=%pr% * %qty%
set /a credits+=gain
set /a HOLD[%g%]-=%qty%
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
if %errorlevel% GEQ 4 goto dock
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
echo set HOLD[0]=%HOLD[0]% >> ELITE.SAV
echo set HOLD[1]=%HOLD[1]% >> ELITE.SAV
echo set HOLD[2]=%HOLD[2]% >> ELITE.SAV
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