# P4MiniShell — Architecture

System views of P4MiniShell: context, runtime flows, concurrency, memory,
power/reset domains, persistence, board abstraction, and the build/deploy
picture — as diagrams and matrices, with a pointer to the owning document for
every detail. This file is deliberately **views, not details**: it never
restates a module's feature list, a C prototype, a command's syntax, or a
config value. Those live in the docs named below.

| Question | Owning document |
|----------|-----------------|
| Who owns which module (ownership + responsibilities) | [`documentation.md`](documentation.md) §Module map |
| Public C functions and structs | [`API.md`](API.md) |
| How to extend the system (recipes, patterns) | [`SDK.md`](SDK.md) |
| Command syntax and switches | [`command.md`](command.md) |
| Batch language | [`batch.md`](batch.md) |
| App ABI and package trust | [`ABI.md`](ABI.md), [`docs/native_packaging.md`](docs/native_packaging.md) |
| Board profiles and bring-up | [`PORTING.md`](PORTING.md), [`schematics.md`](schematics.md) |
| Security policy (device lock, secrets, TLS) | [`SECURITY.md`](SECURITY.md) |
| Working rules for contributors/agents | [`ai-context.md`](ai-context.md) |
| Testing ladder and harness | [`harness.md`](harness.md), [`test/README.md`](test/README.md) |
| Release gate and packaging | [`release.md`](release.md) |

---

## 1. Context and containers

The device is a self-contained handheld: a single ESP32-P4 application SoC with
an on-SD application store, an optional MIPI-CSI camera, and an ESP32-C6
co-processor for Wi-Fi/BLE reached over a shared SDIO transport.

```
        ┌──────────────┐   USB-Serial/JTAG    ┌───────────────────────────────┐
        │  Host tools  │◀────────────────────▶│                               │
        │ tools/*.py   │   (shell + SDFX/     │        ESP32-P4 (host)        │
        └──────────────┘    PIMX framing)     │  ┌─────────────────────────┐  │
                                             │  │      P4MiniShell        │  │
        ┌──────────────┐     MIPI-DSI / I2C   │  │  (this firmware)        │  │
        │  Panel+touch │◀────────────────────▶│  └─────────────────────────┘  │
        │  keyb/audio  │                     │                               │
        └──────────────┘                     └───────────┬───────────────────┘
                                                         │ SDMMC (shared host)
                        ┌────────────────────────────────┼─────────────────┐
                        ▼                                ▼                 ▼
                 ┌────────────┐                  ┌──────────────┐  ┌────────────┐
                 │  SD card   │                  │  ESP32-C6    │  │  USB host  │
                 │ (apps,data)│                  │ Wi-Fi / BLE  │  │ MSC/HID/CDC│
                 └────────────┘                  └──────────────┘  └────────────┘
```

The internal container view (one P4, cooperating tasks, one-way dependencies):

```
  main ─▶ command ─▶ batch ─▶ storage ─▶ shell ─▶ ansi/display/windows/header/keyboard/clock
                                                   ▲            ▲
                       (inverted via registration tables)       │
     applib · networking · usb · c6ota · alarm · pim · editor ───┘
```

The dependency chain, the two registration tables, and the "never require
`main`" rule are specified in [`documentation.md`](documentation.md)
§Architecture/§Layering and [`SDK.md`](SDK.md) §Architecture. This document
does not repeat them.

---

## 2. Runtime flows

### 2.1 Boot timeline

`app_main()` brings subsystems up in a load-bearing order (from `main/main.c`):

```
display_init ─▶ shell_init ─▶ command_init ─▶ shell_start_telemetry ("sheltlm")
   │
   ├─ native_apps_register, SD-first-mount callback, UI-rebuild callback
   ├─ shell_uart_console_start ─▶ shell_build_ui (LVGL surface)
   ├─ c6ota_init ─▶ led_init ─▶ audio_init
   ├─ storage_sdmmc_host_preinit   (serialize SD slot0 + C6 slot1 host bring-up)
   ├─ board_bsp_early_init / board_bsp_charge_enable   (gate C6/USB/charge rails)
   ├─ usb_init (+ keyboard callback) ─▶ tab5kbd_init ─▶ imu_init ─▶ time_init
   ├─ boot_run_startup: mount SD ─▶ ensure+apply CONFIG.SYS ─▶ run AUTOEXEC.BAT
   ├─ font_restore_saved
   ├─ networking_init   ◀── MUST run only AFTER the SD card is mounted
   └─ led_notify(BOOT_OK) ─▶ header refresh timer
```

