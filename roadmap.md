# P4MiniShell Roadmap

P4MiniShell is a **software framework for building palmtops, PDAs,
writerdecks, and similar handhelds on the ESP32-P4 + ESP32-C6**. This document
records how the framework evolved and where it is going.

The application model is **batch-first**: an on-SD `.bat` file is an app, and a
native C SDK (`applib`) supplies the polished surfaces batch cannot draw. Every
app launches through a `.bat` shim; the frozen contract is [`ABI.md`](ABI.md).
See [`readme.md`](readme.md) for the feature tour and
[`documentation.md`](documentation.md) for the architecture.

---

## Part 1 — How we got here

Grouped by milestone. Full per-version detail is in
[`changelog.md`](changelog.md).

### 1. Foundation (v0.1 – v0.13)

Shell on LVGL with coloured transcript, prompt, history, OSK, UART bridge,
status header. Window/display/keyboard managers, rotation, brightness, central
config. First DOS file verbs, ANSI colour, unit-test project.

### 2. Architecture and shell core (v0.14 – v0.17)

Split monolithic command code into modules; established one-way dependency graph
and `shell_command_ops_t` / `batch_command_ops_t` registration tables. Restored
extended DOS commands (`attrib`, `label`, `xcopy`, wildcards).

### 3. DOS language completeness (v0.18 – v0.21)

Batch control flow: `if`/`goto`/`shift`/`pause`/`choice`/`setlocal`/`exit /b`,
runtime prompt template, keypress-wait. Pipes, `<` redirection, quoting (`" "`,
`' '`, `^`), chaining (`&`, `&&`, `||`), `set /a`, `set /p`, line
continuation. Volume management, full `dir` switches, attribute preservation,
free-space guardrails.

### 4. Hardening, devices, and the editor (v0.22 – v0.24)

Semantic colour palette; memory- and crash-hardening. Device subsystems: recycle
bin, `findstr`/`comp`/`xcopy /S`, task introspection (`ps`/`tasks`/`top`),
`config` settings, boot scripting (`CONFIG.SYS`/`AUTOEXEC.BAT`), idle
display-off, audio, clipboard, tab completion, SD-backed history, HTTP server,
known Wi-Fi networks, WS2812 LED, peripheral toolkit (PWM/ADC/I2C/SPI). The
`edit` text editor (touch-first, undo, find/replace, Save-As). The 2026-09-18
writerdeck audit later closed all 16 parity/robustness gaps (ordered lists, GFM
tables, images/task lists, inline nesting, flanking/escape rules, HTML
truncation, export handling, print paginator, template validation,
focus+preview composition, spell tokenizer, `WRITER.BAT` end-to-end, provision
scripts, and corrected docs).

### 5. Apps, data, and connectivity (v0.30 – v0.33)

Serial file transfer (`receive`/`send`) and screenshot framing. Shared-SDMMC
reliability; ESP-Hosted 3.0.6. Batch-first app model: native ABI, `applib`,
PATH/`APPS` discovery with `launch`/`APPINFO`, batch process model
(`proc`/`%ERRORLEVEL%`), shared libraries (`call file::routine`), native modals
(`dialog`/`list`/`ask`). Palm-OS `db` store, `alarm`/`cal`.

### 6. Palmtop parity (v0.34
### 11. Extended ASCII / CP437 support (v1.3.1)

- **Full CP437 8x8 font**: `gfx_font.c` expanded from printable ASCII (0x20-0x7E, 96 glyphs) to full CP437 range 0x00-0xFF (256 glyphs) via `tools/gen_gfx_font.py` using unscii-8 TTF.
- **`gfx_surface_text`**: Renders box-drawing (U+2500-U+257F), accented letters, and symbols (0x80-0xFF) when `P4_CONFIG_GFX_FONT_EXTENDED=1` (default). Control chars (0x00-0x1F, 0x7F) render empty; ASCII 0x20-0x7E always render.
- **Editor USB input**: `editor_view_handle_usb_key` accepts extended ASCII 0x20-0xFF.
- **`hexview` ASCII column**: Displays extended chars (0x80-0xFF) via `strutil_format_hex_row` when extended font enabled.
- **Config**: `P4_CONFIG_GFX_FONT_EXTENDED` (default 1) toggles extended rendering; font array always 256 entries but 0x00-0x1F/0x80-0xFF empty when disabled.
- **Unit tests**: `test_gfx_font_extended`, `test_cas_asc_extended_ascii`, `test_cas_chr_extended_ascii`.
- **Documentation**: Updated `command.md` (gfx text, hexview), `editor.md` (Symbols page), `readme.md`, `changelog.md`, `ai-context.md`. – v0.38)

