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
if "%P4_APP_FEATURES%"=="1" gosub %APPROOT%/APPLIB.BAT::probe
echo [M-TCMD]
title "Commander"
gosub :rich_tcmd
set LD=/
set RD=/APPS
set LCUR=1
set RCUR=1
set PA=L
set ST=ready
rem Layout (TCMD is full-screen): the panes split the live grid and the two
rem info lines keep the bottom rows. The shell publishes the grid as
rem %TUICOLS%/%TUIROWS% (80x25 on the landscape reference board, 80x97 on a
rem portrait panel), so the pane height/width are derived, never 80x25.
rem LH pane height (17 on the 25-row grid), LW/RX/RW the two pane columns
rem (1..39 / 41..79), SY/HY the status and hint rows (21/23), HX the
rem right-hand shortcut column.
if "%TUICOLS%"=="" set TUICOLS=80
if "%TUIROWS%"=="" set TUIROWS=25
set /a LH=%TUIROWS%-8 > NUL
set /a LW=(%TUICOLS%-2)/2 > NUL
set /a RX=%LW%+2 > NUL
set /a RW=%TUICOLS%-%RX% > NUL
set /a SY=%TUIROWS%-4 > NUL
set /a HY=%TUIROWS%-2 > NUL
set /a HX=%TUICOLS%-25 > NUL
draw hold on
if errorlevel 1 goto nogfx
rem Full screen: reclaim the header and on-screen keyboard rows so the
rem 80x25 manager grid is fully visible. Restored on exit (`:end`).
draw fullscreen on
goto refresh

rem ---- v1.6 richer-language showcase -----------------------------------
rem [M-RICH-TCMD] exercises map /set|/values, for /K, str /count|/pad and
rem func/return with the real batch interpreter; pane state is not altered.
:rich_tcmd
echo [M-RICH-TCMD] language tour
map /set PANES left /
map /set PANES right /APPS
for /K %%k in (PANES) do echo [M-RICH-TCMD-K] %%k=%%l
map /values PANES PV
set LBL=commander
str /count LBL m CM
str /pad LBL 14 PL .
func :tcolsum 39 2
echo [M-RICH-TCMD-F] width=%RESULT%
echo [M-RICH-TCMD-S] panes=%PV[0]%,%PV[1]% m=%CM% pad=%PL%
return

:tcolsum
return %1+%2

:refresh
rem Relist a pane only when its directory changed since the last frame:
rem `dir` is the expensive SD op, and j/k navigation does not change dirs.
rem Create the scratch dir only when missing: a redundant `md` on an
rem existing directory is an error and would spam the pane each refresh.
if not exist %APPDRIVE%/tmp md %APPDRIVE%/tmp > NUL
if "%LD%"=="%LDONE%" goto ldone
dir /o:gn /b %APPDRIVE%%LD% > %APPDRIVE%/tmp/_LT.txt
rem Line counts via draw list's /count (a for-loop counter would flood:
rem this shell's `set /a` echoes its result).
draw list 1 4 %LW% %LH% %APPDRIVE%/tmp/_LT.txt /count:LN /countonly
set LDONE=%LD%
:ldone
if "%RD%"=="%RDONE%" goto rdone
dir /o:gn /b %APPDRIVE%%RD% > %APPDRIVE%/tmp/_RT.txt
draw list %RX% 4 %RW% %LH% %APPDRIVE%/tmp/_RT.txt /count:RN /countonly
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
set /a LTOP=%LCUR% - %LH% + 3 > NUL
if %LTOP% LSS 1 set LTOP=1
set /a RTOP=%RCUR% - %LH% + 3 > NUL
if %RTOP% LSS 1 set RTOP=1
rem Active pane gets the bright list; inactive is dim.
set LAC=7
set RAC=7
if %PA%==L set LAC=15
if %PA%==R set RAC=15
rem Frame.
gosub %APPROOT%/APPLIB.BAT::bar 1 3 double "Commander ★"
rem [M-RICH-TCMD] styled status uses the v1.6 `draw run /fg /bg` colour path.
draw run 40 2 "[M-RICH-TCMD] map/forK/str/func" /fg 11 /bg 1
draw text 3 2 "dual-pane commander  (%APPNAME% on %APPDRIVE%)"
draw text %HX% 2 "1:/ 2:/APPS"
draw list 1 4 %LW% %LH% %APPDRIVE%/tmp/_LT.txt %LAC% 16 /top:%LTOP% /cursor:%LCUR% /title:%LD%
draw list %RX% 4 %RW% %LH% %APPDRIVE%/tmp/_RT.txt %RAC% 16 /top:%RTOP% /cursor:%RCUR% /title:%RD%
draw text 2 %SY% "pane %PA%  left=%LN% right=%RN%   status: %ST%"
draw text 1 %HY% "j/k move  t pane  o open  x menu  s swap  1/2 root  p snap  q quit"
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
rem `for /f` does not expand %APPDRIVE% in its file list: cd to the app
rem drive for the read and restore before using the entry. `eol=` disables
rem the cmd.exe `;`-comment filter: a directory entry may legitimately begin
rem with `;`, so the pane listing must not drop it.
set OLDDIR=%CD%
cd %APPDRIVE%/
for /f "eol= skip=%SKP%" %%a in (tmp/_LT.txt) do set FN=%%a & goto openL
:openL
cd %OLDDIR%
if "%FN%"=="" goto refresh
cd %APPDRIVE%%LD%/%FN%
if errorlevel 1 goto viewL
set LD=%LD%/%FN%
set LCUR=1
set ST=entered %FN%
goto refresh
:viewL
browse /v:FN %APPDRIVE%%LD%
set ST=opened
goto refresh
:openR
set /a SKP=%RCUR% - 1
set FN=
set OLDDIR=%CD%
cd %APPDRIVE%/
for /f "eol= skip=%SKP%" %%a in (tmp/_RT.txt) do set FN=%%a & goto openR2
:openR2
cd %OLDDIR%
if "%FN%"=="" goto refresh
cd %APPDRIVE%%RD%/%FN%
if errorlevel 1 goto viewR
set RD=%RD%/%FN%
set RCUR=1
set ST=entered %FN%
goto refresh
:viewR
browse /v:FN %APPDRIVE%%RD%
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
browse /v:F %APPDRIVE%%LD%
if errorlevel 1 goto refresh
copy %F% %APPDRIVE%%RD%
set ST=copied
set LDONE=
set RDONE=
echo [M-TCMD-OP] copy %F%
goto refresh

:movef
set ST=move to %RD%
browse /v:F %APPDRIVE%%LD%
if errorlevel 1 goto refresh
move %F% %APPDRIVE%%RD%
set ST=moved
set LDONE=
set RDONE=
echo [M-TCMD-OP] move %F%
goto refresh

:delf
browse /v:F %APPDRIVE%%LD%
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
if %PA%==R (md %APPDRIVE%%RD%/%N% > NUL & set ST=mkdir %RD%) else (md %APPDRIVE%%LD%/%N% > NUL & set ST=mkdir %LD%)
set LDONE=
set RDONE=
echo [M-TCMD-OP] mkdir %N%
goto refresh

:renf
browse /v:F %APPDRIVE%%LD%
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
browse /v:F %APPDRIVE%%LD%
if errorlevel 1 goto refresh
view %F%
goto refresh

:editf
browse /v:F %APPDRIVE%%LD%
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
screenshot %APPDRIVE%/TCMD.BMP
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