Two ordering constraints are non-obvious and must not be reordered:

- **SD before the C6 transport.** The card (slot 0) and the hosted C6 (slot 1)
  share the SDMMC controller and its DMA-capable internal buffers; bringing the
  C6 up first races the card mount ("not enough mem"). Hence
  `storage_sdmmc_host_preinit()` then `boot_run_startup()` (which mounts) then
  `networking_init()`.
- **Rails before the transport/USB.** `board_bsp_early_init()` asserts the
  expander-gated C6 and USB rails before `usb_init()`/`networking_init()`.

Full step list: [`SDK.md`](SDK.md) §Required boot-time integration; boot-script
semantics: [`documentation.md`](documentation.md) §Boot Scripting.

### 2.2 Command lifecycle

```
input (prompt line)          →  chain split  →  variable expansion
   →  redirection/pipe binding  →  dispatcher arm (single, command.c)
   →  family handler (command/batch/etc.)  →  output
   →  transcript append / delta capture
```

- There is exactly **one** dispatcher ([`SDK.md`](SDK.md) §Command dispatch
  integration); never add a parallel one.
- Pipes and `<` redirection are bound through one input-source slot
  (see ADR-1).
- Output goes to the transcript or, when redirected, to the redirection writer.

### 2.3 Input routing

Every physical input source converges on the same injection points; new input
sources must reuse them (the `shell_usb_keyboard_input()` path):

```
on-screen keyboard ─┐
USB HID keyboard   ─┤
BLE HID keyboard   ─┼─▶ shell_usb_keyboard_input()  ─▶ shell input line / key queue
Tab5Keyboard (I2C) ─┘

serial console     ─▶ UART reader ─▶ prompt line, key-queue, or active modal hook
touch (LVGL indev) ─▶ LVGL event callbacks (input line, modal surfaces, idle wake)
```

An active modal surface (`components/modal/`) captures input through the
`shell_command_ops_t.modal_*` hooks; see [`SDK.md`](SDK.md) §Modal app surfaces
and §USB keyboard integration notes.

File presentation is registry-driven: `components/filetype/` maps extensions
to kinds, and the surfaces ask it rather than re-listing extensions. `view`/
`open`/`type` render Markdown and HTML to ANSI (the document renderers in
`components/markdown/`), images open the image viewer, and `edit` selects the
matching syntax lexer. A new extension is a `filetype` table row plus the
consumer branches, never a local extension check.

---

## 3. Concurrency and locking

P4MiniShell is single-application, multi-task. The worker owns command
execution; LVGL owns rendering; everything else is a leaf task.

| Task | Role | Created in |
|------|------|-----------|
| LVGL port task | Rendering, LVGL event callbacks, timers | `esp_lvgl_port` (via `display_init`) |
| `cmdw` (command worker) | Runs commands off the LVGL task | `components/command/command.c` |
| `sheltlm` (pinned) | Header telemetry sampler (heap/CPU/battery) | `components/shell/shell.c` |
| `alarm` | Alarm checker poll | `components/alarm/alarm.c` |
| `netsvc` | MQTT session + outbox flush (lazy start on first `net` verb) | `components/networking/netsvc.c` |
| `audio_play` | One-shot audio playback | `components/audio/audio.c` (route policy there too; BSP owns amp/jack pins) |
| C6 OTA task | Firmware download + SDIO transfer | `components/c6ota/c6ota.c` |
| Wi-Fi connect/background/watchdog | Networking lifecycle | `components/networking/networking.c` |
| USB host + module | USB host library + class handling | `components/usb/usb.c` |
| `tab5kbd`, `imu_rotate`, `dns_srv`, `boot_script` | Leaf polls/one-shots | their components |

Locking rules:

