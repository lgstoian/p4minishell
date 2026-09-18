# tools/

Host-side serial/screenshot/SD helpers (115200 baud). New scripts must reuse
the shared session driver instead of reimplementing serial loops.

## Port selection (all drivers)

First match wins: explicit `port=` argument, a trailing `COMx` argv token, the
`P4_PORT` environment variable, else `COM11` (the reference board in this
repo is `COM3`; pass it explicitly). Scripts with argparse take
`--port` (same fallback chain). Opening the port asserts DTR and the P4
resets on the transition — every driver drops DTR/RTS on open via
`shell_session.open_port()`; never open raw `serial.Serial` without it.

## Production test framework — `p4test/`

One package replaces the copy-pasted serial loops, BMPX/SDFX readers, and
ad-hoc PASS/FAIL bookkeeping. Import it; do not reimplement.

- `p4test/session.py` — `DeviceSession`: DTR-safe open, marker-synchronised
  `run()`, ANSI strip, panic detection, pushback-buffered `read_until` /
  `read_exact`, `read_binary_frame` (BMPX/SDFX).
- `p4test/asserts.py` — `Checklist` (named PASS/FAIL checks + table) and
  `SuiteResult`.
- `p4test/screenshot.py` — streaming capture + a `Bmp` pixel model
  (`px`/`region_mean`/`count_near`/`to_image`/`save_png`, `diff_ratio`).
- `p4test/sdbridge.py` — `push_file`/`push_local`/`pull_file` over the
  firmware `receive`/`send` channels (CRC-checked).
- `p4test/perf.py` — `parse_perf_report` (the `gfx stats`/`tui stats` line),
  `FrameStats` + smoothness score, `measure_markers`.
- `p4test/device.py` — `Device` facade (boot/run/screenshot/deploy, and the
  EXISTING destructive `factory_reset`/`format_sd` commands).
- `p4test/runner.py` + `p4test_run.py` — suite discovery and PASS/FAIL table.
- `p4test/agent.py` + `dogfood.py` — the autonomous dogfooding agent (below).

Suites live in `tools/suites/` as `sNN_*.py` exposing `NAME`, `TAGS`, and
`run(dev, ctx)`; they cover smoke, storage, batch, data, display, editor,
input/UI, connectivity, power/audio, performance and every reference app.

```powershell
python tools/p4test_run.py COM3                 # run every suite
python tools/p4test_run.py COM3 --list          # list suites
python tools/p4test_run.py COM3 --tag core      # subset by tag
python tools/p4test_run.py COM3 --only s05_storage,s06_batch
python tools/p4test_run.py COM3 --quick         # shorten soak/boot waits
```

## Autonomous dogfooding — `dogfood.py`

A seedable policy drives the board like a curious user (help, status, files,
apps, modals, TUI/canvas animation), captures a screenshot after EVERY action,
and journals anomalies (panics, timeouts, blue "BSOD" frames, all-black
frames, static-frame streaks, heap decline) to `screenshots/dogfood/<run>/`
(a `journal.jsonl`, a `report.md`, and the PNG/BMP proofs). It is
reproducible from `--seed`.

```powershell
python tools/dogfood.py COM3 --minutes 15
python tools/dogfood.py COM3 --minutes 3 --seed 7 --out screenshots/dogfood/run7
```

## Files

- `shell_session.py` — shared USB-Serial/JTAG session driver: `Shell`
  (prompt regex, panic-marker raise), `open_port()` (DTR-safe open that does
  **not** reset the board — the line state is set before `open()`), `hard_reset()`
  (explicit esptool reboot for tools that need a fresh boot), `default_port()`
  (port resolution). Import it, do not reimplement.
- `baseline_sweep.py`, `serial_sweep.py`, `serial_sweep2.py` — canned
  bring-up sweeps (versions, TUI/draw, modals, audio, periph, SD cycle).
- `bsod_watch.py` — timestamped long serial capture for the recurrent
  display-fault hunt; the DSI-underrun "BSOD" was root-caused with it. One
holder per port.
- `display_glitch_watch.py` — camera + serial detector for the "BSOD" MIPI-DSI
  underrun: mean(blue)−mean(red) over the panel ROI (Lorgar = DirectShow index
  1, auto-scaled ROI on 1280x720), saves peak-frame JPEG proofs. Flags:
  `--calibrate`, `--preview`, `--roi`, `--index`, `--device`, `--reset`,
  `--stress` (drives `display stress on/off`). Runs on system Python (cv2).
  See `changelog.md` (0.38.2).
