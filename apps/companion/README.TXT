P4 COMPANION
============

A pure-batch helper app for P4MiniShell. It is written entirely in batch
files - no native code - yet it looks and behaves like a small DOS utility
suite. It exists to exercise the shell's batch engine and to surface bugs
and missing features.

Run it with:

    COMPANION.BAT

WHAT IT TESTS
-------------
  - Menus: menu, choice, set /p, ask, dialogs
  - App mode: a full-screen Live System dashboard that restores on exit
  - File tools: dir, find, trash browser, clipboard, and a note pad
  - Network tools: wifi status/connect, ping, DNS, an httpget fetch, and
    the firmware MQTT event service (net status/sub/pub/msg/outbox)
  - Fun: a tone melody, RGB LED effects, a number-guessing game, calc
  - Settings: persisted across reboots with `appconfig companion`
  - Batch process model: call, call :routine, goto, for /f, pipes,
    redirection, errorlevel / && / ||, setlocal / endlocal

FILES
-----
  COMPANION.BAT   entry point (main menu)
  LIB.BAT         shared routines (banner, settings load, pause)
  SYS.BAT         live system dashboard (app mode)
  FILES.BAT       file tools + note pad (sd:/NOTES)
  NET.BAT         network tools
  FUN.BAT         demos and games
  SET.BAT         settings
  ALIASES.BAT     optional aliases (alias /load ALIASES.BAT)

NETWORK NOTE
------------
Connect Wi-Fi from the Network Tools menu (or pre-connect with
`wifi connect <ssid> <pass>`). The joke fetch uses a configurable URL:
    appconfig companion set fun_url <url>
(default: https://icanhazdadjoke.com/). Offline the tools just say so.

MESSAGING NOTE
--------------
Network Tools -> Messaging (MQTT) shows the persistent event-service state
(read-only, always available). To use it, point at a broker and connect:
    net broker <host> [port] [user]
    net password                 (hidden entry)
    net connect
    net sub p4/demo/#
    net pub p4/demo/heartbeat hi
    net msg
Offline publishes queue in the SD outbox (sd:/NET/OUTBOX/) and flush on
reconnect. Inbound topics can run a batch line via `net onmsg`.
