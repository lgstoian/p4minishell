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
if "%P4_APP_FEATURES%"=="1" gosub %APPROOT%/APPLIB.BAT::probe
echo [M-CHAT]
title "Chat"

:menu
draw clear screen
rem Layout: the shell publishes the grid (80x25 landscape, 80x97 portrait).
rem The menu panel takes the extra rows; the tip bar stays on the bottom rows
rem (19..21) with its text inside it (row 21).
if "%TUICOLS%"=="" set TUICOLS=80
if "%TUIROWS%"=="" set TUIROWS=25
set /a TY=%TUIROWS%-4 > NUL
rem The nine menu lines are authored at rows 7..15 of the 80x25 grid. Step
rem them out by the extra panel rows so they spread down a tall menu panel;
rem with TUIROWS=25 the step is 1 and every row stays exactly as authored.
set /a EX=%TUIROWS%-25 > NUL
set /a CS=1+%EX%/9 > NUL
set /a R7=7 > NUL
set /a R8=7+%CS% > NUL
set /a R9=7+2*%CS% > NUL
set /a R10=7+3*%CS% > NUL
set /a R11=7+4*%CS% > NUL
set /a R12=7+5*%CS% > NUL
set /a R13=7+6*%CS% > NUL
set /a R14=7+7*%CS% > NUL
set /a R15=7+8*%CS% > NUL
gosub %APPROOT%/APPLIB.BAT::bar 1 3 double "P4 CHAT 🎵"
draw text 24 2 "Board-to-board chat over the local link" 14
gosub %APPROOT%/APPLIB.BAT::panel 5 13 single "Menu"
draw text 5 %R7% "1  Host      start/stop the SoftAP host"
draw text 5 %R8% "2  Join      join the host AP"
draw text 5 %R9% "3  Send      send a message"
draw text 5 %R10% "4  Poll      fetch new messages"
draw text 5 %R11% "5  Log       show the local log"
draw text 5 %R12% "6  Status    host/peer/sequence state"
draw text 5 %R13% "7  Clear     clear inbox and log"
draw text 5 %R14% "8  Help      command reference"
draw text 5 %R15% "9  Quit"
gosub %APPROOT%/APPLIB.BAT::footer 19 3 single
draw text 5 %TY% "Tip: the host prints 192.168.4.1; the peer joins that address."
draw refresh
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
gosub %APPROOT%/APPLIB.BAT::page single "Chat help"
draw text 3 5 "chat host [ssid]      start the SoftAP (default P4MiniShell-Chat)"
draw text 3 6 "chat host off         stop it, restore STA-only mode"
draw text 3 7 "chat peer <ip>        join the host AP"
draw text 3 8 "chat peer off         leave it"
draw text 3 9 "chat send <text...>   send to the peer"
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
