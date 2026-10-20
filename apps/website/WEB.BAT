@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem =====================================================================
rem  WEB.BAT - the offline browser showcase.
rem
rem  Renders sd:/WEB/INDEX.HTML with the `browser` verb: headings,
rem  paragraphs, a navigation row, lists, a table and image alt text.
rem  A non-http(s) argument is treated as an SD path, so the page is
rem  parsed and laid out entirely on the device - no AP, cloud or httpd.
rem  (http:// and https:// arguments still fetch over Wi-Fi as before.)
rem
rem  Markers: [M-WEB] / [M-WEB-LOADED] / [M-WEB-NOFILE] / [M-WEB-FAIL] /
rem           [M-WEB-DONE].
rem  The rendered page is held for 15 s with no transcript output (a print
rem  would repaint the transcript over the TUI page) so a capture or a
rem  human can read it; then the app exits.
rem =====================================================================
setlocal
if "%P4_APP_FEATURES%"=="1" gosub APPLIB.BAT::probe
echo [M-WEB]
title "Browser"

if not exist sd:/WEB/INDEX.HTML goto :nofile
rem Emit the sync marker BEFORE rendering: printing after `browser` repaints
rem the transcript over the TUI page, which is what the showcase captures.
echo [M-WEB-LOADED]
rem Let the marker's transcript repaint settle so the browser's TUI page is
rem the last surface painted before the capture.
delay 500
browser sd:/WEB/INDEX.HTML
if errorlevel 1 goto :fail

rem Capture the framebuffer while the page is still on screen, then keep it
rem visible briefly. Writing the BMP to SD (instead of streaming) keeps the
rem capture off the serial console and works while this batch owns the worker.
screenshot sd:/WEB/SHOT.BMP
delay 3000

echo [M-WEB-DONE]
rem Leave the TUI surface so the shell prompt is visible again.
draw close
goto :eof

:nofile
echo [M-WEB-NOFILE]
echo web: sd:/WEB/INDEX.HTML is missing.
echo web: push it with: python apps/push_apps.py ^<PORT^>
echo [M-WEB-DONE]
goto :eof

:fail
echo [M-WEB-FAIL]
echo web: the page could not be read or rendered.
echo [M-WEB-DONE]
goto :eof
