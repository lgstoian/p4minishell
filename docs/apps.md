# Reference app gallery

Screenshots are normally captured live from the device by
[`tools/suites/s25_apps_gallery.py`](../tools/suites/s25_apps_gallery.py)
(JC1060P470C, 1024x600). Any image explicitly labelled a **host-generated
proof** is not a device capture; its caption says so and hardware recapture is
pending. The apps are batch-first and run from SD or USB; metadata comes from
each app's `APPINFO`.

The feature matrix in [`readme.md`](../readme.md) lists which surfaces each app
exercises. Every v1.6.0 app is richer: all **33 updated `.bat` apps** emit an
additive `[M-RICH-*]` marker block that exercises the new batch/GFX/TUI/modal/
webapp surfaces, and all pre-existing `[M-*]` markers are preserved. The host
proof is `python tools/host_apps_gate.py --check` (33/33 apps, 363 host
assertions PASS, 40 hardware surfaces honestly SKIP, 7 red canaries); the
markers are not necessarily visible in the older gallery PNGs below, whose
**hardware recapture is pending**. The new transcript lines mean
`tools/appdiff/baseline/*.transcript.txt` goldens must be regenerated on a
board before hardware transcript parity can be green.

## Proof origin at a glance

| Capture | Origin |
|---------|--------|
| `apps/gfxtool.png` | **Host-generated proof** from the real gfx core (`tools/make_gfx_proof.py`); not a device capture |
| `web-features.png`, `webapp-forms.png` | **Host-generated proofs** from the real DOM/CSS/canvas render (`tools/make_webapp_proof.py`); not device captures |
| Every other `apps/*.png` | Existing device capture from an earlier campaign; hardware recapture for the v1.6 markers is pending |
| `proof/web.png`, `proof/wikipedia_live.png` | Live device captures staged by `s21_wikipedia` (see `docs/assets/README.md`) |

The checked, labelled proof set is [`proof-index.html`](assets/proof-index.html).

## ADVENTURE

![ADVENTURE](assets/apps/adventure.png)

`[M-RICH-ADV]` (`-K`, `-F`, `-S`): `str /rev|/count|/pad`, `map /set` +
`for /K`, and `func`/`return`/`%RESULT%` in the library room.

*Screenshot: `docs/assets/apps/adventure.png` (device capture; v1.6 recapture pending)*

## Bounce Demo

![Bounce Demo](assets/apps/bounce.png)

`[M-RICH-BOUNCE]` (`-F`): gfx pixel-canvas bouncing ball on the app drive, with
batch math in a killable frame loop and the v1.6 result marker.

*Screenshot: `docs/assets/apps/bounce.png` (device capture; v1.6 recapture pending)*

## Chat

![Chat](assets/apps/chat.png)

`[M-RICH-CHAT]` (`-K`, `-F`, `-S`): board-to-board chat over the local link
(SoftAP + HTTP) with `str /count|/pad`, `map /set|/incr` + `for /K` and
`func`/`return`; `draw run` paints the marker.

*Screenshot: `docs/assets/apps/chat.png` (device capture; v1.6 recapture pending)*

## P4 Companion

![P4 Companion](assets/apps/companion.png)

`[M-RICH-COMPANION]` (`-K`, `-F`, `-S`) plus `[M-RICH-LIB-COMPANION]`: the
pure-batch system helper's dashboard, files, network, games and settings menu,
now with `map` + `for /K`, `str /pad` and `func`/`return` tours; `draw run`
shows the marker on the menu.

*Screenshot: `docs/assets/apps/companion.png` (device capture; v1.6 recapture pending)*

## P4 Diagnostics

![P4 Diagnostics](assets/apps/diag.png)

`[M-RICH-DIAG]` (`-K`, `-F`, `-S`): system health, capability and frame-rate
diagnostics console; mem shows internal heap + PSRAM, and the new batch
surfaces run through the SAME interpreter.

*Screenshot: `docs/assets/apps/diag.png` (device capture; v1.6 recapture pending)*

## Elite Trader

![Elite Trader](assets/apps/elite.png)

`[M-RICH-ELITE]` (`-K`, `-F`, `-S`): turn-based space trading with market
previews via `map /set|/values` + `for /K`, `func`/`return` cargo math, saves
on the app drive and a `draw run` marker.

*Screenshot: `docs/assets/apps/elite.png` (device capture; v1.6 recapture pending)*

## FILES-NOTES

![FILES-NOTES](assets/apps/files-notes.png)

