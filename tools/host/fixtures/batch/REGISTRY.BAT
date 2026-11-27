@echo off
rem REGISTRY.BAT - real-interpreter exercise of reg / lib / try+finally.
rem Every marker comes from a real verb on the one host interpreter; no line
rem is a canned string from the gate. The registry is read back after writing
rem so a fake (non-persisted) write cannot satisfy the baseline.

reg /b set sd:/REGISTRY.INI HOST p4
echo [M-REG-SET] %ERRORLEVEL%
reg /b set sd:/REGISTRY.INI PORT 80
reg /b get sd:/REGISTRY.INI HOST
echo [M-REG-GET] %ERRORLEVEL%
reg /b list sd:/REGISTRY.INI
echo [M-REG-LIST] %ERRORLEVEL%
reg /b del sd:/REGISTRY.INI PORT
reg /b list sd:/REGISTRY.INI
echo [M-REG-DEL] %ERRORLEVEL%

lib /b list
echo [M-LIB-LIST] %ERRORLEVEL%
lib /b info UTIL
echo [M-LIB-INFO] %ERRORLEVEL%
lib /b load UTIL
echo [M-LIB-CALL] %ERRORLEVEL%

try :catch
finally :clean
trap /raise 7 "boom"
echo [M-TRY-SKIPPED]
exit /b 0

:catch
echo [M-TRY-CATCH] %TRAP_CODE%
exit /b 7

:clean
echo [M-TRY-FINALLY] done
echo [M-HOST-BATCH-DONE]
