@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem =====================================================================
rem  CHAT.BAT - board-to-board chat front-end (reference app).
rem
rem  Thin menu over the `chat` verb family: one board hosts a SoftAP, the
rem  other joins it, and messages travel over the board's own HTTP server.
rem  No external AP, broker or cable is involved.
rem
rem  Serial driving: type the menu number then Enter (the same convention
rem  as the other list-driven apps). `Host` and `Join` ask for the peer
rem  address; `Send` asks for the message text.
rem
rem  Markers: [M-CHAT] / [M-CHAT-SENT] / [M-CHAT-POLL] / [M-CHAT-LOG] /
rem           [M-CHAT-STATUS] / [M-CHAT-HOST] / [M-CHAT-PEER] /
rem           [M-CHAT-DONE].
rem  Foreground-only: the menu renders with the TUI/draw surface.
rem =====================================================================
setlocal
if "%P4_APP_FEATURES%"=="1" gosub APPLIB.BAT::probe
echo [M-CHAT]
title "Chat"

:menu
draw clear screen
draw box 1 1 80 3 double "P4 CHAT"
draw text 24 2 "Board-to-board chat over the local link" 14
draw box 1 5 80 13 single "Menu"
draw text 5 7  "1  Host      start/stop the SoftAP host"
draw text 5 8  "2  Join      join the host AP"
draw text 5 9  "3  Send      send a message"
draw text 5 10 "4  Poll      fetch new messages"
draw text 5 11 "5  Log       show the local log"
draw text 5 12 "6  Status    host/peer/sequence state"
draw text 5 13 "7  Clear     clear inbox and log"
draw text 5 14 "8  Help      command reference"
draw text 5 15 "9  Quit"
draw box 1 19 80 3 single
draw text 5 21 "Tip: the host prints 192.168.4.1; the peer joins that address."
list "P4 Chat" "Host" "Join" "Send" "Poll" "Log" "Status" "Clear" "Help" "Quit"
on %errorlevel%+1 goto :host,:peer,:send,:poll,:log,:status,:clear,:help,:quit
goto :menu

:host
chat status
set /p CH=Host AP: start, stop or leave blank to keep: 
if /i "%CH%"=="start" goto :host_on
if /i "%CH%"=="stop" goto :host_off
goto :menu
:host_on
chat host on
echo [M-CHAT-HOST]
goto :menu
:host_off
chat host off
echo [M-CHAT-HOST]
goto :menu

:peer
set /p CHIP=Peer IP (blank to leave): 
if "%CHIP%"=="" goto :peer_off
chat peer %CHIP%
echo [M-CHAT-PEER]
goto :menu
:peer_off
chat peer off
echo [M-CHAT-PEER]
goto :menu

:send
set /p MSG=Message: 
if "%MSG%"=="" goto :menu
chat send %MSG%
echo [M-CHAT-SENT]
goto :menu

:poll
chat poll
echo [M-CHAT-POLL]
goto :menu

:log
chat log
echo [M-CHAT-LOG]
goto :menu

:status
chat status
echo [M-CHAT-STATUS]
goto :menu

:clear
chat clear
echo [M-CHAT-CLEAR]
goto :menu

:help
draw clear screen
draw box 1 1 80 3 single "Chat help"
draw text 3 5  "chat host [ssid]      start the SoftAP (default P4MiniShell-Chat)"
draw text 3 6  "chat host off         stop it, restore STA-only mode"
draw text 3 7  "chat peer <ip>        join the host AP"
draw text 3 8  "chat peer off         leave it"
draw text 3 9  "chat send <text...>   send to the peer"
draw text 3 10 "chat poll             fetch new messages"
draw text 3 11 "chat log              show the local log"
draw text 3 12 "chat clear            clear inbox and log"
draw text 3 13 "chat status           host/peer/sequence state"
draw text 3 14 "chat onmsg <line>     run a batch line per message"
draw text 3 16 "Read-only transport: no remote command execution."
pause
goto :menu

:quit
echo [M-CHAT-DONE]
tui fullscreen off
draw clear screen
goto :eof
