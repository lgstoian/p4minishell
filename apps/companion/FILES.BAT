@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem =====================================================================
rem  FILES.BAT - File Tools: browse, view, hexview, trash, clipboard, notes.
rem  Enhanced TUI with draw boxes, fullscreen, and window management.
rem =====================================================================

:main
if "%P4_APP_FEATURES%"=="1" gosub %APPROOT%/APPLIB.BAT::probe
goto :files_menu

rem ---- v1.6 richer-language showcase ------------------------------------
rem [M-RICH-FILES] lists file tools with map /set + for /K, str /count|/pad
rem and func/return. It never touches the browser or note files.
:rich_files
echo [M-RICH-FILES] tools language tour
map /set FTOOLS view view
map /set FTOOLS hex hexview
for /K %%k in (FTOOLS) do echo [M-RICH-FILES-K] %%k=%%l
set FW=files
str /count FW e CF
str /pad FW 10 FP .
func :ftotal 3 6
echo [M-RICH-FILES-F] total=%RESULT%
echo [M-RICH-FILES-S] e=%CF% pad=%FP%
return

:ftotal
return %1*%2

:files_menu
title "File Tools"
echo [M-FILES]
gosub :rich_files
rem Layout: the shell publishes the grid (80x25 landscape, 80x97 portrait).
rem The Tools panel grows into the extra rows; the Status panel stays on the
rem bottom (rows 17..24 of the classic grid).
if "%TUICOLS%"=="" set TUICOLS=80
if "%TUIROWS%"=="" set TUIROWS=25
set /a SY=%TUIROWS%-6 > NUL
draw clear screen
set DOS_THEME=ice
gosub %APPROOT%/APPLIB.BAT::header "FILE TOOLS" "[M-RICH-FILES] map/forK tool index"
gosub %APPROOT%/APPLIB.BAT::tile 1 5 46 10 double "TOOLBOX"
draw text 4 8 "Browse / search / trash / clipboard" 15
gosub %APPROOT%/APPLIB.BAT::tile 49 5 31 10 single "SELECTED FILE"
gosub %APPROOT%/APPLIB.BAT::pager "PATH" 8 4 FILES.TXT
gosub %APPROOT%/APPLIB.BAT::statusbar 21 "drive=%APPDRIVE%  tools=ready" "b browse  h hex  n notes  q quit"
echo [M-DOS-FILES] screen=FILES-TOOLS panels=3 head="FILE TOOLS" theme=ice bars=0 table=N spark=0 status=Y
draw text 35 2 "FILE TOOLS ★" 14
gosub %APPROOT%/APPLIB.BAT::panel 4 12 single "Tools"
draw text 5 6 "Browse the app drive with the full TUI file browser"
draw text 5 7 "Quick dir, search, trash, clipboard, notes"
gosub %APPROOT%/APPLIB.BAT::footer 17 8 single "Status"
draw text 5 %SY% "Use Browse for full-screen file picker  (%APPDRIVE%)"
draw run 50 2 "[M-RICH-FILES] map/forK" /fg 11 /bg 0
list "File Tools" "Browse SD (TUI)" "Quick dir" "Search files" "Trash browser" "Clipboard" "Note pad" "Hexview demo" "Commander" "Back"
on %errorlevel%+1 goto :browse,:dir,:search,:trash,:clipboard,:notes,:hex_demo,:tcmd,:back
goto :back

:browse
tui fullscreen on
browse /v:BROWSE_RESULT
if errorlevel 1 goto :browse_done
gosub %APPROOT%/APPLIB.BAT::page single
draw text 30 2 "SELECTED FILE" 14
echo Selected: %BROWSE_RESULT%
list "Open" "View (TUI)" "Hexview" "Back"
on %errorlevel%+1 goto :do_open,:do_view,:do_hexview,:browse_done
goto :browse_done
:do_open
tui fullscreen off
type < "%BROWSE_RESULT%"
pause
tui fullscreen on
goto :browse_done
:do_view
view %BROWSE_RESULT%
goto :browse_done
:do_hexview
hexview %BROWSE_RESULT%
:browse_done
tui fullscreen off
draw clear screen
goto files_menu

