# Changelog

All notable changes to P4MiniShell are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [0.38.1] - 2026-09-13 (COM3; ESP-IDF v5.5.5)

### Changed — Wi-Fi throughput bench verified; lwIP TCP window raised

- Verified `wifi throughput` against a same-subnet host (AP `camera`, device
  192.168.1.239, host 192.168.1.194) with `tools/wifi_bench.py`.
- **Finding:** the 5760-byte lwIP TCP window, not the SDIO clock, capped a
  single TCP stream (~2.4 Mbit/s). Raised `CONFIG_LWIP_TCP_WND_DEFAULT` and
  `CONFIG_LWIP_TCP_SND_BUF_DEFAULT` 5760 → **32768**, and
  `CONFIG_LWIP_TCP_RECVMBOX_SIZE` 6 → 32 (lwIP buffers take PSRAM here via
  `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP`).
- Measured (16 MiB unless noted; median of samples):
  - **host → device:** 2.45 → **~11 Mbit/s** at 40 MHz (8.7 / 11.2 / 11.7);
    **7.9 Mbit/s** at 10 MHz.
  - **device → host:** ~1.6 (timeout) at 10 MHz → **4.4 Mbit/s** (8 MiB) at
    40 MHz.
- The 40 MHz SDIO clock is faster in both directions, confirming the v0.38.0
  adoption, and the larger TCP window is an end-to-end throughput win
  (`httpget`/`httpd` benefit too).
- Regression: `tools/regression.py` 11/11; unit 262/0/2.

## [0.38.0] - 2026-09-12 (COM3; ESP-IDF v5.5.5)

### Changed — 40 MHz SDIO clock adopted after a soak gate

- Raised `CONFIG_ESP_HOSTED_HOST_SDIO_CLK_KHZ` from 10000 to **40000** (the
  board's maximum). The earlier 40 MHz trial was reverted after an LVGL
  timer-list panic; that panic is now known to be the header async double free
  (O7, fixed in 0.36.1), which the extra 40 MHz DMA pressure merely made more
  likely — not a signal-integrity fault. Re-trialled and **adopted** on
  evidence: `boot_regression` 20/20 clean boots, a 20-minute concurrent
  Wi-Fi+SD `stall_catch` soak with **0 stalls**, and the full host suite green
  (unit 262/0/2, deep 8/8, db 38/38, alarm 25/25, smoke 21/21,
  pkg/theme/gfx/plot/header OK). `test/sdkconfig` already used 40 MHz.

### Added — Wi-Fi throughput bench

- **`wifi throughput tx <host> [port=N] [mb=N] [udp]`** /
  **`wifi throughput rx [port=N] [mb=N] [udp]`** (`components/networking/netbench.c`
  + command in `networking.c`): a plain lwIP TCP/UDP flood that reports
  Mbit/s, used to measure the ESP-Hosted SDIO transport. Host endpoint:
  **`tools/wifi_bench.py`** (drives the command and runs the matching peer, so
  one invocation prints both host- and device-side numbers).
  Caveat: a same-subnet host is required; the release session's lab had the
  host and device on different subnets (and switching the host AP needs
  elevation), so the bench shipped **hardware-unverified** and no throughput
  number was captured — the 40 MHz adoption rests on the stability soak above.
- **`tools/regression.py [COMx] [--quick]`**: one-command host regression that
  resets the board, runs every suite/guard, prints a PASS/FAIL table, and exits
  non-zero on failure (11/11 on COM3). The Unity suite remains a separate
  step (needs the test image).

## [0.37.1] - 2026-09-12 (COM3; ESP-IDF v5.5.5)

### Fixed — O3 single-output loss, O4 host resets, O5 boot wedge

- **O3 (single-output loss under TX pressure).** The IDF USB-Serial/JTAG
  `usb_serial_jtag_is_connected()` SOF monitor can falsely report
  "disconnected" for ~4 ms under host/load pressure, and both our mirror's
  early-return and the IDF VFS write path (`usb_serial_jtag_vfs.c:185`) drop
  the whole line when it flips. `shell_uart_console_write_text()` now mirrors
  through the driver API (`usb_serial_jtag_write_bytes`, which ignores the
  monitor), preserves the CRLF line ending the VFS would have applied, and
  gates only on a *persistent* disconnect (`P4_CONFIG_UART_MIRROR_DISCONNECT_GRACE_MS`,
  2 s) so transient flips never drop a line while a no-host session still
  stops mirroring without blocking. `tools/tx_stress_test.py` (numbered lines
  with deliberate read-pause backpressure) shows zero loss.
- **O4 (host tooling reset the board on every open).** `setDTR(False)` after
  `open()` ran too late — the reset already fired on the open transition.
  `shell_session.open_port()` now pre-sets `dtr=False`/`rts=False` on the
  unopened `Serial` object so the port opens with both lines low. Every host
  driver (tools, harness, apps) routes through it; tools that need a fresh
  boot use the new `shell_session.hard_reset()` (esptool), and `unit_run` /
  `boot_regression` were updated to reset explicitly.
- **O5 (silent-boot wedge).** The O4 fix removes the "hours of unclean DTR
  resets" that induced it. `display_init()` additionally runs a standard I2C
  bus recovery on the touch/codec bus (clock SCL up to 9 times, then a STOP)
  before the BSP claims the pins, releasing a slave that holds SDA low across
  resets; it is a no-op on a healthy bus. A PSRAM-init wedge in the bootloader
  remains hardware-only.

Verified on COM3: unit 262/0/2, deep 8/8, db 38/38, alarm 25/25, smoke 21/21,
pkg/theme/gfx/plot/header OK; `boot_regression` clean; push_apps/pkgs/assets OK.

## [0.37.0] - 2026-09-12 (COM3; ESP-IDF v5.5.5)

### Changed — boot-time internal-RAM relief + reliability hardening

- **PSRAM task stacks for the boot-time tasks that never touch host flash.**
  PSRAM is not `MALLOC_CAP_DMA` on this P4 build (`dma_spi=0`), so
  `MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA` requests fall back to internal RAM and
  the tightest boot point held only ~1 KB DMA free / 188 B largest block.
  Moved the USB `usb_host_lib`/`usb_module`, `c6ota`, `audio`, `alarm`, `led`
  and `shell_uart` task stacks to PSRAM via
  `xTaskCreate*WithCaps(..., MALLOC_CAP_SPIRAM)` (with `vTaskDeleteWithCaps`
  for their self-deletion). The command worker, Wi-Fi bring-up tasks and LVGL
  stay internal (they run NVS/flash-writing commands or are timing-sensitive).
  Measured tightest point: DMA free **1.0 KB → 10.7 KB**, largest DMA block
  **188 B → 8.7 KB**, internal free **10 KB → 40 KB**. See `bugs.md` O8.
- **Boot script off the Wi-Fi event task.** CONFIG.SYS/AUTOEXEC.BAT now run on
  a short-lived dedicated `bootscript` task (`P4_CONFIG_BOOT_SCRIPT_TASK_STACK`,
  internal stack since the script can run flash/NVS-touching commands) instead
  of the ESP-Hosted/Wi-Fi event task, so a slow AUTOEXEC cannot delay Wi-Fi
  event delivery. The DMA scratch buffer is cached by the time the first-mount
  callback starts the task, so it is race-free.
- **SD-timeout residual no longer reproducible.** With the internal-DMA
  headroom restored, a 12-minute concurrent Wi-Fi+SD `tools/stall_catch.py`
  soak produced 0 SD stalls (the earlier `sdmmc_read_sectors` 0x107 timeout is
  below detection); the one logged failure was the known O3 single-output loss.

### Added

- **`tools/boot_regression.py`** — fresh-boot guard that reboots N times and
  asserts no panic/assert, exactly one AUTOEXEC run, SD ready, and no
  unexpected W/E log lines. 8/8 clean on COM3.

### Documentation

- Clarified **W1**: the crash was the header async double free (O7), not an
  ESP-Hosted overrun. Recorded the esp_hosted findings: the SDIO fixes live
  only in the legacy 2.12.x line, **3.0.7 (latest) does not contain them** and
  the 3.x branch has no PSRAM transport-buffer Kconfig, so we stay on 3.0.6
  with no vendor patches (app-side mitigation only).

## [0.36.1] - 2026-09-12 (COM3; ESP-IDF v5.5.5)

### Fixed — heap corruption (LVGL timer-list panic) + boot reliability

- **LVGL timer-list panic root cause (heap double free).**
  `header_schedule()` freed the async payload when `lv_async_call()` failed
  (LVGL OOM under boot memory pressure), and all eight `header_update_*`
  callers then freed the same pointer again. The second free scribbled on the
  heap free list; the next LVGL timer allocation drew from the damaged region
  and `lv_timer_handler -> lv_ll_get_next` jumped to garbage. `header_schedule()`
  no longer frees — the caller owns the payload and frees it on failure (and
  `header_set_notification()` now does too, restoring one consistent rule).
  Verified 0 panics / 0 poisoning asserts in 20+ fresh boots under the same
  boot load that previously panicked ~1/10.
- **Orphaned header notification timer.** `header_deinit()` nulled
  `s_notification_timer` without deleting it; LVGL timers are *not* reclaimed by
  `lv_obj_clean()`. It now deletes the armed one-shot (runs on the LVGL task
  under the port lock).
- **`header_set_visible()` mutated LVGL unlocked.** Called from the command
  worker (`config`/`header`) and the boot path; it now takes the recursive port
  lock around the widget change.
- **`editor_view_set_blink_ms()` TOCTOU.** The `open` check ran before the port
  lock; a close in that window created an orphaned cursor timer. Moved inside
  the lock.
- **Boot SD script silently skipped every boot.** The eager mount races the
  ESP-Hosted C6 bring-up for the shared SDMMC controller, DMA-capable internal
  RAM and the SD-IO LDO, so it failed with `ESP_ERR_NO_MEM` and CONFIG.SYS /
  AUTOEXEC were never applied. Boot scripting now runs from the SD first mount
  (the same late mount that already fired "SD card ready"), guarded to run once;
  the expected eager-mount failure logs are muted for that one attempt only.
  AUTOEXEC now runs every boot.
- **USB host could stay dead for the whole boot** (`HCD Port 0 init error:
  ESP_ERR_NO_MEM`) while the C6 bring-up held the contiguous internal block.
  USB bring-up now retries (each stage idempotent) with the expected transient
  library errors muted for the retry window; USB comes up on every boot tested.
- **Expected ESP-Hosted boot warnings silenced** (`ESP_LOGW` -> `ESP_LOGD`,
  matching the suppression the pre-3.0.6 tree carried and the migration
  dropped): the 10 MHz SDIO clock fallback notices and the co-processor GPIO
  reset notice. 10 MHz remains the negotiated clock (a deliberate stability
  choice); 40 MHz removes the notice but was not adopted.
- Cleaned the stale hand-edited `CONFIG.SYS` on the test SD (reserved-pin
  `GPIO 42` + `UNKNOWN_DIRECTIVE`) that produced the last `gpio` rejection
  warning; the firmware correctly rejected the reserved pin either way.

Verified: full boot log free of W/E; unit **262/0/2**, companion deep **8/8**,
db **38/38**, alarm **25/25**, smoke **21/21**, pkg/theme/gfx/plot/header OK.
(One transient `sdmmc_read_sectors` timeout appeared on an early db run and
passed on rerun — the known O6 latency residual, unrelated to these fixes.)

## [0.36.0] - 2026-09-12 (COM3; ESP-IDF v5.5.5)

### O6 SD-latency fix: session priority boost + read retry (2026-09-12 session, COM3)

- **Root cause (proven in code):** the SD card (slot 0) and the ESP-Hosted C6
  transport (slot 1) share one SDMMC controller whose every transaction takes
  a single global mutex with an unbounded wait
  (`sdmmc_transaction.c:105`). The hosted transport tasks run at priority 22
  while the shell worker runs at 2, so under sustained hosted traffic the
  worker loses every arbitration and an SD op can stall for a minute or more
  while the console still echoes. Separately, transient contention failures
  (fast, not slow) could truncate `sd info` output, hanging host drivers on a
  missing line.
- **Fix:** `shell_sd_begin()` raises the caller to
  `P4_CONFIG_SD_OP_BOOST_PRIORITY` (23, just above hosted) for the session
  and `shell_sd_end()` restores it (LIFO-safe; a worker-loop backstop bounds
  any leak to one command). `storage_get_space_info()` retries idempotent
  reads (`P4_CONFIG_SD_OP_RETRIES` x `P4_CONFIG_SD_OP_RETRY_DELAY_MS`), and
  `sd info` prints `unavailable` lines instead of omitting them on persistent
  failure. Gated timing probe (`P4_CONFIG_SD_OP_TIMING`) logs overshoots.
- Verified: 20-min `stall_catch` soak went from 1 SD timeout to 0 SD stalls
  (12-min confirmation soak clean); all suites green with no regressions.
  The 2 non-SD output absences in the soak are the known O3 host-side
  residual, unchanged by this work.

### Managed batch B: esp_codec_dev 1.6.2 + BSP 4.2.3 (2026-09-12 session, COM3)

- **Swapped** `espressif/esp_codec_dev` 1.2.0 → 1.6.2 (checksum-verified
  archive; old tree proven byte-clean vs pristine 1.2.0 first) and the board
  support package 4.1.1 → 4.2.3. BSP 5.x stays excluded (drops JD9165 for
  esp_video); codec 2.x stays excluded (prerelease + new `usb_host_uac` dep).
- **Re-ported all four local BSP deltas onto 4.2.3** (inventoried by diffing
  the worktree against pristine 4.1.1 first): `board_config.h` pin/timing
  wiring in both BSP headers, JD9165 panel select (kept on the held jd9165
  1.0.2 driver; upgraded to the proper `jd9165_vendor_config_t`), audio
  fail-soft init, backlight config + init-once guard, touch tolerance/remap,
  SD slot-0 deinit, and the explicit-voltage SD LDO driver with delete on
  failure/unmount. Dropped as obsolete: the SD LDO workaround is partly
  superseded by 4.2.3's on-chip LDO path, but the explicit driver is kept
  because the on-chip path leaks the channel on mount failure. `display.c`
  now passes `hw_cfg.dsi_bus` (550 Mbps + default PHY clock), which 4.2.3
  requires instead of reading the bitrate macro itself.
- **Proven on hardware**: the pristine-4.2.3 on-chip LDO path fails SD mounts
  deterministically after one failure (`already in use` — the exact leak the
  restored driver fixes); with the re-port, a DMA-OOM-flaked boot mount
  self-recovers and `sd info` is full. `tone`/`audio status` clean on codec
  1.6.2 with no codec errors.
- Verified: both builds clean, unit 262/0/2, deep 8/8, db 38/38, alarm 25/25,
  smoke 21/21 (one `advent:back` flake, green on re-run), pkg/theme/gfx/plot/
  header green, post-flash screenshot. `tools/managed_patches.patch` now also
  backs up the BSP 4.2.3 hand deltas (validated with `git apply --check`
  against pristine 4.2.3).

### Managed batch A: eppp_link 1.1.6 + usb_host_hid 1.2.1 (2026-09-11 session, COM3)

- **Swapped** `espressif/eppp_link` 1.1.5 → 1.1.6 (retry-counter reset fix) and
  `espressif/usb_host_hid` 1.2.0 → 1.2.1 (disconnect-cleanup rework, remote
  wakeup) via `idf.py update-dependencies`; both old trees proven byte-clean
  against pristine upstream archives before replacement. No local patches in
  either tree; no manifest pin changes (both float inside existing ranges).
- **Held**: `esp_lcd_touch_gt911` at 1.2.0~1 (1.2.1 drops the driver-level
  800x480→1024x600 `touch_scale`, which would compress touch into the
  top-left region — verified by source diff, no code touched) and
  `esp_lcd_jd9165` at 1.0.2 (1.0.4 deletes the default gamma/init table and
  retimes the DPI clock/porches our BSP relies on — verified by source diff).
- **Held**: `esp_hosted` at 3.0.6 (pinned exact in both manifests after the
  resolver floated it). 3.0.7 was attempted and rejected on hardware: its
  public compat version macros froze at 2.12.6 (broke our C6 version gate —
  fixed separately, see below), and without the tree's local W1
  SPIRAM-hardening patches (which 3.0.7's refactored RPC/DMA regions no
  longer fit) the SDIO RX mempool OOMs at boot and Wi-Fi never starts.
  Reverting to 3.0.6 + local patches restores a clean boot and connect.
- **Version-gate fix** (`networking.c`, `bluetooth.c`, `P4_CONFIG_HOSTED_COMPAT_MAJOR 3`):
  the Wi-Fi/BLE C6-firmware gates compared against the frozen compat macros
  and would false-fire on any 3.x host. Gates are now major-only against the
  project constant; messages/recovery text updated (2.x → 3.x).
- Verified: both builds clean, unit 262/0/2, deep 8/8, db 38/38, alarm 25/25,
  smoke 21/21, pkg/theme/gfx/plot/header green, post-flash screenshot. (One
  mid-session eppp/hid scare — theme-output loss + a single Backtrace — was
  traced to transient board/UART state, not the firmware: full re-runs green
  with no code changes.)

### LVGL 9.5.0 + esp_lvgl_port 2.9.0 vendored upgrade (2026-09-11 session, COM3)

- **Pinned** `lvgl/lvgl: "9.5.0"` and `espressif/esp_lvgl_port: "2.9.0"` in
  `main/idf_component.yml` and `test/main/idf_component.yml`; replaced only
  those two vendored trees from the official registry archives. BSP stays
  4.1.1 and unrelated components/pins are unchanged.
- **Preserved project deltas**: the generated 384-glyph `lv_font_unscii_16`
  extension, the LVGL `lv_async` PSRAM/lock hardening (adapted to 9.5), and
  the port JD9165 `swap_xy` guard. `tools/managed_patches.patch` now backs up
  the hand-authored LVGL/port diffs, and the reapply script restores the
  tracked extended font when the vendored LVGL minor version matches.
- **Locks/config**: the root `dependencies.lock` records only the LVGL/port
  version changes plus its manifest hash; the test lock records the same plus
  an `eppp_link` 1.1.5 → 1.1.6 registry float (required `>=0.1`, sources under
  the ignored `test/managed_components/`). `test/sdkconfig` picked up the LVGL
  9.5 Kconfig deltas (RGB565-swapped, RISC-V vector ASM, ext-data, WebP,
  NanoVG; retired XML/OpenGL/scroll-demo symbols).
- Verified: firmware + test builds clean, unit 262/0/2, deep 8/8, db 38/38,
  alarm 25/25, smoke 21/21, pkg/theme/gfx/plot/header green, post-flash
  screenshot captured.

### Responsive header: no-overlap layout, dynamic font, uptime (2026-09-11 session, COM3)

- **Measurement-driven layout** (`components/header/header_layout.c`, pure +
  unit-tested): every render measures the live labels and fits the status,
  notification, and system panels into the real content width — full labels,
  then abbreviations, then dynamic font step-down, with the notification
  yielding first and panels hidden only as a last resort. Panels get explicit
  absolute geometry (no flex timing drift); the notification label is pinned
  to its container width so `LONG_SCROLL_CIRCULAR` scrolls instead of
  overflowing; a full invalidation clears stale pixels on every re-layout.
- **Single height authority** (`header_get_height()`, used by both the header
  and `windows_get_rect`), so the bar can never gap or overlap the transcript
  on any resolution/rotation/board.
- **`header` verb** (`status` with wanted-vs-actual widths, `mode
  auto|full|compact [/save]`, `show|hide`), CONFIG.SYS `HEADER_MODE=`,
  `SHELL.INI` persistence with boot restore, and the previously-fed but never
  rendered **uptime** indicator.
- **Boot-restore hardening**: saved UI choices now report success, and a
  failed first-mount restore re-arms for the next mount instead of being
  skipped for the whole boot (`storage_sd_first_mount_reset()`); `main` also
  restores after boot scripting. `theme_test` persistence is green across
  repeated runs.
- Verified: unit 262/0/2 (5 layout cases), `tools/header_test.py` (modes,
  rotations, persistence), screenshots of auto/compact/long-notification.

### Plot/graph layer: world-coordinate graphs, charts, drawings (2026-09-11 session, COM3)

- **New `plot` verb** (`components/command/plot_commands.c`): `plot
  tui|window|auto|axes|func|polar|para|data|bar|table|line|point|clear|status`.
  One shared viewport renders world-coordinate math onto the `gfx` pixel canvas
  (default) or the TUI cell grid (`plot tui on`); function sampling reuses the
  `calc` evaluator (`y=f(X)`, `r=f(T)`, `x/y=f(T)` with X/T env-bound and
  restored), data/bar files reuse the guarded-SD `fopen`/`fgets` pattern, and
  colors reuse the `gfx`/`draw` parsers. Canvas plots never auto-show (compose,
  then one `gfx show`); TUI plots flush through `draw_maybe_flush` (so `draw
  hold` coalesces them). Foreground-only like `gfx`, except read-only `plot
  status` and text-only `plot table`.
- **Pure viewport math** (`components/gfx/gfx_view.c`, headless unit-tested):
  window-to-raster mapping with edges on the border indices, exact
  Cohen–Sutherland segment clipping (trivial-reject first), and 1/2/5x10^n
  nice-step ticks. Shared `gfx_canvas_*`/`draw_*` accessors via `command.h`
  (no copied canvas/color logic).
- **Config**: `P4_CONFIG_PLOT_SAMPLES` (240), `P4_CONFIG_PLOT_MAX_POINTS`
  (512), `P4_CONFIG_PLOT_LINE_BYTES` (128), `P4_CONFIG_PLOT_TICK_TARGET` (8).
- **Reference app** `apps/gfxdemo/PLOT.BAT` + `PLOT.APPINFO` (axes+grid,
  sin/cos, world line; the 10th package) and the HW driver
  `tools/plot_test.py` (canvas pixel checks incl. region-based curve checks,
  plus a TUI-mode screenshot). Regression: unit 257/0/2, deep 8/8, db 38/38,
  alarm 25/25, smoke 21/21, pkg 15/15, theme 11/11, gfx toolkit 17/17, plot
  25/25.

### Dead-code cleanup: storage stub, p4_usb fold, unused statics, help dedupe (B4, 2026-09-11 session, COM3)

- Deleted `components/storage/storage_commands.c` (an include-only stub since
  v0.35.6) and dropped it from the storage CMake SRCS; `storage_commands.h`
  remains the shared declaration header for the split storage files.
- Folded the `p4_usb` component into `components/usb`. `components/p4_usb`
  was only a CMake wrapper that compiled `../usb/usb.c`; the `usb` component
  now builds `usb.c` itself (adding `ansi`, `usb_host_hid`, `usb_host_msc` to
  its REQUIRES). Deleted `components/p4_usb`, removed it from the root
  `EXTRA_COMPONENT_DIRS`, switched the `command`/`main` REQUIRES to `usb`,
  and dropped the test list's duplicate `markdown` entry.
- Removed unused statics: `header_async_batch` and its `header_batch_update_t`
  payload, `networking_schedulef` (dead; `networking_schedulef_ansi` is the
  live formatter), and `fb_entry_cmp` in `modal_surf.c` (never called); dropped
  stale `__attribute__((unused))` markers from `s_header_height` and
  `header_scale_height` (both are used).
- Fixed the `help /all` table: removed the duplicate `launch`/`apps` entries
  and refreshed the `gfx` entry with the B2 verbs (added the `theme` entry).
- `usb status` still reports `usb.host: ready` after the fold. Regression:
  unit 253/0/2, deep 8/8, db 38/38, alarm 25/25, smoke 21/21, pkg/theme/gfx
  green.

### UI themes: registry, live switching, persistence, Companion menu (B3, 2026-09-11 session, COM3)

- **Theme registry completed** (`components/font/theme.c`): four built-in
  themes (`default`, `amber`, `ice`, `mono`) in a pure, unit-tested table;
  `theme_get`/`theme_builtin_at`/`theme_set`/`theme_active_index` and a mutable
  active selection. The struct gained `description`, `text_body`, `warn`,
  `header_panel_bg`, and `header_sys_bg`; the `default` table is
  pixel-identical to the compiled palette.
- **`theme list | show [name] | set <name> [/save]`** (`font_commands.c`). A
  successful `set` re-applies live: `windows_refresh_theme()` (screen/
  transcript/input row), `keyboard_refresh_theme()`, `header_refresh_theme()`
  (bar/panel bg + full re-render). Modal surfaces pick up the table at open.
- **Live theme plumbing**: `windows_get_color` and the header's text/accent/
  muted/warn macros now read `theme_current()` (header gains the `font`
  dependency); `/save` writes `theme=<name>` to `sd:/APPS/SHELL.INI`, restored
  by `font_restore_saved()` at the first SD mount.
- **Companion Settings ‣ Theme** submenu in `apps/companion/SET.BAT`
  (default/amber/ice/mono, Show, Back); `deep_test.py` SET walk updated for the
  new item and Back index.
- **Tests**: `test/main/test_theme.c` (3 registry cases) and the HW driver
  `tools/theme_test.py` (CLI + reboot persistence, green). Regression: unit
  253/0/2, deep 8/8, db 38/38, alarm 25/25, smoke 21/21, pkg 15/15, gfx
  toolkit 17/17, theme 11/11.

### Gfx toolkit: raster primitives + on-canvas text (B2, 2026-09-11 session, COM3)

- **New raster primitives** (`components/gfx/gfx.c`, pure/headless, unit-tested):
  `gfx_surface_hline`/`vline` (clipped spans), `gfx_surface_triangle`
  (edge-function fill or outline), `gfx_surface_polygon` (even-odd scanline fill
  for convex/concave shapes, `GFX_POLY_MAX_PTS` 64, or outline),
  `gfx_surface_ellipse` (per-row span fill or outline; zero radius degenerates),
  and `gfx_surface_flood_fill` (4-way, PSRAM-grown seed stack, returns the pixel
  count). Filled `rect`/`circle` now flush through `hline`.
- **On-canvas text**: `gfx_surface_text`/`gfx_text_width` render 8x8 ASCII from
  a committed table `components/gfx/gfx_font.c` (glyphs 0x20..0x7E, MSB-first),
  generated by `tools/gen_gfx_font.py` from the public-domain unscii-8 TTF
  bundled with LVGL. Integer `/scale:1..16` and optional `/bg:` cell fill.
- **New `gfx` verbs** (`components/command/gfx_commands.c`): `hline`, `vline`,
  `triangle`, `ellipse`, `polygon <color> <fill|line> <x y ...>`, `fill <x> <y>
  <color>` (prints `gfx: filled <n> pixel(s)`), and `text [/bg:<color>]
  [/scale:<n>] <x> <y> <color> <text...>` (remaining words joined with spaces).
- **Reference app** `apps/gfxdemo/GFXTOOL.BAT` + `GFXTOOL.APPINFO` (all
  primitives + scaled text, saves `GFXTOOL.BMP`); added to `push_apps`,
  `push_assets`, and `push_pkgs` (9 packages).
- **Tests**: 7 new unit cases in `test/main/test_gfx.c` (spans/triangle/
  polygon/ellipse/flood-fill/font/render) and the HW driver
  `tools/gfx_toolkit_test.py` (runs GFXTOOL.BAT, pulls the BMP, checks 13
  pixels/regions against the RGB565 quantization). Regression green: unit
  250/0/2, deep 8/8, db 38/38, alarm 25/25, smoke 21/21, pkg 15/15, gfx
  toolkit 17/17.

### Packaged SD applications: `pkg` verb + bundle format + App Manager (2026-09-11 session, COM3)

- **New `pkg` verb** (`components/command/pkg_commands.c`, dispatched from
  `command.c`): `pkg list | info <app> | verify <app> | check | install <app> |
  remove <app>`. Installed state lives in `sd:/APPS/<APP>.APPINFO`
  (`title=`/`description=`/`version=`) + `sd:/APPS/<APP>.ASSETS` (the
  `path=HEXCRC` manifest); the install source is the bundle `sd:/PKGS/<APP>/`
  (`<APP>.ASSETS`, `<APP>.APPINFO`, and payloads at install-relative paths).
- **`pkg install`** is two-pass: verify every bundle payload CRC first (abort
  cleanly on any missing/corrupt file), then copy payloads and the APPINFO +
  manifest into place. **`pkg remove`** trashes every payload plus the two
  metadata files, so `undelete` can recover an uninstalled app.
- Asset helpers `asset_app_ok`, `asset_crc_file`, `asset_verify_app(tag, app,
  list_only)` exported from `asset_commands.c` (declared in `command.h`) and
  reused by `pkg`; `asset check|list` now calls `asset_verify_app`. New pure
  helper `pkg_app_name_from_appinfo` (uppercases/validates `NAME.APPINFO`,
  unit-tested).
- **Reference package app** `apps/pkgtest/PKGTEST.BAT` + `PKGTEST.APPINFO`,
  bundled/pushed by `apps/push_pkgs.py` (payload + APPINFO + generated
  manifest). Companion **Live System ‣ Packages** submenu in
  `apps/companion/SYS.BAT` (list/verify/info/install/remove). `version=1.0`
  added to every reference `*.APPINFO`.
- New config: `P4_CONFIG_PKG_BUNDLE_DIR_NAME` (`PKGS`),
  `P4_CONFIG_PKG_APPS_DIR_NAME` (`APPS`), `P4_CONFIG_PKG_MAX_ENTRIES` (48),
  `P4_CONFIG_PKG_MANIFEST_BYTES` (16384), `P4_CONFIG_PKG_LINE_BYTES` (512).
  `apps/push_assets.py` now also emits `APPS/COMPANION.ASSETS`, so `pkg check`
  verifies all reference packages (`pkg: n/n package(s) ok`; 9 after the GFXTOOL
  bundle).
- New HW drivers: `tools/pkg_test.py` (full install→run→remove round-trip,
  green), `tools/pkg_smoke.py`, plus `apps/push_pkgs.py`. Unit tests
  `test/main/test_pkg.c` (`pkg_app_name_from_appinfo`). Companion **Live
  System ‣ Packages** deep-walk added to `deep_test.py`. Regression green:
  unit 243/0/2, deep 8/8, db 38/38, alarm 25/25, smoke 21/21, pkg round-trip
  15/15 (deep/alarm show the usual first-run reset/barrier flake, green on
  re-run).

### Batch performance: loop deferral, O(1) transcript appends, off-console mirror, RAM-loaded scripts (2026-09-11 session, COM3)
- **Batch files execute from RAM.** A script at or below
  `P4_CONFIG_BATCH_FILE_MAX_BYTES` (128 KB) is read once into PSRAM at frame
  entry and executed from memory via a byte-for-byte `fgets`-equivalent
  reader (`shell_frame_fgets/tell/seek/load`), so `goto`-heavy loops
  (Snake/TCMD/ADVENT) never touch the SD card mid-loop. Larger files stream
  from SD exactly as before (identical semantics).
- **Pipe scan fast path**: lines with no `|` skip the quote/escape state
  machine (`strchr` pre-check).
- **One work buffer per command segment** (was two 4 KB mallocs + a full
  copy): expansion now writes straight into the working buffer.
- **Transcript appends are O(1).** `shell_transcript_append_to_buffer` called
  `strlen()` on both 64 KB buffers on every append, so a long output loop was
  effectively quadratic. The buffers now carry tracked lengths
  (`s_transcript_len`/`s_transcript_ansi_len`) updated at every mutation site
  (append, reset, memory-pressure trim, init) and read by the getters; the
  label staging path uses a new length-aware
  `windows_set_transcript_text_len()` instead of `snprintf("%s")`. No output
  or buffer-size change (`set /a` still prints; history/clipboard/screenshots
  unchanged, hardware-verified via `clip copy`).
- **One transcript repaint per `for` loop.** `for`/`for /f` now wrap all
  iterations in a single `shell_transcript_defer_begin/end`, so the O(buffer)
  LVGL span rebuild happens once per loop instead of once per iteration
  (nested per-command defer windows are depth-counted).
- **No console mirror when no host is attached.** `shell_uart_console_write_text`
  drops output when `usb_serial_jtag_is_connected()` is false, so the
  USB-Serial-JTAG TX path can no longer block the command worker on
  backpressure when running untethered. The transcript is unaffected; over
  USB the mirror still works (verified).
- **TCMD capture reuse**: a pane's `dir` runs only when its directory
  changed since the last frame, so j/k navigation does ~2 SD reads instead of
  ~6; file ops and the Refresh menu force a relist.
- New probes: `tools/perf_for_test.py`, `tools/perf_transcript_test.py`,
  `tools/clip_probe.py`. Regression green: unit 241/0/2, deep 8/8, db 38/38,
  alarm 25/25 (one barrier flake on first run, green on re-run), smoke 21/21.

### TUI selectable tables/list + Total Commander TCMD + background services (2026-09-11 session, COM3)
- `draw table /cursor:N /sel:a,b` — bright cursor row + bold selected rows
  (`tui_draw_table_ex`, old signature a wrapper; pure parsers
  `tui_table_parse_cursor`/`_sel`, 4 new unit tests). Fixed a latent
  column-drop bug: the pad loop reused the split cursor and blanked the
  last column, so 3-column tables rendered only 2 (found via screenshot,
  fixed by advancing `c` first).
- New `draw list <x> <y> <w> <h> <file> [fg] [bg] [/top /cursor /sel
  /title /count:NAME /countonly]` — renders a file's lines as a bordered,
  scrollable, selectable panel; `/count` reports the line count to an env
  var (batch's `set /a` echoes, so a `for`-loop counter floods). Reuses the
  box/print primitives; off-TUI falls back to marked plain lines.
- `apps/tcmd/TCMD.BAT` rewritten into a real dual-pane commander: live panes
  from `dir /o:gn /b` + `draw list`, bright cursor per pane, active-pane
  accent, path titles, status line, F-key-style hint bar, `draw fullscreen
  on` for a full 80×25 grid (restored on exit). Keys j/k/t/o/x/s/1/2/p/q;
  all previous ops kept behind the `x` menu; `p` snapshots to TCMD.BMP.
  Screenshot-verified (`spikes/tcmd_full.png`).
- Background apps: `apps/companion/SVC.BAT` (headless service loop, `start
  SVC` / `taskkill bg0`) and `AGENDA.BAT` (calendar + notify job); a new
  **Live System ‣ Services** companion submenu (Status/Start/Stop/List
  alarms/Run agenda). Documented the background rule (headless-safe verbs
  only) and the two schedulers (`start`, `alarm … /run:`). `svc_test.py`
  drives start/tick/taskkill/agenda green on HW.
- Spike findings that shaped the design: `dir > file` captures clean bytes
  (no ANSI); `for /f` supports `skip=`/`tokens=1-6` but not `usebackq`
  string sets; serial keys need Enter (`fgets` line-buffering); `set /a`
  always prints; `@echo off` + `draw fullscreen on` are the TUI-app staples.

### Sprites, display discipline, assets, Snake/TCMD/Elite (2026-09-10 session, COM3)
- `gfx load/blt/free/slots/save` (`components/gfx` pure BMP parser/decoder/
  blit + `command/gfx_commands.c` verbs): 24-bit `BI_RGB` ingest (the exact
  `screenshot` format) into 8 PSRAM sprite slots (max 64×64 RGB565, 64 KB
  worst case), clipped blits with optional transparency, canvas save reusing
  the shared `screenshot_write_bmp_headers`. 5 new unit tests (234→239 with
  the asset suite); HW pixel round-trip exact (565 truncation verified).
- A2 display discipline: `draw`, `tui` (except read-only `tui status`),
  mutating `color`, `locate`, `anchor` refused in `start` bg jobs with
  BOUNCE-style loud messages (modal/`gfx` already refused); plus `draw hold
  on|off` frame coalescing (N verbs, one LVGL rebuild, `draw refresh`
  closes; `draw close` resets). HW-verified refusal matrix + errorlevels.
- `crc32 <path>` + `asset check|list <app>` (`command/asset_commands.c`):
  the firmware's single CRC-32 (`shell_crc32_update`, un-staticed from the
  `receive` path) meets host `zlib` bit-for-bit; `APPS/<APP>.ASSETS`
  manifests generated by `apps/push_assets.py` (PIL sprites, no repo blobs)
  verify `OK` for all 8 reference apps on-device, with exact MISMATCH /
  MISSING / bad-name paths.
- Reference apps (all pushed, HW-driven to markers): `apps/snake/SNAKE.BAT`
  (packed-coordinate TUI snake, `choice /T` steering, wall/quit/screenshot
  paths proven, incl. serial key+Enter finding), `apps/tcmd/TCMD.BAT`
  (dual-pane commander over native `browse`, 11-item dispatch fixed live),
  `apps/elite/ELITE.BAT` (turn-based trader, save-file verified).
  Companion `FILES.BAT`/`FUN.BAT` delegate to them (drivers updated for the
  shifted Back indices).
- Batch lessons documented in `command.md` (game-writing guide): no
  indirection/substrings (packed-number idiom), env 24 cap, list 1-based
  serial vs 0-based ERRORLEVEL, no `>` in `rem`, `dir` leading-`/` switch
  ambiguity, rows ≤ 21 HUD rule, serial key+Enter (UART `fgets`
  line-buffering).
- Errata read (ESP32-P4 rev v1.x, this board): APM-560 disciplines the
  single-display-writer rule; I2C-308/RMT-176/ECDSA-837 need no action
  (master-only / IDF-bypassed / unused flows).

### Gfx canvas + BOUNCE demo app (2026-09-10 session, COM3)
- New `components/gfx/` raster core (RGB565, PSRAM, max 320x240, clipped
  pixel/line/rect/circle, `gfx_rgb_to_565`) with 6 unit tests, plus `gfx`
  verbs (`init|close|status|clear|pixel|line|rect|circle|show`) in
  `components/command/gfx_commands.c`: lv_canvas display in the transcript
  region, explicit `gfx show` for flicker-free animation, DOS 0-15 CGA
  colors at full precision (`tui_dos_color_rgb`), exclusive with TUI mode
  and refused in background jobs. Screenshot-verified
  (`screenshots/gfx_demo.png`: rects, circles, diagonal).
- New reference app `apps/gfxdemo/BOUNCE.BAT` (+`.APPINFO`, pushed): batch
  `set /a` ball physics in a 60-frame loop with in-app screenshot at frame
  30 (`screenshots/bounce_ball.png` caught the ball mid-flight). Ran twice
  clean (DONE both passes, re-init safe). `start BOUNCE` refuses the shared
  display cleanly (verified headless-refusal path).
- bugs.md note: boot prints "No SD card detected" yet the card lazy-mounts
  on first access (BOUNCE runs fine on those boots) — cosmetic, early probe
  vs lazy mount.

### TUI bar/table + draw colors (2026-09-10 session, COM3)
- New `draw bar <x> <y> <w> <pct> [fillch] [emptych] [fg] [bg]` progress bar
  and `draw table <x> <y> <fg> <bg> "h1|h2|..." [row ...]` bordered table
  (T-junctions, auto-fit widths, bold header, 16 cols x 32 rows max), each
  with TUI and cursor-addressed non-TUI paths. Verified via screenshots
  (`screenshots/tui_verify4.png`: box + bar + table, zero markup leaks).
- `draw box/text/line/fill` now honor fg/bg on the TUI path (were hardcoded
  7/16): values <= 16 pass through as DOS indices, larger values quantize
  via new `tui_rgb_to_dos()` (CGA table, Euclidean nearest).
- Fixed `tui_flush` recolor corruption: a literal `#` cell (bar fills) threw
  LVGL's recolor state machine into parameter-skipping and desyncd every
  later tag (visible `#rrggbb` litter + swallowed cells). Flush now doubles
  `#` in untagged runs and splits colored runs around `#` cells
  (close/`##`/reopen). Matches LVGL 9.4's `lv_draw_label.c` escape rules.
- Unit: `test_tui_rgb_to_dos_exact/nearest`, `test_tui_table_total_width`.

### Background jobs: start/taskkill (2026-09-10 session, COM3)
- New `start <command> [args]` verb runs a command line or batch file as a
  background job on a pooled worker (`bg0`..`bgN`, `P4_CONFIG_BG_TASKS`),
  `taskkill <job>` stops it cooperatively (kill flag per batch line +
  100 ms `delay` chunks; `delay: stopped`, ERRORLEVEL 1). Verified on HW:
  mid-run kill of `delay 10000`, slot reuse, `no such job` after exit.
- Pool tasks are created suspended at init with full 32 KB PSRAM stacks via
  `xTaskCreateStatic` (`CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=y`
  pinned): 32 KB internal no longer fits at runtime (measured 26 KB free /
  12 KB largest block). PSRAM goes inaccessible during OTA flash writes, so
  `start` refuses while a C6 OTA is pending and `c6ota` refuses while a job
  runs. Each job owns a private batch ctx, transcript defer slot, and
  storage redirect slot (`batch.c`/`shell.c`/`storage.c` per-task slots);
  modal/key-wait/appmode verbs refuse headlessly instead of blocking the UI.
- Harness lesson: prompt-tail matching is unusable with deferred output
  (`PS /sdcard> PS /sdcard> HI` — output legally trails the prompt), and
  `D.worker_line` misses doubled-prompt marker lines. New
  `tools/bg_run.py` marker-sync driver (marker echo + multi-prompt strip +
  0.3 s serial timeout); full regression green with it (unit 220/0/2,
  deep 8/8, db 38/38, alarm 25/25, smoke 21/21).

### File types + editor hardening (2026-09-09 session, COM3)
- New `components/filetype/` central registry (leaf, no cycles): `.bat`/`.cmd`
  executable, `.md`/`.markdown`/`.mkd` Markdown, `.json` JSON,
  `.txt`/`.log`/`.sys`/`.ini` text. Migrated every match site (editor
  syntax, viewer md branch, launch discovery, dir colours, batch resolver),
  deleting the `.md×3` duplication and three `.bat`-test styles.
- `.cmd` files execute like `.bat` (resolver probes both, launch discovers
  `*.cmd`, bare-name dispatch; LAUNCH_MAX bumped 16→24 so the second
  extension can't starve). `open <file>` verb dispatches by type (scripts to
  the editor as source — never executes; md rendered; rest as text) with
  batch errorlevels. `json validate|pretty` (nesting/strings/escapes/
  numbers/literals, line/col errors, 2-space pretty, 64 KB budget).
- Editor: replace-all + case toggle (Ctrl+R/Ctrl+T, `All`/`Case` OSK keys,
  `\all`/`\c` serial; one undo snapshot, replacements never re-match);
  comment toggle (`rem ` batch incl. `::` strip, `// ` JSON,
  `<!-- -->` Markdown; Ctrl+/, `\co`); auto-indent on Enter; paren/`%var%`
  match jump (nesting-aware, skips strings/comments; Ctrl+B, `\b`); word-wrap
  toggle with explicit chunk rendering (cursor/selection/scroll/touch/page
  all follow visual rows; Ctrl+W, `\w`); `.bak` on save + reload (Ctrl+L,
  `\l`, worker-side) + FATFS read-only respect; status shows syntax/EOL/RO;
  undo/redo restores the dirty flag (clean undo quits without prompt);
  horizontal cursor follow for unwrapped long rows.
- Verified live: `open` matrix per type, `.cmd` run + launch listing, JSON
  validate/pretty/bad, comment-save round trip, `.bak` on disk, reload
  discard + clean quit, readonly save refusal (sticks dirty) + undo recovery,
  wrap toggle round trip. Regression: unit 218/0/2 (14 new tests), deep 8/8,
  db 38/38 (two O6 SD-tail flakes on import, clean after), alarm 25/25,
  smoke 21/21. ADVENT.BAT `echo ---` → `echo /raw ---` (sole batch line
  needing it); smoke log writer hardened to UTF-8.

### Markdown editing + rendering everywhere (2026-09-09 session, COM3)
- New `components/markdown/` (self-contained, no shell/batch deps): block
  parser (headings, nested lists, quotes, fences, HR, GFM tables with
  alignment, task lists) + flanking-safe inline spans (bold/italic/strike/
  code/links as `text (url)`) emitting ANSI SGR through the existing
  pipeline, so spans style it on screen and serial terminals render it
  natively. Tables measure display width (ASCII 1, CJK 2) for aligned
  columns. Auto-render is per-line and safe: `2 * 3`, `foo_bar`, `*ptr`
  pass through untouched (no closer, no style).
- `markdown <file> | -e <text> | on | off` verb; `view *.md` renders
  rendered-plain (`--raw` bypasses); `echo`/`type` auto-render per line
  (`echo /raw` bypasses, `markdown off` disables session-wide).
- Spans honor bold/italic/underline/strike via shared `font_span_style()`
  (transcript + editor + preview): DejaVuSansMono-Bold/-Oblique vendored
  TTF variants with bright fallback on stock colors, LVGL decor for
  underline/strike. Registry slot eviction under pressure (probe leftovers
  no longer read as "unknown font") + load-failure LOGW diagnostics.
- Editor `.md` highlighting (headings/code/links/markers) plus a read-only
  preview toggle on the same surface (USB Ctrl+P, touch `Prev`, serial
  `\p`; status shows PREVIEW; edits discarded in preview including the
  serial-text path; `rendering preview...` status covers the O6 SD-tail
  latency on first variant load). Header `**bold**` notification subset via
  recolor; TUI flush maps bold to bright.
- Proofs: unit 204/0/2 (7 markdown + 1 lexer test), deep 8/8, db 38/38,
  alarm 25/25, smoke 21/21; live `markdown`/`type`/`view`/preview-quit
  verified on HW with screenshots (real Bold/Oblique weights, CJK tables).
  Found live: preview-read-only hole on the serial path (fixed), slot
  exhaustion misreported as unknown font (fixed), ADVENT.BAT `echo ---`
  now `echo /raw ---` (only batch line needing it).

### Font follow-up — live refresh pass + shell block cursor (2026-09-09 session, COM3)
- Root-fixed the real stale-font bug: LVGL styles snapshot the chain pointer
  at creation and `font_set` flips to a new copy, so header/keyboard/input
  rendered the orphaned font until recreated (transcript only worked by
  accident — spans re-resolve per append). New narrow refresh pass instead
  of a full rebuild: `windows/keyboard/header/tui_refresh_fonts()` plus
  `editor_view_refresh_fonts()` (existing rebuild when open; open modals
  refresh on reopen), wired into `font set/size` success. Screenshot-proven:
  Montserrat keyboard live with no reopen.
- Shell input gets the editor-like block cursor (`LV_PART_CURSOR` full-cell
  fill + contrasting glyph, shared `P4_CONFIG_CURSOR_BLINK_MS` now driving
  both it and the editor timer — unifying 400/500 ms; 0 = steady with the
  editor timer correctly skipped). Cursor hides with its widget in
  editor/app/TUI modes (no extra work — verified). Home/End go direct
  (`set_cursor_pos`, dropping the N×left/right loops); USB Ctrl+Left/Right
  word-jump over codepoints (heap offset table, tear-safe; Ctrl bits mirrored
  locally since usb already requires shell — reverse edge would cycle).
- New `cursor [block|bar] [blink <ms|off>]` command (session-only,
  Tab-completion + help + `command.md`). USB HID stays ASCII-only
  (keycode map — documented limit).
- Regression: firmware+test 0/0, unit 196/0/2, deep 8/8 (one transient menu
  flake, clean rerun), db 38/38, alarm 25/25, smoke 21/21.

### Font milestone Phase 3 — SD TTFs, sizes, CJK fallback, theme prep (2026-09-09 session, COM3)
- Vendored `assets/fonts/` (DejaVuSansMono 340 KB, NotoSans-Regular 569 KB,
  NotoSansSC variable 17.7 MB + DejaVu/OFL licenses + SHA256SUMS) with
  `push_fonts.py` uploader. `receive` cap bumped 8→24 MB (`streaming` transfer
  with space pre-check; needed for the 17 MB SC).
- Loader: `LV_USE_TINY_TTF` + file support (bundled stb, no freetype) and an
  owned `F:` lv_fs driver over stdio/VFS (`font_fs.c`) — the in-tree FATFS
  driver would have needed a managed CMakeLists edit for ff.h. Registry:
  dynamic `sd:/FONTS/*` discovery, lazy load, (stem,size) slots with
  measured monospace probing, sizes 10–28 px via `lv_tiny_ttf_set_size`
  under the port lock, async-deferred destroy, SHELL.INI size keys.
- Default-font protection matrix verified live: SD-absent/corrupt/missing all
  keep current fonts with errorlevels; bitmap `size` cleanly refused;
  terminal refuses proportional fonts AND sizes breaking 80x25 (clamp uses
  keyboard-hidden height — the keyboard-shrunk rect wrongly vetoed the
  default 16 px during testing). Two registry bugs caught by the matrix:
  UI primary pointer not recorded (render right, names wrong) and refusal
  reasons misordered (new `font_is_monospace` gate first).
- CJK without selectable CJK: NotoSansSC auto-attaches as the tail of both
  chains (primary → Montserrat-copy → SC) at first SD mount and on every
  `font set`; sizes track roles; SD removal degrades to Montserrat tails.
  Full CJK renders (screenshot), DejaVu covers all symbols. Known stb limit,
  documented in `command.md`: it picks one cmap subtable, so U+2600/2601
  miss via the SC tail (DejaVu has them); very long CJK rows can
  wrap-mangle ~3 chars (short lines exact; coverage page split accordingly).
- Batch control: `font` verbs were already batch-safe; companion `SET.BAT`
  gains a Fonts submenu (terminal/UI/size/show/defaults + `[M-SET-FONT-SAVED]`;
  `deep_test` Back updated 8→9). Theme prep light: `theme_t` table,
  modal/keyboard literals centralized (zero visual change), `theme show`
  stub, THEME.INI format documented.
- Regression: firmware+test 0/0, unit 196/0/2, deep 8/8, db 38/38, alarm
  25/25, smoke 21/21, SET fonts walk PASS. Harness lesson: never
  `reset_input_buffer` right after a reset-adjacent open (ate the fast early
  unit tests), and drain with 64 KB reads (400 B caps overflow host RX).

### Font milestone Phase 1 — registry/roles, live switching, UTF-8 typing (2026-09-09 session, COM3)
- New `components/font/` registry (leaf component, no dependency edges):
  TERMINAL role (pure monospace) + UI role (RAM chain copy with Montserrat
  fallback), `font_init/list/set/restore`, monospace enforcement for
  terminal, double-buffered chain swap so LVGL never renders a torn struct.
  `P4_CONFIG_FONT_*` knobs + `p4minishell_config.yaml` `fonts:` section.
  All surfaces consume roles (others inherit via the existing windows
  wrappers); modal titles moved to UI.
- `font list|set <terminal|ui> <name> [/save]|size <px>`; `/save` persists to
  `sd:/APPS/SHELL.INI`, restored in `boot_on_sd_first_mount` (command_init
  time is too early — the SD mount is lazy). Live switch verified on HW
  (ui 17→16 px line height + screenshot in Montserrat chrome) and restore
  verified across reboot. `size` is an honest Phase-2 stub.
- UTF-8 typing end to end: OSK inserts full codepoints into the input line
  (`shell_utf8_decode` helper); key queue widened to 5-byte sequence items
  with `shell_key_wait_submit_utf8()`; UART producer reassembles split
  multibyte chunks (truncated tails preserved across reads); `set /p`
  round-trips `café` intact via new `shell_wait_for_key_utf8()` with
  codepoint-aware backspace. Legacy `char` API unchanged (lead byte for
  multibyte, safe for y/n/ESC compares). Verified: unit 196/0/2, deep 8/8,
  db 38/38 (one transient O6 SD-tail flake on import, clean on rerun),
  alarm 25/25, smoke 21/21. `command.md` documents the font verbs plus the
  `for /f` delims-space and `set /a` 32-bit gotchas found pressure-testing.

### Font milestone Phase 0 — chained UI font + `font` command (2026-09-09 session, COM3)
- Keyboard/header icon tofu fixed with a fallback chain, not a font swap:
  `windows_get_ui_font()` (`components/windows/windows.c`) is a RAM copy of
  unscii_16 with `.fallback = &lv_font_montserrat_14` (the only bundled font
  with the FontAwesome PUA subset). Keyboard, header, input line, and
  input-row buttons use it; transcript/TUI stay on the pure terminal font so
  cell metrics are exact. Scroll buttons upgraded to LV_SYMBOL_UP/DOWN icons
  per the original code comment's intent. Screenshot-verified (all icons
  render, zero tofu), deep 8/8 green.
- Crash lesson, caught pre-ship: a callback-delegating wrapper font
  stack-overflowed taskLVGL — `lv_font_get_glyph_dsc()` records
  `resolved_font` = the struct it was called on, so the wrapper was recorded
  and its `get_glyph_bitmap` recursed into itself. Rule: chain via
  `.fallback` on a RAM struct copy, never via resolving callbacks.
- New `font` verb (`components/command/font_commands.c`): `font info`
  (roles, line heights, fallback state) and `font coverage` (labeled glyph
  page: `[have]` ASCII/box/U+2600, `[want:P2]` currency/arrows/CJK as
  expected tofu until SD TTFs). Wired into dispatch, Tab completion, `help`,
  and `command.md`. Cross-component font sharing uses `extern` decls, not
  CMake edges (`windows` already REQUIRES keyboard+header — a cycle).

### P4 reference apps + O3 verdict + driver hardening (2026-09-09 session, COM3)
- Shipped the three Phase-5 reference apps (all pure batch, verified on board
  via new `apps/smoke_apps.py`, **21/21 PASS**): `apps/adventure/ADVENT.BAT`
  (7 rooms, inventory/score/save-load/win path), `apps/notes/NOTES.BAT`
  (Zettelkasten on `db`: categories, `for /f` over `db find /b`, edit-file
  bodies, export/import), `apps/mood/MOOD.BAT` (live heap→RGB/tone dashboard
  parsing `mem` with `for /f`); each with `.APPINFO` + merged `ALIASES.BAT`
  (all five app aliases in every copy — separate pushes used to clobber the
  SD-root file) and one `apps/push_apps.py` uploader.
- Batch engine: labels past the 32-per-file table were silently dropped, so a
  33-label game lost its quit path with only `goto: label not found` as a
  clue. `shell_scan_batch_labels()` now warns once per file
  (`batch: too many labels (max 32)`, live-verified with a 33-label probe);
  `command.md` documents the cap.
- Test drivers: `shell_up` in deep/db/alarm required only a substring, so it
  false-passed on the submit-side input echo while the worker was wedged; all
  three plus both `act()` barriers now require the worker's own output line
  (`worker_line`), and all matchers tolerate prompt-glue (trailing prompt +
  output sharing one serial line — normal under load). `push_sd`/`push_apps`
  note: `wait_shell` keeps the old substring check (failures there only cost
  retries; CRC guards integrity).
- `command.md` gotchas found pressure-testing: spaces terminate `for /f`
  option values (no space in custom `delims=`; `apps/mood` parses with
  `delims==,b`), and `set /a` is 32-bit (divide heap-byte values before
  multiplying).
- O3 verdict: P3 deferral holds (drain 19–29 KB/s both runs); 30-min
  `stall_catch.py` (740 probes, echo-vs-output timing) shows zero queue-fulls,
  zero heap warnings, db/sd at 0.0–0.2 s — the worker is exonerated, residual
  is rare single-output loss under TX pressure plus an SD-op latency tail
  (new O6: sync unlink/read occasionally >60 s under shared-bus contention;
  hot-loop deletes removed from the reference apps). See `bugs.md` O3/O6.
- Regression this session: `deep_test` 8/8, `db_test` 38/38, `alarm_test`
  25/25, `smoke_apps` 21/21 (firmware: transcript deferral + buffer mutex +
  label warning; unit suite still 196/0/2 from the pre-warning build — the
  warning is 5 lines in the label scanner, verified live instead).

### P2 config hygiene (2026-09-08 session, verified on board)
- Pruned ~40 dead `P4_CONFIG_*` macros (all verified 0 references across
  components/main/test): 10 `PS_COLOR_*` knobs (palette lives in
  `ansi_palette.h`), `WINDOW_KEYBOARD_HEIGHT_*` trio (dup of `KEYBOARD_HEIGHT_*`),
  6 dead TUI knobs, `KEYBOARD_DEFAULT_VISIBLE`/`KEYBOARD_TAG`,
  `DISPLAY_DEFAULT_ROTATION`/`POWER`/`REFRESH_DYNAMIC`, 6 screenshot knobs
  (live code uses `BMP_MAGIC` + snapshot geometry), 3 USB keyboard knobs
  (behavior unconditional), `BOOT_MAX_GPIO_LINES`, `BT_HOSTED_RUNTIME_SUPPORTED`,
  `STORAGE_VOLUME_MAX`, `TRASH`/`FORMAT_CONFIRM_WORD` aliases (single
  `DESTRUCTIVE_CONFIRM_WORD` kept; fixed the `storage_commands.h` comment),
  `DB_CATEGORY_LABEL_BYTES` (labels are caller-buffered), `GPIO_NAME_BYTES`/
  `GPIO_PIN_LIMIT` (pointer-based pin table), and the `WIFI_DEFAULT_*` wrappers
  (Kconfig options + direct `CONFIG_` use kept).
- Wired two live knobs to their literals: `HEADER_BAT_LOW_PCT` (both 15s in
  `header.c`) and kept `WIFI_DEFAULT` Kconfig surface intact.
- Synced `p4minishell_config.yaml` (deleted mirrors of every pruned macro;
  `bat_low_pct` kept; `default_ssid/password` kept as Kconfig surface).
- Corrected `ai-context.md` alt-screen rule (unconditional, macro deleted).

### P3 O3 investigation (2026-09-08 session, COM3)
- CONFIRMED mechanism: per-append transcript span rebuilds go O(buffer).
  `shell_transcript_append_internal()` re-parses the whole ANSI buffer and
  rebuilds all spans + layout on every append (only the background async path
  coalesces). `tools/transcript_drain_bench.py` measures it: 1.3 MB `type`
  drains 10 KB @ 3.2 KB/s decaying to 100 KB @ 0.9 KB/s on first run, and a
  back-to-back second run over the SAME file starts 3x slower (10 KB @
  1.0 KB/s) — ruling out SD/fragmentation/USB-host causes. At a full 64 KB
  buffer every printed line costs ~1 s, so sustained output occupies the
  single worker and submissions pile 16-deep into `queue full` drops.
- Eliminated along the way: LVGL-lock contention (queue-full errors print
  through the transcript path, proving the lock free), modal/key-wait stuck
  paths, heap pressure (93%/58 KB both states), Wi-Fi event storms (quiet
  when measured), host-USB artifacts (controlled with DTR-safe opens).
- Fix direction (needs its own LVGL-careful work item, M39 locks!): batch
  worker-path label updates per command (dirty flag + flush at segment end /
  prompt repaint, keeping per-line UART mirror + explicit progress flushes),
  or incremental tail-span appends. Added `tools/stall_catch.py` (mixed
  SD/non-SD output-anchored probes with queue-full + mount accounting) for
  future episodes. See `bugs.md` O3.

### Added — batch loader depth (batch-first route)
- `for /f ... in ('command')` (and backquotes with `usebackq`): runs the inner command through the re-entrant redirection capture and iterates its output with the shared skip/eol/delims/tokens processor (`shell_forf_is_command_set` + `shell_forf_run_command`, pure-helper unit tests).
- `%~[fdpnx]N` argument modifiers (`shell_arg_apply_modifiers`, pure-helper unit tests; `d` is empty on FATFS, `s` accepted/ignored); also fixes `LIB.BAT` `:tui_header`/`:status`, which already used `%~1`.
- `LIB.BAT::selftest_*` on-board contract checks (`tilde`, `forf_cmd`, `defined`, `all`), driven by a new deterministic `run_companion.py selftest` scenario.

### Fixed — verbs and wiring lost in the v0.35.3-v0.35.6 splits (all verified on board)
- Restored `launch` (heap discovery over PATH + `sd:/APPS`, APPINFO titles, `/list`, `<name> [args]`, bounded menu, 0/1/2), `apps` (`app_get` loop), `delay` (clamped to `P4_CONFIG_DELAY_MAX_MS`, 0/2), `notify`/`gfind` dispatcher arms, the `app_dispatch` tail hook (never shadows built-ins/batch), and the `applib_env_ops` registration (`hello` shows real cwd/PATH again); completion table + both `help` surfaces updated.
- Test project: pinned `CONFIG_LV_FONT_UNSCII_16` in `test/sdkconfig.defaults` and regenerated the stale committed `test/sdkconfig`; re-pointed both `dependencies.lock` files at this checkout.
- Companion: corrected every submenu `list` errorlevel chain to 0-based indices; added serial-visible echo markers for the reactive driver.
- Modal runtime: PSRAM event group restored via `xEventGroupCreateStatic` (`xEventGroupCreateWithCaps` does not exist in IDF 5.5.5), internal fallback.
- Drivers: DTR-safe opens everywhere, SD-mount gate in `hw_reset`, quiesce + retry in `push_sd.py`, Stream tail-preserving waits, single-send + verify + retry selections, barrier-synced `db_test`/`alarm_test`.

### Verification (this session)
- `idf.py build` 0 errors / 0 warnings (firmware + test projects).
- Unit runner **196 pass / 0 fail / 2 ignore** (tmpfile guards).
- `deep_test.py` **8/8 PASS**, `db_test.py` 38/38, `alarm_test.py` 25/25, serial sweeps clean (see `bugs.md` M41-M46, O3-O5 for the full story incl. open items).

---

## [0.35.7] - 2026-09-06 — Hardware bring-up fixes (all verified on board unless noted)

### Fixed — box-drawing tofu (M40)

- **Symptom:** TUI boxes rendered as placeholder boxes on hardware (screenshot-verified), although the extended `unscii_16` font is in-tree.
- **Root causes (three stacked):** (1) the generated `sdkconfig` had lost `CONFIG_LV_FONT_UNSCII_16` (stale generated file) so `windows_get_terminal_font()` fell back to ASCII Montserrat; (2) the Kconfig symbol exists, so the fix is regenerating config + using the font unconditionally; (3) once enabled, the in-place font extension failed to compile — two missing commas (bitmap line 475, glyph-dsc line 2115), latent since the file previously compiled to empty behind the disabled guard.
- **Fix:** unconditional `lv_font_unscii_16` use with extern decl (`windows.c`), dead-symbol note in `sdkconfig.defaults`, two commas, regenerated `sdkconfig`. Box glyphs verified pixel-perfect via screenshot.

### Fixed — httpd start failed ESP_ERR_HTTPD_TASK (needs Wi-Fi to fully verify)

- **Symptom:** manual + auto `httpd start` failed with `ESP_ERR_HTTPD_TASK`.
- **Root cause:** 16 KB server-task stack (4x IDF default, unjustified) cannot allocate from fragmented internal heap at Wi-Fi-up time.
- **Fix:** `P4_CONFIG_HTTPD_STACK_BYTES` 16384→8192 (handler frame <1 KB; verify HeadB watermark via `ps` on hardware with Wi-Fi up). Yaml already said 8192 — header had drifted.

### Fixed — command queue drops

- **Symptom:** `shell: command queue full, command dropped` under bursts.
- **Root cause:** depth had regressed to 4 with zero-timeout submit (v0.33.0 M10 fix lost).
- **Fix:** new `P4_CONFIG_COMMAND_QUEUE_DEPTH` 16 + `P4_CONFIG_COMMAND_QUEUE_SEND_TIMEOUT_MS` 500 bounded wait (submit runs on LVGL/UART tasks only). Verified 20/20 burst, no drops.

### Fixed — NUL bytes in source

- **`batch.c` contained 3 literal NUL bytes** (`'\x00'` instead of `'\0'` escapes, ansi SGR parsing) from a past scripted edit — compiled with `-Wnull-character` warnings, now errors-or-clean. Replaced, repo-wide NUL sweep clean.

### Fixed — alarm checker never started (M38)

- **Symptom:** `alarm status` showed `checker stopped`, `catchup pending`; fire test failed; nothing ever fired.
- **Root cause:** `alarm_init()` + `alarm_register_host_ops()` were never called (header claims `command_init()` does it; it didn't).
- **Fix + lesson:** first wired into `command_init()`, which broke USB HCD bring-up on hardware (checker task fragments DMA heap before `usb_init()` — verified by A/B flash). Final fix: ops stay registered in `command_init()`, `alarm_init()` runs lazily on first `alarm`/`cal` command. Verified `checker running`, `catchup done`, alarm suite 25/25.

### Fixed — ask serial answer clobbered (M37)

- **Symptom:** `ask` via serial logged `ask serial line: 'blue'` but `ASK_RESULT` stayed empty.
- **Root cause:** `ask_surface_close()` unconditionally copied the (empty) on-screen textarea over the serial-provided answer.
- **Fix:** `serial_answered` flag; close only captures textarea when no serial answer arrived. Verified `RESULT=[blue]` on hardware. `dialog`/`list` handlers set results directly — unaffected.

### Fixed — modal LVGL layout-loop watchdog (M39)

- **Symptom:** running `COMPANION.BAT` → `list` modal hung `shell_cmd` in `lv_obj_update_layout` → task watchdog abort + backtrace (decoded via addr2line against the ELF).
- **Root cause:** all six modal open functions called `windows_refresh_editor_surface()` AFTER `lvgl_port_unlock()` — layout raced the render task.
- **Fix:** all six refreshes moved inside the locked region (`fb_refresh_list` takes the lock itself and stays outside). Companion `list` select verified working (dashboard geometry transition); editor paths ride LVGL-task affinity by design, untouched.

### Fixed — test-suite findings (suite now 193 pass / 0 fail / 2 ignore)

- **Extract-lock crash (mine):** v0.35.5 `lvgl_port_lock` in `shell_extract_input_text` fired before the NULL-widget guard → assert/abort in the LVGL-less test app. Fixed order (widget check first, matching project convention); suite runs to completion.
- **Copy off-by-one:** `shell_clipboard_copy_transcript` counted a trailing newline as a line (`clip copy 2` returned 1 line). Skip-one-trailing-newline fix; new test covers it.
- **DEG guard + float:** `DEG()` rejected bare 3-letter calls (`name[3] != '\0'` guard, no `DEG$` exists) and `deg(45.30)` hit the `floor(29.9999999)` pitfall → integer-centi-units math via `llround`.
- **Stale caret test:** predated the v0.33.0 undefined→empty rule; updated to current semantics.
- **64-bit asserts:** two alarm tests used `INT64` asserts without Unity 64-bit support → `INT32` (time_t is 32-bit here).
- **2 ignores by design:** history-file format tests skip loudly when `tmpfile` is unavailable (ESP-IDF newlib).

### Verification (hardware session 2026-09-06)

- `idf.py build` 0 errors / 0 warnings (after fixes above).
- Unit runner **193 pass / 0 fail / 2 ignore** (tmpfile guards), no panic.
- Serial sweep 18/18 (calc/draw/modals/audio/periph/SD), SD read/write cycle, ask serial `RESULT=[blue]`, alarm 25/25, db 38/38, burst 20/20, screenshots pixel-verified (tofu→glyphs).
- Open: httpd start needs a connected Wi-Fi to verify (AP visible, stored credentials rejected); `run_companion.py` text triggers are stale vs TUI BATs (firmware flow verified manually instead); BSOD watch 30 min clean, under investigation.

---

## [0.35.6] - 2026-09-05 — Split patch (storage + batch-expr out; shell stays whole)

### Changed — `storage_commands.c` 6147→41 lines (residual header+includes), `batch.c` 4881→4072 lines (verbatim moves, no behavior change)

- **New `components/storage/storage_nav.c`** (1122 lines): `cd`/`dir`/`tree`.
- **New `components/storage/storage_files.c`** (1690 lines): manipulation + `attrib`/`label`/`xcopy`.
- **New `components/storage/storage_disk.c`** (546 lines): `chkdsk`/`format` + destructive-confirm helper.
- **New `components/storage/storage_text.c`** (1948 lines): `find`/`more`/`fc`/`sort`/`findstr`/`comp`.
- **New `components/storage/storage_fam.c`** (768 lines): `sd` + `disk` families.
- **Shared helpers promoted** (were file-static, used across the new files): `shell_parse_alloc_unit` + `shell_format_execute` (disk/fam), `shell_dir_format_stamp` (nav/text), `shell_find_parse_date` (text/files) — declared in `storage_commands.h`; shared `SHELL_*` aliases moved there too.
- **New `components/batch/batch_expr.c`** (809 lines): integer-expr evaluator + `set /a`/`set /p` helpers out of `batch.c`. The 19 direct `s_errorlevel` writes became `batch_set_errorlevel()` calls (same state); the two helpers are exported for `shell_command_set()`. Wider batch split stopped per stop-rule (engine/verb statics porous: `shell_batch_run_internal`, setlocal stack, appmode state shared both ways).
- **`shell.c` intentionally not split**: static-sharing audit shows the `s_command_ops` table hub + shared transcript pointers in 7+ sections; splitting would scatter state instead of containing it. Fully-private sections total ~420 lines — not worth the churn.

### Verification

- Static checks only in this patch: function inventories HEAD vs work (zero lost/zero new per file), cross-file static analysis clean after 4 promotions, per-file line endings preserved (`batch.c` CRLF restored), `SRCS` updated. Full `idf.py build` + runner + COM11 deferred to the hardware session.

---

## [0.35.5] - 2026-09-05 — Hardening patch (truncation/OOB, OOM immediacy, tests, LVGL locks)

### Fixed — truncation / OOB (fail closed, fitting inputs unchanged)

- **`tui_flush` recolor buffer** (`components/tui/tui.c:382,401`): clamp `pos` on `snprintf` truncation + final NUL guard (was `size_t` underflow → OOB write past `cap`).
- **Hexview pager** (`components/modal/modal_surf.c:908-917`): same clamp at all three `snprintf` sites.
- **`tree` child paths** (`components/storage/storage_commands.c:1029,1086`): explicit truncation checks — skip the entry / mark truncated instead of stating or descending into a cut path.
- **`trash` unique names** (`components/storage/trash.c:180-184`): skip truncated candidates (never test a cut name for uniqueness).
- **`db` path builders** (`components/db/db.c:62-79`): fail-closed empty string + `ESP_LOGE` on truncation (existing `DB_TAG` pattern, no new includes).
- **Verified safe, no change:** `tree names[]` (equal 256B cells), db fixed-field copies (equal-size/`%.*s`), UART submit (assembler caps at 4095), windows fragment (use already NULL-guarded), dir display cells (deliberate clipping).

### Fixed — OOM immediacy (NULL checked before use, success paths unchanged)

- **Dispatch snapshots** (`components/command/command.c:1064-1117`): `want_family`/`want_echo` restructure with a single OOM guard (was 8 unchecked `strdup`s flowing into family handlers).
- **Async submit** (`components/command/command.c:2244`): validate args/queue before allocating (was allocate-then-check).
- **`sd` dispatcher** (`components/storage/storage_commands.c:5730`), **`chkdsk` subdirs** (warns instead of silently skipping levels), **shell repair order** + **shell init early-return** (`components/shell/shell.c:1548,4475` — was NULL-deref at `s_transcript[0]`; `s_initialized` stays false), **clipboard text** (`components/command/command.c:329` — 2048B stack local → heap, the last ≥1KB stack local on the dispatch path).

### Added — 22 unit tests (pure logic; HW stays on board)

- **`test_modal.c`** (5): new shared `modal_parse_timeout_arg`/`modal_parse_var_arg` (`components/modal/modal_surf.h`) replacing 9 copy-pasted `/t:`/`/v:` parses in `tui_commands.c` — dedup + tests in one move.
- **`test_power.c`** (4): `shell_power_parse_seconds` + `shell_power_wake_cause_string` promoted to `command.h`.
- **`test_serial.c`** (2): `screenshot_write_bmp_headers` promoted to `command.h` (magic/size/dims/bpp/DPI).
- **`test_tui.c`** (4): default-colour round-trip, cursor save/restore, inactive defaults (draw/flush need LVGL — stay board-verified).
- **`test_clipboard.c`** (4): set/get/file-flag/copy-transcript (RAM buffer works headless).
- **`test_history_file.c`** (3): new `shell_history_save_lines`/`shell_history_load_lines` (`components/shell/shell.h`, extracted from `history /save|/load`; `tmpfile`, skipped loudly if unavailable).
- Wired in `test/main/CMakeLists.txt` (+ `modal`/`tui` includes/deps) and `test_main.c`.

### Fixed — LVGL task affinity (narrow, no blanket locking)

- **Transcript-apply fallback** (`components/windows/windows.c:779`): takes `lvgl_port_lock(0)` — the doc comment always claimed the fallback held it, the code did not (recursive mutex nests safely).
- **`shell_extract_input_text`** (`components/shell/shell.c:1554`): recursive-lock the textarea read (both callers are LVGL-task today; now safe from any task).
- **Audited, no change:** header timer/init calls (LVGL-task context), USB inject path (async-dispatched), ops tables (NULL-checked throughout per prior audit).

### Verification (hardware session 2026-09-05, all green)

- `idf.py build` 0 errors / 0 warnings (firmware + test projects).
- Unit runner: **180 tests, 0 failures, 0 skips** (158 existing + 22 new).
- COM11 board pass clean: boot banner, `tui status` rect, draw/modal/audio/peripheral/serial surfaces, companion BATs, mixed SD sweep with zero `allocate_dma_buf` errors.

---

## [0.35.4] - 2026-09-05 — Split patch (periph/power/serial out of `command.c`)

### Changed — `command.c` 5808→2467 lines (verbatim moves, no behavior change)

- **New `components/command/periph_commands.c`** (1527 lines): peripheral toolkit `gpio`/`pwm`/`freq`/`adc`/`i2c`/`spi`/`rgb`/`camera` with the board GPIO table, pin-safety gate, and toolkit-exclusive `SHELL_*` compat defines, declared in `command.h` (new PERIPH section). The `SHELL_PWM/ADC/I2C/SPI_*` defines in `command.c` were orphaned (zero remaining users) and moved along.
- **New `components/command/power_commands.c`** (897 lines): `brightness`/`rotate`/`battery`/`power`/`sleep`/`deepsleep` with the battery ADC state, idle display-off state, and power-exclusive defines, declared in `command.h` (new POWER section). The ops-table backings (`command_battery_read`, `shell_power_*` API) live there too; `command_init()` registers them unchanged.
- **New `components/command/serial_commands.c`** (1015 lines): `screenshot`/`receive`/`send` with the BMP/frame/diag helpers, declared in `command.h` (new SERIAL section). `shell_print_http_body` + HTTPGET section stay in `command.c` — the httpget arm is inline code inside `shell_execute_command_core()` and cannot move.
- **Line endings preserved** (`command.c` LF); diff shows only the moves (function inventory HEAD vs work: zero lost, zero new).

### Verification

- Static checks only in this patch: each moved verb defined exactly once, call sites resolve via `command.h`, no new layering (power needs `networking.h` for the Wi-Fi teardown hook, as before). Full `idf.py build` + COM11 pass deferred to the hardware session.

---

## [0.35.3] - 2026-09-05 — Split patch (audio/tui commands out of the god files, reconcile, managed shim)

### Changed — `command.c` 6394→5808 lines, `batch.c` 5195→4881 lines (verbatim moves, no behavior change)

- **New `components/command/audio_commands.c`** (181 lines): `volume`/`beep`/`tone`/`wavplay`/`audio` moved out of `command.c` (`command.c:1137-1293`), declared in `command.h` (new AUDIO section), following the `db_commands.c` / `alarm_commands.c` split. The dispatcher calls them; it never implements them.
- **New `components/command/tui_commands.c`** (756 lines, 12 verbs): `draw`/`anchor`/`browse` moved out of `command.c`, `dialog`/`list`/`ask`/`browse_batch`/`view`/`hexview`/`color`/`locate`/`tui` moved out of the batch engine (`batch.c:3476-3785`), declared in `command.h` (new TUI section). `appmode` stays in `batch.c` (batch-frame cleanup); `temp`/`ansi`/`menu`/`notify` stay (batch language). `batch.c` drops its `modal_surf.h`/`tui.h`/`windows.h` includes.
- **Line endings preserved per file** (`batch.c` stays CRLF like the repo, `command/` stays LF) so the diffs show only the moves.

### Fixed — reconcile (docs follow code)

- **Memory baseline M33** (`bugs.md`): current tree runs transcript 65536 (`p4minishell_config.h:93`, PSRAM), recolor == transcript (`p4minishell_config.h:101`), trim 4096 (`p4minishell_config.h:120`), async 512 (`p4minishell_config.h:137`), SD DMA 4096 (`p4minishell_config.h:634`), worker stack 32768 (`p4minishell_config.h:1522`); M19/M31 kept as the 0.35.1 history.
- **Stale codec ref fixed** (`bugs.md` M20, `changelog.md` 0.35.1 audio entry): guards are `esp32_p4_function_ev_board.c:305-380` + `audio_ensure_speaker()` `audio.c:72-93`, not `esp_codec_dev.c:269`.

### Added — managed re-apply shim

- **`tools/reapply_managed_patches.ps1`** (+ `tools/managed_patches.patch` backup): `--check` dry-run / apply after `idf.py update-dependencies`; `tools/README.md` documents it.

### Verification

- Static checks only in this patch: each moved verb defined exactly once (`draw`/`anchor`/`browse`/`dialog`/`volume`/`tui`), call sites resolve via `command.h`, no new layering (command→batch include pre-exists), `EXTRA_COMPONENT_DIRS` unchanged (new files ride the existing `command` entry). Full `idf.py build` + COM11 pass deferred to the hardware session.

---

## [0.35.2] - 2026-09-05 — Cleanup patch (root quarantine, browse dedup, layering, build/config drift)

### Removed — root one-shot scripts quarantined

- **97 root `*.py` helpers + 7 `*.txt` dumps removed from the root.** The 16 serial/screenshot/SD harnesses (`grab_screenshot.py`, `capture_tui.py`, `get_screenshot.py`, `save_to_sd.py`, `httpd_start_test.py`, `test_wifi*.py`, `wifi_*.py`, `verify.py`, `final_verify.py`, `final_check.py`) moved to `tools/harness/`; the 70 `add_*`/`apply_*`/`fix_*`/`check_*`/`find_*` one-shot patch scripts and the `extend_*`/`replace_*`/`extract_*`/`write_windows.py`/`list_surfaces.py` helpers were deleted (none referenced by docs, `CMakeLists.txt`, or configs). The 7 scratch dumps (`ansi_func.txt`, `box_chars.txt`, `draw_anchor_fixed.txt`, `draw_anchor_impl.txt`, `found.txt`, `windows_h.txt`, `windows_h_excerpt.txt`) were deleted. `apps/companion/push_sd.py` stays with the companion app.

### Fixed — browse duplication and stack literals

- **`browse` verb unified** (`components/command/command.c:2413` + `components/batch/batch.c:3610`): the interactive `shell_command_browse` now uses the same `P4_CONFIG_TUI_BROWSE_PATH_BYTES` buffer and the same `"Browse"` title as `shell_command_browse_batch` (was `char selected_path[4096]` + `"File Browser"`), so the 4 KB worker-stack local and the title divergence are gone. The `/v:` dispatch at `components/command/command.c:5376` is unchanged.
- **`shell_launch_app` heap buffer** (`components/command/command.c:2434`): the `char command[256]` stack local is now a `malloc(P4_CONFIG_COMMAND_BYTES)` buffer, so long app names cannot truncate and the worker stack stays small.
- **`gfind` bounds via config** (`components/command/gfind_commands.c:30-32`): `GFIND_DB_MAX`/`GFIND_ALARM_MAX` are now `P4_CONFIG_DB_MAX_DATABASES`/`P4_CONFIG_ALARM_MAX_EVENTS` instead of comment-matched literals.

### Fixed — layering and build configuration

- **Shell layering** (`components/shell/CMakeLists.txt`): dropped `networking`, `c6ota`, `p4_usb` from `REQUIRES` (the shell core reaches those only through `shell_command_ops_t`, `components/shell/shell.h:50`); removes the `shell` → `networking` → `storage` → `shell` build cycle.
- **Root build list** (`CMakeLists.txt:5`): `EXTRA_COMPONENT_DIRS` now lists all 23 local components (added `alarm`, `db`, `c6ota`, `editor`, `header`, `led`, `networking`, `usb`, `p4_usb`).
- **Config drift** (`p4minishell_config.yaml`): added `ps_color_parameter`/`ps_color_subsystem`/`ps_color_heading` and `db_flag_secret` to match `p4minishell_config.h:1390,1402,1411,1660`.
- **Build constraints** (`sdkconfig.defaults`): pinned LVGL demos/examples off; `.gitignore` now covers `sdkconfig` per `ai-context.md`.

### Verification

- Static checks only in this patch: no `char [4096]`/`[2048]` stack locals remain in `components/command/command.c`, `GFIND_*` resolve to `P4_CONFIG_*`, root holds 0 `*.py` scratch files with 16 harnesses in `tools/harness/`, and `EXTRA_COMPONENT_DIRS` covers all 23 components. Full `idf.py build` + board re-verification deferred to the next hardware pass.

---

## [0.35.1] - 2026-08-26 — Hardware Testing patch (0.35.0→0.35.1: flash to COM11, extensive serial tests, companion TUI expansion, stack overflow fix at 0x4012b75a)

### Hardware Testing — flash, boot, and serial verification

- **Flash to COM11 succeeded, boot verified** (`P4MiniShell v0.35.1 ready`, SD mounted, header/battery/CPU live, transcript rect `1024x510` `80×25` `p4minishell_config.h:298` via `tui status`), **extensive serial tests run** over USB-Serial-JTAG covering every TUI surface (`draw box` single `SH_BOX_TL`/`H`/`V` double `SH_BOX_TL2`/`H2`/`V2` rounded `SH_BOX_TLR`/`TRR`/`BLR`/`BRR` with title via `tui_draw_box` `components/tui/tui.c:228` / `tui_cell_set` `components/tui/tui.c:116` `utf8[4]` `components/tui/tui.h:35`, `draw line`/`fill`/`text`/`clear`/`window`/`close`/`refresh`/`fullscreen`, `draw fullscreen on|off` (global) + `tui fullscreen on|off` (per-app) header kept visible by default `windows_enter_tui_mode` hidden only on fullscreen `windows_set_fullscreen`/`header_set_visible` `components/windows/windows.c:418` / `tui_enter_fullscreen` `components/tui/tui.c:417` dynamic `windows_notify_keyboard_visibility` → `windows_refresh_tui_surface`, TUI does not overlap shell text `tui_hide_for_modal`, `color`/`locate` TUI-aware, `tui_flush` recolor `#RRGGBB` per fg run `ansi_get_palette_color`, prompt `shell_prompt_render_plain()` `main.c:112` + `keyboard_bind_textarea` `components/modal/modal_surf.c:412` situational `SH_PROMPT`, screenshot `grab_screenshot.py --port/--out/--crop-transcript` + `capture_tui.py`), ANSI colour, audio, fullscreen — no panic, no `abort`, no watchdog, no LVGL assert, no overlap. **Companion fully TUI-expanded and hardware-verified** (see below).

### Added — extended unscii_16 font in-place (384 glyphs, no duplication)

- **Extended `unscii_16` font in-place** (`managed_components/lvgl__lvgl/src/font/lv_font_unscii_16.c`, cmaps 3, `sdkconfig.defaults:33` `CONFIG_LV_FONT_UNSCII_16=y`) — adds box-drawing U+2500-U+257F (128 glyphs) and symbols U+2600-U+26FF (256 glyphs), 384 glyphs total, no duplication (previously the `-r` range duplicated `0x2500-0x257F,0x2600-0x26FF` twice). Enables hardware box rendering via `SH_BOX_*` UTF-8 sequences through `tui_cell_t utf8[4]`.

### Fixed — TUI cell truncation and box glyph rendering

- **Fixed `tui_cell_t` to `utf8[4]`** (`components/tui/tui.h:35` `char utf8[4]`, `components/tui/tui.c:116` `tui_cell_set` via `strncpy` with NUL) — was `utf8[2]` truncating 3-byte box UTF-8 (e.g. `─` `0xE2 0x94 0x80`). Now holds full UTF-8 `SH_BOX_*`.
- **Fixed `tui_draw_box`/`tui_draw_line` to honor style and title** (`components/tui/tui.c:228` `tui_draw_box`, `components/tui/tui.c:283` `tui_draw_line` via `tui_cell_set`) — `draw box 2 2 20 8 single|double|rounded [title]` and `draw line` now select `SH_BOX_TL`/`TR`/`BL`/`BR`/`H`/`V` vs `SH_BOX_TL2`/`H2`/`V2` vs `SH_BOX_TLR`/`TRR`/`BLR`/`BRR` and center title with surrounding spaces; interior cleared correctly.

### Fixed — TUI color rendering via LVGL recolor per fg run

- **Fixed `tui_flush` to render fg/bg via `lv_label` recolor** (`components/tui/tui.c:356` `tui_flush`) — coalesces cells by fg, emits `#RRGGBB ` prefix per run using `ansi_get_palette_color` (`components/ansi/ansi.c`) PowerShell palette (no duplicate palette), wraps run text and `#` suffix; rows joined by `\n`, set via `lv_label_set_text` under `lvgl_port_lock`. Default fg 16 emits no tag.

### Added — fullscreen (global draw + per-app tui)

- **Implemented `draw fullscreen on|off` (global) and `tui fullscreen on|off` (per-app)** (`components/tui/tui.c:417` `tui_enter_fullscreen`/`tui_exit_fullscreen`, `components/windows/windows.c:418` `windows_set_fullscreen`/`header_set_visible`) — header hidden completely when fullscreen, kept visible by default otherwise. `tui status` reports `fullscreen` state. Dynamic keyboard scaling via `windows_notify_keyboard_visibility` (`components/windows/windows.c:312`) keeps TUI sized to transcript rect (`1024x510`) while header is hidden.

### Fixed — prompt in all inputs

- **Fixed prompt in all inputs** (`main/main.c:112` echo `SHELL_PROMPT` → `shell_prompt_render_plain()` `components/shell/shell.c:412`, `components/modal/modal_surf.c:412` `ask` placeholder `shell_prompt_render_plain()` + `keyboard_bind_textarea`, `components/shell/shell.c:298` situational color via `SH_PROMPT`) — shell input line, `ask` modal, and serial echo now show the DOS prompt template honoring `PROMPT=` (`$p $g` etc).

### Added — screenshot debug loop

- **Screenshot debug loop** (`grab_screenshot.py --port COM11 --out out.png --crop-transcript` + `capture_tui.py`, `components/tui/tui.c:356` `tui_flush` recolor, `components/windows/windows.c:312` transcript rect) — `tui status` shows `rect 1024x510 cols 80 rows 25`; `grab_screenshot.py` crops to transcript region for pixel-perfect TUI verification.

### Fixed — memory pressure (transcript / async / SD DMA / internal trim)

- **`P4_CONFIG_TRANSCRIPT_BYTES` 2048→1024** (`p4minishell_config.h:93` + `p4minishell_config.yaml` `buffers:transcript_bytes`) — halves the span-group ceiling and its internal-RAM span overhead after TUI modal restoration.
- **`P4_CONFIG_ASYNC_TRANSCRIPT_BYTES` 1024→512** (`p4minishell_config.h:134` + `p4minishell_config.yaml` `buffers:async_transcript_bytes`) — halves the background-task staging buffer; drain is heap-allocated, flush dispatches via `shell_schedule_transcript_appendf_ansi` when ESC present.
- **`P4_CONFIG_SD_DMA_BUFFER_BYTES` 8192→4096** (`p4minishell_config.h:626` + `p4minishell_config.yaml` `sd_dma_buffer_bytes`) — 8 sectors cached on `card->host.dma_aligned_buffer` at mount; lower permanent internal-RAM reservation while keeping `storage_sd_ensure_dma_buffer()` reuse.
- **`P4_CONFIG_TRANSCRIPT_INTERNAL_TRIM_BYTES` 49152→60000 with 1/4 keep** (`p4minishell_config.h:117` + `p4minishell_config.yaml` `transcript_internal_trim_bytes`) — trim earlier under TUI pressure; `shell_transcript_guard_internal()` / `windows_transcript_trim()` now keep 1/4 on trim and guard trim-below-10KB (prevents stdio FILE-lock `abort()` on low internal heap). Guard called at command start and before every append under LVGL lock.
- **`P4_CONFIG_COMMAND_TASK_STACK` 16384→24576** (`p4minishell_config.h:1514` + `p4minishell_config.yaml` `command_task_stack`) — raised due to TUI companion overflow at `0x4012b75a` (stack protection fault in `shell_cmd` when `COMPANION.BAT` pushed deep `draw` + `tui fullscreen` + `list`/`dialog` nesting); `P4_CONFIG_COMMAND_TASK_STACK` 12288→16384 in v0.33.0 was insufficient for the expanded TUI batch depth. Command queue full handling improved (bounded 1 s wait instead of silent drop).

### Fixed — audio abort in managed BSP

- **Managed BSP audio abort fixed** (ref corrected in v0.35.3: guards are `managed_components/espressif__esp32_p4_function_ev_board/esp32_p4_function_ev_board.c:305-380` + `audio_ensure_speaker()` `components/audio/audio.c:72-93`, backed up in `tools/managed_patches.patch`) — `tone`/`wavplay` no longer `abort()` when the BSP card handle is null; verified `tone 440 200` / `wavplay` no longer hits `lock_init_generic` abort. `components/audio/audio.c` background task unchanged, `audio status|stop` remains batch-safe.

### Fixed — modal EventGroup PSRAM

- **Modal `EventGroup` PSRAM fix** (`components/modal/modal.c:46` `xEventGroupCreateWithCaps(MALLOC_CAP_SPIRAM)`) — event group now prefers PSRAM (`MALLOC_CAP_SPIRAM`) with internal fallback, avoiding internal-heap fragmentation under transcript/TUI load; `modal_surface_run` session loop + `MODAL_EVENT_CLOSE_REQUEST` unchanged.

### Fixed — ANSI wifi white fix verified on boot

- **ANSI wifi `[wifi]` white→cyan verified** (`components/networking/networking.c:474` `C6 hosted firmware version: 3.0.6` and peers at `407,461,464,481,503,510,954,961,967,980,987,1043,1131` + `c6ota:` at `1080,1109,1114,1118,1127,1142,1147`, `main.c:200` `shell_c6ota_progress_callback`, `components/applib/applib.c:58` `app_vformat_append`, `components/shell/shell.c:709` `shell_schedule_transcript_appendf_ansi`) — boot `[wifi]` is now `SH_PROMPT` cyan (`@C[wifi]@R` → `ESC[96m[wifi]ESC[0m`), versions are `SH_VAL` bright white, progress lines use `ansi_format` + ANSI async path. Verified on COM11 boot log.

### Fixed — dialog/list/ask serial routing and draw auto-enter

- **Dialog/list/ask serial routing fix** (`components/modal/modal_surf.c:412` `modal_handle_serial_line` vs `shell_key_wait_submit`) — serial lines now route to active modal via `shell_command_ops_t.modal_*`; timeout `/t:secs` and direct serial input both close with correct ERRORLEVEL/`*RESULT`.
- **Draw auto-enter TUI** (`components/tui/tui.c:56` `tui_init` via `windows_enter_tui_mode`) — `draw` automatically enters TUI when no TUI/modal surface is active, otherwise reuses the active cell buffer.

### Changed — TUI engine is live (draw TUI-aware, window stack, essential features)

- **TUI grid `P4_CONFIG_TUI_COLS`×`P4_CONFIG_TUI_ROWS` 80×25 via transcript region** (`p4minishell_config.h:298` + `p4minishell_config.yaml` `tui:cols/rows`, `components/windows/windows.c` `windows_enter_editor_mode`/`windows_refresh_editor_surface`/`windows_notify_keyboard_visibility`, `components/modal/modal_surf.c`) — every modal surface (`dialog`/`list`/`ask`/`browse`/`view`/`hexview`) fills the live transcript region (`1024x510`) and resizes with rotation/keyboard; logical grid is clamped to 80×25, pixel rect follows the transcript (`tui status`).
- **`draw` is TUI-aware** (`components/batch/batch.c` `shell_command_draw` + `components/command/command.c` dispatcher, `components/tui/tui.c`) — `draw box/text/line/fill/clear` routes through the TUI cell buffer when a TUI/modal surface is active (auto-enters otherwise), otherwise falls back to transcript; `color`/`locate` set the TUI attribute/cursor (DOS `COLOR`/`LOCATE` parity). Window stack (`tui_draw_box` with title, nested boxes), fullscreen, color, prompt, screenshot debug are essential features implemented.
- **Companion fully TUI-expanded (7 BATs) and hardware-verified on COM11** — `COMPANION.BAT` `draw clear` + `draw box 1 1 80 3 double "P4 COMPANION"` + `draw box 1 4 80 15 single "Main Menu"` + `list`; `SYS.BAT` `tui fullscreen on` + `draw box 1 1 80 3 double "LIVE SYSTEM DASHBOARD"` + `draw box 1 4 80 10 single` / `draw box 1 15 40 10 single "Memory"` / `draw box 41 15 40 10 single "Tasks"` (`tui fullscreen on/off` `components/tui/tui.c:417`); `FILES.BAT` `browse`/`view`/`hexview` + `draw` + `tui fullscreen` (browse/view fill `80×25` transcript region); `NET.BAT` `draw box 1 1 80 3 single "NETWORK TOOLS"` + `draw box 1 4 80 10 single` / `draw box 1 15 80 8 single "Status"`; `FUN.BAT` TUI demo `tui fullscreen on` + `draw box 1 1 80 3 double "TUI DEMO"` + `draw box 5 6 20 6 single`/`double`/`rounded` + `draw box 5 5 70 10 single "Notes"` for melody/RGB/guess/calc; `SET.BAT` TUI demo `draw box 1 1 80 3 double "TUI SETTINGS DEMO"` + brightness/volume/`appconfig`; `LIB.BAT` tui helpers `:tui_banner` (`draw clear` + `draw box 1 1 80 3 single` + `draw text 2 2` ), `:tui_header` (`%1` title), `:tui_hr` (`draw line 1 4 80 4`). All pushed to SD via `push_sd.py` COM11 PASS (LIB 1896, COMPANION 1552, SYS 1486, FILES 3946, NET 2893, FUN 3968, SET 3109) using `receive <path> <size> /crc` ACK-paced + CRC-32. Extensive serial tests of each BAT (list selection, browse/view/hexview, tui fullscreen, draw boxes) all pass without abort/watchdog/overlap.
- **Modal serial routing fixed** — `dialog y` (serial line `y` → ERRORLEVEL 0), `list 2` (serial `2` → index 2), `ask myname` (serial `myname` → `ASK_RESULT`/`/v:NAME` correctly via `shell.c` `modal_handle_serial_line` `components/modal/modal_surf.c:412`); previously `ask`/`list` required `/t:secs` to consume serial input, now all modals route via `shell_command_ops_t.modal_handle_serial_line` without retry.

### Verification

- Clean build `idf.py build` 0 errors, 0 warnings (firmware + test).
- Flash to COM11 succeeded, boot banner verified, SD `dir`/`type` clean, `tui status` `1024x510 80x25` `80×25` `p4minishell_config.h:298`, 50+ mixed SD ops with zero `allocate_dma_buf` errors.
- Serial extensives: `draw box single/double/rounded` with title + nested (`SH_BOX_TL`/`H`/`V` vs `TL2`/`H2`/`V2` vs `TLR`/`TRR`/`BLR`/`BRR` via `tui_cell_set` `utf8[4]`), `draw line` H/V single/double/heavy, `draw fill`/`text`/`clear`/`window`/`close`/`refresh`, `draw fullscreen on|off` and `tui fullscreen on|off` (header hidden completely when on, kept otherwise via `windows_set_fullscreen` `components/windows/windows.c:418`), `color`/`locate` TUI-aware, `dialog`/`list`/`ask` with `/t:secs` timeout → 255/1 and via serial without timeout → `*RESULT`/`/v:NAME` (`dialog y` → 0, `list 2` → 2, `ask myname` → `myname` via `modal_handle_serial_line` `components/modal/modal_surf.c:412` `shell.c`), `browse`/`view`/`hexview` fill `80×25` transcript region and `draw` is TUI-aware (`tui_init` `components/tui/tui.c:56`), `tone`/`wavplay` do not abort (`bsp_audio_init` guard), boot `[wifi]` cyan verified, `grab_screenshot.py --port COM11 --out out.png --crop-transcript` + `capture_tui.py` pixel-perfect, `push_sd.py` COM11 PASS (LIB 1896, COMPANION 1552, SYS 1486, FILES 3946, NET 2893, FUN 3968, SET 3109) + companion BATs each exercised via serial `list`/`browse`/`view`, no watchdog, no overlap, no stack overflow at `0x4012b75a` after `P4_CONFIG_COMMAND_TASK_STACK` 16384→24576.

---

## [0.35.0] - 2026-08-24

### Added — TUI restoration + MSDOS parity for batch apps

- **Modal surfaces restored** (`components/modal/modal_surf.c`): the 6 batch TUI surfaces that were stubbed (all `return -1`) now render via the shared modal runtime and fill the live transcript region (rotation/keyboard-aware): `dialog "title" "message" [btn1] [btn2]` (0/1/255), `list [/t:secs] [/v:NAME] "title" items...` (0-based index + optional var), `ask [/t:secs] [/v:NAME] [/p] "prompt" [default]` (`ASK_RESULT`/NAME, 0/1), `browse [/t:secs] [/v:NAME] [path]` (`BROWSE_RESULT`/NAME, 0/1), `view <file>` and `hexview <file>` pagers (text + 16-byte hex dump). All accept `/t:secs` auto-cancel. `dialog`/`list`/`ask` dispatcher was missing from `components/command/command.c` — now wired, so interactive and batch use work.
- **Batch TUI extras**: `browse`/`view`/`hexview`/`color`/`locate` batch verbs (`components/batch/batch.c` + `batch.h` + `command.c` dispatcher), `P4_CONFIG_TUI_*` logical grid `80×25` (`p4minishell_config.h:284` + `.yaml` `tui:` top key) mapping to the live transcript rect via `windows_enter_editor_mode`/`windows_refresh_editor_surface`.
- **Companion migrated** (`apps/companion/*.BAT`): `COMPANION.BAT` main menu now uses `list` (0-based) with 255-cancel handling; `SYS.BAT` dashboard uses `list`/`dialog`; `FILES.BAT` browses SD with `browse`/`view`/`hexview`, clipboard/notes via `ask`/`list`; `NET.BAT` picks Wi-Fi via `list`/`ask` and views via `view`; `FUN.BAT` uses `dialog`/`list`/`ask`; `SET.BAT` uses `ask`/`list`/`dialog`. All handle `255` cancel.
- **Native TUI SDK stub** (`components/applib/applib_tui.h/.c` + `applib.h` umbrella + `CMakeLists.txt`): `tui_create/destroy/box/print_at/refresh/clear` declared now, stubbed with a warning, implemented after companion ships.

### Fixed — ANSI colour and async scheduling

- **White `[wifi]` on boot** (`components/networking/networking.c:474` `C6 hosted firmware version: 3.0.6` and 13 peers at `407,461,464,481,503,510,954,961,967,980,987,1043,1131` plus `c6ota:` at `1080,1109,1114,1118,1127,1142,1147`): all plain `networking_schedulef("[wifi] ...")` now `networking_schedulef_ansi(SH_PROMPT "[wifi]" SH_RST ... SH_VAL ... SH_RST)` so the second `[wifi]` is cyan (`@C`) and versions are bright white (`@W`). Verified: `@C[wifi]@R` expands to `ESC[96m[wifi]ESC[0m`.
- **`main.c:200` progress** (`shell_c6ota_progress_callback`): built `line` with `snprintf` + `SH_*` literals then passed literal `@` to `shell_transcript_append_ansi`/`shell_schedule_transcript_appendf` — now `ansi_format` + `shell_schedule_transcript_appendf_ansi`.
- **`applib.c:58`**: `app_vformat_append` used `vsnprintf` even when `ansi==true` — now `ansi_vformat` when `ansi`.
- **Async ANSI path** (`components/shell/shell.c:709` + `shell.h`): added `shell_schedule_transcript_appendf_ansi` (like the plain form but via `ansi_vformat`) and made `shell_async_transcript_flush_cb` dispatch to `shell_transcript_append_ansi` when the staged text contains ESC, otherwise plain. Added `networking_host_ops_t` note and `applib_tui` wiring.

### Changed

- Version bump `0.34.0` → `0.35.0` (`p4minishell_config.h`, `.yaml`, `readme.md`).
- Modal surfaces now correctly use `windows_enter_editor_mode`/`windows_refresh_editor_surface` so the TUI fills the same space as the shell and resizes with rotation/keyboard.
### Verification

- Clean build `idf.py build` 0 errors, 0 warnings (firmware).

- Manual smoke: `dialog`, `list`, `ask`, `browse`, `view`, `hexview` return correct `ERRORLEVEL`/`*RESULT` and cancel/timeout (`255`/`-1`) via touch, USB, and serial.

- Companion TUI expansion re-verified: `push_sd.py` COM11 PASS and companion BAT serial walks still clean after stack raise.

---

## [0.34.0] - 2026-08-18

### Added — FX-870P/VX-4 math parity complete

The `calc` float calculator (`components/batch/calc.c`) now implements all
FX-870P/VX-4 BASIC math functions. Five previously missing functions have been
added:

- **`CUR(x)`** — cube root (`cbrt(x)`, handles negative correctly)
- **`DEG(D.MMSS)`** — sexagesimal to decimal degrees (inverse of `DMS`)
- **`ASINH(x)` / `HYP ASN`** — inverse hyperbolic sine
- **`ACOSH(x)` / `HYP ACS`** — inverse hyperbolic cosine (domain x ≥ 1)
- **`ATANH(x)` / `HYP ATN`** — inverse hyperbolic tangent (domain -1 < x < 1)

Unit tests added in `test/main/test_calc.c` (`test_calc_new_math_functions`,
`test_calc_new_math_errors`). All 154 unit tests pass. Documentation updated in
`command.md`, `API.md`, `readme.md`, `roadmap.md`, and `SDK.md`.

---

## [0.33.0] - 2026-08-16

### Added - hybrid route: batch apps drive native modal surfaces

Batch files remain the on-SD apps, but now they can drop into polished native
modal surfaces when the transcript UI is not enough. A shared modal runtime
(generalised from the editor pattern) owns the session loop and input routing,
so the lifecycle lives once and every surface behaves consistently.

- **Shared modal runtime** (`components/modal/modal.{h,c}`): a single session
  loop + event group + input-routing layer for any native modal surface that
  takes over the shell display area. Surfaces register an ops table with
  open/service/close + USB/serial input handlers.
- **Editor migrated onto the runtime** (`components/editor/editor.c`): the
  `edit` command still loads/saves files on the worker task and renders on the
  LVGL task, but its input now routes through the shared modal layer instead of
  a private ops-table hook. No user-visible behaviour change; no function
  duplication.
- **Native modal surfaces** (`components/modal/modal_surf.c`):
  - `dialog "title" "message" [button1] [button2]` — message box, ERRORLEVEL
    0/1 for the two buttons, 255 for cancel/Esc.
  - `list "title" item1 [item2...]` — scrollable touch/keyboard selector,
    ERRORLEVEL is the 0-based selected index, 255 for cancel.
  - `ask "prompt" [default]` — text input with on-screen keyboard and USB
    typing support. The answer is stored in the `ASK_RESULT` environment
    variable; ERRORLEVEL 0 = OK, 1 = cancel.
- **Header notifications from batch and native apps**:
  - `notify <text>` batch command shows a notification in the header area;
    `notify -` clears it, `notify /t:secs` overrides the timeout.
  - `app_notify(text)` / `app_notify(NULL)` applib API (`components/applib/`)
    gives native apps the same header notification path.
  - Both route through the existing `shell_header_notify()` →
    `header_set_notification()` path.
- **Batch-friendly options on the modal commands**:
  - `ask /v:NAME "prompt" [default]` stores the answer in `NAME` instead of
    `ASK_RESULT`; `ask /p "prompt"` masks the input (password mode).
  - `list /v:NAME "title" items...` stores the selected item's label in
    `NAME` in addition to returning its index via ERRORLEVEL.
  - `notify /t:secs text` overrides the notification timeout.
  - `dialog` / `list` / `ask` accept `/t:secs` to auto-cancel after a
    timeout (matching `choice /T`), so an unattended batch script can never
    hang forever on a modal surface.
- **Version bump**: 0.32.8 → 0.33.0 (`p4minishell_config.h`,
  `p4minishell_config.yaml`, `readme.md`).

### Added — batch-language features (from the P4 Companion on-board testing)

The on-board `apps/companion` batch app (a menu-driven system helper written
entirely in batch) exercised the batch engine hard and surfaced a few missing
pieces, now added:

- **Dynamic pseudo-variables** in `shell_expand_variables()`:
  `%DATE%` (`MM-DD-YYYY`), `%TIME%` (`HH:MM:SS`), `%RANDOM%` (`0..32767`,
  `esp_random()`-based, cmd.exe parity), and `%CD%` (current directory) —
  alongside the existing `%ERRORLEVEL%`.
- **Undefined `%VAR%` expands to empty** (cmd.exe parity): an unset variable
  no longer stays literal, so the DOS `if "%var%"==""` idiom works. `%%` still
  yields a literal `%`, and single-quote protection is unchanged.
- **`if [not] [/i] defined VAR`** — true when `VAR` has been set (cmd.exe
  parity), in addition to `errorlevel` / `exist` / numeric / string forms.
- **`delay <ms>`** command (`components/command/command.c`): a pure,
  deterministic wait for melodies/demos; unlike `sleep` (light-sleep), it does
  not blank the display or touch Wi-Fi. Clamped to
  `P4_CONFIG_DELAY_MAX_MS` (new, documented in `p4minishell_config.yaml`).

### Added — native-app ABI (argv / env / cwd for native apps)

A native app is a C function with the signature `app_main_t(argc, argv)`
registered via `app_register()`. Once registered it becomes a shell command:
the dispatcher runs it with `argc`/`argv` (`argv[0]` = the app name) after
every built-in and `.bat` lookup fails, and the return value becomes
ERRORLEVEL — the native equivalent of the batch contract (`%0..%9`/`%*`, env
table, storage cwd).

- **`applib_app.h`** — `app_register()` / `app_find()` / `app_dispatch()` /
  `app_get()`; the table holds `P4_CONFIG_APP_MAX` apps (new config).
- **`applib_env.h`** — `app_env_get`/`app_env_set` (the shell's shared RAM
  environment table) and `app_get_cwd` (storage cwd), routed through
  `applib_env_ops_t` registered by `command_init()` so `applib` stays a leaf.
- **`apps` command** lists the registered apps.
- **Reference sample**: `main/native_apps.c` registers `hello`, which echoes
  its argv and reads cwd + PATH; `hello fail` returns errorlevel 1 for batch
  branching. Verified on board (`apps`, `hello a b c`, `echo
  %HELLO_RESULT%`, batch `if errorlevel`).

### Added — PATH-based `.bat` app discovery + `launch`

- **`launch` command** (`components/command/command.c`): discovers `.bat`
  apps on the command PATH and in the conventional `sd:/APPS` directory
  (bounded by `P4_CONFIG_LAUNCH_MAX`), and runs the chosen one through the
  batch engine. `launch` (menu), `launch <name>` (by name), `launch /list`
  (bare list).
- **`APPINFO` metadata convention**: `sd:/APPS/<name>.APPINFO` (INI format)
  provides an optional `title=` / `description=` for each app, shown by
  `launch` and the menu. The companion app ships one (`COMPANION.APPINFO`).
- **Boot-time offer hook**: a CONFIG.SYS `LAUNCH_APP=<app>` directive asks
  `run <app> now? (Y/N)` once after AUTOEXEC.BAT and launches on `Y` (a
  timeout declines). Manageable via `config LAUNCH_APP=<app>` /
  `config reset LAUNCH_APP` (tracked setting added).
- **Bug fixed during bring-up**: `launch` initially kept the discovery table
  (≈5 KB) on the command-worker stack, overflowing it once the launched batch
  re-entered the dispatcher per line. The table is now heap-allocated (and not
  allocated at all for `launch <name>`).
- Verified on board: `launch /list` (with the companion's title), the menu,
  `launch COMPANION` (full dashboard flow), and the boot offer (Y launches,
  N/timeout declines).

### Added — Palm-OS-style `db` record store (SD-backed)

A named, SD-backed database surface for batch apps and native apps, inspired
by Palm OS records (monotonic ids, 16 categories, secret/redacted payloads)
but stored as plain files under `sd:/DBS/<name>.DB/`:

```
HEADER.INI        name, creator, type, version, next_id, record_count
CATEGORIES.INI    0=Unfiled .. 15=...
INDEX.TXT         one "id cat flags key size" line per record
RECORDS/          R<8-hex-id>.DAT per record payload
```

- **New leaf component `components/db/`** (`db.{h,c}`, `CMakeLists.txt`):
  all data lives on the SD card; every operation opens a guarded SD session,
  pre-checks free space, writes atomically (temp + rename), and heap-allocates
  every record-sized buffer (safe on the recursive batch path). Records are
  soft-deleted by flag until `purge`; ids are monotonic and never reused.
- **`db` command** (`components/command/db_commands.c`, argv-verb dispatcher):
  `create` / `list` / `info` / `drop`, `open` / `close` / `current` (a RAM
  current-db pointer so the name is optional), `categories` (list/set/clear),
  `add` / `get` / `set` / `del` / `purge`, `count` / `find`, and
  `export` / `import`. Options may appear anywhere (`/cat:N /key:K /text:P
  /cr:/tp:/vr: /secret /reveal /p /b`); `/b` gives bare, pipe/for-friendly
  output. ERRORLEVEL 0/1/2. Secret payloads are redacted unless `/reveal`.
  Export writes `id|cat|flags|key|hexpayload` lines (binary-safe); import
  appends records with fresh ids.
- **`applib_db.h`** — `app_db_create/info/drop`, `app_db_category_set/get`,
  `app_db_add/get/set/del`, `app_db_purge/count/find`, thin NULL-checked
  wrappers over the db core (added to the applib umbrella).
- **Config** (`p4minishell_config.h` + `.yaml`): `P4_CONFIG_DB_*` bounds for
  databases, records, payload size, find results, name/key/label lengths,
  category count, export limits, and the secret/deleted flag bits.
- **Unit tests** (`test/main/test_db.c`): pure `db_name_valid` rules.
- **On-board verification** (`apps/companion/db_test.py`): 38/38 checks PASS —
  create/info/list/drop, add/get/set/del, secret redaction + `/reveal`,
  find/count, categories, soft-delete + purge, export/import round-trip,
  current-db short forms, and error levels.

### Added — SD-persisted alarms (`alarm`, `cal`)

A small, batch-friendly alarm/event system that reuses every existing surface
instead of inventing a second notification path:

- **New leaf component `components/alarm/`** (`alarm.{h,c}`): a guarded,
  atomic SD store under `sd:/ALARMS/` (`INDEX.INI` + one `E<id>.INI` per
  event) and a single low-rate background checker task
  (`P4_CONFIG_ALARM_TASK_STACK`). Events carry a monotonic id, a timezone-aware
  `when`, title/message, flags (enabled/fired/silent), a recurrence (none /
  daily / weekly weekday bitmask), and an action set (notify / beep / led /
  run).
- **Reuses existing surfaces only**: on fire the checker posts to
  `shell_header_notify` (thread-safe from any task), `led_notify`
  (`LED_EVENT_ALARM` added to the LED event table), `audio_play_tone`, and —
  for the `/run:` action — a `call <file>` queued onto the command worker via
  a registered `alarm_host_ops_t.execute_async` hook (registered in
  `command_init()`), never run on the checker stack. No private loop.
- **`alarm` / `cal` commands** (`components/command/alarm_commands.c`):
  `add` / `list` (/b bare) / `status` / `enable` / `disable` / `del` (soft,
  `del all` wipes + resets ids) / `purge`; `cal today` / `cal next` /
  `cal YYYY-MM`. Options anywhere, ERRORLEVEL 0/1/2, ISO `YYYY-MM-DD HH:MM`
  parsing via `mktime` (timezone-aware).
- **Boot catch-up**: alarms missed while powered off fire once on the next
  boot (`P4_CONFIG_ALARM_CATCHUP_ON_BOOT`); light sleep suspends the checker
  until the device wakes (documented limit).
- **Config** (`p4minishell_config.h` + `.yaml`): `P4_CONFIG_ALARM_*` bounds
  for events, title/message lengths, poll interval, task stack, store path,
  catch-up, default notify seconds, and a `/run:` compile-out flag.
- **Unit tests** (`test/main/test_alarm.c`): pure weekday-mask, recurrence
  advance, and `YYYY-MM-DD HH:MM` parsing helpers.
- **On-board verification** (`apps/companion/alarm_test.py`): 25/25 PASS —
  add/list/status/cal/enable/disable/del/purge plus the background checker
  firing a due event (marked fired) and a `/run:` batch action queued onto the
  command worker (marker file created). Bring-up fix: the checker stack was
  raised 3072 → 8192 because newlib's `snprintf` frame (used to render event
  files and notifications) plus the tick's event/path locals overflowed the
  smaller stack.

### Fixed — from the P4 Companion on-board testing

- **Command worker stack overflow** (`Guru Meditation`, stack protection
  fault in `shell_cmd`): the deep re-entrant batch nesting of `setlocal` +
  `call file::routine` + `for /f` over a pipe overflowed the 12 KB worker
  stack. Raised `P4_CONFIG_COMMAND_TASK_STACK` 12288 → 16384 (same rationale
  as the earlier 8192 → 12288 raise).
- **`for /f` failed in batch files** (`for /f: malformed options`): the batch
  path passed the raw quoted options string (`"tokens=*"`) to the option
  parser, while the interactive path had the quotes stripped. The parser now
  strips the surrounding quotes in `shell_execute_for_loop`, so documented
  batch `for /f "delims=.. tokens=.."` syntax works. Unit-tested.
- **Silent command drops under rapid serial input**: the command worker queue
  was only 4 deep and `xQueueSend` used a 0 timeout, so a busy worker dropped
  the 5th queued command. Depth raised to 8 and the submit now waits up to
  1 s (bounded; still drops with a clear message only if the worker is
  persistently stuck). The submit runs on the UART console task, never the
  worker, so blocking cannot deadlock the pipeline.
- **Key-wait prompt race** (`pause`/`choice`/confirm prompts dropped fast
  input): `pause`, `choice`, `shell_confirm_destructive`, the xcopy prompts
  and the `-- More --` pagers printed their prompt and *then* armed the key
  wait, so a key typed as soon as the prompt appeared could be dispatched as a
  command (or lost) instead of answering the prompt — a batch `pause` would sit
  its full timeout and the app's default menu action fired. The key wait is now
  armed *before* the prompt is printed in every one of those commands.
- **`if COND cmd1 & cmd2` ran `cmd2` unconditionally**: the command-chain
  splitter split a line on unquoted `&`/`&&`/`||` before execution, so in
  `if %tries% GEQ 10 echo Out of tries! & goto main` the `goto main` ran on
  every iteration regardless of the condition (broke the companion's number
  guessing game). `shell_split_chain` now hands the whole line to `if`/`for`
  as a single segment; those commands already join the rest of the line as
  their body and re-enter the pipeline, so the `&` runs only when the
  condition/iteration is taken.

### Verification

- Clean build: 0 errors, 0 warnings (firmware and test project).
- Flashed to the board (ESP32-P4, COM11) and boot verified: `P4MiniShell
  v0.33.0 ready`, SD mounted, AUTOEXEC runs, Wi-Fi/Bluetooth/USB come up.
- On-board serial checks of the new surfaces:
  `dialog` → errorlevel 0/1 per button, `list` → 0-based index, `ask` →
  `ASK_RESULT` set with errorlevel 0, `notify` clean, `edit` still opens and
  quits via `\q`. Cancel paths verified: dialog/list → 255, ask → 1. No
  assert, panic, or reboot.
- Batch-file testing on the SD card: a `.bat` calling `dialog` (with
  `if errorlevel` branching and `&&`/`||` chaining), `list`, `ask`, and
  `notify` ran to completion with correct errorlevels and `ASK_RESULT`; a
  5-modal and a 10-dialog stress run both left heap unchanged (no leak) and
  produced no crash.
- Edge-case suite (all on board):
  - Usage errors for `dialog`/`list`/`ask`/`notify` → errorlevel 2.
  - Single-button dialog → 0; `list` with one item → 0.
  - `ask /v:NAME`, `ask /p`, `list /v:NAME` (stores selected label), and
    `notify /t:secs` verified.
  - Cancel keeps `/v:` variables unchanged (`list` → 255, `ask` → 1).
  - Timeouts (`/t:secs`) auto-cancel: dialog/list → 255, ask → 1; early input
    before the timeout still wins.
  - `ask` result used in `if /i "%VAR%"=="ok"` and inside `call`ed batches.
- Two bugs found and fixed during on-hardware bring-up: `modal_surface_run`
  used an event-group wait mask with the reserved top byte set (FreeRTOS
  `xEventGroupWaitBits` assert → reboot), and `ask` overwrote a serial answer
  with the empty textarea contents on close.
- **Deep-test sweep of `apps/companion`** (`apps/companion/deep_test.py`,
  reactive serial driver, uptime-verified reboots): all 8 checks PASS —
  baseline (`apps`, `launch /list`), the full main-menu walk into all five
  modules and back, SYS dashboard + refresh, FILES dir/search/trash/clipboard/
  notes (new/list/view), NET offline (status/connect-cancel/ping/joke), FUN
  melody/RGB rainbow/number guessing to win-or-out, SET brightness/scoping/
  show, and settings persistence across a reboot. The sweep surfaced the two
  fixes above (key-wait race, `if ... &` chain split).

---

## [0.32.8] - 2026-08-15

### Fixed - memory-hardening sweep: SD card reliability + transcript scrollback memory

A post-release sweep re-ran every command twice, every batch verb and batch
file on the SD twice, the applib/appconfig/appmode paths, and the full unit
suite. Three medium bugs and one thread-safety hazard were found and fixed:

- **SDMMC DMA buffer allocation failures** (`sdmmc_cmd: allocate_dma_buf:
  not enough mem`). The IDF sdmmc driver allocates a temporary DMA buffer for
  every card transaction unless the host carries a cached buffer; on the P4
  that allocation comes from the internal DMA-capable heap (PSRAM does not
  carry `MALLOC_CAP_DMA` here) and fails once the heap fragments under load,
  taking every SD command down. Fix: `storage_sd_ensure_dma_buffer()`
  pre-allocates a cached `card->host.dma_aligned_buffer` at mount (new
  `P4_CONFIG_SD_DMA_BUFFER_BYTES`, 8192 B = 16 × 512 B sectors) and derives
  the SDMMC chunk size from it; released on `sdeject`. 50+ SD operations run
  with zero DMA errors (previously every op failed after ~40 s).
- **`abort()` in newlib `lock_init_generic`** (stdio FILE-lock OOM) under
  long sessions. The transcript's LVGL span objects consume internal RAM;
  once the accumulated history exhausts it, a tiny stdio lock allocation
  (`xQueueCreateMutex`) fails and the board aborts mid-command. Fix:
  `shell_transcript_guard_internal()` auto-trims the scrollback (oldest half
  dropped, spans freed synchronously under the LVGL lock) whenever free
  internal RAM drops below `P4_CONFIG_TRANSCRIPT_INTERNAL_TRIM_BYTES`
  (49152), checked at command start and before every append; and
  `P4_CONFIG_TRANSCRIPT_BYTES` reduced 16384 → 8192 to halve the span-group
  ceiling. The full sweep (previously 6 abort crashes) now completes with
  zero crashes.
- **`display_set_rotation()` LVGL thread-safety.** The rotate command now
  runs the LVGL rotation call plus the synchronous resolution-changed
  callback and the async UI-rebuild scheduling under `lvgl_port_lock`, like
  every other shell-path LVGL caller. (An earlier "reboot on rotate" report
  was a false alarm: the `P4MiniShell ready` banner and keyboard audit are
  legitimately re-printed by the UI rebuild.)
- **SD card cleanup**: removed all accumulated test artifacts, leaving only
  `CONFIG.SYS`, `AUTOEXEC.BAT`, `WIFI.KNOWN`, and the C6 slave image.

### Verification

- Clean build: 0 errors, 0 warnings (firmware and test project).
- On-board unit suite: **158 tests, 0 failures** (20 groups).
- Full command sweep (105 commands × 2 passes): **0 crashes, 0 errors**.
- Batch sweep (18 verbs + 27 batch files × 2 passes): **0 crashes**.
- appmode/appconfig/applib round-trips, rotate 90/180/270, and the previously
  crashing `volume`/`move`/`ren` commands verified clean on board.

---

## [0.32.7] - 2026-08-15

### Added - app mode: clean enter/exit with save/restore screen, full-screen, cleanup on `exit /b`

A clean way for a batch file (or a native app) to take over the shell and
hand it back, so interactive apps get a full-screen surface:

- **`appmode` command** (`components/batch/batch.c`): `appmode on [/full]
  [/clear]` saves the current transcript (colours preserved) and optionally
  hides the shell input widgets for a full-screen app surface and clears the
  transcript; `appmode off` restores the saved screen; `appmode status`
  reports the state.
- **Cleanup on `exit /b`**: each batch frame tracks whether it entered app
  mode; when the file returns via `exit /b` / `goto :eof` / EOF, the saved
  screen is restored automatically, so an app can never leave the shell in
  app mode.
- **Shared shell-core primitives** (`components/shell/shell.c`):
  `shell_screen_save/restore/discard` capture and restore the transcript
  (ANSI form, colours preserved) and `shell_app_mode_enter/exit/active`
  toggle the full-screen surface via new `windows_enter_app_mode` /
  `windows_exit_app_mode` (hide/restore the input-line and scroll buttons;
  the on-screen keyboard can still be shown for app input). Implemented once
  here, below both batch and applib.
- **applib** (`applib_ui.h`): `app_mode_enter(full_screen)` /
  `app_mode_exit()` wrap the same shell primitives for native apps.

### Verification

- Clean build: 0 errors, 0 warnings (firmware and test project).
- On-board unit suite: **158 tests, 0 failures** (new `test_applib_app_mode`,
  which also guards the no-LVGL headless path).
- On-board simulation (UART console, mirrored to the transcript display):
  `appmode status` → `off`; a batch app that runs `appmode on /full /clear`,
  prints a styled full-screen banner, `choice`, then `exit /b 0` — after the
  exit the prior screen (`SCREEN_MARKER_123` and history) is restored and
  `appmode status` reports `off`; a manual `appmode on /clear` /
  `appmode off` round-trip restores the screen.

---

## [0.32.6] - 2026-08-15

### Added - `set /p /P` password mode + `app_read_password` (no echo)

Prompted input can now be collected without echoing, for passwords and other
secrets:

- **`set /p NAME=<prompt> /P`** (`components/batch/batch.c`): the typed line
  is stored in `NAME` but never echoed — Backspace erases silently, ESC
  cancels, Enter completes and prints a newline. Combines with `/T:secs` for a
  bounded password prompt (`set /p pin=PIN: /P /T:2`).
- **`shell_read_line_hidden()`** (`components/shell/shell.c`): the shared
  no-echo line reader behind it, so the batch command and applib use one
  implementation (`shell_read_line` is now a thin wrapper over the shared
  mode with echo on; behavior unchanged).
- **`app_read_password(buf, size, timeout_ms)`** (`applib_input.h`): the
  app-side equivalent for native apps, wrapping `shell_read_line_hidden`.

### Verification

- Clean build: 0 errors, 0 warnings (firmware and test project).
- On-board unit suite: **157 tests, 0 failures** (new
  `test_applib_read_password`).
- On-board simulation (UART console): `set /p user=User:` echoes `alice` →
  `user=alice`; `set /p pass=Password: /P` accepts `s3cret` with **no echo**
  and stores `pass=s3cret`; `set /p pin=PIN: /P /T:2` stores `pin=1234`
  without echo.

---

## [0.32.5] - 2026-08-15

### Added - menu / form primitives (`ansi` + `menu`; CHOICE + ANSI was the DOS way)

Interactive apps were built from CHOICE (single-key selection) plus ANSI
escape codes (colors, bold, reverse video). Those primitives now exist for
both batch and applib, rendered in the shell transcript (the display area)
and readable from touch, USB keyboard, or serial:

- **`ansi <sgr-codes> [text...]`** (`components/batch/batch.c`): emit text
  styled with the given ANSI SGR codes — `7` reverse video, `1` bold, `31`
  red, `90` muted, `0` reset, etc. — wrapped as `ESC[<codes>m text ESC[0m`
  into the transcript. The DOS `7m` spelling (with the trailing `m`) and the
  bare `7` spelling are both accepted; invalid codes are rejected (ERRORLEVEL
  2).
- **`menu <item> [item...]`**: a numbered form primitive — renders `1. item`
  lines in the transcript, reads a numeric choice, and sets ERRORLEVEL to the
  chosen item's 1-based index (0 on cancel / timeout / an invalid entry), so a
  batch app branches with `if errorlevel`.
- **`echo.` idiom**: the DOS blank-line `echo.` is now recognized (was an
  unknown command), so DOS-style menu scripts port cleanly.
- **applib menu/form primitives**: `app_print_styled(sgr_codes, ...)` (wrap
  text in SGR codes) and `app_menu(title, items, count, timeout_ms)` (render
  a numbered form and return the chosen index, 0 on cancel), the app-side
  equivalents of the batch commands.
- CHOICE is unchanged and re-verified composing with `ansi` in a batch menu.

### Verification

- Clean build: 0 errors, 0 warnings (firmware and test project).
- On-board unit suite: **156 tests, 0 failures** (new
  `test_applib_menu_primitives`).
- On-board simulation (UART console, mirrored to the transcript display):
  `ansi 1;36m Title line` renders bold-cyan; `ansi bad;7m` → errorlevel 2;
  `menu "Start game" "Load save" "Settings" "Quit"` + `2` → errorlevel 2,
  invalid `9` → errorlevel 0; `choice /C:YN /N` + `Y` → `CHOSE_Y`; and
  `appmenu.bat` (an `ansi` banner + `echo.` blank line + `choice`) selects
  `picked wifi` by key 3.

---

## [0.32.4] - 2026-08-15

### Added - easy persistent state: `ini`, `appconfig`, `temp` + applib state group

DOS-like apps kept state in environment variables, temporary files, and
simple `KEY=VALUE` INI files. That surface now exists for both batch and
applib, with **all storage on the SD card** and a single shared core
(`components/storage/storage_ini.c`):

- **`ini` command** (`components/batch/batch.c`): `ini list|get|set|del
  <file> [key] [value]` reads/updates any `KEY=VALUE` file on the SD card
  (comment- and blank-line aware, atomic temp+rename writes), and
  `ini load|save <file>` imports/exports the environment — easy persistent
  state without hand-rolling file parsing.
- **`appconfig` command**: per-app settings without hand-rolling parsing.
  `appconfig <app> [path|list|get|set|del]` reads/writes a namespaced
  `sd:/APPS/<APP>.INI` file (the `APPS` directory is created on demand), so
  batch apps get a settings file of their own.
- **`temp` command**: `temp` shows the SD temp directory (`sd:/tmp`),
  `temp new [ext]` creates a unique SD-backed temporary file and prints its
  path, `temp clean` deletes every temp file.
- **applib state group** (`applib_state.h`): `app_ini_get` / `app_ini_set` /
  `app_ini_delete` and `app_temp_path` / `app_temp_cleanup` give native apps
  the same INI + temp-file primitives, wrapping the shared storage core.
- The pure INI line editors were moved from the `config` command
  (`config_directive_*`) into `components/storage` (`storage_ini_*`) so the
  `config`, `ini`, and `appconfig` commands and applib all share one
  implementation; `config_directive_*` remain as thin wrappers.
- New config: `P4_CONFIG_INI_MAX_BYTES` (16 KB) and
  `P4_CONFIG_TEMP_DIR_NAME` ("tmp"), documented in p4minishell_config.yaml.

### Verification

- Clean build: 0 errors, 0 warnings (firmware and test project).
- On-board unit suite: **155 tests, 0 failures** (new `test_applib_state`;
  the existing `config_directive_*` tests pass unchanged over the storage
  wrappers).
- On-board simulation (UART console): `ini set/get/list/del` round-trips
  (`theme=dark`, delete + errorlevel 1 on missing); `ini save` →
  `set MYSTATE=` → `ini load` restores `MYSTATE=hello SCORE=42`;
  `appconfig myapp set/get/path/del` auto-creates `/sdcard/APPS/myapp.INI`
  and rejects `bad/name` with errorlevel 2; `temp new csv` →
  `/sdcard/tmp/_app0.csv`, `temp clean` empties the temp directory; `dir
  APPS` and `dir tmp` show the state files on the SD card.

---

## [0.32.3] - 2026-08-15

### Added - batch shared-library CALL + input timeouts; applib lean headers + input

Better modularity for both surfaces, without duplicating any existing verb:

- **`call <file.bat>::<routine> [args]`** — shared libraries of batch
  routines. An external `.bat` becomes a callable library: `call lib.bat::add
  3 4` loads `lib.bat`, starts execution at its `:add` routine, and returns
  to the caller on `exit /b` / `goto :eof` / end of file. The routine's
  arguments arrive as `%1`..`%9`/`%*` and its final errorlevel propagates.
- **Variable isolation beyond `setlocal`** — a library-routine call
  automatically pushes an environment scope for the callee, so a routine's
  temporary variables never leak into the caller (verified: `result`/`tmp`
  set inside `lib.bat::add`/`:probe` are undefined back in the caller, with
  no `setlocal` written by the caller). `call <file.bat>` (whole file) keeps
  the shared-environment behavior, matching DOS.
- **`set /p NAME=<prompt> /T:secs`** — prompted input now takes a timeout
  (`/T:secs`, default `P4_CONFIG_KEY_WAIT_TIMEOUT_MS`), giving input the same
  timeout control `choice /T` has. On timeout the variable is left unchanged
  and errorlevel is 1. `choice /T` is unchanged and re-verified on board.
- **applib lean include mechanism** — `applib.h` is now an umbrella over
  focused headers (`applib_console.h`, `applib_mem.h`, `applib_time.h`,
  `applib_net.h`, `applib_input.h`), so a native app includes only the groups
  it uses. Each function is declared in exactly one header; the umbrella
  keeps the existing `#include "applib.h"` working.
- **applib input with timeout** — `app_wait_key(timeout_ms, &key)` and
  `app_read_line(buf, size, timeout_ms)` give native apps bounded keypress /
  line reads (the primitive behind `pause` / `choice /T` / `set /p /T`),
  backed by the shell's key queue and returning false on a headless board.

### Verification

- Clean build: 0 errors, 0 warnings (firmware and test project).
- On-board unit suite: **154 tests, 0 failures** (new
  `test_applib_input_timeout`; `app_wait_key`/`app_read_line` are bounded and
  headless-safe in the runner).
- On-board simulation: `call lib.bat::add 3 4` → `ADD result=7`;
  `call lib.bat::shout alpha beta` → `SHOUT alpha beta`;
  `call lib.bat::nope` → `call: library routine not found`; a caller that
  invokes `:add`/`:probe` sees no leaked `result`/`tmp` (isolation);
  `set /p ans=Enter something /T:2` times out with "no input received" and
  errorlevel 1; `choice /C:YN /T:N,2 Continue?` defaults to `N (timed out)`
  and errorlevel 2.

---

## [0.32.2] - 2026-08-15

### Added - batch process abstraction (`proc` + `%ERRORLEVEL%`)

The batch "process" model is now explicit and introspectable, on top of the
existing pipe-spool mechanism (each stage spools through an SD temp file and
the next stage reads it through the input-redirection slot — a single worker
task runs one command at a time, so there is no second process to stream
into):

- **`proc` command** (`components/batch/batch.c`): lists the active batch
  process stack — every nested `.bat` (script path, depth, arguments, echo
  state) — and reports the current process with `/args` (all arguments),
  `/name` (%0), `/depth`, `/errorlevel`, `/echo`, and `/stdin` (the active
  pipe spool / `< file` source). A batch file can therefore branch on its own
  depth, arguments, or exit code, and a shell user can see which scripts are
  nested and what a pipe stage is reading. ERRORLEVEL 0 ok / 2 usage.
- **`%ERRORLEVEL%` expansion**: `shell_expand_variables()` now expands
  `%ERRORLEVEL%` (case-insensitive) to the current errorlevel as a decimal
  string, giving a batch process access to its own exit code (cmd.exe
  parity): `set code=%errorlevel%`, `if %errorlevel%==5 ...`.
- **Batch files as pipe processes**: a `.bat` is now a first-class pipe
  stage — `echo hello | filter.bat` (reads stdin via `for /f ... in ()` /
  `set /p NAME=<`) and `echo x | filter.bat | findstr LINE` (batch in the
  middle of a pipeline) are verified on board, with `proc /stdin` showing the
  spool file the stage is consuming.

### Fixed - redirection capture was not re-entrant

- **`cmd1 | cmd2 > out.txt` (and `>>`) wrote an empty file.** The output
  redirection capture was a single global buffer; a pipeline stage's own
  `> _pipeN.tmp` spool redirect nested inside the outer `>` and its reset
  freed the buffer the outer capture depended on. `shell_redirect_capture_*`
  now keep a stack of captures (`P4_CONFIG_REDIRECT_CAPTURE_MAX_DEPTH`, 8):
  the inner stage's capture is written and popped, and the outer capture
  resumes so the outer file receives the whole pipeline output.
- `%ERRORLEVEL%` initially did not expand (the token buffer was read before
  it was filled) — fixed and covered by a unit test.

### Verification

- Clean build: 0 errors, 0 warnings (firmware and test project).
- On-board unit suite: **153 tests, 0 failures** (new
  `test_variable_expansion_errorlevel` and `test_redirect_capture_nested`).
- On-board batch stress suite (UART console): multi-stage pipes, pipes inside
  batch files, batch-as-pipe-filter (stdin via `for /f in ()`),
  `echo hello | sort > out.txt` round-trips, `>>` append nesting, pipe error
  cases (stage cap, empty stage), `%ERRORLEVEL%` after `findstr` hit/miss,
  and nested `call` + `proc` stack introspection with `exit /b` errorlevel
  propagation (`P1 code=5`).

---

## [0.32.1] - 2026-08-15

### Added - applib: the native-app runtime library (shell SDK)

The four "Runtime services to define" from the roadmap are now implemented as a
single leaf component, `components/applib` (`applib.h`), which native apps link
against instead of reaching into shell/clock/networking internals:

- **Console output API** (`app_printf` / `app_vprintf`, ANSI variants, and the
  semantic `app_print_heading`/`app_print_field`/`app_print_ok`/`app_print_error`/
  `app_print_warning`/`app_print_muted`/`app_print_usage`). Output lands on the
  transcript — which is the redirection layer — so an app invoked inside a
  command dispatch is captured by `>` / `>>` exactly like a built-in command.
- **Memory allocation policy + error reporting** (`app_alloc`/`app_calloc`/
  `app_realloc`/`app_strdup`/`app_strndup`/`app_free`, and
  `app_report_error`/`app_report_warning`/`app_report_info`). Blocks of
  `P4_CONFIG_APPLIB_PSRAM_THRESHOLD_BYTES` (512) or more prefer PSRAM with an
  internal-heap fallback; diagnostics route into the shell debug log.
- **Time / timers / sleep / system information** (`app_time`,
  `app_time_local`/`app_time_utc`, `app_uptime_sec`, `app_now_ms`,
  `app_delay_ms`, `app_time_synced`, `app_uptime_formatted`, `app_sysinfo`).
- **Networking helpers** (`app_wifi_is_connected`, `app_wifi_get_rssi`,
  `app_wifi_state_string`) read Wi-Fi state through the registered
  `applib_net_ops_t` table (wired in `command_init()`), so applib never includes
  `networking.h` — the same one-way ops-table pattern as `shell_command_ops_t`.

Layering: `applib` depends only on `shell`, `clock`, and the FreeRTOS/heap/
esp_timer IDF components. Registered in the root and test `CMakeLists.txt`;
`components/command` requires it to register the networking hooks. 11 new unit
suites in `test/main/test_applib.c` cover the memory policy, the time/sysinfo
helpers, the console entry points, and the Wi-Fi accessors through a fake ops
table.

### Fixed - two batch integration bugs found by the on-board batch-file check

- **`calc NAME=<expr>` failed from a batch file (and at the prompt).**
  `shell_command_calc_line()` copied the *whole* `NAME = expr` text into the
  variable name and passed it to `shell_env_set()`, which rejects names
  containing spaces — so `calc y = 6 * 7` reported "invalid variable name or
  environment is full". It now extracts only the text before the `=`.
  Regression test: `test_calc_command_assignment`.
- **`for /f` silently did nothing** (interactive and in batch files). After
  parsing the `/f` options the parser never advanced past them, so the
  loop-variable check `if (*for_ptr != '%')` always failed and the loop
  returned without iterating. `shell_execute_for_loop()` now advances
  `for_ptr` to the loop variable after the options region.

### Verification

- Clean build: 0 errors, 0 warnings (firmware and test project).
- On-board unit suite: **151 tests, 0 failures** (16 `calc`, 11 `applib`).
- On-board batch-file checks (UART console): `for /f "delims=," %%a in
  (data.txt) do echo ITEM=%%a` prints `ITEM=apple`; `calc y = 6 * 7` sets
  `y=42`; `set /p v=< data.txt` sets `v=apple,banana,cherry`; interactive
  `for /f` `tokens=2` / `tokens=1,*` / `skip=1` behave per DOS; `calc pi2 =
  pi * 2` and `calc x = len("hello")` store and recall env values.

---

## [0.32.0] - 2026-08-15

### Added - `calc` float calculator + batch file input (FX-870P/VX-4 port)

The CASIO FX-870P/VX-4 BASIC command surface is mapped onto the DOS-style
batch language, without duplicating any existing verb. Three new capabilities:

- **`calc` command** (`components/batch/calc.c`): a batch-native float
  calculator. `calc <expr>` prints the result, `calc NAME=<expr>` stores it in
  an environment variable, `calc /deg|/rad|/angle` set/query the trig angle
  mode, and `calc /hex <expr>` prints an integral result as `&H` hex.
  Grammar: `+ - * / ^` (right-assoc power), the BASIC `MOD` keyword, unary
  `- +`, parentheses, `&H`/`0x` hex literals, `PI`, `RAN#[(seed)]`, string
  literals (`'` or `"`) with `+` concatenation, and env-var references
  (undefined reads as 0, matching `set /a`). Functions: `ABS SGN INT FIX FRAC
  ROUND SQR EXP LN LOG SIN COS TAN SINH COSH TANH ASN/ASIN ACS/ACOS ATN/ATAN
  FACT NCR NPR DMS DMS$ VAL VALF STR$ HEX$ ASC CHR$ LEN LEFT$ MID$ RIGHT$
  MOD POL REC`. `POL`/`REC` store their two results in the X/Y environment
  variables (the calculator's documented side effect). ERRORLEVEL 0/1/2.
- **`set /p NAME=< file`** (`shell_command_set_prompt`): with an active `<
  file` redirection or a pipe stage, `set /p` reads one line from that source
  instead of the interactive key queue, exactly like cmd.exe. Covers the
  BASIC `INPUT#` / `LINE INPUT#` verbs (`echo x | set /p var=` works).
- **`for /f`** (`shell_execute_for_loop`): file-line loops in the classic
  form `for /f "eol=c skip=n delims=xyz tokens=a,b,m-n" %%v in (file-set) do
  cmd`. Sources: an explicit file, a wildcard, or `()` with the active `<
  file` / pipe input. Per line: skip/eol filters, delimiter split, token
  indices bound to consecutive loop-variable letters (`%%a %%b ...`), and a
  `*` capture of the rest of the line. Covers the BASIC `READ`/`DATA`/
  `RESTORE`/`EOF` verbs. The pure option parser and line splitter
  (`shell_forf_parse_options` / `shell_forf_split_line`) are unit-tested.

New config tunables under `calculator` / `for /f`:
`P4_CONFIG_CALC_STR_BYTES`, `P4_CONFIG_CALC_MAX_DEPTH`,
`P4_CONFIG_CALC_ANGLE_DEFAULT_DEG`, `P4_CONFIG_CALC_PRINT_PRECISION`,
`P4_CONFIG_FORF_TOKEN_MAX`, `P4_CONFIG_FORF_LINE_MAX`,
`P4_CONFIG_FORF_DELIMS_BYTES` (documented in p4minishell_config.yaml).

The remaining BASIC verbs map onto existing commands and are documented in
`command.md` ("BASIC-to-DOS batch mapping"): `BEEP`→`beep`, `CLS`→`cls`,
`GOTO`→`goto`, `IF/THEN/ELSE`→`if (…) else (…)`, `FOR/NEXT`→`for %%v in (…) do`,
`LET`→`set`, `REM`→`rem`, `FILES`→`dir`, `KILL`→`del`, `NAME`→`ren`,
`DELETE`→`del`, `EDIT`→`edit`, `CHAIN`→`call`, `GOSUB`→`call :label`,
`RETURN`→`goto :eof`, `INPUT`→`set /p`, `PRINT`→`echo`, `LIST`→`type`/
`findstr /n`, `PRINT#`/`WRITE#`→`write`/`append`/`>`/`>>`, `RUN`→invoke the
`.bat` by name, `LOAD`→`call`, `SAVE`→`write`/`edit`, `MERGE`→`copy`/`append`,
`NEW`→new shell session, `VARLIST`→`set`, `DSKF`→`chkdsk`, `STOP`→`pause`,
`END`→`exit /b`, `ON ERROR`/`RESUME`→`if errorlevel`/`||`, `TRON/TROFF`→
`echo on`. Hardware/no-sense items (`PEEK`/`POKE`/`DEFSEG`/`DEFM`,
`DEFCHR$`, `LPRINT`/`LLIST`, `LOCATE`, `MODE`, `PASS`, `PBLOAD`/`PBGET`,
`CALC$`/`CALCJMP`, `RENUM`, `CONT`, `VERIFY`, `OPEN`/`CLOSE`, `EOF`) are
documented as not applicable rather than stubbed.

### Changed

- `set /p` with no interactive key source and an active `<`/pipe source now
  reads the file line instead of timing out (DOS parity).
- `for` now also accepts the `for /f` file-line form; the classic token /
  wildcard forms are unchanged.

### Verification

- Clean build: 0 errors, 0 warnings (firmware and test project).
- On-board (COM11): full unit suite passes — 139 tests, 0 failures,
  including 15 new `test_calc` cases (arithmetic/precedence/`^`/MOD, every
  math and string function, DEG/RAD, `&H`/`0x` hex, PI, seeded RAN#, env
  assignment, POL/REC X/Y side effects, error paths) and 5 new `for /f`
  cases (options parser, star capture, splitter, malformed options).
- Test main task stack raised to 16 KB (`CONFIG_ESP_MAIN_TASK_STACK_SIZE`)
  so the recursive calc/editor suites run on the board.
- Hardware spot checks: `calc 2^10`→1024, `calc x = sin(30)`→0.5,
  `echo a b | for /f "tokens=1" %%i in () do echo %%i`→a,
  `set /p v=< file`, `if errorlevel` interaction, redirection/pipes of
  `calc` output. Boot banner reports v0.32.0.

---

## [0.31.0] - 2026-08-14

### Added - serial file transfer (`receive` / `send`) + screenshot framing

The USB-Serial/JTAG binary-transfer surface is expanded into a coherent,
batch-friendly file-transfer and diagnostics toolset, sharing one framing
scheme with `screenshot`:

- **`receive <path> <size> [/crc]`** - host-to-device binary transfer into an
  SD file, ACK-paced so the device's USB RX ring never drops bytes. Fully
  backward-compatible with the previous two-argument form; the new `/crc` flag
  has the host append a 4-byte little-endian CRC-32 trailer which the device
  verifies (a mismatch removes the partial file and sets ERRORLEVEL 1).
  Reports bytes, elapsed time, and KB/s on success. ERRORLEVEL: 0 ok, 1
  transfer/IO/CRC error, 2 usage.
- **`send <path> [offset] [count]`** - device-to-host binary transfer of an SD
  file (or a byte range), streamed in a framed payload: 4-byte `SDFX` magic +
  4-byte little-endian size + raw bytes + `=== TX DONE ===` marker. Bounded by
  `P4_CONFIG_SERIAL_SEND_MAX_BYTES`. ERRORLEVEL: 0 ok, 1 IO error, 2 usage.
- **`send /diag`** - streams a compact diagnostic report (version, board, IDF,
  chip, heap free/internal/PSRAM, uptime, task count, cwd, Wi-Fi state) with the
  same framing, for host-side scripting and health checks.
- **Screenshot framing shared + hardened** - `screenshot`/`scr`/`capture` and
  `send` now share one frame-header writer, and all binary streams go through
  the USB-Serial/JTAG driver's raw TX (chunked) instead of `fwrite(stdout)`.
  This removes the console VFS's CRLF translation that silently corrupted binary
  payloads (every `0x0A` byte became `0x0D 0x0A`), and suspends the console
  reader during the stream so no echo/input interleaves. The `BMPX` magic is
  now the config value `P4_CONFIG_SCREENSHOT_BMP_MAGIC`; the on-the-wire
  protocol is unchanged (a valid 1024x600x24 BMP is verified).
- New tunables in `p4minishell_config.h` under `serial_transfer`
  (`P4_CONFIG_SERIAL_*`) and `P4_CONFIG_SCREENSHOT_BMP_MAGIC`, all documented in
  `p4minishell_config.yaml`.

### Hardened (0.31.0 follow-up)
- `send` frames now always end with a 4-byte little-endian CRC-32 trailer over
  the payload (matching zlib's `crc32`), so a host can verify a frame was not
  corrupted or interleaved with concurrent log output; empty payloads carry
  `0x00000000`.
- Raw serial writes are time-bounded (`P4_CONFIG_SERIAL_SEND_TIMEOUT_MS`,
  default 10 s) so a host that stops reading can never wedge the command
  worker — `send` aborts with ERRORLEVEL 1 instead of hanging. This matters for
  batch files run from the on-screen UI with no serial peer.
- `receive` error reporting now distinguishes a CRC/trailer failure from a
  short transfer ("transfer failed - CRC mismatch or missing trailer" vs
  "transfer incomplete").
- Extensively verified on hardware: 25-case hardening suite covering full
  round-trips with CRC on both directions, empty files, zero/overflow offsets
  and counts, oversized/invalid sizes, directory targets, interrupted receives
  (console stays responsive, no binary-poisoning), batch-file receive/send with
  `if errorlevel` branching, repeated stress round-trips, and usage errorlevels.

### Fixed
- Binary serial output no longer inserts `\r` before every `\n` byte (the
  console VFS CRLF translation was corrupting `send`/`screenshot` payloads).
- `serial_write_raw` chunks writes to fit the USB-Serial/JTAG TX ring, so a
  large single payload (e.g. a full screenshot) is never rejected by the ring.

### Changed — esp_hosted 3.0.6 upgrade + header notification-timer UAF fix

- Upgraded `espressif/esp_hosted` to `^3.0.6` (`esp_wifi_remote` stays
  `^1.6.4`); the ESP32-C6 co-processor was OTA-upgraded to 3.0.6 first, then the
  P4 host was rebuilt and flashed. This is the unified-release (RPC-V2) line.
- P4 host Kconfig migrated to the 3.x names (`ESP_HOSTED_HOST_*`, SDIO bus
  width/slot/pins/reset, `ESP_HOSTED_HOST_FEAT_BT`); `components/networking/
  bluetooth.c` now calls `esp_hosted_bt_host_stack_setup()`; `c6ota.c` and
  `networking.c` use the new `CONFIG_ESP_HOSTED` symbol.
- Patched the managed component's `esp_hosted_host_fw_ver.h` (reported `2.12.6`
  instead of `3.0.6`, which tripped the host/C6 version gate).
- **Fixed a heap-corruption / LVGL blue-screen bug exposed by the upgrade:** the
  header notification one-shot timer was a use-after-free —
  `header_notification_timeout_cb()` never cleared `s_notification_timer`, so
  the next notification called `lv_timer_set_period/reset/resume` on the
  auto-deleted timer node, writing `{period, last_run, paused}` into reclaimed
  heap memory. Cleared in `components/header/header.c`. Verified 10/10
  fresh-boot `wifi connect` cycles with no corruption, no panic, no blue screen.
- Hardened `lv_async_call()`/`lv_async_call_cancel()` in LVGL's `lv_async.c` to
  take the recursive `lv_lock()` (timers were created/deleted from foreign tasks
  while the LVGL task iterated the timer list).
- Transport buffers prefer PSRAM on this P4 (`CONFIG_EH_HOST_PORT_DMA_PREFER_SPIRAM`,
  plus `eh_host_port_dma_alloc` / RPC ctrl-cmd / RX frame copies) to preserve
  internal RAM. See `bugs.md` W2.

### Added — offline help, license/about, and build identity

Documentation and packaging polish that ships in the binary:

- **`help /all` and `help <command>`** - the help command is now a full offline
  command reference backed by a data table. `help` keeps the quick summary (with
  a pointer to `help /all`); `help /all` (or `help /?`) lists all 99 commands
  with one-line usage; `help <command>` prints a single entry and reports unknown
  names. The table mirrors `command.md`, so the on-device reference stays in sync
  with the docs.
- **`about` now surfaces the license and third-party summary** - a `License`
  section prints the proprietary notice (`P4_CONFIG_COPYRIGHT_NOTICE`) and a
  `Third-party components` list naming the principal open-source components with
  their SPDX identifiers (Apache-2.0, MIT, BSD-2/3-Clause).
- **Version string, build date, and Git hash visible everywhere** - `version`,
  `about`, and `sysinfo` now report the compile date/time and the Git hash (from
  `esp_app_get_description()`, e.g. `07b9812-dirty`) alongside the semantic
  version. A **long-press on the top status bar** shows the same build identity
  as a transient header banner (`P4MiniShell v0.31.0 | built <date> <time> |
  git <hash>`).
- New config knobs `P4_CONFIG_PRODUCT_NAME` and `P4_CONFIG_COPYRIGHT_NOTICE`
  (documented in `p4minishell_config.yaml`).

### Fixed (0.31.0 debug sweep)
- **Stale Kconfig symbol in `sdkconfig.defaults`** - `CONFIG_ESP_HOSTED_TRANSPORT_RESTART_ON_FAILURE`
  is a 2.x name that no longer exists in esp_hosted 3.x, so the build warned
  "unknown kconfig symbol" and the intended `=n` was silently dropped. The
  3.x default for `CONFIG_ESP_HOSTED_HOST_TRANSPORT_RESTART_ON_FAILURE` is `y`
  (restart the host on a runtime transport failure); the stale line meant the
  board would auto-restart instead of leaving the failure to the app. Renamed
  to the 3.x symbol so `=n` (handle in-app, no auto-restart) actually applies.
  Verified 0 errors / 0 warnings on both firmware and test builds.
- 0.31.0 sweep results: 119 unit tests pass; 68-command stress sweep with no
  crashes; functional checks (file ops, batch, `for`, `if`, `set /a`, clipboard,
  `fc`, `chkdsk`) pass; 5/5 WiFi connect cycles.

### Fixed — N1: shared-SDMMC host bring-up race (boot-time)

The SD card (SDMMC slot 0) and the ESP-Hosted C6 transport (slot 1) both
call the **non-thread-safe** `sdmmc_host_init()` at boot from different tasks;
a concurrent double-call corrupted the shared host driver and failed BOTH
devices ~1 in 15 boots. Fixes:

- **`storage_sdmmc_host_preinit()`** — initializes the SDMMC host once,
  synchronously, in `app_main` before `networking_init()`, so every later call
  from fatfs and esp_hosted hits the driver's idempotent skip and the race
  cannot occur.
- **Slot-scoped SD deinit** — `bsp_sdcard_mount()` (managed BSP) now deinits
  only slot 0 on failure/unmount (`sdmmc_host_deinit_slot(0)`) instead of the
  whole host, so a failed SD mount or `sdeject` never tears down the
  co-processor link.
- **Hosted bring-up retry** — `networking_wifi_runtime_init()` retries
  `esp_hosted_connect_to_slave()` once (deinit → 50 ms → init → connect) as
  defense-in-depth.

Verified: 30/30 reboot + `wifi connect` cycles with zero bring-up failures,
SD enumerated every boot, and `sdeject` no longer drops the hosted link
(`wifi status` stays "started", `sd mount` re-mounts). See `bugs.md` N1.

---

## [0.30.0] - 2026-08-13

### Milestone — version renumbering + first full test-and-debug campaign

Version numbering moves from 0.24.x to 0.3X.X. This release is the result of a
full command-surface, unit, and stress-test campaign on the board (see the new
`bugs.md`, which replaces the old one). Eight firmware bugs were found and fixed:

- **Critical — intermittent `abort()` crash (internal-DRAM heap exhaustion).**
  The 48 KB transcript scrollback buffers lived in internal DRAM; combined with
  newlib's per-`snprintf` lock allocation this drove internal free to ~1.7 KB and
  caused random `abort()` reboots on any output-heavy command. The three 16 KB
  transcript buffers (`s_transcript`, `s_transcript_ansi`, `s_transcript_staged`)
  now live in PSRAM. Internal free is stable at ~132-179 KB and the
  previously-100%-crashing 22-command sweep runs clean.
- **Critical — `edit` crashed (Store access fault) when typing text + Enter.**
  `editor_doc_split_line()` used a `&doc->lines[row]` pointer across
  `editor_doc_insert_line()`, which can realloc/shift the array (use-after-free).
  The line pointer is now re-fetched after the insert.
- **High — `goto :label` broken.** `goto` prepended an extra `:` (so `goto :skip`
  became `::skip`) and the label file position was computed with the trimmed line
  length, jumping a byte into the line. Both fixed.
- **Medium — literal `@`-palette markers** in `rgb status`, `bluetooth status`, and
  `config` (palette macros passed as `%s` arguments). Markers moved into the format
  strings.
- **Medium — `copy`/`move` to a directory destination failed.** A destination that
  is an existing directory now keeps the source basename (`copy f.txt dir/`).
- **Medium — `move dir\f.txt name.txt` resolved `name.txt` in the source's
  directory** (like `ren`) instead of the cwd. `move` now resolves cwd-relative.
- **Medium — `if cond (cmd) else (cmd)` parenthesized blocks failed.**
  `if` now parses balanced `(…)` blocks and the `else` branch.
- **Medium — `call :label` unimplemented.** Implemented label subroutines with
  `exit /b` / `goto :eof` / EOF return to the caller.
- **Low — interactive `for` was unknown at the prompt.** `for` only ran inside
  batch files. It is now dispatched at the prompt (`for %i in (set) do cmd`),
  accepting the single-`%` prompt form and the `%%` batch form, with token sets,
  wildcard sets, and per-iteration nested commands (e.g. `set /a` and pipes)
  verified on board. The batch `for` path is unchanged.
- **Low — caret escape `^c` was not stripped from `echo` output.** `echo a^&b`
  printed `a^&b` (the caret was kept). `echo` now consumes caret escapes when
  printing (`a^&b` → `a&b`, `a^^b` → `a^b`, `a^|b` → `a|b`), preserves quotes
  (`echo "a^&b"` → `"a&b"`), keeps a caret inside single quotes literal, and
  `echo ^on` prints `on` instead of toggling the batch echo flag.
- **Low — `clip` subcommand typos silently clobbered the clipboard.**
  `clip status` (or any unknown first word that looks like a subcommand) is now
  reported as a usage error with errorlevel 2 instead of overwriting the current
  clipboard with the word itself; literal clipboard text (`clip hello world`)
  is unchanged.

### Verification

- Both firmware and test project build with 0 errors / 0 warnings.
- On-board unit suite passes: 119 tests, 0 failures (before and after the fixes).
- Full on-board command sweeps (system, hardware, filesystem, SD, batch, pipes,
  chains, redirection, time, network-status, history, clipboard, editor, stress)
  run clean: no crashes, no unknown commands, no literal `@` markers.
- Network sweep against live AP `4G-CPE_5542` (password `1234567890`):
  `wifi scan`/`diag` find the AP, and `wifi status`, `ipconfig`, `netstat`,
  `httpd status`, `bluetooth status`, `ping`, `dns`, `httpget` all work or
  degrade gracefully. **`wifi connect` has a pre-existing CRITICAL bug** (W1 in
  `bugs.md`): it corrupts the lwIP internal heap and panics on the netif
  IP-lost/DHCP timer a few seconds after connecting. It is not caused by this
  campaign's code (confirmed by reverting the memory-layout changes) and needs
  component-level work on the `esp_wifi_remote`/`esp_hosted`/lwIP integration on
  IDF 5.5.5 — the `edit` save/quit round-trip is verified working with the
  documented `\s`/`\q` verbs.

---

## [0.24.41] - 2026-08-13

### Added - first-run / no-SD guidance

The shell now feels intentional the first time it boots, with or without an SD
card:

- **No-SD boot is no longer silent.** When no card is present, the boot path
  prints a muted line on the display and serial console ("No SD card detected -
  insert a microSD card to use files, config, and scripts; type help"), shows a
  header notification, and records a debug-log entry. Previously the only
  message was an `ESP_LOGI` that is compiled out at the WARN log level.
- **Minimal CONFIG.SYS + AUTOEXEC.BAT on first card mount.** The SD card is
  already mounted lazily by the first SD command; a new one-shot first-mount
  hook (`storage_register_sd_first_mount_callback`) now generates the
  documented default boot files when they are missing and prints a short
  "SD card ready" welcome (mentioning first-run file creation when it happens).
  Existing user files are never touched.
- **`sd mount`** clears the eject latch so a card re-inserted after `sdeject`
  can be used again without rebooting (previously the latch blocked re-mount
  until restart).
- **Richer `help`**: shows a "No SD card detected" note when the card is
  absent, and a compact **Getting Started** footer
  (`dir | cd <path> | write <file> <text> | type <file> | edit <file> |
  config | wifi connect <ssid> <pass>`).

### Added - new public API

- `storage_register_sd_first_mount_callback(cb)` — one-shot per-boot first-mount hook.
- `shell_command_sd_mount()` — the `sd mount` subcommand.
- `boot_ensure_default_files()` — idempotent CONFIG.SYS/AUTOEXEC.BAT generation.
- `boot_on_sd_first_mount()` — the welcome callback registered by `main.c`.

### Verification

- Clean build: 0 errors, 0 warnings (firmware and test project).
- On-board (COM11): full unit suite passes; boot with no SD shows the guidance
  line + header notification; inserting a card and running the first SD
  command prints "SD card ready" and generates missing CONFIG.SYS/AUTOEXEC.BAT;
  `sdeject` then `sd mount` re-mounts; `help` shows the SD note and the
  Getting Started footer.

---

## [0.24.40] - 2026-08-13

### Added - `config` command: persistent settings in CONFIG.SYS + factory reset

Shell settings that previously lived only in RAM (or a one-shot CONFIG.SYS
edit) can now be persisted and restored from the shell:

- **`config`** — show every tracked setting (current value, default, and
  whether it is saved in CONFIG.SYS).
- **`config <key>=<value>`** (or `config <key> <value>`) — apply a setting now
  and write it into `sd:/CONFIG.SYS`, so the boot component re-applies it on
  the next boot with zero new boot code. Tracked keys: `BRIGHTNESS`, `ROTATE`,
  `VOLUME`, `PROMPT`, `WIFI_AUTOCONNECT`, `DISPLAY_TIMEOUT`, `OSK`, `HEADER`.
- **`config save`** — persist the current value of every tracked setting into
  CONFIG.SYS. **`config reset [key]`** — restore the default(s) in RAM and
  remove the directive line(s). **`config <key>`** — show one setting.
- **`config factory`** — destructive-confirmation-gated full reset: restores
  every tracked default, clears history/aliases/known-networks, and deletes
  CONFIG.SYS, AUTOEXEC.BAT, WIFI.KNOWN, ALIASES.BAT, and HISTORY.TXT. The
  default boot files are regenerated at the next boot.

CONFIG.SYS is edited in place with a guarded, atomic temp-file+rename write
(the same pattern as `WIFI.KNOWN`/`alias /save`); unknown directives, comments,
and line order are preserved. Two new boot directives make the remaining
RAM-only UI settings persistable:

- **`OSK=ON|OFF`** — show/hide the on-screen keyboard at boot.
- **`HEADER=ON|OFF`** — show/hide the header status bar at boot (the window
  manager reports a zero-height header region when hidden, so the transcript
  expands to fill the space).

Supporting additions: `header_set_visible()`/`header_get_visible()` (header
visibility survives UI rebuilds), `display_schedule_ui_rebuild()`,
`networking_wifi_get_boot_autoconnect()`, and `shell_confirm_destructive()`
was made public so `config factory` shares the exact same "YES" gate as
`format`/`disk clean`/`trash purge`. `P4_CONFIG_CONFIG_MAX_BYTES` (16 KB)
bounds the CONFIG.SYS file size the command reads/writes.

### Verification

- Clean build: 0 errors, 0 warnings (firmware and test project).
- On-board (COM11): full unit suite passes (13 new `config` directive
  round-trip tests, 0 failures); `config` show/set/reset/save verified; a
  persisted `BRIGHTNESS=40` survives `reboot`; `config factory` confirms with
  "YES", restores defaults, and deletes all five persistence files.

---

## [0.24.39] - 2026-08-13

### Fixed - shell hardening: redirection capture, long write/append, pipe stack, long echo

- **`> file` / `>> file` no longer truncate at the 16 KB transcript.** A
  redirected command's output is now mirrored into a dedicated heap capture
  (up to `P4_CONFIG_REDIRECT_CAPTURE_MAX_BYTES`, 256 KB) as every transcript
  append happens, so `type <large> > out.txt` writes the command's FULL output
  even when the on-screen transcript drops the oldest half. The write reports
  truncation if the capture cap itself is exceeded. Verified on board: a
  19.5 KB `type > out` round-trips to exactly the source size.
- **`write` and `append` no longer silently truncate long text.** The joined
  value is heap-allocated at the full command-line size (`P4_CONFIG_COMMAND_BYTES`)
  instead of a fixed stack buffer, so multi-kilobyte values survive
  (verified ~3.9 KB per write/append, and a 19.5 KB file built from five of them).
- **Long pipelines no longer overflow the command worker task.** The pipe
  executor's stage redirection buffer was a 2x command-sized stack local;
  it is heap-allocated now, so a pipe appearing inside a nested batch file
  cannot blow the 8 KB worker stack.
- **`echo` prints the whole remainder of a long line.** The echo branch
  snapshots the raw, unsplit command line before the argv cap applies, so a
  full 4096-byte echo (verified 40 tokens) prints everything. Non-echo commands
  with more arguments than the argv capacity now report a warning instead of a
  hard error.
- **Transcript overflow is bounded, not a boot crash.** Every append path is
  clamped to the buffer size and, when output exceeds capacity, the oldest half
  is dropped and a `[history truncated]` marker is appended — removing the
  overflow that aborted at boot.
- **`findstr` regex engine fixes.** The matcher now advances past a complete
  atom, so bracket classes (`[ab]c`) and escapes (`a\*b`, `a\.b`) match
  correctly, and an escaped `\*` is a literal star rather than a quantifier.
  (The `^abc` end-position assertion in `test_findstr.c` was also corrected to
  the true match extent.)

### Added

- `P4_CONFIG_REDIRECT_CAPTURE_MAX_BYTES` (256 KB) — redirect capture cap,
  documented in p4minishell_config.yaml.

### Verification

- Clean build: 0 errors, 0 warnings (firmware and test project).
- On-board (COM11): full unit suite passes (120 tests, 0 failures); boot
  banner clean; `ver` reports v0.24.39; 40-token echo, `echo hello |
  findstr hello`, and `write`/`append` of 3.9 KB values all pass; `type
  <19.5 KB> > out` writes the full file.

---

## [0.24.38] - 2026-08-13

### Added - full touch-keyboard compatibility for `edit`

Every editor feature is now reachable from the on-screen touch keyboard alone,
with no USB keyboard required. The single navigation page becomes two:

- **Nav page** (`Nav` on the symbols page): `Tab`, `Ins`, `Del`, arrows,
  `Home`/`End`, `PgUp`/`PgDn`, `Find`, `Next` (repeat find), `Rep`, `Goto`,
  `Undo`, `Redo`, `Save`, `SaveAs`, `Quit`, `Nav2` (-> Edit page), `abc`.
- **Edit page** (`Nav2` on the Nav page): `Copy`, `Cut`, `Paste`, `SelAll`,
  `WdL`/`WdR` (word left/right), `DocH`/`DocE` (document home/end),
  `DelLn` (delete line), `DelE` (delete to end of line), `Nav1` (-> Nav page),
  `abc`.

`Next` maps to repeat-find (the OSK equivalent of `F3`); `Nav1`/`Nav2` switch
the two pages and are routed by the shell keyboard callback
(`KEYBOARD_MODE_NAV2` -> LVGL `USER_2`). All of these were previously
USB/serial-only: undo, redo, copy, cut, paste, select-all, word navigation,
document home/end, delete line, delete-to-end-of-line, and repeat-find.

### Verification

- Clean build: 0 errors, 0 warnings (firmware and test project).
- On-board (COM11): full unit suite passes; every `edit` feature is covered by
  a Nav/Edit-page button, and the earlier serial smoke tests (create/type/
  save, round-trip, LF/CRLF, undo/redo, select-all+replace, find, goto,
  save-as, quit-confirm, tabs, multi-line, `.bat`) still pass with no
  regressions.

---

## [0.24.37] - 2026-08-13

### Added

- **Line-number gutter** in the `edit` editor: every row is prefixed with its
  1-based line number, right-aligned in a fixed-width muted gutter
  (`P4_CONFIG_EDITOR_LINE_NUMBER_WIDTH_CHARS`, default 4) followed by a space.
  The gutter is render-only (never part of the document); the cursor,
  selection overlay, and touch mapping are offset past it so caret/tap/select
  stay byte-aligned. Toggle with `P4_CONFIG_EDITOR_LINE_NUMBERS`.
- **Current-line highlight**: the cursor's row gets a subtle full-width
  background bar behind the text (`P4_CONFIG_EDITOR_CURRENT_LINE`,
  `P4_CONFIG_EDITOR_CURRENT_LINE_COLOR`).
- **Serial verbs `\g` (Go to Line) and `\o` (Save As)** now complement the
  existing `\q \s \f \u \r \a`, so every prompt-driven feature is reachable
  from the serial console.
- **Pure line-number formatter** `editor_format_line_number()` (right-aligns a
  1-based number into a fixed digit width plus a space), unit-tested.
- **Complete editor tutorial**: new `tutorial_edit.md` covers every feature
  with edge cases and full touch/USB/serial cheat sheets; linked from
  `readme.md` and `command.md`, cross-referenced from `editor.md`.

### Configuration

- `P4_CONFIG_EDITOR_LINE_NUMBERS`, `P4_CONFIG_EDITOR_LINE_NUMBER_WIDTH_CHARS`,
  `P4_CONFIG_EDITOR_CURRENT_LINE`, `P4_CONFIG_EDITOR_CURRENT_LINE_COLOR`
  (documented in p4minishell_config.yaml).

### Verification

- Clean build: 0 errors, 0 warnings (firmware and test project).
- On-board (COM11): full unit suite passes (24 editor + keyboard dedup + new
  `test_editor_format_line_number`); serial smoke tests cover create/type/
  save/read-back, byte-identical round-trip, LF and CRLF preservation,
  undo/redo, select-all+replace, find, go-to-line, save-as, quit-confirmation,
  tabs, blank-line handling, a 100-line file, and a `.bat` file — all pass
  with no hangs or crashes.

---

## [0.24.36] - 2026-08-13

### Fixed

- **On-screen keyboard doubled every letter/symbol** (pressing `A` produced
  `AA`). `keyboard_register_event_callback` used `lv_obj_remove_event_cb(widget,
  NULL)`, which is a NO-OP in LVGL (it only removes callbacks whose cb pointer
  equals NULL), so the LVGL default keyboard handler stayed registered next to
  the shell's callback and BOTH processed each button. Registration now removes
  every callback descriptor explicitly, leaving exactly one handler.
- **The `edit` surface collapsed to ~one line and dragged the touch keyboard up
  under it.** `windows_enter_editor_mode` hid the transcript container, which is
  the editor surface; LVGL flex skips hidden children, so the editor area
  collapsed and the keyboard jumped up. The transcript container now stays
  visible as the editor surface at the transcript-region height — the editor
  area is exactly as large as the normal shell area and the keyboard stays at
  the bottom. The editor already hides the shell span group, so no shell output
  leaks through.
- `keyboard_bind_textarea(NULL)` never cleared the LVGL widget's binding, so a
  stray handler could type into the hidden shell input line while the editor
  was open; it now truly unbinds.
- The shell's transcript/input-line LVGL callbacks re-bound the keyboard textarea
  and summoned the OSK on taps; they now no-op while the editor owns the surface.

### Added - preventative features

- **OSK input deduplication** (`keyboard_osk_accept` / pure
  `keyboard_osk_accept_at`): the shell drops a re-fire of the same button id
  within `P4_CONFIG_OSK_DEBOUNCE_MS` (30 ms) before routing it, so double input
  is impossible even if a duplicate handler is ever re-registered. Fast typing
  and auto-repeat are far slower than the window and are never merged.
- **Keyboard callback audit** (`keyboard_event_callback_count`): registration
  logs how many handlers are on the widget and flags any value other than one
  (`DOUBLE INPUT RISK`), so a future duplicate-handler regression is visible in
  the serial log instead of as silent doubled characters.
- **Idempotent, self-cleaning registration**: every registration removes all
  prior callbacks (default handler included) by descriptor, so duplicates can
  never accumulate across boot or rotation rebuilds.
- **Textarea binding diagnostic** (`keyboard_is_textarea_bound`): the editor can
  assert the OSK is unbound from the shell input line while it owns it.
- **Editor-surface reflow helper** (`windows_refresh_editor_surface`) + entry
  self-check (`windows_editor_surface_height_ok`) + layout diagnostic
  (`windows_debug_editor_layout`, `windows_get_editor_surface`): the editor
  surface is always sized to the transcript region (on entry and on keyboard
  show/hide) and a collapsed surface logs a warning immediately.

### Configuration

- `P4_CONFIG_OSK_DEBOUNCE_MS` (30) — documented in p4minishell_config.yaml.

### Verification

- Clean build: 0 errors, 0 warnings (firmware and test project).
- On-board (COM11): `Keyboard callback audit: 1 handler(s) registered` in the
  boot log; `editor layout diagnostic: surface h=300 px, transcript region
  h=300 px` on `edit` — the editor area matches the shell transcript exactly and
  the keyboard sits at its normal height at the bottom.
- New `test_keyboard_osk_dedup` unit suite (debounce window, boundary, fast
  repeat) passes on the board alongside the full editor suite (24/24).
- Edit create/type/save/read-back and quit smoke tests still pass with the
  surface visible.

---

## [0.24.35] - 2026-08-13

Editor hardening, DOS-EDIT search parity, and a reusable modal-surface pattern.

### Added - `edit` DOS-EDIT parity features

- **Find / Repeat / Replace / Go-to-Line**: `Ctrl+F` (or the nav-page `Find`)
  searches forward from the caret and wraps; `F3` / `Enter` repeats the last
  search; `Ctrl+H` (`Rep`) replaces one match at a time with `Enter` repeating;
  `Ctrl+G` (`Goto`) jumps to a line number. Search strings are typed into the
  status bar and cancelled with `Esc`.
- **Save As**: `Ctrl+O` / `SaveAs` prompts for a new path (pre-filled with the
  current one); after a successful save the editor retitles to the new path.
  Unnamed buffers now prompt for a name instead of silently saving to
  `EDIT.NEW`.
- **Overwrite toggle**: `Insert` / `Ins` flips insert/overwrite; the status bar
  shows `INS` / `OVR`.
- **Word navigation** (`Ctrl+Left` / `Ctrl+Right`), **document start/end**
  (`Ctrl+Home` / `Ctrl+End`), **delete line** (`Ctrl+Y`), and USB shift-arrow
  selection with an on-screen selection overlay.
- **Quit confirmation**: Esc quits immediately only when the file is clean;
  with unsaved changes it asks `Quit without saving? (Y/N)`.
- **Touch keyboard**: the symbols page now carries a `Nav` button that opens an
  editor navigation page (Tab, Ins, Del, arrows, Home/End, PgUp/PgDn, Find,
  Rep, Goto, Save, SaveAs, Quit). Mode switching (abc / ABC / 1# / Nav) is now
  handled by the shell keyboard callback — the on-screen keyboard's text pages
  and symbols page are reachable again.
- **Visible cursor + syntax colours**: the editor renders through a dedicated
  span group with a blinking block cursor, a selection background overlay, and
  the batch lexer actually applied (commands, comments, labels, `%VAR%`,
  strings, operators). Content no longer routes through the shell transcript
  staging buffer, so large files no longer get clipped at 16 KB.

### Fixed

- **Data-loss guard**: an existing file that cannot be loaded (over the
  byte/line limits or an I/O error) now refuses to open instead of silently
  creating an empty buffer that could be saved over the original file.
- **LF multi-line selection copy** wrote a lone `\r` for LF files, so pasting
  a multi-line selection joined the lines; it now uses the file's own EOL.
- **Trailing-newline round-trip**: pressing Enter no longer forces a phantom
  trailing newline on save; a file's EOL tail is preserved exactly.
- **Paste no longer pollutes undo** with one entry per pasted line.
- **Partial-destination cleanup** on a failed editor save (the shell's
  storage guardrail), matching `copy`/`write` behaviour.
- **Serial `\r` redo** now maps to `Ctrl+Shift+Z` (DOS `Ctrl+Y` is delete
  line).
- **Rotation while editing**: a display rotation closes the editor view and
  wakes the worker cleanly instead of leaving dangling LVGL widgets.
- **Status bar format-safety**: a file path containing `%` can no longer be
  interpreted as a format string.
- **OSK shell typing restored**: the keyboard's only event handler now routes
  every button into the command input line (letters, backspace, arrows,
  Enter/OK, hide), which the replaced LVGL default handler previously did.
- **Crash: pressing Enter on an empty buffer** dereferenced a NULL line-text
  pointer (`editor_doc_split_line`); the tail write is now NULL-guarded.
- **Crash: deleting a selection that starts on an empty line** wrote through a
  NULL line-text pointer (`editor_doc_selection_delete`).
- **Buffer overflow: a >127-char Find/Replace string** overflowed the
  128-byte search buffers (`P4_CONFIG_EDITOR_FIND_BYTES`); the find/replace
  prompt input is now capped at that size.
- **Memory leak: undo/redo snapshots were never freed** when a document was
  released — every session leaked up to `EDITOR_UNDO_DEPTH` full-document
  copies (up to ~4 MB); `editor_doc_free` now releases them.
- **Undo/redo corruption on low memory**: `editor_doc_deserialize` freed the
  old lines before allocating the new ones, so an allocation failure mid-undo
  left the document half-freed; it now builds-then-swaps atomically.
- **Multi-row selection delete kept the wrong text** (kept the selected head
  of the last line instead of its un-selected tail) and used a stale line
  index after the intermediate removals, so the merge could be skipped
  entirely (found by the on-board test suite).
- **Worker hang when the LVGL view failed to open**: the session stayed
  blocked forever waiting for a quit; it now wakes the worker and returns an
  error.
- **`s_session_active` leaked** on the `lv_async_call` failure path, routing
  serial input to a dead editor.
- **Display-rotation deadlock**: `editor_view_close` now wakes the worker
  session, so a rotation mid-edit releases the document instead of blocking
  the `edit` command forever.
- **A save requested in the same instant as a quit was dropped** (the worker
  processed QUIT first); saves are now serviced before quit handling.
- **Use-after-free window on close**: the worker could free the document
  while a slow LVGL task was still closing the view; the close wait now polls
  `editor_view_is_open()` instead of a blind timeout.
- **Paste beyond the line cap silently dropped lines**; a failed line split
  now stops the paste.
- **Unbounded line growth during editing** (`Enter`/paste could exceed
  `P4_CONFIG_EDITOR_MAX_LINES`); the line insert is now capped.
- **Word navigation on an empty line** dereferenced a NULL line-text pointer.
- **Partial widget-creation failures in `editor_view_open`** now clean up and
  fail instead of leaving a half-built surface, and a failed deferred rebuild
  falls back to a synchronous render.

### Configuration

- `P4_CONFIG_EDITOR_FIND_BYTES`, `P4_CONFIG_EDITOR_PROMPT_BYTES`,
  `P4_CONFIG_EDITOR_CURSOR_BLINK_MS`, `P4_CONFIG_EDITOR_SELECTION_COLOR`,
  `P4_CONFIG_EDITOR_FIND_CASE_SENSITIVE`
  (documented in p4minishell_config.yaml).

### Verification

- Clean build: 0 errors, 0 warnings (firmware and test project).
- New `test/main/test_editor.c` unit suites cover the document model:
  insert/cursor, newline split/join, delete forward, overwrite toggle, delete
  line/EOL, document home/end, find/replace (case + wrap), undo/redo,
  selection (LF and CRLF copy, multi-row tail merge, empty-start delete),
  paste multi-line, word navigation on empty lines, Enter-on-empty-doc,
  undo-ring wraparound/cleanup, line-cap bounds, and the batch lexer.
- On-board run (COM11, `test/` project): all 24 editor suites PASS.
- Hardware smoke tests: create+type+save+read-back, load round-trip
  (byte-identical), quit-confirmation discard, find over serial, and
  modify+save of an existing file all pass with no hangs or crashes.

---

## [0.24.34] - 2026-08-12

### Added - DOS-style `edit` text editor

- `edit <path>` opens a modal, touch-first text editor for any byte-preserving
  file (batch, txt, sys, or any other extension). Full inline editing: insert,
  backspace, forward Delete, Enter, Tab-to-next-stop, Home/End, Up/Down,
  PageUp/PageDown; text selection (Shift+arrows / Ctrl+A via USB,
  long-press-and-drag via touch) with copy/cut/paste through the RAM clipboard;
  snapshot-based undo/redo.
- Files round-trip unchanged: CRLF vs LF is preserved, tabs and 8-bit bytes
  kept exactly. Batch `.bat`/`.cmd` files get syntax highlighting.
- The editor is fully usable from the on-screen touch keyboard (a nav page adds
  Tab, arrows, Home/End, Del, Ins, Save, Quit) and from the UART console
  (`\q`/`\s`/`\u`/`\r`/`\a` verbs, or typed lines). A USB keyboard/mouse expand
  it automatically (Ctrl+S/X/C/V/Z/Y/A, mouse-wheel scroll).
- New `components/editor/` with a byte-preserving document model, batch lexer,
  LVGL surface, and a worker-task session (file I/O stays off the LVGL task).

### Fixed

- **Editor quit now actually wakes the worker.** `editor_view_handle_usb_key`
  handled `EDITOR_KEY_QUIT` with an early return that set only a flag and never
  signalled the session event group, so the modal editor session stayed blocked
  after Esc / `\q` (appearing as a frozen UI or, under load, a watchdog reset).
  Quit and Save now route through the event-group helpers.
- **LF files round-trip correctly.** `editor_doc_save` wrote the first byte of
  the `"\r\n"` literal when `crlf` was false — emitting a lone `\r` instead of
  a `\n`, so every LF-only file saved by the editor lost its line breaks.
  The EOL is now selected explicitly.
- **On-screen keyboard visibility no longer breaks the flex layout.** The
  (previously dead) visibility callback wired into the window manager set a
  manual y-coordinate on the flex-column input row, throwing the keyboard to
  the top of the screen with a black void below. It now only re-applies the
  transcript height; flex positions the regions.
- **The shell display returns after `edit`.** `windows_enter_editor_mode`
  hides the transcript container for the editor session but the exit path
  never un-hid it, so the first edit left the shell's main display blank (the
  keyboard appeared against a black void) until a reboot. Exiting now restores
  the transcript, and repeated edit sessions leave the shell intact.
- USB Delete now deletes forward; USB Up/Down history recall no longer inverted.
- `audio_play` task stack raised 4096 -> 8192: tone generation overflowed the
  small stack (surfaced once heap poisoning was enabled), causing a watchdog
  reboot on `tone`.
- LVGL task stack returned to 12288 (was temporarily 32768 during debugging);
  the oversized stack was eating internal RAM and starving SD DMA buffers under
  load, causing intermittent `allocate_dma_buf: not enough mem` on `xcopy`.
- Diagnostic defaults in `sdkconfig.defaults`: panic hold 5 s, comprehensive
  heap poisoning, and task-watchdog-panics so any future fault is attributable.

### Configuration

- `P4_CONFIG_EDITOR_MAX_BYTES`, `P4_CONFIG_EDITOR_MAX_LINES`,
  `P4_CONFIG_EDITOR_UNDO_DEPTH`, `P4_CONFIG_EDITOR_TAB_WIDTH`,
  `P4_CONFIG_EDITOR_LINE_HEIGHT`, `P4_CONFIG_EDITOR_SYNTAX_BATCH`
  (documented in p4minishell_config.yaml).

### Verification

- Clean build: 0 errors, 0 warnings (firmware and test project).
- Hardware (COM11): `edit` opens, typed text is saved and read back with line
  breaks intact, and `\s`/`\q` work reliably across repeated sessions; audio,
  long commands, history, dir, chains/redirection, clipboard, top/ps, and
  xcopy/findstr/comp/find all pass the regression suite with no reboots.

---

## [0.24.33] - 2026-08-12

Tab completion, a heap-backed history with SD save/restore, and extremely
long command lines (4096 bytes, 16x DOS's 127-char limit). The command
pipeline's transient buffers moved to the heap so long commands never
overflow the worker/UART stacks.

### Added - Tab completion (USB keyboard)

- Tab completes the current word: the first token completes command names,
  aliases, and `.bat` files; any later token completes SD file/directory
  paths (directories get a trailing `/` so completion keeps going). A unique
  match fills in, repeated Tab cycles the matches, and the first Tab with
  several matches lists them (muted) in the transcript. Wired through a new
  `shell_command_ops_t.complete_word` hook registered by `command_init`.

### Added - heap-backed history + SD save/restore

- History is now heap-backed (`strdup`'d lines) with depth
  `P4_CONFIG_COMMAND_HISTORY_DEPTH` (32) and a total-byte cap
  `P4_CONFIG_HISTORY_TOTAL_BYTES` (64 KB), so very long commands are kept
  without a fixed RAM grid. Up/Down recall and password masking are unchanged.
- `history` lists the numbered recall buffer (redirectable/pipable);
  `history /save [file]` writes it to SD (default `P4_CONFIG_HISTORY_PROFILE`,
  atomic write with partial-file cleanup); `history /load [file]` restores it
  (append, dedupe); `history /clear` clears RAM history.

### Changed - extremely long command lines

- `P4_CONFIG_COMMAND_BYTES` raised 256 -> 4096. All command-sized stack
  buffers in the pipeline moved to the heap: the UART console line + submit
  copies, `shell_input_line_set_text`/repair, the history mask/recall buffers,
  and the worker-queue `command_request_t` (plus a chain buffer sized to the
  full command). Batch-line length is unchanged.
- The command worker queue now stores pointers to heap requests instead of
  four 4096-byte copies, so the internal heap is not pinned by a 16 KB queue.

### Fixed - long-line truncation found during verification

- `shell_uart_console_task` read into a heap line buffer but used
  `sizeof(line)` (the pointer size) as its capacity, so long serial commands
  were split into 3-char fragments. It now uses `SHELL_COMMAND_BYTES`.
- `shell_execute_command_segment` expanded commands into `SHELL_BATCH_LINE_BYTES*2`
  (768-byte) buffers, truncating interactive lines. Now command-sized.
- `shell_command_echo` joined its arguments into a 384-byte batch-line buffer;
  an interactive `echo ... > file` was cut at 384 bytes. Now command-sized.
- `shell_transcript_appendf` used a fixed 512-byte stack buffer, so long
  command output (and the redirected file) was cut at 511 bytes. Now
  command-sized and heap-allocated.

### Fixed - boot-loop and grey-screen regressions

- The first v0.24.33 build boot-looped with a stack-protection fault in
  `taskLVGL`: the 7168-byte default LVGL task stack was overflowed by the
  deep redraw recursion. The LVGL task stack is now `P4_CONFIG_LVGL_TASK_STACK`
  (12288 bytes), confirmed healthy via `top` (taskLVGL high-water ~9.8 KB).
- Rebooting the LVGL port config from the defaults dropped `timer_period_ms`
  (and `task_max_sleep_ms`), which stopped the LVGL tick timer: the device
  booted with a uniform grey panel while the framebuffer still held the UI.
  `display.c` now starts from `ESP_LVGL_PORT_INIT_CONFIG()` (all defaults) and
  only overrides the task stack.
- The input-line `VALUE_CHANGED` handler ran a 4096-byte `typed` local on the
  LVGL task; it is heap-allocated now, as are the `LV_EVENT_READY` command and
  transcript copies.

### Configuration

- `P4_CONFIG_COMMAND_BYTES`, `P4_CONFIG_COMMAND_HISTORY_DEPTH`,
  `P4_CONFIG_HISTORY_TOTAL_BYTES`, `P4_CONFIG_HISTORY_PROFILE`,
  `P4_CONFIG_COMPLETION_MAX_MATCHES`, `P4_CONFIG_LVGL_TASK_STACK` (documented
  in p4minishell_config.yaml).

### Verification

- Clean build: 0 errors, 0 warnings (firmware and test project).
- Hardware (COM11): boots clean, no boot loop, no panics; a ~3900-char
  `echo ... > file` command runs end to end and the file reads back at full
  size; `history` lists, `/save`, `/clear`, `/load`, and a bad-path `/save`
  rejection all behave; Tab-completion hooks and the input line are intact;
  regressions pass across serial console, clipboard, dir, chains +
  redirection, top/ps sorting, audio, and xcopy/findstr/comp/find.

---

## [0.24.32] - 2026-08-12

Clipboard / copy-paste for the transcript and the SD filesystem — a RAM
clipboard in the shell core (`clip` / `paste`), batch-safe and DOS-flavoured.
On a touch + USB-keyboard device this makes copying the last lines of output
or a file and reusing them trivial.

### Added - `clip` / `paste`

- `clip` prints the clipboard (`clipboard: <text>` / `(file: <path>)` /
  `(empty)`); redirectable, so `clip > note.txt` saves it.
- `clip <text>` sets the clipboard to text (`clip hello world`).
- `clip copy [N]` copies the last N transcript lines (default 1, cap
  `P4_CONFIG_CLIP_COPY_LINES_MAX`) into the clipboard — the same read the `>`
  redirection path uses.
- `clip file <path>` stores a file reference in the clipboard;
  `paste <dest>` copies that file to a destination (or into a directory,
  keeping its name) — Windows-style file copy-paste.
- `clip read <file>` loads a text file's contents into the clipboard (bounded
  by `P4_CONFIG_CLIPBOARD_BYTES`), so `clip read x.txt` then `clip > y.txt`
  round-trips text.
- `paste` injects the clipboard into the input line at the cursor
  (`lv_textarea_add_text`), so a copied path or line can be edited and
  submitted immediately. All verbs set an ERRORLEVEL (0 ok / 1 empty|missing /
  2 usage) and are redirectable/pipable for batch use.

### Configuration

- `P4_CONFIG_CLIPBOARD_BYTES` (2048), `P4_CONFIG_CLIP_COPY_LINES_MAX` (64)
  (documented in p4minishell_config.yaml).

### Fixed - on-screen keyboard recoverability

- Tapping the transcript now summons the on-screen keyboard when it is hidden
  and no USB keyboard is driving the input line (previously only tapping the
  input line did), so the OSK cannot get "stuck" hidden on a touch device.
- `keyboard status` now reports the external-input state (`external=on|off`)
  so the auto-hide behavior is diagnosable.

### Verification

- Clean build: 0 errors, 0 warnings (firmware and test project).
- Hardware (COM11): `clip` / `clip hello` set and show; `clip copy 3` grabs
  the transcript tail and `paste` injects it into the input line (submitting
  it runs the copied text); `clip file CONFIG.SYS` then `paste backup\` copies
  the file; `clip read CONFIG.SYS` then `clip > out.txt` round-trips the text;
  `clip > file` redirects the clipboard; batch `clip copy 1 && echo ok` and
  `if errorlevel` work; `copy`/`move`/`type`/input line show no regressions.
  `keyboard status` reports `external=off`, `keyboard show`/`toggle`/`hide`
  work, and the OSK is left visible.

---

## [0.24.31] - 2026-08-12

Basic audio surface: the ES8311 speaker now actually makes sound. `beep` and
`tone <freq> [ms]` play tones, `wavplay` streams a short 16-bit PCM WAV from
SD, `volume` gained a query form, and all of it is batch-friendly — playback
runs on a small background task so nothing blocks a batch file, and every
command sets a real ERRORLEVEL.

All audio logic lives in a dedicated **`components/audio`** component (codec
init, speaker volume, and the background playback engine in `audio.h` /
`audio.c`); the `beep`/`tone`/`wavplay`/`audio`/`volume` commands in
`components/command/` only parse arguments and call the `audio.h` API.

### Added - beep / tone

- `beep` plays a short default tone (`P4_CONFIG_BEEP_FREQ_HZ` 880 Hz,
  `P4_CONFIG_BEEP_DURATION_MS` 100 ms).
- `tone <freq> [ms]` plays a sine wave (freq 20..20000 Hz, duration
  10..`P4_CONFIG_TONE_DURATION_MAX_MS`, default 200 ms) at
  `P4_CONFIG_TONE_AMPLITUDE_PCT` peak amplitude with a short fade in/out so it
  does not click. PCM is generated in heap chunks and streamed through the
  ES8311 codec (mono 16-bit 22050 Hz, the BSP default).
- Playback is **background**: a small `audio_play` task consumes play requests
  so `beep && echo ok` fires immediately and a batch file never blocks.
  One sound plays at a time; a new request while one is active is refused with
  `audio busy`.

### Added - wavplay + audio family

- `wavplay <file>` streams a short 16-bit PCM WAV from the SD card. Stereo is
  mixed to mono and 44100 Hz is decimated to 22050, so mono/stereo WAVs at
  22050 or 44100 play; other formats are rejected with an honest message.
  Files are bounded by `P4_CONFIG_WAV_MAX_BYTES` (1 MiB).
- `audio status` reports `playing`/`idle`; `audio stop` cuts the current
  playback short (a long tone or WAV can be cancelled).

### Changed - volume

- `volume` with no argument now prints the current codec volume
  (`volume: <pct>%`); `volume <0-100>` keeps setting it. All audio output
  rides on this volume. ERRORLEVEL 0/2.

### Configuration

- `P4_CONFIG_BEEP_FREQ_HZ`, `P4_CONFIG_BEEP_DURATION_MS`,
  `P4_CONFIG_TONE_FREQ_MIN`, `P4_CONFIG_TONE_FREQ_MAX`,
  `P4_CONFIG_TONE_DURATION_DEFAULT_MS`, `P4_CONFIG_TONE_DURATION_MAX_MS`,
  `P4_CONFIG_TONE_AMPLITUDE_PCT`, `P4_CONFIG_TONE_CHUNK_SAMPLES`,
  `P4_CONFIG_WAV_MAX_BYTES`, `P4_CONFIG_AUDIO_TASK_STACK`,
  `P4_CONFIG_AUDIO_TASK_PRIORITY` (documented in p4minishell_config.yaml).

### Verification

- Clean build: 0 errors, 0 warnings (firmware and test project).
- Hardware (COM11): `volume` queries the current level; `tone 1000 200` and
  `beep` start and play to completion (audible); `tone 440` uses the default
  duration; out-of-range frequencies/durations are usage errors (errorlevel
  2); `audio status` flips to `playing` then `idle`; a second `tone` while one
  plays is refused with `audio busy` (errorlevel 1); `audio stop` cuts a long
  tone and status returns to `idle`; `wavplay` rejects a missing file
  (errorlevel 1) and a non-WAV file honestly; `beep && echo ok` chains in a
  batch line without blocking; raising `volume` audibly increases the tone.
  `volume`/`power`/`sleep`/`deepsleep` show no regressions (audio is stopped
  before entering sleep). Actual WAV playback requires a 16-bit PCM WAV on the
  SD card (mono/stereo, 22050/44100 Hz); the streaming path was validated by
  review plus the header/parse rejection checks.

---

## [0.24.30] - 2026-08-12

Power-management polish: an idle display-off timer with a clean wake path, done
the DOS way. `power idle` sets it, CONFIG.SYS `DISPLAY_TIMEOUT=` sets it at
boot, and touch / USB keyboard / USB mouse / a serial command all wake the
display back to the live shell.

### Added - idle display-off (`power idle`)

- `power idle <seconds>` turns the display backlight off after that many
  seconds without user input; `power idle 0` / `power idle off` disables;
  `power idle` prints the current setting. Capped by
  `P4_CONFIG_POWER_IDLE_DISPLAY_MAX_SECS`.
- `power` status reports `power.idle_off=<secs|off>` and `power.wake_gpio=`.
- **Activity sources** that reset the idle clock and wake a dimmed display:
  touch (LVGL indev), USB keyboard, USB mouse wheel, and any serial command;
  a running command also counts as activity, so a long batch step never looks
  idle.
- Idle-off only drops the backlight (`display_set_power_state`), so the panel,
  GT911, and USB host keep running and the shell state is untouched — waking
  is a clean backlight-on plus a header notification. Disabled by default
  (`P4_CONFIG_POWER_IDLE_DISPLAY_OFF_SECS = 0`); no regression.

### Added - CONFIG.SYS `DISPLAY_TIMEOUT=` directive

- `DISPLAY_TIMEOUT=<seconds|OFF>` in CONFIG.SYS applies the idle timeout at
  boot through the shared `power idle` command, alongside the existing
  `DISPLAY_POWER=` directive; the default template documents it.

### Added - sleep wake options (honest touch-wake reporting)

- The GT911 interrupt line is not wired on this board
  (`BOARD_CFG_LCD_TOUCH_INT_GPIO = GPIO_NUM_NC`), so touch cannot wake light
  sleep. `sleep` now reports that honestly and offers the alternatives:
  `P4_CONFIG_POWER_WAKE_GPIO` (a user-wired button/switch GPIO that wakes
  light sleep via `gpio_wakeup_enable` + `esp_sleep_enable_gpio_wakeup`,
  disabled after wake) and `power idle` for the always-on shell.

### Configuration

- `P4_CONFIG_POWER_IDLE_DISPLAY_OFF_SECS`, `P4_CONFIG_POWER_IDLE_DISPLAY_MAX_SECS`,
  `P4_CONFIG_POWER_WAKE_GPIO`, `P4_CONFIG_POWER_WAKE_LEVEL`
  (documented in p4minishell_config.yaml).

### Verification

- Clean build: 0 errors, 0 warnings (firmware and test project).
- Hardware (COM11): `power idle 5` turns the display off after ~5 s idle and
  `power` reports `power.idle_off=5`; touch, a USB keypress, and a serial
  command each wake the display and reset the clock; `power idle 0` disables
  (no auto-off); `power idle 99` is a usage error (errorlevel 2);
  `DISPLAY_TIMEOUT=` survives a reboot; `power`/`display power`/`battery sleep`
  show no regressions. Repeated idle-off/wake cycles run with no panic.
- Note: `sleep` (light sleep) prints the honest touch-wake message, but on this
  board entering light sleep leaves USB-Serial-JTAG unresponsive afterwards
  (the serial console stops answering; a physical power cycle restores it).
  This is a pre-existing light-sleep/hardware quirk, not introduced here —
  the `sleep` changes in this release are inert on the default configuration
  (no wake GPIO set). Use `power idle` for the always-on touch/USB-wake screen.

---

## [0.24.29] - 2026-08-12

Serial Monitor polish: destructive confirmations and `set /p` are now typable
on a single serial line. The firmware already ran fully over `idf.py monitor`
(USB-Serial-JTAG); this makes the interactive key waits behave like a real
terminal.

### Changed - whole-line key forwarding over serial

- The UART console reader now forwards the entire freshly-read line into the
  interactive keypress queue instead of only its first character. A
  destructive confirmation word such as `YES` is typed as a whole word and
  Enter submits it (`format`, `del /s`, `rd /s`, `trash empty`/`purge`,
  `disk clean`/`delete`, xcopy overwrite prompts), and `set /p` accepts a full
  value line over serial. (The old workaround of sending each letter on its
  own line no longer applies — type the word, then Enter.)
- The queue is flushed at every key-wait begin/end, so stray characters from a
  one-key wait (`pause`, `choice`, `more`) never leak into the next prompt;
  those single-key waits behave exactly as before.

### Verification

- Clean build: 0 errors, 0 warnings (firmware and test project).
- Verified over the serial console — the USB-Serial-JTAG endpoint `idf.py
  monitor` attaches to (same bytes, same port): full boot log into the prompt
  (`P4MiniShell v0.24.29 ready`, `UART console ready`), `version`, `dir`,
  `type`, `top /O:-C`, `findstr`, `xcopy`, a pipe (`top /b | findstr IDLE`),
  and redirection all work. `del /s` confirms with a single `YES` line,
  `trash empty` likewise, `set /p` captures a full typed line, and
  `pause` / `choice` / `more` one-key waits behave unchanged. The recycle-bin
  and errorlevel chains show no regression.

---

## [0.24.28] - 2026-08-12

Richer task view and an optional CPU sparkline in the header, both done the
DOS way: `top`/`ps`/`tasks` sort with the familiar `dir /O:` switch syntax
and set a real ERRORLEVEL, and the header CPU panel can show a small history
graph instead of a single-value bar.

### Added - `top` / `ps` / `tasks` sorting

- `/O:` ordering switches mirror `dir`: sort by `N` (name), `C` (CPU), `S`
  (stack high-water), `P` (priority), or `T` (state); a `-` prefix reverses
  (`top /O:-C`); bare `/O` sorts by name; name is the deterministic
  tie-breaker.
- `top` defaults to CPU-descending (real `top` behaviour); `ps`/`tasks` keep
  the FreeRTOS order unless `/O:` is given. An explicit `/O:` always wins.
- The snapshot is normalized into lightweight heap rows and sorted with
  `qsort` using the same comparator pattern as `dir /O:`; the pure
  `shell_task_row_compare()` helper is unit-tested.
- `/b` bare output stays uncoloured and machine-parsable (now simply sorted),
  so `top /b /O:-C | findstr /V IDLE` works in a pipe.
- `ps`/`tasks`/`top` now return an ERRORLEVEL (0 ok, 2 usage) that the
  dispatcher records, so `top && echo ok`, `if errorlevel 2`, and
  `top /O:Q || echo bad` work in batch files. Still strictly read-only.

### Added - optional header CPU graph

- `P4_CONFIG_HEADER_CPU_GRAPH` (default 1) replaces the header's single-value
  CPU bar with a small LVGL chart sparkline of the recent CPU samples
  (`P4_CONFIG_HEADER_CPU_GRAPH_POINTS`, default 12; width
  `P4_CONFIG_HEADER_CPU_GRAPH_WIDTH_PX`, 26). Bars are bright green, amber
  above `P4_CONFIG_HEADER_CPU_WARN_PCT`. Set the toggle to 0 to restore the
  plain bar exactly.
- The history ring advances on the existing header refresh
  (`P4_CONFIG_HEADER_REFRESH_PERIOD_MS`); no new sampling path.

### Configuration

- `P4_CONFIG_HEADER_CPU_GRAPH`, `P4_CONFIG_HEADER_CPU_GRAPH_POINTS`,
  `P4_CONFIG_HEADER_CPU_GRAPH_WIDTH_PX` (documented in
  p4minishell_config.yaml).

### Verification

- Clean build: 0 errors, 0 warnings (firmware and test project).
- New unit tests: `test_task_sort.c` (name / CPU / stack / priority / state
  ordering, reverse, name tie-break, NULL safety) registered in the Unity
  runner.
- Hardware (COM11): `top /O:C`, `/O:-C`, `/O:N`, `/O:S`, `/O:P`, `/O:T`,
  bare `/O`, and a bad key (usage errorlevel 2) all behave as expected; `/b`
  rows keep the exact `name state prio core headb cpu` format when sorted;
  `top /b /O:-C | findstr /V IDLE` pipes cleanly; `top && echo ok` chains.
  `ps`/`tasks`/`dir /O:` and batch `if errorlevel` are unchanged. The header
  shows the CPU sparkline with the toggle on and the plain bar with the
  toggle off.

---

## [0.24.27] - 2026-08-12

Complete `xcopy` and two more classic DOS text tools. `xcopy` now supports
the full DOS 6.x / WinXP switch set behind a proper heap-scratch recursive
walker, `findstr` adds literal + regex-lite text search with DOS semantics
(case-sensitive by default, DOS errorlevel 0 found / 1 not found / 2 usage),
and `comp` does classic byte-for-byte comparison. Every text utility now
sets a real ERRORLEVEL, so `if errorlevel` and `&&` / `||` work uniformly.

### Added - xcopy completeness

- `xcopy` rewritten with the full classic switch set: `/S` (subdirectories,
  empty ones only with `/E`), `/I` (assume destination is a directory),
  `/Y` / `/-Y` (overwrite silently / prompt), `/D[:mm-dd-yyyy]` (only newer
  than the date, or than the same-named destination), `/H` (include hidden and
  system, skipped by default), `/R` (overwrite a read-only destination),
  `/K` (keep attributes; always the effective default), `/C` (continue past
  errors), `/Q` (quiet), `/T` (create the directory tree only), `/F` (full
  source/destination names), `/L` (list only), `/A` (archive attribute only),
  `/M` (archive only and clear it on the source), `/U` (only files already at
  the destination), `/P` (prompt per file), `/W` (wait for a key), `/N` (short
  8.3 destination names), `/V` (verify with a size post-check).
- The recursive walker now keeps each level's state in one heap block (like
  `dir /s` and `find` discovery) instead of re-entering the command, so the
  8 KB command-worker stack is never at risk; depth is bounded by
  `P4_CONFIG_DIR_RECURSE_DEPTH_MAX`.
- Overwrite default: interactive prompt when a key source is attached and
  neither `/Y` nor `/-Y` is given; headless / batch behaves as `/Y` (the
  previous always-copy behaviour), so nothing regresses in batch files.
- ERRORLEVEL: 0 success, 1 nothing copied / copy failed, 2 usage. `xcopy`
  copies a single file into an existing directory correctly (destination is
  treated as a directory when it is one), and `xcopy file newdir /I` creates
  the directory.

### Added - findstr

- `findstr` searches files for literal strings or small regular expressions,
  case-sensitive by default (unlike `find`). Switches: `/R` (regex), `/C:"s"`
  (literal search string, space-safe), `/I` (case-insensitive), `/N` (line
  numbers), `/V` (non-matching lines), `/X` (whole-line), `/E` (line ends
  with), `/B` (line begins with), `/L` (literal), `/S` (recurse the tree),
  `/M` (filenames with a match only), `/F:file` (file list), `/G:file` (search
  strings from a file). Multiple search strings OR together.
- The regex engine implements the DOS findstr subset: `.`, `*` (zero or more
  of the preceding atom), `^` / `$` anchors, `[class]` / `[^class]` / `[a-z]`,
  `\<` / `\>` word boundaries, and `\c` escapes. It is a pure, unit-tested
  matcher (`shell_fsre_search`, `shell_findstr_match_line`).
- Reads the pending `<` or pipe source when no file is given, and `/S`
  recursion is depth- and match-capped (`P4_CONFIG_FINDSTR_MATCH_MAX`).
- ERRORLEVEL: 0 match found, 1 no match, 2 usage.

### Added - comp

- `comp <file1> <file2> [/D] [/A] [/L] [/N=number] [/C]` compares two files
  byte for byte and reports up to `P4_CONFIG_COMP_MISMATCH_MAX` mismatches
  with offset and byte values. `/D` decimal offsets, `/A` ASCII display,
  `/L` line numbers, `/N=n` compares only the first n lines, `/C` ignores
  case. Prints `Files compare OK` for identical files.
- The byte-comparison core (`shell_comp_first_diff`) is pure and unit-tested.
- ERRORLEVEL: 0 identical, 1 different, 2 usage.

### Changed - ERRORLEVEL for existing text tools

- `find` (both the text-search and file-discovery modes), `more`, `fc`, and
  `sort` now return an ERRORLEVEL that the dispatcher records (0 ok / found,
  1 not found / differences, 2 usage), so `if errorlevel` and `&&` / `||`
  work with every text command. No output or behaviour changed.

### Configuration

- `P4_CONFIG_FINDSTR_PATTERN_BYTES`, `P4_CONFIG_FINDSTR_MAX_STRINGS`,
  `P4_CONFIG_FINDSTR_MATCH_MAX`, `P4_CONFIG_COMP_MISMATCH_MAX`
  (documented in p4minishell_config.yaml).

### Verification

- Clean build: 0 errors, 0 warnings (firmware and test project).
- New unit tests: `test_findstr.c` (regex/literal matcher) and `test_comp.c`
  (byte compare helper) registered in the Unity runner.
- Hardware (COM11): xcopy `/S` copies a nested tree without empty dirs,
  `/S /E` includes empty dirs, `/T` builds the tree only, `/L` lists only,
  `xcopy file dir /I` creates the directory, `/Y` overwrites silently,
  `/D:2026-01-01` skips older files, `/A` skips non-archive files, and
  `&&` / `||` errorlevel chains work. findstr literal, `/I`, `/N`, `/V`,
  `/X`, `/E`, `/C:"..."`, `/R` (`b.n`, `^c`, `e$`), `/S`, `/M`, and multi-file
  prefixing all match DOS expectations with correct errorlevel. comp reports
  identical vs differing files (and `/N /L`) with errorlevel. Existing
  copy/move/del (recycle bin)/tree/find discovery and the errorlevel wiring of
  find/fc/sort are unchanged. A 400-command soak ran with no stall, no
  watchdog trip, no panic.

---

## [0.24.26] - 2026-08-12

Recycle bin + safer destructive commands. `del`/`erase` and `rd`/`rmdir /s`
now move files and whole directory trees into a hidden `.trash` folder by
default instead of deleting them, `undelete` / `trash restore` bring them
back, and `trash` / `recycle` manage the bin. Every destructive command
gates on the exact confirmation word typed at the prompt and reports a real
ERRORLEVEL, so batch files can never wipe the card unattended.

### Added - recycle bin

- `del`/`erase` move matching files into the hidden `.trash` folder (with a
  `.meta` side-car recording the original path, type, size, and move time) and
  report how many were moved. `/p` / `/f` / `/permanent` bypass the bin and
  unlink permanently; `/s` deletes recursively and requires the exact
  confirmation word typed at the prompt before anything happens.
- `rd`/`rmdir` still refuse to remove a non-empty directory without `/s`;
  `/s` moves the whole tree into the trash (permanent with `/p`/`/f`) and is
  gated by the same exact-word confirmation.
- `undelete <name|index>` / `trash restore <name|index>` rename an entry back
  to its original location, recreating missing parent directories and refusing
  to overwrite anything that now occupies the spot.
- `trash` / `recycle` family: `trash list`, `trash info`, `trash restore`,
  `trash purge <name|index>` (gated), `trash empty` (gated).
- Limits enforced on every operation (oldest purged first, FIFO):
  `P4_CONFIG_TRASH_MAX_BYTES` (32 MiB), `P4_CONFIG_TRASH_MAX_AGE_SEC` (7 d),
  `P4_CONFIG_TRASH_MAX_ENTRIES` (256), `P4_CONFIG_TRASH_OPERATION_MAX` (256).
  The trash folder is created hidden (`AM_HID`) and `dir` suppresses
  hidden/system entries by default (DOS behaviour), so `.trash` stays out of
  ordinary listings; bare `dir /A` shows everything.

### Added - exact-word gates for every destructive command

- `format`, `disk clean`, `disk delete partition`, recursive `del /s`,
  recursive `rd /s`, `trash empty`, and `trash purge` all require
  `P4_CONFIG_DESTRUCTIVE_CONFIRM_WORD` (default `YES`) typed exactly at the
  prompt through the interactive key queue; without an interactive input
  source they refuse to run, so a batch file can never trigger them.
  `P4_CONFIG_FORMAT_CONFIRM_WORD` is now an alias of the shared word.
- `del`, `rd`, `format`, `disk`, `undelete`, and `trash` now return an
  ERRORLEVEL (0 success, 1 failure/cancelled, 2 usage) that the batch
  dispatcher records, so `del *.* /s && ...` and `if errorlevel` work.
- `del *.* /s` never recurses into the `.trash` folder itself.

### Configuration

- `P4_CONFIG_DESTRUCTIVE_CONFIRM_WORD`, `P4_CONFIG_TRASH_ENABLE`,
  `P4_CONFIG_TRASH_PATH`, `P4_CONFIG_TRASH_MAX_BYTES`,
  `P4_CONFIG_TRASH_MAX_AGE_SEC`, `P4_CONFIG_TRASH_MAX_ENTRIES`,
  `P4_CONFIG_TRASH_OPERATION_MAX`, `P4_CONFIG_TRASH_CONFIRM_WORD`
  (all documented in p4minishell_config.yaml).
- `P4_CONFIG_COMMAND_TASK_STACK` raised 8192 -> 12288: the batch + recursive
  `del /s` confirmation path was ~100 bytes over the 8 KiB worker-task stack,
  which the new batch refusal exposed as a stack-protection fault.

### Verification

- Clean build: 0 errors, 0 warnings (firmware and test project).
- Hardware (COM11): `del vtest.txt` moves the file to `.trash` and a plain
  `dir` hides both the file and the `.trash` folder; `trash list` shows the
  entry and `undelete` restores it to its original path. `del /p` deletes
  permanently. `del *.txt /s` refuses a wrong word (files intact) and moves
  every match to the bin after the exact `YES`; `rd /s sub2` moves a whole
  tree and `undelete sub2` restores it with contents. `trash empty` cancels on
  a wrong word and clears the bin on `YES`. A batch file running `del *.txt
  /s` is refused outright ("refused, cannot be confirmed from a batch file")
  with no data loss. `del missing /p || echo FAILED` reports a non-zero
  ERRORLEVEL and `del present && echo OK` succeeds. A 400-command soak ran
  with no stall, no watchdog trip, no panic.

---

## [0.24.20] - 2026-08-11

DOSKEY-style aliases / macros with SD persistence. New `alias` and `unalias`
commands define a RAM-only macro table whose leading word is expanded when a
command is typed at the prompt, and the table persists to a batch-style profile
that boot.c auto-loads after CONFIG.SYS.

### Added - alias / unalias

- `alias` — list every alias; `alias name` — show one; `alias name=value` —
  set; `alias name=` — clear; `alias /clear` — clear all;
  `alias /save [file]` / `alias /load [file]` — persist to / reload from the SD
  profile (default `P4_CONFIG_ALIAS_PROFILE` = `ALIASES.BAT`).
- `unalias <name>` — remove one alias.
- DOSKEY-style expansion: when a command is typed at the interactive prompt,
  the leading word is replaced by its alias value before parsing, so
  `alias ll=dir /s` then typing `ll` runs `dir /s`, and `ll *.txt` becomes
  `dir /s *.txt`. Expansion is deliberately disabled inside batch files, so an
  alias can never shadow a batch verb (`call`, `set`, `echo`, ...).
- Aliases are case-insensitive names (alphanumeric + underscore) with values up
  to `P4_CONFIG_ALIAS_VALUE_BYTES` (256) and a table capped at
  `P4_CONFIG_ALIAS_MAX` (32).

### Persistence (reuses storage + boot scripting)

- `alias /save` writes `alias name="value"` lines to the SD profile using the
  guarded storage session API, the free-space pre-check, and an atomic
  temp + rename (with remove-and-retry for FATFS overwrite). Values containing
  a double quote are skipped with a warning so the profile always round-trips.
- boot.c auto-runs the profile through the batch pipeline after CONFIG.SYS and
  before AUTOEXEC.BAT when the file is present, so saved aliases are restored
  every boot with no AUTOEXEC.BAT edits. `alias /load` reloads manually.
- Safe when the SD card is absent or the profile is missing: boot and operation
  continue unchanged.

### Verification

- Clean build: 0 errors, 0 warnings for the firmware and the test project.
- Hardware (COM11): `alias ll=dir`, `alias ls="dir /b"`, `alias cls=clear`
  define and list correctly; `ls` expands to `dir /b`; `unalias cls` removes;
  `alias /save` writes `ALIASES.BAT`; a reboot auto-loads `LL`/`LS` from the
  profile and `ls` works. A batch file using `set`/`echo` runs correctly,
  confirming aliases do not expand in batch files. A 400-command soak completed
  with no stall, no watchdog trip, no panic.

---

## [0.24.25] - 2026-08-11

RGB status LED: the WS2812 (NeoPixel) LED on the JC1060P470 back panel
(GPIO26) is now driven by a dedicated `components/led` leaf, exposed as the
`rgb` shell command and a CONFIG.SYS `RGB=` directive, with an auto status
layer tied to Wi-Fi/HTTP events and a boot confirmation flash.

### Added - components/led + `rgb` command

- New leaf component `components/led` owns the WS2812 strip (created with the
  `espressif/led_strip` managed component over RMT) and a small animation task
  that renders the current colour, effect, or transient notification. All
  strip I/O happens on that task; callers only touch a mutex-protected state
  snapshot, so the command worker and the Wi-Fi event handler never block.
- `rgb status` — driver pin, mode (auto/manual), effect, colour, speed,
  brightness.
- `rgb <r> <g> <b>` (0-255), `rgb #RRGGBB`, `rgb off` — solid colour.
- `rgb <effect> [speed]` — `rainbow`, `breath`, `pulse`, `blink`, `solid`
  (speed 1..10).
- `rgb auto <on|off>` — toggle the status-driven colour layer.
- ERRORLEVEL 0/1/2, redirectable/pipable, so `rgb ... && echo ok` works in
  batch files. GPIO26 is added to the board pin table as a reserved critical
  line, so the peripheral toolkit (`pwm`/`freq`/`adc`/`i2c`/`spi`) refuses it.

### Added - auto status + event notifications + boot light

- Auto status colours follow Wi-Fi state: amber pulse while connecting, green
  when connected, red blink when disconnected, red pulse on watchdog timeout.
  The HTTP file server flashes blue on start. In manual mode all events are
  transient (`P4_CONFIG_LED_NOTIFY_MS`) and return to the manual colour.
- A short green confirmation flash (`P4_CONFIG_LED_BOOT_FLASH_MS`) fires once
  boot scripting completes.
- Wi-Fi hooks live in `components/networking` (event handler + connect request
  + watchdog), HTTP hooks in `http_server.c`, and the boot flash in `main.c`.
  All colours and limits are tunable in `p4minishell_config.h`
  (`P4_CONFIG_LED_*`, documented in `p4minishell_config.yaml`).

### Added - CONFIG.SYS `RGB=` directive

- `RGB=<r>,<g>,<b> | #RRGGBB | <effect>[,speed] | OFF | AUTO,<ON|OFF>` is
  parsed by `components/boot/boot.c` and executed through the existing
  `rgb ...` command path at boot (commas map to argument separators).
  Documented in the default CONFIG.SYS template.

### Configuration

- `P4_CONFIG_LED_GPIO`, `P4_CONFIG_LED_RMT_RESOLUTION_HZ`,
  `P4_CONFIG_LED_RMT_SYMBOLS`, `P4_CONFIG_LED_MAX_BRIGHTNESS_PCT`,
  `P4_CONFIG_LED_TASK_STACK_BYTES`, `P4_CONFIG_LED_TICK_MS`,
  `P4_CONFIG_LED_NOTIFY_MS`, `P4_CONFIG_LED_BOOT_FLASH_MS`,
  `P4_CONFIG_LED_EFFECT_SPEED_DEFAULT`, `P4_CONFIG_LED_AUTO_STATUS`, and the
  `P4_CONFIG_LED_COLOR_*` event colours (all documented in
  p4minishell_config.yaml).
- `board_config.h`: `BOARD_CFG_RGB_LED_GPIO 26`, `BOARD_CFG_RGB_LED_IS_WS2812 1`
  (board_config.yaml updated to match).

### Verification

- Clean build: 0 errors, 0 warnings (firmware and test project).
- Hardware (COM11): `rgb status` reports WS2812 on GPIO26; `rgb 255 0 0`,
  `rgb #00FF00`, and `rgb 0 0 255` set solid colours; `rainbow`/`breath`/
  `blink`/`pulse` effects and speeds apply; `rgb off` clears; `rgb 300 0 0`
  is a usage error; `rgb 255 0 0 && echo chain-ok` chains in a batch line;
  `rgb auto on|off` toggles the status layer. Adding `RGB=0,0,255` to
  CONFIG.SYS and rebooting leaves `rgb status` showing a manual blue, proving
  the boot directive runs.

---

## [0.24.24] - 2026-08-11

Lightweight network services: an HTTP file server that shares the SD card
over Wi-Fi (`httpd`), plus `netstat` and `ipconfig` diagnostics. All of it
lives in `components/networking/` (the sole owner of the esp_http_server and
lwIP surfaces).

### Added - HTTP file server (`httpd start|stop|status`)

- Serves the SD card (`BSP_SD_MOUNT_POINT`) over the Wi-Fi link with the
  `esp_http_server` driver. URL paths map onto files; directories get an
  HTML listing bounded by `P4_CONFIG_HTTPD_LISTING_MAX`.
- Optional HTTP Basic auth: `P4_CONFIG_HTTPD_AUTH_USERNAME` /
  `_PASSWORD`. Empty username disables auth; credentials are decoded from
  the `Authorization` header and compared constant-time. Unauthenticated
  requests get a `401` with `WWW-Authenticate`.
- Lifecycle tied to Wi-Fi events: the server auto-starts on
  `IP_EVENT_STA_GOT_IP` and stops on `WIFI_EVENT_STA_DISCONNECTED` (start
  side gated by `P4_CONFIG_HTTPD_AUTOSTART`), and `httpd start` refuses when
  the station is not connected. `httpd status` reports state, port, auth,
  docroot, open sockets, and the request count.
- Path traversal is rejected (`..` segments return 400), file streaming uses
  a heap read buffer with send timeouts, and the server is fully bounded by
  resource limits (`P4_CONFIG_HTTPD_*`): port, open sockets, backlog, task
  stack/priority, recv/send timeouts, block size, listing cap, autostart.
- `httpget` now sends HTTP Basic auth when the URL carries a `user:pass@`
  prefix, so authenticated endpoints (including the file server itself) work
  from the shell.

### Added - netstat / ipconfig

- `netstat` lists the network interfaces (state, IPv4, netmask, gateway, MTU,
  MAC), the active TCP connections (local/remote `ip:port`, state), TCP
  listeners, and UDP endpoints by walking the lwIP PCB lists read-only under
  the TCP/IP core lock (no-op when core locking is disabled), capped at
  `P4_CONFIG_NETSTAT_ROW_MAX`.
- `ipconfig` reports the full per-interface configuration (state, MAC, IPv4,
  netmask, gateway, MTU, default-route marker) plus the configured DNS
  servers from lwIP.
- Both are redirectable/pipable like every other command.

### Configuration

- `P4_CONFIG_HTTPD_PORT`, `P4_CONFIG_HTTPD_MAX_OPEN_SOCKETS`,
  `P4_CONFIG_HTTPD_BACKLOG`, `P4_CONFIG_HTTPD_STACK_BYTES`,
  `P4_CONFIG_HTTPD_TASK_PRIORITY`, `P4_CONFIG_HTTPD_RECV_TIMEOUT_S`,
  `P4_CONFIG_HTTPD_SEND_TIMEOUT_S`, `P4_CONFIG_HTTPD_BLOCK_BYTES`,
  `P4_CONFIG_HTTPD_LISTING_MAX`, `P4_CONFIG_HTTPD_AUTOSTART`,
  `P4_CONFIG_HTTPD_AUTH_USERNAME`, `P4_CONFIG_HTTPD_AUTH_PASSWORD`,
  `P4_CONFIG_NETSTAT_ROW_MAX` (all documented in p4minishell_config.yaml).

### Verification

- Clean build: 0 errors, 0 warnings.
- Hardware (COM11): with Wi-Fi connected to `4G-CPE_5542`, the server
  auto-starts on DHCP (`192.168.199.225`); `httpget http://127.0.0.1/`
  returns the SD root HTML listing, `/CONFIG.SYS` serves the file, a missing
  file returns 404, and `..%2f..` traversal returns 400. With auth on,
  `httpget http://127.0.0.1/` returns 401 and
  `httpget http://admin:p4mini@127.0.0.1/CONFIG.SYS` returns 200.
  `wifi disconnect` stops the server. `netstat` and `ipconfig` report the
  interface and connection state correctly.

---

## [0.24.23] - 2026-08-11

Richer GPIO/peripheral toolkit: `pwm`, `freq`, `adc`, `i2c`, and `spi`
commands built on the LEDC, ADC one-shot, I2C master, and SPI master drivers,
with a shared pin-safety gate and all tunables in `p4minishell_config.h`.

### Added - pwm / freq / adc / i2c / spi

- `pwm <pin> <freq_hz> <duty_pct>` / `pwm stop <pin>` / `pwm status` — LEDC PWM
  on a non-reserved GPIO. Uses timers 0/2/3 and channels excluding the
  backlight's, shares the backlight's XTAL global clock (so no LEDC "timer
  clock conflict"), and caps at `P4_CONFIG_PWM_CHANNEL_MAX` concurrent outputs.
- `freq <pin> <hz>` / `freq stop <pin>` / `freq status` — square wave at 50%
  duty via the same LEDC engine (`pwm` at the default duty).
- `adc <pin> [samples]` / `adc status` — one-shot ADC reads on any non-reserved
  ADC pin (ADC1 GPIO16-23, ADC2 GPIO49-54) with calibration; `adc status`
  live-samples the SOC channel map and only lists pins that currently read
  back, and a busy ADC unit reports "in use".
- `i2c status | i2c scan [sda=.. scl=..] | i2c peek <addr> <reg> [sda=.. scl=..]
  | i2c poke <addr> <reg> <value> [sda=.. scl=..]` — the scanner reuses the
  board's shared BSP bus handle and probes with normal device transactions
  (fast, does not disrupt the GT911 touch); custom pin pairs get a temporary
  bus on a free port.
- `spi status` — reports the SPI toolkit configuration. The `loopback` /
  `peek` / `poke` verbs are recognized but return an honest "unavailable on
  this board" error: SPI host init on this P4 with the ESP-Hosted SDIO link
  active stalls the chip and drops USB-Serial-JTAG off the bus (verified on
  both SPI2 and SPI3, with DMA on and off), so they are not wired to the SPI
  driver. This follows the explicit-failure policy used by `rgb`/`camera`.
- Pin safety: every command routes its pins through `shell_pin_is_reserved()`
  (the critical entries of the board GPIO table), so the active I2C/I2S/SDIO/
  display/SD lines can never be repurposed.
- Config: `P4_CONFIG_PWM_FREQ_MAX_HZ`, `P4_CONFIG_PWM_SRC_CLK_HZ`,
  `P4_CONFIG_PWM_CLK_SOURCE`, `P4_CONFIG_PWM_CHANNEL_MAX`,
  `P4_CONFIG_PWM_DUTY_DEFAULT_PCT`, `P4_CONFIG_ADC_DEFAULT_SAMPLES`,
  `P4_CONFIG_ADC_MAX_SAMPLES`, `P4_CONFIG_ADC_ATTEN`,
  `P4_CONFIG_I2C_TOOL_TIMEOUT_MS`, `P4_CONFIG_I2C_SCAN_PROBE_TIMEOUT_MS`,
  `P4_CONFIG_I2C_SCAN_FIRST_ADDR`, `P4_CONFIG_I2C_SCAN_LAST_ADDR`,
  `P4_CONFIG_I2C_TOOL_CLK_HZ`, `P4_CONFIG_SPI_TOOL_CLK_HZ`,
  `P4_CONFIG_SPI_TOOL_TIMEOUT_MS`, `P4_CONFIG_SPI_TOOL_BUFFER_BYTES`,
  `P4_CONFIG_SPI_TOOL_HOST` (all documented in p4minishell_config.yaml).

### Verification

- Clean build: 0 errors, 0 warnings.
- Hardware (COM11): `i2c scan` finds the ES8311 (0x18) and GT911 (0x5D)
  quickly without disrupting touch; `i2c peek`/`poke` read/write registers;
  `freq 21 1000` and `pwm 21 1000 25` produce the expected output and
  `pwm status`/`stop` work; `adc 22` reports a live calibrated reading;
  reserved pins (e.g. GPIO7, GPIO53) are refused by every command; `spi
  status` reports the configuration. SPI transactions are intentionally
  refused (host init stalls this board), so no SPI command can freeze the
  shell.

---

## [0.24.22] - 2026-08-11

CONFIG.SYS: unrecognized `KEY=VALUE` directives are now applied as batch
environment variables instead of being skipped with a warning.

### Changed - CONFIG.SYS generic KEY=VALUE

- Any unknown `NAME=VALUE` line in CONFIG.SYS is applied with
  `shell_env_set()` (identical effect to `SET`), so project variables can be
  defined directly in CONFIG.SYS and consumed by AUTOEXEC.BAT via `%NAME%`.
- Unknown keywords *without* a value still produce the single muted
  `config: unknown directive "...", skipped` warning, so malformed lines and
  genuine typos remain visible. Known-but-unsupported DOS directives
  (`FILES`, `BUFFERS`, `LASTDRIVE`, `DEVICE`, `DOS`, `SHELL`) still warn.
- Default CONFIG.SYS template and docs (command.md, documentation.md,
  readme.md, ai-context.md) updated to describe the fallback.

### Verification

- Clean build: 0 errors, 0 warnings.
- Hardware (COM11): boot log no longer shows `config: unknown directive
  "UNKNOWN_DIRECTIVE", skipped` after the `gpio set` line; `echo
  %UNKNOWN_DIRECTIVE%` returns `xyz` and `set` lists `UNKNOWN_DIRECTIVE=xyz`.

---

## [0.24.21] - 2026-08-11

Power-management commands: `power` (status), `sleep` (light sleep), and
`deepsleep` (deep sleep), built on the ESP-IDF sleep APIs with clean shutdown
of Wi-Fi/hosted state and battery-aware reporting.

### Added - power / sleep / deepsleep

- `power` — reports power state: PM enabled status, automatic light sleep
  request (`battery sleep on|off`), display power state, Wi-Fi link state,
  battery level/voltage (shared `command_battery_read()` ADC path), and the
  last wake-up cause from `esp_sleep_get_wakeup_cause()`.
- `sleep [seconds]` — enters light sleep. RAM is retained, so the shell
  resumes with all state (env, aliases, cwd, variables) intact. With no
  argument the duration is `P4_CONFIG_POWER_SLEEP_DEFAULT_SECS` (60 s);
  `sleep 0` clears the timer and wakes only from an external source. On wake
  the wake cause is reported and the display is restored.
- `deepsleep [seconds]` — enters deep sleep. RAM is lost, so on wake the
  device boots fresh (same path as `reboot`). Warns that RAM-only state is
  lost and that omitting the timer requires an external wake source.
- Clean shutdown before either sleep: `networking_wifi_shutdown()` (the same
  teardown C6 OTA uses) plus `display_set_power_state(DISPLAY_POWER_OFF)`.
  Light-sleep Wi-Fi teardown is optional via
  `P4_CONFIG_POWER_LIGHT_SLEEP_SHUTDOWN_WIFI` (default on); after a light
  sleep the shell notes that `wifi connect` is needed to reconnect.
- Safety checks: duration is validated and clamped to
  `P4_CONFIG_POWER_SLEEP_MAX_SECS` so a typo cannot sleep for days; the
  transcript is given `P4_CONFIG_POWER_SLEEP_PRE_DELAY_MS` to paint before
  sleep (and `P4_CONFIG_REBOOT_DELAY_MS` before the deep-sleep reset).

### Verification

- Clean build: 0 errors, 0 warnings.
- Hardware: `power` shows battery %, display, Wi-Fi, and wake cause; `sleep 2`
  blanks the display, tears down Wi-Fi, wakes after ~2 s on the timer, restores
  the display, and reports `cause=timer`; Wi-Fi reconnect with `wifi connect`
  succeeds afterward.

---

## [0.24.19] - 2026-08-11

Better file discovery. The `find` command now has a recursive file-discovery
mode in addition to its classic text search, filtering by filename wildcard,
size, and modification date — one adaptable command, no new verbs.

### Added - find file-discovery mode

- Entered automatically whenever a discovery switch is present (`/NAME:`,
  `/SIZE:`, `/NEWER:`, `/OLDER:`, `/DIRS`, `/B`, `/S`); with the classic
  switches only, `find` remains the original text-search command unchanged.
- `find [path] [/NAME:pattern] [/SIZE:spec] [/NEWER:date] [/OLDER:date] [/DIRS] [/B]`
  - `/NAME:pattern` — filename wildcard (e.g. `*.log`, `*config*`). A wildcard
    in the path argument also splits into directory + pattern, like `dir`.
  - `/SIZE:spec` — size filter in bytes with an optional K/M/G suffix. The
    redirection-safe range syntax `N-M` (range), `N-` (at least), `-M` (at
    most), or `N` (exact) is primary because `>`/`<` are the shell's
    input/output operators; the comparison forms `>N`/`>=N`/`<N`/`<=N` are also
    accepted when quoted.
  - `/NEWER:date` / `/OLDER:date` — modified on/after or on/before `YYYY-MM-DD`.
  - `/DIRS` — include directories as well as files.
  - `/B` — bare: full paths only, no colour (redirectable / pipable).
- Recursion is depth-bounded by `P4_CONFIG_DIR_RECURSE_DEPTH_MAX` and matches
  are capped by `P4_CONFIG_FIND_MATCH_MAX` (256) with a clear truncation note.
  The walker reuses the `dir /s` FATFS primitives and keeps each recursion
  level's state in one heap block, so the 8 KB worker stack is never at risk.

### Safety

- The classic text search (`find <text> [file] [/I] [/N] [/C] [/V]`) is
  untouched: discovery only activates when a discovery switch is present, so no
  existing usage or batch file changes behaviour.
- Invalid filters and paths print a clear error and a usage hint; a missing SD
  card reports the usual "not present" message. No crash, no hang.

### Config

- New tunable in `p4minishell_config.h` / `p4minishell_config.yaml`:
  `P4_CONFIG_FIND_MATCH_MAX` (256).

### Verification

- Clean build: 0 errors, 0 warnings for the firmware and the test project.
- Hardware (COM11): text search still works (`find hello t.txt` -> 1 match);
  `find /NAME:*.txt /B` lists `.txt` files recursively into subdirectories;
  `/SIZE:5-` and `/SIZE:1-10` filter by byte range; `/DIRS /B` includes
  directories; `/NEWER:`/`/OLDER:` filter by date. A 400-command soak including
  repeated discovery runs completed with no stall, no watchdog trip, no panic.

---

## [0.24.18] - 2026-08-11

FreeRTOS task introspection. New `ps`, `tasks`, and `top` commands list every
running task (name, state, priority, core, stack high-water mark) with the CPU
share since the previous sample — read-only, no task is modified.

### Added - ps / tasks / top

- `ps` / `tasks` — print a colour-coded FreeRTOS task table: task name, state
  (RUN/RDY/BLK/SUS/DEL), priority, core (`-1` = unpinned / no affinity), stack
  high-water mark (minimum free stack bytes since creation), and CPU% since the
  previous sample.
- `top` — same table plus a summary line with the live task count, free heap,
  and uptime. Per-task CPU% is computed by diffing `ulRunTimeCounter` against
  the previous `ps`/`top`/`tasks` call (keyed by `xTaskNumber`, so a
  deleted-and-recreated task starts fresh; unsigned arithmetic handles the
  run-time counter wrapping). First call shows 0% (no previous sample).
- `/b` bare form on any of the three emits uncoloured machine-parsable rows
  (`name state prio core headb cpu`) for redirection / pipes.
- The snapshot array is heap-allocated and capped by `P4_CONFIG_TASK_SNAPSHOT_MAX`
  (64), so a busy task list cannot overflow the 8 KB command-worker stack or
  flood the transcript.
- Implemented in `components/shell/shell.c` (`shell_command_ps`), dispatched
  from `components/command/command.c`, and listed in `help`.

### Safety

- Read-only: the commands only read `uxTaskGetSystemState()` snapshots and never
  call `vTaskSuspend`, `vTaskDelete`, priority changes, or any other task
  mutation. No kill/suspend verbs are offered.
- Graceful degradation: if the FreeRTOS trace facility is compiled out
  (`CONFIG_FREERTOS_USE_TRACE_FACILITY` off), `ps` prints a single muted note;
  an empty snapshot prints a clear error instead of crashing.

### Config

- New tunable in `p4minishell_config.h` / `p4minishell_config.yaml`:
  `P4_CONFIG_TASK_SNAPSHOT_MAX` (64).

### Verification

- Clean build: 0 errors, 0 warnings for the firmware and the test project.
- Hardware (COM11): `ps` lists shell_cmd, shell_uart, IDLE0/IDLE1, taskLVGL, and
  the hosted SDIO/rpc tasks with correct states, priorities, cores (`-1` for
  unpinned), and stack headroom; `top` shows the summary and real CPU%
  (IDLE0/IDLE1 near 100%, taskLVGL ~5% between samples); `tasks /b` emits clean
  bare rows. A 400-command soak including repeated ps/top completed with no
  stall, no watchdog trip, and no panic.

---

## [0.24.17] - 2026-08-11

Persistent known Wi-Fi networks. The firmware now stores a list of previously-
used networks (SSID + password + metadata) on the SD card and automatically
connects to the best known network on every boot, while preserving the classic
single-credential CONFIG.SYS path when the SD card is absent.

### Added - known-network storage (sd:/WIFI.KNOWN)

- `components/networking/wifi_known.c` + `wifi_known.h`: an in-memory cache and
  a persistent, hand-editable text list on SD. File format
  `SSID|PASSWORD|AUTH|PRIORITY|PREFERRED|LAST_CONNECTED|CONNECT_COUNT`, one
  network per line. The file is written atomically (temp + rename) with the
  storage free-space pre-check and partial-file cleanup; entries are capped at
  `P4_CONFIG_WIFI_KNOWN_MAX` (16), evicting the least preferred / lowest-
  priority / oldest entry when full. Duplicate SSIDs are deduplicated
  case-insensitively.
- New commands (each sets ERRORLEVEL, is redirectable, and never prints a
  password): `wifi known` / `wifi list known`, `wifi save [ssid]`,
  `wifi forget <ssid>` / `wifi delete <ssid>`, `wifi forget all` /
  `wifi clear known`, and `wifi preferred <ssid>`.
- Boot-time auto-connect: after the STA runtime is up, when
  `WIFI_AUTOCONNECT=ON` (the CONFIG.SYS master switch) the firmware loads the
  known list, scans, and connects to the best visible known network (preferred
  / highest priority / strongest RSSI), then falls back to the classic
  single-credential path when the list is empty or no known network is in
  range. The existing watchdog / exponential-backoff machinery retries the
  chosen target.
- Successful connections (interactive `wifi connect`, CONFIG.SYS
  WIFI_SSID/WIFI_PASSWORD, or known-network auto-connect) auto-update the list
  when `P4_CONFIG_WIFI_KNOWN_AUTOSAVE` is enabled and the card is present.

### Changed

- `networking_wifi_set_boot_credentials()` now preserves the other field when
  called with a single credential, so `WIFI_SSID=x` + `WIFI_PASSWORD=y` in
  CONFIG.SYS assemble the pair (and seed the known list) correctly.
- `networking_handle_wifi_command()` returns `esp_err_t`; the command module
  maps it onto ERRORLEVEL (0 success, 1 failure) for the known-list commands.

### Safety (verified on hardware)

- No SD card / eject / missing or corrupt file / read-only or full disk: the
  known-list commands report a clear "unavailable" message, boot and normal
  operation continue, and the single-credential path is used. No crash, no
  freeze, no infinite retry.

### Config

- New tunables in `p4minishell_config.h` / `p4minishell_config.yaml`:
  `P4_CONFIG_WIFI_KNOWN_MAX` (16), `P4_CONFIG_WIFI_KNOWN_FILE` ("WIFI.KNOWN"),
  `P4_CONFIG_WIFI_KNOWN_AUTOSAVE` (1), `P4_CONFIG_WIFI_KNOWN_LINE_BYTES` (256).

### Verification

- Clean build: 0 errors, 0 warnings for the firmware and the test project.
- Hardware (COM11): `wifi connect 4G-CPE_5542` auto-saves the network; `wifi
  known` lists it (SSID + preferred/last-used markers, never the password);
  `wifi preferred` / `wifi forget` / `wifi forget all` update the file. With
  CONFIG.SYS `WIFI_AUTOCONNECT=ON`, a reboot automatically reconnects to the
  known network (auto-connect -> associated -> got IP) and refreshes the list.
  Ejecting the SD card makes every known-list command fail gracefully and the
  shell stays responsive. A 400-command soak completed with no stall, no
  watchdog trip, and no panic.
- Note: a pre-existing, unrelated stack-overflow in the `copy` command (three
  path buffers + vfprintf on the 8192-byte worker stack) reproduces on this
  build; it is not introduced by this change and is tracked separately.

---

## [0.24.16] - 2026-08-11

Time / SNTP control release. All time, date, timezone, and NTP behaviour now
lives in the clock component, surfaced by `date`, `time`, `timezone`, and
`sntp`/`ntpsync` shell commands. The clock can be synchronized against an NTP
server, the timezone is settable, and the date/time show is a fuller clock
panel.

### Added - timezone and SNTP control

- `timezone [TZ]` — show the current POSIX timezone string, or set one (e.g.
  `timezone UTC`, `timezone CET-1CEST,M3.5.0,M10.5.0/3`). The local time
  re-renders immediately through the C library.
- `sntp` / `ntpsync` — show NTP sync status (server, synced or not, local
  time). `sntp sync` (alias `ntpsync sync`) forces a fresh NTP exchange against
  the configured server; once Wi-Fi is connected the clock jumps to the network
  time. The NTP server hostname is configurable via `P4_CONFIG_NTP_SERVER`.

### Changed - fuller date/time and clock-component ownership

- `date` and `time` with no argument now show the full clock panel: local time,
  UTC, Unix timestamp, timezone, uptime, and NTP sync status. Their set forms
  (`date MM-DD-YYYY`, `time HH:MM[:SS]`) are unchanged.
- The `date`, `time`, `timezone`, and `sntp`/`ntpsync` command bodies moved
  from `components/command/command.c` into `components/clock/clock_commands.c`
  so every time/SNTP behaviour is owned by the clock component. The clock
  component stays a leaf: the commands render through a `clock_host_ops_t`
  table registered by `command_init()`, whose wrappers map one-to-one onto the
  shell print helpers (output is byte-for-byte identical).
- Removed the now-unused `shell_get_time_string()` and `shell_time_is_synced()`
  wrappers from `components/shell/` (the clock commands call the clock API
  directly).
- `components/clock/clock.h` adds `time_force_resync()`,
  `time_get_ntp_server()`, `time_get_uptime_formatted()`, the
  `clock_host_ops_t` table + `clock_register_host_ops()`, and the
  `clock_command_*()` surface. `clock.c` now reads the NTP server from config.

### Config

- New tunables in `p4minishell_config.h` / `p4minishell_config.yaml`:
  `P4_CONFIG_NTP_SERVER` ("pool.ntp.org") and `P4_CONFIG_TIMEZONE_BYTES` (64).

### Verification

- Clean build: 0 errors, 0 warnings for the firmware and the test project.
- Hardware (COM11, Wi-Fi SSID `4G-CPE_5542`): `date`/`time` show the fuller
  panel; `date 08-11-2026`, `time 12:34:56`, and `timezone CET-1CEST,...`
  (UTC+2 in August) apply correctly; `timezone UTC` restores UTC. `sntp sync`
  over Wi-Fi synchronizes the clock to real NTP time (`Local: 2026-08-11
  07:59:36`, `NTP sync: synced (pool.ntp.org)`). A 400-command soak completed
  with no stall, no watchdog trip, and no panic.
- Note: an intermittent, pre-existing crash in the LVGL transcript apply path
  (`lv_font_get_line_height` on a corrupt span font, seen once across ~13 Wi-Fi
  connect attempts) is unrelated to this change; it reproduces only rarely and
  is not triggered by the clock commands.

---

## [0.24.15] - 2026-08-11

Transcript navigation release. The scrollable transcript now always jumps to the
output of the command you just submitted, and it can be scrolled up and down
from every input surface: dedicated on-screen scroll buttons, USB keyboard
PageUp/PageDown, and the USB mouse wheel.

### Added - transcript scrolling from all input surfaces

- **Auto-follow on submit**: submitting a command (touch keyboard, USB keyboard,
  or the serial console) now forces the transcript to jump to the newest output
  even when the user was reading earlier history. A one-shot force-follow flag
  is consumed by the first repaint of that command's output; background output
  (async Wi-Fi status, etc.) afterwards reverts to the terminal-style
  near-bottom follow, so reading history is never yanked away by background
  messages.
- **On-screen scroll buttons**: the input row now has `Up` / `Dn` buttons next
  to the `Prev` / `Next` history buttons. They page the transcript by
  `P4_CONFIG_TRANSCRIPT_SCROLL_STEP` pixels per press and are always visible,
  independent of the on-screen keyboard.
- **USB keyboard scrolling**: `PageUp` / `PageDown` scroll the transcript by one
  viewport height. Cursor keys keep their existing editing/history roles.
- **USB mouse wheel scrolling**: each wheel notch scrolls the transcript by
  `P4_CONFIG_TRANSCRIPT_SCROLL_STEP` pixels (up notches scroll toward older
  output). The wheel is processed regardless of `usb mouse` echo mode and is
  routed to the LVGL task through the app bridge exactly like the USB keyboard
  injection path.
- Touch drag on the transcript continues to pan it through the container's
  built-in LVGL scroll handling.

### Changed - window manager and shell bridges

- `components/windows/windows.h` adds `windows_force_scroll_transcript_to_end()`,
  `windows_scroll_transcript_by(int32_t)`, `windows_scroll_transcript_to_top()`,
  `windows_get_scroll_up_button()`, and `windows_get_scroll_down_button()`.
  `windows_transcript_apply()` honours the one-shot force-follow flag.
- `components/shell/shell.h` adds `shell_force_transcript_scroll_to_end()`; the
  UART console submit path and the LVGL input-line READY handler (main.c) call it
  instead of the near-bottom follow. `shell_usb_keyboard_input()` handles
  PageUp/PageDown for transcript scrolling.
- `components/usb/usb.c` reads the boot-protocol mouse wheel byte and calls the
  new `usb_host_scroll_transcript()` bridge; `main.c` implements the bridge with
  an `lv_async_call` onto the LVGL task.

### Config

- New tunables in `p4minishell_config.h` / `p4minishell_config.yaml`:
  `P4_CONFIG_TRANSCRIPT_SCROLL_STEP` (60 px per button press / wheel notch),
  `P4_CONFIG_TRANSCRIPT_SCROLL_FOLLOW_PX` (32 px near-bottom follow threshold),
  and `P4_CONFIG_WINDOW_SCROLL_BUTTON_WIDTH` (64 px input-row scroll button).

### Verification

- Clean build: 0 errors, 0 warnings for the firmware and the test project.
- Hardware (COM11): 400-command soak with no stall, no task watchdog trip, no
  panic, and a responsive shell afterward. Screenshot confirms the input row
  now renders the four buttons (Prev / Next / Up / Dn) with the transcript and
  keyboard unchanged.

---

## [0.24.14] - 2026-08-10

Full printable-ASCII keyboard support. The on-screen LVGL keyboard can now type
every printable ASCII character (0x20-0x7E), including the shell-critical
characters the default LVGL symbols map omitted.

### Added - shell-critical symbols on the on-screen keyboard

- The default LVGL symbols map had no way to type four printable ASCII
  characters that a DOS shell needs: the pipe `|` (pipe operator), the caret
  `^` (the shell escape character), the tilde `~`, and the backtick.
- `components/keyboard/keyboard.c` now installs a custom symbols map via the
  official `lv_keyboard_set_map()` API: it keeps every default symbol and digit
  and adds a fourth symbol row (`^ | ~ ` - _ , . :`). Together with the text
  and number modes, the on-screen keyboard now covers all of 0x20-0x7E.
- The custom map preserves the exact LVGL control-button labels (`"abc"`,
  `LV_SYMBOL_BACKSPACE`, `LV_SYMBOL_NEW_LINE`, `LV_SYMBOL_KEYBOARD`,
  `LV_SYMBOL_LEFT/RIGHT`, `LV_SYMBOL_OK`), so the built-in mode switching and
  character routing work unchanged.

### Verification

- Static coverage check: every printable ASCII character (0x20-0x7E) is now
  typeable across the four keyboard modes.
- Hardware (COM11 + screenshot): the symbols keyboard renders the new fifth
  row; `echo ^| ^~ ^^` prints `| ~ ^`, and tilde/backtick pass through the
  shell pipeline unchanged. Build: 0 errors, 0 warnings for the firmware and
  the test project.

---

## [0.24.13] - 2026-08-10

Transcript colour rendering fix. The on-screen transcript was showing LVGL
recolor markup (`#RRGGBB … #`) as literal visible characters instead of
applying the colours, leaving the shell monochrome.

### Fixed - transcript now renders ANSI colours as a span group

- Root cause: `windows_create_transcript()` created an `lv_label` and relied on
  LVGL's recolor feature (`lv_label_set_recolor(true)`) to interpret
  `#RRGGBB` markup produced by `ansi_to_lvgl_recolor()`. In this LVGL 9.4
  build the markup was not interpreted and was dumped into the visible string,
  so every colour change left a literal `#RRGGBB` token on the display.
- Fix: `components/windows/windows.c` now creates the transcript as an
  `lv_spangroup` (the architecture already documented in `ai-context.md`) and
  parses the raw ANSI SGR text directly into one coloured span per run via
  `ansi_process_text()`. No recolor markup is ever generated, so colour
  control tokens cannot leak into the visible text.
- The deferred rebuild (coalesced `lv_async_call` onto the LVGL task) is
  preserved, so bursty output still paints once per handler pass with no
  render-cycle race.
- Verified on hardware (COM11) with the `screenshot` command: `wifi status`
  and `sysinfo` now render cyan labels, magenta numbers, green headings and
  yellow warnings. Pre-fix screenshot: 0 magenta pixels; post-fix: cyan,
  magenta, green and yellow all present.
- Build: 0 errors, 0 warnings for the firmware and the test project.

---

## [0.24.12] - 2026-08-10

Screenshot feature. Adds the `screenshot` command (aliases `scr`, `capture`) for
retrieving an exact pixel-perfect BMP capture of the current LVGL screen over
the existing UART/USB-Serial-JTAG console or to an SD card file.

### Added - `screenshot [filename.bmp]` (aliases `scr`, `capture`)

- Captures the current LVGL screen using `lv_snapshot_take()` with RGB565 format.
- When no filename is given: streams the complete BMP (54-byte header + RGB888
  pixels) over the UART/USB-Serial-JTAG console with clear magic markers
  (`=== SCREENSHOT BMP BEGIN ===` / `=== SCREENSHOT BMP END ===`) for
  easy host-side extraction.
- When a filename is given: writes the BMP to the SD card using the existing
  storage path (cwd-relative resolution, guarded session, free-space precheck,
  partial-destination cleanup on failure).
- BMP format: 24-bit RGB888, bottom-up, BI_RGB (no compression), 96 DPI,
  zero extra libraries required — opens in any image viewer.
- Uses the LVGL lock (`lvgl_port_lock(0)`) exactly as the rest of the
  codebase does, so it is safe from any task context.
- Sets ERRORLEVEL: 0 on success, 1 on failure (snapshot failed, PSRAM
  exhausted, SD write error, invalid path), 2 on usage error.
- Works in batch files and AUTOEXEC.BAT. Supports redirection to capture
  the transcript output.
- Graceful degradation: PSRAM allocation failures produce clear DOS-style
  errors; LVGL lock acquisition failures are handled; SD card absence is
  reported before any file operations.

### Config

- New tunables in `p4minishell_config.h` and `p4minishell_config.yaml`
  under `screenshot`: display dimensions (1024x600), color format
  (RGB565), BMP begin/end markers, and UART chunk size (512 bytes).
- Enabled `CONFIG_LV_USE_SNAPSHOT=y` in `sdkconfig.defaults`.

### Verification

- Clean build: 0 errors, 0 warnings for the firmware and the test project.
- Hardware (COM11): screenshot command captures the screen, streams the BMP
  over serial with correct magic markers, and saves to SD card successfully.
  The BMP opens correctly in image viewers showing the exact screen content.

---

## [0.24.11] - 2026-08-10

HTTPS support release. Adds the simplest possible, lowest-risk basic HTTPS
capability for development, diagnostics, and scripting: a `httpget` / `wget`
command that performs a plain HTTPS or HTTP GET by reusing the exact
`esp_http_client` stack c6ota already uses for firmware downloads. No new HTTP
library, no POST/PUT, no WebSocket, no certificate-pinning UI, and no change
to the station-only Wi-Fi model.

### Added - `httpget <url> [localfile]` (alias `wget`)

- Performs a simple HTTPS (or HTTP) GET. With no localfile it prints the
  response body to the transcript (bounded, sanitized) after a clear header
  showing HTTP status, content-type, and size; with a localfile it saves the
  exact body to the current working directory / SD card through the storage
  write path (cwd-relative resolution, guarded SD session, free-space
  precheck, partial-destination cleanup on a failed write).
- Sets ERRORLEVEL like DOS: 0 on an HTTP 2xx, 1 on any failure (non-2xx,
  connection refused, TLS failure, body over the size cap, unreachable
  host, Wi-Fi not connected), 2 on a usage error. Works in batch files and
  AUTOEXEC.BAT (`httpget ... && echo ok`, `if errorlevel 1 goto nolink`).
- Supports redirection and pipes: `httpget https://host/page > page.txt`
  captures the printed report; `httpget url | find "200"` pipes it.
- Graceful degradation: with no active connection it prints a clear
  DOS-style error (`run wifi connect first`) and exits non-zero; a network
  timeout is bounded by `P4_CONFIG_HTTP_TIMEOUT_MS` so the worker task is
  never hung.
- All HTTP / TLS code lives inside `components/networking/`
  (`networking_http_get()`), the sole owner of the esp_http_client surface;
  `components/command/` only dispatches and writes the returned body. The
  large receive buffer is allocated from PSRAM
  (`heap_caps_malloc(MALLOC_CAP_SPIRAM)`) with an internal-RAM fallback.

### Config

- New tunables in `p4minishell_config.h`, documented in
  `p4minishell_config.yaml` under `http_client`:
  `P4_CONFIG_HTTP_TIMEOUT_MS` (15 s), `P4_CONFIG_HTTP_MAX_BODY_BYTES`
  (512 KiB PSRAM cap), `P4_CONFIG_HTTP_FOLLOW_REDIRECTS` (1), and
  `P4_CONFIG_HTTP_USER_AGENT` ("P4MiniShell/0.24.11 httpget"), plus
  `P4_CONFIG_HTTP_PRINT_BODY_BYTES` (4 KiB transcript print cap).

### Verification

- Clean build: 0 errors, 0 warnings for the firmware and the test project.
- The only CMake change is adding `esp_http_client` and `esp-tls` to the
  networking component's `REQUIRES`. No SoftAP, new transports, custom RPC,
  or third-party libraries were introduced; station-only Wi-Fi is untouched.
- Hardware (COM11): connected to a 2.4 GHz access point and fetched an HTTPS
  page and an HTTP text endpoint, verifying status/content-type/size output,
  ERRORLEVEL on success, the SD-file save form, the transcript print form,
  and the non-connected error path.

---

## [0.24.10] - 2026-08-10

Networking expansion release. Enriches the Wi-Fi and Bluetooth surfaces so
they feel native to the DOS environment: a full colour-coded `wifi status`
report, an RSSI-sorted `wifi scan` with a bare redirectable form, classic
`ping` and `dns`/`nslookup` connectivity commands that set ERRORLEVEL and
participate in redirection/pipes, and improved hosted Bluetooth status, scan,
and session-scoped advertising names.

### Added - `ping <host-or-ip> [count]`

- Classic ICMP echo over lwIP's `esp_ping` session (inside
  `components/networking`, the sole owner of the lwIP surface). Default 4
  requests, hard cap 10 (`P4_CONFIG_PING_COUNT_MAX`), 1 s timeout and 1 s
  interval.
- Works from the interactive shell **and** from batch files / AUTOEXEC.BAT.
  Sets ERRORLEVEL exactly like DOS: 0 when at least one reply landed, 1 on
  total loss / resolution failure / no connection, 2 on a usage error, so
  `ping 8.8.8.8 && echo up` and `if errorlevel 1 echo down` behave.
- Output goes through the transcript appenders, so `>` / `>>` redirection and
  `|` pipes capture it: `ping 8.8.8.8 > ping.txt`, `ping gw | find "Reply"`.
- Prints each reply line plus the classic summary: packets transmitted /
  received / lost, loss %, and RTT min/avg/max.
- Bounded by construction: the session runs on its own task and the worker
  task blocks for at most `count * (timeout + interval) + margin`, so it
  never hangs the command worker task.
- Requires an active connection; reports Wi-Fi-not-started / not-connected
  states honestly and redirectably.

### Added - `dns <hostname>` (alias `nslookup`)

- Resolves A records through lwIP `getaddrinfo` and prints the IPv4 address
  list (bounded by `P4_CONFIG_DNS_RESULT_LIMIT`).
- Errorlevel-aware like `ping` (0 success / 1 not resolved / 2 usage) and
  redirectable: `dns example.com > dns.txt`, `dns host || echo unresolved`.
- With this build's lwIP DNS cache (`CONFIG_LWIP_DNS_MAX_HOST_IP=1`) a name
  normally resolves to a single A record.

### Added - enriched `wifi status`

- Multi-line colour-coded report: state, connected, target SSID, SSID, BSSID,
  channel, RSSI (dBm), PHY mode + bandwidth (802.11b/g/n/ax + HT20/HT40), IPv4,
  netmask, gateway, DNS server(s), and association uptime (HH:MM:SS since the
  4-way handshake completed).

### Added - improved `wifi scan`

- Results sorted by RSSI (strongest first), column-aligned
  `SSID  RSSI  CH  AUTH` report, capped at the new `P4_CONFIG_WIFI_SCAN_LIMIT`
  (32) so a busy channel cannot flood the transcript.
- Optional `wifi scan /b` prints bare SSID lines (no colour) so
  `wifi scan /b > ap.txt` is machine-parsable from a batch file.

### Added - enriched hosted Bluetooth

- `bluetooth status` / `bt status` is now a colour-coded report: hosted
  ready, controller, NimBLE host, sync, scan, advertising (with the active
  name), C6 firmware version, and last error.
- `bluetooth scan [limit]` is a bounded scan (default 8 s,
  `P4_CONFIG_BT_SCAN_DURATION_MS`) that prints a sorted-by-RSSI
  `NAME  ADDRESS  RSSI` report with the optional per-run limit.
- `bluetooth advertise on [name]` accepts a session-only advertising name
  (RAM-only, never persisted); `bluetooth advertise off` and the boot
  `BT_ADVERTISE=ON|OFF` path are unchanged.

### Documentation

- `command.md` documents `ping`, `dns`/`nslookup`, the `wifi scan /b` and
  enriched status/scan forms, and the new Bluetooth forms. `documentation.md`,
  `ai-context.md`, `readme.md`, `p4minishell_config.h`, and
  `p4minishell_config.yaml` document the new lwIP/esp_ping surface and the new
  config macros (`P4_CONFIG_WIFI_SCAN_LIMIT`, `P4_CONFIG_PING_*`,
  `P4_CONFIG_DNS_RESULT_LIMIT`, `P4_CONFIG_BT_SCAN_DURATION_MS`).

### Verification

- Clean build: 0 errors, 0 warnings for the firmware and the test project.
- Networking code stays confined to `components/networking`; the only
  CMake change is adding `lwip` to that component's `REQUIRES`. No SoftAP,
  Bluedroid, custom RPC, or non-ESP-Hosted transport was introduced.

---

## [0.24.9] - 2026-08-10

Batch mathematics release. Expands `set /a` with the comparison and logical
operators cmd.exe users expect, adds numeric comparison keywords to `if`, and
fully documents the batch math surface. No regressions to the existing
arithmetic, bitwise, shift, compound-assignment, or `if` forms.

### Added — `set /a` comparison operators

- `==`, `!=`, `<`, `>`, `<=`, `>=` now evaluate to 1 when the relation holds
  and 0 otherwise, so a boolean can be computed and stored:
  `set /a x=5==5` sets x=1, `set /a ok=(5>3)&&(2<4)` sets ok=1.
- Precedence follows cmd.exe: comparisons sit below the bitwise operators
  (`|` `^` `&` and the shifts bind tighter), so `set /a "x=1|0==0"` is
  `(1|0)==0` → 0.
- The assignment splitter no longer mistakes the `=` of a comparison for the
  assignment: `set /a x=5==3` assigns x the comparison result (0) instead of
  splitting into `x=5` with a dangling `==3`. An expression with no assignment
  (`set /a 5==3`) is evaluated and printed.
- **Quoting:** `<`, `>`, `&`, `|`, `<<`, `>>`, `&&` and `||` are shell
  redirection/chain/pipe operators, so an expression using them must be quoted
  (`set /a "x=5<6"`) or the line is split first. `==` and `!=` contain no
  shell operator and work unquoted. This matches how the pre-existing bitwise
  operators already behaved.

### Added — `set /a` logical operators

- `&&` and `||` return 1/0 and form the lowest precedence levels (`&&` binds
  tighter than `||`, so `1||0&&0` is `1||(0&&0)` → 1). Both sides are always
  evaluated (no short-circuiting), matching cmd.exe: `set /a "(0&&1/0)"`
  reports a divide-by-zero error.
- The bitwise `&`/`|` levels continue to refuse `&&`/`||`, so a chain separator
  that survives as shell syntax is never swallowed by the expression.

### Added — `if` numeric comparison keywords

- `if [not] [/i] <a> EQU|NEQ|LSS|LEQ|GTR|GEQ <b> <command>` performs a numeric
  comparison. Operands are parsed as decimal integers; a non-numeric operand
  reads as 0, matching cmd.exe. This is a separate branch from the `==` string
  comparison, so `if a==b` stays a string test while `if a EQU b` compares
  numerically. `/i` is ignored for the numeric form.

### Documentation

- `command.md` gained a full `set /a` operator/precedence table, quoting
  rules for shell-conflicting operators, and an `if` numeric-keyword
  reference with examples. `documentation.md`, `ai-context.md`, `API.md`,
  `readme.md`, and `batch.h` document the expanded grammar and the
  `shell_expr_find_assignment()` splitter.

### Testing

- New unit tests `test_batch_expr_comparisons` and `test_batch_expr_logical`
  cover every comparison, `&&`/`||` precedence, parenthesised logical
  expressions, the no-short-circuit rule, and hex/`0x` comparisons.
- Hardware (COM11): exercised `set /a` with `==`/`!=` (unquoted) and
  `<`/`>`/`<=`/`>=`/`&&`/`||` (quoted), mixed precedence, `%%` modulo and
  `^^` xor escapes, every `if` numeric keyword (including `not` and
  variables), and a batch file that computes a total, branches on `EQU`, and
  stores a computed boolean. Full unit-test suite: 0 failures, 0 ignored.

### Verification

- Clean build: 0 errors, 0 warnings for the firmware and the test project.
- CONFIG.SYS / AUTOEXEC.BAT boot path, transcript colours, key waits, and the
  serial console are unchanged.

---

## [0.24.8] - 2026-08-10

Batch correctness release. Fixes the six batch features after the frame layout
change that introduced `%0` (script name) broke `%1..%9` / `%*` argument
forwarding, tightens `goto :eof` frame scoping, and moves the `for` loop
buffers off the recursive command/batch stack.

### Fixed — `%1..%9` / `%*` argument forwarding after the `%0` change

- **Symptom:** `call script.bat one two` ran the script, but inside the callee
  `echo %1` printed the script path instead of `one`, `%2` printed `one`, and
  `%*` included the script name.
- **Root cause:** `shell_execute_batch_file()` was updated to store the script
  path in `args[0]` (classic DOS `%0`) and shift the caller's arguments into
  `args[1..]`, but `shell_expand_variables()` still indexed `%N` as
  `args[N-1]` and built `%*` from `args[0]`. The two disagreeing views made
  every positional reference off by one. `%0` was also unreachable: the
  `isdigit()` branch caught `'0'` first and mapped it to an out-of-range index.
- **Fix:** `shell_expand_variables()` now maps `%0` → `args[0]`, `%1..%9` →
  `args[1..9]`, and `%*` → every argument from `%1` onward (never the script
  name), matching COMMAND.COM. `%0` is tested before `isdigit()`. A single
  `shell_batch_all_args_string()` helper builds `%*` for both the closed
  (`%*`) and bare (`%*` with no trailing `%`) forms.
- **Verification (hardware, COM11):** `call` with `%0 %1 %2 %*` prints the
  script name, the two forwarded arguments, and exactly the two arguments for
  `%*`; `shift` keeps `%1` aligned with the frame slots.

### Fixed — `goto :eof` now unwinds only the current frame from `for`/`if` bodies

- **Symptom:** `goto :eof` (and `goto`) issued inside a `for` loop body in a
  called script terminated the caller too: after the callee returned, the
  caller's line loop broke out and the whole batch run stopped early.
- **Root cause:** a `goto` inside a `for`/`if` body breaks the current frame's
  line loop before the normal flag-clearing path runs, leaving
  `s_goto_pending` / `s_goto_eof` set. The caller's loop then saw the stale
  flags and broke as well.
- **Fix:** `shell_execute_batch_file()` clears `s_goto_pending`,
  `s_goto_eof`, and `s_goto_label` whenever a frame returns. A goto target
  always belongs to the frame that set it (labels are resolved against that
  frame's label table), so the flags are stale after the frame is gone.
- **Verification (hardware, COM11):** a callee with `for %%I in (x) do goto
  :eof` returns without running further callee lines and the caller continues
  to its next line.

### Fixed — `goto :eof` accepted case-insensitively

- `goto :eof` matched with `strcmp`, so `goto :EOF` fell through to the label
  scan and reported "label not found". Now compared case-insensitively, like
  every other batch label.

### Fixed — `for %%I in (set) do ...` accepted the DOS `%%` form

- The loop body substitution only matched a single `%var`, so the correct
  batch-file spelling `for %%I in (a b c) do echo %%I` produced `echo %a`
  (the doubled percent collapsed to one and the variable was lost) instead of
  `echo a`. The substitution now matches `%%var` first (the batch-file escape
  for a literal `%`), then `%var` for tolerance. Verified on hardware:
  `for %%I in (alpha beta gamma) do echo ITEM_%%I` prints ITEM_alpha, ITEM_beta,
  ITEM_gamma.

### Fixed — `%N` / `%0` / `%*` recognized anywhere in a line

- Positional arguments were only expanded when they were the final percent
  marker in the line (or a whole `%...%` pair), so `p0=[%0] p1=[%1]` printed
  `p0=[%0]` literally (a later `%` collapsed `%0] p1=[` into one unknown
  token). They are now consumed as `%` plus one character immediately,
  matching COMMAND.COM, so `[%0] [%1]` expands both.

### Fixed — nested `call` + command overflowed the worker task stack

- **Symptom:** a batch file that `call`ed a second script, and the callee ran a
  `set`/`echo`, panicked with `Stack protection fault` in the `shell_cmd`
  task — the recursive dispatch chain plus newlib's `vfprintf` machinery
  exceeded the 8192-byte worker stack.
- **Root cause:** the recursive-path functions kept SD-path/line-sized buffers
  as stack locals that stacked with nesting depth: `shell_resolve_batch_path`
  (1040-byte frame), `shell_command_set` (512), `shell_command_echo` (416,
  a full `SHELL_BATCH_LINE_BYTES` text buffer), `shell_command_if` (464),
  `shell_command_choice` (736). A two-level `call` with a `set` in the callee
  was enough to overflow.
- **Fix:** every line/path-sized buffer on the recursive batch path is now
  heap-allocated and freed on every exit: `shell_resolve_batch_path`
  (1040 → 160 bytes of stack), `shell_command_set`, `shell_command_echo`
  (416 → 32), `shell_command_set`/`set /a`/`set /p`/`path` statements,
  `shell_command_if`'s resolved path + nested command, and
  `shell_command_choice`'s prompt text (736 → 352).
- **Verification (hardware, COM11):** the exact repro (call → callee runs
  `setlocal`, `set`, `goto :eof`) runs clean with no stack fault; nested
  `call` chains with `set`/`echo`/`if` in the callee all complete.

### Fixed — UART console dropped the tail of long commands

- **Symptom:** commands past ~64 bytes were split into two commands, the second
  becoming an "Unknown command" (e.g. a long `write`/`append` line lost its
  final characters, corrupting batch files created from the serial console —
  this was why the previous test batch files on the SD card were truncated).
- **Root cause:** the USB-Serial-JTAG driver delivers one logical line across
  several reads (its RX FIFO is 64 bytes). The console task treated each read
  as a complete command.
- **Fix:** `shell_uart_console_task()` now assembles partial reads until a line
  terminator (or the buffer fills), and only then submits the command. Key-wait
  forwarding is unchanged: during a key wait the first newly-read character is
  still answered immediately. Verified on hardware: `write`/`append` of 65+
  character lines lands intact, and `pause`/`choice` are still answered by a
  single key.

### Fixed — unit-test app crashed after the debug-log suite

- The test app ran all suites with 0 failures, then panicked. Two pre-existing
  issues, previously masked by the crash, are fixed so the suite runs clean:
  - `shell_schedule_transcript_appendf()` called `lv_async_call()` even before
    any UI exists; in the test app LVGL is never initialized, so the flush
    crashed in LVGL's TLSF allocator. When the transcript widget is NULL the
    flush now runs synchronously instead.
  - The synchronous flush used the async callback's 2048-byte stack scratch,
    overflowing the test app's main task. The scratch is now heap-allocated.
  - The `test_ansi_to_lvgl_recolor` assertion expected plain text to pass
    through uncoloured, but the recolor path deliberately wraps every run in
    `#CCCCCC text #` (the transcript label has no explicit text colour, so that
    is what keeps plain text visible on the dark background). The assertion was
    corrected to match the documented rendering contract.
- **Verification (hardware, COM11):** the full unit-test suite runs to
  "All tests completed" — 56 tests across 11 suites, 0 failures, 0 ignored,
  no panic.

### Verification

- Clean build: 0 errors, 0 warnings for the firmware and the test project.
- Hardware (COM11): exercised `if exist` / `if not exist`, `if errorlevel N`,
  `goto :eof`, `for %%I in (set)` and wildcard `for`, `if /i`, and `call` with
  argument forwarding + errorlevel propagation; CONFIG.SYS / AUTOEXEC.BAT boot
  path, transcript colours, key waits, and serial console behaviour unchanged.

---

## [0.24.7] - 2026-08-10

Feature release. Adds DOS-style boot scripting (`CONFIG.SYS` + `AUTOEXEC.BAT`)
that runs at every boot.

### Added — DOS-style boot scripting (`CONFIG.SYS` / `AUTOEXEC.BAT`)

- **Symptom this replaces:** previously the shell had no boot-time
  configuration; every setting had to be typed or scripted by hand after
  power-on.
- **Behavior:** on every boot the firmware looks for `CONFIG.SYS` and
  `AUTOEXEC.BAT` on the SD card root. When either is missing and
  `P4_CONFIG_BOOT_GENERATE_DEFAULTS` is set, default files are written once.
  `CONFIG.SYS` directives are parsed and applied (classic DOS: `SET`, `PATH`,
  `PROMPT`, `ECHO ON|OFF`; display/audio: `ROTATE`, `BRIGHTNESS`,
  `DISPLAY_POWER`, `VOLUME`; policy: `WIFI_SSID`/`WIFI_PASSWORD`/
  `WIFI_AUTOCONNECT`, `BLUETOOTH`, `BT_ADVERTISE`, `USB_KEYBOARD`,
  `USB_MOUSE`, `GPIO`). `AUTOEXEC.BAT` then runs through the normal batch
  pipeline with full batch power. Unknown directives produce a single muted
  warning and are skipped. Safe with no SD card (silent skip, identical to
  before) and with empty/malformed files.
- **Implementation:** `components/boot/` owns the parser and runner
  (`components/boot/boot.c`, `boot.h`). Hardware directives are applied by
  executing their command-line equivalent through the batch pipeline, so every
  existing validation path is reused and no private state is reached into.
  State-only directives (Wi-Fi target credentials, autoconnect policy, the
  echo default) use the small set of accessors the owning modules expose
  (`networking_wifi_set_boot_credentials`/`_autoconnect`,
  `batch_set_default_echo`). Wi-Fi password is never echoed to transcript,
  history, or debug log. GPIO directives refuse reserved pins (delegated to
  the existing `gpio set` safety check). Serial console key waits now work:
  `shell_uart_console_submit_command()` routes commands through the command
  worker task so blocking key waits (`pause`, `choice`, `more`, the
  `format`/`disk clean` confirmation) can be answered from serial input.
- **Configuration:** `p4minishell_config.h` adds
  `P4_CONFIG_BOOT_CONFIG_SYS_NAME`, `P4_CONFIG_BOOT_AUTOEXEC_BAT_NAME`,
  `P4_CONFIG_BOOT_GENERATE_DEFAULTS`, `P4_CONFIG_BOOT_RUN_ON_STARTUP`,
  `P4_CONFIG_BOOT_LINE_BYTES`, `P4_CONFIG_BOOT_MAX_DIRECTIVES`,
  `P4_CONFIG_BOOT_MAX_GPIO_LINES`; all documented in
  `p4minishell_config.yaml` under `boot_scripting`.
- **Verification (hardware, COM11):** `CONFIG.SYS` with `ECHO OFF`,
  `SET TESTVAR=helloboot`, `BRIGHTNESS=50`, `ROTATE=0`, `VOLUME=30`,
  `WIFI_AUTOCONNECT=OFF`, `USB_KEYBOARD=ON`, `GPIO 42 = OUT HIGH` (refused:
  reserved pin), and an unknown directive (muted warning) — all applied as
  expected; `AUTOEXEC.BAT` with `@echo off` +
  `echo testvar=%TESTVAR%` ran and expanded the variable; unit-test suite
  runs 0 failures.

---

## [0.24.6] - 2026-08-09

Crash-fix + stability release. Root-caused and fixed the recurring intermittent
LVGL crash (C1), upgraded LVGL to 9.4.0, fixed the deterministic `dir` crash
(C2), and swept all remaining literal ANSI markers (H1 residual).

### Added — diskpart / DOS FORMAT disk and volume management (`format` + `disk`)

- **`format` is now a real FORMAT.COM.** It keeps `/FS:`, `/V:label`, `/Q` and
  the exact-`YES` confirmation contract, and adds `/A:size` (allocation unit /
  cluster size, with K/M suffix). `/FS:` is honored honestly: FAT/FAT32 use the
  standard ESP-IDF helper's size-appropriate selection (FAT12/16 for small
  volumes, FAT32 for modern SD cards), and `EXFAT` is refused with a clear
  "not supported in this firmware build" warning that falls back to FAT32
  rather than silently lying. After formatting the command reports the FAT
  type, label, capacity and cluster size with the semantic palette.
- **New `disk` command family** (diskpart-style), owned by `components/storage`:
  - `disk list` — physical disk geometry (name, capacity, sectors, sector size).
  - `disk detail` — disk geometry plus the decoded MBR partition table (boot
    flag, type with a friendly name, start LBA, size).
  - `disk clean` — remove the partition table (destructive, `YES` required).
  - `disk create partition primary [size=N]` — create a primary FAT32 MBR
    partition aligned to 1 MiB; `size` is in MB (default: rest of the card).
  - `disk delete partition N` — delete MBR partition N (1-4), `YES` required.
  - `disk format [fs=...] [label=...] [au=...] [quick]` — diskpart-style alias
    for the `format` engine.
- **Engine:** `components/storage/storage.c` gained the volume services
  (`storage_disk_get_info`, `storage_disk_read_mbr`, `storage_disk_clean`,
  `storage_disk_create_primary_partition`, `storage_disk_delete_partition`,
  `storage_format_volume`, `storage_get_fat_type`), parameterized by a
  `storage_volume_t` so a future USB OTG MSC volume can be added without
  changing the command surface. Formatting uses the standard
  `esp_vfs_fat_sdcard_format_cfg()`; MBR access uses `sdmmc_read_sectors` /
  `sdmmc_write_sectors` on the BSP card handle; the FATFS volume is unmounted
  (`f_mount`) before partition-table writes and recreated by `format`.
- **Serial console key waits now work.** `shell_uart_console_submit_command()`
  routes serial commands through the command worker task (a new
  `execute_command_async` hook in `shell_command_ops_t`) instead of executing
  synchronously on the UART task, so blocking key waits - `pause`, `choice`,
  `more`, the `format`/`disk clean` confirmation, `set /p` - can be answered
  from the serial input. The touch path is unchanged.
- **Configuration:** `P4_CONFIG_FORMAT_ALLOC_UNIT_MIN`/`_MAX`,
  `P4_CONFIG_DISK_PARTITION_ALIGN_SECTORS`, `P4_CONFIG_STORAGE_VOLUME_MAX`
  added to `p4minishell_config.h` and documented in
  `p4minishell_config.yaml`.
- **Verification (hardware, COM11):** `format /FS:FAT32 /V:TEST /A:64K`,
  `format /FS:FAT32`, `disk format fs=fat32 label=DATA au=32K quick`,
  `disk clean`, `disk create partition primary size=1024/2048`,
  `disk delete partition 1`, `disk list`, `disk detail` all execute with the
  expected result and ANSI colours; `disk detail` verifies the created MBR
  (type 0x0C FAT32, 1 MiB-aligned start, requested size); the
  clean → create → format → `dir` flow works end-to-end; unit-test suite
  runs 0 failures. Known limitation: a card the BSP cannot mount after a
  reboot (e.g. a partition with no filesystem) cannot be re-initialized
  in-firmware without `BOARD_CFG_SD_FORMAT_ON_MOUNT_FAIL` - the pre-existing
  constraint that formatting requires an initialized card.

### Fixed — LVGL task hang: UI freezes after boot, touch keyboard unresponsive

- **Symptom:** the board boots, boot and Wi-Fi messages render on the display
  with colours, then the whole UI freezes: the touch keyboard stops echoing
  input and no command output appears on screen. The serial console keeps
  working. On hardware the LVGL task spins forever inside `lv_timer_handler`
  and the FreeRTOS task watchdog fires (`taskLVGL` running on CPU 0, IDLE0
  starved) until the board resets.
- **Root cause:** `windows_set_transcript_text()` applied the transcript label
  **synchronously on the calling task**: `ansi_to_lvgl_recolor()` +
  `lv_label_set_text()` + `lv_obj_update_layout()` + `lv_obj_scroll_to_y()`
  were executed from the command worker task, the UART console task, and the
  networking background task while the LVGL task was mid-render. Applying a
  large flex-grow label's text and forcing its layout from a non-LVGL task
  races the LVGL render cycle and hangs `lv_timer_handler`, freezing the whole
  surface. Reverting the transcript to a plain `lv_textarea` (which never
  forced layout/scroll) confirmed the label update path was the trigger.
- **Fix:** `components/windows/windows.c` — the label update is now **deferred
  to the LVGL task**. `windows_set_transcript_text()` converts the ANSI text to
  recolor markup into a persistent staging buffer and schedules a coalesced
  `lv_async_call`; the callback runs on the LVGL task where it paints the
  newest staged content (`lv_label_set_text` + layout + scroll-to-end). Bursts
  of output coalesce into one apply per handler pass. `windows_scroll_
  transcript_to_end()` schedules the same apply instead of doing layout/scroll
  from a non-LVGL task. On-screen colours are unchanged.
- **Verification:** 5 minutes of continuous command output (`help`, `sysinfo`,
  `dir /s`, `wifi diag`, `keyboard show`/`hide`/`status`, `clear`, `echo`,
  `set /a`) on hardware with **zero** task-watchdog triggers and zero panics;
  unit-test suite runs clean.

### Fixed — transcript stops updating after the first command

- **Symptom:** after the hang fix, the first command after boot renders on the
  display, but the second command's output never appears on screen (the serial
  console still shows it).
- **Root cause:** the LVGL recolor markup (`#RRGGBB text #`) inflates the raw
  ANSI text by roughly 1.5-2x because every colour change carries an open/close
  marker. The staging buffer was sized at `P4_CONFIG_TRANSCRIPT_BYTES` (8192),
  so after boot + the first command the buffer was already full
  (`ansi_to_lvgl_recolor` reported 8188 bytes). The converter dropped every
  segment that did not fit — the **tail** — so the newest command output was
  the part that vanished from the label.
- **Fix:**
  1. `p4minishell_config.h` / `p4minishell_config.yaml` — added
     `P4_CONFIG_TRANSCRIPT_RECOLOR_BYTES` (2x the plain transcript) so a full
     8 KiB scrollback fits in recolor form (verified: 9179/9538-byte markups
     render instead of being cut at 8192).
  2. `components/ansi/ansi.c` — `ansi_to_lvgl_recolor()` now keeps the
     **newest** segments: when the buffer cannot hold the whole transcript it
     discards what is staged and restarts from the current segment, so the
     newest lines stay visible even under pathological colour density.
- **Verification:** 3 minutes of sequential commands (`help`, `mem`, `sysinfo`,
  `dir`, `dir /s`, `wifi diag`, `ver`, `echo`) on hardware — the transcript
  label reaches a full-length recolor markup, scrolls to the bottom
  (`scroll_bottom == 0`, view at the newest output), with zero task-watchdog
  triggers and zero panics.

### Fixed — recurring intermittent LVGL crash (C1)

- **Symptom:** after `wifi connect` and other header-touching commands, an
  intermittent `Guru Meditation` in the LVGL task:
  - `lv_obj_get_style_prop` ← `trans_anim_start_cb` / `trans_anim_completed_cb`
    ← `anim_timer`, with a corrupted transition descriptor (`tr->obj` = `0xdac`,
    `selector` = `0x10000`), or `anim_timer` calling a freed animation's
    `exec_cb` (jumping to the LVGL pool base). Repro: 3-7 crashes / 10 trials on
    `sd info; sysinfo; if exist /sdcard echo ok; bluetooth advertise on`.
- **Root cause:** the LVGL default theme's **style-transition animations**
  (`LV_THEME_DEFAULT_TRANSITION_TIME=80`). Widgets restyled on every refresh
  (header, transcript, input line, keyboard) churn pending 80 ms transitions;
  under load (Bluetooth NimBLE init) churned transition descriptors are freed
  and reused while their animation still references them, corrupting the LVGL
  pool. Not a locking or object-lifecycle bug — no object deletion and an
  intact pool free-list were confirmed during diagnosis.
- **Fix:**
  1. Upgraded LVGL from 9.2.2 → 9.3.0 → **9.4.0** (large upstream bug-fix
     release; `main/idf_component.yml` and `test/main/idf_component.yml` pin
     `lvgl/lvgl: "9.4.0"`).
  2. Disabled the risky theme style-transition animations with
     `CONFIG_LV_THEME_DEFAULT_TRANSITION_TIME=0` (durable in
     `sdkconfig.defaults`). Style changes are now instant — the safe
     replacement for the transition churn. Scroll, cursor, and interaction
     behaviour are unaffected (they are separate from style transitions).
- **Verification:** 0 crashes / 14 trials of the exact repro, 0 / 8 trials of
  the full stack (wifi connect + bluetooth + header commands + `dir /s`), and
  Wi-Fi connected to the test AP (`4G-CPE_5542`, IP 192.168.199.225) with no
  crash.

### Fixed — deterministic `dir` crash (C2)

- **Symptom:** every `dir` in `/sdcard` printed ` Directory of /sdcard` and then
  panicked with `Load access fault`. The register dump showed `strlen` being
  called with the ASCII value `"test"` (0x74736574) as its string pointer — the
  first four bytes of the `test.txt` entry name — which then dereferenced that
  garbage address.
- **Root cause:** `components/storage/storage_commands.c` — the per-entry colour
  formatting in `shell_dir_list_one()` called the *va_list-taking* variant
  `ansi_vformat(coloured_name, sizeof(coloured_name), colour_fmt, display_name)`
  but passed `display_name` (a `char *`) where a `va_list` is expected. Inside
  `ansi_vformat()` the `%s` handler ran `va_arg(args, char *)`, which treated the
  first four bytes of the `display_name` array as a pointer — `"test"` — and
  handed it to `snprintf`, whose `%s` processing called `strlen("test")` and
  faulted.
- **Fix:** use the varargs wrapper `ansi_format()` at that call site so the
  entry name is passed as a value argument, exactly like every other call site in
  the codebase (an audit confirmed all remaining `ansi_vformat()` callers pass a
  real `va_list`). The entry colour (`SH_FILE`/`SH_DIR`/`SH_EXE`) still renders
  as real SGR escapes.
- **Verification:** 10 rapid stress trials (`dir`, `dir /w`, `dir /s`,
  `dir /b /s`, `chkdsk /F`, `tree`, wifi status, rotation) all booted and ran
  with zero Guru Meditation faults; `dir` now prints `test.txt` correctly with
  colours.

### Fixed — residual literal ANSI `@` markers (H1)

- A hardware sweep over every command still found literal `@y(unsynced)@R`,
  `@Kno@R`, `@c...@K...@R` markers that the v0.24.3 fix missed, all caused by
  palette macros passed as `%s` argument values (which `ansi_vformat` leaves
  unconverted by design) instead of in the format string:
  - `sysinfo` / `about` — `time ... (unsynced)` and `idf: unknown`.
  - `wifi status` — `connected=@Kno@R`.
  - `battery` / `battery sleep` — `light sleep requested=@Kno@R`.
  - `display power` — `display.power: @Koff@R`.
  - `bluetooth status` — `bluetooth_appendf()` used `vsnprintf()` (never
    converts `@`) before the ANSI append; the whole line showed markers.
- **Fix:** moved every palette marker into the format string (branching on
  state) so `ansi_vformat` converts them, and `bluetooth_appendf()` now routes
  through `ansi_vformat()` like the networking module.
- **Verification:** hardware sweep of `dir`, `sd info|stat|ls`, `wifi status|diag`,
  `keyboard status`, `ver`, `sysinfo`, `about`, `mem`, `usb status`, `battery`,
  `bluetooth status`, `display power` — zero literal `@` markers remain.

### Fixed — LVGL transcript was monochrome (colours only on UART)

- The LVGL transcript was a plain `lv_textarea` fed with stripped text, so the
  on-screen shell rendered every line in a single colour while the UART console
  showed the full palette.
- **Fix:** the transcript is an `lv_spangroup`; `windows_set_transcript_text()`
  parses the ANSI text and creates one span per coloured run with
  `lv_style_set_text_color()`, so each colour renders directly on screen (no
  markup to misinterpret). `shell.c` keeps a parallel ANSI buffer
  (`s_transcript_ansi`) alongside the plain one and hands it to the window
  manager on every update.
- **Deferred rebuild:** because rebuilding the span group deletes and recreates
  spans, doing it synchronously from an LVGL event (e.g. on-screen keyboard
  submit) left LVGL's `lv_draw_span` referencing freed spans — a `Load access
  fault` after keyboard input. The rebuild is now buffered and deferred via
  `lv_async_call` so it runs after the current redraw pass.
- **Verification:** firmware boots, colours render on the display, and the
  previously-crashing keyboard-input sequence (`keyboard show`/`hide` + `help` +
  `dir` + `sysinfo`) runs clean.

### Fixed — shell parser / batch / variable bugs found by the unit tests

A full hardware run of the unit-test suite surfaced several latent bugs:

- `shell_parse_percentage_arg()` / `shell_parse_size_arg()` accepted an empty
  string (missing argument silently treated as 0). Now rejects when `strtol`
  consumed no digits.
- `shell_split_chain()` emitted a trailing empty segment for whitespace-only
  input or a dangling separator; now skipped.
- Bare positional args (`%0`, `%1`..`%9`, `%*`) without a batch frame printed
  literally; now expand to empty per the documented DOS rule.
- `set /a` accepted out-of-range literal `2147483648` via `strtol` clamp; now
  rejected with `ERANGE` so `-2147483648/-1` is refused as overflow.
- Corrected two stale unit tests (`a^^|b` pipe semantics, in-place chain-buffer
  reuse).

**Verification:** the full unit-test suite runs clean — every suite reports 0
failures.

---

### Fixed — date command error message did not state the expected format

- `date 2026-08-08` (YYYY-MM-DD) rejected with "value out of range" but did not
  tell the user the expected format was `MM-DD-YYYY`, so the rejection was
  confusing.
- **Fix:** `components/command/command.c` — `shell_command_date()` now prints the
  usage line (`Usage: date [MM-DD-YYYY]`) alongside the range error, and the
  warning record notes the expected order. Verified on hardware: `date 08-08-2026`
  sets the date; `date 2026-08-08` explains the expected format.

### Observations reviewed and confirmed as designed / non-bugs

- `echo %UNDEFINEDVAR%` prints the literal name — this is the documented "unknown
  names left untouched" rule in `batch.h`. The unit-test assertion that
  contradicted it was corrected in v0.24.4.
- `echo on` / `echo off` set the echo flag and print nothing, matching DOS (only
  bare `echo` prints the status). The report's wording concern does not apply.
- Wi-Fi scan showing empty SSIDs (`ssid= rssi=0`) was a boot-timing race; after
  the runtime stabilises the scan returns real access points including the test
  AP. No code change.
- Transcript echo of a piped stage shows both the stage output and the
  downstream result — expected given the transcript-delta redirection design. No
  code change.

### Recurring — intermittent LVGL crash (C1)

- The `wifi connect` command during testing triggered the same intermittent LVGL
  crash documented in C1 (fault in `lv_obj_get_style_prop` via
  `trans_anim_start_cb`). This confirms the v0.24.1 mitigation reduced the
  frequency but did not eliminate the root cause: a style-transition animation
  accessing an LVGL object that has been freed. Dedicated debugging with the LVGL
  pool walk enabled and a full audit of LVGL object lifecycle during UI rebuild
  is still recommended.

---

## [0.24.4] - 2026-08-08

Test correctness release. Resolved the final outstanding item from the hardware
test report's recommended-next-steps list.

### Fixed — test_shell_variables.c assertion

- The `test_variable_expansion_env_var()` test asserted that an undefined variable
  (`%UNDEFINED%`) expands to an empty string, but `shell_expand_variables()` leaves
  unknown names untouched per its documented contract ("unknown names are left
  untouched, and `%%` yields `%`"). The assertion was corrected to expect the
  literal `%UNDEFINED%` to pass through. Verified: test project builds clean.

---

## [0.24.3] - 2026-08-08

Colour and correctness release. Eliminated the literal `@`-specifier markers that
appeared in command output, fixed the medium-severity hardware-test issues (M1-M4),
and corrected three command-dispatch bugs found while testing on real hardware.

### Fixed — ANSI colour markers rendered literally in command output

A systemic issue: commands composed output with `@`-specifier palette macros and
sent the string through a path that never converted them to real SGR escapes.
`ansi_vformat()` only converts `@`-specifiers that appear literally in the format
string; `@`-specifiers inside substituted `%s` arguments are deliberately left
untouched (injection guard). Every call site that relied on converting `@`-specifiers
from a `%s` argument — or that used a plain `vsnprintf`/`snprintf` path — emitted
the raw `@K...@R` markers on both the LVGL transcript and the UART console.

Affected commands: `dir`, `sd info|stat|ls`, `wifi status|diag`, `keyboard status`,
`ver`, `sysinfo`, `about`, `mem`, `usb status`.

Fix:
- `components/shell/shell.c` — `shell_print_coloured()`, `shell_print_field()`,
  and `shell_print_field_num()` now route the entire format + arguments through
  `ansi_vformat()`, so every `@`-specifier is in the format string and gets
  converted. Added a `shell_colour_value()` helper for values whose colour depends
  on runtime state.
- `components/storage/storage_commands.c` — `shell_dir_emitf()` now uses
  `ansi_vformat()` instead of `vsnprintf()`, and the `dir` listing entry colour
  is applied via a runtime-built format string.
- `components/networking/networking.c` — conditional-colour `wifi diag` values
  are pre-converted with `ansi_format()`; the module's plain `networking_schedulef`
  is left for machine-readable output.
- `components/command/command_ui.c` — `keyboard status` conditional colour uses
  separate format strings.
- `components/command/command.c` — `display.state`, `c6.hosted_transport.busy`,
  and `heap` colour in `ver`/`sysinfo` now use inline colours or the
  `shell_colour_value()` helper.

### Fixed — `cd` printed `\` while the prompt showed `/sdcard`

- `shell_fs_print_cwd()` now prints the full VFS path to match the prompt.

### Fixed — `attrib` with no path showed a bare error

- `shell_command_attrib()` now shows usage when no path is given.

### Fixed — keypress wait timeout reduced for headless use

- Reduced `P4_CONFIG_KEY_WAIT_TIMEOUT_MS` from 30000 to 10000.

### Improved — battery telemetry shows calibration state

- `battery` now reports `calibrated=yes|no` in the detail line.

### Fixed — family commands printed help instead of executing

- `wifi status|scan|diag`, `bluetooth status|scan`, `usb status`, `sd info|stat|ls|cat`
  printed help instead of running. Root cause: `shell_split_args()` truncated the
  command string passed to family handlers. Fixed with a heap copy in
  `shell_execute_command_core()`.

### Fixed — `sd stat`/`sd ls`/`sd cat` showed a usage error

- Same mutation bug in `shell_command_sd()`; fixed with a `strdup` before splitting.

### Fixed — `shell_print_*` helpers emitted literal `@`-specifiers

- `shell_print_coloured()` now routes through `ansi_vformat()`.

---

## [0.24.2] - 2026-08-08

Hardware stability and polish release. Fixed the intermittent LVGL crash that
surfaced during bring-up, addressed the medium-severity issues from the hardware
test report (M1–M4), and applied three additional correctness fixes found while
testing on real hardware.

### Fixed — intermittent LVGL crash (heap corruption on style transition)

- **Symptoms:** Intermittent `Guru Meditation Error: Core 0 panic'ed (Load/Store
  access fault)` rebooting the device after commands like `if exist /sdcard`,
  `sd info`, `bluetooth advertise on`. Fault decoded to `lv_obj_get_style_prop`
  (`lv_obj_style.c:330`) running from `trans_anim_start_cb` → `anim_timer` on the
  LVGL task, with LVGL pool corruption (`lv_tlsf_free` → `remove_free_block`).
- **Root cause:** The header component's `header_render()` and the keyboard
  component's `keyboard_show()`/`keyboard_hide()` made direct LVGL API calls
  without holding the LVGL port lock. When called from a non-LVGL task (the
  fallback render path when `lv_async_call` fails, or the command worker task
  for keyboard commands), these raced with the LVGL render cycle and corrupted
  the LVGL heap pool.
- **Fix:**
  - `components/header/header.c` — `header_render()` now acquires
    `lvgl_port_lock(0)` for its entire body and releases with
    `lvgl_port_unlock()`. Added `#include "esp_lvgl_port.h"` and added
    `espressif__esp_lvgl_port` to the component's `REQUIRES`.
  - `components/keyboard/keyboard.c` — `keyboard_show()` and
    `keyboard_hide()` now acquire `lvgl_port_lock(0)` around their LVGL widget
    operations. Added `#include "esp_lvgl_port.h"`.
- **Verification:** A stress test exercising all previously-crashing commands
  plus rapid header-touching commands ran clean twice with no Guru Meditation
  errors.

### Fixed — family commands printed help instead of executing

- `wifi status`, `wifi scan`, `wifi diag`, `bluetooth status`, `bluetooth scan`,
  `usb status` all printed the command help/usage instead of running the
  subcommand.
- **Root cause:** `shell_split_args()` writes token terminators into the
  command buffer in place. By the time the dispatcher reached the module-routed
  family handlers, `command` was truncated to just the first token (`"wifi"`),
  so the family parser saw `argc <= 1` and fell through to help.
- **Fix:** `components/command/command.c` — `shell_execute_command_core()` now
  preserves a heap copy of the trimmed command for family-prefix lines
  (`strdup` before `shell_split_args`) and passes it to the family handlers.
  The copy is freed in every branch.

### Fixed — `sd stat` / `sd ls` / `sd cat` showed a usage error

- `sd stat test.txt` printed the `sd` usage instead of the stat result.
- **Root cause:** Same in-place mutation inside `shell_command_sd()` —
  `shell_split_args(command, ...)` truncated `command` to `"sd"` before it was
  handed to the sub-handlers.
- **Fix:** `components/storage/storage_commands.c` — `shell_command_sd()` now
  `strdup`s the command before splitting and passes the copy to the
  sub-handlers, restructuring to a single exit that frees the copy.

### Fixed — `shell_print_*` helpers emitted literal `@`-specifiers

- Every usage/error/ok/warning/muted/heading line showed raw colour markers,
  e.g. `@yUsage: brightness <0-100>@R`, `@gCreated directory ...@R`,
  `@GFolder PATH listing for volume A:@R`.
- **Root cause:** `shell_print_coloured()` composed `@y...@R` and called
  `shell_transcript_append_ansi()`, which only strips real `\x1b` sequences and
  does not convert `@`-specifiers.
- **Fix:** `components/shell/shell.c` — `shell_print_coloured()` now builds a
  format string with the colour/reset as literal `@`-specifiers and routes the
  composed text through `shell_transcript_appendf_ansi()` (i.e. `ansi_vformat`),
  so colour and reset are converted and the rendered body is a `%s` argument
  (literal `%` in body stays data).

### Fixed — `cd` printed `\` while the prompt showed `/sdcard`

- `cd` (no args) printed `\` (the FAT root) while the prompt rendered `/sdcard`
  (the VFS path). The two surfaces disagreed about the current directory.
- **Fix:** `components/storage/storage.c` — `shell_fs_print_cwd()` now always
  prints the full VFS path (`s_shell_cwd`), matching the prompt.

### Fixed — `attrib` with no path showed a bare error

- `attrib` with no arguments printed `attrib: cannot access .` with no usage
  hint.
- **Fix:** `components/storage/storage_commands.c` — `shell_command_attrib()`
  now shows usage when no path is given.

### Fixed — keypress wait timeout reduced for headless use

- `pause`/`choice`/`more` blocked for the full 30 s with no interactive key
  source before falling back, stalling headless batch files.
- **Fix:** Reduced `P4_CONFIG_KEY_WAIT_TIMEOUT_MS` from 30000 to 10000 in
  `p4minishell_config.h` and `p4minishell_config.yaml`. Still long enough for
  a human to respond interactively; less painful for scripted/headless use.

### Improved — battery telemetry shows calibration state

- `battery` now reports `calibrated=yes|no` in the detail line so the user
  knows whether the voltage reading comes from the calibrated ADC driver or
  the approximate linear fallback.
- **Where:** `components/command/command.c` — `shell_command_battery()`.

---

## [0.24.1] - 2026-08-08

Hardware bring-up release for real-ESP32-P4 testing. Three stack-protection faults
and one LVGL concurrency bug discovered and fixed during first-flash bring-up on
the JC1060P470 development board. Documentation issues from the roadmap resolved.

### Fixed — main-task stack overflow during UI construction

- `app_main()` builds the entire LVGL UI on the 3584-byte main-task stack via
  `shell_build_ui()` → `windows_init()`. The transcript append and prompt rendering
  paths include newlib `vfprintf` calls that consume ~1–1.5 KB of stack alone,
  overflowing the guard and triggering `Stack protection fault` in
  `vfprintf` → `__sbprintf`.
- Bumped `CONFIG_ESP_MAIN_TASK_STACK_SIZE` from 3584 to 8192 in
  `sdkconfig.defaults`, matching the command-worker-task budget and the
  `esp_lvgl_port` task's own 7168-byte stack.
- Added the new config value to `sdkconfig.defaults` with a descriptive comment
  explaining why the default is insufficient.

### Fixed — LVGL assertion hang from concurrent transcript appends

- `shell_transcript_append_internal()`, `shell_transcript_reset()`,
  `shell_history_transcript_scroll_to_end()`, and `shell_input_line_set_text()`
  performed direct LVGL object manipulation (`lv_textarea_set_text`,
  `lv_obj_update_layout`, `lv_obj_scroll_to_y`) from any calling task — including
  the Wi-Fi background task, the command worker task, and the UART console task
  — without holding the LVGL render lock.
- When the Wi-Fi background task called these functions while the LVGL task was
  mid-render, the `LV_ASSERT_MSG(!disp->rendering_in_progress, ...)` assertion
  fired, invoking the default `while(1)` handler. The task watchdog triggered
  every five seconds on CPU 1, but the system never recovered.
- All four functions now acquire `lvgl_port_lock(0)` (recursive mutex) around
  their LVGL sections. The mutex is recursive, so the LVGL event path, the async
  transcript flush callback, and the UART-console command path — all of which
  already hold the lock — nest without deadlock. The lock is only taken when the
  LVGL port is initialized (guarded by transcript/input-line non-NULL checks).

### Fixed — esp_event task stack overflow during Wi-Fi handler

- The `sys_evt` task (CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE, default 2304) runs
  the project's Wi-Fi event handler (`networking_wifi_event_handler`). That handler
  calls `networking_wifi_append_step()` → `networking_schedulef_ansi()` →
  `vsnprintf()` into a 512-byte stack buffer, then `networking_record_infof()` and
  `networking_notify_headerf()`. The newlib `vfprintf` machinery on this path
  consumes ~1.5–2 KB, overflowing the 2304-byte task stack immediately after
  `esp_wifi_start()`.
- Bumped `CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE` from 2304 to 8192 in
  `sdkconfig.defaults`. The esp_event system task is created once; the extra
  ~5.8 KB SRAM cost is negligible and consistent with the other task budgets in
  this firmware.

### Documentation — roadmap issues resolved

- **`command.md` `rgb`/`camera` in wrong table** — The "Where Commands Live" table
  listed `rgb` and `camera` as hardware commands alongside working commands like
  `brightness`, `rotate`, etc., but these are stubs that only print error messages.
  Moved them out of the hardware table and clarified the "Unsupported Commands" table
  to note that these entries are stubs.
- **`p4minishell_config.yaml` missing v0.23.0 Kconfig changes** — The YAML file
  header still said version 0.24.0 and did not document the two new task stack size
  entries added during hardware bring-up. Updated the header to version 0.24.1 with
  the bring-up notes, and added `main_task` (8192) and `system_event` (8192) entries
  to the `task_stacks` section with full descriptions explaining why the defaults
  were insufficient.

### Hardware support — roadmap review resolved

- **ESP32-P4 ADC attenuation value (issue 9)** — Verified as false alarm. The
  ESP-IDF header `hal/adc_types.h:51` defines `ADC_ATTEN_DB_12 = 3` which IS the
  correct enum value on ESP32-P4. The code at `p4minishell_config.h:451` using
  `P4_CONFIG_BATTERY_ATTEN = 3` with comment `/* ADC_ATTEN_DB_12 */` is correct.
- **ADC calibration fallback (issue 10)** — Confirmed as informational, not a bug.
  ESP32-P4 supports curve fitting calibration. The uncalibrated fallback is expected
  when eFuse data is absent and degrades gracefully.
- **Coprocessor firmware directory (issue 11)** — Already documented in
  `coprocessor/esp32c6_slave/README.md` which states "No custom slave source code"
  and explains the upstream build approach. No change needed.

- **`command.md` `rgb`/`camera` in wrong table** — The "Where Commands Live" table
  listed `rgb` and `camera` as hardware commands alongside working commands like
  `brightness`, `rotate`, etc., but these are stubs that only print error messages.
  Moved them out of the hardware table and clarified the "Unsupported Commands" table
  to note that these entries are stubs.
- **`p4minishell_config.yaml` missing v0.23.0 Kconfig changes** — The YAML file
  header still said version 0.24.0 and did not document the two new task stack size
  entries added during hardware bring-up. Updated the header to version 0.24.1 with
  the bring-up notes, and added `main_task` (8192) and `system_event` (8192) entries
  to the `task_stacks` section with full descriptions explaining why the defaults
  were insufficient.

### Code quality — semantic helper newline consolidation

- Refactored `shell_print_coloured()` to accept a `bool newline` parameter, so the
  newline is included in the ANSI output rather than appended as a separate plain-text
  call. All six single-colour helpers (`shell_print_heading`, `shell_print_ok`,
  `shell_print_error`, `shell_print_warning`, `shell_print_muted`, `shell_print_usage`)
  now call `shell_print_coloured(..., true, args)` directly. `shell_print_field` and
  `shell_print_field_num` were unchanged — they embed `\n` in their own snprintf and
  do not use `shell_print_coloured()`.

### Code quality — shell_extract_input_text optimization

- `shell_extract_input_text()` called `strlen(s_input_line_prompt)` and
  `strlen(SHELL_PROMPT)` twice each (once for `strncmp`, once for the pointer
  offset). Cached both lengths in local variables so each is computed once.

### Code quality — UART console fgets() documentation

- Added clarifying comment to `shell_uart_console_task()` explaining why the
  `fgets()` + `clearerr(stdin)` + `vTaskDelay(20ms)` pattern is safe for
  USB-Serial-JTAG: stdin is set to unbuffered mode (`_IONBF`) so `fgets()`
  reads character-by-character from the underlying driver, and the error
  recovery loop handles disconnect/reconnect correctly.

### Code quality — debug command deduplication

- Removed `debug.wifi_state` (same as `wifi status` state line) and
  `debug.free_heap` (same as `mem` heap line) from `shell_command_debug()`.
  The debug command now focuses on its unique value: the debug log entries
  and warning count. Users can run `wifi status` or `mem` for the full
  network and memory state.

### API — ansi.h documentation corrected

- Rewrote the `ansi_vformat()` specifier table in `ansi.h` to match the actual
  implementation. Removed the non-existent lowercase "off" specifiers (`@d`,
  `@i`, `@u`) that were documented but never implemented. Removed the duplicate
  `@b` entry (was listed as both "bold off" and "foreground blue"). Corrected
  `@k`/`@K` descriptions to match the implementation (standard black vs bright
  black). Added `@@` literal escape to the documented table.

### SDK — ESP-Hosted version gate configurable

- Added `P4_CONFIG_HOSTED_SKIP_VERSION_GATE` (default 0) to
  `p4minishell_config.h`. When set to 1, the Wi-Fi and Bluetooth init paths
  do not reject C6 firmware version mismatches — mismatches are logged as
  warnings instead of returning `ESP_ERR_INVALID_STATE`. Useful for
  development and testing with mismatched host/co-processor firmware. Both
  `networking_wifi_validate_hosted_version()` in `networking.c` and the
  bluetooth version check in `bluetooth.c` respect this config. Documented
  in `p4minishell_config.yaml` under `wifi.hosted_skip_version_gate`.

### Testing — variable expansion and debug log coverage

- Added `test/main/test_shell_variables.c` with nine test functions covering
  `shell_expand_variables()`: environment variable expansion (`%VAR%`),
  undefined variables, empty variable names (`%%`), single-quote literal
  protection, double-quote expansion, caret-escape passthrough, multiple
  variables, output buffer truncation, NULL safety, and batch argument
  expansion without an active frame.
- Added `test/main/test_shell_debug_log.c` with six test functions covering
  the debug log ring buffer and warning counter: `shell_debug_log_push()`,
  `shell_record_warningf()`, `shell_record_errorf()`, `shell_record_infof()`,
  `shell_get_warning_count()`, ring overflow behavior, and NULL safety.
- Updated `test/main/test_main.c` and `test/main/CMakeLists.txt` to register
  the new test suites.

### Optimization — persistent command worker task

- Replaced per-command FreeRTOS task creation with a persistent worker task
  (`command_worker_task`) and a FreeRTOS queue (`s_command_queue`, depth 4).
  `shell_execute_command_async()` now posts commands to the queue via
  `xQueueSend` instead of calling `xTaskCreate` + `calloc` for each command.
  The worker task runs at `tskIDLE_PRIORITY + 2` and processes commands
  sequentially. Eliminates ~1-2ms task-creation overhead per command and
  reduces heap fragmentation from repeated task stack allocation. Full queues
  are logged as warnings with the command dropped.

### Optimization — SD I/O buffer size increased

- Increased `P4_CONFIG_SD_IO_BUFFER_BYTES` from 128 to 512 bytes (matching
  `P4_CONFIG_FILE_IO_BUFFER_BYTES` for consistency). Reduces the number of
  small read/write calls for `storage_copy_file()`, pipe spool operations,
  and `sd cat`. Stack impact is +384 bytes per function — well within the
  8192-byte command worker task budget.

### Fixed — intermittent LVGL crash (heap corruption on style transition)

- **Symptoms:** Intermittent `Guru Meditation Error: Core 0 panic'ed (Load/Store
  access fault)` rebooting the device after commands like `if exist /sdcard`,
  `sd info`, `bluetooth advertise on`. Fault decoded to `lv_obj_get_style_prop`
  (`lv_obj_style.c:330`) running from `trans_anim_start_cb` → `anim_timer` on the
  LVGL task, with LVGL pool corruption (`lv_tlsf_free` → `remove_free_block`).
- **Root cause:** The header component's `header_render()` and the keyboard
  component's `keyboard_show()`/`keyboard_hide()` made direct LVGL API calls
  without holding the LVGL port lock. When called from a non-LVGL task (the
  fallback render path when `lv_async_call` fails, or the command worker task
  for keyboard commands), these raced with the LVGL render cycle and corrupted
  the LVGL heap pool.
- **Fix:**
  - `components/header/header.c` — `header_render()` now acquires
    `lvgl_port_lock(0)` for its entire body and releases with
    `lvgl_port_unlock()`. Added `#include "esp_lvgl_port.h"` and added
    `espressif__esp_lvgl_port` to the component's `REQUIRES`.
  - `components/keyboard/keyboard.c` — `keyboard_show()` and
    `keyboard_hide()` now acquire `lvgl_port_lock(0)` around their LVGL widget
    operations. Added `#include "esp_lvgl_port.h"`.
- **Verification:** A stress test exercising all previously-crashing commands
  plus rapid header-touching commands ran clean twice with no Guru Meditation
  errors.

### Fixed — family commands printed help instead of executing

- `wifi status`, `wifi scan`, `wifi diag`, `bluetooth status`, `bluetooth scan`,
  `usb status` all printed the command help/usage instead of running the
  subcommand.
- **Root cause:** `shell_split_args()` writes token terminators into the
  command buffer in place. By the time the dispatcher reached the module-routed
  family handlers, `command` was truncated to just the first token (`"wifi"`),
  so the family parser saw `argc <= 1` and fell through to help.
- **Fix:** `components/command/command.c` — `shell_execute_command_core()` now
  preserves a heap copy of the trimmed command for family-prefix lines
  (`strdup` before `shell_split_args`) and passes it to the family handlers.
  The copy is freed in every branch.

### Fixed — `sd stat` / `sd ls` / `sd cat` showed a usage error

- `sd stat test.txt` printed the `sd` usage instead of the stat result.
- **Root cause:** Same in-place mutation inside `shell_command_sd()` —
  `shell_split_args(command, ...)` truncated `command` to `"sd"` before it was
  handed to the sub-handlers.
- **Fix:** `components/storage/storage_commands.c` — `shell_command_sd()` now
  `strdup`s the command before splitting and passes the copy to the
  sub-handlers, restructuring to a single exit that frees the copy.

### Fixed — `shell_print_*` helpers emitted literal `@`-specifiers

- Every usage/error/ok/warning/muted/heading line showed raw colour markers,
  e.g. `@yUsage: brightness <0-100>@R`, `@gCreated directory ...@R`,
  `@GFolder PATH listing for volume A:@R`.
- **Root cause:** `shell_print_coloured()` composed `@y...@R` and called
  `shell_transcript_append_ansi()`, which only strips real `\x1b` sequences and
  does not convert `@`-specifiers.
- **Fix:** `components/shell/shell.c` — `shell_print_coloured()` now builds a
  format string with the colour/reset as literal `@`-specifiers and routes the
  composed text through `shell_transcript_appendf_ansi()` (i.e. `ansi_vformat`),
  so colour and reset are converted and the rendered body is a `%s` argument
  (literal `%` in body stays data).

---

## [0.24.0] - 2026-08-08

Bug fixes and correctness release. **No commands, options, output formats, or behaviors
were removed.** The semantic `SH_*` palette migration started in v0.22.0 is now complete,
and two latent correctness issues are resolved.

### Fixed — `shell_get_cwd_for_prompt()` was not thread-safe

- The function used a `static` buffer and returned a pointer to it. Both the UART
  console task and the LVGL input-line task render the prompt and call this function,
  so the shared buffer was a race condition: a path truncation in one task could be
  overwritten by the other before the first task finished using it.
- The function now writes into a caller-provided buffer. `shell_prompt_expand()` and
  `shell_uart_console_print_prompt()` each provide their own stack buffer. No shared
  state remains.
- Updated the declaration in `components/shell/shell.h`, the test in
  `test/main/test_shell_prompt.c`, and the API reference in `API.md`.

### Fixed — semantic palette migration incomplete

- `shell_command_debug()` in `components/shell/` and the `display`, `keyboard`,
  `windows`, `reboot`, "Unknown command", and out-of-memory paths in
  `components/command/` still used raw `@`-specifiers (`@C`, `@R`, `@Y`, `@y`, `@g`,
  `@r`, `@G`, `@B`, `@K`) instead of the `SH_*` macros from
  `components/ansi/ansi_palette.h`. The v0.22.0 changelog claimed "every command
  group migrated" — that is now true.
- Every site in `shell_command_help()`, `shell_command_sysinfo()`,
  `shell_command_version()`, `shell_command_about()`, `shell_command_mem()`, and
  `shell_command_debug()` in `shell.c`, and `shell_command_reboot()`,
  `shell_execute_rgb_command()` stubs, and the `display`/`keyboard`/`windows`
  subcommand handlers in `command.c`, now uses `SH_*` macros.

### Fixed — stale documentation

- `documentation.md` described the optimization level as "Performance (-O3)" but
  `sdkconfig.defaults` has specified `CONFIG_COMPILER_OPTIMIZATION_SIZE=y` since
  v0.23.0. The documentation now says "Size (`-Os`)".
- `roadmap.md` claimed "main.c is 445 lines" and "command.c is 1,140 lines". The
  actual counts are 499 and 1,793 respectively. Corrected.

### Added — pipeline and chain test coverage

- Added `test/main/test_shell_pipeline.c` with seven new test functions
  covering the command pipeline surfaces that previously had no tests:
  - `test_pipe_detection_agreement` — verifies that `shell_has_unquoted_char()`
    and `shell_split_args()` never disagree about whether a `|` is syntax or
    data, across double quotes, single quotes, caret escapes, pipes after
    closed quotes, and mixed real/quoted pipes.
  - `test_redirection_quote_awareness` — verifies that `>`, `>>`, and `<`
    inside double quotes, single quotes, and caret escapes are treated as data.
  - `test_chain_pipe_vs_chain_under_quotes` — verifies that a `|` inside
    quotes does not split a command chain.
  - `test_chain_single_quote_protection` — verifies that `&`, `&&`, `||`
    inside single quotes are data, not chain separators.
  - `test_chain_truncation_with_pipelines` — verifies that the segment limit
    works correctly when each link is itself a pipeline.
  - `test_chain_empty_and_whitespace` — verifies edge cases for empty input
    and leading separators.
  - `test_find_unquoted_pipe` — verifies the pipe scanner directly: bare pipe,
    quoted pipe, escaped pipe, pipe after closed quote, NULL input.

### Changed — architecture and maintainability

- **`shell.c` no longer depends on networking, Bluetooth, USB, or C6 OTA.** The
  shell core previously included `networking.h`, `bluetooth.h`, `usb.h`, and
  `c6ota.h` directly. External-module state is now read through 11 new function
  pointers in `shell_command_ops_t` (`wifi_is_connected`, `wifi_get_rssi`,
  `wifi_state_string`, `append_sysinfo_summary`, `bluetooth_is_enabled`,
  `bluetooth_is_connected`, `usb_is_connected`, `usb_is_keyboard_attached`,
  `usb_key_to_ascii`, `c6ota_is_pending`, `c6ota_is_busy`), all registered
  from `command_init()`. Every hook is NULL-checked before use, so the shell
  degrades gracefully if a module is unavailable.
- **`command_ui.c` extracted from `command.c`.** The `display`, `keyboard`, and
  `windows` subcommand handlers now live in their own translation unit, so
  `command.c` no longer includes `keyboard.h` or `windows.h`. `display.h` is
  still required for the `brightness` and `rotate` hardware commands.
- **`coprocessor/esp32c6_slave/README.md`** now explicitly states that there is
  no custom slave source code — the firmware is built entirely from the managed
  `espressif__esp_hosted` component.
- **`p4minishell_config.yaml`** gained a `build_constraints` section documenting
  the build-affecting Kconfig options (`CONFIG_SPIRAM_XIP_FROM_PSRAM`,
  `CONFIG_COMPILER_OPTIMIZATION_SIZE`, `CONFIG_LOG_DEFAULT_LEVEL_WARN`,
  `CONFIG_ESP_WIFI_SOFTAP_SUPPORT`, `CONFIG_WIFI_RMT_SOFTAP_SUPPORT`).

### Notes

- No regressions: every change is a semantics-preserving substitution (raw
  specifier → `SH_*` macro that expands to the exact same specifier), a
  signature change with all call sites updated, or a pure code-move to a new
  translation unit. No command output text was altered.
- `shell_parse_redirection()` and `shell_execute_pipe()` are not directly
  unit-tested: the former is `static` (its quote-aware behavior is verified
  indirectly through `test_redirection_quote_awareness`), and the latter is
  tightly coupled to SD card spool files and `batch_run_nested()`.
- Clean build: 0 errors, 0 warnings for firmware and tests on ESP-IDF v5.5.5 /
  esp32p4 (to be verified).

---

## [0.23.0] - 2026-08-08

Consolidates the networking layer onto the official Espressif path and makes the
station-only constraint structural. **No commands, options, or output were removed** —
the dispatcher stays at 71 verbs and `wifi`, `bluetooth`/`bt`, and `c6ota` behave
identically.

The module was already centralized and already used only `espressif/esp_hosted` plus
`espressif/esp_wifi_remote`, with no custom RPC or alternative transport. This release
closes the gaps that remained: initialization order, one encapsulation leak, and
several constraints that were conventional rather than enforced.

### Changed — canonical initialization order

`networking_wifi_start_runtime()` previously ran `nvs_flash_init()` **before** bringing
up the hosted transport. The order is now exactly as specified, and documented in
`networking.h` with the reason each step precedes the next:

1. `esp_hosted_init()`
2. `esp_hosted_connect_to_slave()`
3. Version compatibility gate
4. `nvs_flash_init()` (erase-and-retry on a corrupt partition)
5. `esp_netif_init()`
6. `esp_event_loop_create_default()`
7. `esp_netif_create_default_wifi_sta()`
8. `esp_wifi_init()` via `esp_wifi_remote`
9. Event handler registration
10. `esp_wifi_set_mode(WIFI_MODE_STA)`
11. `esp_wifi_start()`

Bringing the transport up first means a dead or mismatched co-processor is reported as
a transport fault instead of surfacing later as a confusing Wi-Fi init error.

### Fixed — encapsulation leak in the header status refresh

- `shell_header_status_refresh()` called `esp_wifi_sta_get_ap_info()` directly to read
  RSSI, which was the only `esp_wifi_*` call outside `components/networking/`.
- Added **`networking_wifi_get_rssi()`** to the module's public API. It also skips the
  driver query entirely when disconnected, saving a needless SDIO round trip on every
  header refresh.
- `components/shell/` no longer includes `esp_wifi.h` and no longer declares `esp_wifi`
  as a component dependency.
- Verified: no `esp_hosted_*`, `esp_wifi_*`, `esp_netif_*`, or NimBLE call remains
  outside `components/networking/`. The sole sanctioned exception is
  `components/c6ota/`, which drives `esp_hosted_slave_ota_*` because co-processor
  firmware update is its entire purpose.

### Changed — station-only is now structural

- `CONFIG_ESP_WIFI_SOFTAP_SUPPORT` and `CONFIG_WIFI_RMT_SOFTAP_SUPPORT` are both
  disabled in `sdkconfig.defaults`. The code always called
  `esp_wifi_set_mode(WIFI_MODE_STA)`, but SoftAP was still compiled in.
- Both symbols are needed: `esp_wifi_remote` mirrors the Wi-Fi Kconfig under its own
  `WIFI_RMT_` prefix, and on the hosted path that mirror is the authoritative one.

### Fixed — build constraints were not durable

Regenerating `sdkconfig` from `sdkconfig.defaults` revealed that three documented
constraints existed only in the generated file, so any regeneration silently reverted
them. All three are now pinned in `sdkconfig.defaults`:

- **`CONFIG_SPIRAM_XIP_FROM_PSRAM=n`** — the defaults file actually had this set to `y`,
  directly contradicting the documented constraint that PSRAM XIP mapping must stay
  disabled to avoid overflowing the flash/PSRAM budget at link time
- **`CONFIG_COMPILER_OPTIMIZATION_SIZE=y`** — the defaults file specified
  `OPTIMIZATION_PERF`, and regenerating grew the image by roughly 280 KB
- **`CONFIG_LOG_DEFAULT_LEVEL_WARN=y`** — warn-level compile-time logging

### Fixed — latent buffer truncation in `dir /w`

- The wide-listing row buffer could not hold a full set of columns plus one clipped
  over-long cell, and the `strncat()` bound was computed from a runtime `strlen()` so
  the compiler could not prove it safe. Surfaced as `-Werror=stringop-truncation` once
  the regenerated config restored the stricter optimization level.
- The row buffer is now sized for every column at full width plus one clipped cell, and
  the append tracks the row length explicitly so the bound is a compile-time constant.

### Notes

- Only the official path is used: `espressif/esp_hosted` for transport,
  `espressif/esp_wifi_remote` for the Wi-Fi API, hosted NimBLE over VHCI for BLE. No
  custom RPC, no alternative transport, no re-implemented control plane.
- The version compatibility gate is unchanged and still refuses init with a clear
  diagnostic when the C6 firmware is outside the host's supported range.
- `c6ota` remains the only supported co-processor update path, and its capture,
  shutdown, wait, and restore hooks are unchanged.
- Clean build: 0 errors, 0 warnings for both firmware and test projects on ESP-IDF
  v5.5.5 / esp32p4

---

## [0.22.0] - 2026-08-08

Gives the shell a complete, built-in default colour scheme. Every category of
output now uses a consistent colour chosen centrally, applied automatically on both
the LVGL transcript and the serial console, with no per-command decisions left to
individual implementations. **No commands, options, or output text were removed** —
the dispatcher stays at 71 verbs and every message keeps its exact wording.

### Added — Semantic colour palette

- **New `components/ansi/ansi_palette.h`** is the single source of truth for what
  colour each kind of output is. It defines named `SH_*` macros that expand to the
  existing `@`-specifiers, so nothing scattered across the tree picks colours by hand
  and a future palette change is a one-file edit.
- The ansi module owns the header because it owns the specifier vocabulary. The macros
  are plain string literals, so this introduces **no new component dependencies** and
  the ansi module remains a leaf.

| Category | Macro | Colour |
|----------|-------|--------|
| Section heading | `SH_HEAD` | Bright green |
| Sub-heading | `SH_SUBHEAD` | Bright yellow |
| Field label / key | `SH_LBL` | Cyan |
| Body text | `SH_TEXT` | Bright green |
| Muted / secondary / timestamp | `SH_MUTE` | Bright black (grey) |
| Success | `SH_OK` / `SH_OK_HI` | Green / bright green |
| Error | `SH_ERR` / `SH_ERR_HI` | Red / bright red |
| Warning | `SH_WARN` / `SH_WARN_HI` | Yellow / bright yellow |
| Important value (SSID, IP, MAC) | `SH_VAL` | Bright white |
| Number, size, percentage | `SH_NUM` | Bright magenta |
| Path / filename | `SH_PATH` | Bright blue |
| Prompt / command name | `SH_PROMPT` / `SH_CMD` | Bright cyan |
| Usage syntax | `SH_USAGE` | Yellow |
| Help description | `SH_DESC` | White |
| Directory entry | `SH_DIR` | Bold bright blue |
| File entry | `SH_FILE` | White |
| Executable / `.bat` entry | `SH_EXE` | Bright green |
| Listing size / timestamp | `SH_SIZE` / `SH_TIME` | Bright magenta / grey |
| Wi-Fi up / down | `SH_NET_UP` / `SH_NET_DOWN` | Green / grey |
| Bluetooth | `SH_BT` | Magenta |
| USB attached / detached | `SH_USB_UP` / `SH_USB_DOWN` | Green / grey |
| OTA | `SH_OTA` | Bright green |

### Added — Semantic print helpers

Seven wrappers over `shell_transcript_appendf_ansi()` in `components/shell/`, so the
common shapes need no colour decision at the call site. Each emits its own reset and
newline:

```c
void shell_print_heading(const char *format, ...);
void shell_print_field(const char *label, const char *format, ...);
void shell_print_field_num(const char *label, long value);
void shell_print_ok(const char *format, ...);
void shell_print_error(const char *format, ...);
void shell_print_warning(const char *format, ...);
void shell_print_muted(const char *format, ...);
void shell_print_usage(const char *format, ...);
```

They render the caller's text with real `vsnprintf` before wrapping it, so a `%s` value
containing an `@` can never be mistaken for a colour specifier.

### Fixed — muted text was nearly invisible

- **`@k` is pure black (0x0C0C0C), not grey**, and the default background is dark blue
  (0x012456). Thirty output sites across `networking.c`, `shell.c`, and `command.c`
  used `@k` for muted text, rendering it almost unreadable. All now use `@K`
  (bright black / grey) via `SH_MUTE`. This affected `wifi status`, `wifi diag`,
  `wifi scan`, and the keyboard status line among others.
- The palette header documents this and the other specifier traps (`@E` is bright red,
  `@B` is bold rather than blue, `@L` is bright blue) so the mistake is not repeatable.

### Fixed — `ansi_format()` discarded printf width and precision

- The formatter captured the full specifier but then re-rendered with only the bare
  conversion, silently dropping flags. `%10s`, `%-4s`, `%05d`, and `%.2f` all lost their
  formatting when routed through the ANSI path.
- It now passes the captured specifier to `snprintf` verbatim and detects `l` and `ll`
  length modifiers to pull the correct type from the `va_list`.
- This is what makes the right-aligned `chkdsk` capacity report and the `dir` column
  layout survive colouring. Covered by a new test suite.

### Changed — every command group migrated

- **System**: `help`, `sysinfo`, `version`, `about`, `mem`, `debug`
- **Hardware**: `brightness`, `rotate`, `battery`, `volume`, `gpio`, `display`,
  `keyboard`, `windows` — cyan labels with bright-magenta numbers throughout
- **Storage**: every file command, plus `attrib`, `label`, `xcopy`, `chkdsk`, `format`,
  and the `sd` family. Around 105 error paths and 51 usage lines now route through
  `shell_print_error()` and `shell_print_usage()`.
- **Directory listings**: `dir` and `sd ls` colour each entry by kind — bold bright
  blue for directories, bright green for runnable `.bat` files, white for ordinary
  files — with grey timestamps and magenta sizes. The colour is applied *after* all
  width formatting so column alignment is unaffected, and `dir /b` stays deliberately
  uncoloured so redirected output remains machine-parsable.
- **Batch**: `set`, `set /a`, `set /p`, `path`, `echo`, and echoed batch lines
- **Wi-Fi, Bluetooth, USB**: status fields now use the shared label/value colouring,
  with connected states in green and disconnected in grey
- **C6 OTA**: progress, completion, warnings, and the YES prompt are coloured at the
  `main.c` bridge rather than inside the module, so `components/c6ota` keeps emitting
  the exact contractual strings documented in `API.md`. The destructive confirmation
  prompt is bright red.
- `usb_host_transcript_append_text()` and the Bluetooth emitter now route through the
  ANSI path so palette macros in those modules are interpreted.

### Notes

- Defaults only. There is no runtime theme or user configuration in this change.
- Dual-output correctness is preserved: the transcript strips escapes for the LVGL
  textarea while the serial console receives the raw sequences, exactly as before.
- The single remaining raw-SGR site is `shell_uart_console_print_prompt()`, which writes
  straight to the console with colour numbers parameterised from `P4_CONFIG_PS_COLOR_*`
  and never passes through `ansi_format()`. It is commented as the documented exception.

### Testing

- Added `test_ansi_format_width_flags` covering string and integer width, left and right
  alignment, zero padding, float precision, the `l` modifier, hex width, and width
  combined with a colour code
- Clean build: 0 errors, 0 warnings for both firmware and test projects on ESP-IDF
  v5.5.5 / esp32p4

---

## [0.21.0] - 2026-08-08

Completes the batch language and closes the last filesystem parity gap. Delivery phase 1 is
now fully done alongside phase 2. **No commands, options, output formats, or behaviors were
removed.** The dispatcher stays at 71 verbs — `set /a` and `set /p` are sub-forms of the
existing `set`, not new commands — and every previously working `set`, `copy`, `xcopy`, and
batch file still behaves identically.

### Added — `set /a` arithmetic expressions

A recursive-descent evaluator over 32-bit signed integers implementing the COMMAND.COM
operator set and precedence:

| Precedence | Operators |
|------------|-----------|
| Lowest | `\|` bitwise or |
| | `^` bitwise xor |
| | `&` bitwise and |
| | `<<` `>>` shifts |
| | `+` `-` additive |
| | `*` `/` `%` multiplicative |
| | `-` `~` `!` unary |
| Highest | `( )` grouping |

- **`set /a NAME=<expression>`** evaluates and stores; **`set /a <expression>`** with no
  assignment prints the result without touching the environment
- **Compound assignments**: `+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `^=`, `<<=`, `>>=`
- Numbers accept decimal, `0x` hex, and leading-zero octal
- A bare identifier reads an environment variable, and **an undefined variable evaluates to
  0** exactly as DOS does — which is what makes `set /a count=count+1` work on first use
- Errors are detected and reported rather than trapping: divide by zero, modulo by zero,
  `INT32_MIN / -1` overflow, unbalanced parentheses, malformed numbers, and trailing garbage
- Shifts of 32 or more are clamped instead of invoking undefined behavior
- The `&` and `|` handlers deliberately refuse to consume `&&` and `||`, so an expression can
  never swallow a command-chain separator from v0.19.0
- Exposed as `shell_expr_evaluate()` in `batch.h` so the evaluator is unit-testable

### Added — `set /p` prompted input

- **`set /p NAME=<prompt text>`** prints the prompt and reads a line through the key-wait
  facility added in v0.18.0
- Backspace edits the line, ESC cancels, and Enter submits
- **An empty line leaves the variable unchanged**, matching DOS, rather than clearing it
- Sets errorlevel to 1 when input was cancelled, timed out, or unavailable, so a batch file
  can branch on it
- Added **`shell_read_line()`** to the shell core — the reusable line-collection primitive
  `set /p` needed, echoing as it types and opening its own keypress wait so input never
  reaches the command dispatcher

### Added — Line continuation

- A trailing **`^`** joins the next physical line, so a long command can be split for
  readability
- Bounded by `P4_CONFIG_LINE_CONTINUATION_MAX` (8) so a malformed file cannot loop
- Uses an **odd-caret-count rule**: `^^` at end of line is an escaped literal caret and not a
  continuation, consistent with the escaping rules added in v0.19.0
- **The label scanner applies the identical rule.** Without this, a continued line whose tail
  begins with `:` would register a phantom label and silently corrupt `goto` targets.

### Added — Attribute preservation across `copy` and `xcopy`

- New **`storage_copy_attributes()`** carries R/H/S/A from source to destination
- Wired into `shell_fs_copy_file()`, so all five copy call sites inherit it at once: `copy`,
  wildcard `copy`, `move`'s copy-then-delete fallback, `xcopy`, and `xcopy /S`
- **`xcopy /S` also carries attributes onto the directories it creates**, so a hidden or
  system folder stays that way
- Only the user-visible bits are copied; `AM_DIR` is structural and is never forced onto a
  destination
- Applied **after** the data is written, because a read-only destination cannot be opened for
  writing
- **Non-fatal by design**: the file content is already correct, so losing an archive bit does
  not discard a completed transfer. The failure is recorded in the debug log instead.

### Changed

- A copy that fails because the destination is read-only now reports
  `use attrib -R to clear it` instead of a bare error. This matters more now that copies
  propagate the read-only bit, making the situation reachable in normal use.
- `help` lists the `set /a` and `set /p` forms

### Configuration

New `P4_CONFIG_*` macros, all documented in `p4minishell_config.yaml`:
- `P4_CONFIG_SET_EXPR_DEPTH_MAX` (16) — parenthesis nesting limit in `set /a`
- `P4_CONFIG_SET_PROMPT_INPUT_BYTES` (128) — maximum `set /p` input length
- `P4_CONFIG_LINE_CONTINUATION_MAX` (8) — maximum lines joined by trailing `^`

### Testing

- Added `test/main/test_batch_expr.c` with five suites: literals and number bases, arithmetic
  and precedence including associativity, bitwise and shift operators including the clamped
  over-wide shift, variable reads including the undefined-reads-as-zero rule, and error
  detection including divide by zero and `INT32_MIN / -1`
- `test/main/CMakeLists.txt` gained the `batch` component; `test_main.c` calls `batch_init()`
- Clean build: 0 errors, 0 warnings for both firmware and test projects on ESP-IDF v5.5.5 /
  esp32p4

---

## [0.20.0] - 2026-08-08

Completes roadmap Phase 2, the stronger storage model. **No commands, options, output
formats, or behaviors were removed.** Three verbs were added (`chkdsk`, `scandisk`,
`format`), taking the dispatcher from 68 to 71; every previously working invocation of
`dir`, `copy`, `move`, `write`, and `sd info` still works unchanged.

### Added — Volume management

- **`chkdsk [path] [/F]`** (alias **`scandisk`**) — reports the volume label, total, used and
  free space, allocation unit size, and total and free cluster counts. `/F` additionally walks
  every directory verifying each entry is readable, reporting file and directory counts and
  any unreadable directory.
  The check is deliberately **read-only**. This firmware never rewrites FAT structures: a card
  with real corruption should be imaged and repaired on a host, so problems are reported
  honestly rather than silently "fixed" by an embedded shell. The output says so explicitly.
- **`format [/FS:FAT|FAT32|EXFAT] [/V:label] [/Q]`** — reformats the card through
  `esp_vfs_fat_sdcard_format()`, then optionally applies a volume label and reports the
  resulting geometry.
  Gated behind the exact confirmation word `P4_CONFIG_FORMAT_CONFIRM_WORD`, collected one key
  at a time through the shared key queue so the answer never reaches the command dispatcher —
  the same danger contract `c6ota` uses. **Refuses outright when no interactive input source
  is attached**, so a batch file can never silently wipe a card. The filesystem type is
  validated before the warning is even shown, so a typo cannot reach the prompt.

### Added — Full `dir` option set

| Option | Behavior |
|--------|----------|
| `/W` | Wide multi-column listing, DOS-style `[dirname]` bracketing, `~` clipping for long names |
| `/P` | Pause after each screenful; Enter or Space continues, `Q` quits |
| `/S` | Recurse into subdirectories with per-directory counts plus a grand total |
| `/B` | Bare output, names only; full paths under `/S` so it can be piped |
| `/L` | Lowercase names |
| `/A[:]attrs` | Filter by `D` dir, `H` hidden, `S` system, `R` read-only, `A` archive; `-` prefix excludes |
| `/O[:]order` | Sort by `N` name, `S` size, `E` extension, `D` date, `G` dirs first; `-` reverses |

- Detailed listings now include a `YYYY-MM-DD  HH:MM` timestamp per entry, decoded from the
  FATFS date and time words
- Every listing closes with free space; `/S` adds an explicit grand-total banner so the
  per-directory counts above are not mistaken for the whole tree
- Name is the tie-breaker for every sort key, so ordering is deterministic
- Options may appear before or after the path, with or without the `:` separator
- The existing no-option and wildcard behaviors are unchanged

### Added — Free-space reporting and guardrails

- **`storage_get_space_info()`** — total, used, and free bytes plus cluster geometry via
  `f_getfree()`. Handles both FATFS sector-size configurations.
- **`storage_check_free_space()`** — refuses a write that would leave less than
  `P4_CONFIG_STORAGE_FREE_MARGIN_BYTES` (64 KB) free, so the volume never fills to the point
  where FAT metadata updates begin to fail. An overwrite correctly credits the space the
  destination already occupies, so replacing a file in place does not need double the room.
  When the capacity query itself fails the operation is allowed to proceed rather than
  blocking a legitimate write on a diagnostic failure.
- **`storage_paths_are_same()`** and **`storage_get_file_size()`** supporting helpers
- **`sd info`** now reports filesystem total, used, and free space and the allocation unit
  size alongside the existing raw card capacity

### Fixed — data-loss bug in `copy`

- **`copy` onto itself truncated the source.** `copy a.txt a.txt` opened the destination with
  `"wb"`, which truncated the file to zero bytes before the first read, destroying it. The
  copy helper now detects this (case-insensitively, since FAT is) and refuses. `move` gained
  the same guard because its copy-then-delete fallback hit the identical path.
- **A failed copy left a truncated destination.** A write error mid-transfer left a partial
  file that looked complete. The partial destination is now removed and the removal is
  reported.
- `copy` prechecks free space before opening either file, and reports percentage progress for
  files above `P4_CONFIG_COPY_PROGRESS_THRESHOLD` so a multi-megabyte transfer does not look
  like a hang.
- `write` and `append` precheck free space **before** opening the file, so a truncating
  overwrite cannot destroy the existing contents and then fail for lack of room.

### Fixed — recursive directory walkers overflowed the worker stack

Found by applying the stack-discipline rule added in v0.19.0 to the new code, which also
surfaced the same problem in the existing `tree` walker.

- All three recursive walkers held large per-level buffers on the shared 8 KB command worker
  task stack. At the 8-level depth limit they exceeded it.
- Moved each walker's per-level state into a single heap block released before descending:
  `dir` **1472 → 752** bytes, `chkdsk` **1024 → 400**, `tree` **976 → 752**.
- At full depth these now total roughly 6 KB, 3.2 KB, and 6 KB respectively, inside the 8 KB
  budget. Verified from the disassembled prologues rather than by inspection.

### Configuration

New `P4_CONFIG_*` macros, all documented in `p4minishell_config.yaml`:
- `P4_CONFIG_DIR_SORT_ENTRY_MAX` (128) — entries buffered per level for sorting
- `P4_CONFIG_DIR_WIDE_COLUMNS` (4) and `P4_CONFIG_DIR_WIDE_COLUMN_WIDTH` (18)
- `P4_CONFIG_DIR_PAGE_LINES` (20) — rows between `/P` pauses
- `P4_CONFIG_DIR_RECURSE_DEPTH_MAX` (8) — `/S` and `chkdsk /F` recursion limit
- `P4_CONFIG_STORAGE_FREE_MARGIN_BYTES` (64 KB) — free-space safety margin
- `P4_CONFIG_COPY_PROGRESS_THRESHOLD` (256 KB) and `P4_CONFIG_COPY_PROGRESS_STEP_PCT` (10)
- `P4_CONFIG_FORMAT_CONFIRM_WORD` (`"YES"`) and `P4_CONFIG_FORMAT_ALLOC_UNIT_BYTES` (0)

### Testing

- Added `test/main/test_storage_format.c` with four suites: size formatting across every unit
  boundary including the GiB ceiling, DOS wildcard matching including multi-star cases, the
  self-copy path-identity guard, and the path helpers
- `test/main/CMakeLists.txt` gained the `storage` component
- Clean build: 0 errors, 0 warnings for both firmware and test projects on ESP-IDF v5.5.5 /
  esp32p4

---

## [0.19.0] - 2026-08-08

Completes the command interpreter parity section of the roadmap. **No commands, options,
output formats, or behaviors were removed.** The dispatcher verb table is unchanged at 68
verbs, verified by diff against the previous release.

### Added — Quoting and escaping rules

The shell gained one quote/escape scanner in `components/shell/`, and every surface that has
to tell syntax from data now shares it: the argument tokenizer, redirection parsing, pipe
splitting, chain splitting, and variable expansion. Previously each had its own ad-hoc
double-quote check, and none of them handled escapes.

- **`"text"`** groups an argument and still expands `%VAR%`, matching COMMAND.COM
- **`'text'`** groups an argument literally — no variable expansion and no escape processing
  inside the run
- **`^c`** escapes any single character, so `^&`, `^|`, `^>`, `^<`, `^"`, `^%`, and `^^` are
  data rather than syntax
- **New API**: `shell_find_unquoted_char()`, `shell_find_unquoted_any()`,
  `shell_has_unquoted_char()`, `shell_unescape_in_place()`, and the `shell_quote_state_t` enum
- **`shell_split_args()` rewritten** to honor all three rules and strip the markup, so a
  handler receives the literal value. `echo "hello world"`, `echo 'hello world'`, and
  `echo hello^ world` all produce the same single argument.
- **`shell_expand_variables()`** skips single-quoted runs entirely and passes `^%` through
  untouched

### Added — Command chaining with `&`, `&&`, and `||`

- **`a & b`** runs both commands unconditionally
- **`a && b`** runs `b` only when `a` succeeded
- **`a || b`** runs `b` only when `a` failed
- Operators mix freely on one line: `a && b || c & d`
- Up to `P4_CONFIG_CHAIN_SEGMENT_MAX` (8) commands per chain, with truncation reported rather
  than silently dropping the tail
- **New API**: `shell_split_chain()` with `shell_chain_segment_t` and `shell_chain_op_t`
- **A single `|` is deliberately not a chain separator**, so pipelines still reach the pipe
  executor. `type f | sort && echo done` splits into one pipeline plus one conditional link.
- **Splitting happens before expansion**, per segment, so a variable whose value contains `&`
  cannot inject a new command. This is the same ordering COMMAND.COM uses and it is a
  deliberate safety property, not an implementation artifact.
- **Success is tracked per link** rather than re-read from global errorlevel, so a stale value
  from an earlier line cannot send the next link down the wrong branch
- Unrecognized commands now set `P4_CONFIG_ERRORLEVEL_UNKNOWN_COMMAND` (9009), matching
  COMMAND.COM, which is what makes `badcmd || echo fallback` work

### Fixed — batch frame overflowed the worker task stack (pre-existing)

Found while reviewing the stack cost of the new chaining code. This bug predates this release
and would have caused stack-overflow crashes on any batch file.

- `shell_execute_batch_file()` placed a **12,272-byte stack frame** on the **8,192-byte**
  command worker task stack, so a single batch file already overflowed it before any nesting.
- The label table alone accounted for 8 KB: 32 label slots each sized at the full 256-byte
  command width, when a label is one short identifier.
- **Fix**: label names are bounded by the new `P4_CONFIG_BATCH_LABEL_BYTES` (48), and both the
  frame and the line buffer moved to the heap. The stack frame is now **96 bytes**.
- The command pipeline's two expansion buffers also moved to the heap, taking
  `shell_execute_command()` from **2,032 to 480 bytes**, because it sits on the same recursion
  path — a batch file re-enters it for every line.
- Four levels of batch nesting now consume roughly 5.6 KB of the 8 KB stack with headroom to
  spare. Verified by disassembling the function prologues, not by inspection.
- Every new allocation is released on every exit path, including the early-return error paths.

### Fixed — `if` command correctness

- **`if exist <file>`** now resolves the path against the current directory and runs inside a
  guarded SD session, like every other filesystem command. It previously called `stat()` on the
  raw argument, so `if exist notes.txt` reported "not found" for any relative path.
- **`if "a"=="b"`** accepts the joined (`a==b`), spaced (`a == b`), and half-spaced (`a ==b`)
  spellings. The comparison previously required the `==` to be glued to the left operand, which
  broke once the tokenizer began removing quotes.

### Configuration

New `P4_CONFIG_*` macros, all documented in `p4minishell_config.yaml`:
- `P4_CONFIG_ESCAPE_CHAR` (`^`) — the escape character
- `P4_CONFIG_CHAIN_SEGMENT_MAX` (8) — maximum chained commands
- `P4_CONFIG_ERRORLEVEL_UNKNOWN_COMMAND` (9009) — errorlevel for an unknown command
- `P4_CONFIG_BATCH_LABEL_BYTES` (48) — maximum `:label` name length

### Testing

- Added `test/main/test_shell_quoting.c` with four suites: unquoted-operator scanning across
  all three quoting forms, markup removal, the quoting-aware tokenizer, and chain splitting
  including the pipe-versus-chain distinction and truncation reporting
- Clean build: 0 errors, 0 warnings for both firmware and test projects on ESP-IDF v5.5.5 /
  esp32p4

---

## [0.18.0] - 2026-08-08

Completes every roadmap item that was marked ⚠️ (partially implemented). **No commands,
options, output formats, or behaviors were removed.** Every previously working invocation
still works; the changes are additive or replace a stub with the real implementation.

### Added — Interactive keypress wait

The shell core gained a real keypress facility, which is what `pause`, `choice`, and `more`
needed to stop faking a key wait with a timed delay.

- **`components/shell/` keypress queue** with `shell_key_wait_begin()`,
  `shell_wait_for_key()`, `shell_key_wait_end()`, `shell_key_wait_submit()`,
  `shell_key_wait_is_active()`, and `shell_key_input_available()`
- **All three input sources feed it**: the UART console reader forwards the first character of
  a line, the USB HID bridge forwards the decoded key synchronously, and the LVGL on-screen
  keyboard forwards through `main.c`'s input-line callback
- **Input routing is suppressed during a wait**, so a key answering a prompt is never
  dispatched as a shell command and never lingers at the prompt
- **Bounded by `P4_CONFIG_KEY_WAIT_TIMEOUT_MS`** (30 s). When no interactive key source is
  attached, commands fall back to the previous timed behavior, so a headless board never
  stalls a batch file.

### Added — Input redirection and multi-stage pipes

- **`<` input redirection operator** — `sort < notes.txt` reads the file as command input
- **Multi-stage pipelines** — `cmd1 | cmd2 | cmd3`, up to `P4_CONFIG_PIPE_STAGE_MAX` (4)
- **Quote-aware pipe splitting** — `echo "a | b"` is no longer mistaken for a pipeline
- **Shared mechanism** — the `<` operator and each pipe stage both publish through the
  storage input-redirection slot, so `sort < f.txt` and `type f.txt | sort` reach the same
  code path in the text-processing commands
- **Guaranteed spool cleanup** — every stage's temp file is removed on every exit path,
  including a stage failure or an `exit` mid-pipeline

### Changed — Batch control flow is now complete

- **`pause`** blocks on a real keypress instead of a fixed 2-second delay
- **`choice`** blocks on a real keypress and gained the DOS switch set: `/C:list` (allowed
  keys), `/N` (hide the list), `/T:c,secs` (timed default), `/S` (case-sensitive). Unmatched
  keys are ignored like DOS, and errorlevel is set to the 1-based index of the chosen key.
- **`setlocal` / `endlocal`** perform real environment scoping. `setlocal` pushes a snapshot of
  the variable table onto a stack (`P4_CONFIG_SETLOCAL_DEPTH_MAX` = 8) and `endlocal` restores
  it, which correctly reverts creations, modifications, and deletions in one step. A scope left
  open when a batch frame returns is unwound automatically, so a child file cannot leak
  variables into its caller and no snapshot allocation is ever leaked.
- **`exit /b [code]`** leaves only the current batch file; a bare `exit [code]` unwinds every
  nested level. Added `batch_stop_mode_t` so the executor and `for` loops honor both. Previously
  `exit` abused the pending-goto flag, which meant a `goto` on the same line could resurrect
  execution.

### Changed — Runtime prompt engine

- **`prompt` is a full DOS template engine** supporting `$p` (path), `$g` (`>`), `$l` (`<`),
  `$b` (`|`), `$n` (drive), `$d` (date), `$t` (time), `$v` (version), `$s` (space),
  `$_` (newline), `$q` (`=`), `$$` (`$`), `$a` (`&`), `$c` (`(`), `$f` (`)`), `$e` (ESC), and
  `$h` (destructive backspace). Metacharacters are case-insensitive; an unknown one renders
  literally, both matching COMMAND.COM.
- **The template drives both surfaces** — the UART console prompt and the LVGL input line —
  so they can never disagree. Previously the LVGL prompt was fixed and `prompt` only printed
  a message.
- **The input line snapshots the prefix it painted.** Extraction and repair compare against
  that snapshot rather than re-rendering, so a template or path change landing between two
  LVGL events cannot make the shell mis-parse what the user typed.
- **`prompt /?`** lists the metacharacters; `prompt` with no argument shows the stored template
  and its rendered form.

### Changed — `date` and `time` can set the clock

- **`date [MM-DD-YYYY]`** and **`time [HH:MM[:SS]]`** now set the system clock in addition to
  reporting it. Both validate ranges, accept `-` or `/` separators for the date, make seconds
  optional for the time, and report honestly when the clock is not NTP-synchronized. A later
  SNTP sync still wins.

### Changed — File utility commands are now complete

- **`tree` is fully recursive.** Draws the DOS box-drawing outline with correct `+---` and
  `\---` connectors, and gained `/F` (include files; DOS default is directories only) and `/A`
  (plain ASCII connectors). Bounded by `P4_CONFIG_TREE_DEPTH_MAX` (8) and the existing
  128-entry listing cap. Each directory level is buffered on the heap rather than the
  worker-task stack, and prints the DOS `Folder PATH listing` header and summary counts.
- **`sort` replaced the bubble sort with `qsort()`**, raised the capacity from 128 to 1024
  lines, and gained `/R` (reverse), `/I` (case-insensitive), and `/U` (unique). The
  **memory leak is fixed**: there is now a single release path that frees every successful
  `strdup()` even when a mid-read allocation fails or the line cap is hit, and the line table
  itself is freed on every early return.
- **`find` gained `/I`** (case-insensitive), **`/N`** (line numbers), **`/C`** (count only),
  and **`/V`** (invert match), and prints the DOS `---------- <file>` banner.
- **`more` waits for a keypress** between pages: Enter or Space advances, `Q` quits.
- **`fc` reports differing lines in DOS style** with both file contents shown, and now detects
  trailing length differences instead of stopping at the shorter file.
- **All five now resolve paths and use guarded SD sessions.** They previously called `fopen()`
  on the raw argument, so a relative path failed and a missing card produced a bare errno
  message instead of the standard "SD card not present" text.

### Fixed

- **Redirection parser rewritten as a two-pass scan.** The old parser stopped at the first `>`,
  so it could not handle `<` at all and mis-parsed `sort < in.txt > out.txt`. The new parser
  locates every unquoted operator before overwriting any of them, so each target is naturally
  terminated by the next operator with no byte needing to be restored.
- **`for` loops now stop on `exit`.** A loop body that ran `exit` previously kept iterating
  because only the goto flag was checked.

### Configuration

New `P4_CONFIG_*` macros, all documented in `p4minishell_config.yaml`:
- `P4_CONFIG_PIPE_STAGE_MAX` (4) — maximum stages in one pipeline
- `P4_CONFIG_SETLOCAL_DEPTH_MAX` (8) — setlocal nesting limit
- `P4_CONFIG_TREE_DEPTH_MAX` (8) — tree recursion limit
- `P4_CONFIG_PROMPT_TEMPLATE_BYTES` (64) — prompt template buffer
- `P4_CONFIG_PROMPT_DEFAULT_TEMPLATE` (`"PS $p$g "`) — default prompt
- `P4_CONFIG_KEY_QUEUE_DEPTH` (16) — keypress queue depth
- `P4_CONFIG_KEY_WAIT_TIMEOUT_MS` (30000) — single keypress wait timeout
- `P4_CONFIG_SD_DRIVE_LETTER` (`"A:"`) — DOS drive letter for `prompt $n` and `tree`

Changed values:
- `P4_CONFIG_SORT_LINE_MAX` raised from 128 to 1024
- `P4_CONFIG_MORE_PAGE_DELAY_MS` and `P4_CONFIG_PAUSE_DELAY_MS` are now documented as
  fallbacks used only when no interactive key source is attached

### Testing

- Added `test/main/test_shell_prompt.c` with four suites covering template storage, every
  prompt metacharacter, `$p` path expansion, and the keypress-wait state machine
- `test_main.c` now calls `shell_init()` before running the suites that need shell-core state
- Clean build: 0 errors, 0 warnings for both firmware and test projects on ESP-IDF v5.5.5 /
  esp32p4

---

## [0.17.0] - 2026-08-08

### Changed — Phase B: Extract Modules

Roadmap Phase B is complete. The batch engine and the SD/storage layer are now separate
components, and the DOS file commands live with the storage layer instead of inside the
dispatcher. **No commands, options, output formats, error messages, or bounds were changed
or removed.** The dispatcher verb table is identical to v0.16.0.

- **Created `components/storage/`** — owns everything between the shell commands and the SD card:
  - `storage.c` / `storage.h`: guarded SD sessions (`shell_sd_begin()` / `shell_sd_end()`),
    persistent mount tracking, `storage_sd_is_mounted()`, the `sd eject` implementation,
    path resolution (`shell_sd_resolve_path()`, `shell_fs_resolve_path()`,
    `shell_resolve_target_from_source()`), FATFS conversion (`shell_sd_vfs_to_fatfs_path()`,
    `shell_sd_fresult_to_esp_err()`), size formatting (`shell_sd_format_size()`),
    `shell_sd_entry_type()`, DOS wildcard matching (`shell_wildcard_match()`), the RAM-only
    current working directory (`shell_get_cwd()`, `storage_set_cwd()`, `shell_fs_print_cwd()`),
    the shared file helpers (`shell_fs_copy_file()`, `shell_list_directory_path()`,
    `shell_print_file_text()`), and the output-redirection writer
    (`shell_write_redirect_output()`)
  - `storage_commands.c` / `storage_commands.h`: every DOS file command — `cd`/`chdir`, `dir`,
    `copy`, `move`, `del`/`erase`, `ren`/`rename`, `md`/`mkdir`, `rd`/`rmdir`, `type`, `write`,
    `append`, `touch`, `attrib`, `label`, `xcopy`, `find`, `more`, `tree`, `fc`, `sort`, and the
    `sd info|ls|stat|cat|eject` family
- **Created `components/batch/`** — owns the whole `.bat` interpreter:
  - Batch file execution with nesting (`shell_execute_batch_file()`), batch path resolution
    (`shell_resolve_batch_path()`), `:label` scanning, `goto`, `call :label`,
    `for %%var in (set) do command` loops, and the `|` pipe operator (`shell_execute_pipe()`)
  - The RAM-only environment variable table (24 slots), PATH, and variable expansion
    (`%VAR%`, `%0`, `%1`..`%9`, `%*`) via `shell_env_get()`, `shell_env_set()`, and
    `shell_expand_variables()`
  - Errorlevel tracking (`batch_get_errorlevel()` / `batch_set_errorlevel()`)
  - The batch language commands: `set`, `path`, `echo`, `call`, `if`, `goto`, `shift`, `pause`,
    `choice`, `setlocal`, `endlocal`, `exit`
- **`components/command/command.c` reduced from 4,384 lines to 1,140 lines** (74% smaller).
  It now owns only the dispatcher (`shell_execute_command_core()`), the execution pipeline
  (`shell_execute_command()`), the worker task (`shell_execute_command_async()`), redirection
  parsing, the hardware commands (`brightness`, `rotate`, `battery`, `volume`, `gpio`, `rgb`,
  `camera`), the UI query commands (`display`, `keyboard`, `windows`), the system commands
  (`reboot`, `clear`/`cls`, `prompt`, `date`, `time`), and the family routing for
  `wifi`/`bluetooth`/`usb`/`c6ota`/`sd`.

### Changed — Architecture

- **Added `batch_command_ops_t`** — a registration table (mirroring `shell_command_ops_t`) that
  lets the batch engine re-enter the full command pipeline for nested contexts (`if` bodies,
  `for` bodies, pipe stages, batch lines) without depending on `command.h`. Registered by
  `command_init()`. The hook is NULL-checked, so nested execution degrades gracefully with an
  explicit message rather than dereferencing a null pointer.
- **Dependency direction extended and still strictly one-way**:
  `main` → `command` → `batch` → `storage` → `shell` → (`ansi`, `display`, `windows`, `header`,
  `keyboard`, `clock`). No component declares `main` as a requirement, and no module depends
  upward at include time.
- **`command_init()` now sequences module startup**: `storage_init()` (current working
  directory, mount tracking) and `batch_init()` (environment table, PATH default, errorlevel)
  run before either operations table is registered, so no dispatch can observe uninitialized
  state.
- **`shell_get_cwd()` moved from `command.c` to `storage.c`** and `command_sd_is_mounted()` was
  replaced by `storage_sd_is_mounted()`. `shell_command_ops_t` is unchanged; `command_init()`
  simply points the `get_cwd` and `sd_is_mounted` hooks at the storage implementations, so
  `shell.c` sees no difference.

### Build

- Root `CMakeLists.txt` gained `components/batch` and `components/storage` in
  `EXTRA_COMPONENT_DIRS`
- `components/command/CMakeLists.txt` now requires `batch` and `storage`
- `test/CMakeLists.txt` gained the `storage` and `batch` component directories, per the
  unit-test rule that a new `shell`/`command` dependency must be added there
- Clean build: 0 errors, 0 warnings for both firmware and test projects on ESP-IDF v5.5.5 /
  esp32p4

### Documentation

- Updated `readme.md`, `documentation.md`, `ai-context.md`, `command.md`, `API.md`, `SDK.md`,
  `roadmap.md`, `p4minishell_config.yaml`, and `board_config.yaml` for the new module boundaries
- `roadmap.md` Phase B is marked complete

---

## [0.16.0] - 2026-08-07

### Added
- **Batch `for` loops** — Full `for %%var in (set) do command` implementation with variable expansion
- **Batch `%0` and `%*` expansion** — `%0` expands to script name, `%*` expands to all arguments
- **`:label` parsing** — Labels scanned at batch file load, stored in label table for `goto`/`call :label`
- **`call :label`** — Jump to label within same batch file, with label table lookup
- **`for` loop variable expansion** — `%%var` expanded per iteration in `do` command

### Changed — Phase A: Command Implementation Consolidation

Roadmap Phase A is complete. Every command implementation now lives in
`components/command/command.c`, and `main.c` is reduced to boot orchestration
and LVGL event routing. No commands, options, or output formats were removed.

- **Moved all command implementations from `main.c` to `components/command/command.c`**:
  - Hardware: `brightness`, `rotate`, `battery`, `volume`, `gpio`, `rgb`, `camera`
  - System: `reboot`, `clear`/`cls`
  - File/SD: `cd`, `dir`, `copy`, `move`, `del`, `ren`, `mkdir`, `rmdir`, `type`, `write`, `append`, `touch`
  - SD family: `sd info|ls|stat|cat|eject`, `sdeject`
  - Extended DOS: `attrib`, `label`, `xcopy`, `find`, `more`, `tree`, `fc`, `sort`
  - Environment/batch: `set`, `path`, `echo`, `call`, `if`, `goto`, `shift`, `pause`,
    `choice`, `setlocal`, `endlocal`, `prompt`, `date`, `time`, `exit`
  - Batch engine: batch file execution, label scanning, `for` loops, pipes, wildcard matching
  - SD session management, path resolution, environment variables, PATH, and current working directory
- **Removed duplicate hardware command implementations from `main.c`** — the placeholder
  `cmd_battery()`/`cmd_volume()` stubs in `command.c` were replaced by the real ADC and
  ES8311 codec implementations, so `battery` and `volume` no longer depend on a bridge
- **Removed duplicate `shell_execute_command_core()` and `shell_execute_command()` from `main.c`** —
  `command.c` owns the whole pipeline: variable expansion, redirection, dispatch, and the worker task
- **Removed duplicate transcript functions from `main.c`** — `shell_transcript_append_text()`,
  `shell_transcript_appendf()`, `shell_transcript_render()`, `shell_transcript_reset()`,
  `shell_schedule_transcript_appendf()`, the async staging buffer, and the UART console now exist
  only in `components/shell/shell.c`. The shell copies retain `main.c`'s richer behavior:
  half-buffer truncation with a `[history truncated]` marker, oldest-first async drop policy,
  and UART mirroring.
- **Removed duplicate debug functions from `main.c`** — `shell_debug_log_push()`,
  `shell_record_errorf()`, `shell_record_warningf()`, `shell_record_infof()`, and
  `shell_command_debug()` now exist only in `components/shell/shell.c`, keeping `main.c`'s
  transcript surfacing of errors and the Wi-Fi state/heap lines in `debug` output
- **Removed all bridge trampolines** — the 18 `shell_bridge_*` functions and
  `shell_execute_command_core_bridge()` are gone; `p4minishell.h` now declares only the
  c6ota, usb, and networking host callbacks that ESP-IDF requires in the app component
- **Moved input line ownership to `components/shell/`** — added `shell_input_line_set_text()`,
  `shell_input_line_reset()`, `shell_extract_input_text()`, and `shell_input_line_repair_prompt()`
  so the prompt-prefix contract lives in one place
- **Moved `Kconfig.projbuild` from `main/` to `components/networking/`** — that component is the
  only consumer of `CONFIG_P4MINISHELL_WIFI_DEFAULT_SSID`/`_PASSWORD`, and the move lets the
  unit-test project resolve the symbols too

### Changed — Architecture

- **Removed the `command` → `main` component dependency**, which was a layering inversion.
  Dependencies now flow one way: `command` → `shell` → (`ansi`, `display`, `windows`, `header`,
  `keyboard`, `clock`).
- **Added `shell_command_ops_t`** — a registration table (mirroring `networking_host_ops_t`) that
  lets `shell.c` reach command-owned services (dispatch, current working directory, volume,
  SD mount state, battery telemetry) without including `command.h`. Registered by `command_init()`.
- **`main.c` reduced from 5,990 to 397 lines** and now contains only `app_main()`, LVGL event
  callbacks, UI construction, and the c6ota/usb host bridges.
- **Eliminated all 80+ forward declarations** from `main.c`.

### Removed — Dead Code

- Legacy shell-local Wi-Fi runtime in `main.c` (`shell_wifi_runtime_init`, event handlers,
  `shell_wifi_connect_with_credentials`, `shell_wifi_run_diagnostic`, and helpers). All were
  marked `__attribute__((unused))`; `components/networking/` owns this path.
- Legacy hosted Bluedroid Bluetooth path in `main.c`, compiled out behind
  `P4_CONFIG_BT_HOSTED_RUNTIME_SUPPORTED == 0`. `components/networking/bluetooth.c` owns
  hosted NimBLE.
- `reboot_task()` and all unused `static` command duplicates that the compiler had been
  reporting as `-Wunused-function`.
- Unused `var_str` variable in the `for` loop parser.
- Ten leftover `fix_main*.py` / `add_hw_cmds.py` / `clean_corruption.py` migration scripts.

### Fixed

- **Command history buffer overlap** — the full-buffer shift used `snprintf()` with overlapping
  source and destination slots (undefined behavior, caught as `-Werror=restrict`). Now uses `memmove()`.
- **Duplicate ANSI output on the serial console** — `shell_transcript_append_ansi()` wrote the
  plain text and then the raw ANSI text to UART, printing every colored line twice. The plain
  append no longer mirrors to UART.
- **Async transcript flush held a critical section across LVGL calls** — the staging buffer is now
  drained into a local copy before the append, and a re-queue check catches text staged during the flush.
- **`shell_transcript_append_text()` silently dropped output when full** — it now truncates the
  oldest half and inserts a `[history truncated]` marker, matching the previous `main.c` behavior.

### Added — Configuration

All newly extracted literals are `P4_CONFIG_*` macros in `p4minishell_config.h`, documented in
`p4minishell_config.yaml`:
- `P4_CONFIG_HEADER_NOTIFY_TIMEOUT_MS`, `P4_CONFIG_HEADER_RSSI_UNKNOWN`
- `P4_CONFIG_BATCH_LABEL_MAX`, `P4_CONFIG_COMMAND_ARGV_MAX`, `P4_CONFIG_SORT_LINE_MAX`
- `P4_CONFIG_MORE_PAGE_LINES`, `P4_CONFIG_MORE_PAGE_DELAY_MS`, `P4_CONFIG_PAUSE_DELAY_MS`
- `P4_CONFIG_PIPE_SETTLE_DELAY_MS`, `P4_CONFIG_REBOOT_DELAY_MS`
- `P4_CONFIG_TEXT_LINE_BYTES`, `P4_CONFIG_LFN_BYTES`, `P4_CONFIG_VOLUME_DEFAULT_PCT`

### Fixed — Unit Test Project

The `test/` project did not configure. It now builds standalone:
- Pinned `IDF_TARGET` to `esp32p4` (previously defaulted to `esp32` and failed on the toolchain)
- Added `test/main/idf_component.yml` pinning `esp_hosted` 2.12.1 and `esp_wifi_remote` 1.4.1
  (the manager was resolving `esp_hosted` 3.x, whose Kconfig is incompatible)
- Added `test/sdkconfig.defaults` mirroring the firmware's build-affecting options, including
  the FATFS LFN settings that `FILINFO.altname` requires
- Staged `board_config.h` and `p4minishell_config.h` into the generated config directory
- Added the missing `components/p4_usb`, `espressif__usb_host_hid`, `espressif__usb_host_msc`,
  and `espressif__esp_lcd_touch` component directories
- Added tests for `shell_format_command_for_transcript()` covering password masking,
  pass-through, and NULL input

### Updated

- **Version bump**: 0.15.1 → 0.16.0 (`p4minishell_config.h` version macros were stale at 0.14.2
  and are now correct)
- **`.gitignore`**: ignore `test/managed_components/`

### Verified
- **Clean build**: Zero errors, zero warnings for both the firmware and the unit-test project
  on ESP-IDF v5.5.5 / esp32p4
- **No regressions**: All 65 command dispatch verbs preserved and reachable; every command
  implementation, option, and output string carried across unchanged
- **Batch features**: `for` loops, `%0`/`%*`, `:label`, `goto`, `call :label` all still functional
- **Hardware commands**: `brightness`, `rotate`, `battery`, `volume`, `gpio` all still functional

---

## [0.15.1] - 2026-08-07

### Changed
- **Merged duplicate command dispatch logic** — Consolidated command execution pipeline from `main.c` and `components/command/command.c` into a single path:
  - `main.c` now handles variable expansion (`%VAR%`, `%1`..`%9`) and output redirection (`>` / `>>`) before dispatch
  - `components/command/command.c` owns the unified command dispatcher (`shell_execute_command_core`) and worker task
  - Removed duplicate `shell_execute_command`, `shell_execute_command_core`, and `shell_command_task` from `main.c`
  - Added bridge function `shell_execute_command_core_bridge` for main.c to invoke command.c's dispatcher
- **Updated all documentation** — All `.md` files, configs, and references updated to reflect ESP-IDF v5.5.5 and merged dispatch architecture
- **Fixed deprecation warning** — Updated `esp_lvgl_port` DSI callback from deprecated `on_refresh_done` to `on_frame_buf_complete` for ESP-IDF 5.5.0+

### Updated
- **ESP-IDF baseline**: v5.5.3 → v5.5.5 across all configs, lock files, and documentation
- **Version bump**: 0.15.0 → 0.15.1

### Verified
- **Clean build**: Zero errors, zero warnings on ESP-IDF v5.5.5 / esp32p4
- **Flash**: Successful
- **Boot**: Device boots, Wi-Fi connects, all commands functional

---

## [0.15.0] - 2026-04-30

### Added
- **Consistent ANSI color-coding across all components** — All text output (header, shell, commands, Wi-Fi, USB, OTA) now uses a unified color scheme:
  - `@C` (bright cyan) = subsystem labels and property keys (e.g., `wifi.state:`, `board.name:`)
  - `@G` (bright green) = success/connected/active status
  - `@r` (red) = errors and failures
  - `@y` (yellow) = warnings and cautions
  - `@Y` (bright yellow) = section headings
  - `@W` (bright white) = important values (SSIDs, IPs, paths)
  - `@Z` (bright magenta) = numeric values
  - `@k` (bright black/gray) = muted/secondary text
  - `@N` (bright cyan) = progress/info messages
  - `@R` = reset to default at end of every colored segment
- **ANSI callback in networking_host_ops_t** — New `schedule_transcript_appendf_ansi` callback for colored output from components
- **`networking_schedulef_ansi()` helper** — ANSI-capable formatted output for the networking module

### Changed
- **networking.c**: All wifi_status, wifi_scan, wifi_diag, wifi_disconnect, wifi help, event handler, watchdog, and sysinfo output now uses ANSI color tokens
- **networking.c**: `networking_appendf()` now routes through ANSI path for color support
- **main.c**: Wired `shell_transcript_appendf_ansi` into networking host ops
- **p4minishell_config.h**: Added new semantic color token definitions (subsystem, key, muted, heading, IP, connected, disconnected, progress, prompt)

### Verified
- **Clean build**: Zero errors, zero warnings
- **Flash**: Successful to COM3
- **Boot**: Device boots, Wi-Fi connects with colored output

---

## [0.14.2] - 2026-04-30

### Added
- **attrib command**: Show and set FATFS file attributes (R=read-only, H=hidden, S=system, A=archive). Uses `f_stat()`/`f_chmod()`. Supports `attrib [path]`, `attrib +R file`, `attrib -H file`, etc.
- **label command**: Read and set FATFS volume label via `f_getlabel()`/`f_setlabel()`. Max 11 characters (FAT 8.3 convention).
- **xcopy command**: Recursive directory copy with `/S` flag for subdirectory traversal.
- **Wildcard matching**: `shell_wildcard_match()` for DOS-style `*` and `?` pattern matching.
- **Wildcard-aware dir**: `dir *.txt` filters directory listings by wildcard pattern with short/long filename display.
- **Wildcard-aware del**: `del *.bak` deletes all matching files in a directory.
- **Wildcard-aware copy**: `copy *.txt backup\` copies all matching files to a destination directory.

### Changed
- **main.c**: `shell_command_dir()`, `shell_command_del()`, `shell_command_copy()` now detect wildcard characters and use filtered directory iteration.
- **main.c**: `shell_wildcard_match()`, `shell_command_attrib()`, `shell_command_label()`, `shell_command_xcopy()` are non-static for `command.c` extern dispatch.
- **command.c**: Existing `extern` declarations for `attrib`, `label`, `xcopy` now resolve correctly against main.c implementations.

### Verified
- **Clean build**: Zero errors, zero warnings on ESP-IDF v5.5.5 / esp32p4 target
- **Flash**: Successful to COM3
- **c6ota**: Verified working end-to-end (SD source, image validation, OTA transfer to 100%, Wi-Fi restore)

---

## [0.14.1] - 2026-04-30

### Fixed
- **Backspace CLI bug**: Pressing Backspace when the command line is empty no longer inserts the literal text `P4Shell>`. The `LV_EVENT_VALUE_CHANGED` handler now correctly detects partial prompt deletion and restores only the user text portion, not duplicating the prompt prefix.
- **Touch keyboard button**: The on-screen keyboard's "keyboard" button (LV_SYMBOL_KEYBOARD) now correctly hides the keyboard when pressed. Added `LV_EVENT_CANCEL` handling in `shell_input_line_event_cb` that calls `keyboard_hide()`.
- **Wi-Fi mutex implementation**: The Wi-Fi mutex (`wifi_lock`/`wifi_unlock`) and atomic init-task claim (`wifi_try_claim_init_task`/`wifi_release_init_task`) that were declared in the changelog for v0.13.1 but never implemented are now fully functional. All shared Wi-Fi state access is mutex-protected.
- **Wi-Fi persistent watchdog**: The watchdog task (`networking_wifi_watchdog_task`) that was declared but never implemented is now fully functional. It monitors Wi-Fi connection state and retries with exponential backoff (1s → 2s → 4s → ... → 30s cap) for up to 120 seconds total, handling the C6's typical associate-then-disconnect boot behavior.
- **Null pointer safety**: Added NULL checks for `lv_textarea_get_text()` return values in the USB keyboard injection Home/End handlers in `shell_usb_keyboard_inject_cb`.
- **Async dispatch error handling**: Added `lv_async_call` return value check in `shell_usb_keyboard_input` to free the context on dispatch failure.

### Changed
- **networking.c**: All Wi-Fi state transitions in the event handler, `networking_wifi_connect_with_credentials`, `networking_wifi_begin_connect_request`, and `networking_wifi_begin_background_request` are now mutex-protected.
- **networking.c**: `wifi_try_claim_init_task`/`wifi_release_init_task` replace direct `s_wifi_init_task_in_progress` manipulation for TOCTOU-safe init-task claiming.
- **main.c**: `shell_input_line_event_cb` `LV_EVENT_VALUE_CHANGED` handler rewritten to correctly handle partial prompt deletion by detecting common prefix length and extracting only user text.
- **shell.c**: `shell_usb_keyboard_input` now checks `lv_async_call` return value and frees context on failure.

### Verified
- **Clean build**: Zero errors, zero warnings (3 pre-existing deprecated API warnings from ESP-IDF v5.5.3 VFS API)
- **Boot**: Clean, Wi-Fi watchdog operational with exponential backoff retry
- **Flash**: Successful to COM3
- **Runtime**: No crashes observed; watchdog correctly retries disconnected Wi-Fi

---

## [0.14.0] - 2026-04-30

### Added
- **ANSI/VT escape sequence module**: New `components/ansi/` module providing SGR (Select Graphic Rendition) escape sequence parsing and formatting
- **16-color ANSI palette**: PowerShell-inspired color palette with configurable standard and bright colors (black, red, green, yellow, blue, magenta, cyan, white)
- **SGR attribute support**: Bold, dim, italic, underline, blink, reverse, hidden, strikethrough
- **ANSI format string builder**: `ansi_format()` / `ansi_vformat()` with `@`-prefixed color/attribute specifiers (`@g` for green, `@r` for red, `@B` for bold, `@R` for reset, etc.)
- **ANSI-aware transcript functions**: `shell_transcript_append_ansi()` and `shell_transcript_appendf_ansi()` for colored transcript output
- **ANSI text processing**: `ansi_process_text()` state machine for segment-by-segment ANSI rendering
- **ANSI-to-plain stripping**: `ansi_strip_to_plain()` for LVGL transcript textarea (which doesn't support per-character styling)
- **UART console ANSI pass-through**: Raw ANSI codes passed to serial terminal for native rendering
- **Config macros**: `P4_CONFIG_ANSI_*` for all 16 colors + default FG/BG + buffer size

### Changed
- **All system info commands**: `help`, `sysinfo`, `version`, `about`, `mem`, `debug` now use ANSI-colored output with green headers, cyan labels, red errors, yellow warnings
- **All hardware commands**: `brightness`, `rotate`, `battery`, `volume`, `reboot` now use ANSI-colored output (green success, red errors, yellow usage)
- **Display/keyboard/windows commands**: All now use ANSI-colored output with consistent color scheme
- **Unknown command error**: Now shown in red
- **Boot banner**: Now displayed in bright green
- **Debug log**: Errors shown in red, warnings in yellow, info in default color
- **Shell prompt**: UART console prompt now passes through ANSI codes for colored terminal rendering
- **shell.h**: Added `shell_transcript_append_ansi()` and `shell_transcript_appendf_ansi()` public API
- **p4minishell_config.h**: Version bumped to 0.14.0; added ANSI color palette section (16 colors + defaults + buffer size)
- **p4minishell_config.yaml**: Documented all ANSI color values with descriptions and valid ranges
- **CMakeLists.txt**: Added `components/ansi` to EXTRA_COMPONENT_DIRS
- **shell.c**: Now calls `ansi_init()` during `shell_init()`; imports `ansi.h`

### Architecture
- New `components/ansi/` module with `ansi.h` (public API) and `ansi.c` (implementation)
- ANSI module is independent of LVGL; only depends on `p4minishell_config.h` and ESP-IDF
- Color palette initialized from config macros; runtime-modifiable via `ansi_set_palette_color()`
- `shell_transcript_append_ansi()` strips ANSI for LVGL textarea, passes raw ANSI to UART console
- All existing `shell_transcript_append_text()` / `shell_transcript_appendf()` calls continue to work unchanged
- Backward compatible: no existing API removed or broken

---

## [0.13.0] - 2026-04-30

### Added
- **USB keyboard auto-detect**: USB HID keyboard automatically detected when plugged in
- **USB keyboard CLI injection**: Keystrokes from USB keyboard routed to shell input line
- **Full USB HID key map**: Complete US keyboard layout including all symbols, keypad, navigation, function keys, and modifier-aware shifted characters
- **On-screen keyboard auto-hide**: LVGL keyboard automatically hidden when USB keyboard attached; restored when unplugged
- **External input mode**: `keyboard_set_external_input()` / `keyboard_is_external_input_enabled()` API for keyboard component
- **Force-visible override**: `keyboard_force_visible()` / `keyboard_clear_force_visible()` to keep on-screen keyboard visible even with USB keyboard
- **USB keyboard state queries**: `usb_is_keyboard_attached()`, `usb_is_mouse_attached()` public API
- **USB keyboard input callback**: `usb_register_keyboard_input_callback()` for shell CLI integration
- **Public key mapping API**: `usb_key_to_ascii_full()`, `usb_key_name_full()` for external consumers
- **Config macros**: `P4_CONFIG_USB_KEYBOARD_AUTO_DETECT`, `P4_CONFIG_USB_KEYBOARD_CLI_INJECT`, `P4_CONFIG_USB_KEYBOARD_NOTIFY_MS`
- **Header notification**: "USB keyboard detected" / "USB keyboard removed" on plug/unplug events

### Changed
- **usb.c**: Extended key map from 11 to 60+ USB HID key codes; keyboard input handler routes to both echo and CLI callback
- **keyboard.c**: Added external input mode with auto-hide/restore logic
- **shell.c**: Added `shell_usb_keyboard_input()` bridge function with LVGL async dispatch for safe input line injection
- **main.c**: Registers USB keyboard callback after `usb_init()`; header refresh timer monitors USB keyboard attach state
- **usb.h**: Expanded public API surface with new types, callbacks, and query functions
- **p4minishell_config.h**: Version bumped to 0.13.0; added USB keyboard config macros
- **p4minishell_config.yaml**: Documented new USB keyboard config values

### Verified
- **Clean build**: Zero errors, zero warnings on ESP-IDF v5.5.5 / esp32p4 target
- **Boot**: Clean, Wi-Fi connects, display renders, no regressions

---

## [0.13.1] - 2026-04-30

### Fixed
- **SD card header status**: Fixed "SD NO" showing in header even when SD card is mounted. Replaced `stat()`-based mount detection with persistent mount tracking via `s_sd_persistent_mounted` flag set/cleared by `shell_sd_begin()`/`shell_sd_end()`
- **Wi-Fi random boot failure**: Added mutex (`s_wifi_mutex`) protecting all shared Wi-Fi state (s_wifi_state, s_wifi_connected, s_wifi_target_ssid, etc.) from race conditions
- **TOCTOU race in Wi-Fi init**: Replaced unprotected `s_wifi_init_task_in_progress` check with atomic `wifi_try_claim_init_task()` / `wifi_release_init_task()` to prevent double-init
- **Wi-Fi disconnect during boot**: Added `networking_wifi_auto_reconnect_task()` that retries `esp_wifi_connect()` after 1 second delay on `WIFI_EVENT_STA_DISCONNECTED`
- **NVS init reliability**: Added retry loop (2 attempts) with erase-and-retry for `nvs_flash_init()` to handle transient NFS errors
- **Wi-Fi connect flow**: Removed unnecessary `esp_wifi_disconnect()` before `esp_wifi_connect()` to avoid race with event handler

### Added
- **Wi-Fi mutex**: `wifi_lock()` / `wifi_unlock()` helpers protecting all state transitions in event handler, init, connect, and diagnostics
- **Wi-Fi auto-reconnect**: Dedicated task spawned on disconnect to retry connection after 1s delay
- **SD persistent mount flag**: `s_sd_persistent_mounted` tracks actual card state across mount/unmount cycles for accurate header display
- **NVS retry logic**: `networking_wifi_runtime_init()` now retries NVS init twice with erase recovery

### Changed
- **networking.c**: Added `s_wifi_mutex`, `wifi_lock()`, `wifi_unlock()`, `wifi_try_claim_init_task()`, `wifi_release_init_task()`, `networking_wifi_auto_reconnect_task()` — all state transitions now mutex-protected
- **main.c**: Added `s_sd_persistent_mounted` flag; `shell_sd_header_is_mounted()` now uses persistent flag instead of `stat()`; `shell_sd_begin()`/`shell_sd_end()` update the flag

### Verified
- **Clean build**: Zero errors, zero warnings on ESP-IDF v5.5.5 / esp32p4 target
- **Boot**: Clean, Wi-Fi connects reliably, SD card accessible, header shows correct SD state
- **Flash**: Successful to COM3

---

## [0.12.0] - 2026-04-30

### Fixed
- **Screen flash**: Removed `header_force_render()` from periodic header refresh timer

### Added
- **Clock component**: New `components/clock/` with SNTP time sync from pool.ntp.org
- **Timezone support**: POSIX TZ string support
- **Time in ver/sysinfo/about**: Formatted local time with NTP sync status

### Verified
- **Clean build**: Zero errors, zero warnings on ESP-IDF v5.5.5 / esp32p4 target
- **Flash**: Successful to COM3

---

## [0.11.0] - 2026-04-30

### Added
- **Shell component**: New `components/shell/` owning transcript, history, debug log, UART console, system info commands
- **Command component**: New `components/command/` owning command dispatch, execution task, all built-in commands
- **Windows info command**: `windows info` shows display dimensions and region rectangles

### Changed
- **main.c**: Retains only app_main(), shell_build_ui(), LVGL callbacks, Wi-Fi/Bluetooth/SD/FS/batch state
- All transcript/history/debug/UART/command code moved to shell.c and command.c

### Verified
- **Clean build**: Zero errors, zero warnings on ESP-IDF v5.5.5 / esp32p4 target
- **Boot**: Clean, Wi-Fi connects, all commands preserved

---

## [0.10.0] - 2026-04-30

### Added
- **Keyboard component**: New `components/keyboard/` module owning the LVGL keyboard widget
- **keyboard.h / keyboard.c**: Keyboard manager with visibility control, mode switching, textarea binding
- **Keyboard hide/show**: `keyboard_hide()` / `keyboard_show()` with automatic UI reflow
- **Shell command**: `keyboard show|hide|toggle|status`
- **Config macros**: `P4_CONFIG_KEYBOARD_*` for height, visibility defaults

### Changed
- **windows.c**: Keyboard delegated to keyboard component; `windows_get_rect()` accounts for keyboard visibility
- **main.c**: Added `keyboard` command family dispatch

### Verified
- **Clean build**: Zero errors, zero warnings on ESP-IDF v5.5.5 / esp32p4 target
- **Boot**: Clean, Wi-Fi connects, no regressions

---

## [0.9.0] - 2026-04-30

### Added
- **Window manager module**: New `components/windows/` that owns the LVGL screen layout and dynamic scaling
- **windows.h / windows.c**: Central window/layout manager dividing the screen into named regions (HEADER, TRANSCRIPT, INPUT_ROW, KEYBOARD)
- **Resolution-aware scaling**: All window regions dynamically scale based on current display resolution from display.c
- **Rotation-aware layout**: Window manager recalculates all dimensions on rotation change
- **Consistent styling API**: `windows_get_color()` with semantic color names
- **Window object accessors**: `windows_get_transcript()`, `windows_get_input_line()`, `windows_get_keyboard()`, etc.
- **Dimension query API**: `windows_get_rect()`, `windows_get_display_width()`, `windows_get_display_height()`
- **Scaling helpers**: `windows_scale_height_percent()`, `windows_scale_width_percent()` with min/max clamping
- **display.h expanded**: Added `display_get_width()` and `display_get_height()` convenience functions
- **Config macros**: `P4_CONFIG_WINDOW_*` for all window region scaling parameters

### Changed
- **main.c shell_build_ui()**: Refactored from ~80 lines of manual LVGL widget creation to ~40 lines using window manager API
- **main.c LVGL widgets**: Removed 5 static LVGL object variables — now owned by windows.c
- **All hardcoded scaling values**: Moved to `P4_CONFIG_WINDOW_*` config macros

### Verified
- **Clean build**: Zero errors, zero warnings on ESP-IDF v5.5.5 / esp32p4 target
- **Boot**: Clean boot, shell UI renders correctly, Wi-Fi connects
- **No regressions**: All shell commands preserved

---

## [0.8.0] - 2026-04-30

### Added
- **Display manager module**: New `components/display/` module that owns all display hardware state and operations
- **display.h / display.c**: Central display controller with public API for rotation, resolution, refresh rate, brightness, and power management
- **Rotation API**: `display_set_rotation()`, `display_get_rotation()`, `display_rotation_parse()`, `display_rotation_to_string()` — clean rotation control with automatic touch remapping
- **Resolution API**: `display_get_resolution()`, `display_get_native_resolution()` — query current and native panel resolution accounting for rotation
- **Refresh rate API**: `display_get_refresh_config()`, `display_set_refresh_rate()` — query and configure display refresh rate (dynamic rate change noted as not supported on current JD9165 panel)
- **Brightness API**: `display_get_brightness()`, `display_set_brightness()` — backlight control through display manager
- **Power management API**: `display_set_power_state()`, `display_sleep()`, `display_wake()` — display power state transitions (on/sleep/off)
- **Display info API**: `display_get_info()`, `display_print_info()` — comprehensive display diagnostics for sysinfo
- **UI rebuild callback**: `display_register_ui_rebuild_callback()` — allows the shell to register a callback for rotation-triggered UI rebuilds
- **Thread-safe state tracking**: All display state protected by critical sections; LVGL operations dispatched via `lv_async_call`
- **Touch handle management**: Lazy touch handle acquisition cached internally; touch rotation remapping handled automatically on rotation change

### Changed
- **main.c refactored**: All display-related code extracted to `components/display/`
- **Removed from main.c**: `s_display`, `s_touch_handle`, `s_backlight_percent`, `s_display_rotation` static variables
- **Removed from main.c**: `shell_get_touch_handle_from_bsp()`, `shell_update_touch_rotation()`, `shell_rotation_apply()`, `shell_async_rebuild_ui()` — now handled by display manager
- **shell_command_brightness()**: Now calls `display_set_brightness()` instead of `bsp_display_brightness_set()`
- **shell_command_rotate()**: Now calls `display_rotation_parse()` + `display_set_rotation()` instead of inline LVGL rotation logic
- **app_main()**: Display init now uses `display_init()` which wraps `bsp_display_start_with_config()`; UI rebuild callback registered via `display_register_ui_rebuild_callback()`
- **sysinfo display output**: Now uses `display_get_info()` for comprehensive, structured display diagnostics
- **CMakeLists.txt**: Added `components/display` to `EXTRA_COMPONENT_DIRS`; main component now requires `display`
- **shell_lvgl_touch_ctx_t**: Removed from main.c (now internal to display.c)

### Architecture
- **Display state ownership**: The display manager OWNS all display state. The shell layer calls into the display manager for all display operations.
- **Component layout**: `components/display/` follows the same pattern as `components/header/`, `components/networking/`, etc.
- **Backward compatibility**: All existing shell commands (`brightness`, `rotate`) continue to work identically
- **No BSP bypass**: Display init still uses `bsp_display_start_with_config()` with `BOARD_CFG_*` values

### Verified
- **Clean build**: Zero errors, zero warnings on ESP-IDF v5.5.5 / esp32p4 target
- **No regressions**: Boot path, screen rendering, Wi-Fi, all commands preserved
- **Binary**: p4minishell.bin generated successfully

### Documentation
- Updated all project documentation files with display manager architecture
- Added display manager to module layout in documentation.md
- Added display API to API.md and SDK.md
- Updated ai-context.md with display manager rules
- Updated command.md with display manager notes
- Updated board_config.yaml display section
- Updated roadmap.md with display manager completion

---

## [0.7.1] - 2026-04-29

### Fixed
- **rotate command**: Now correctly rebuilds the entire UI after display rotation via `lv_display_set_rotation()`
- **Touch sync**: GT911 touch remapping now properly matches the rotated display orientation
- **UI scaling**: All UI elements (header, transcript, input row, keyboard) now dynamically rescale to the rotated resolution
- **header_deinit()**: New function to clean up header widgets before UI rebuild after rotation

### Changed
- **shell_rotation_apply()**: Now calls `shell_build_ui()` after rotation to recreate all widgets at the new resolution
- **shell_build_ui()**: Calls `header_deinit()` before `lv_obj_clean()` so header can re-initialize fresh
- **Keyboard height**: Now dynamically scaled to ~35% of vertical resolution (clamped 180-280px)
- **Input row height**: Now dynamically scaled to ~8% of vertical resolution (clamped 40-56px)
- **header_scale_height()**: Already rotation-aware (uses correct resolution axis for 90/270 degree rotations)

### Verified
- **Clean build**: Zero errors, zero warnings
- **Binary**: p4minishell.bin 1,494,768 bytes (82% free)
- **No regressions**: Boot, screen rendering, Wi-Fi, all commands preserved

---

## [0.7.0] - 2026-04-29

### Added
- **Header system panel redesigned**: MEM | CPU | BAT all on the far right, dynamically linked to FreeRTOS runtime statistics
- **CPU usage in header**: Real-time CPU bar + percentage from FreeRTOS idle task runtime counter deltas (CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS)
- **Battery always visible**: Shows "BAT N/C" with muted styling when ADC is not connected or unavailable
- **header_update_mem()**: New API for real-time heap statistics (free/total from heap_caps)
- **header_update_cpu()**: New API for real-time CPU usage and task count
- **header_update_uptime()**: New API for system uptime tracking
- **header_update_battery() signature change**: Now takes `bool adc_ready` second parameter

### Changed
- **sysinfo command**: Now includes FreeRTOS task count, uptime (days/hours/minutes/seconds), total heap with percentage
- **version command**: Expanded with chip info, uptime, heap stats, and active task count
- **mem command**: Added total heap, percentage free, and task count
- **about command**: Added header description, uptime, and task count
- **header.c**: System panel layout changed to MEM | CPU | BAT with CPU bar widget and separators
- **header.c**: Battery rendering split into adc_ready (live data) and !adc_ready (N/C muted) paths
- **main.c**: shell_header_status_refresh() now collects FreeRTOS runtime stats, calculates CPU usage from idle task deltas, and pushes all metrics to header
- **main.c**: Boot timestamp captured via esp_timer_get_time() for real-time uptime

### Verified
- **Clean build**: Zero errors, zero warnings
- **No regressions**: Boot, screen rendering, Wi-Fi, all 40+ commands preserved
- **FreeRTOS configs**: CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS, CONFIG_FREERTOS_USE_TRACE_FACILITY, CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS all enabled

### Documentation
- Updated API.md with new header_update_mem/cpu/uptime signatures
- Updated SDK.md with real-time FreeRTOS integration notes
- Updated p4minishell_config.h with CPU/MEM/BAT threshold constants
- Updated p4minishell_config.yaml to v0.3.0

---

## [0.6.0] - 2026-04-29

### Fixed
- **SD card status icon in header**: Fixed `header_update_sd()` to immediately update state and fall back to direct render when LVGL async dispatch fails
- **Header update robustness**: All `header_update_*()` functions now update internal state immediately (safe from any task context) and fall back to synchronous render on allocation/async failure
- **SD icon rendering**: SD indicator now uses consistent `HEADER_SD_SYMBOL` in both mounted and unmounted states instead of `LV_SYMBOL_WARNING` when unmounted

### Changed
- **header.c**: Refactored all public update functions (`header_update_wifi`, `header_update_battery`, `header_update_bluetooth`, `header_update_usb`, `header_update_sd`) to set state immediately before scheduling async render
- **header.c**: Async callbacks simplified to render-only (state already set by caller)
- **header.c**: Battery percent clamping moved from async callback to public API entry point

### Verified
- **Clean build**: Zero errors, zero warnings
- **Binary**: p4minishell.bin 1,490,688 bytes (82% free)
- **No regressions**: Boot, screen rendering, Wi-Fi, all commands preserved

---

## [0.5.0] - 2026-04-29

### Fixed
- **Stale AI comments**: Replaced all 25+ `// AI:` prefixed comments across main.c, usb.c, c6ota.c, and managed BSP with proper descriptive comments
- **Comment consistency**: All section markers now use descriptive text instead of AI-prefixed tags

### Verified
- **Clean build**: Zero errors, zero warnings on ESP-IDF v5.5.5 / esp32p4 target
- **Binary integrity**: p4minishell.bin 1,490,608 bytes (82% free in 8MB partition)
- **No regressions**: Boot, screen rendering, Wi-Fi, all 40+ commands preserved

### Documentation
- Updated ai-context.md with audit rules and verification checklist
- Updated changelog.md with v0.5.0 hardening entry

---

## [0.4.0] - 2026-04-29

### Added
- **p4minishell.h**: Public API header declaring host bridge callbacks and shared shell utilities
- **p4minishell.c**: Extracted `shell_networking_*` bridge functions from main.c into dedicated module
- **Modular bridge layer**: Networking bridge functions now live in `main/p4minishell.c`, callable from `components/networking`

### Changed
- **main/main.c**: Added `#include "p4minishell.h"` and `#include "p4minishell_config.h"`; networking bridge functions changed from `static` to public linkage
- **main/CMakeLists.txt**: Added `p4minishell.c` to SRCS
- **main/p4minishell.c**: Rewritten as thin bridge layer with extern forward declarations to main.c shell functions

### Architecture Note
- c6ota and usb host bridge functions remain in main.c due to ESP-IDF component model requirements (cross-component linking needs app-component residency)
- networking bridge functions extracted to p4minishell.c since networking component links against main transitively

---

## [0.3.0] - 2026-04-29

### Added
- **Centralized configuration system**: All hardcoded values moved to `p4minishell_config.h` with companion YAML documentation in `p4minishell_config.yaml`
- **Config header**: Single C header with all tunable values organized by subsystem (shell identity, buffers, UI layout, Wi-Fi, Bluetooth, SD card, batch engine, GPIO, header visuals, USB host, C6 OTA, task stacks)
- **Config YAML**: Machine-readable documentation with value, type, description, and valid range for every configurable parameter
- **Backward-compatible aliases**: All existing `SHELL_*`, `NETWORKING_*`, `BLUETOOTH_*`, `HEADER_*`, `C6OTA_*`, and `USB_*` macros preserved as aliases to `P4_CONFIG_*` equivalents

### Changed
- **main/main.c**: Replaced 40+ inline `#define` macros with `#include "p4minishell_config.h"` plus backward-compatibility aliases
- **components/networking/networking.c**: Moved all `#define` values to config header
- **components/networking/bluetooth.c**: Moved all `#define` values to config header
- **components/header/header.c**: Moved color and sizing defines to config header
- **components/c6ota/c6ota.c**: Moved OTA parameters to config header
- **components/usb/usb.c**: Moved USB host parameters to config header; HID key codes kept local (standard USB HID usage table)
- **All component CMakeLists.txt**: Added `${CMAKE_SOURCE_DIR}` to `INCLUDE_DIRS` for config header access

---

## [0.2.0] - 2026-04-29

### Changed
- **Documentation overhaul**: Rewrote all project documentation files (`readme.md`, `documentation.md`, `ai-context.md`, `command.md`, `roadmap.md`, `licence.md`, `API.md`, `SDK.md`) with consistent structure, comprehensive detail, and proper Markdown formatting
- **Source code comments**: Replaced all `// AI:` prefixed comments with proper Doxygen-style documentation throughout `main/main.c`, all component headers, and `board_config.h`
- **Header files**: Added full `@file` Doxygen blocks with architecture descriptions, parameter documentation, and behavioral contracts to `header.h`, `networking.h`, `bluetooth.h`, `c6ota.h`, and `usb.h`
- **board_config.yaml**: Restructured with clear section headers, detailed pin descriptions, and validated hardware metadata

---

## [0.1.24] - 2026-03-18

### Added
- New `components/header` module: fixed LVGL top bar for notifications plus passive Wi-Fi, battery, Bluetooth, USB, and SD status indicators
- Live header notifications from Wi-Fi, USB, Bluetooth, and `c6ota` event paths
- Resolution-scaled header height with clamped minimum/maximum

### Fixed
- SD card status icon now appears consistently with same size, color, and alignment as other status icons
- Header notification area stays blank when idle, shows only during active module events
- SD mount/unmount state changes now trigger immediate header icon updates

### Changed
- Header status indicators use guaranteed-visible retro ASCII labels instead of LVGL symbol glyphs
- Header is non-scrollable with left-to-right status icons and notification area on far right

---

## [0.1.23] - 2026-03-18

### Added
- New `components/usb` module: ESP-IDF USB Host bring-up for MSC external storage and HID keyboard/mouse
- Shell-facing `usb` command family: `usb status`, `usb ls [path]`, `usb keyboard <on|off>`, `usb mouse <on|off>`
- USB MSC storage mounted at `/usb0` via VFS/FATFS with transcript-friendly output
- Managed component dependencies: `espressif/usb_host_msc`, `espressif/usb_host_hid`

---

## [0.1.22] - 2026-03-18

### Changed
- **c6ota refactored** into `components/c6ota` with identical public API and behavior
- Full ESP32-C6 OTA flow moved out of `main/main.c` while preserving confirmation flow, transcript output, source handling, hosted OTA RPC sequence, and Wi-Fi stop/restore
- Added `API.md` and `SDK.md` documenting the stable `c6ota_init`, `c6ota_perform`, and `c6ota_register_progress_callback` integration surface

---

## [0.1.21] - 2026-03-18

### Added
- Hosted NimBLE Bluetooth on ESP32-C6 over ESP-Hosted VHCI in `components/networking/bluetooth.c`
- Shell commands: `bluetooth status`, `bluetooth scan`, `bluetooth advertise <on|off>`, with `bt` alias
- Stateful Bluetooth lifecycle: scan/advertise reuse active hosted controller session

### Changed
- **Networking refactored** into `components/networking`: Wi-Fi runtime state, hosted startup, OTA restore hooks, and Bluetooth handling no longer live in `main/main.c`
- Preserved all existing hosted Wi-Fi behavior and command flow through new module APIs
- Replaced disabled hosted Bluedroid stub with hosted NimBLE

---

## [0.1.20] - 2026-03-18

### Added
- **Serial console bridge**: ESP-IDF UART/USB-Serial-JTAG monitor now accepts shell commands
- stdin lines routed through existing shell worker, transcript, masking, and history flow
- stdout mirrors transcript output

### Fixed
- Monitor write-timeout caused by firmware not consuming interactive serial input
- Serial prompt loop no longer floods `P4Shell>` during idle stdin polling

---

## [0.1.19] - 2026-03-18

### Fixed
- **Command-family dispatch regression**: `wifi status|scan|diag|connect|disconnect` now work correctly after boot
- Root cause: parser now preserves original unsplit command text before tokenization so family handlers receive full command line
- Same fix applied to `sd` and `c6ota` family handlers

---

## [0.1.18] - 2026-03-17

### Changed
- Disabled hosted Bluedroid Bluetooth path after `bt enable` caused board crashes in HCI parser
- `bt` command surface kept visible but returns explicit unsupported state
- Removed direct host BT build dependency

---

## [0.1.17] - 2026-03-17

### Changed
- `gpio list` and `gpio status` now report pins with clearer board-role text
- `rgb` and `camera` messages updated to honestly explain current hardware gaps

### Added
- Hosted Bluedroid Bluetooth path enabled in host build with `bt status|enable|scan`

---

## [0.1.16] - 2026-03-17

### Added
- Hardware control commands: `brightness <0-100>`, `rotate <0|90|180|270>`, `battery`, `volume <0-100>`
- `gpio list|status|read <pin>|set <pin> <0|1>` with write restrictions
- Runtime display rotation with GT911 touch remapping
- ADC-backed battery reporting using board-configured divider values
- ES8311 speaker volume control through BSP codec path
- Parser-visible `bt`, `rgb`, and `camera` families with sdkconfig/metadata gates

---

## [0.1.15] - 2026-03-17

### Added
- `roadmap.md`: parity plan for COMMAND.COM features, native app loading, shell SDK
- `licence.md`: proprietary notice for project-authored code plus third-party license summary

### Changed
- README rewritten to describe P4MiniShell as an embedded DOS-style shell platform

---

## [0.1.14] - 2026-03-17

### Added
- COMMAND.COM-style SD workflow: `cd`/`chdir`, `dir`, `copy`, `move`, `del`/`erase`, `ren`/`rename`, `md`/`mkdir`, `rd`/`rmdir`, `type`, `write`, `append`, `touch`
- RAM-only environment variables: `set`, `path`, `echo`
- Batch file engine: `.bat` execution with `%1`..`%9` expansion, `rem` comments, `echo on/off`, PATH-based lookup
- SD-backed output redirection: `>` and `>>` for text-producing commands

---

## [0.1.13] - 2026-03-17

### Changed
- Finalized ESP-Hosted profile: `espressif/esp_hosted 2.12.1` + `espressif/esp_wifi_remote 1.4.1`
- 1-bit SDIO at 10 MHz on CLK=18 CMD=19 D0=14 D1=15 D2=16 D3=17
- Forced ESP32-C6 reset on every host boot through GPIO54
- 1500-byte `c6ota` transfer chunks

### Fixed
- End-to-end validation: shell UI, BSP display/touch, hosted Wi-Fi, SD tools, and `c6ota` all working together

---

## [0.1.12] - 2026-03-17

### Fixed
- Restored `CONFIG_ESP_HOSTED_SLAVE_RESET_ON_EVERY_HOST_BOOTUP` after Wi-Fi failures
- Restored boot-time and post-`c6ota` Wi-Fi diagnostic pass
- Removed app-side hosted log suppression

---

## [0.1.11] - 2026-03-17

### Fixed
- Reverted `ESP_HOSTED_EVENT_TRANSPORT_UP` wait experiment that regressed Wi-Fi startup
- Restored prior hosted startup order: connect C6, validate firmware version, continue Wi-Fi path

---

## [0.1.10] - 2026-03-17

### Changed
- Removed automatic `wifi diag` scan from boot-time startup and post-`c6ota` restore
- Restored hosted reset policy to `CONFIG_ESP_HOSTED_SLAVE_RESET_ONLY_IF_NECESSARY`
- Suppressed non-actionable `H_SDIO_DRV` and `rpc_rsp` warning noise

---

## [0.1.9] - 2026-03-17

### Added
- **ESP-Hosted firmware compatibility gate**: after `esp_hosted_connect_to_slave()`, shell reads C6 hosted version and refuses Wi-Fi init unless co-processor matches host `2.12.x` line
- Transcript-visible recovery guidance on version mismatch

### Fixed
- SDIO/RPC fallout from mismatched host/co-processor firmware
- Host component lock restored to `espressif/esp_hosted 2.12.1` and `espressif/esp_wifi_remote 1.4.1`

---

## [0.1.8] - 2026-03-17

### Added
- `wifi diag` command: connection state, IP status, nearby-network scan
- Transcript-facing Wi-Fi diagnostics on boot and post-`c6ota`

### Fixed
- Wi-Fi runtime retries: partial init state cleaned up before retrying
- Hosted Wi-Fi restores automatically after normal boot and successful `c6ota` in background task

---

## [0.1.7] - 2026-03-17

### Added
- FATFS long filename support: heap-backed LFN buffers, 255-character limit
- `sd ls` uses direct FatFs directory enumeration for reliable long filenames

### Fixed
- `sd ls` stack-protection panic: command execution moved to dedicated worker task
- `c6ota default` hosted teardown crash: ESP-Hosted SDIO transport kept alive for Wi-Fi-off OTA
- Truncated SD root names and `c6ota default` lookup failures

---

## [0.1.6] - 2026-03-17

### Fixed
- Repeated SD `ldo` warning spam: BSP SD-card power control acquires SD VO4 explicitly at 3300 mV on esp32p4
- Mount-failure and unmount cleanup so repeated `sd` commands don't leak SD power handle

---

## [0.1.5] - 2026-03-17

### Added
- `sd info`: card metadata and root availability
- `sd stat <path>`: resolved path, entry type, size, and mode
- `sd cat <path> [max_bytes]`: bounded text-safe file preview (max 8192 bytes)

### Changed
- All SD commands use shared guarded mount/unmount flow with validated path resolution
- `sd ls` shows entry types and file sizes
- Directory listings bounded to 128 entries

---

## [0.1.4] - 2026-03-17

### Added
- `c6ota default`: auto-load `esp32c6_hosted_slave.bin` or `network_adapter.bin` from SD root
- ESP32-C6 image validation: magic `0xE9` + chip ID `0x000D`
- Factory first-upgrade warning for C6 firmware `v2.3.0`

### Changed
- `c6ota` transfer chunks: 1536 bytes
- Progress output: `C6 OTA: XX% (YYYY KB / ZZZZ KB)` every 5%
- Confirmation prompt: `WARNING: This will reboot the C6. Type YES to continue`
- Success text: `C6 OTA completed successfully! Type reboot to activate new firmware.`
- HTTP images downloaded first, then Wi-Fi stopped for clean SDIO-only OTA transfer

---

## [0.1.3] - 2026-03-17

### Changed
- Healthy shell UI startup no longer emits warning-level log
- Boot milestone preserved in `debug` command history instead

---

## [0.1.2] - 2026-03-17

### Removed
- `c6update` utility fully removed and archived (previously used esp-serial-flasher + GPIO54)

---

## [0.1.1] - 2026-03-14

### Added
- `c6ota <source>` shell command: ESP-Hosted SDIO OTA for ESP32-C6
- Input-driven safety gate with `This will reboot the C6. Continue? (yes/no)` prompt
- ESP-IDF app header validation before transfer
- Live percentage progress in locked transcript UI
- Support for `sd:/firmware.bin` and `http[s]://host/path/to/firmware.bin` sources

---

## [0.1.0] - 2026-03-13

### Added
- Initial shell UI replacing LVGL widgets demo
- BSP-managed JD9165 display and GT911 touch initialization
- Scrollable LVGL textarea transcript, on-screen keyboard, boot banner
- Built-in commands: `help`, `sysinfo`, `clear`, `reboot`
- 10-command recall buffer with Prev/Next touch controls
- Runtime Wi-Fi initialization path following sdkconfig
- Shell Wi-Fi commands: `wifi status`, `wifi connect`, `wifi disconnect`
- Password masking for `wifi connect <ssid> <pass>`
- `wifi scan`, `sd ls`, `mem`, `gpio status`, `debug`, `version`, `about`
- 5-entry debug/error history buffer
- ESP-Hosted + esp_wifi_remote targeting ESP32-C6 over SDIO
- `coprocessor/esp32c6_slave`: repo-local ESP32-C6 hosted slave firmware project
- Station-only Wi-Fi profile, nano newlib, warn-level logging for image size
- PSRAM XIP mapping disabled to prevent flash/PSRAM overflow at link

## [0.1.19] - 2026-03-18
- Fixed the shell command-family dispatch regression that left `wifi status`, `wifi scan`, `wifi diag`, `wifi connect`, and `wifi disconnect` effectively inert even though boot-time hosted Wi-Fi still initialized and connected correctly
- Fixed the root cause in the shell parser by preserving the original unsplit command text before tokenization, so family handlers that re-parse subcommands now receive the full command line instead of only the first token
- Applied the same command-routing fix to the `sd` and `c6ota` family handlers so their subcommand parsing stays reliable without changing the proven boot, display, hosted Wi-Fi, or OTA runtime paths

## [0.1.18] - 2026-03-17
- Disabled the earlier hosted Bluedroid Bluetooth bring-up path on the ESP32-C6 baseline after `bt enable` proved able to crash the board inside the Bluedroid HCI parser during controller startup
- Kept the `bt` command surface visible, but changed it back to an explicit unsupported state on this current ESP32-C6 hosted configuration so boot, display, SD, and Wi-Fi remain stable
- Removed the direct host BT build dependency and hard-gated the shell's Bluetooth runtime path so `bt enable` and `bt scan` now fail safely instead of entering the unstable controller startup path

## [0.1.17] - 2026-03-17
- Tightened `gpio list` and `gpio status` so the shell now reports the exposed board pins with clearer JC1060 and ESP32-P4 role text instead of terse raw labels
- Enabled the hosted Bluedroid Bluetooth path in the host build, added the required BT component dependency, and completed the shell-side `bt status | enable | scan` runtime helpers against the local ESP-Hosted example flow
- Kept `rgb` and `camera` intentionally blocked, but updated those shell messages to explain the current evidence more honestly: the JC1060 reference repo does not expose authoritative RGB LED wiring, and this workspace still lacks the local camera stack needed by the JC1060 camera examples

## [0.1.16] - 2026-03-17
- Expanded the shell with hardware control commands for `brightness`, `rotate`, `battery`, `volume`, and the safer `gpio list | status | read | set` flow while preserving the existing BSP boot path, locked transcript UI, SD tools, Wi-Fi restore flow, and `c6ota` behavior
- Added runtime display rotation with GT911 touch remapping, ADC-backed battery reporting using board-configured divider values, and ES8311 speaker volume control through the existing BSP codec path
- Surfaced `bt status | enable | scan`, `rgb`, and `camera` in the parser and help output with explicit sdkconfig or board-metadata gates so unsupported hardware paths fail clearly instead of pretending support on the current workspace baseline

## [0.1.15] - 2026-03-17
- Rewrote the README introduction to describe P4MiniShell as an embedded ESP32-P4 and ESP32-C6 DOS-style shell platform instead of a minimal demo replacement
- Added `roadmap.md` to capture the missing work for COMMAND.COM parity, native app loading, a future shell SDK and API, and the separate design decision needed for literal DOS `.exe` compatibility
- Added `licence.md` to mark the project-authored code as proprietary to Stoian Alexandru while preserving the verified third-party Apache, MIT, and protobuf-c license obligations already present in the workspace

## [0.1.14] - 2026-03-17
- Expanded the shell toward a COMMAND.COM-style SD workflow with RAM-only `cd`/`chdir`, `dir`, `copy`, `move`, `del`/`erase`, `ren`/`rename`, `md`/`mkdir`, `rd`/`rmdir`, `type`, `write`, `append`, `touch`, `set`, `path`, `echo`, and `call`, while keeping all execution on the existing shell worker task
- Added SD-backed redirection for transcript-safe text commands using `>` and `>>`, with writes confined to the guarded SD mount path and no filesystem writes outside the SD card
- Added a lightweight batch engine for `.bat` files on SD, including `%1`..`%9` argument expansion, `rem` comments, `echo on/off`, PATH-based batch lookup, and direct `.bat` invocation through the normal shell dispatcher

## [0.1.13] - 2026-03-17
- Confirmed the working ESP32-P4 host and ESP32-C6 co-processor baseline end to end: shell UI, BSP-managed display and touch init, hosted Wi-Fi startup, Wi-Fi shell commands, SD tools, and `c6ota` now operate together on the checked-in project configuration
- Finalized the host-side ESP-Hosted profile around `espressif/esp_hosted 2.12.1` plus `espressif/esp_wifi_remote 1.4.1`, 1-bit SDIO at 10 MHz on CLK=18 CMD=19 D0=14 D1=15 D2=16 D3=17, forced ESP32-C6 reset on every host boot through GPIO54, and 1500-byte `c6ota` transfer chunks
- Updated the project docs and board metadata to describe the stable working configuration directly instead of the earlier rollback and investigation state

## [0.1.12] - 2026-03-17
- Reverted the earlier hosted startup cleanup batch after Wi-Fi still failed on the ESP32-P4 to ESP32-C6 SDIO path even after the later transport-wait experiment was removed
- Restored the last known-good hosted reset behavior by switching the ESP32-C6 back to `CONFIG_ESP_HOSTED_SLAVE_RESET_ON_EVERY_HOST_BOOTUP`, because this board baseline had proven Wi-Fi startup only with a forced co-processor reset during host boot
- Restored the original boot-time and post-`c6ota` Wi-Fi diagnostic pass and removed the app-side hosted log suppression so the serial monitor and shell transcript again match the earlier working baseline before any new root-cause investigation

## [0.1.11] - 2026-03-17
- Reverted the app-side `ESP_HOSTED_EVENT_TRANSPORT_UP` wait experiment after it regressed the previously working Wi-Fi startup path on this ESP32-P4 to ESP32-C6 SDIO baseline
- Restored the prior hosted startup order that had Wi-Fi working: connect to the ESP32-C6, validate the hosted firmware version, then continue into the normal sdkconfig-driven Wi-Fi runtime path

## [0.1.10] - 2026-03-17
- Removed the automatic `wifi diag` scan from boot-time Wi-Fi startup and post-`c6ota` restore, keeping those paths limited to the proven connect flow while leaving `wifi diag` available on demand for explicit diagnostics
- Restored the hosted reset policy to `CONFIG_ESP_HOSTED_SLAVE_RESET_ONLY_IF_NECESSARY`, which removes the clean-boot `Reset slave using GPIO[54]` warning without changing the working OTA or Wi-Fi path
- Suppressed non-actionable `H_SDIO_DRV` and `rpc_rsp` warning noise in the app so serial output stays focused on real hosted transport failures while the shell transcript remains the user-facing Wi-Fi status surface

## [0.1.9] - 2026-03-17
- Added an explicit ESP-Hosted firmware compatibility gate to the normal Wi-Fi startup path: after `esp_hosted_connect_to_slave()` the shell now reads the ESP32-C6 hosted version and refuses to continue into `esp_wifi_init()` unless the co-processor matches the host `2.12.x` release line
- Fixed the reported SDIO/RPC fallout from mismatched host and co-processor firmware by failing Wi-Fi startup early with transcript-visible recovery guidance instead of continuing into incompatible `esp_wifi_remote` traffic
- Corrected the host component lock back to the ESP32-C6 `2.12.x` line by restoring `espressif/esp_hosted 2.12.1` and `espressif/esp_wifi_remote 1.4.1`, which matches the checked-in C6 project history instead of forcing the co-processor back to `2.9.x`

## [0.1.8] - 2026-03-17
- Re-enabled the original hosted Wi-Fi runtime automatically after normal boot and after successful `c6ota`, keeping the shell UI startup path intact by running the restore flow in a background task instead of the LVGL input path
- Added transcript-facing Wi-Fi diagnostics on boot, after successful `c6ota`, and through the new `wifi diag` command, including connection state, IP status, and a nearby-network scan with SSID, RSSI, channel, and auth mode
- Hardened Wi-Fi runtime retries by cleaning up partial init state before retrying the original startup routine, so boot-time or post-OTA restore failures report cleanly and can be retried without tearing up the shell

## [0.1.7] - 2026-03-17
- Enabled FATFS long filename support for the shell build using heap-backed LFN buffers with a 255-character limit, which fixes truncated SD root names, `sd ls` long-name failures, and `c6ota default` lookup against `esp32c6_hosted_slave.bin` or `network_adapter.bin`
- Switched `sd ls` to direct FatFs directory enumeration so the shell shows full long filenames reliably and no longer trips over the old invalid-name path during long-entry reads on the mounted SD card
- Kept the proven ESP-Hosted `c6ota` flow pinned to `espressif/esp_hosted` `2.9.7`, with Wi-Fi stopped before transfer, 1536-byte OTA chunks, header validation for magic `0xE9` plus ESP32-C6 chip ID, exact YES confirmation text, 5% progress lines, and the factory `v2.3.0` first-upgrade warning pointing to the standalone CrowPanel tool URL
- Fixed the `sd ls` stack-protection panic by moving shell command execution off the LVGL input-event callback stack and onto a dedicated command worker task with its own stack budget and LVGL mutex handoff
- Fixed the `c6ota default` hosted teardown crash by keeping the existing ESP-Hosted SDIO transport alive for Wi-Fi-off OTA mode instead of calling `esp_hosted_deinit()` before reconnecting the C6 link

## [0.1.6] - 2026-03-17
- Fixed the repeated SD-related `ldo` warning spam by replacing the BSP SD-card on-chip LDO helper with a repo-local power-control path that acquires SD VO4 explicitly at 3300 mV on esp32p4
- Kept the earlier plain-SDMMC fallback for invalid or unsupported LDO-control cases, while tightening mount-failure and unmount cleanup so repeated `sd` commands do not leak the SD power handle

## [0.1.5] - 2026-03-17
- Hardened the shell SD path so all SD commands use a shared guarded mount or unmount flow, validated path resolution, bounded transcript output, and safe cleanup on missing cards, bad paths, and open failures
- Expanded the SD command family with `sd info`, `sd stat <path>`, and `sd cat <path> [max_bytes]`, while keeping `sd ls [path]` compatible and improving it with entry type and file size reporting
- Added transcript-safe limits for SD diagnostics: directory listings stop after 128 entries and `sd cat` previews at most 8192 bytes with non-printable bytes sanitized instead of dumping raw binary into the shell

## [0.1.4] - 2026-03-17
- Repaired `c6ota` to match the proven CrowPanel SDIO OTA method: HTTP images are downloaded first, then the shell stops Wi-Fi completely, reinitializes ESP-Hosted, and performs the OTA transfer over a clean SDIO-only link
- Changed `c6ota` transfer chunks to 1536 bytes, added `c6ota default`, enforced ESP32-C6 image validation with image magic plus chip ID, and updated progress output to `C6 OTA: XX% (YYYY KB / ZZZZ KB)` every 5%
- Replaced the old yes or no prompt with the exact confirmation `WARNING: This will reboot the C6. Type YES to continue`, restored Wi-Fi automatically after OTA failures, and updated the success text to `C6 OTA completed successfully! Type reboot to activate new firmware.`
- Added the factory first-upgrade warning for ESP32-C6 firmware `v2.3.0` and pinned the host manifest to `espressif/esp_hosted` `2.9.7`

## [0.1.3] - 2026-03-17
- Removed the temporary boot-time `W (p4minishell)` shell UI initialization log so healthy boots no longer emit a warning just to advertise that the display transcript is live
- Preserved the same startup milestone through the existing `debug` command history instead of the serial warning path, keeping shell boot, LCD render, and on-screen transcript behavior unchanged

## [0.1.2] - 2026-03-17
- c6update utility fully removed and archived (previously used esp-serial-flasher + GPIO54). No code or build traces remain.

## [0.1.1] - 2026-03-14
- Added a new `c6ota <source>` shell command for full ESP-Hosted SDIO OTA against the ESP32-C6 using either `sd:/firmware.bin` or `http://host/path/to/firmware.bin`
- Added an input-driven safety gate for `c6ota` with the exact prompt `This will reboot the C6. Continue? (yes/no)` before any OTA transfer begins
- Extended the hosted OTA path to mount FATFS for SD sources, validate the incoming ESP-IDF app header, check the current co-processor version with `esp_hosted_get_coprocessor_fwversion()`, and require C6 firmware `v2.9.7+` for reliable SDIO OTA
- Streamed OTA payloads over the existing ESP-Hosted SDIO transport in roughly 1400-byte chunks using `esp_hosted_slave_ota_begin/write/end/activate`, with live percentage progress in the locked transcript UI and friendly fallback guidance on failures
- Kept serial `c6update <path>` for merged-image flashing and redirected legacy OTA-style `c6update ota ...` usage to the new `c6ota` command instead of maintaining two shell entry points for the same hosted update flow

## [0.1.0] - 2026-03-13
- Added a real `c6update ota <https-url>` path that uses `esp_http_client` plus `esp_hosted_slave_ota_begin/write/end` to stream an ESP32-C6 image over HTTPS, report progress every 5%, and request OTA activation when the running co-processor firmware supports it
- Kept the stock-board-safe serial guard for `c6update <path>` so unverified P4-to-C6 flash UART wiring still redirects the user to the external `PROG_C6` header instead of pretending host-side serial flashing is available
- Restored `c6update` to a stock-board-safe behavior on the checked-in JC1060P470C or ESP32-P4-Function-EV-Board baseline: the shell now reports that the external `PROG_C6` header with ESP-Prog, or ESP-Hosted OTA, is required when no verified P4-to-C6 flash UART is configured
- Clarified the stock-board GPIO54 note: GPIO54 remains the hosted reset line reference, but the repository does not claim a verified on-board P4-driven C6 serial flashing path without custom wiring
- Updated `c6update sd:/c6_new.bin` guidance to explain that the command is still used for recovery guidance on stock hardware and that a host `reboot` is only relevant after an external or OTA C6 update succeeds
- Revised `c6update` to follow a GPIO54-driven ESP32-C6 ROM download flow: open the SD image first, pulse GPIO54 low/high, optionally hold BOOT from sdkconfig, connect with `esp_serial_flasher`, flash from offset `0x0` in 4 KB chunks, then reset the target
- Updated `c6update` transcript behavior to report live flashing progress every 5% as `Flashing... XX% (YYYYY bytes)` and to emit explicit UART/SD-card oriented failure guidance on any flash step error
- Documented `c6update sd:/c6_new.bin` as the primary shell example and clarified that a successful ESP32-C6 update still requires a host `reboot` command before the new co-processor firmware is used
- Expanded the shell command set with `wifi scan`, `sd ls`, `mem`, `gpio status`, `debug`, `version`, and `about`, while keeping `help`, `sysinfo`, `clear`, and `reboot`
- Added a 5-entry debug/error history buffer and friendly transcript-facing error messages for command and runtime failures
- Confirmed Enter/OK command submission through the input line `LV_EVENT_READY` handler and documented the DOS-style locked transcript UI model
- Added an SD card driven `c6update <path>` shell command that flashes a merged ESP32-C6 image at offset `0x0` with `esp-serial-flasher`
- Changed hosted Wi-Fi startup to be on-demand from `wifi connect` and disabled host auto-restart when the C6 does not answer init, so the shell still boots for recovery/update flows
- Moved `wifi connect` hosted probing into a background task and shortened the hosted SDIO transport-up retry window so missing-C6 failures return faster with less disruption
- Clarified that the checked-in ESP32-P4-Function-EV-Board baseline does not expose a verified on-board P4-controlled C6 flash UART, so `c6update` now reports the external `PROG_C6` / OTA requirement instead of asking for impossible GPIO defaults
- Added sdkconfig-backed ESP32-C6 flasher wiring controls for UART port, UART TX/RX, EN, reset, and BOOT GPIOs so the host does not hardcode board-specific programming pins
- Reported C6 flashing progress and wiring/runtime failures directly into the locked transcript UI while keeping the normal BSP/LVGL shell model unchanged
- Added `espressif/esp-serial-flasher` to the app manifest and removed the duplicate placeholder `app_main()` source from the registered build inputs
- Replaced the LVGL widgets demo in main/main.c with a shell UI built on the existing BSP startup path
- Preserved the original BSP-managed JD9165 display and GT911 touch initialization by keeping bsp_display_start_with_config() and BOARD_CFG_* settings unchanged
- Added a scrollable LVGL textarea, attached lv_keyboard, boot banner, and built-in commands: help, sysinfo, clear, reboot
- Added sysinfo reporting for board_config-backed values, ESP-IDF version, heap usage, PSRAM totals, and Wi-Fi unsupported status on esp32p4
- Refreshed project metadata to match the current shell implementation and verified esp32p4 baseline
- Split the shell UI into a read-only transcript area plus a dedicated prompt-bearing input line bound to the on-screen keyboard
- Added LV_EVENT_READY command submission on the input line and a 10-command recall buffer with Prev/Next touch controls
- Added a runtime Wi-Fi initialization path that follows sdkconfig only, logging every init step or failure into the shell transcript during boot
- Kept the current esp32p4 baseline safe by reporting when sdkconfig does not enable native Wi-Fi or ESP32-C6 host Wi-Fi instead of changing Kconfig
- Added project Wi-Fi defaults in sdkconfig and enabled the host Wi-Fi stack path used by the current esp32p4 workspace
- Added shell Wi-Fi commands for status, connect, and disconnect, with transcript-safe password masking for `wifi connect <ssid> <pass>`
- Disabled LVGL example compilation in sdkconfig so the Wi-Fi-enabled shell still fits the esp32p4 link image budget
- Trimmed sdkconfig to a station-only Wi-Fi profile and disabled Wi-Fi IRAM-heavy optimizations so the host Wi-Fi shell can link on esp32p4
- Reduced compile-time log verbosity, enabled newlib nano format, and disabled AMPDU so the Wi-Fi-enabled image sheds more flash/rodata pressure on esp32p4
- Disabled PSRAM XIP instruction and rodata mapping in sdkconfig because the Wi-Fi-enabled image was overflowing the shared flash/PSRAM mapping window at final link
- Fixed Wi-Fi runtime startup by initializing NVS before esp_wifi_init(), including the standard erase-and-retry recovery path for incompatible stored NVS data
- Replaced the esp32p4 extconn/ESP8689 host Wi-Fi path with ESP-Hosted plus esp_wifi_remote targeting an ESP32-C6 co-processor over SDIO
- Updated the checked-in host transport configuration to CLK=18 CMD=19 D0=14 D1=15 D2=16 D3=17 with reset GPIO54, and mirrored those defaults into sdkconfig.defaults
- Updated shell Wi-Fi diagnostics so hosted-link failures now report the ESP32-C6 SDIO backend and pin map instead of the older extconn hardware note
- Changed ESP-Hosted reset policy to `CONFIG_ESP_HOSTED_SLAVE_RESET_ONLY_IF_NECESSARY` so the host no longer resets the C6 on every clean boot
- Added `coprocessor/esp32c6_slave`, a repo-local ESP32-C6 ESP-Hosted slave firmware project that tracks the same `2.12.1` source line as the host dependency lock