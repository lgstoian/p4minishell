@echo off
rem HYBRID.BAT - hybrid app shim: every app launches through a .BAT file,
rem even when the work happens in a linked-in C entry (see ABI.md).
rem Runs the `hello` native entry, then branches on its ERRORLEVEL and
rem environment results exactly like a pure-batch app would.
echo [M-HYBRID] start 
echo [M-DOS-HYBRID] screen=HYBRID-HEADLESS panels=0 head="NONE" theme=mono bars=0 table=N spark=0 status=N
if "%P4_APP_FEATURES%"=="1" gosub %APPROOT%/APPLIB.BAT::probe
title "Hybrid Demo"
hello test
if not "%HELLO_RESULT%"=="ok" echo [M-HYBRID] BAD result
if errorlevel 1 echo [M-HYBRID] BAD errorlevel
echo [M-HYBRID] native said HELLO_RESULT=%HELLO_RESULT%
rem ---- v1.6 signed native payload showcase ------------------------------
rem [M-RICH-HYBRID] uses the documented `.bat` front door for a signed .P4X.
rem The demo envelope is a host TEST fixture (test-only key), so on a device
rem without NATIVE/DEMO.P4X the skip is reported honestly, never faked.
if not exist %APPDRIVE%/NATIVE/DEMO.P4X goto :rich_native_skip
natrun DEMO
echo [M-RICH-HYBRID-NATIVE] signed P4X launched
goto :rich_native_done
:rich_native_skip
echo [M-RICH-HYBRID-SKIP] no signed demo payload installed
:rich_native_done
echo [M-HYBRID-DONE]
exit /b 0
