@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem =====================================================================
rem  WRITER.BAT - writerdeck reference app.
rem
rem  Demonstrates the writing workflow end to end without leaving the shell:
rem  templates (edit /template), focus/typewriter mode, and Markdown export.
rem  It builds a small Markdown document, exports it to HTML, and prints the
rem  exact commands to start writing. Markers: [M-WRITER] / [M-WRITER-DONE].
rem =====================================================================
setlocal
echo [M-WRITER]
echo WRITER: P4MiniShell writerdeck
echo.

rem --- 1. Templates shipped on the SD card (push_templates.py) ---
if exist TEMPLATES\NOTE.MD (echo WRITER: template NOTE.MD present) else (echo WRITER: no templates pushed ^(run apps/push_templates.py^))
if exist TEMPLATES\LETTER.MD (echo WRITER: template LETTER.MD present)
if exist TEMPLATES\LOG.MD (echo WRITER: template LOG.MD present)
echo.

rem --- 2. Build a Markdown document and export it ---
if exist WRITERDEMO.MD del /f WRITERDEMO.MD
write WRITERDEMO.MD "# Writer Deck"
append WRITERDEMO.MD ""
append WRITERDEMO.MD "Drafted on the **P4MiniShell** writerdeck."
append WRITERDEMO.MD ""
append WRITERDEMO.MD "- templates"
append WRITERDEMO.MD "- focus mode"
append WRITERDEMO.MD "- markdown export"
echo WRITER: wrote WRITERDEMO.MD
markdown export WRITERDEMO.MD WRITERDEMO.HTML html
if errorlevel 1 echo WRITER: HTML export failed
markdown export WRITERDEMO.MD WRITERDEMO.TXT text
if errorlevel 1 echo WRITER: text export failed
echo.

rem --- 3. Point the user at the interactive writing tools ---
echo WRITER: focus writing  -> edit NOTES.TXT /focus
echo WRITER: from a template -> edit DIARY.TXT /template NOTE
echo WRITER: share over LAN  -> httpd start, then open /WRITERDEMO.HTML
echo WRITER: spellcheck toggle lives on the editor Edit page ^(Spell^)
echo [M-WRITER-DONE]
endlocal
