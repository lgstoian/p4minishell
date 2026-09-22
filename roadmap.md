# P4MiniShell Roadmap

P4MiniShell is a **software framework for building palmtops, PDAs,
writerdecks, and similar handhelds on the ESP32-P4 + ESP32-C6**. This document
records how the framework evolved and where it is going.

The application model is deliberately **batch-first**: an on-SD `.bat` file is
an app, and a native C SDK (`applib`) supplies the polished surfaces batch
cannot draw. Every app — pure batch, hybrid, or native-linked — launches
through a `.bat` shim; the frozen contract is [`ABI.md`](ABI.md). See
[`readme.md`](readme.md) for the feature tour and
[`documentation.md`](documentation.md) for the architecture.

---

## Part 1 — How we got here

Grouped by milestone rather than by version. Full per-version detail is in
[`changelog.md`](changelog.md).

### 1. Foundation (v0.1 – v0.13)

- Embedded shell on LVGL with a coloured transcript, prompt input line, command
  history, on-screen keyboard, UART console bridge, and a fixed status header.
- Window/display/keyboard managers, rotation, brightness, and a central config
  header.
- First DOS-style file verbs, ANSI colour processing, and the unit-test project.

### 2. Architecture and shell core (v0.14 – v0.17)

- Split the monolithic command code into modules; established the one-way
  dependency graph and the `shell_command_ops_t` / `batch_command_ops_t`
  registration tables that invert the only upward dependencies.
- Restored the extended DOS command set (`attrib`, `label`, `xcopy`, wildcard
  matching).

### 3. DOS language completeness (v0.18 – v0.21)

- Batch control flow: `if`/`goto`/`shift`/`pause`/`choice`/`setlocal`/`exit /b`,
  a runtime prompt template, and a real keypress-wait facility.
- Pipes and `<` input redirection; quoting (`" "`, `' '`, `^`), command
  chaining (`&`, `&&`, `||`), `set /a` integer arithmetic, `set /p`, and line
  continuation.
- Volume management (`chkdsk`/`format`), the full `dir` switch set, attribute
  preservation, and free-space guardrails.

### 4. Hardening, devices, and the editor (v0.22 – v0.24)

- Semantic colour palette and print helpers; memory- and crash-hardening sweeps.
- Device subsystems: recycle bin, `findstr`/`comp`/`xcopy /S`, task
  introspection (`ps`/`tasks`/`top` + CPU graph), `config` settings, boot
  scripting (`CONFIG.SYS`/`AUTOEXEC.BAT`), idle display-off, audio, clipboard,
  tab completion, SD-backed history, HTTP file server, known Wi-Fi networks,
  the WS2812 status LED, and the peripheral toolkit (PWM/ADC/I2C/SPI).
- The `edit` text editor (touch-first, with undo, find/replace, and Save-As).

### 5. Apps, data, and connectivity (v0.30 – v0.33)

- Serial file transfer (`receive`/`send`) and screenshot framing.
- Shared-SDMMC reliability fixes; ESP-Hosted 3.0.6.
- Batch-first app model: native-app ABI, `applib`, PATH/`APPS` discovery with
  `launch` and `APPINFO`, the batch process model (`proc`/`%ERRORLEVEL%`),
  shared batch libraries (`call file::routine`), and native modal surfaces
  (`dialog`/`list`/`ask`).
- Palm-OS-style `db` record store and SD-persisted `alarm`/`cal`.

### 6. Palmtop parity (v0.34 – v0.38)

- FX-870P/VX-4 style `calc` (math + string + financial/date functions, angle
  modes, base conversions) and the BASIC-to-batch mapping.
- Display/graphics: the 80x25 TUI cell buffer and `draw` verbs, the `gfx`
  RGB565 canvas with sprites and BMP support, and the `plot` graph layer.
- App ecosystem: `crc32`/`asset` manifests, `pkg` install bundles, UI themes,
  the font registry with SD TTFs and CJK fallback, markdown rendering, and
  reference apps (`companion`, `tcmd`, `snake`, `elite`, `adventure`, `notes`,
  `mood`, `gfxdemo`, `pics`).
- Data/state: `csv` grid + `=EXPR`, `export`/`import` interchange, `archive`
  backups, `crypt` encryption, `ini`/`appconfig`/`temp`.
- Experience: responsive colour-coded header, notification queue, network clock
  with auto timezone, background jobs (`start`/`taskkill`), macro recorder,
  F-key binds, `tcpterm`, `usb userial`, boot splash, and device security.
- Reliability campaign: transcript batching, boot internal-RAM relief, SD/C6
  bring-up ordering, the MIPI-DSI "BSOD" fix, Wi-Fi throughput tuning, and the
  host regression runner.

