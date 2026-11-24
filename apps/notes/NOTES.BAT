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
if "%P4_APP_FEATURES%"=="1" gosub %APPROOT%/APPLIB.BAT::probe
echo [M-NOTE]
title "Zettelkasten"
gosub :rich_note
rem The `db` record store is SD-owned by design (sd:/DBS/zettel.DB); everything
rem else this app writes uses %APPDRIVE% so the shell and editor data follow
rem the drive the app was launched from.
db info zettel > %APPDRIVE%/_zinit.txt
if errorlevel 1 goto first_run
del /p %APPDRIVE%/_zinit.txt > NUL
goto menu

rem ---- v1.6 richer-language showcase -----------------------------------
rem [M-RICH-NOTE] previews category labels with map /set + for /K, string
rem counting/padding and func/return; the db store is not touched.
:rich_note
echo [M-RICH-NOTE] category language tour
map /set CATS 0 Fleeting
map /set CATS 1 Permanent
for /K %%k in (CATS) do echo [M-RICH-NOTE-K] %%k=%%l
set NT=Zettelkasten
str /count NT e CE
str /pad NT 16 NP .
func :notelen 12 34
echo [M-RICH-NOTE-F] len=%RESULT%
echo [M-RICH-NOTE-S] e=%CE% pad=%NP%
return

:notelen
return %1+%2

:first_run
del /p %APPDRIVE%/_zinit.txt > NUL
db create zettel /cr:NOTE /tp:ZTL /vr:1
db categories zettel set 0 Fleeting
db categories zettel set 1 Permanent
db categories zettel set 2 Project
db categories zettel set 3 Archive
dialog "Notes" "Created the zettel store." "OK"

:menu
draw clear screen
gosub %APPROOT%/APPLIB.BAT::page double "ZETTELKASTEN ★"
draw run 52 2 "[M-RICH-NOTE] map/str/func" /fg 11 /bg 0
draw text 5 2 "linked notes on the db store" 14
echo [M-NOTE-MENU]
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
edit %APPDRIVE%/_zettel.txt
db add zettel /cat:%zcat% /key:%zkey% %APPDRIVE%/_zettel.txt
if errorlevel 1 dialog "Notes" "Could not save." "OK" & goto menu
del /p %APPDRIVE%/_zettel.txt > NUL
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
gosub %APPROOT%/APPLIB.BAT::page single
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
dialog "Notes" "Exported to sd:/DBS/zettel.EXPORT (db store is SD-only)." "OK"
goto menu

:import
db import zettel
if errorlevel 1 dialog "Notes" "No export file found." "OK" & goto menu
dialog "Notes" "Imported." "OK"
goto menu

:back
draw clear screen
return