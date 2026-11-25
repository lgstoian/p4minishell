@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem =====================================================================
rem  MEMLOG.BAT - ambient memory-logging service. Every tick it appends one
rem  `date,time,heap-free` CSV line to %APPDRIVE%/MEMLOG.CSV under the `memlog`
rem  mutex (so a second instance, or a manual sample, can never interleave
rem  a line) and sets the `memlogtick` event for any waiter. `mem` also
rem  reports the PSRAM pool; this service keeps the bounded heap series.
rem  Past 200 lines it rotates to MEMLOG.OLD first (two files, never growth
rem  without bound).
rem  Install with `service install memlog "MEMLOG" /restart`. Headless
rem  (mem/for/set/append/lock/signal/delay only).
rem  Markers: [M-MEMLOG] / [M-MEMLOG-SAMPLE].
rem  Tick ms comes from %1 (default 300000 = 5 min; the suite passes 2000).
rem =====================================================================
echo [M-MEMLOG]  logger started (mem reports heap + PSRAM)
echo [M-DOS-MEMLOG] screen=MEMLOG-HEADLESS panels=0 head="NONE" theme=mono bars=0 table=N spark=0 status=N
set every=300000
if not "%1"=="" set every=%1
rem ---- v1.6 richer-language showcase -----------------------------------
rem [M-RICH-MEMLOG] one-shot headless str/map/forK/func tour; the CSV tick
rem loop and the memlog mutex are unchanged.
echo [M-RICH-MEMLOG] headless language tour
set MW=memlog
str /rev MW WM
str /pad MW 8 MP .
map /set LOGCOLS date date
map /set LOGCOLS heap heap_free
for /K %%k in (LOGCOLS) do echo [M-RICH-MEMLOG-K] %%k=%%l
func :lograte 300000 1000
echo [M-RICH-MEMLOG-F] ticks=%RESULT%
echo [M-RICH-MEMLOG-S] rev=%WM% pad=%MP%
:loop
lock memlog
mem > %APPDRIVE%/_memlog.txt
set heapf=0
rem `for /f` does not expand variables in its file list: cd to the app drive
rem and read the scratch/CSV files by name. Accept one or two leading spaces
rem on the mem row (the formatter can emit either).
set OLDDIR=%CD%
cd %APPDRIVE%/
for /f "eol= tokens=1,2,5 delims==,b" %%a in (_memlog.txt) do if "%%a"=="  mem.heap: free" set heapf=%%b
for /f "eol= tokens=1,2,5 delims==,b" %%a in (_memlog.txt) do if "%%a"==" mem.heap: free" set heapf=%%b
if not exist %APPDRIVE%/MEMLOG.CSV write %APPDRIVE%/MEMLOG.CSV date,time,heap_free
set lines=0
for /f %%a in (MEMLOG.CSV) do set /a lines+=1
cd %OLDDIR%
if %lines% GTR 200 copy %APPDRIVE%/MEMLOG.CSV %APPDRIVE%/MEMLOG.OLD > NUL
if %lines% GTR 200 write %APPDRIVE%/MEMLOG.CSV date,time,heap_free
append %APPDRIVE%/MEMLOG.CSV %DATE%,%TIME%,%heapf%
unlock memlog
echo [M-MEMLOG-SAMPLE] heap %heapf%
signal memlogtick
delay %every%
if errorlevel 1 goto stopped
goto loop
:stopped
echo [M-MEMLOG] logger stopped
goto :eof

:lograte
return %1/%2