- **LVGL is not thread-safe.** Cross-task LVGL work is dispatched with
  `lv_async_call()`, or guarded with the recursive LVGL port lock
  (`bsp_display_lock`). LVGL must never call a blocking SD/heap-heavy path on the
  render task (see the F26 note in [`bugs.md`](bugs.md)).
- **The SD card is guarded** by a single session (`shell_sd_begin`/`shell_sd_end`);
  all file I/O goes through it.
- **Transcript staging** uses its own mutex, separate from the LVGL port lock, so
  the command worker does not serialize behind a render on the append path.
- Every module that owns mutable shared state has one mutex/critical section
  (for example the alarm store mutex). Public APIs document their thread-safety.

Lock ordering and the deadlock-prone paths are recorded in
[`documentation.md`](documentation.md) §Command/§Shell and
[`SDK.md`](SDK.md) §Stack budget on the dispatch path.

---

## 4. Memory and DMA

The P4 has a large external PSRAM and a smaller internal (DMA-capable) RAM.
The policy is **PSRAM-first for bulk, internal for DMA**:

```
  bulk buffers (transcripts, render docs, collections, app data)
        └─▶ p4heap / PSRAM allocators (components/p4heap)

  DMA descriptors + bounce buffers (LVGL draw, editor I/O, crypt, spell)
        └─▶ internal DMA-capable RAM, cache-aligned, never PSRAM

  per-command strings / record buffers
        └─▶ heap-allocated on the dispatch path (never line-sized stack locals)
```

Rules that follow from this:

- Never DMA into PSRAM; use the documented bounce-buffer path (editor, crypt,
  and spell each own one; do not add another copy).
- Keep large buffers off the dispatch-path stack ([`SDK.md`](SDK.md) §Stack
  budget on the dispatch path).
- The command worker and `start` pool reuse the same execution pipeline rather
  than spawning per-command tasks.

Details: [`documentation.md`](documentation.md) §Runtime Constraints;
[`API.md`](API.md) §Batch/§Applib memory.

---

## 5. Power, reset, and time domains

```
   run (display on)
     │  `power idle` timeout       ── backlight off only (shell stays alive; ANY input wakes)
     ▼
   idle display-off ── touch / USB / serial / key ──▶ run

   run ── `sleep [s]` ──▶ light sleep ── timer, user GPIO, touch INT*, kbd INT* ──▶ run
   run ── `deepsleep [s]` ─▶ deep sleep ── timer, user GPIO (RTC IOs only) ──▶ cold boot
   run ── `shutdown` ──────▶ Tab5: PMIC latch cut        JC1060: deep-sleep fallback
                                     │
                                     └─ physical power button (single press on / double off)

   `reboot` → esp_restart()            C6 always reset-on-boot over its own GPIO
```

`*` touch/keyboard interrupt wake is hardware/revision dependent — see the
matrix below.

| Board | Timer | User GPIO | Touch INT | Keyboard INT | PMIC latch |
|-------|-------|-----------|-----------|--------------|------------|
| `jc1060p470c` | light+deep | light+deep (RTC IO) | none wired | none | no (deep sleep) |
| `m5stack_tab5` (ST7123/ST7121) | light+deep | light+deep (RTC IO) | **light** (GPIO23) | **light** (GPIO50) | yes |
| `m5stack_tab5` (ILI9881C+GT911) | light+deep | light+deep (RTC IO) | none (pin strapped by BSP) | light (GPIO50) | yes |
| Deep sleep (any board) | yes | only GPIO0..GPIO15 (RTC IOs) | never | never | — |

Wake arming lives in `power_commands.c` (`shell_power_arm_light_wake` /
`shell_power_arm_deep_wake`); touch availability is read from the **live** panel
driver (`display_get_touch_int_gpio()`), not the compile-time pin macro. Time
is kept by the internal RTC counter + NVS anchor, with the Tab5 RX8130CE as an
optional external chip; the RX8130CE `INT` pin is **not routed**, so there is no
RTC-alarm wake (see [`roadmap.md`](roadmap.md) Open gaps). Full policy:
[`ai-context.md`](ai-context.md) §Power / Idle Rules, [`command.md`](command.md)
`sleep`/`deepsleep`, [`schematics.md`](schematics.md) §power/§I2C.

