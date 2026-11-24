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

rem ---- v1.6 webapp showcase ---------------------------------------------
rem [M-RICH-WEBAPP] opens the bundled HTML5 page (forms, canvas, CSS gradients,
rem a JS timer) through the SAME webapp facade, then drives an event and a tick.
set WEBAPP_RICH=%APPDRIVE%/WEB/RICH.HTML
if not exist "%WEBAPP_RICH%" set WEBAPP_RICH=sd:/WEB/RICH.HTML
webapp open /no "%WEBAPP_RICH%"
if errorlevel 1 goto rich_failed
echo [M-RICH-WEBAPP] forms+canvas+timers live
webapp js "console.log('p4-rich')"
webapp event p4run click
webapp tick
webapp dump 1 8 /a
echo [M-RICH-WEBAPP-DONE]
goto rich_done
:rich_failed
echo [M-RICH-WEBAPP-FAIL]
:rich_done
echo [M-WEBAPP-DONE]
endlocal
exit /b 0

:failed
echo [M-WEBAPP-FAIL]
endlocal
exit /b 1
