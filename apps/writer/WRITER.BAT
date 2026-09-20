@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem =====================================================================
rem  WRITER.BAT - writerdeck reference app.
rem
rem  Exercises the writing workflow without leaving the shell:
rem  1. template seeding (copies sd:/TEMPLATES/NOTE.MD the way
rem     `edit <new> /template NOTE` seeds a new buffer),
rem  2. the Markdown render path (`markdown`, the same engine preview uses),
rem  3. all three `markdown export` formats (text, standalone HTML page,
rem     paginated print).
rem  It also verifies the spell wordlist (sd:/DICTS/en.words) behind the
rem  editor's `Spell` underlines. Focus/typewriter mode and the interactive
rem  preview/spell toggles need a live `edit` session, so the app prints
rem  their exact commands instead of driving them.
rem  Every step fails loudly (errorlevel 1) so a broken SD/push is visible.
rem  Markers: [M-WRITER] / [M-WRITER-DONE].
rem =====================================================================
setlocal
set ERR=0
echo [M-WRITER]
echo WRITER: P4MiniShell writerdeck
echo.
echo WRITER: templates need apps/push_templates.py; spell needs apps/push_dicts.py
echo.

rem --- 1. Templates shipped on the SD card (push_templates.py) ---
set HAVE_TEMPLATES=1
if exist TEMPLATES\NOTE.MD (echo WRITER: template NOTE.MD present) else (set HAVE_TEMPLATES=0)
if exist TEMPLATES\LETTER.MD (echo WRITER: template LETTER.MD present) else (set HAVE_TEMPLATES=0)
if exist TEMPLATES\LOG.MD (echo WRITER: template LOG.MD present) else (set HAVE_TEMPLATES=0)
if "%HAVE_TEMPLATES%"=="0" (echo WRITER: FAIL some templates missing ^(run apps/push_templates.py^) & set ERR=1)
echo.

rem --- 2. Seed the demo document from the NOTE template (as edit /template does) ---
if exist WRITERDEMO.MD del /f WRITERDEMO.MD
if "%HAVE_TEMPLATES%"=="1" (copy TEMPLATES\NOTE.MD WRITERDEMO.MD)
if exist WRITERDEMO.MD (echo WRITER: seeded WRITERDEMO.MD from template NOTE) else (echo WRITER: FAIL template seed & set ERR=1)
append WRITERDEMO.MD ""
append WRITERDEMO.MD "# Writer Deck"
append WRITERDEMO.MD ""
append WRITERDEMO.MD "Drafted on the **P4MiniShell** writerdeck."
append WRITERDEMO.MD ""
append WRITERDEMO.MD "1. templates"
append WRITERDEMO.MD "2. focus mode"
append WRITERDEMO.MD "3. markdown export"
append WRITERDEMO.MD ""
append WRITERDEMO.MD "- [x] text export"
append WRITERDEMO.MD "- [ ] html + print exports"
echo WRITER: wrote WRITERDEMO.MD
echo.

rem --- 3. Render it (the same engine the editor preview uses) ---
markdown WRITERDEMO.MD
echo.

rem --- 4. Export it in every format ---
markdown export WRITERDEMO.MD WRITERDEMO.HTML html
if errorlevel 1 (echo WRITER: FAIL html export & set ERR=1)
markdown export WRITERDEMO.MD WRITERDEMO.TXT text
if errorlevel 1 (echo WRITER: FAIL text export & set ERR=1)
markdown export WRITERDEMO.MD WRITERDEMO.PRN print
if errorlevel 1 (echo WRITER: FAIL print export & set ERR=1)
echo.

rem --- 5. Spell wordlist behind the editor underlines (push_dicts.py) ---
if exist DICTS\en.words (echo WRITER: spell wordlist DICTS\en.words present) else (echo WRITER: FAIL spell wordlist missing ^(run apps/push_dicts.py^) & set ERR=1)
echo.

rem --- 6. Interactive writing tools (need a live edit session) ---
echo WRITER: focus writing  -> edit NOTES.TXT /focus
echo WRITER: from a template -> edit DIARY.TXT /template NOTE
echo WRITER: preview markup  -> open the file and press Preview (Ctrl+P)
echo WRITER: spellcheck      -> open the file and press Spell (needs DICTS\en.words)
echo WRITER: share over LAN  -> httpd start, then open /WRITERDEMO.HTML
if "%ERR%"=="1" (echo WRITER: one or more steps failed) else (echo WRITER: all steps passed)
echo [M-WRITER-DONE]
endlocal
exit /b %ERR%
