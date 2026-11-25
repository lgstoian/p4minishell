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
rem
rem ---- DOS UI vocabulary (one renderer, one palette decision point) -----
rem Apps set `DOS_THEME` to amber|green|ice|mono before calling these. Every
rem helper resolves the theme through :palette: there is no second palette and
rem no second drawing engine. All primitives are shipped batch verbs (draw
rem box/line/text/run/bar/table/list, tui dump).
rem   :header   <title> [subtitle]      box-drawn title band + colour run
rem   :tile     <x> <y> <w> <h> <style> <title>   tiled CP437 panel
rem   :statusbar <y> <left> <right>     bottom key-hint/status band
rem   :meter    <x> <y> <w> <pct> [label]        CP437 progress/data bar
rem   :table    <x> <y> <heads> <r1> <r2> <r3>   bordered data table
rem   :sparkline <x> <y> <w> v1..v6     six-cell ASCII sparkline
rem   :pager    <title> <y> <h> <file>  scrollable panel over a real file
rem   :themebox <x> <y> <w>             palette swatch panel
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
echo [M-DOS-APPLIB] screen=APPLIB-HEADLESS panels=0 head="NONE" theme=mono bars=0 table=N spark=0 status=N
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

rem ---- DOS UI palette --------------------------------------------------
rem The ONE colour decision point. `call :palette` from a helper sets local
rem PFG/PBG/PACC/PSTYLE for the rest of that helper's scope. A library gosub
rem cannot export variables, so callers only publish DOS_THEME (amber, green,
rem ice or mono); the default is green. All values are DOS 0-15 CGA entries,
rem never CSS gradients or 24-bit art colours.
:palette
set PFG=10
set PBG=0
set PACC=14
set PSTYLE=double
if "%DOS_THEME%"=="amber" set PFG=14
if "%DOS_THEME%"=="amber" set PACC=12
if "%DOS_THEME%"=="ice" set PFG=11
if "%DOS_THEME%"=="ice" set PBG=1
if "%DOS_THEME%"=="ice" set PACC=15
if "%DOS_THEME%"=="mono" set PFG=7
if "%DOS_THEME%"=="mono" set PACC=15
if "%DOS_THEME%"=="mono" set PSTYLE=single
goto :eof

rem ---- DOS UI helpers --------------------------------------------------
rem Title band: a full-width double-line box with the app title in the top
rem border and a coloured one-line subtitle/identity run in row 2.
:header
if "%TUICOLS%"=="" set TUICOLS=80
call :palette
draw box 1 1 %TUICOLS% 3 %PSTYLE% %PFG% %PBG% %1
draw line 1 3 %TUICOLS% 3 single %PFG% %PBG%
if not "%2"=="" draw run 3 2 %2 /fg %PACC% /bg %PBG%
goto :eof

rem Tiled panel: an individually titled CP437 box (single/double/rounded).
:tile
call :palette
draw box %1 %2 %3 %4 %5 %PFG% %PBG% %6
goto :eof

rem Bottom status/key band. The right-hand text is a colour run so key hints
rem stay legible on every theme while the left text carries live data.
:statusbar
if "%TUICOLS%"=="" set TUICOLS=80
call :palette
set /a SBROW=%1+1 > NUL
draw box 1 %1 %TUICOLS% 3 single %PFG% %PBG%
draw run 3 %SBROW% %2 /fg %PACC% /bg %PBG%
if not "%3"=="" draw run 42 %SBROW% %3 /fg %PACC% /bg %PBG%
goto :eof

rem Data bar (the panel-embedded meter, distinct from the legacy :bar frame).
:meter
call :palette
draw bar %1 %2 %3 %4 # . %PACC% %PBG%
if not "%5"=="" draw run %1 %2 %5 /fg %PFG% /bg %PBG%
goto :eof

rem Bordered table over the shipped `draw table` primitive.
:table
call :palette
draw table %1 %2 %PFG% %PBG% %3 %4 %5 %6
goto :eof

rem Six-cell ASCII/CP437 sparkline: one short `draw bar` per sample.
:sparkline
call :palette
set SPX=%1
draw bar %SPX% %2 %3 %4 # . %PACC% %PBG%
set /a SPX+=%3+1 > NUL
draw bar %SPX% %2 %3 %5 # . %PACC% %PBG%
set /a SPX+=%3+1 > NUL
draw bar %SPX% %2 %3 %6 # . %PACC% %PBG%
set /a SPX+=%3+1 > NUL
draw bar %SPX% %2 %3 %7 # . %PACC% %PBG%
set /a SPX+=%3+1 > NUL
draw bar %SPX% %2 %3 %8 # . %PACC% %PBG%
set /a SPX+=%3+1 > NUL
draw bar %SPX% %2 %3 %9 # . %PACC% %PBG%
goto :eof

rem Scrollable pager panel over a real on-device file.
:pager
call :palette
draw list 1 %2 %TUICOLS% %3 %APPDRIVE%/%4 %PFG% %PBG% /title:%1
goto :eof

rem Theme swatch: four labelled colour runs inside a titled panel.
:themebox
call :palette
set /a TBX=%1+2 > NUL
set /a TBY=%2+2 > NUL
set /a TBY2=%2+3 > NUL
draw box %1 %2 %3 5 double %PFG% %PBG% PALETTE
draw run %TBX% %TBY% "AMBER  GREEN  ICE  MONO" /fg %PACC% /bg %PBG%
draw run %TBX% %TBY2% "%DOS_THEME% / %PSTYLE%" /fg %PFG% /bg %PBG%
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
