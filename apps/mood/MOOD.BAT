@echo off
rem =====================================================================
rem  MOOD.BAT - a live dashboard: samples `mem`, maps free-heap percent to
rem  an RGB/audio mood, and can snapshot mem/sysinfo for viewing.
rem  Reference app: redirection + for /f parsing + draw + rgb + tone.
rem  Markers: [M-MOOD] / [M-MOOD-SAMPLE].
rem =====================================================================
setlocal
echo [M-MOOD]
goto menu

:menu
draw clear screen
draw box 1 1 80 3 double "MOOD"
draw text 5 2 "board mood from live heap" 14
draw text 45 2 "local %TIME%" 7
list "Mood" "Live mood (5 samples)" "RGB demo" "Tone demo" "Snapshot" "Back"
if errorlevel 255 goto back
if errorlevel 4 goto back
if errorlevel 3 goto snapshot
if errorlevel 2 goto tone_demo
if errorlevel 1 goto rgb_demo
goto live

:live
set round=0
:live_loop
set /a round+=1
mem > _mood.txt
rem No del: `>` overwrites each round, and a hot-loop delete risks the
rem occasional slow-SD unlink stalling the worker (see bugs.md O6).
set heapf=0
set heapt=1
set pct=0
for /f "tokens=1,2,5 delims==,b" %%a in (_mood.txt) do if "%%a"=="  mem.heap: free" set heapf=%%b
for /f "tokens=1,2,5 delims==,b" %%a in (_mood.txt) do if "%%a"=="  mem.heap: free" set heapt=%%c
if %heapt% GTR 100 set /a hunit=%heapt% / 100
if %heapt% GTR 100 set /a pct=%heapf% / %hunit%
echo [M-MOOD-SAMPLE]
echo heap %heapf%/%heapt% pct %pct%
draw clear screen
draw box 1 1 80 3 double "MOOD SAMPLE"
draw text 5 5 "heap free %heapf% / %heapt% bytes = %pct pct%" 14
rem %DATE%/%TIME% are the auto-synced local clock (SNTP + detected timezone).
draw text 5 6 "local %DATE% %TIME%" 7
if %pct% GEQ 80 goto mood_calm
if %pct% GEQ 50 goto mood_ok
if %pct% GEQ 20 goto mood_tense
goto mood_low
:mood_calm
draw text 5 7 "mood: CALM" 10
rgb 0 255 0
tone 523 120
goto live_next
:mood_ok
draw text 5 7 "mood: CONTENT" 11
rgb 0 0 255
tone 659 120
goto live_next
:mood_tense
draw text 5 7 "mood: TENSE" 14
rgb 255 165 0
tone 440 180
goto live_next
:mood_low
draw text 5 7 "mood: STRAINED" 12
rgb 255 0 0
tone 220 250
goto live_next
:live_next
if %round% LSS 5 delay 2000 & goto live_loop
dialog "Mood" "Sampling done." "OK"
goto menu

:rgb_demo
draw clear screen
draw text 5 5 "Rainbow..." 14
rgb rainbow 3
draw text 5 6 "Breath..." 14
rgb breath 2
rgb off
goto menu

:tone_demo
tone 523 150
delay 200
tone 659 150
delay 200
tone 784 250
goto menu

:snapshot
mem > _m.txt
sysinfo > _s.txt
rem Show the auto-synced local clock (SNTP + network-detected timezone).
timezone > _tz.txt
date > _d.txt
view _m.txt
view _s.txt
view _tz.txt
view _d.txt
goto menu

:back
rgb off
draw clear screen
goto :eof
