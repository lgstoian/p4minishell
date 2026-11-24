@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem =====================================================================
rem  APPLIB.BAT - shared routines for the reference apps.
rem
rem  Every entry app calls `gosub %APPROOT%/APPLIB.BAT::probe` once at
rem  startup: %APPROOT% is the running script's own directory, so the same
rem  line finds the library whether the bundle is installed at sd:/ or
rem  usb:/APPS. The probe is OPT-IN: with P4_APP_FEATURES unset it is a no-op, so the
rem  normal app transcript and pace are unchanged. The regression harness
rem  sets `P4_APP_FEATURES=1` to make every app exercise the newer firmware
rem  surfaces (locale, JS engine, browser verb, cookie jar) and emit the
rem  [M-FEAT-*] markers s19_app_features.py asserts.
rem
rem  Routines (call with `gosub %APPROOT%/APPLIB.BAT::<name> [args]`):
rem    :probe          feature exercise, opt-in via P4_APP_FEATURES=1
rem    :rich           v1.6 language showcase, unconditional, [M-RICH-LIB-*]
rem    :paths          default APPDRIVE/APPROOT when a script runs outside
rem                    the shell's .bat launcher (apps use %APPDRIVE% instead
rem                    of a hardcoded sd:/ so they run from SD or USB)
rem    :box            whole-grid frame: `:box <style> [title]`
rem    :page           page frame across the whole grid (title in the top border)
rem    :bar            full-width bar at an authored row: `:bar <y> <h> <style> [title]`
rem    :panel          full-width panel that grows into the extra panel rows
rem    :footer         full-width panel pinned to the bottom edge
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

rem ---- v1.6 richer-language showcase ----------------------------------
rem Deliberately SEPARATE from :probe: the opt-in probe above is unchanged.
rem Any app can call `gosub %APPROOT%/APPLIB.BAT::rich <TAG>` and get a visible
rem [M-RICH-LIB-*] proof that the new batch surfaces are live: str, map,
rem for /K, func/return and %RESULT%. All values are library-scope temporaries.
:rich
echo [M-RICH-LIB]
set RTAG=%1
if "%RTAG%"=="" set RTAG=APP
set RWORD=p4mini
str /rev RWORD RR
str /count RWORD i RC
str /pad RWORD 10 RP -
map /set RCFG host p4
map /set RCFG port 80
for /K %%k in (RCFG) do echo [M-RICH-LIB-K] %%k=%%l
map /values RCFG RV
func :libscore 7 6
echo [M-RICH-LIB-F] result=%RESULT%
echo [M-RICH-LIB-S] %RTAG% rev=%RR% count=%RC% pad=%RP% vals=%RV[0]%,%RV[1]%
goto :eof

:libscore
return %1*10+%2

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

rem ---- layout helpers ---------------------------------------------------
rem The shell publishes TUICOLS/TUIROWS for every .bat it starts: the logical
rem grid the TUI app surface lays out against (80x25 on the landscape
rem reference board, 80x97 on the 720x1280 portrait panel). These are the one
rem set of frame/width/height helpers every reference app uses:
rem
rem   :box    <style> [title]          frame across the whole grid
rem   :page   <style> [title]          page frame across the whole live grid;
rem                                    the title sits in the top border
rem   :bar    <y> <h> <style> [title]  full-width bar at the authored row
rem   :panel  <y> <h> <style> [title]  full-width panel that grows into the
rem                                    extra panel rows (h is the height on the
rem                                    classic 25-row grid; the bottom edge is
rem                                    clamped to the grid)
rem   :footer <y> <h> <style> [title]  same panel, pinned to the bottom (the
rem                                    rows move down by the extra panel rows)
rem
rem Horizontal text stays on the authored 80-column positions: the logical
rem grid keeps the documented 80 columns on every supported panel (only a
rem panel too narrow for 80 falls back), so no app needs centring maths.
rem
rem A library call runs in its own environment scope (the shell restores the
rem caller on return), so a helper cannot export variables: each one takes the
rem classic 25-row coordinates it would have been authored with and resolves
rem them against %TUICOLS%/%TUIROWS% itself. With TUIROWS=25 every helper
rem draws exactly the authored 80x25 geometry, so landscape screens do not
rem move; on a portrait panel the same call fills the panel. A script run
rem outside the shell launcher (no %TUICOLS%) assumes the 80x25 reference.

rem Whole-grid frame: the page border every full-page app opens with.
:box
if "%TUICOLS%"=="" set TUICOLS=80
if "%TUIROWS%"=="" set TUIROWS=25
draw box 1 1 %TUICOLS% %TUIROWS% %1 %2
goto :eof

rem Page frame: the whole live grid, on both panel shapes. Callers put their
rem content at rows 5+ (and some at the bottom edge), so a 3-row title bar on
rem the classic 80x25 grid framed nothing - the prose/bars sat outside the
rem box and the rest of the page looked empty. Drawing the full grid keeps the
rem title in the top border on every panel and gives tall panels the same
rem full-height page they already had (TUIROWS=97 on the portrait board).
:page
if "%TUICOLS%"=="" set TUICOLS=80
if "%TUIROWS%"=="" set TUIROWS=25
draw box 1 1 %TUICOLS% %TUIROWS% %1 %2
goto :eof

rem Full-width bar at an authored row/height (title bars, help bars): the
rem width follows the grid, the rows stay where the app put them.
:bar
if "%TUICOLS%"=="" set TUICOLS=80
draw box 1 %1 %TUICOLS% %2 %3 %4
goto :eof

rem Full-width panel at row <y>. Its height grows by (TUIROWS-25) so the
rem bottom edge follows the panel; clamped so it can never leave the grid and
rem never collapses below three rows (a box needs two borders + one row).
rem Every value is a working variable of this routine's own scope: a library
rem call cannot leak them into the caller.
:panel
if "%TUICOLS%"=="" set TUICOLS=80
if "%TUIROWS%"=="" set TUIROWS=25
set /a PH=%2 + %TUIROWS% - 25 > NUL
set /a MAXH=%TUIROWS% - %1 + 1 > NUL
if %PH% GTR %MAXH% set PH=%MAXH%
if %PH% LSS 3 set PH=3
draw box 1 %1 %TUICOLS% %PH% %3 %4
goto :eof

rem Full-width panel of the authored height, moved down by the extra rows so
rem it stays anchored to the bottom edge of the panel.
:footer
if "%TUICOLS%"=="" set TUICOLS=80
if "%TUIROWS%"=="" set TUIROWS=25
set /a PY=%1 + %TUIROWS% - 25 > NUL
set /a MINY=%TUIROWS% - %2 + 1 > NUL
if %PY% GTR %MINY% set PY=%MINY%
if %PY% LSS 1 set PY=1
draw box 1 %PY% %TUICOLS% %2 %3 %4
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
