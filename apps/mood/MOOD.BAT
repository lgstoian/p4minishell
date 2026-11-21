@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem =====================================================================
rem  MOOD.BAT - a live dashboard: samples `mem`, maps free-heap percent to
rem  an RGB/audio mood, and can snapshot mem/sysinfo for viewing.
rem  Reference app: redirection + for /f parsing + draw + for /A over an
rem  array + while/gosub sampling + draw bar trend + rgb + tone.
rem  Markers: [M-MOOD] / [M-MOOD-SAMPLE] / [M-MOOD-TREND].
rem =====================================================================
setlocal
if "%P4_APP_FEATURES%"=="1" gosub %APPROOT%/APPLIB.BAT::probe
echo [M-MOOD]
title "Mood"
rem Layout: the trend bars are authored for rows 8..12 of the 80x25 grid.
rem Shifting by half the extra rows centres the block on an 80x97 panel
rem without moving a single row on landscape. Bar width/label follow TUICOLS.
if "%TUICOLS%"=="" set TUICOLS=80
if "%TUIROWS%"=="" set TUIROWS=25
set /a MO=(%TUIROWS%-25)/2 > NUL
set /a BW=%TUICOLS%-40 > NUL
set /a LX=14+%BW%+4 > NUL
set /a TRH=2+%MO% > NUL
set /a S5=5+%MO% > NUL
set /a S6=6+%MO% > NUL
set /a S7=7+%MO% > NUL
set /a S8=8+%MO% > NUL
set /a S9=9+%MO% > NUL
draw fullscreen on
goto menu

:menu
draw clear screen
gosub %APPROOT%/APPLIB.BAT::page double "MOOD 💖"
draw text 5 2 "board mood from live heap (mem includes PSRAM)" 14
draw text 45 2 "local %TIME%" 7
list "Mood" "Live mood (5 samples)" "RGB demo" "Tone demo" "Snapshot" "Service sampler" "Back"
on %errorlevel%+1 goto :live,:rgb_demo,:tone_demo,:snapshot,:svcview,:back
goto back

:live
set round=0
rem A goto loop rather than `while ... do call`: the while wrapper keeps its
rem own batch frame for the whole tick, and the five samples then overflow the
rem command worker's 28 KB stack by ~200 B on MOOD's deepest path. The loop
rem below has identical behaviour and markers with one frame less.
:live_loop
if %round% GEQ 5 goto :live_trend
call :live_tick
goto :live_loop
:live_trend
draw clear screen
gosub %APPROOT%/APPLIB.BAT::page double "MOOD TREND"
draw text 5 %TRH% "heap free percent per sample" 14
for /A %%k in (SAMPLE) do call :barline %%k %%l
draw refresh
rem Save the trend page for the docs gallery before the dialog covers it.
screenshot %APPDRIVE%/MOOD_TREND.BMP
echo [M-MOOD-TREND]
dialog "Mood" "Sampling done." "OK"
goto menu

