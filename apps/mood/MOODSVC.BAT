@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem =====================================================================
rem  MOODSVC.BAT - headless heap sampler feeding the MOOD dashboard.
rem  Install with `service install moodsampler "MOODSVC" /restart` and open
rem  the "Service sampler" view in MOOD.BAT: the dashboard reads
rem  sd:/MOODLAST.INI (SD-only: the service store; the registry is SD-only too)
rem  while this service keeps it fresh. Scratch uses %APPDRIVE%, so a sampler
rem  installed from USB keeps its own temp file on the stick. Coordination is
rem  files + the `moodtick` event (no shared handles to rendezvous, and the
rem  job-private environment cannot leak into the dashboard either way).
rem  Runs headless (mem/for/set/signal/delay only). Markers: [M-MOODSVC].
rem =====================================================================
echo [M-MOODSVC]  sampler started
:loop
rem Scratch follows the app/service drive; MOODLAST.INI stays on SD because
rem the service registry and its published store are SD-only by design.
mem > %APPDRIVE%/_moods.txt
set heapf=0
set heapt=1
set pct=0
rem `for /f` does not expand variables in its file list: cd to the app drive
rem and read the scratch file by name. Accept one or two leading spaces.
set OLDDIR=%CD%
cd %APPDRIVE%/
for /f "eol= tokens=1,2,5 delims==,b" %%a in (_moods.txt) do if "%%a"=="  mem.heap: free" set heapf=%%b
for /f "eol= tokens=1,2,5 delims==,b" %%a in (_moods.txt) do if "%%a"==" mem.heap: free" set heapf=%%b
for /f "eol= tokens=1,2,5 delims==,b" %%a in (_moods.txt) do if "%%a"=="  mem.heap: free" set heapt=%%c
for /f "eol= tokens=1,2,5 delims==,b" %%a in (_moods.txt) do if "%%a"==" mem.heap: free" set heapt=%%c
cd %OLDDIR%
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
