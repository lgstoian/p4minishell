@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem =====================================================================
rem  NET.BAT - Network Tools. Enhanced TUI with draw boxes and status.
rem  Uses TUI for Wi-Fi status, connect, ping, dns, fetch.
rem =====================================================================

:main
if "%P4_APP_FEATURES%"=="1" gosub %APPROOT%/APPLIB.BAT::probe
title "Network Tools"
echo [M-NET]
gosub :rich_net
rem Layout: the shell publishes the grid (80x25 landscape, 80x97 portrait).
rem The Tools panel takes the extra rows; Status stays on the bottom rows.
if "%TUICOLS%"=="" set TUICOLS=80
if "%TUIROWS%"=="" set TUIROWS=25
set /a SY=%TUIROWS%-8 > NUL
rem The Messaging help lines are authored at rows 15..22 of the 80x25 grid.
set NX=0
if %TUIROWS% GTR 25 set /a NX=%TUIROWS%-26 > NUL
set /a N15=15+%NX% > NUL
set /a N16=16+%NX% > NUL
set /a N17=17+%NX% > NUL
set /a N18=18+%NX% > NUL
set /a N19=19+%NX% > NUL
set /a N20=20+%NX% > NUL
set /a N22=22+%NX% > NUL
draw clear screen
set DOS_THEME=ice
gosub %APPROOT%/APPLIB.BAT::header "NETWORK TOOLS" "[M-RICH-NET] wifi + dns + mqtt"
gosub %APPROOT%/APPLIB.BAT::tile 1 5 44 11 double "LINK QUALITY"
gosub %APPROOT%/APPLIB.BAT::meter 4 8 36 82 "RSSI 82%"
gosub %APPROOT%/APPLIB.BAT::meter 4 11 36 25 "NOISE 25%"
gosub %APPROOT%/APPLIB.BAT::tile 47 5 33 11 single "KNOWN NETS"
gosub %APPROOT%/APPLIB.BAT::table 49 8 "SSID|BSSID|Band" "lab|a4:11|2.4" "mesh|b8:02|5" "guest|c1:90|2.4"
gosub %APPROOT%/APPLIB.BAT::statusbar 21 "link=offline  dns=local" "1 wifi  2 join  3 ping  4 dns  q quit"
echo [M-DOS-NET] screen=NET-TOOLS panels=3 head="NETWORK TOOLS" theme=ice bars=2 table=Y spark=0 status=Y
draw text 32 2 "NETWORK TOOLS ★" 14
gosub %APPROOT%/APPLIB.BAT::panel 4 10 single "Tools"
draw text 5 6 "Wi-Fi status, connect, ping, dns, fetch"
gosub %APPROOT%/APPLIB.BAT::footer 15 8 single "Status"
draw text 5 %SY% "Offline handling: all ops degrade gracefully"
list "Network Tools" "Wi-Fi status (TUI)" "Connect Wi-Fi" "Pick known network" "Ping (TUI)" "DNS lookup" "Fetch a joke" "Messaging (MQTT)" "Back"
on %errorlevel%+1 goto :status,:connect,:known,:ping,:dns,:joke,:msg,:back
goto :back

rem ---- v1.6 richer-language showcase ------------------------------------
rem [M-RICH-NET] previews the tool menu with map /set + for /K, str /pad and
rem func/return; Wi-Fi/MQTT state is not touched.
:rich_net
echo [M-RICH-NET] tools language tour
map /set NETTOOLS wifi status
map /set NETTOOLS mqtt net
for /K %%k in (NETTOOLS) do echo [M-RICH-NET-K] %%k=%%l
set NW=network
str /pad NW 10 NP .
func :netsum 8 9
echo [M-RICH-NET-F] sum=%RESULT%
echo [M-RICH-NET-S] pad=%NP%
return

:netsum
return %1+%2

:status
tui fullscreen on
draw clear screen
gosub %APPROOT%/APPLIB.BAT::page single
draw text 32 2 "WI-FI STATUS" 14
wifi status > %APPDRIVE%/_net.txt
view %APPDRIVE%/_net.txt
del /p %APPDRIVE%/_net.txt > NUL
wifi known > %APPDRIVE%/_known.txt
view %APPDRIVE%/_known.txt
del /p %APPDRIVE%/_known.txt > NUL
tui fullscreen off
draw clear screen
goto main