### 7. First public release (v1.0.0)

- Relicensed to **MIT**; added SPDX headers across the project and an
  authoritative third-party license table.
- Rewrote the documentation for newcomers: a new readme, a grouped roadmap,
  three tutorials (getting started, batch apps, native apps), an agent-focused
  `ai-context.md`, and a reset `bugs.md`.
- Firmware behaviour is unchanged from v0.38.5; this release is about
  openness, documentation, and a clean baseline for the next campaign.

### 8. Two-board era + hardware abstraction (v1.2.0)

- **M5Stack Tab5 is a first-class `-DP4_BOARD` target** alongside the
  JC1060P470C reference: 1280x720 MIPI-DSI with runtime ILI9881C / ST7123 /
  ST7121 auto-detect, ES8388 audio, RX8130CE RTC, BMI270 IMU (tilt
  auto-rotate), INA226 pack gauge with charging, SC202CS MIPI-CSI camera
  (BMP stills), the Tab5Keyboard (input + two independent RGB LEDs), hosted C6
  Wi-Fi + BLE at 10 MHz, and PMIC `shutdown`. See [`PORTING.md`](PORTING.md) §6.
- Hardware abstraction hardened without new layers: a zero-cost
  `components/board_caps/` helper set, compile-time `_Static_assert` guards
  that board/sdkconfig pins agree, a board-driven backlight LEDC timer, and
  board-neutral logs/labels.

### 9. Batch language era (v1.2.x)

- Loops and control flow: `for /L` (numeric), `for /A` (array iteration),
  `for /D` and `for /R` (directories/trees), `while` condition loops,
  `switch` string dispatch, and single-line `if (…) else (…)` groups.
- Data: `NAME[i]` indexed arrays, `%VAR:~start[,len]%` substrings,
  `%VAR:old=new%` replacement, and `!VAR!` delayed expansion via
  `setlocal enabledelayedexpansion`.
- Calculator + graphics: `calc` string functions (`UPPER$ LOWER$ TRIM$ INSTR
  REPLACE$`) and `gfx blitmany` with `/s:` upscale and `/r:` quarter-turn
  rotation.
- Authoring: the single-file [`batch.md`](batch.md) spec.

### 10. App model, packaging, and trust (v1.2.x)

- **Every app launches through a `*.bat` shim** — pure batch, hybrid (a shim
  driving a linked-in C entry), or native-linked for dev/test. Documented and
  frozen in [`ABI.md`](ABI.md).
- **Signed `PKGS` manifests**: optional ECDSA P-256 `SIGN=` line verified
  against a trusted public key in NVS (`pkg key …`); bad signatures always
  refuse, and `pkg install /signed` enforces signing. Host signer
  `tools/pkg_sign.py`, wired into `apps/push_pkgs.py --sign`.
- **Declarative screens and flows**: a `.FRM` file describes a
  `dialog`/`list`/`ask`/`form`/`menu` screen rendered through the existing
  verbs (`screen run|info`), and a `.FLOW` file declares a multi-screen
  navigation graph over those nodes (`screen flow`) — routing is the pure
  `screen_flow_select()`, so apps neither hand-roll every screen nor a `goto`
  web (see `batch.md` §13 / §13.1).

### 11. Search everywhere (v1.2.x)

- `gfind` grew an opt-in `/files` text-file scope on top of its existing db +
  alarm search: one query over everything a PDA would hold. It reuses the ONE
  shared storage search core (`storage_walk_files` + `storage_scan_file_lines`,
  the same one `findstr /S` uses) and the same matcher, with `/root:`, `/ext:`,
  `/hidden`, `/filesonly`, and `/nofiles`.
- **There is no on-disk search index and none is planned**: search is always
  live over the stores and the files, so it can never go stale. (An index would
  have duplicated the existing scanners — see the no-duplication hard rule in
  [`ai-context.md`](ai-context.md).)

---

## Part 2 — Where we are going

Realistic, useful directions grouped by the kind of device the framework is
meant to build. Priorities are suggestions, not commitments.

### A. Framework and portability

| Feature | Why | Notes |
|---------|-----|-------|
| **Additional board / panel profiles** | More hardware | The EK79007 and LT8912B display drivers are vendored; a `boards/<name>/` profile + BSP wiring is the remaining work (see [`PORTING.md`](PORTING.md)). |

### B. Writerdeck

#### Writerdeck audit — closed (code audit 2026-09-18, fixes landed after)

