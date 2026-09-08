@echo off
rem =====================================================================
rem  NET.BAT - Network Tools. Enhanced TUI with draw boxes and status.
rem  Uses TUI for Wi-Fi status, connect, ping, dns, fetch.
rem =====================================================================

:main
echo [M-NET]
draw clear screen
draw box 1 1 80 3 single
draw text 32 2 "NETWORK TOOLS" 14
draw box 1 4 80 10 single "Tools"
draw text 5 6 "Wi-Fi status, connect, ping, dns, fetch"
draw box 1 15 80 8 single "Status"
draw text 5 17 "Offline handling: all ops degrade gracefully"
list "Network Tools" "Wi-Fi status (TUI)" "Connect Wi-Fi" "Pick known network" "Ping (TUI)" "DNS lookup" "Fetch a joke" "Back"
if errorlevel 255 goto :back
if errorlevel 6 goto :back
if errorlevel 5 goto :joke
if errorlevel 4 goto :dns
if errorlevel 3 goto :ping
if errorlevel 2 goto :known
if errorlevel 1 goto :connect
if errorlevel 0 goto :status
goto main

:status
tui fullscreen on
draw clear screen
draw box 1 1 80 3 single
draw text 32 2 "WI-FI STATUS" 14
wifi status > _net.txt
view _net.txt
del /p _net.txt
wifi known > _known.txt
view _known.txt
del /p _known.txt
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
del /p _known.txt
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
del /p _ping.txt
goto main

:dns
draw box 1 1 80 3 single
draw text 37 2 "DNS LOOKUP" 14
ask /v:host "Hostname" "example.com"
if errorlevel 1 goto main
draw text 5 10 "Resolving %host%..." 14
dns %host% > _dns.txt
view _dns.txt
del /p _dns.txt
goto main

:joke
appconfig companion get fun_url > funurl.txt
if errorlevel 1 goto :joke_default
for /f "tokens=*" %%u in (funurl.txt) do set funurl=%%u
del /p funurl.txt
goto :joke_go
:joke_default
del /p funurl.txt
set funurl=https://icanhazdadjoke.com/
:joke_go
draw box 1 1 80 3 single
draw text 35 2 "FETCH JOKE" 14
draw text 5 10 "Fetching: %funurl%" 14
httpget %funurl% _joke.txt
echo [M-NET-FETCHED]
if errorlevel 1 dialog "Fetch" "Fetch failed - offline or endpoint down." "OK"
if not errorlevel 1 view _joke.txt
del /p _joke.txt
goto main

:back
echo [M-NET-BACK]
draw clear screen
goto :eof