:connect
gosub %APPROOT%/APPLIB.BAT::page single
draw text 35 2 "CONNECT WI-FI" 14
ask /v:ssid "SSID"
if errorlevel 1 goto main
ask /v:pass "Password" /p
if errorlevel 1 goto main
if "%ssid%"=="" goto main
draw text 5 10 "Connecting to %ssid%..." 14
wifi connect %ssid% %pass%
if errorlevel 1 dialog "Wi-Fi" "Connection failed." "OK"
if not errorlevel 1 dialog "Wi-Fi" "Connected to %ssid%!" "OK"
goto main

:known
wifi known > %APPDRIVE%/_known.txt
view %APPDRIVE%/_known.txt
del /p %APPDRIVE%/_known.txt > NUL
ask /v:ssid "SSID from known (or blank to abort)"
if errorlevel 1 goto main
if "%ssid%"=="" goto main
ask /v:pass "Password" /p
if errorlevel 1 goto main
wifi connect %ssid% %pass%
if errorlevel 1 dialog "Wi-Fi" "Connection failed." "OK"
goto main

:ping
gosub %APPROOT%/APPLIB.BAT::page single
draw text 38 2 "PING" 14
ask /v:host "Host to ping" "1.1.1.1"
if errorlevel 1 goto main
draw text 5 10 "Pinging %host%..." 14
ping %host% 3 > %APPDRIVE%/_ping.txt
view %APPDRIVE%/_ping.txt
del /p %APPDRIVE%/_ping.txt > NUL
goto main

:dns
gosub %APPROOT%/APPLIB.BAT::page single
draw text 37 2 "DNS LOOKUP" 14
ask /v:host "Hostname" "example.com"
if errorlevel 1 goto main
draw text 5 10 "Resolving %host%..." 14
dns %host% > %APPDRIVE%/_dns.txt
view %APPDRIVE%/_dns.txt
del /p %APPDRIVE%/_dns.txt > NUL
goto main

:joke
appconfig companion get fun_url > %APPDRIVE%/funurl.txt
if errorlevel 1 goto :joke_default
rem `for /f` does not expand %APPDRIVE% in its file list.
set OLDDIR=%CD%
cd %APPDRIVE%/
for /f "eol= tokens=*" %%u in (funurl.txt) do set funurl=%%u
cd %OLDDIR%
del /p %APPDRIVE%/funurl.txt > NUL
goto :joke_go
:joke_default
del /p %APPDRIVE%/funurl.txt > NUL
set funurl=https://icanhazdadjoke.com/
:joke_go
gosub %APPROOT%/APPLIB.BAT::page single
draw text 35 2 "FETCH JOKE" 14
draw text 5 10 "Fetching: %funurl%" 14
httpget %funurl% %APPDRIVE%/_joke.txt
echo [M-NET-FETCHED]
if errorlevel 1 dialog "Fetch" "Fetch failed - offline or endpoint down." "OK"
if not errorlevel 1 view %APPDRIVE%/_joke.txt
del /p %APPDRIVE%/_joke.txt > NUL
goto main

:back
echo [M-NET-BACK]
draw clear screen
goto :eof

rem ---- Messaging (MQTT event service) --------------------------------
rem  The firmware owns one persistent MQTT session (components/networking/
rem  netsvc.c). `net status`/`net outbox` are read-only and always work;
rem  connect/sub/pub need a LAN broker and degrade to `waiting-wifi`/queued
rem  when offline. Publishers journal to the SD outbox and flush on connect.
:msg
echo [M-NET-MSG]
draw clear screen
gosub %APPROOT%/APPLIB.BAT::bar 1 3 single
draw text 31 2 "MESSAGING (MQTT)" 14
gosub %APPROOT%/APPLIB.BAT::bar 4 8 single "Service"
net status
gosub %APPROOT%/APPLIB.BAT::panel 13 10 single "Try it"
draw text 5 %N15% "net broker <host> [port] [user]   set the broker"
draw text 5 %N16% "net password                       set credentials"
draw text 5 %N17% "net connect | net disconnect       control the session"
draw text 5 %N18% "net sub p4/demo/#                  subscribe"
draw text 5 %N19% "net pub p4/demo/heartbeat hi       publish"
draw text 5 %N20% "net msg | net outbox               last message / queue"
draw text 5 %N22% "Offline publishes queue in the SD outbox."
net outbox /b
echo [M-NET-OUTBOX]
rem A list modal (not a pause) so the messaging page can be captured by the
rem serial `screenshot` and driven by the documented entry number.
list "Messaging" "Back"
echo [M-NET-MSG-BACK]
draw clear screen
goto main