---

## 6. Persistence map (index only)

All runtime state that survives a reboot lives either on the SD card (guarded,
atomic temp+rename writes) or in NVS. Semantics per file are in the owning
module; this is just the index.

| Path | Owner | Notes |
|------|-------|-------|
| `sd:/CONFIG.SYS`, `sd:/AUTOEXEC.BAT` | `components/boot` | Generated once if missing, then user-owned |
| `sd:/WIFI.KNOWN` | `components/networking` | Known-network list |
| `sd:/NET.INI`, `sd:/NET/OUTBOX/` | `components/networking` | MQTT broker coordinates (with credential) and the offline publish journal (atomic `.MSG` records, flushed oldest-first) |
| `sd:/ALIASES.BAT`, `sd:/HISTORY.TXT`, `sd:/DEBUG.LOG` | `components/shell` / `components/command` | Aliases, recall history, debug log |
| `sd:/DBS/<name>.DB/` | `components/db` | `HEADER.INI`, `CATEGORIES.INI`, `INDEX.TXT`, `RECORDS/R<id>.DAT` |
| `sd:/ALARMS/` | `components/alarm` | `INDEX.INI` + `E<id>.INI` |
| `sd:/CERTS/` | `components/certs` | User CA trust store (+ rebuilt `BUNDLE.PEM`) |
| `sd:/APPS/`, `sd:/PKGS/<APP>/` | app model | Installed apps; signed bundles |
| `sd:/TEMPLATES/`, `sd:/DICTS/`, `sd:/FONTS/` | editor / spell / font | Content the shell reads (`DICTS/user.words` is the learned overlay) |
| `sd:/<temp dir>`, `sd:/.trash` | `components/storage` | Temp files (except `tmp/edit/` crash files, owned by `recover`); recycle bin |
| `sd:/tmp/edit/*.autosave` | editor autosave | Dirty-buffer crash files with `#P4AUTOSAVE` headers |
| NVS namespaces | clock / Wi-Fi / PHY | Time anchor, Wi-Fi state, calibration |

Atomicity rule: writes go through guarded sessions with temp-file + rename
([`documentation.md`](documentation.md) §Storage, [`SDK.md`](SDK.md) §Adding a
command that writes files). The SD card must be mounted before the C6 transport
starts (see §2.1).

---

## 7. Board abstraction layer

One profile selects both the pins/timing and the staged BSP; components only
ever read `BOARD_CFG_*` / `BSP_*` and zero-cost `board_caps_*` helpers, never a
raw pin.

```
  boards/<name>/board_config.yaml ──(mirror)── board_config.h  ── staged into config/
        │                                            │
        │                                            ├─ BOARD_CFG_* (pins, timing, feature flags)
        │                                            └─ board_caps_* (compile-time capability queries)
        ▼
  boards/<name>/board_bsp/  ── thin wrapper or vendored vendor BSP ──▶ BSP_* API
        │
        ▼
  components/display · audio · led · imu · camera · power_monitor · clock
```

- `-DP4_BOARD=<name>` selects the profile at configure time; the same name stages
  `board_config.h` into the build's `config/` include path.
- The reserved-pin guard (`components/command/periph_commands.c`) stops the GPIO
  toolkit from repurposing an active bus line.
- Per-board deltas and the bring-up checklist: [`PORTING.md`](PORTING.md); pin
  and power-tree ground truth for humans/agents: [`schematics.md`](schematics.md).

---

## 8. Build, test, and deploy

```
  two IDF projects                per-board build dirs            deploy
  ┌──────────────┐   ┌───────────────────────────┐   ┌──────────────────────────┐
  │ firmware     │   │ build/        (jc1060p470c)│   │ idf.py flash             │
  │ test/        │   │ build-tab5/   (tab5)       │   │ tools/push_*.py → SD     │
  │ coprocessor/ │   │ test/build, test/build-tab5│   │ pkg install → APPS       │
  └──────────────┘   └───────────────────────────┘   │ c6ota → C6 firmware      │
                                                      └──────────────────────────┘
```

