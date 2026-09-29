@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem =====================================================================
rem  TCMD.BAT - dual-pane file commander (batch adaptation of the DOS
rem  Total Commander idea). Two live panes rendered with `draw list` over
rem  `dir /o:gn /b` captures, a bright cursor row per pane, an active-pane
rem  accent, path titles, a status line and an F-key-style hint bar. All
rem  file ops stay on the stock verbs via the F2-style menu; `browse` is
rem  still reachable for full-screen picking. The key-poll and the F2
rem  menu dispatch through `on` (choice/list 0-based index -> 1-based on
rem  target), and pane-conditional ops use single-line if groups.
rem  Keys (serial: letter then Enter): j/k move, t switch pane, o open,
rem  x menu, s swap panes, 1/2 jump pane to a root, q quit.
rem  Markers: [M-TCMD] / [M-TCMD-R] / [M-TCMD-OP] / [M-TCMD-DONE].
rem  Foreground-only: draw/browse refuse in background jobs (see :nogfx).
setlocal
echo [M-TCMD]
title "Commander"
set LD=/
set RD=/APPS
set LCUR=1
set RCUR=1
set PA=L
set ST=ready
draw hold on
if errorlevel 1 goto nogfx
rem Full screen: reclaim the header and on-screen keyboard rows so the
rem 80x25 manager grid is fully visible. Restored on exit (`:end`).
draw fullscreen on
goto refresh

:refresh
rem Relist a pane only when its directory changed since the last frame:
rem `dir` is the expensive SD op, and j/k navigation does not change dirs.
rem Create the scratch dir only when missing: a redundant `md` on an
rem existing directory is an error and would spam the pane each refresh.
if not exist sd:/tmp md sd:/tmp > NUL
if "%LD%"=="%LDONE%" goto ldone
dir /o:gn /b sd:%LD% > sd:/tmp/_LT.txt
rem Line counts via draw list's /count (a for-loop counter would flood:
rem this shell's `set /a` echoes its result).
draw list 1 4 39 17 sd:/tmp/_LT.txt /count:LN /countonly
set LDONE=%LD%
:ldone
if "%RD%"=="%RDONE%" goto rdone
dir /o:gn /b sd:%RD% > sd:/tmp/_RT.txt
draw list 41 4 39 17 sd:/tmp/_RT.txt /count:RN /countonly
set RDONE=%RD%
:rdone
draw clear
rem Clamp cursors to the (possibly empty) listings.
if %LCUR% LSS 1 set LCUR=1
if %LCUR% GTR %LN% set LCUR=%LN%
if %LN% EQU 0 set LCUR=1
if %RCUR% LSS 1 set RCUR=1
if %RCUR% GTR %RN% set RCUR=%RN%
if %RN% EQU 0 set RCUR=1
rem Scroll offsets keep the cursor inside the visible page (h-2 rows).
set /a LTOP=%LCUR% - 14
if %LTOP% LSS 1 set LTOP=1
set /a RTOP=%RCUR% - 14
if %RTOP% LSS 1 set RTOP=1
rem Active pane gets the bright list; inactive is dim.
set LAC=7
set RAC=7
if %PA%==L set LAC=15
if %PA%==R set RAC=15
rem Frame.
draw box 1 1 80 3 double TCMD
draw text 3 2 "dual-pane commander"
draw text 55 2 "1:/ 2:/APPS"
draw list 1 4 39 17 sd:/tmp/_LT.txt %LAC% 16 /top:%LTOP% /cursor:%LCUR% /title:%LD%
draw list 41 4 39 17 sd:/tmp/_RT.txt %RAC% 16 /top:%RTOP% /cursor:%RCUR% /title:%RD%
draw text 2 21 "pane %PA%  left=%LN% right=%RN%   status: %ST%"
draw text 1 23 "j/k move  t pane  o open  x menu  s swap  1/2 root  p snap  q quit"
draw refresh
echo [M-TCMD-R]
choice /C:jktosx12qzp /N /T:z,30 key?
on %errorlevel% goto :down,:up,:toggle,:openf,:swap,:menu,:drive1,:drive2,:done,:refresh,:snap
goto refresh

:down
if %PA%==R (set /a RCUR+=1) else (set /a LCUR+=1)
set ST=down
goto refresh

:up
if %PA%==R (set /a RCUR-=1) else (set /a LCUR-=1)
set ST=up
goto refresh

:toggle
if %PA%==L (set PA=R) else (set PA=L)
goto refresh

:drive1
if %PA%==R (set RD=/) else (set LD=/)
goto refresh

