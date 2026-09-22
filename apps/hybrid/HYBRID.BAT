@echo off
rem HYBRID.BAT - hybrid app shim: every app launches through a .BAT file,
rem even when the work happens in a linked-in C entry (see ABI.md).
rem Runs the `hello` native entry, then branches on its ERRORLEVEL and
rem environment results exactly like a pure-batch app would.
echo [M-HYBRID] start
hello test
if not "%HELLO_RESULT%"=="ok" echo [M-HYBRID] BAD result
if errorlevel 1 echo [M-HYBRID] BAD errorlevel
echo [M-HYBRID] native said HELLO_RESULT=%HELLO_RESULT%
echo [M-HYBRID-DONE]
exit /b 0
