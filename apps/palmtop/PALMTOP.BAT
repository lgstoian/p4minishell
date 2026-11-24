@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem =====================================================================
rem  PALMTOP.BAT - reference app for the palmtop-parity primitives:
rem  timer/stopwatch, calc conversions, csv grid + =EXPR eval, crypt,
rem  bind and tcpterm. Pure batch; runs on the foreground worker.
rem  Markers: [M-PALM], [M-PALM-TIMER], [M-PALM-CALC], [M-PALM-CSV],
rem           [M-PALM-CRYPT], [M-PALM-BIND], [M-PALM-TCP].
rem =====================================================================
setlocal
if "%P4_APP_FEATURES%"=="1" gosub %APPROOT%/APPLIB.BAT::probe
echo [M-PALM]
title "PalmTop"
gosub :rich_palm
rem The sheet/TCP "Press any key" lines are authored at rows 15/20 of the
rem 80x25 grid. Anchor them just inside the bottom edge on a taller panel.
if "%TUIROWS%"=="" set TUIROWS=25
set /a PY15=%TUIROWS%-10 > NUL
set /a PY20=%TUIROWS%-5 > NUL
goto menu

rem ---- v1.6 richer-language showcase -----------------------------------
rem [M-RICH-PALM]: func/return computes a conversion, str /pad formats it and
rem map /set + for /K lists the unit aliases. No sheet/TCP state is changed.
:rich_palm
echo [M-RICH-PALM] conversion language tour
func :c2f 100
echo [M-RICH-PALM-F] c2f=%RESULT%
set PUNIT=mm
str /pad PUNIT 8 PP .
map /set UNITS c celsius
map /set UNITS f fahrenheit
for /K %%k in (UNITS) do echo [M-RICH-PALM-K] %%k=%%l
echo [M-RICH-PALM-S] pad=%PP%
return

:c2f
return %1*9/5+32

:menu
draw clear screen
gosub %APPROOT%/APPLIB.BAT::page double "PALMTOP ★"
draw run 52 2 "[M-RICH-PALM] func/str/map" /fg 14 /bg 0
draw text 5 2 "palmtop-parity primitives demo  (%APPNAME% on %APPDRIVE%)" 14
draw text 45 2 "local %TIME%" 7
list "PalmTop" "Calculator + conversions" "Stopwatch" "CSV sheet (=EXPR)" "Encrypt a note" "Bind an F-key" "TCP probe" "Back"
on %errorlevel%+1 goto :calc_demo,:timer,:sheet,:crypt,:bind,:tcp,:back
goto back

:calc_demo
draw clear screen
gosub %APPROOT%/APPLIB.BAT::page double "CALCULATOR"
ask "Value to convert (number)" 100
set v=%ASK_RESULT%
calc F=c2f(%v%)
calc MM=in2mm(%v%)
calc KG=lb2kg(%v%)
calc BIN=bin$(int(%v%))
echo [M-PALM-CALC]
echo %v% -> c2f=%F% mm=%MM% kg=%KG% bin=%BIN%
draw text 5 5 "%v%" 14
draw text 8 7 "C to F   = %F%" 1
draw text 8 8 "in to mm = %MM%" 1
draw text 8 9 "lb to kg = %KG%" 1
draw text 8 10 "binary   = %BIN%" 1
draw text 5 12 "Press any key..." 7
pause
goto menu

:timer
draw clear screen
gosub %APPROOT%/APPLIB.BAT::page double "STOPWATCH"
timer start demo
draw text 5 5 "Counting 2 s, then a lap..." 14
delay 2000
timer lap demo /v:LAP
delay 500
timer stop demo /v:TOTAL
echo [M-PALM-TIMER]
echo lap=%LAP% total=%TOTAL%
draw text 5 7 "lap   = %LAP% ms" 1
draw text 5 8 "total = %TOTAL% ms" 1
draw text 5 10 "Press any key..." 7
pause
goto menu

:sheet
rem Build a tiny sheet and let csv eval resolve the =EXPR cells.
write %APPDRIVE%/_palm.csv item,qty,price
append %APPDRIVE%/_palm.csv apple,3,0.5
append %APPDRIVE%/_palm.csv pear,10,=R3C2*R2C3
append %APPDRIVE%/_palm.csv total,,=R2C3+R3C3
echo [M-PALM-CSV]
draw clear screen
gosub %APPROOT%/APPLIB.BAT::page double "CSV"
csv eval %APPDRIVE%/_palm.csv
draw text 5 %PY15% "Press any key..." 7
pause
del %APPDRIVE%/_palm.csv > NUL
goto menu

:crypt
write %APPDRIVE%/_palm_note.txt secret palmtop note
crypt lock %APPDRIVE%/_palm_note.txt %APPDRIVE%/_palm_note.enc /p:open123
crypt unlock %APPDRIVE%/_palm_note.enc %APPDRIVE%/_palm_note.out /p:open123
echo [M-PALM-CRYPT]
draw clear screen
gosub %APPROOT%/APPLIB.BAT::page double "CRYPT"
draw text 5 5 "Locked then unlocked _palm_note.txt" 14
type < %APPDRIVE%/_palm_note.out > %APPDRIVE%/_palm_view.txt
draw text 5 7 "Plaintext recovered:" 7
view %APPDRIVE%/_palm_view.txt
del %APPDRIVE%/_palm_note.txt > NUL
del %APPDRIVE%/_palm_note.enc > NUL
del %APPDRIVE%/_palm_note.out > NUL
del %APPDRIVE%/_palm_view.txt > NUL
goto menu

:bind
draw clear screen
gosub %APPROOT%/APPLIB.BAT::page double "BIND"
bind F9 sysinfo
bind F10 dir
bind
echo [M-PALM-BIND]
draw text 5 6 "F9 = sysinfo, F10 = dir (this session)" 14
draw text 5 8 "Press those keys at the prompt, then run BIND.BAT" 7
draw text 5 10 "Press any key..." 7
pause
goto menu

:tcp
draw clear screen
gosub %APPROOT%/APPLIB.BAT::page double "TCP"
draw text 5 5 "Probing the on-board httpd on loopback..." 14
rem Offline (no Wi-Fi/httpd) the connect is refused cleanly; ERRORLEVEL 1.
tcpterm 127.0.0.1 80 /t:6 GET / HTTP/1.0\r\n\r\n
echo [M-PALM-TCP]
draw text 5 %PY20% "Press any key..." 7
pause
goto menu

:back
draw clear screen
return
