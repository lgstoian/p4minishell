@echo off
set NAME=p4
set /a N=1+2
if "%NAME%"=="p4" echo HOST NAME OK
call :sub
echo after call N=%N%
goto :eof

:sub
echo in sub NAME=%NAME%
echo /raw [M-HOST-BATCH-DONE]
return