FX-870P/VX-4 `calc` (math/string/financial, base conversions). TUI cell buffer
+ `draw` verbs, `gfx` RGB565 canvas with sprites/BMP, `plot` graph layer. App
ecosystem: `crc32`/`asset` manifests, `pkg` bundles, UI themes, font registry
with SD TTFs/CJK, markdown rendering, reference apps. Data: `csv` grid +
`=EXPR`, `export`/`import`, `archive`, `crypt`, `ini`/`appconfig`/`temp`.
Experience: header, notifications, network clock, background jobs, macro
recorder, F-key binds, `tcpterm`, `usb userial`, boot splash, device security.
Reliability: transcript batching, boot RAM relief, SD/C6 bring-up, MIPI-DSI
fix, Wi-Fi tuning, host regression runner.

### 7. First public release (v1.0.0)

Relicensed to MIT; SPDX headers, third-party license table. Rewrote docs for
newcomers: readme, roadmap, tutorials, `ai-context.md`, reset `bugs.md`.
Firmware unchanged from v0.38.5.

### 8. Two-board era (v1.2.0)

M5Stack Tab5 as first-class `-DP4_BOARD` target: 1280x720 MIPI-DSI with
runtime panel auto-detect, ES8388 audio, RX8130CE RTC, BMI270 IMU, INA226 pack
gauge, SC202CS camera, Tab5Keyboard, hosted C6 Wi-Fi+BLE at 10 MHz, PMIC
shutdown. See [`PORTING.md`](PORTING.md). Hardware abstraction: zero-cost
`board_caps/` helpers, compile-time board/sdkconfig guards, board-driven
backlight, board-neutral logs.

### 9. Batch language era (v1.2.x)

`for /L`, `for /A`, `for /D`, `for /R`, `while`, `switch`, single-line
`if (...) else (...)`. `NAME[i]` arrays, `%VAR:~start[,len]%` substrings,
`%VAR:old=new%` replacement, `!VAR!` delayed expansion. `calc` string functions,
`gfx blitmany` with `/s:` upscale and `/r:` rotation. Single-file
[`batch.md`](batch.md) spec.

### 10. App model, packaging, and trust (v1.2.x)

Every app launches through a `*.bat` shim — pure batch, hybrid, or native-linked.
Frozen in [`ABI.md`](ABI.md). Signed `PKGS` manifests (ECDSA P-256); host
signer `tools/pkg_sign.py`. Declarative screens (`.FRM`) and flows (`.FLOW`)
over `dialog`/`list`/`ask`/`form`/`menu` verbs.

### 11. Search everywhere (v1.2.x)

`gfind` with opt-in `/files` text scope over db + alarm + files. One shared
storage search core (`storage_walk_files` + `storage_scan_file_lines`), no
on-disk index — always live.

---

## Part 2 — Where we are going

Realistic directions grouped by device class. Priorities are suggestions, not
commitments.

### A. Framework and portability

| Feature | Why | Notes |
|---------|-----|-------|
| Additional board / panel profiles | More hardware | EK79007 and LT8912B vendored; `boards/<name>/` profile + BSP wiring remains ([`PORTING.md`](PORTING.md)) |
| Build-tree hygiene | Release-ready tree | Done: `build-tab5/` + `test/build-tab5/` untracked from git (were committed before the ignore rules); `git ls-files` shows no build output. See [`release.md`](release.md) §0 |
| Single capability-query style | One way to ask | Done: runtime code queries `board_caps_*` (battery ADC/INA226, RTC BSP-I2C, keyboard RGB routing); preprocessor `#if BOARD_CFG` remains only where required (macro definitions, includes, table entries, value defaults). Both boards + `test/` build clean |
| CI board auto-discovery | Scaling | Done: `build.yml` discovers `boards/*/` for the matrix and completeness lint — adding a profile needs no workflow edit |
| Per-board release artifacts | Shippable images | Done: `make_release.py bins` verified for both boards (`p4minishell-<board>.bin` merge; `release/` stays ignored) |
| Two-board stability gate | Exit criterion | Open: `bugs.md` F27 (transient Tab5 crash under long mixed sweeps) must close with three clean full sweeps on both boards + a `dogfood` soak before this chapter is declared complete |

