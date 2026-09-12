# tools/

Host-side serial/screenshot/SD helpers (115200 baud). New scripts must reuse
the shared session driver instead of reimplementing serial loops.

## Port selection (all drivers)

First match wins: explicit `port=` argument, a trailing `COMx` argv token, the
`P4_PORT` environment variable, else `COM11`. Scripts with argparse take
`--port` (same fallback chain). Opening the port asserts DTR and the P4
resets on the transition — every driver drops DTR/RTS on open via
`shell_session.open_port()`; never open raw `serial.Serial` without it.

## Files

- `shell_session.py` — shared USB-Serial/JTAG session driver: `Shell`
  (prompt regex, panic-marker raise), `open_port()` (DTR-safe open),
  `default_port()` (port resolution). Import it, do not reimplement.
- `baseline_sweep.py`, `serial_sweep.py`, `serial_sweep2.py` — canned
  bring-up sweeps (versions, TUI/draw, modals, audio, periph, SD cycle).
- `bsod_watch.py` — timestamped long serial capture for the recurrent
  display-blink hunt (see `bugs.md` O3). One holder per port.
- `harness/` — one-shot host drivers, all on `shell_session` port handling:
  - Wi-Fi: `wifi_connect.py`, `wifi_connect2.py`, `wifi_connect3.py`,
    `wifi_test.py`, `test_wifi.py`, `test_wifi_cycles.py`, `wifi_dump.py`,
    `wifi_dump2.py`, `httpd_start_test.py` (all need a reachable AP; the
    `4G-CPE_5542` credential inside is lab-only).
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
  (`-Check` for a dry run). See `bugs.md` M20/M28/M40 and
  `changelog.md` 0.35.1/0.35.2.

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
  `pull.py`, and checks pixels for the B2 primitives + text.
- `gen_gfx_font.py` — regenerate `components/gfx/gfx_font.c` from the
  public-domain unscii-8 TTF (run only if the font/cell size changes).
- `theme_test.py` — B3 theme CLI + reboot persistence (`theme list/show/set`).
- `header_test.py` — responsive header: `header mode` switching, layout-fit
  assertions across rotations, persistence (`header mode auto /save`).
- `plot_test.py` — runs `PLOT.BAT`, pulls canvas BMPs with `pull.py` and checks
  world-coordinate pixels (func/axes/data/bar/point/line), plus a TUI-mode
  screenshot to `spikes/plot_tui.bmp`.
- `unit_run.py` — build-independent capture of the unit-test summaries.
- `boot_regression.py` — fresh-boot regression guard: reboots N times and
  asserts no panic/assert, exactly one AUTOEXEC run, SD ready, and no
  unexpected W/E log lines (guards the deferred boot-script, header async
  ownership, and boot-warning suppression fixes).
- `pull.py` — pull an SD file over `send` (SDFX framing).
- Deploy tools: `apps/push_apps.py` (reference apps), `apps/push_assets.py`
  (PIL sprites + `.ASSETS` manifests), and `apps/push_pkgs.py` (build + push
  `PKGS/<APP>/` install bundles).
