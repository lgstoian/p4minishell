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
echo [M-UITEST]
ui state
ui targets /v:UICOUNT
echo touch targets: %UICOUNT%
ui hit 500 20 /v:UIHIT
echo hit at 500,20: %UIHIT%
ui tap 500 20
ui state
echo [M-UITEST-DONE]
return