`[M-RICH-FILES]` and `[M-RICH-NOTE]` (`-K`, `-F`, `-S` each): file tools and
Zettelkasten notes share the same additive batch tour.

*Screenshot: `docs/assets/apps/files-notes.png` (device capture; v1.6 recapture pending)*

## File Tools

![File Tools](assets/apps/files.png)

`[M-RICH-FILES]` (`-K`, `-F`, `-S`): browse, view, search, trash, clipboard and
note pad on `%APPDRIVE%`, with a `draw run` marker.

*Screenshot: `docs/assets/apps/files.png` (device capture; v1.6 recapture pending)*

## Fun / Demos

![Fun / Demos](assets/apps/fun.png)

`[M-RICH-FUN]` (`-K`, `-F`, `-S`): melody, RGB effects, number guessing, calc,
Snake and Elite Trader, plus a scored demo-menu tour.

*Screenshot: `docs/assets/apps/fun.png` (device capture; v1.6 recapture pending)*

## Gfx Toolkit

![Gfx Toolkit](assets/apps/gfxtool.png)

`[M-RICH-GFXTOOL]` (`-F`, `-ATLAS`): raster toolkit demo with triangle/ellipse/
polygon/flood-fill/spans, scaled on-canvas text and the v1.6 AA/gradient/
composite/atlas verbs.

*Host-generated proof (hardware recapture pending): `docs/assets/apps/gfxtool.png`*

This one image is not a device capture: it is rendered on the host from the
real gfx core by [`tools/make_gfx_proof.py`](../tools/make_gfx_proof.py), with
the text drawn after the sprite stamps as the fixed `GFXTOOL.BAT` does.

## Mood Dashboard

![Mood Dashboard](assets/apps/mood.png)

`[M-RICH-MOOD]` (`-K`, `-F`, `-S`) and `[M-RICH-MOODSVC]`: live board mood from
free heap, RGB/audio bands plus mem (heap + PSRAM) and sysinfo snapshots, with
a headless sampler tour.

*Screenshot: `docs/assets/apps/mood.png` (device capture; v1.6 recapture pending)*

## NET-MSG

![NET-MSG](assets/apps/net-msg.png)

`[M-RICH-NET]` (`-K`, `-F`, `-S`): the MQTT message view's companion tool tour
over the same real interpreter.

*Screenshot: `docs/assets/apps/net-msg.png` (device capture; v1.6 recapture pending)*

## Network Tools

![Network Tools](assets/apps/net.png)

`[M-RICH-NET]` (`-K`, `-F`, `-S`): Wi-Fi status, connect, ping, DNS, fetch and
the MQTT messaging view, with a previewed tool menu.

*Screenshot: `docs/assets/apps/net.png` (device capture; v1.6 recapture pending)*

## Zettelkasten Notes

![Zettelkasten Notes](assets/apps/notes.png)

`[M-RICH-NOTE]` (`-K`, `-F`, `-S`): linked notes on the db record store with
categories, search and export/import (the db store is SD-only).

*Screenshot: `docs/assets/apps/notes.png` (device capture; v1.6 recapture pending)*

## PalmTop Demos

![PalmTop Demos](assets/apps/palmtop.png)

`[M-RICH-PALM]` (`-K`, `-F`, `-S`): timer/calc/csv/crypt/bind/tcpterm on the
app drive, with a `func`/`return` conversion and `draw run` marker.

*Screenshot: `docs/assets/apps/palmtop.png` (device capture; v1.6 recapture pending)*

## Pictures

![Pictures](assets/apps/pics.png)

`[M-RICH-PICS]`: BMP image viewer, TUI draw and gfx canvas blit on the app
drive; the v1.6 canvas tour adds a gradient backdrop, a translucent composite
and an atlas stamp (needs `PHOTO.BMP`).

*Screenshot: `docs/assets/apps/pics.png` (device capture; v1.6 recapture pending)*

## Plotter

![Plotter](assets/apps/plot.png)

`[M-RICH-PLOT]` (`-F`): scientific graphs (sin/cos on a world-coordinate
window with axes and grid) plus the v1.6 result marker.

*Screenshot: `docs/assets/apps/plot.png` (device capture; v1.6 recapture pending)*

## Preferences

![Preferences](assets/apps/set.png)

`[M-RICH-SET]` (`-K`, `-F`, `-S`): brightness, volume, rotate, theme, fonts,
input and security (CONFIG.SYS) with a settings-menu batch tour.

*Screenshot: `docs/assets/apps/set.png` (device capture; v1.6 recapture pending)*

