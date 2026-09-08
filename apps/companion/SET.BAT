@echo off
rem =====================================================================
rem  SET.BAT - Settings. Values persist in sd:/APPS/companion.INI via the
rem  `appconfig companion` commands, so they survive reboots.
rem  Enhanced TUI with draw boxes and status.
rem =====================================================================

:main
echo [M-SET]
draw clear screen
draw box 1 1 80 3 single
draw text 35 2 "SETTINGS" 14
draw box 1 4 80 10 single "Options"
draw text 5 6 "Brightness, volume, config, scoping"
list "Settings" "Set brightness" "Set volume" "Show settings" "Scoping demo" "View config" "TUI Demo" "Reset" "Back"
if errorlevel 255 goto :back
if errorlevel 7 goto :back
if errorlevel 6 goto :reset
if errorlevel 5 goto :tui_demo
if errorlevel 4 goto :view_cfg
if errorlevel 3 goto :scoping
if errorlevel 2 goto :show
if errorlevel 1 goto :volume
if errorlevel 0 goto :brightness
goto main

:tui_demo
tui fullscreen on
draw clear screen
draw box 1 1 80 3 double "TUI SETTINGS DEMO"
draw box 1 4 80 8 single "Colors"
draw text 5 6 "PowerShell palette: Red Green Yellow Blue Magenta Cyan" 7
ansi 31m Red 32m Green 33m Yellow 34m Blue 35m Magenta 36m Cyan
draw box 1 13 80 10 single "Boxes"
draw box 5 15 15 6 single "Single"
draw box 25 15 15 6 double "Double"
draw box 45 15 15 6 rounded "Rounded"
draw text 5 23 "Press any key..."
pause
tui fullscreen off
draw clear screen
goto main

:brightness
draw box 1 1 80 3 single
draw text 30 2 "BRIGHTNESS" 14
ask /v:val "Brightness (0-100)"
if errorlevel 1 goto main
if "%val%"=="" goto main
brightness %val%
if errorlevel 1 dialog "Brightness" "Failed to set brightness." "OK" & goto main
appconfig companion set brightness %val%
dialog "Settings" "Saved brightness=%val%." "OK"
goto main

:volume
draw box 1 1 80 3 single
draw text 32 2 "VOLUME" 14
ask /v:val "Volume (0-100)"
if errorlevel 1 goto main
if "%val%"=="" goto main
volume %val%
if errorlevel 1 dialog "Volume" "Failed to set volume." "OK" & goto main
appconfig companion set volume %val%
dialog "Settings" "Saved volume=%val%." "OK"
goto main

:show
draw box 1 1 80 3 single
draw text 30 2 "SAVED SETTINGS" 14
appconfig companion > _set.txt
view _set.txt
del /p _set.txt
goto main

:view_cfg
draw box 1 1 80 3 single
draw text 33 2 "CONFIG.SYS" 14
type CONFIG.SYS > _cfg.txt
view _cfg.txt
del /p _cfg.txt
goto main

:scoping
tui fullscreen on
draw clear screen
draw box 1 1 80 3 double "SCOPING DEMO"
draw text 5 6 "Setting TEST inside a setlocal block..." 7
setlocal
set TEST=inside-scope
draw box 5 8 70 3 single "Inside"
draw text 10 10 "Inside : TEST=%TEST%" 14
pause
endlocal
draw box 5 12 70 3 single "After"
draw text 10 14 "After endlocal : TEST=%TEST% (should be empty)" 14
draw text 10 16 "Press any key..."
pause
tui fullscreen off
draw clear screen
goto main

:reset
draw box 1 1 80 3 single
draw text 35 2 "RESET" 14
dialog "Reset" "Clear saved settings?" "Yes" "No"
if errorlevel 1 goto main
appconfig companion del brightness
appconfig companion del volume
dialog "Reset" "Cleared saved settings." "OK"
goto main

:back
echo [M-SET-BACK]
draw clear screen
goto :eof
