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
rem    :paths          default APPDRIVE/APPROOT when a script runs outside
rem                    the shell's .bat launcher (apps use %APPDRIVE% instead
rem                    of a hardcoded sd:/ so they run from SD or USB)
rem    :emoji_demo     report the retro DOS fallback and any EMOJI/ pack
rem    :html_demo      prove the `html` page verb is reachable
rem    :locale_demo    print the active locale's date/time/number samples
rem    :cookie_show    show the persisted HTTP cookie jar
rem =====================================================================

rem ---- feature probe ---------------------------------------------------
:probe
if not "%P4_APP_FEATURES%"=="1" goto :eof
echo [M-FEAT]
call :paths
call :locale_demo
call :emoji_demo
call :html_demo
call :js_demo
call :browser_demo
call :cookie_show
echo [M-FEAT-DONE]
goto :eof

rem ---- app paths -------------------------------------------------------
rem The shell publishes APPDRIVE ("sd:" or "usb:") and APPROOT (the app's
rem directory) for every .bat it runs, so an app installed on USB keeps its
rem data there. Nothing here guesses a drive; when a script is called outside
rem that path (rare), fall back to SD.
:paths
if not "%APPDRIVE%"=="" goto :eof
set APPDRIVE=sd:
if "%APPROOT%"=="" set APPROOT=%CD%
goto :eof

rem ---- emoji -----------------------------------------------------------
rem `emoji status` reports the retro DOS fallback and any EMOJI/ pack and
rem proves the core is reachable from an app.
:emoji_demo
emoji status
echo [M-FEAT-EMOJI]
goto :eof

rem ---- HTML page surface ----------------------------------------------
rem `html` renders through the same browser engine as `browser` (DOM, CSS,
rem JavaScript, images, canvas, HTTP(S)); the marker proves the verb exists.
:html_demo
html
echo [M-FEAT-HTML]
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