### B. PDA / PIM

| Feature | Why | Notes |
|---------|-----|-------|
| Contacts / tasks apps | Complete the PIM story | Build on `db` store; batch apps with modal surfaces |
| Agenda integration | One place for time | Merge `alarm`/`cal` events, tasks, documents into today view |
| RSS / plain-text reader | Classic PDA use | Core landed in `components/rss`: `rss_parse()` (RSS 2.0 / Atom 1.0 / JSON Feed) and `rss_fetch()` (conditional GET + Basic/Bearer auth). Subscription store, polling scheduler, and the reading surface remain. |
| Secrets manager | Passwords on the go | `crypt` core + `db` secret fields, behind passcode lock |
| **EPUB e-book reader** | Classic PDA use | `epub` verb family: open/read/next/prev/goto/toc/extract; OPF/NCX/nav parsing; HTML-to-ANSI chapter rendering; spine/TOC navigation; resource extraction |

### C. Palmtop / programmable

| Feature | Why | Notes |
|---------|-----|-------|
| Spreadsheet app | The other defining feature | Build on `csv` + `calc`; add interactive grid modal and recalculation engine |
| Calculator UI app | Everyday use | Modal keypad over `calc` (HP-12C / FX-870P layouts) |

### D. Platform, power, and security

| Feature | Why | Notes |
|---------|-----|-------|
| Deep low-power / AON | All-day battery | Started: light-sleep wake from the wired touch/keyboard interrupts, deep-sleep RTC-IO GPIO wake, camera rail off. Still open: power-domain tuning, retention/AON memory, peripheral runtime PM, RTC-alarm wake (RX8130 INT not routed), and touch wake on GT911 Tab5 units / the JC1060P470 reference (no interrupt wired) |

### E. Connectivity and services

| Feature | Why | Notes |
|---------|-----|-------|
| HTTP-client CardDAV sync | Real PIM | Done: `pim net pull\|push\|sync` against a collection URL (PROPFIND/GET/PUT, Basic auth, newer-wins merge, no tombstones v1). Live-server E2E pending: needs board↔server LAN (bench LANs unrouted/filtered; F37). Bench tool ready: `tools/radicale_fixture.py` (local Radicale + 3-contact fixture, self-tested 207) |
| Event service (MQTT + outbox) | Live apps | Done: one `netsvc` task owns a persistent MQTT 3.1.1 session (plaintext LAN or TLS via `net tls on`, BYO broker); `net` verbs + `/onmsg` hook for batch, `applib_msg.h` for native apps; SD outbox with oldest-first flush; `$pim/...` topics merge newer-wins. Live TLS handshake still pending: no TLS endpoint reachable from the bench (both LANs HTTP-only; F37). Local amqtt bench recipe: self-signed CA via bench PKI + `certs add`, broker on PC, board on a routed LAN |
| MQTTS / WSS transport | Encrypted events | Done for MQTTS (same driver over `esp_tls`, CA policy shared with `httpget`); WSS remains open |

### F. P4Sync — USB desktop sync (ActiveSync spirit, harness-proven first)

North star: a future `P4Sync` desktop app manages boards over USB-Serial/JTAG
the way Palm Desktop managed handhelds — one session covers files + PIM
(`db`/`alarm`) + apps (`pkg`/`asset`) + screenshots. Until that app exists,
the harness is the prototype: every needed protocol feature is proven through
`tools/p4test/` suites and the `tools/p4sync/` host client, never through a
second firmware stack. All firmware additions are thin shells over the
existing `storage` / `db` / `alarm` / `pim` / `pkg` / `asset` / serial-engine
cores (one dispatcher, one framing writer, one CRC-32, one search core — see
[`ai-context.md`](ai-context.md) Hard Rules). Batch-first launch
([`ABI.md`](ABI.md)), no SD code execution, the device-lock secret gate
([`SECURITY.md`](SECURITY.md)), and gated destructive actions stay unchanged.