- `led_watch.py` — webcam verifier for the WS2812 status LED. Locates the LED by
  cycling colours, calibrates an exposure-invariant (channel-ratio) colour table,
  then checks the solids, the effects (rainbow/breath/pulse/blink), auto-status,
  the `httpd start` transient blue pulse and the boot flash. Usage:
  `python tools/led_watch.py [COMx] [--camera N]`. System Python (cv2). See
  `changelog.md` (0.38.5).
- `harness/` — one-shot host drivers, all on `shell_session` port handling:
  - Wi-Fi: `wifi_connect.py`, `wifi_connect2.py`, `wifi_connect3.py`,
    `wifi_test.py`, `test_wifi.py`, `test_wifi_cycles.py`, `wifi_dump.py`,
    `wifi_dump2.py`, `httpd_start_test.py` (all need a reachable AP; the
    SSID/password are placeholders (`<P4_WIFI_SSID>` / `<P4_WIFI_PASSWORD>`); set your own before running).
  - Screenshots: `grab_screenshot.py` (`--port/--out/--crop-transcript`,
    BMPX binary protocol), `capture_tui.py` (`--port`, TUI regression +
    goldens), `get_screenshot.py`, `save_to_sd.py` (legacy helpers).
  - Static tree checks (no hardware): `verify.py`, `final_check.py`,
    `final_verify.py` — run from the repo root.
- `apps/companion/` drivers (not here, same conventions): `deep_test.py`
  (reactive marker-driven suite, 8/8 on hardware — the reference suite),
  `db_test.py`, `alarm_test.py`, `push_sd.py`, `run_companion.py` (retired
  stub forwarding to `deep_test.py`; the pre-TUI triggers it asserted can no
  longer occur).
- `managed_patches.patch` — backup of the hand-authored `managed_components/`
  deltas (currently: LVGL `lv_async` PSRAM/lock hardening, the port
  JD9165 `swap_xy` guard, and the BSP 4.x board wiring: `board_config.h`
  pin/timing defines, JD9165 panel select, audio fail-soft init, backlight
  config, touch tolerance + remap, SD slot-0 deinit + explicit-V LDO power).
  The generated 384-glyph `unscii_16` font is
  tracked in git; the reapply script restores it from HEAD when the vendored
  LVGL minor version matches.
  Re-apply after `idf.py update-dependencies` with
  `powershell -File tools/reapply_managed_patches.ps1`
  (`-Check` for a dry run). See `changelog.md` 0.35.1/0.35.2.

## Focused drivers (recent)

- `bg_run.py` — shared marker-sync drain driver (`run_quiet`, `marker_done`,
  `boot`) for background/loop tests; reuses `apps/companion/deep_test.py`
  (`worker_line`) rather than reimplementing serial reading.
- `perf_for_test.py`, `perf_transcript_test.py`, `clip_probe.py` — batch
  performance/correctness probes (loop output, transcript slice path).
- `bg_test.py`/`bg_test2.py`/`bg_kill*.py` — `start`/`taskkill` lifecycle.
- `tcmd_test.py`/`tcmd_shot.py`/`tcmd_debug.py` — dual-pane commander.
- `snake_test.py`/`snake_steer_test.py`/`snake_quit_test.py` — SNAKE.
- `bounce_test.py`/`bounce_twice.py` — gfx BOUNCE demo (incl. re-init safety).
- `elite_test.py`, `svc_test.py` — ELITE trader + background service/alarm.
- `asset_test.py`/`asset_test2.py`/`assets_verify.py`, `draw_list_test.py`,
  `table_cursor_shot.py`, `gfx_sprite_test.py` — asset/manifest + TUI/gfx verbs.
- `pkg_test.py`/`pkg_smoke.py` — packaged SD apps (`pkg` list/info/verify +
  install→run→remove round-trip; needs `apps/push_pkgs.py` first).
- `gfx_toolkit_test.py` — runs `GFXTOOL.BAT`, pulls the saved BMP with
  `pull.py`, and checks pixels for the B2 primitives + text, plus the scaled
  `PHOTO.BMP` blit region (when `[M-GFXTOOL-IMG]` is emitted).
- Image support: `apps/push_assets.py` generates/pushes `PHOTO.BMP` (96x64);
  `apps/pics/PICS.BAT` demos `view`/`draw image`/`gfx image`/`image info`. Decode,
  scaled decode, fit, and scaling are unit-tested in `test_gfx.c`.
- **Screenshots capture modals too**: `grab_screenshot.py` works while a
  `dialog`/`list`/`ask`/`browse`/`view`/`image show`/`hexview` modal or the
  `edit` editor is open (the bare streaming `screenshot` is handled by the
  console-reader task because a modal blocks the worker).
