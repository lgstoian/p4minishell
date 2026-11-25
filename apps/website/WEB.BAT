@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem =====================================================================
rem  WEB.BAT - the offline browser showcase.
rem
rem  Renders %APPDRIVE%/WEB/INDEX.HTML with the `browser` verb: headings,
rem  paragraphs, a navigation row, lists, a table and image alt text.
rem  A non-http(s) argument is treated as an SD path, so the page is
rem  parsed and laid out entirely on the device - no AP, cloud or httpd.
rem  (http:// and https:// arguments still fetch over Wi-Fi as before.)
rem
rem  Markers: [M-WEB] / [M-WEB-LOADED] / [M-WEB-NOFILE] / [M-WEB-FAIL] /
rem           [M-WEB-DONE].
rem  The page is captured in its clean boot state for the showcase screenshot,
rem  then the RUN DEMO onclick is dispatched and the live status row is read
rem  back with `tui dump` so the suite can assert the event fired.
rem =====================================================================
setlocal
if "%P4_APP_FEATURES%"=="1" gosub %APPROOT%/APPLIB.BAT::probe
echo [M-WEB] 
echo [M-DOS-WEB] screen=WEB-SITE panels=2 head="P4 MINISHELL WEB" theme=amber bars=0 table=Y spark=0 status=Y
title "Browser"

if not exist %APPDRIVE%/WEB/INDEX.HTML goto :nofile
rem Emit the sync marker BEFORE rendering: printing after `browser` repaints
rem the transcript over the TUI page, which is what the showcase captures.
echo [M-WEB-LOADED]
rem Let the marker's transcript repaint settle so the browser's TUI page is
rem the last surface painted before the capture.
delay 500
html /no %APPDRIVE%/WEB/INDEX.HTML
if errorlevel 1 goto :fail

rem Capture the clean boot state (the JS boot message) for the showcase.
screenshot %APPDRIVE%/WEB/SHOT.BMP

rem Hold the rendered page briefly so a human can read it, then demonstrate the
rem in-page JS event hook: dispatch the RUN DEMO onclick and read the status row
rem back so the suite can assert the event fired. The screenshot above stays the
rem clean boot state; the clicked state is verified through the dump.
delay 3000
browser js go
tui dump 3 9

rem ---- v1.6 webapp facade showcase --------------------------------------
rem [M-RICH-WEB] dispatches the SAME button event through the webapp facade
rem and runs the page's JS timer via webapp tick - no second engine.
echo [M-RICH-WEB] event+tick
webapp event go click
webapp tick
webapp dump 3 9 /a

echo [M-WEB-DONE]
rem Leave the TUI surface so the shell prompt is visible again.
draw close
goto :eof

:nofile
echo [M-WEB-NOFILE]
echo web: %APPDRIVE%/WEB/INDEX.HTML is missing.
echo web: push it with: python apps/push_apps.py ^<PORT^>
echo [M-WEB-DONE]
goto :eof

:fail
echo [M-WEB-FAIL]
echo web: the page could not be read or rendered.
echo [M-WEB-DONE]
goto :eof
