@echo off
rem =====================================================================
rem  FUN.BAT - Demos and games: melody, RGB effects, number guessing,
rem  and a calc playground. Enhanced TUI with draw and fullscreen.
rem =====================================================================

:main
draw clear screen
draw box 1 1 80 3 single
draw text 35 2 "FUN / DEMOS" 14
draw box 1 4 80 10 single "Demos"
draw text 5 6 "Melody, RGB, guessing game, calc, file browser"
list "Fun / Demos" "Melody (TUI)" "RGB effects" "Number guessing" "Calc playground" "Browse demo file" "TUI Demo" "Back"
if errorlevel 255 goto :back
if errorlevel 6 goto :tui_demo
if errorlevel 5 goto :back
if errorlevel 4 goto :browse_demo
if errorlevel 3 goto :calc
if errorlevel 2 goto :rgb
if errorlevel 1 goto :guess
goto :melody

:tui_demo
tui fullscreen on
draw clear screen
draw box 1 1 80 3 double "TUI DEMO"
draw box 1 4 80 10 single "Boxes"
draw box 5 6 20 6 single "Single"
draw box 30 6 20 6 double "Double"
draw box 55 6 20 6 rounded "Rounded"
draw text 5 15 "Colors: PowerShell palette via ANSI"
ansi 1;31m Red  ansi 1;32m Green  ansi 1;33m Yellow
ansi 1;34m Blue  ansi 1;35m Magenta  ansi 1;36m Cyan
draw text 5 18 "Box chars: single double rounded + symbols"
draw text 5 20 "Press any key..."
pause
tui fullscreen off
draw clear screen
goto main

:melody
tui fullscreen on
draw clear screen
draw box 1 1 80 3 double "MELODY"
draw text 30 2 "Playing a little tune..." 14
draw box 5 5 70 10 single "Notes"
tone 523 180
draw text 10 7 "C5 (523Hz)" 11
delay 450
tone 659 180
draw text 10 8 "E5 (659Hz)" 11
delay 450
tone 784 180
draw text 10 9 "G5 (784Hz)" 11
delay 450
tone 1046 350
draw text 10 10 "C6 (1046Hz)" 11
delay 600
tone 784 180
draw text 10 11 "G5 (784Hz)" 11
delay 450
tone 659 180
draw text 10 12 "E5 (659Hz)" 11
delay 450
tone 523 500
draw text 10 13 "C5 (523Hz)" 11
delay 300
tui fullscreen off
dialog "Melody" "Done." "OK"
goto main

:rgb
draw clear screen
draw box 1 1 80 3 single
draw text 35 2 "RGB LED (WS2812)" 14
list "RGB LED" "Rainbow" "Breath" "Solid red" "Solid blue" "Off" "Back"
if errorlevel 255 goto main
if errorlevel 5 goto main
if errorlevel 4 goto main
if errorlevel 3 goto rgb_off
if errorlevel 2 goto rgb_blue
if errorlevel 1 goto rgb_red
if errorlevel 0 goto rgb_rainbow
goto main
:rgb_rainbow
draw text 5 10 "Rainbow effect..." 14
rgb rainbow 4
if errorlevel 1 dialog "RGB" "LED unavailable." "OK"
pause
goto main
:rgb_breath
draw text 5 10 "Breath effect..." 14
rgb breath 3
if errorlevel 1 dialog "RGB" "LED unavailable." "OK"
pause
goto main
:rgb_red
rgb 255 0 0
if errorlevel 1 dialog "RGB" "LED unavailable." "OK"
draw box 10 10 20 5 single
draw text 12 12 "RED" 12
pause
goto main
:rgb_blue
rgb 0 0 255
draw box 10 10 20 5 single
draw text 12 12 "BLUE" 9
pause
goto main
:rgb_off
rgb off
pause
goto main

:guess
tui fullscreen on
draw clear screen
draw box 1 1 80 3 double "NUMBER GUESSING"
draw text 20 2 "I am thinking of a number 1..100." 14
calc secret = INT(RAN# * 100) + 1
set tries=0
:guess_loop
set /a tries+=1
draw box 5 5 70 5 single "Guess"
ask /v:g "Guess (1..100)"
if errorlevel 1 tui fullscreen off & dialog "Guess" "Cancelled." "OK" & goto main
if %g% EQU %secret% goto guess_win
if %g% GTR %secret% dialog "Guess" "Too high!" "OK"
if %g% LSS %secret% dialog "Guess" "Too low!" "OK"
if %tries% GEQ 10 tui fullscreen off & dialog "Guess" "Out of tries! It was %secret%." "OK" & goto main
goto guess_loop
:guess_win
tui fullscreen off
dialog "Guess" "Correct! It took you %tries% tries." "OK"
goto main

:calc
draw clear screen
draw box 1 1 80 3 single
draw text 32 2 "CALC PLAYGROUND" 14
ask /v:expr "Expression (e.g. 2^10, sin(30))"
if errorlevel 1 goto main
if "%expr%"=="" goto main
calc %expr% > _calc.txt
draw box 5 5 70 10 single "Result"
view _calc.txt
del /p _calc.txt
goto main

:browse_demo
browse /v:demo "Pick a file to view"
if errorlevel 1 goto main
view %demo%
goto main

:back
draw clear screen
goto :eof
