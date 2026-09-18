# P4MiniShell — Bug Log (v1.0.0)

This file is the working log for bug hunting and hardware stress testing. It
was reset for the v1.0.0 public release so it reflects what is **still open**.
Fixed entries are removed at each reset (their history lives in
[`changelog.md`](changelog.md) and git).

- **Firmware:** v1.0.0 (ESP-IDF v5.5.5)
- **Reference board:** ESP32-P4 Function EV Board (JC1060P470C), ESP32-C6
  co-processor, JD9165 1024x600, GT911 touch, SD card present

> Before you start, read the working rules in
> [`ai-context.md`](ai-context.md) and the module you are about to touch. A bug
> is only closed when the fix is verified on hardware.

---

## Severity definitions

| Severity | Meaning |
|----------|---------|
| **CRITICAL** | Data loss, corruption, or a crash/reset on a normal path; bricking risk |
| **HIGH** | A major feature is broken or unreliable; crash reachable with specific input |
| **MEDIUM** | A feature is wrong, misleading, or fails in an edge case; no data loss |
| **LOW** | Cosmetic, documentation drift, ergonomics, or a rare flake |
| **OBSERVATION** | Not a bug; recorded so it is not "fixed" or re-reported |

---

## Reported-but-open

No open findings. Use this template for every new finding. Keep one heading
per bug; move the entry to the matching severity section once triaged. When
it is fixed, add the fix + verification and remove it at the next reset.

```
### <ID>. <one-line summary>

- **Severity:** CRITICAL | HIGH | MEDIUM | LOW
- **Component:** <component / command>
- **Found on:** <date>, <COM port>, firmware <version>
- **Symptom:** what the user sees; exact output or error text
- **Repro:** the minimal, deterministic steps
  1. ...
  2. ...
- **Expected:** what should happen instead
- **Root cause:** (fill in once known, with file:line)
- **Fix:** (fill in once fixed)
- **Verified:** the hardware check that proves it (suite/command + result)
```

---

## Running the campaign

```powershell
# 1. Build firmware + tests (both must be 0 errors / 0 warnings)
$env:IDF_PATH = "<path-to-esp-idf-v5.5.5>"; . $env:IDF_PATH\export.ps1
idf.py build

# 2. Unit suite on the board (Unity; black screen is expected)
cd test; idf.py build flash; cd ..
python tools/unit_run.py <COM_PORT>

# 3. Host hardware regression (apps, modals, editor, TUI/GFX, networking, boot)
python tools/regression.py <COM_PORT>
python tools/p4test_run.py <COM_PORT>

# 4. Visual sweep (screenshots + invariants + contact sheet)
python tools/p4test/visual_sweep.py <COM_PORT>

# 5. Physical panel (webcam) + dogfooding
python tools/display_glitch_watch.py --port <COM_PORT> --duration 120
python tools/dogfood.py <COM_PORT> --minutes 15

# 6. Reflash the main firmware after any test-app run
idf.py -p <COM_PORT> flash
```

Replace `<COM_PORT>` with your board's port (or set `P4_PORT`).

Useful long-run / stress drivers live in `tools/` (see
[`tools/README.md`](tools/README.md)):

| Area | Driver |
|------|--------|
| Boot reliability (panics, AUTOEXEC, SD ready) | `tools/boot_regression.py` |
| Long serial soak / stalls | `tools/stall_catch.py` |
| Output integrity under TX pressure | `tools/tx_stress_test.py` |
| Wi-Fi + SD concurrent soak | `tools/wifi_bench.py` + `wifi throughput` |
| Automated host suites | `tools/p4test_run.py` |
| Visual capture + invariants | `tools/p4test/visual_sweep.py` |
| Autonomous dogfooding / visual anomalies | `tools/dogfood.py` |
| Touch automation (every OSK key/modal) | `tools/ui_touch_test.py` |
| Screen "BSOD" (camera) | `tools/display_glitch_watch.py` |
| Status LED (camera) | `tools/led_watch.py` |

When a bug is camera-visible, capture a proof frame before and after the fix.
See the diagnostics section in [`ai-context.md`](ai-context.md).

> **Sweep flakiness.** The full regression is long; `crypt` and `tcpterm`
> occasionally report a missing `RESULT OK` in the sweep (board/network state
> left by the previous step) but pass on an immediate re-run. Treat a single
> sweep failure there as non-reproducible until it repeats.

---

## Known quirks / by design

These look like bugs but are intentional. Check here before "fixing" one.

- **`pwd` is not a command.** It returns `Unknown command` (the prompt and
  `%CD%` show the directory). Use `cd` with no argument.
- **Serial keys need Enter.** The UART console reader is line-buffered, so a
  bare keypress does not reach `pause`/`choice`/`more` waits; type the key then
  Enter. USB and on-screen keyboards deliver raw keys.
- **`list` selection vs ERRORLEVEL.** Serial selection is 1-based (the number
  you type); the returned ERRORLEVEL is the 0-based index. `q` cancels with 255.
  Count the items carefully when writing `if errorlevel` chains.
