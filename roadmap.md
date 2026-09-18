# P4MiniShell Roadmap

P4MiniShell is a **software framework for building palmtops, PDAs,
writerdecks, and similar handhelds on the ESP32-P4 + ESP32-C6**. This document
records how the framework evolved and where it is going.

The application model is deliberately **batch-first**: an on-SD `.bat` file is
an app, and a native C SDK (`applib`) supplies the polished surfaces batch
cannot draw. There is no PC-executable runtime and none is planned; the batch
surface *is* the app/compatibility layer. See [`readme.md`](readme.md) for the
feature tour and [`documentation.md`](documentation.md) for the architecture.

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

### 8. Portability groundwork (v1.1.0)

- Board profiles (`boards/<name>/` + `-DP4_BOARD=`, SD/SDIO pins promoted to
  `BOARD_CFG_*`) and a `PORTING.md` bring-up checklist.
- Native-app packaging spec (`docs/native_packaging.md`); `pkg` installs
  native bundles store-only (execution needs a future loader).
- SDK scaffolding: `tools/newapp.py` plus the `whoami` sample component.
- CI build matrix across board profiles; `debug save` export
  (`txt`/`csv`/`json`) with host-side `tools/parse_debuglog.py`.

### 9. Writerdeck (v1.2.0)

- Editor **focus / typewriter mode** (`edit /focus`): header and keyboard
  hidden, caret centred, live word count.
- **Markdown export** (`markdown export <src> <out> [text|html|print]`), a
  self-contained HTML serializer, and a fixed-page print paginator.
- **Reading typography**: a proportional/serif `reading` font role (vendored
  `DejaVuSerif`) with reader line spacing for the viewer and editor preview.
- **Offline spellcheck**: SD wordlist (`sd:/DICTS/`) with editor underlines.
- **Document templates** (`sd:/TEMPLATES/`) and the `WRITER` reference app.

---

## Part 2 — Where we are going

Realistic, useful directions grouped by the kind of device the framework is
meant to build. Priorities are suggestions, not commitments.

### A. Framework and portability

Section-A groundwork is done (v1.1.0, see Part 1 §8). Remaining:

| Feature | Why | Notes |
|---------|-----|-------|
| **M5Stack Tab5 + Tab5Keyboard port** | Second board proves the abstraction | `boards/m5stack_tab5/` profile (ST7123/ST7121 panel split, ES8388 audio, RX8130CE RTC, Tab5Keyboard I2C input); needs `esp_lcd_st7123`, not vendored yet |
| **Native-app loader** | Execute the stored native bundles | Position-independent blob + `app_register`; `abi`/`arch` enforced |
| **Signed manifests** | Safe installation | Sign `PKGS` manifests, verify before install (platform/security row) |

### B. Writerdeck

Section-B groundwork is done (v1.2.0, see Part 1 §9). Remaining:

| Feature | Why | Notes |
|---------|-----|-------|
| **Thesaurus / richer dictionary** | Writing aid | Extend the spellcheck wordlist with synonyms; larger bundled dictionaries |
| **Full-page HTML wrapper** | Standalone share | The HTML export is a fragment; a `<!DOCTYPE html>` page wrapper with reader CSS is future work |
| **More serif faces / weights** | Choice | Only `DejaVuSerif` (+Bold/Italic) ships; add more reading faces |

### C. PDA / PIM

| Feature | Why | Notes |
|---------|-----|-------|
| **Contacts / tasks apps** | Complete the PIM story | Build on the existing `db` store; ship as batch apps with modal surfaces |
| **Agenda integration** | One place for time | Merge `alarm`/`cal` events, tasks, and documents into a today view |
| **Sync (WebDAV / CalDAV-lite)** | Back up and share | HTTP client + VFS bridge; reuse the `export`/`import` interchange formats |
| **RSS / plain-text reader** | The classic PDA use | `httpget` + a reading view with the font/markdown stack |
| **Secrets manager** | Passwords on the go | Uses the existing `crypt` core and `db` secret fields, behind the passcode lock |
| **Search everywhere** | Find anything fast | A global index over `db`, alarms, and text files (`gfind` is the seed) |

### D. Palmtop / programmable

| Feature | Why | Notes |
|---------|-----|-------|
| **Spreadsheet app** | The other defining feature | Build on `csv` + `calc`; add an interactive grid modal and a recalculation engine |
| **Calculator UI app** | Everyday use | A modal keypad over `calc` (HP-12C / FX-870P layouts) |
| **Structured app format** | Richer apps | A declarative form/menu description so apps do not hand-roll every screen |
| **Third-party app registry** | Distribution | A signed index of `PKGS` bundles fetchable over Wi-Fi |

### E. Platform, power, and security

| Feature | Why | Notes |
|---------|-----|-------|
| **Deep low-power / AON** | All-day battery | RTC wake, power-domain tuning, and peripheral runtime PM beyond idle display-off |
| **Battery/charging UI** | Trust the gauge | Better ADC calibration, charge-state detection, and a battery panel |
| **Secure boot + flash encryption** | Protect the device | Document and provide a build profile; keep an unencrypted dev profile |
| **Signed apps** | Safe installation | Sign `PKGS` manifests and verify before install |
| **Push-to-talk / BLE HID** | Peripheral use | BLE keyboard/mouse bridging and simple phone-side transfer |

### F. Connectivity and services

| Feature | Why | Notes |
|---------|-----|-------|
| **Captive-portal Wi-Fi setup** | First-run without a card | SoftAP is currently compiled out by policy; a temporary setup mode is a design decision |
| **USB gadget modes** | Easier file transfer | RNDIS/MTP-style gadget as an alternative to the SD and `receive` paths |
| **TLS trust management** | Real HTTPS | Certificate store on SD and a `certs` command; `httpget`/`c6ota` already use mbedTLS |
| **Calendar/contact sync protocols** | Real PIM | CalDAV/CardDAV-lite over the existing HTTP client |

### Explicitly not planned

- A second application-runtime model: the batch + native-app model is the
  contract.
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
- **Camera** — no local camera stack in the workspace; camera verbs report the
  gap rather than pretending.

---

## How to propose work

1. Check this file and [`bugs.md`](bugs.md) to avoid duplicate effort.
2. Read the owning module's rules in [`ai-context.md`](ai-context.md) and the
   integration guide in [`SDK.md`](SDK.md).
3. Follow the change and verification workflow in [`ai-context.md`](ai-context.md):
   build both projects clean, verify on hardware, update the docs and changelog.
