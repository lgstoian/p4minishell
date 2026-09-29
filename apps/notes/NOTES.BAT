@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem =====================================================================
rem  NOTES.BAT - a Zettelkasten note app on the `db` record store.
rem  Reference app: db create/add/get/find/count/export/import, for /f over
rem  pipe-fed find output, category labels, file-payload bodies via edit,
rem  and `on`-dispatched menus (list's 0-based index -> 1-based on target).
rem  Markers: [M-NOTE] / [M-NOTE-SAVED] / [M-NOTE-COUNT].
rem =====================================================================
setlocal
echo [M-NOTE]
title "Notes"
db info zettel > _zinit.txt
if errorlevel 1 goto first_run
del /p _zinit.txt > NUL
goto menu
:first_run
del /p _zinit.txt > NUL
db create zettel /cr:NOTE /tp:ZTL /vr:1
db categories zettel set 0 Fleeting
db categories zettel set 1 Permanent
db categories zettel set 2 Project
db categories zettel set 3 Archive
dialog "Notes" "Created the zettel store." "OK"

:menu
draw clear screen
draw box 1 1 80 3 double "ZETTELKASTEN"
draw text 5 2 "linked notes on the db store" 14
list "Zettelkasten" "New note" "List notes" "View note" "Search" "Delete note" "Export" "Import" "Back"
on %errorlevel%+1 goto :new_note,:list_notes,:view_note,:search,:del_note,:export,:import,:back
goto :back

:new_note
ask /v:zkey "Link key (one word)"
if errorlevel 1 goto menu
if "%zkey%"=="" goto menu
list "Category" "Fleeting" "Permanent" "Project" "Archive"
if errorlevel 255 goto menu
rem list returns the 0-based index, which is exactly the db category number.
set zcat=%errorlevel%
:body_choice
list "Body entry" "Type it" "Edit file" "Cancel"
if errorlevel 255 goto menu
if errorlevel 2 goto menu
if errorlevel 1 goto body_file
ask /v:zbody "Body (one line)"
if errorlevel 1 goto menu
if "%zbody%"=="" goto menu
db add zettel /cat:%zcat% /key:%zkey% %zbody%
if errorlevel 1 dialog "Notes" "Could not save." "OK" & goto menu
echo [M-NOTE-SAVED]
dialog "Notes" "Saved [%zkey%]." "OK"
goto menu

:body_file
edit _zettel.txt
db add zettel /cat:%zcat% /key:%zkey% _zettel.txt
if errorlevel 1 dialog "Notes" "Could not save." "OK" & goto menu
del /p _zettel.txt > NUL
echo [M-NOTE-SAVED]
dialog "Notes" "Saved [%zkey%] from editor." "OK"
goto menu

:list_notes
db count zettel
echo [M-NOTE-COUNT]
rem No temp file: `db find /b` feeds the for /f pipe source directly, so a
rem slow-SD unlink never stalls the listing (see bugs.md O6).
db find zettel /b | for /f "eol= tokens=1-3 delims=|" %%r in () do echo %%r. [%%s] %%t
draw clear screen
draw box 1 1 80 3 single
draw text 35 2 "ALL NOTES" 14
pause
goto menu

:view_note
ask /v:zid "Note id"
if errorlevel 1 goto menu
if "%zid%"=="" goto menu
db get zettel %zid%
if errorlevel 1 dialog "Notes" "No such note." "OK"
pause
goto menu

:search
ask /v:zq "Search text"
if errorlevel 1 goto menu
if "%zq%"=="" goto menu
db find zettel /text:%zq%
if errorlevel 1 dialog "Notes" "Nothing found." "OK"
pause
goto menu

:del_note
ask /v:zid "Note id to delete"
if errorlevel 1 goto menu
if "%zid%"=="" goto menu
db del zettel %zid%
if errorlevel 1 dialog "Notes" "No such note." "OK" & goto menu
dialog "Notes" "Deleted %zid%." "OK"
goto menu

:export
db export zettel
dialog "Notes" "Exported to sd:/DBS/zettel.EXPORT." "OK"
goto menu

:import
db import zettel
if errorlevel 1 dialog "Notes" "No export file found." "OK" & goto menu
dialog "Notes" "Imported." "OK"
goto menu

:back
draw clear screen
return