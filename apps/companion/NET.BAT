@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem =====================================================================
rem  NET.BAT - Network Tools. Enhanced TUI with draw boxes and status.
rem  Uses TUI for Wi-Fi status, connect, ping, dns, fetch.
rem =====================================================================

:main
title "Network Tools"
echo [M-NET]
draw clear screen
draw box 1 1 80 3 single
draw text 32 2 "NETWORK TOOLS" 14
draw box 1 4 80 10 single "Tools"
draw text 5 6 "Wi-Fi status, connect, ping, dns, fetch"
draw box 1 15 80 8 single "Status"
draw text 5 17 "Offline handling: all ops degrade gracefully"
list "Network Tools" "Wi-Fi status (TUI)" "Connect Wi-Fi" "Pick known network" "Ping (TUI)" "DNS lookup" "Fetch a joke" "Messaging (MQTT)" "Back"
on %errorlevel%+1 goto :status,:connect,:known,:ping,:dns,:joke,:msg,:back
goto :back

:status
tui fullscreen on
draw clear screen
draw box 1 1 80 3 single
draw text 32 2 "WI-FI STATUS" 14
wifi status > _net.txt
view _net.txt
del /p _net.txt > NUL
wifi known > _known.txt
view _known.txt
del /p _known.txt > NUL
tui fullscreen off
draw clear screen
goto main

:connect
draw box 1 1 80 3 single
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
wifi known > _known.txt
view _known.txt
del /p _known.txt > NUL
ask /v:ssid "SSID from known (or blank to abort)"
if errorlevel 1 goto main
if "%ssid%"=="" goto main
ask /v:pass "Password" /p
if errorlevel 1 goto main
wifi connect %ssid% %pass%
if errorlevel 1 dialog "Wi-Fi" "Connection failed." "OK"
goto main

:ping
draw box 1 1 80 3 single
draw text 38 2 "PING" 14
ask /v:host "Host to ping" "1.1.1.1"
if errorlevel 1 goto main
draw text 5 10 "Pinging %host%..." 14
ping %host% 3 > _ping.txt
view _ping.txt
del /p _ping.txt > NUL
goto main

:dns
draw box 1 1 80 3 single
draw text 37 2 "DNS LOOKUP" 14
ask /v:host "Hostname" "example.com"
if errorlevel 1 goto main
draw text 5 10 "Resolving %host%..." 14
dns %host% > _dns.txt
view _dns.txt
del /p _dns.txt > NUL
goto main

:joke
appconfig companion get fun_url > funurl.txt
if errorlevel 1 goto :joke_default
for /f "eol= tokens=*" %%u in (funurl.txt) do set funurl=%%u
del /p funurl.txt > NUL
goto :joke_go
:joke_default
del /p funurl.txt > NUL
set funurl=https://icanhazdadjoke.com/
:joke_go
draw box 1 1 80 3 single
draw text 35 2 "FETCH JOKE" 14
draw text 5 10 "Fetching: %funurl%" 14
httpget %funurl% _joke.txt
echo [M-NET-FETCHED]
if errorlevel 1 dialog "Fetch" "Fetch failed - offline or endpoint down." "OK"
if not errorlevel 1 view _joke.txt
del /p _joke.txt > NUL
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
draw box 1 1 80 3 single
draw text 31 2 "MESSAGING (MQTT)" 14
draw box 1 4 80 8 single "Service"
net status
draw box 1 13 80 10 single "Try it"
draw text 5 15 "net broker <host> [port] [user]   set the broker"
draw text 5 16 "net password                       set credentials"
draw text 5 17 "net connect | net disconnect       control the session"
draw text 5 18 "net sub p4/demo/#                  subscribe"
draw text 5 19 "net pub p4/demo/heartbeat hi       publish"
draw text 5 20 "net msg | net outbox               last message / queue"
draw text 5 22 "Offline publishes queue in the SD outbox."
net outbox /b
echo [M-NET-OUTBOX]
draw text 5 24 "Press any key to return."
pause
echo [M-NET-MSG-BACK]
draw clear screen
goto main
