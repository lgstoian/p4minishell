@echo off
rem =====================================================================
rem  TCMD.BAT - dual-pane file commander (batch adaptation of the DOS
rem  Total Commander idea). Two remembered dirs (LD/RD), chrome via draw,
rem  selection via the native `browse` modal (no duplicated listing UI),
rem  ops via `list` + `ask` + stock file verbs. Sequential by design: one
rem  worker, so copies block (no live bg progress); the bar pulses around
rem  wildcard ops. Serial: list/browse take numbers, `q` cancels.
rem  Markers: [M-TCMD] / [M-TCMD-OP] / [M-TCMD-DONE].
rem  Foreground-only: draw/browse refuse in background jobs (see :nogfx).
rem =====================================================================
setlocal
echo [M-TCMD]
set LD=/
set RD=/APPS
draw hold on
if errorlevel 1 goto nogfx
goto refresh

:refresh
draw clear
draw box 1 1 80 3 double TCMD
draw box 1 4 39 11
draw box 41 4 39 11
draw text 3 4 Left
draw text 43 4 Right
draw text 3 5 %LD%
draw text 43 5 %RD%
draw text 3 7 browse to list files
draw text 43 7 ops target the other pane
draw text 3 17 status: %ST%
draw text 3 19 F=file G=other pane
draw refresh
list "Commander" "Copy to other" "Move to other" "Delete file" "Mkdir here" "Rename file" "View file" "Edit file" "Swap panes" "Change dir" "Refresh" "Quit"
if errorlevel 255 goto done
if errorlevel 10 goto done
if errorlevel 9 goto refresh
if errorlevel 8 goto chdir
if errorlevel 7 goto swap
if errorlevel 6 goto editf
if errorlevel 5 goto viewf
if errorlevel 4 goto renf
if errorlevel 3 goto mkdirf
if errorlevel 2 goto delf
if errorlevel 1 goto movef
goto copyf

:copyf
set ST=copy to %RD%
browse /v:F %LD%
if errorlevel 1 goto refresh
copy %F% %RD%
set ST=copied
echo [M-TCMD-OP] copy %F%
goto refresh

:movef
set ST=move to %RD%
browse /v:F %LD%
if errorlevel 1 goto refresh
move %F% %RD%
set ST=moved
echo [M-TCMD-OP] move %F%
goto refresh

:delf
browse /v:F %LD%
if errorlevel 1 goto refresh
ask /v:YN "Delete %F%? (yes/no)" no
if "%YN%"=="yes" del %F%
if "%YN%"=="yes" set ST=deleted
echo [M-TCMD-OP] del %F%
goto refresh

:mkdirf
ask /v:N "New dir name"
if "%N%"=="" goto refresh
md %RD%/%N%
set ST=mkdir
echo [M-TCMD-OP] mkdir %N%
goto refresh

:renf
browse /v:F %LD%
if errorlevel 1 goto refresh
ask /v:N "New name"
if "%N%"=="" goto refresh
ren %F% %N%
set ST=renamed
echo [M-TCMD-OP] ren %F%
goto refresh

:viewf
browse /v:F %LD%
if errorlevel 1 goto refresh
view %F%
goto refresh

:editf
browse /v:F %LD%
if errorlevel 1 goto refresh
edit %F%
goto refresh

:swap
set ST=swapped
set T=%LD%
set LD=%RD%
set RD=%T%
echo [M-TCMD-OP] swap
goto refresh

:chdir
ask /v:LD "Left dir" %LD%
ask /v:RD "Right dir" %RD%
set ST=dirs changed
echo [M-TCMD-OP] chdir
goto refresh

:done
echo [M-TCMD-DONE]
goto end

:nogfx
echo TCMD needs the foreground display (no draw/browse in background jobs)
goto end

:end
draw hold off
draw close
rem _T.txt no longer used: panes list via browse (no SD churn per refresh).
endlocal
