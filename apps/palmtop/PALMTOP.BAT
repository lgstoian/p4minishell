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
echo [M-PALM]
goto menu

:menu
draw clear screen
draw box 1 1 80 3 double "PALMTOP"
draw text 5 2 "palmtop-parity primitives demo" 14
draw text 45 2 "local %TIME%" 7
list "PalmTop" "Calculator + conversions" "Stopwatch" "CSV sheet (=EXPR)" "Encrypt a note" "Bind an F-key" "TCP probe" "Back"
if errorlevel 255 goto back
if errorlevel 6 goto back
if errorlevel 5 goto tcp
if errorlevel 4 goto bind
if errorlevel 3 goto crypt
if errorlevel 2 goto sheet
if errorlevel 1 goto timer
goto calc_demo

:calc_demo
draw clear screen
draw box 1 1 80 3 double "CALCULATOR"
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
draw box 1 1 80 3 double "STOPWATCH"
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
write _palm.csv item,qty,price
append _palm.csv apple,3,0.5
append _palm.csv pear,10,=R3C2*R2C3
append _palm.csv total,,=R2C3+R3C3
echo [M-PALM-CSV]
draw clear screen
draw box 1 1 80 3 double "CSV"
csv eval _palm.csv
draw text 5 15 "Press any key..." 7
pause
del _palm.csv
goto menu

:crypt
write _palm_note.txt secret palmtop note
crypt lock _palm_note.txt _palm_note.enc /p:open123
crypt unlock _palm_note.enc _palm_note.out /p:open123
echo [M-PALM-CRYPT]
draw clear screen
draw box 1 1 80 3 double "CRYPT"
draw text 5 5 "Locked then unlocked _palm_note.txt" 14
type _palm_note.out > _palm_view.txt
draw text 5 7 "Plaintext recovered:" 7
view _palm_view.txt
del _palm_note.txt
del _palm_note.enc
del _palm_note.out
del _palm_view.txt
goto menu

:bind
draw clear screen
draw box 1 1 80 3 double "BIND"
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
draw box 1 1 80 3 double "TCP"
draw text 5 5 "Probing the on-board httpd on loopback..." 14
rem Offline (no Wi-Fi/httpd) the connect is refused cleanly; ERRORLEVEL 1.
tcpterm 127.0.0.1 80 /t:6 GET / HTTP/1.0\r\n\r\n
echo [M-PALM-TCP]
draw text 5 20 "Press any key..." 7
pause
goto menu

:back
draw clear screen
return
