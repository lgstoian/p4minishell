# Reference app gallery

Every screenshot below is captured live from the device by
[`tools/suites/s25_apps_gallery.py`](../tools/suites/s25_apps_gallery.py)
(JC1060P470C, 1024x600). The apps are batch-first and run from SD or USB;
metadata comes from each app's `APPINFO`.

The reference-app list in [`readme.md`](../readme.md) names the ship set; each
section's title and description come from the app's `APPINFO` where one exists
(the extra companion/library/service screens use their file name).

## ADVENTURE

![ADVENTURE](assets/apps/adventure.png)

*Screenshot: `docs/assets/apps/adventure.png`*

## Agenda

![Agenda](assets/apps/agenda.png)

Prints today's date and the next alarm, then notifies

*Screenshot: `docs/assets/apps/agenda.png`*

## ALIASES

![ALIASES](assets/apps/aliases.png)

*Screenshot: `docs/assets/apps/aliases.png`*

## APPL

![APPL](assets/apps/appl.png)

*Screenshot: `docs/assets/apps/appl.png`*

## Bounce Demo

![Bounce Demo](assets/apps/bounce.png)

Gfx pixel-canvas bouncing ball on the app drive: raster verbs plus batch math in a killable frame loop

*Screenshot: `docs/assets/apps/bounce.png`*

## Chat

![Chat](assets/apps/chat.png)

Board-to-board chat over the local link (SoftAP + HTTP)

*Screenshot: `docs/assets/apps/chat.png`*

## P4 Companion

![P4 Companion](assets/apps/companion.png)

A pure-batch system helper: dashboard, files, network, games, settings

*Screenshot: `docs/assets/apps/companion.png`*

## Control Flow

![Control Flow](assets/apps/control.png)

Demo of gosub / return / on dispatch plus switch, while and DOS-parity constructs

*Screenshot: `docs/assets/apps/control.png`*

## P4 Diagnostics

![P4 Diagnostics](assets/apps/diag.png)

System health, capability and frame-rate diagnostics console; mem shows internal heap + PSRAM

*Screenshot: `docs/assets/apps/diag.png`*

## Elite Trader

![Elite Trader](assets/apps/elite.png)

Turn-based space trading: markets, travel, pirates, saves on the app drive

*Screenshot: `docs/assets/apps/elite.png`*

## FILES-NOTES

![FILES-NOTES](assets/apps/files-notes.png)

*Screenshot: `docs/assets/apps/files-notes.png`*

## File Tools

![File Tools](assets/apps/files.png)

Browse, view, search, trash, clipboard and note pad on %APPDRIVE%

*Screenshot: `docs/assets/apps/files.png`*

## Fun / Demos

![Fun / Demos](assets/apps/fun.png)

Melody, RGB effects, number guessing, calc, Snake and Elite Trader

*Screenshot: `docs/assets/apps/fun.png`*

## GFX

![GFX](assets/apps/gfx.png)

*Screenshot: `docs/assets/apps/gfx.png`*

## Hybrid demo

![Hybrid demo](assets/apps/hybrid.png)

Batch shim driving the linked hello entry (hybrid app model)

*Screenshot: `docs/assets/apps/hybrid.png`*

## LIB

![LIB](assets/apps/lib.png)

*Screenshot: `docs/assets/apps/lib.png`*

## MEMLOG

![MEMLOG](assets/apps/memlog.png)

*Screenshot: `docs/assets/apps/memlog.png`*

## Mood Dashboard

![Mood Dashboard](assets/apps/mood.png)

Live board mood from free heap: RGB/audio bands plus mem (heap + PSRAM) and sysinfo snapshots

*Screenshot: `docs/assets/apps/mood.png`*

## Mood Sampler Service

![Mood Sampler Service](assets/apps/moodsvc.png)

Headless heap sampler feeding the Mood dashboard via sd:/MOODLAST.INI (SD-only store; install: service install moodsampler"MOODSVC" /restart)

*Screenshot: `docs/assets/apps/moodsvc.png`*

## NET-MSG

![NET-MSG](assets/apps/net-msg.png)

*Screenshot: `docs/assets/apps/net-msg.png`*

## Network Tools

![Network Tools](assets/apps/net.png)

Wi-Fi status, connect, ping, DNS, fetch and the MQTT messaging view

*Screenshot: `docs/assets/apps/net.png`*

## Zettelkasten Notes

![Zettelkasten Notes](assets/apps/notes.png)

Linked notes on the db record store: categories, search, export/import (db store is SD-only)

*Screenshot: `docs/assets/apps/notes.png`*

## PalmTop Demos

![PalmTop Demos](assets/apps/palmtop.png)

Reference app for timer/calc/csv/crypt/bind/tcpterm on the app drive

*Screenshot: `docs/assets/apps/palmtop.png`*

## Pictures

![Pictures](assets/apps/pics.png)

BMP image viewer, TUI draw, and gfx canvas blit on the app drive (needs PHOTO.BMP)

*Screenshot: `docs/assets/apps/pics.png`*

## Package Test

![Package Test](assets/apps/pkgtest.png)

Minimal app installed from a PKGS bundle via pkg install

*Screenshot: `docs/assets/apps/pkgtest.png`*

## Plotter

![Plotter](assets/apps/plot.png)

Scientific graphs: sin/cos on a world-coordinate window with axes and grid

*Screenshot: `docs/assets/apps/plot.png`*

## Preferences

![Preferences](assets/apps/set.png)

Brightness, volume, rotate, theme, fonts, input and security (CONFIG.SYS)

*Screenshot: `docs/assets/apps/set.png`*

## Snake

![Snake](assets/apps/snake.png)

TUI snake: fixed body slots, timeout steering, one flush per frame

*Screenshot: `docs/assets/apps/snake.png`*

## SVC

![SVC](assets/apps/svc.png)

*Screenshot: `docs/assets/apps/svc.png`*

## Live System

![Live System](assets/apps/sys.png)

Live dashboard: memory (heap + PSRAM), tasks, services, packages

*Screenshot: `docs/assets/apps/sys.png`*

## Task Manager

![Task Manager](assets/apps/taskmgr.png)

Batch task manager: pool jobs, priorities, services, events; pool view reports mem incl. PSRAM

*Screenshot: `docs/assets/apps/taskmgr.png`*

## Commander

![Commander](assets/apps/tcmd.png)

Dual-pane file commander: browse selectors plus copy/move/delete/mkdir/rename/view/edit on the app drive

*Screenshot: `docs/assets/apps/tcmd.png`*

## Touch Test

![Touch Test](assets/apps/uitest.png)

Synthetic touch automation demo (ui verbs)

*Screenshot: `docs/assets/apps/uitest.png`*

## Browser

![Browser](assets/apps/web.png)

Renders a basic website from <drive>/WEB/ with the html page verb

*Screenshot: `docs/assets/apps/web.png`*

## Writer Deck

![Writer Deck](assets/apps/writer.png)

Templates, focus writing, and Markdown export on the app drive

*Screenshot: `docs/assets/apps/writer.png`*
