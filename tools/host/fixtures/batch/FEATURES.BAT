@echo off
rem FEATURES.BAT - real-interpreter exercise of the additive batch language
rem Each new surface emits an assertable [M-HOST-*] transcript marker.
set S=abc
str /rev S R
str /count S b C
str /pad S 5 R2 -
echo [M-HOST-STR] rev=%R% count=%C% pad=%R2%
map /set CFG host p4
map /set CFG port 80
for /K %%k in (CFG) do echo [M-HOST-FORK] %%k=%%l
map /values CFG VALS
echo [M-HOST-MAP] vals=%VALS[0]%,%VALS[1]%
map /incr CFG hits 3
map /get CFG hits H
echo [M-HOST-MAP] hits=%H%
func double 21
echo [M-HOST-FUNC] result=%RESULT%
goto :funcdone

:double
return %1*2

:funcdone
trap /on errorlevel :onerr
trap /raise 4 boom
echo [M-HOST-TRAP] skipped
goto :aftererr

:onerr
echo [M-HOST-TRAP] code=%TRAP_CODE% msg=%TRAP_MSG%
goto :aftererr

:aftererr
trap /clear
trap /on exit :onexit
echo /raw [M-HOST-BATCH-DONE]
exit /b 7

:onexit
echo [M-HOST-EXIT] code=%TRAP_CODE%
exit /b 0
