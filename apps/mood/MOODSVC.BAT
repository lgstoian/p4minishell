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
echo [M-DOS-MOODSVC] screen=MOODSVC-HEADLESS panels=0 head="NONE" theme=mono bars=0 table=N spark=0 status=N
rem ---- v1.6 richer-language showcase -----------------------------------
rem [M-RICH-MOODSVC] one-shot headless str/map/forK/func tour; the sampling
rem loop and the SD-only store are unchanged.
echo [M-RICH-MOODSVC] headless language tour
set SW=sampler
str /rev SW WS
str /pad SW 9 SP .
map /set MSAMPLE pct 0
for /K %%k in (MSAMPLE) do echo [M-RICH-MOODSVC-K] %%k=%%l
func :msum 20 30
echo [M-RICH-MOODSVC-F] sum=%RESULT%
echo [M-RICH-MOODSVC-S] rev=%WS% pad=%SP%
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
goto :eof

:msum
return %1+%2