:drive2
if %PA%==R (set RD=/APPS) else (set LD=/APPS)
goto refresh

:openf
rem Resolve the cursor entry name from the active pane's capture, then
rem either enter it (directory) or open it (file).
if %PA%==R goto openR
set /a SKP=%LCUR% - 1
set FN=
rem `eol=` disables the cmd.exe `;`-comment filter: a directory entry may
rem legitimately begin with `;`, so the pane listing must not drop it.
for /f "eol= skip=%SKP%" %%a in (sd:/tmp/_LT.txt) do set FN=%%a & goto openL
:openL
if "%FN%"=="" goto refresh
cd sd:%LD%/%FN%
if errorlevel 1 goto viewL
set LD=%LD%/%FN%
set LCUR=1
set ST=entered %FN%
goto refresh
:viewL
browse /v:FN sd:%LD%
set ST=opened
goto refresh
:openR
set /a SKP=%RCUR% - 1
set FN=
for /f "eol= skip=%SKP%" %%a in (sd:/tmp/_RT.txt) do set FN=%%a & goto openR2
:openR2
if "%FN%"=="" goto refresh
cd sd:%RD%/%FN%
if errorlevel 1 goto viewR
set RD=%RD%/%FN%
set RCUR=1
set ST=entered %FN%
goto refresh
:viewR
browse /v:FN sd:%RD%
set ST=opened
goto refresh

:swap
set T=%LD%
set LD=%RD%
set RD=%T%
set T=%LCUR%
set LCUR=%RCUR%
set RCUR=%T%
set ST=swapped
echo [M-TCMD-OP] swap
goto refresh

rem ---- F2-style operation menu (stock verbs, kept from the previous
rem      commander so every op still works) ----
:forcerelist
set LDONE=
set RDONE=
set ST=refreshed
goto refresh

:menu
list "Commander" "Copy to other" "Move to other" "Delete file" "Mkdir here" "Rename file" "View file" "Edit file" "Change path" "Refresh" "Quit"
on %errorlevel%+1 goto :copyf,:movef,:delf,:mkdirf,:renf,:viewf,:editf,:chdir,:forcerelist,:done
goto refresh

:copyf
set ST=copy to %RD%
browse /v:F sd:%LD%
if errorlevel 1 goto refresh
copy %F% sd:%RD%
set ST=copied
set LDONE=
set RDONE=
echo [M-TCMD-OP] copy %F%
goto refresh

:movef
set ST=move to %RD%
browse /v:F sd:%LD%
if errorlevel 1 goto refresh
move %F% sd:%RD%
set ST=moved
set LDONE=
set RDONE=
echo [M-TCMD-OP] move %F%
goto refresh

:delf
browse /v:F sd:%LD%
if errorlevel 1 goto refresh
ask /v:YN "Delete %F%? (yes/no)" no
if "%YN%"=="yes" del %F% > NUL
if "%YN%"=="yes" set ST=deleted
set LDONE=
set RDONE=
echo [M-TCMD-OP] del %F%
goto refresh

:mkdirf
ask /v:N "New dir name"
if "%N%"=="" goto refresh
if %PA%==R (md sd:%RD%/%N% > NUL & set ST=mkdir %RD%) else (md sd:%LD%/%N% > NUL & set ST=mkdir %LD%)
set LDONE=
set RDONE=
echo [M-TCMD-OP] mkdir %N%
goto refresh

:renf
browse /v:F sd:%LD%
if errorlevel 1 goto refresh
ask /v:N "New name"
if "%N%"=="" goto refresh
ren %F% %N%
set ST=renamed
set LDONE=
set RDONE=
echo [M-TCMD-OP] ren %F%
goto refresh

:viewf
browse /v:F sd:%LD%
if errorlevel 1 goto refresh
view %F%
goto refresh

:editf
browse /v:F sd:%LD%
if errorlevel 1 goto refresh
edit %F%
goto refresh

:chdir
ask /v:LD "Left path" %LD%
ask /v:RD "Right path" %RD%
set LCUR=1
set RCUR=1
set ST=paths changed
echo [M-TCMD-OP] chdir
goto refresh

:done
echo [M-TCMD-DONE]
goto end

:snap
rem Save the current commander screen to the SD card (test/debug aid).
screenshot TCMD.BMP
set ST=snapped
goto refresh

:nogfx
echo TCMD needs the foreground display (no draw/browse in background jobs)
goto end

:end
draw hold off
draw close
draw fullscreen off
rem No del needed: the captures are overwritten each refresh (see O6).
endlocal