@echo off
rem Shared batch/applib routine library fixture (NOT native code loading).
rem The `lib load UTIL` facade resolves LIBS.INI and calls this routine through
rem the ONE `call <file.bat>::<routine>` engine, with variable isolation.
goto :eof

:probe
echo [M-LIB-PROBE] ok
exit /b 0
