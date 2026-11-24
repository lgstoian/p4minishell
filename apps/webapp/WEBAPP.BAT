@echo off
rem WEBAPP.BAT - batch shim presenting a page through the webapp facade.
rem The page, CSS and JS come from the same html/css/js engine the browser uses;
rem a native payload can be added later with the same .bat front door.
setlocal
echo [M-WEBAPP] starting

set WEBAPP_PAGE=%APPDRIVE%/WEB/INDEX.HTML
if not exist "%WEBAPP_PAGE%" set WEBAPP_PAGE=sd:/WEB/INDEX.HTML

webapp open /no "%WEBAPP_PAGE%"
if errorlevel 1 goto failed

rem Read the rendered grid back without pixels so suites can assert structure.
webapp dump 1 8 /a
webapp css
echo [M-WEBAPP-DONE]
endlocal
exit /b 0

:failed
echo [M-WEBAPP-FAIL]
endlocal
exit /b 1