## Snake

![Snake](assets/apps/snake.png)

`[M-RICH-SNAKE]` (`-K`, `-F`, `-S`): TUI snake with fixed body slots, timeout
steering and one flush per frame, plus the v1.6 language tour.

*Screenshot: `docs/assets/apps/snake.png` (device capture; v1.6 recapture pending)*

## Live System

![Live System](assets/apps/sys.png)

`[M-RICH-SYS]` (`-K`, `-F`, `-S`): live dashboard of memory (heap + PSRAM),
tasks, services and packages, with the batch tour running through the same
interpreter.

*Screenshot: `docs/assets/apps/sys.png` (device capture; v1.6 recapture pending)*

## Task Manager

![Task Manager](assets/apps/taskmgr.png)

`[M-RICH-TMGR]` (`-K`, `-F`, `-S`, `-MODAL`): batch task manager for pool jobs,
priorities, services and events, with the v1.6 modal-surface marker.

*Screenshot: `docs/assets/apps/taskmgr.png` (device capture; v1.6 recapture pending)*

## Commander

![Commander](assets/apps/tcmd.png)

`[M-RICH-TCMD]` (`-K`, `-F`, `-S`): dual-pane file commander with browse
selectors plus copy/move/delete/mkdir/rename/view/edit on the app drive.

*Screenshot: `docs/assets/apps/tcmd.png` (device capture; v1.6 recapture pending)*

## Browser

![Browser](assets/apps/web.png)

`[M-RICH-WEB]`: renders a website from `<drive>/WEB/` with the html page verb
and dispatches the same button event through the `webapp` facade (`event` +
`tick`).

*Screenshot: `docs/assets/apps/web.png` (device capture; v1.6 recapture pending)*

## Webapp Demo

![HTML5 feature render](assets/web-features.png)

`[M-RICH-WEBAPP]` (`-DONE`, `-FAIL`): the batch-first reference app for the
`webapp` facade. It opens the local `WEB/` page, dumps colour runs and shows
CSS/JS through the shared browser engine. The shipped richer page
`apps/webapp/RICH.HTML` is deployed via `apps/app_manifest.py`.

*Host-generated proof (hardware recapture pending): `docs/assets/web-features.png`
and the form-detail crop `docs/assets/webapp-forms.png`, produced by
[`tools/make_webapp_proof.py`](../tools/make_webapp_proof.py) from the real
host render of `tools/host/fixtures/webapp/features.html`. No device capture is
claimed for this board-free release candidate.*

## Other enriched apps (no gallery capture)

These apps also carry additive `[M-RICH-*]` markers and are covered by
`tools/host_apps_gate.py --check`; they have no dedicated gallery PNG yet.

| App | Markers | What it shows |
|-----|---------|---------------|
| `apps/controlflow/CONTROL.BAT` | `[M-RICH-CONTROL]` (`-F`, `-K`, `-S`, `-T`) | `func`/`return`, newly wired `trap /on errorlevel`, `map`/`for /K`, `str` |
| `apps/hybrid/HYBRID.BAT` | `[M-RICH-HYBRID]` (`-NATIVE`, `-SKIP`) | hybrid shim with the one signed native payload path |
| `apps/pkgtest/PKGTEST.BAT` | `[M-RICH-PKGTEST]` (`-K`, `-F`, `-S`) | manifest tour via `map` + `for /K` and `func` |
| `apps/uitest/UITEST.BAT` | `[M-RICH-UITEST]` | UI/touch diagnostics with the new markers |
| `apps/writer/WRITER.BAT` | `[M-RICH-WRITER]` (`-K`, `-S`, `-F`, `-FINAL`) | writerdeck word/length stats via `str /count|/pad`, `map /incr` and `func` |
| `apps/APPLIB.BAT` | `[M-RICH-LIB]` (`-K`, `-F`, `-S`) | shared batch routine library language showcase |
| `apps/companion/AGENDA.BAT` | `[M-RICH-AGENDA]` (`-K`, `-F`, `-S`) | headless str/map/forK/func tour |
| `apps/companion/MEMLOG.BAT` | `[M-RICH-MEMLOG]` (`-K`, `-F`, `-S`) | headless tour alongside the CSV tick |
| `apps/companion/SVC.BAT` | `[M-RICH-SVC]` (`-K`, `-F`, `-S`) | resident-service tour |
| `apps/companion/LIB.BAT` | `[M-RICH-LIB-COMPANION]` (`-K`, `-F`, `-S`) | companion library routine tour |