- `gen_gfx_font.py` — regenerate `components/gfx/gfx_font.c` from the
  public-domain unscii-8 TTF (run only if the font/cell size changes).
- `theme_test.py` — B3 theme CLI + reboot persistence (`theme list/show/set`).
- `header_test.py` — responsive header: `header mode` switching, layout-fit
  assertions across rotations, persistence (`header mode auto /save`).
- `keyboard_test.py` — OSK page registry + `keyboard mode` command: status,
  the `keyboard.page=<name>` round-trip for every page and alias, an unknown
  page rejection, and `show`/`hide`. Pure `keyboard_mode_name/parse` are unit
  tested in `test/main/test_keyboard.c`.
- `editor_test.py` — the `edit` surface over serial: File > Open round-trip
  (unnamed buffer → `\open` → path → marker → save), the new Nav-page
  Comment/Match/Wrap/Reload verbs, and shell restoration after quit. OSK
  auto-Nav/page switching is unit-tested + screenshot-verified (the editor
  owns serial input while open, so the page cannot be queried there).
- `editor_large_test.py` — large-file `edit`: pushes a generated multi-line
  file (`push_sd.push_file`), loads it, jumps to the end, appends a marker,
  saves, and verifies the marker via `type`. Exercises the PSRAM document, the
  virtualized render window (> `editor_render_rows`), and the internal DMA
  bounce buffer used by save. `[lines]` defaults to 4000.
- `ui_touch_test.py` — exhaustive touch suite built on the firmware `ui`
  verbs (synthetic LVGL pointer indev): taps every shell keyboard key on every
  page, header indicators, input-row buttons, every editor Nav/Edit action key,
  and dialog/list buttons; `--sweep [px]` adds a full-screen coordinate sweep.
  `ui target <id>` is a semantic activation (modal panels live in the
  auto-scrolling transcript, so raw coordinate taps are racy there); `ui tap`
  remains the raw-coordinate path.
- `completion_test.py` — shell input-area verification: the `history /search`
  filter (hit, miss, ERRORLEVEL), the debounced autosave profile in
  `HISTORY.TXT`, and the inline ghost completion driven through the OSK
  (`ui key` + the `ui state` `ghost=` field). Pure completion/search helpers
  are unit-tested in `test/main/test_completion.c` and
  `test/main/test_history_search.c`.
- `plot_test.py` — runs `PLOT.BAT`, pulls canvas BMPs with `pull.py` and checks
  world-coordinate pixels (func/axes/data/bar/point/line), plus a TUI-mode
  screenshot to `spikes/plot_tui.bmp`.
- `unit_run.py` — build-independent capture of the unit-test summaries.
- `regression.py` — one-command host regression: resets the board, runs every
  suite/guard (`deep`/`db`/`alarm`/`smoke`/`pkg`/`theme`/`gfx`/`plot`/`header`/
  `keyboard`/`editor`/`editor large`/`ui touch`/`completion`/`tx_stress`/`boot_regression`), prints a
  PASS/FAIL table, non-zero exit on failure. `--quick` shortens the boot soak.
  Unit tests remain a separate step.
- `wifi_bench.py` — host endpoint for the firmware `wifi throughput` command:
  runs the matching TCP/UDP peer and prints host- and device-side Mbit/s.
  Host and device must share a subnet.
- `tx_stress_test.py` — TX-pressure output-integrity guard:
  emits numbered lines while pausing reads to build backpressure and verifies
  every line arrived (guards the driver-API transcript mirror).
- `boot_regression.py` — fresh-boot regression guard: reboots N times and
  asserts no panic/assert, exactly one AUTOEXEC run, SD ready, and no
  unexpected W/E log lines (guards the deferred boot-script, header async
  ownership, and boot-warning suppression fixes).
- `audit_fixes_test.py` — hardware checks for the v1.0.0 audit fixes not
  covered elsewhere: `plot auto` refitting an inline `plot bar a,b,c` list,
  `del /s` recursing into subdirectories, and an overlong markdown pipe table
  keeping every row. Prints RESULT OK.
- `pull.py` — pull an SD file over `send` (SDFX framing).
- `parse_debuglog.py` — host-side parser for `debug save` exports
  (`txt`/`csv`/`json`, auto-detected): severity summary + chronological
  table. Pull `DEBUG.LOG` with `pull.py`, then
  `python tools/parse_debuglog.py DEBUG.LOG`.
- Deploy tools: `apps/push_apps.py` (reference apps), `apps/push_assets.py`
  (PIL sprites + `.ASSETS` manifests), and `apps/push_pkgs.py` (build + push
  `PKGS/<APP>/` install bundles).