:live_tick
set /a round+=1
if %round% GTR 1 delay 2000
mem > %APPDRIVE%/_mood.txt
rem No del: `>` overwrites each round, and a hot-loop delete risks the
rem occasional slow-SD unlink stalling the worker (see bugs.md O6).
set heapf=0
set heapt=1
set pct=0
rem `for /f` does not expand variables inside its file list, so read the
rem scratch file from the app drive through a temporary cd. The mem row's
rem first token can carry one or two leading spaces; accept either spelling.
set OLDDIR=%CD%
cd %APPDRIVE%/
for /f "eol= tokens=1,2,5 delims==,b" %%a in (_mood.txt) do if "%%a"=="  mem.heap: free" set heapf=%%b
for /f "eol= tokens=1,2,5 delims==,b" %%a in (_mood.txt) do if "%%a"==" mem.heap: free" set heapf=%%b
for /f "eol= tokens=1,2,5 delims==,b" %%a in (_mood.txt) do if "%%a"=="  mem.heap: free" set heapt=%%c
for /f "eol= tokens=1,2,5 delims==,b" %%a in (_mood.txt) do if "%%a"==" mem.heap: free" set heapt=%%c
cd %OLDDIR%
if %heapt% GTR 100 set /a hunit=%heapt% / 100
if %heapt% GTR 100 set /a pct=%heapf% / %hunit%
set SAMPLE[%round%]=%pct%
echo [M-MOOD-SAMPLE]
echo heap %heapf%/%heapt% pct %pct%
draw clear screen
gosub %APPROOT%/APPLIB.BAT::page double "MOOD SAMPLE"
draw text 5 %S5% "heap free %heapf% / %heapt% bytes = %pct pct%" 14
rem %DATE%/%TIME% are the auto-synced local clock (SNTP + detected timezone).
draw text 5 %S6% "local %DATE% %TIME%" 7
if %pct% GEQ 80 goto mood_calm
if %pct% GEQ 50 goto mood_ok
if %pct% GEQ 20 goto mood_tense
goto mood_low
:mood_calm
draw text 5 %S7% "mood: CALM" 10
rgb 0 255 0
tone 523 120
goto live_done
:mood_ok
draw text 5 %S7% "mood: CONTENT" 11
rgb 0 0 255
tone 659 120
goto live_done
:mood_tense
draw text 5 %S7% "mood: TENSE" 14
rgb 255 165 0
tone 440 180
goto live_done
:mood_low
draw text 5 %S7% "mood: STRAINED" 12
rgb 255 0 0
tone 220 250
goto live_done
:live_done
return

:barline
rem %1 = sample index, %2 = pct; one trend bar per sample.
set /a my=7+%MO%+%1
draw text 2 %my% "sample %1"
draw bar 14 %my% %BW% %2 # . 10 0
draw text %LX% %my% "%2 pct" 7
return

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
rem `mem` reports internal heap/DMA and the PSRAM pool; keep the four views.
mem > %APPDRIVE%/_m.txt
sysinfo > %APPDRIVE%/_s.txt
rem Show the auto-synced local clock (SNTP + network-detected timezone).
timezone > %APPDRIVE%/_tz.txt
date > %APPDRIVE%/_d.txt
view %APPDRIVE%/_m.txt
view %APPDRIVE%/_s.txt
view %APPDRIVE%/_tz.txt
view %APPDRIVE%/_d.txt
goto menu

:svcview
rem Service sampler view: the headless MOODSVC service (installed with
rem `service install moodsampler "MOODSVC" /restart`) keeps sd:/MOODLAST.INI
rem fresh; that file is the SD-only service store by design, while the app's
rem own scratch files use %APPDRIVE%. Files are the IPC (no shared handles).
draw clear screen
gosub %APPROOT%/APPLIB.BAT::page double "MOOD SERVICE"
if not exist sd:/MOODLAST.INI goto svcnone
set svcf=0
set svct=1
set svcp=0
for /f "eol= tokens=1,2 delims==" %%a in (sd:/MOODLAST.INI) do if "%%a"=="heap_free" set svcf=%%b
for /f "eol= tokens=1,2 delims==" %%a in (sd:/MOODLAST.INI) do if "%%a"=="heap_total" set svct=%%b
for /f "eol= tokens=1,2 delims==" %%a in (sd:/MOODLAST.INI) do if "%%a"=="pct" set svcp=%%b
echo [M-MOOD-SVC]
draw text 5 %S5% "sampler heap %svcf% / %svct% = %svcp pct%" 14
draw bar 14 %S7% %BW% %svcp% # . 10 0
draw text 5 %S9% "Press any key..." 7
pause
goto menu
:svcnone
echo [M-MOODSVC-NONE]
draw text 5 %S5% "no sampler running" 12
draw text 5 %S6% "install: service install moodsampler MOODSVC /restart" 7
draw text 5 %S8% "Press any key..." 7
pause
goto menu

:back
rgb off
rem Leave the TUI: `draw clear screen` alone keeps TUI mode active and would
rem make the next gfx app refuse ("gfx: TUI mode is active").
draw fullscreen off
draw close
return