:hex_demo
draw clear screen
gosub %APPROOT%/APPLIB.BAT::page single
draw text 32 2 "HEXVIEW DEMO" 14
ask /v:hexfile "File for hexview"
if errorlevel 1 goto files_menu
hexview %hexfile%
goto files_menu

:dir
gosub %APPROOT%/APPLIB.BAT::page single
draw text 35 2 "DIRECTORY" 14
dir /a > %APPDRIVE%/_dir.txt
view %APPDRIVE%/_dir.txt
del /p %APPDRIVE%/_dir.txt > NUL
goto files_menu

:search
ask /v:pat "Search pattern (e.g. *.txt)" "*.txt"
if errorlevel 1 goto files_menu
find /NAME:%pat% > %APPDRIVE%/_search.txt
view %APPDRIVE%/_search.txt
del /p %APPDRIVE%/_search.txt > NUL
goto files_menu

:trash
trash list > %APPDRIVE%/_trash.txt
view %APPDRIVE%/_trash.txt
del /p %APPDRIVE%/_trash.txt > NUL
ask /v:idx "Restore entry number (blank = back)"
if errorlevel 1 goto files_menu
if "%idx%"=="NONE" goto files_menu
if "%idx%"=="" goto files_menu
trash restore %idx%
echo.
pause
goto files_menu

:clipboard
draw clear screen
gosub %APPROOT%/APPLIB.BAT::page single
draw text 35 2 "CLIPBOARD" 14
clip
echo.
list "Clipboard" "Copy last line" "Read a file" "View clipboard" "Back"
if errorlevel 255 goto files_menu
if errorlevel 3 goto files_menu
if errorlevel 2 goto :clip_view
if errorlevel 1 goto :clip_read
goto :clip_copy
:clip_copy
clip copy
dialog "Clipboard" "Copied the last transcript line." "OK"
goto clipboard
:clip_read
ask /v:f "File to read"
if errorlevel 1 goto clipboard
clip read %f%
if errorlevel 1 dialog "Clipboard" "Could not read that file." "OK"
goto clipboard
:clip_view
clip > %APPDRIVE%/_clip.txt
view %APPDRIVE%/_clip.txt
del /p %APPDRIVE%/_clip.txt > NUL
goto clipboard

:notes
echo [M-FILES-NOTES]
draw clear screen
gosub %APPROOT%/APPLIB.BAT::page single
draw text 35 2 "NOTE PAD" 14
draw text 5 5 "Notes stored in %APPDRIVE%/NOTES"
if not exist %APPDRIVE%/NOTES md %APPDRIVE%/NOTES > NUL
list "Notes" "New note" "List notes" "View note" "Browse notes" "Back"
if errorlevel 255 goto :back
if errorlevel 4 goto :back
if errorlevel 3 goto :browse_notes
if errorlevel 2 goto :view_note
if errorlevel 1 goto :list_notes
goto :new_note

:new_note
ask /v:NNAME "Note name (no spaces)"
if errorlevel 1 goto notes
ask /v:NBODY "Body"
if errorlevel 1 goto notes
echo [%DATE% %TIME%] >> %APPDRIVE%/NOTES/%NNAME%.txt
echo %NBODY% >> %APPDRIVE%/NOTES/%NNAME%.txt
echo [M-FILES-NOTED]
dialog "Notes" "Saved %APPDRIVE%/NOTES/%NNAME%.txt" "OK"
goto notes

:list_notes
dir %APPDRIVE%/NOTES > %APPDRIVE%/_notes.txt
view %APPDRIVE%/_notes.txt
del /p %APPDRIVE%/_notes.txt > NUL
goto notes

:view_note
ask /v:NNAME "Note name"
if errorlevel 1 goto notes
view %APPDRIVE%/NOTES/%NNAME%.txt
goto notes

:browse_notes
browse /v:NNAME %APPDRIVE%/NOTES
if errorlevel 1 goto notes
view %NNAME%
goto notes

:back
echo [M-FILES-BACK]
draw clear screen
goto :eof

:tcmd
call %APPROOT%/TCMD.BAT
goto files_menu
