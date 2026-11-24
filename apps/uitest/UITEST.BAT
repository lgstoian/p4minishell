@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem =====================================================================
rem  UITEST.BAT - touch automation demo. Drives the firmware `ui` verbs
rem  from a batch file (proves batch support) and reports how many tappable
rem  targets the current screen exposes.
rem
rem  `ui state`  - prints modal/keyboard/editor state
rem  `ui targets /v:NAME` - sets NAME to the target count
rem  `ui hit x y /v:NAME` - sets NAME to the target id under a point
rem  `ui tap x y` - synthetic touch at a point
rem  Markers: [M-UITEST] / [M-UITEST-DONE].
rem =====================================================================
setlocal
if "%P4_APP_FEATURES%"=="1" gosub %APPROOT%/APPLIB.BAT::probe
echo [M-UITEST]  touch automation demo
title "UI Test"
ui state
ui targets /v:UICOUNT
echo touch targets: %UICOUNT%
ui hit 500 20 /v:UIHIT
echo hit at 500,20: %UIHIT%
ui tap 500 20
ui state
rem ---- v1.6 TUI layout/focus/route showcase -----------------------------
rem [M-RICH-UITEST] drives the new focus/layout/scroll/route surfaces, all
rem read-only geometry here, so the transcript is an honest capability proof.
echo [M-RICH-UITEST] focus+layout+scroll+route
tui focus next
tui layout row 1 1 80 4 2
tui scroll richlog 4 12 1
tui route j
tui frame status
echo [M-UITEST-DONE]
return