- **`dir` parses a leading-`/` path as switches.** `dir /APPS` reads `/A`
  attributes. Use the `sd:` prefix or `cd` first. Same for `del`/`find`/etc.
- **`set /a` prints its result** (by design, even with `@echo off`), so a
  `for`-loop counter floods the transcript. Use
  `draw list /count:VAR /countonly` for counts.
- **Batch files are RAM-resident during execution** (128 KB cap). Larger files
  stream from SD. `goto`-heavy loops stay off the card.
- **`pkg install` verifies before it copies; `pkg remove` trashes.** Install
  CRC-checks every payload and aborts without touching installed files on a
  mismatch; remove sends files through `storage_trash_delete_file()` so
  `undelete` can recover an uninstalled app.
- **`gfx text` uses a committed bitmap font** (8x8 unscii-8), not an LVGL font,
  so the raster core stays LVGL-free. `gfx fill` grows a PSRAM seed stack.
- **Theme switch leaves two small bits stale** until a rebuild: the header
  panel `|` separators and any already-open modal keep their old colour. No
  functional impact; reopen/rebuild to refresh.
- **Black screen after flashing the test app** is expected: `p4minishell_tests`
  speaks only over serial. Reflash the main firmware from the repo root.
- **`plot` sampling binds `X`/`T` through the environment** and restores them
  afterwards; a pre-existing `X`/`T` survives a plot.
- **Screenshot captures can show a horizontal wrap artifact** (right-edge
  pixels appearing at the far left). Assert layout through `header status` /
  `tui status` metrics rather than absolute screenshot pixels.
- **Boot "No SD card detected" is sometimes printed even though the card
  works.** The early probe can miss while the card lazy-mounts on first access;
  batch apps run normally on those boots.
- **The first `tone` after boot logs a benign `i2s_common` error** while the
  tone still plays; it comes from the managed codec/i2s open path and clears
  after first use.
- **Transcript span footprint grows with history** and stabilises at the
  scrollback ceiling; `cls` (or the automatic trim) reclaims it. This is the
  cost of coloured scrollback, not a leak.
- **ESP32-P4 APM-560 errata note.** Concurrent AHB access to PSRAM/flash can
  wedge later traffic until reset. Keep one display writer at a time (background
  display verbs refuse), do not run an OTA overlapping PSRAM background stacks,
  and serialise SDMMC bring-up and host tools. I2C-308 (slave-only) and
  RMT-176/ECDSA-837 need no action on this board.

---

## Tracked / deliberately retained (not bugs)

Known code-quality items intentionally left as-is pending a dedicated pass.
Recorded here so they are not re-reported as new bugs.

| ID | Location | Finding | Why retained |
|----|----------|---------|--------------|
| C1 | `networking.c` / `bluetooth.c` / `usb.c` / `c6ota.c` | four copies of `*_text_equals_ignore_case` and three of `*_split_args` | a shared copy would force a new dependency onto the USB leaf; the helpers are tiny and stable. Revisit with a shared `strutil` component |
| C2 | `storage_ini.c` / `trash.c` / `db.c` / `archive.c` | four `mkdir -p` variants | each sits behind a different session/guard pattern; unify behind `storage_mkdir_p()` as a follow-up |
| C3 | `components/header/header.c` | `s_header_state` (incl. `clock_text`) is written from several tasks while the LVGL task reads it | scalar writes are atomic here and the render reads a consistent snapshot in practice; a mutex is the clean follow-up |
| C4 | `components/markdown/markdown.c` | `md_render_table()` uses a file-scope static scratch array (not reentrant) | the renderer runs on the single worker task; make it a heap buffer if concurrent rendering is ever added |
| C5 | `components/display/display.c` | `display_set_refresh_rate()` is a documented "not supported on this panel" stub | the panel/BSP does not expose dynamic refresh; reported honestly |
| C6 | `components/shell/shell.c` | `camera` verbs are honest "no camera stack" errors | no on-board camera hardware/stack in this workspace (documented gap) |

---

## Suggested starting points for the next hunt

Not bugs — just high-value areas to probe while a campaign is open:

- **Visual:** header panel/separator alignment across styles and rotations,
  OSK key labels/clipping, modal panel centring and button rows, editor
  gutter/caret alignment, TUI box-border continuity, plot axes, and
  theme/font/cursor consistency.
- **Data safety:** interrupted `copy`/`export`/`archive`/`crypt`, full-card
  mid-write, eject during a write, `db`/`alarm` corruption recovery.
- **Batch edge cases:** deeply nested `call`/`for /f`/`setlocal`, pipes inside
  batch stages, redirect capture at the depth limit, `^` continuation with
  labels.
- **Memory:** long sessions driving the internal DMA heap; background jobs that
  allocate PSRAM while an OTA is pending.
- **Modal/input:** rotation during a modal, serial bursts while a modal is
  open, screen-off then input, USB keyboard + touch simultaneously.
- **Networking:** Wi-Fi drop mid-transfer, reconnect storms, `httpd` under load,
  `tcpterm` with slow/idle peers.
- **Power:** repeated `sleep`/`deepsleep` cycles, idle-off then wake, audio
  playing into sleep.
- **Board portability:** panels other than JD9165 and their timing/touch.