| Phase | Task | Why (P4Sync need) | Harness proof |
|-------|------|-------------------|---------------|
| 0 | USB-serial protocol inventory | Freeze what exists before extending it | `receive`/`send` (SDFX), `screenshot` (BMPX), `pim get/put` (PIMX) magics, markers, limits documented in `command.md` / `harness.md` |
| 0 | P4Sync gap list | Name delta/inventory/progress/cancel/lock-aware gaps explicitly | Gaps tracked here, not as silent firmware drift |
| 1 | `s15_p4sync` conformance suite | Prove full-image sync without new firmware | `tools/suites/s15_p4sync.py`: `sync status` handshake, file push/pull, PIM round-trip, `pkg`/`asset`, screenshot geometry on both boards |
| 1 | Framing robustness matrix | Host scans for magic past log text; CRC/timeout paths covered | Short-payload, CRC-mismatch, and backpressure cases through `p4test.session.read_binary_frame` |
| 2 | `sync status` handshake verb | One capability/lock/limits snapshot for the host | Thin read-only shell over `storage` + lock state; `s15_p4sync` asserts every `sync.*` line |
| 2 | `P4_CONFIG_SYNC_*` tunables | No hardcoded sync limits | Header + YAML mirror (`p4minishell_config.h`, `p4minishell_config.yaml`) |
| 3 | `tools/p4sync/` host prototype | Reusable sync core for the future desktop app | Python client reusing `p4test.device/sdbridge/screenshot` + `pim_sync` merge; no raw `serial.Serial`, DTR-safe open only |
| 3 | Full-image CLI run | PIM + files + apps + screenshot in one session | `tools/p4sync full-image` (handshake + PIM mirror sync + file provisioning + app inventory + shot + `image.json`), HW-verified on COM3 |
| 4 | Two-board verification ladder | No regressions | Firmware + `test/` zero warnings, unit suite green, `p4test_run` + `regression.py` PASS, `dogfood` soak, main firmware reflashed |

### Not planned

| Item | Why not |
|------|---------|
| Second application-runtime model | Batch + hybrid/native is the contract ([`ABI.md`](ABI.md)) |
| SD/flash code execution | C apps linked into firmware, reached through `.bat` shim |
| Second display writer / windowing model | Transcript/TUI/modal/GFX surfaces are the contract |
| Native Preferences GUI | Settings stay in `CONFIG.SYS` + `config`; `SET.BAT` is the touch UI |
| Rich Text Format (RTF) | A large spec (font/colour tables, stylesheets, fields, `\pict`); a lossy subset would mislead. Markdown + HTML reading cover the on-device document needs |

---

## Open gaps (intentionally deferred)

| Gap | Status |
|-----|--------|
| Touch wake from sleep | Wired on Tab5 ST7123/ST7121 units (GPIO23) and armed by `sleep`; unavailable on GT911 Tab5 units (pin strapped low by the BSP) and the JC1060P470 reference (no interrupt wired) |
| Camera live preview | BMP capture works (`camera init` + `camera snap`); streaming frames into LVGL canvas not implemented |
| RTC-alarm wake | RX8130CE `INT` pin not routed on the Tab5 (no INT macro/row) and no alarm-IRQ driver; deep sleep also only wakes from GPIO0..GPIO15 |
| USB MSC with a physical stick attached | Untested: all hardware checks so far used the no-device error paths (`usb: MSC drive N not connected`); automount-on-arrival and real file I/O on `u:`..`x:` still need a stick on the bench |
| `xcopy` / `findstr` file operands on USB | Done in code (volume sessions + VFS enumerators, R-only attr mapping, SD paths re-verified); live stick I/O still needs a USB stick on the bench (see row above) |
| Pixel-exact transcript link mapping | Tap hit-testing is font-metric approximate (wrapped rows / trimmed scrollback drift); safe by tap-confirm, but only the first 32 regions are tappable. A span-tagged transcript model would make every row exact |

---

## How to propose work

1. Check this file and [`bugs.md`](bugs.md) to avoid duplicate effort.
2. Read the owning module's rules in [`ai-context.md`](ai-context.md), the
   integration guide in [`SDK.md`](SDK.md), and the app contract in
   [`ABI.md`](ABI.md).
3. Follow the change and verification workflow in [`ai-context.md`](ai-context.md):
   build both projects clean, verify on hardware, update the docs and changelog.
