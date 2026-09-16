# P4MiniShell — Bug Campaign (v1.0.0)

This file is the working log for bug hunting and hardware stress testing. It is
reset at the start of each campaign so it always reflects what is **still
open**. Fixed entries are removed when the next campaign begins (their history
lives in [`changelog.md`](changelog.md) and git).

- **Firmware:** v1.0.0 (ESP-IDF v5.5.5)
- **Board:** ESP32-P4 rev 1 on COM3, ESP32-C6 co-processor, JD9165 1024x600,
  GT911 touch, SD card present
- **Test matrix:** the on-board unit suite plus the host regression — see
  [Running the campaign](#running-the-campaign)

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

## Code audit — v1.0.0 (fixed in this release)

A full static + hardware audit was run for the first public release. Every
finding below was fixed and verified on COM3; the fix locations are recorded so
they can be reviewed later. The audit covered storage/db/archive,
batch/command, networking/usb/c6ota, and the UI stack (display/windows/header/
keyboard/tui/gfx/font/editor/modal/markdown/applib).

### HIGH — fixed

| ID | Location | Finding | Fix | Verified |
|----|----------|---------|-----|----------|
| A1 | `components/header/header.c` | `header_schedule()` called `lv_async_call()` before LVGL/header init; the unit-test app aborted inside the TLSF allocator | guard on `s_header_root == NULL`; route the raw `lv_async_call` sites through it | unit suite 364/0/2 and completes (`main_task: Returned`) |
| A2 | `components/archive/archive.c` | `archive_entry_fits(NULL)` called `strlen(NULL)` | NULL check returning false | `test_archive_*` pass; unit 364/0/2 |
| A3 | `components/command/crypt_commands.c` | `remove(tmp)` on an uninitialised stack buffer on the "source open failed" path (could target an arbitrary path) | initialise `tmp = ""` and guard `remove()` | `crypt_test.py` RESULT OK |
| A4 | `components/command/tui_commands.c` (`form`) | a per-iteration `char label[80]` address was stored in the field table and used after the loop (dangling pointer) | labels held in a `labels[NF][80]` array owned by the command | build + modal suites (`ui_touch`, deep) green |
| A5 | `components/command/alarm_commands.c` | `time_s[12]` too small for `"%Y-%m-%d %H:%M"`, then printed `time_s + 11` (over-read) | buffer enlarged to 24 | `alarm_test.py` 25/25 |
| A6 | `config_cmd.c`, `storage_ini.c`, `wifi_known.c`, `csv_commands.c`, `crypt_commands.c` | `fflush(x) != 0 \|\| fclose(x) != 0` short-circuits: a failed flush skipped `fclose` and leaked the stream (`config`/`ini` then double-`fclose`d) | always `fclose` once and test the saved results | `theme`, `csv`, `crypt` suites green |
| A7 | `components/storage/trash.c` | limits enforcement could call `trash_delete_entry()` with a zeroed victim → resolved to the trash root and recursively deleted the whole bin | always select a real victim; reject empty/dot/`/` names in `trash_delete_entry` | `pkg_test` (removes use the trash) RESULT OK |
| A8 | `components/db/db.c` | nested `shell_sd_begin/end` on the same handle cleared the outer session and left the worker priority-boosted | reuse the held session (`stat`) instead | `db_test.py` 38/38 |
| A9 | `components/networking/networking.c` | a runtime-init failure after `esp_wifi_init()` never called `esp_wifi_stop/deinit`, so no retry could succeed | track `s_wifi_driver_inited`; release it in cleanup and shutdown | boot 8/8 clean; Wi-Fi associates |
| A10 | `components/font/font.c` | lock-order inversion (`s_ttf_lock` ↔ LVGL port mutex) deadlock, and a released TTF slot could be reacquired while its async destroy was pending (use-after-free) | destroy callback no longer takes `s_ttf_lock`; slots cleared at schedule time | `font`/`theme` + deep green |
| A11 | `components/markdown/markdown.c` | `realloc` failure double-freed/dangled the line arrays; the row that overflowed `MD_TABLE_ROWS_MAX` was dropped | grow one array at a time, commit only on success; keep the overflow row | `audit_fixes_test.py` |
| A12 | `components/modal/modal_surf.c` | timeout timers were created before the fallible open work, so an open failure left a timer to fire on the modal's freed event group | create/start the timer only at the end of each surface open | `ui_touch` + deep (dialog/list/ask/view) green |
| A13 | `components/display/display.c` | `lv_async_call` without the LVGL port lock from worker/boot tasks; rotation reported success when the lock failed | lock around the call; return an error if the lock fails | `header_test`, `rotate` |
| A14 | `components/windows/windows.c` | keyboard-visibility callback ran LVGL layout unlocked; `windows_deinit()` left stale surface-mode flags | take the recursive port lock; reset all mode flags/pointers | `keyboard_test`, `ui_touch` green |

### MEDIUM / LOW — fixed

| ID | Location | Finding | Fix |
|----|----------|---------|-----|
| B1 | `command/image_commands.c`, `gfx_commands.c`, `tui_commands.c` | the BMP file-load block was duplicated in `image info`, `draw image`, `gfx image`, `gfx load` | one shared `command_load_file_psram()` |
| B2 | `components/command/userial_commands.c` | `recv [/n]` documented but rejected by the parser | implement `/n` (suppress the trailing newline) |
| B3 | `components/modal/modal_surf.c` | filebrowser path copies not NUL-terminated; form SELECT ignored its stored default; form fields not bound to the OSK; a dialog Tab stub | terminate copies; select the matching option; bind the first editable field; drop the stub |
| B4 | `components/command/plot_commands.c` | `plot auto` after an inline `plot bar a,b,c` tried to open the list as a file | skip the file path for inline bar lists |
| B5 | `components/command/import_commands.c` | the oversized-CSV quote-drain condition was inverted | stop on the first balanced line |
| B6 | `components/editor/editor.c` | unchecked `lv_async_call` on close (view could outlive a freed doc); replace-next advanced the cursor on a failed insert; undo/redo applied history on a failed deserialise | synchronous fallback; guard the cursor; bail without popping |
| B7 | `components/storage/storage.c` | `copy` reported success after an `fread` I/O error; the wildcard `realloc` failure leaked every token string | test `ferror`; free the strings on realloc failure |
| B8 | `components/storage/storage_text.c` | `findstr /G:`/`/F:` read files before opening the guarded SD session; public matcher helpers lacked NULL guards | open a local session for the list files; add guards |
| B9 | `trash.c`, `storage_files.c`, `storage_ini.c` | restore/purge did I/O before the session; a failed trash move counted as deleted; `del /s /p` did not recurse; INI could deref a NULL key/value | session first; count only on success; recurse for permanent too; NULL guards |
| B10 | `components/archive/archive.c` | max-length manifest path truncated (`mpath[256]`); the create-walker kept the parent FF_DIR open across recursion | size to `ARCHIVE_PATH_MAX+1`; defer subdirs until the parent handle is closed |
| B11 | `components/networking/netdiag.c` | self-aliasing `snprintf` (destination also a `%s` source) is undefined behaviour | format the IP into a temporary |
| B12 | `netbench.c`, `tcpterm.c`, `bluetooth.c` | uninitialised result on invalid-arg; unchecked `fcntl`; NimBLE partial init with no teardown; wrong scan cap printed | zero the result first; check `fcntl`; `nimble_port_deinit()`; correct the message |
| B13 | `components/usb/usb.c` | partial mutex/queue allocation leaked; HID device dropped if the lock take failed | delete created handles; close the device |
| B14 | `components/c6ota/c6ota.c` | double `esp_hosted_slave_ota_end()` on finalize failure; 32-bit percent overflow | clear the started flag on all paths; 64-bit math |
| B15 | `editor_view.c`, `keyboard.c`, `gfx.c`, `tui.c`, `serial_commands.c`, `batch.c` | NULL indev; unlocked `keyboard_bind_textarea`; ignored flood-fill OOM; unreachable double-free guard and a table-stop off-by-one; diagnostic `pos` could exceed the buffer; a one-char `/` flag read `[2]` out of bounds | targeted guards/clamps |
| B16 | `components/batch/calc.c` | `LEN()` was shadowed by the `LN` prefix match (documented function unreachable) | exact-match `LN` |

### Retained (tracked, deliberately not changed)

| ID | Location | Finding | Why retained |
|----|----------|---------|--------------|
| C1 | `networking.c` / `bluetooth.c` / `usb.c` / `c6ota.c` | four copies of `*_text_equals_ignore_case` and three of `*_split_args` | a shared copy would force a new dependency onto the USB leaf; the helpers are tiny and stable. Revisit with a shared `strutil` component |
| C2 | `storage_ini.c` / `trash.c` / `db.c` / `archive.c` | four `mkdir -p` variants | each sits behind a different session/guard pattern; unify behind `storage_mkdir_p()` as a follow-up |
| C3 | `components/header/header.c` | `s_header_state` (incl. `clock_text`) is written from several tasks while the LVGL task reads it | scalar writes are atomic here and the render reads a consistent snapshot in practice; a mutex is the clean follow-up |
| C4 | `components/markdown/markdown.c` | `md_render_table()` uses a file-scope static scratch array (not reentrant) | the renderer runs on the single worker task; make it a heap buffer if concurrent rendering is ever added |
| C5 | `components/display/display.c` | `display_set_refresh_rate()` is a documented "not supported on this panel" stub | the panel/BSP does not expose dynamic refresh; reported honestly |
| C6 | `components/shell/shell.c` | `camera` verbs are honest "no camera stack" errors | no on-board camera hardware/stack in this workspace (documented gap) |

**Hardware verification (v1.0.0 audit):** unit **364/0/2** (clean completion,
no reboot); boot regression **8/8** clean; companion deep **8/8**; db **38/38**;
alarm **25/25**; app smoke **21/21**; `pkg`/`theme`/`gfx`/`plot`/`header`/
`keyboard`/`editor`/`editor large`/`timer`/`csv`/`export`/`bind`/`crypt`/
`tcpterm`/`userial`/`completion`/`tx stress`/`ui touch` all RESULT OK on COM3;
new `tools/audit_fixes_test.py` RESULT OK (plot-auto inline bar, `del /s`
recursion, overlong markdown table).

---

## CRITICAL

*(none open)*

## HIGH

*(none open)*

## MEDIUM

*(none open)*

## LOW

*(none open)*

---

## Reported-but-open

Use this template for every new finding. Keep one heading per bug; move the
entry to the matching severity section once triaged. When it is fixed, add the
fix + verification and remove it at the next campaign reset.

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
$env:IDF_PATH = "C:\esp\v5.5.5\esp-idf"; . $env:IDF_PATH\export.ps1
idf.py build

# 2. Unit suite on the board (Unity; black screen is expected)
cd test; idf.py build flash; cd ..
python tools/unit_run.py COM3

# 3. Host hardware regression (apps, modals, editor, TUI/GFX, networking, boot)
python tools/regression.py COM3

# 4. Reflash the main firmware after any test-app run
idf.py -p COM3 flash
```

Useful long-run / stress drivers live in `tools/` (see
[`tools/README.md`](tools/README.md)):

| Area | Driver |
|------|--------|
| Boot reliability (panics, AUTOEXEC, SD ready) | `tools/boot_regression.py` |
| Long serial soak / stalls | `tools/stall_catch.py` |
| Output integrity under TX pressure | `tools/tx_stress_test.py` |
| Wi-Fi + SD concurrent soak | `tools/wifi_bench.py` + `wifi throughput` |
| Boot/first-mount and SD/C6 ordering | `tools/boot_regression.py` |
| Touch automation (every OSK key/modal) | `tools/ui_touch_test.py` |
| Audit regression checks (plot auto, `del /s`, markdown) | `tools/audit_fixes_test.py` |
| Screen "BSOD" (camera) | `tools/display_glitch_watch.py` |
| Status LED (camera) | `tools/led_watch.py` |

When a bug is camera-visible, capture a proof frame before and after the fix.
See the diagnostics section in [`ai-context.md`](ai-context.md).

> **Sweep flakiness.** The full regression is long; `crypt` and `tcpterm`
> occasionally report a missing `RESULT OK` in the sweep (board/network state
> left by the previous step) but pass on an immediate re-run. Both were green
> individually after the v1.0.0 audit; treat a single sweep failure there as
> non-reproducible until it repeats.

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
  attributes. Use the `sd:` prefix or `cd` first.
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

## Suggested starting points for the next hunt

Not bugs — just high-value areas to probe while the campaign is open:

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
