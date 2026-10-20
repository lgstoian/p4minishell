@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem =====================================================================
rem  APPLIB.BAT - shared routines for the reference apps.
rem
rem  Every entry app calls `gosub APPLIB.BAT::probe` once at startup. The
rem  probe is OPT-IN: with P4_APP_FEATURES unset it is a no-op, so the
rem  normal app transcript and pace are unchanged. The regression harness
rem  sets `P4_APP_FEATURES=1` to make every app exercise the newer firmware
rem  surfaces (locale, JS engine, browser verb, cookie jar) and emit the
rem  [M-FEAT-*] markers s19_app_features.py asserts.
rem
rem  Routines (call with `gosub APPLIB.BAT::<name> [args]`):
rem    :probe          feature exercise, opt-in via P4_APP_FEATURES=1
rem    :locale_demo    print the active locale's date/time/number samples
rem    :cookie_show    show the persisted HTTP cookie jar
rem =====================================================================

rem ---- feature probe ---------------------------------------------------
:probe
if not "%P4_APP_FEATURES%"=="1" goto :eof
echo [M-FEAT]
call :locale_demo
call :js_demo
call :browser_demo
call :cookie_show
echo [M-FEAT-DONE]
goto :eof

rem ---- locale ----------------------------------------------------------
:locale_demo
locale
echo [M-FEAT-LOCALE]
goto :eof

rem ---- JS engine -------------------------------------------------------
rem `js eval` prints through the transcript, so the marker is produced by
rem the script itself and proves the engine ran.
:js_demo
js eval "print('[M-FEAT-JS]')"
goto :eof

rem ---- browser verb (usage path; no network is touched) ----------------
:browser_demo
browser
echo [M-FEAT-BROWSER]
goto :eof

rem ---- HTTP cookie jar -------------------------------------------------
:cookie_show
httpget /cookie
echo [M-FEAT-COOKIE]
goto :eof
