@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem =====================================================================
rem  MOODSVC.BAT - headless heap sampler feeding the MOOD dashboard.
rem  Install with `service install moodsampler "MOODSVC" /restart` and open
rem  the "Service sampler" view in MOOD.BAT: the dashboard reads
rem  sd:/MOODLAST.INI while this service keeps it fresh. Coordination is
rem  files + the `moodtick` event (no shared handles to rendezvous, and the
rem  job-private environment cannot leak into the dashboard either way).
rem  Runs headless (mem/for/set/signal/delay only). Markers: [M-MOODSVC].
rem =====================================================================
echo [M-MOODSVC] sampler started
:loop
mem > sd:/_moods.txt
set heapf=0
set heapt=1
set pct=0
for /f "eol= tokens=1,2,5 delims==,b" %%a in (sd:/_moods.txt) do if "%%a"=="  mem.heap: free" set heapf=%%b
for /f "eol= tokens=1,2,5 delims==,b" %%a in (sd:/_moods.txt) do if "%%a"=="  mem.heap: free" set heapt=%%c
if %heapt% GTR 100 set /a hunit=%heapt% / 100
if %heapt% GTR 100 set /a pct=%heapf% / %hunit%
write sd:/MOODLAST.INI heap_free=%heapf%
append sd:/MOODLAST.INI heap_total=%heapt%
append sd:/MOODLAST.INI pct=%pct%
append sd:/MOODLAST.INI sampled=%TIME%
echo [M-MOODSVC] pct %pct%
signal moodtick
delay 10000
if errorlevel 1 goto stopped
goto loop
:stopped
echo [M-MOODSVC] sampler stopped
