@echo off
rem =====================================================================
rem  FILES.BAT - File Tools: browse, view, hexview, trash, clipboard, notes.
rem  Enhanced TUI with draw boxes, fullscreen, and window management.
rem =====================================================================

:main
:files_menu
draw clear screen
draw box 1 1 80 3 single
draw text 35 2 "FILE TOOLS" 14
draw box 1 4 80 12 single "Tools"
draw text 5 6 "Browse SD with full TUI file browser"
draw text 5 7 "Quick dir, search, trash, clipboard, notes"
draw box 1 17 80 8 single "Status"
draw text 5 19 "Use Browse for full-screen file picker"
list "File Tools" "Browse SD (TUI)" "Quick dir" "Search files" "Trash browser" "Clipboard" "Note pad" "Hexview demo" "Back"
if errorlevel 255 goto :back
if errorlevel 7 goto :hex_demo
if errorlevel 6 goto :back
if errorlevel 5 goto :notes
if errorlevel 4 goto :clipboard
if errorlevel 3 goto :trash
if errorlevel 2 goto :search
if errorlevel 1 goto :dir
goto :browse

:browse
tui fullscreen on
browse /v:BROWSE_RESULT
if errorlevel 1 goto :browse_done
draw box 1 1 80 3 single
draw text 30 2 "SELECTED FILE" 14
echo Selected: %BROWSE_RESULT%
list "Open" "View (TUI)" "Hexview" "Back"
if errorlevel 255 goto :browse_done
if errorlevel 2 goto :do_hexview
if errorlevel 1 goto :do_view
if errorlevel 0 goto :do_open
goto :browse_done
:do_open
tui fullscreen off
type %BROWSE_RESULT%
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
draw box 1 1 80 3 single
draw text 32 2 "HEXVIEW DEMO" 14
ask /v:hexfile "File for hexview"
if errorlevel 1 goto files_menu
hexview %hexfile%
goto files_menu

:dir
draw box 1 1 80 3 single
draw text 35 2 "DIRECTORY" 14
dir /a > _dir.txt
view _dir.txt
del /p _dir.txt
goto files_menu

:search
ask /v:pat "Search pattern (e.g. *.txt)" "*.txt"
if errorlevel 1 goto files_menu
find /NAME:%pat% > _search.txt
view _search.txt
del /p _search.txt
goto files_menu

:trash
trash list > _trash.txt
view _trash.txt
del /p _trash.txt
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
draw box 1 1 80 3 single
draw text 35 2 "CLIPBOARD" 14
clip
echo.
list "Clipboard" "Copy last line" "Read a file" "View clipboard" "Back"
if errorlevel 255 goto files_menu
if errorlevel 3 goto :clip_view
if errorlevel 2 goto clip_read
if errorlevel 1 goto :clip_copy
goto files_menu
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
clip > _clip.txt
view _clip.txt
del /p _clip.txt
goto clipboard

:notes
draw clear screen
draw box 1 1 80 3 single
draw text 35 2 "NOTE PAD" 14
draw text 5 5 "Notes stored in sd:/NOTES"
if not exist NOTES md NOTES
list "Notes" "New note" "List notes" "View note" "Browse notes" "Back"
if errorlevel 255 goto :back
if errorlevel 4 goto :browse_notes
if errorlevel 3 goto view_note
if errorlevel 2 goto list_notes
if errorlevel 1 goto :new_note
goto notes

:new_note
ask /v:NNAME "Note name (no spaces)"
if errorlevel 1 goto notes
ask /v:NBODY "Body"
if errorlevel 1 goto notes
echo [%DATE% %TIME%] >> NOTES/%NNAME%.txt
echo %NBODY% >> NOTES/%NNAME%.txt
dialog "Notes" "Saved NOTES/%NNAME%.txt" "OK"
goto notes

:list_notes
dir NOTES > _notes.txt
view _notes.txt
del /p _notes.txt
goto notes

:view_note
ask /v:NNAME "Note name"
if errorlevel 1 goto notes
view NOTES/%NNAME%.txt
goto notes

:browse_notes
browse /v:NNAME NOTES
if errorlevel 1 goto notes
view %NNAME%
goto notes

:back
draw clear screen
goto :eof
