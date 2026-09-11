@echo off
@echo off
rem =====================================================================
rem  TCMD.BAT - dual-pane file commander (batch adaptation of the DOS
rem  Total Commander idea). Two live panes rendered with `draw list` over
rem  `dir /o:gn /b` captures, a bright cursor row per pane, an active-pane
rem  accent, path titles, a status line and an F-key-style hint bar. All
rem  file ops stay on the stock verbs via the F2-style menu; `browse` is
rem  still reachable for full-screen picking.
rem  Keys (serial: letter then Enter): j/k move, t switch pane, o open,
rem  x menu, s swap panes, 1/2 jump pane to a root, q quit.
rem  Markers: [M-TCMD] / [M-TCMD-OP] / [M-TCMD-DONE].
rem  Foreground-only: draw/browse refuse in background jobs (see :nogfx).
setlocal
echo [M-TCMD]
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
md sd:/tmp
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
if errorlevel 11 goto snap
if errorlevel 10 goto refresh
if errorlevel 9 goto done
if errorlevel 8 goto drive2
if errorlevel 7 goto drive1
if errorlevel 6 goto menu
if errorlevel 5 goto swap
if errorlevel 4 goto openf
if errorlevel 3 goto toggle
if errorlevel 2 goto up
if errorlevel 1 goto down
goto refresh

:down
if %PA%==L set /a LCUR+=1
if %PA%==R set /a RCUR+=1
set ST=down
goto refresh

:up
if %PA%==L set /a LCUR-=1
if %PA%==R set /a RCUR-=1
set ST=up
goto refresh

:toggle
if %PA%==L goto toR
set PA=L
goto refresh
:toR
set PA=R
goto refresh

:drive1
if %PA%==R goto d1R
set LD=/
goto refresh
:d1R
set RD=/
goto refresh

:drive2
if %PA%==R goto d2R
set LD=/APPS
goto refresh
:d2R
set RD=/APPS
goto refresh

:openf
rem Resolve the cursor entry name from the active pane's capture, then
rem either enter it (directory) or open it (file).
if %PA%==R goto openR
set /a SKP=%LCUR% - 1
set FN=
for /f "skip=%SKP%" %%a in (_LT.txt) do set FN=%%a & goto openL
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
for /f "skip=%SKP%" %%a in (_RT.txt) do set FN=%%a & goto openR2
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
if errorlevel 255 goto refresh
if errorlevel 9 goto done
if errorlevel 8 goto forcerelist
if errorlevel 7 goto chdir
if errorlevel 6 goto editf
if errorlevel 5 goto viewf
if errorlevel 4 goto renf
if errorlevel 3 goto mkdirf
if errorlevel 2 goto delf
if errorlevel 1 goto movef
goto copyf

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
if "%YN%"=="yes" del %F%
if "%YN%"=="yes" set ST=deleted
set LDONE=
set RDONE=
echo [M-TCMD-OP] del %F%
goto refresh

:mkdirf
ask /v:N "New dir name"
if "%N%"=="" goto refresh
if %PA%==R goto mkR
md sd:%LD%/%N%
set ST=mkdir %LD%
goto mkd
:mkR
md sd:%RD%/%N%
set ST=mkdir %RD%
:mkd
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