The `Full-page HTML wrapper` item is done (`markdown_render_html_page()`
ships the `<!DOCTYPE html>` reader page). The 16 parity/robustness gaps it
exposed are all fixed in the current tree: ordered lists, GFM tables,
images/task lists, inline nesting, flanking/escape rules, the HTML
truncation contract, export truncation/ERRORLEVEL handling, export
titles/format selection, the print paginator, template-seeding validation,
focus+preview composition, loud viewer/preview/export caps, the UTF-8-aware
spell tokenizer (length-unlimited lookups, wrap+spell composed), a
`WRITER.BAT` that seeds/renders/exports/verifies end to end, discoverable
provisioning (`push_fonts.py`, `apps/push_templates.py`,
`apps/push_dicts.py` + `apps/dicts/en.words`), and corrected writerdeck
docs (limits, caps, gates, troubleshooting). No open writerdeck items
remain.

### C. PDA / PIM

| Feature | Why | Notes |
|---------|-----|-------|
| **Contacts / tasks apps** | Complete the PIM story | Build on the existing `db` store; ship as batch apps with modal surfaces |
| **Agenda integration** | One place for time | Merge `alarm`/`cal` events, tasks, and documents into a today view |
| **Sync (WebDAV / CalDAV-lite)** | Back up and share | HTTP client + VFS bridge; reuse the `export`/`import` interchange formats |
| **RSS / plain-text reader** | The classic PDA use | `httpget` + a reading view with the font/markdown stack |
| **Secrets manager** | Passwords on the go | Uses the existing `crypt` core and `db` secret fields, behind the passcode lock (crypt reliable on both boards now — bugs.md F23 fixed in v1.2.1) |

### D. Palmtop / programmable

| Feature | Why | Notes |
|---------|-----|-------|
| **Spreadsheet app** | The other defining feature | Build on `csv` + `calc`; add an interactive grid modal and a recalculation engine |
| **Calculator UI app** | Everyday use | A modal keypad over `calc` (HP-12C / FX-870P layouts) |

### E. Platform, power, and security

| Feature | Why | Notes |
|---------|-----|-------|
| **Deep low-power / AON** | All-day battery | RTC wake, power-domain tuning, and peripheral runtime PM beyond idle display-off |
| **Battery/charging UI** | Trust the gauge | Better ADC calibration, charge-state detection, and a battery panel |
| **Push-to-talk / BLE HID** | Peripheral use | BLE keyboard/mouse bridging and simple phone-side transfer |

### F. Connectivity and services

| Feature | Why | Notes |
|---------|-----|-------|
| **Captive-portal Wi-Fi setup** | First-run without a card | SoftAP is currently compiled out by policy; a temporary setup mode is a design decision |
| **USB gadget modes** | Easier file transfer | RNDIS/MTP-style gadget as an alternative to the SD and `receive` paths |
| **TLS trust management** | Real HTTPS | Certificate store on SD and a `certs` command; `httpget`/`c6ota` already use mbedTLS |
| **Calendar/contact sync protocols** | Real PIM | CalDAV/CardDAV-lite over the existing HTTP client |

### Explicitly not planned

- A second application-runtime model: the batch + hybrid/native-app model is
  the contract (see [`ABI.md`](ABI.md)).
- SD/flash code execution (`dlopen`, dynamic linking, per-app memory
  isolation): C apps are linked into the firmware and reached through a `.bat`
  shim.
- A second display writer or windowing model: the transcript/TUI/modal/GFX
  surfaces and the single UI-rebuild path are the contract.
- A native Preferences GUI — settings stay in `CONFIG.SYS` behind the `config`
  machinery, with `SET.BAT` as the touch UI.

---

## Open gaps (intentionally deferred)

- **SPI transactions** — reported as unavailable on this board: initializing
  the SPI host with the hosted SDIO link active stalls the chip. `spi status`
  works; the peek/poke/loopback verbs return an honest error.
- **Touch wake from sleep** — the GT911 INT line is not wired on this board, so
  light-sleep wake is timer/GPIO only (reported honestly).
- **Camera live preview** — still BMP capture works on the Tab5
  (`camera init` + `camera snap <file.bmp>`, verified 1280x720 to SD); the
  live-preview path (streaming frames into an LVGL canvas) is not implemented
  yet. Boards without a camera report the gap rather than pretending.

---

## How to propose work

1. Check this file and [`bugs.md`](bugs.md) to avoid duplicate effort.
2. Read the owning module's rules in [`ai-context.md`](ai-context.md), the
   integration guide in [`SDK.md`](SDK.md), and the app contract in
   [`ABI.md`](ABI.md).
3. Follow the change and verification workflow in [`ai-context.md`](ai-context.md):
   build both projects clean, verify on hardware, update the docs and changelog.