- Root `CMakeLists.txt` and `test/CMakeLists.txt` mirror the board/sdkconfig
  layering (`sdkconfig.defaults` + `boards/<board>/sdkconfig.defaults` →
  `sdkconfig.<board>`); managed components are pinned by `dependencies.lock` and
  patched by `tools/managed_patches.patch` (re-apply after updates).
- Verification ladder and commands: [`harness.md`](harness.md) §Layers; the
  release gate that ties it together: [`release.md`](release.md).
- SD packaging (which files ship, and the push order): [`release.md`](release.md)
  and [`tools/README.md`](tools/README.md).

---

## 9. Security (pointer only)

Policy lives in [`SECURITY.md`](SECURITY.md): single-user device, the SD card is
the trust boundary, the device lock gates private db records and secrets, TLS
trust comes from the compiled bundle plus `sd:/CERTS/`, and `httpd` is
LAN-scoped. App trust (signed `PKGS` manifests, ECDSA P-256) is specified in
[`ABI.md`](ABI.md) and [`docs/native_packaging.md`](docs/native_packaging.md).
This document intentionally restates none of it.

---

## 10. Decision log (ADRs)

Short records of choices that shape the architecture. Each links to where the
decision is enforced.

- **ADR-1 — Pipes spool, they do not stream.** A pipe runs each side as its own
  batch process with an SD spool, not a live byte stream, so a slow reader
  cannot deadlock a writer. Enforced in `components/batch` and
  [`SDK.md`](SDK.md) §Batch process model.
- **ADR-2 — One dispatcher, one command family per module.** Commands register
  through a family arm/table; there is no second dispatcher. Enforced in
  `components/command/command.c` ([`SDK.md`](SDK.md) §Command dispatch
  integration).
- **ADR-3 — One-way dependencies, inverted by ops tables.** Upper services reach
  lower layers only through registration tables (`shell_command_ops_t`,
  `batch_command_ops_t`, `*_host_ops_t`); no trampolines. Enforced
  [`ai-context.md`](ai-context.md) §Module Layering Rules.
- **ADR-4 — Every capability has exactly one implementation.** No second parser,
  matcher, CRC, scanner, registry, or search path; extend the existing one.
  Enforced [`ai-context.md`](ai-context.md) §Hard Rules.
- **ADR-5 — Apps are `.bat` shims; no SD code execution.** Native code is linked
  into the firmware and reached through a shim. Contract: [`ABI.md`](ABI.md).
- **ADR-6 — Station-only Wi-Fi, BYO network.** No SoftAP data path outside the
  captive-portal first-run flow; every `esp_hosted`/Wi-Fi/NimBLE call stays in
  `components/networking`. Enforced [`SDK.md`](SDK.md) §Networking ownership.
- **ADR-7 — SD before C6, and rail-before-transport at boot.** Ordering is
  load-bearing (shared SDMMC host/buffers); see §2.1.
- **ADR-8 — The 0x44 expander is single-writer and reset-free.** Its chip-reset
  floats the C6 power rail; only the bounded-retry raw path touches it. See
  [`bugs.md`](bugs.md) F6 and [`PORTING.md`](PORTING.md).
- **ADR-9 — The editor document is byte-preserving.** CRLF/LF, trailing-newline,
  and non-UTF-8 bytes round-trip unchanged; the worker owns file I/O and the
  LVGL view is a surface over the shared modal runtime. Enforced
  `components/editor` ([`ai-context.md`](ai-context.md) §Editor Rules).
- **ADR-10 — Wake reporting is per board/revision, never aspirational.** Sleep
  arms only interrupts the fitted hardware actually wires and reports gaps
  honestly. Enforced `power_commands.c` + `display_get_touch_int_gpio()`
  ([`ai-context.md`](ai-context.md) §Power / Idle Rules).
- **ADR-11 — One event service, journal-first delivery.** A single `netsvc`
  task owns the only persistent client connection (MQTT); apps subscribe
  through verbs/ops, never sockets. Every publish journals to SD before the
  wire (at-least-once, oldest-first flush); inbound state topics merge
  through the existing `pim` merger. Delivery to batch is the registered
  async hook plus the last-message slot — no second dispatcher, no private
  notification loop. Enforced `components/networking/netsvc.c`.
