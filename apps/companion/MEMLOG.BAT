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
echo [M-MEMLOG] ▒ logger started (mem reports heap + PSRAM)
set every=300000
if not "%1"=="" set every=%1
:loop
lock memlog
mem > %APPDRIVE%/_memlog.txt
set heapf=0
for /f "eol= tokens=1,2,5 delims==,b" %%a in (%APPDRIVE%/_memlog.txt) do if "%%a"=="  mem.heap: free" set heapf=%%b
if not exist %APPDRIVE%/MEMLOG.CSV write %APPDRIVE%/MEMLOG.CSV date,time,heap_free
set lines=0
for /f %%a in (%APPDRIVE%/MEMLOG.CSV) do set /a lines+=1
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
