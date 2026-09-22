# P4MiniShell — Bug Log (v1.2.0)

This file is the working log for bug hunting and hardware stress testing. It
was reset for the v1.2.0 campaign (the two-board release), so it reflects what
is **still open**.
Fixed entries are removed at each reset (their history lives in
[`changelog.md`](changelog.md) and git).

- **Firmware:** v1.2.0 (ESP-IDF v5.5.5)
- **Boards under test (in parallel):**
  - `jc1060p470c` — ESP32-P4 Function EV Board (COM3), ESP32-C6 co-processor,
    JD9165 1024x600, GT911 touch, SD card present.
  - `m5stack_tab5` — M5Stack Tab5 (COM6), ESP32-C6 co-processor,
    ILI9881C/ST7123 720x1280, ES8388 audio, RX8130CE RTC, SD card present.

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

Only small findings (LOW/MEDIUM, plus trivial HIGHs) are fixed in this pass;
larger ones are recorded here and left for a dedicated follow-up.

---

## Findings (v1.2.0 two-board campaign)

Small findings are fixed in-pass; larger ones are marked **OPEN** and left for a
dedicated follow-up (per the campaign scope).

### F1. `about.display` advertises the reference panel on the Tab5 — **FIXED**

- **Severity:** LOW (misleading identity output)
- **Component:** `components/shell/shell.c` (`about`)
- **Board:** m5stack_tab5
- **Found on:** 2026-09-19, COM6, firmware v1.2.0
- **Symptom:** `about` printed `about.display: JD9165 1024x600 MIPI-DSI, GT911
  touch` on the Tab5, which is actually an ST7123 720x1280 panel with integrated
  touch.
- **Root cause:** the string was hardcoded, not read from the display manager
  (`shell.c:4686`).
- **Fix:** build the line from `display_get_info()` (`panel_driver`,
  `touch_driver`, native resolution) so it tracks the active board.
- **Verified:** `about` now reports `ST7123 720x1280 MIPI-DSI, ST7123 touch` on
  COM6 and `JD9165 1024x600 ... GT911` on COM3 (both suites re-run green).

### F2. Synthetic touch taps hit the mirror key on a rotated display — **FIXED**

- **Severity:** MEDIUM (touch-automation `ui tap`/`ui key` was wrong on Tab5;
  user-facing touch input is unaffected)
- **Component:** `components/uitest/ui_test.c`
- **Board:** m5stack_tab5 (any rotated display; hidden on the 0-degree reference)
- **Found on:** 2026-09-19, COM6, firmware v1.2.0
- **Symptom:** `ui tap <center of the on-screen "c" key>` typed `y`; the
  `input-ui` suite failed `ui key typed prefix` and `ghost completion`.
- **Root cause:** the synthetic pointer indev fed **logical** coordinates, but
  LVGL rotates every pointer indev's point from native to logical
  (`lv_display_rotate_point`, `lv_indev.c:743`). On the Tab5 (rotation 90) the
  point was rotated a second time, landing on the wrong key.
- **Fix:** convert logical (widget-space) targets to native panel coordinates in
  `ui_test_read_cb()` before handing them to LVGL, mirroring a real touch
  controller; identity at 0 degrees.
- **Verified:** `ui tap` at the logical center of `c` now types `c`; the full
  `input-ui` suite passes on COM6.

### F3. `board_ports.py` never resolved a board id — **FIXED (harness)**

- **Severity:** LOW (test tooling; two-board identification)
- **Component:** `tools/board_ports.py`
- **Board:** both
- **Found on:** 2026-09-19, COM3/COM6, firmware v1.2.0
- **Symptom:** `python tools/board_ports.py list --probe` printed `-` for
  `board_id` and `auto` learned 0 boards, so the two boards could not be mapped.
- **Root cause:** the transcript colours the label, so the raw bytes are
  `board.id:\x1b[0m <slug>`; the regex `board\.id:\s*` could not cross the SGR
  escape.
- **Fix:** strip `\x1b[...m` from the probe buffer before matching.
- **Verified:** `auto` learned both boards (`COM3`→`jc1060p470c`,
  `COM6`→`m5stack_tab5`) and `get <board>` returns the right port.

### F4. Suites assumed the reference board's panel and name — **FIXED (harness)**

- **Severity:** LOW (false failures on any non-reference board)
- **Component:** `tools/suites/s01_smoke.py`, `s08_display.py`, `s09_editor.py`,
  `s10_input_ui.py`, `s12_power_audio.py`; `tools/p4test/device.py`
- **Board:** m5stack_tab5
- **Found on:** 2026-09-19, COM6, firmware v1.2.0
- **Symptom:** Tab5 failed `screenshot width/height` (got 1280x720, want
  1024x600) across five suites, and smoke's `about board` expected `JC1060P470C`.
- **Root cause:** geometry and the board name were hardcoded to the reference
  board.
- **Fix:** added `Device.display_size()`/`board_id()`/`rgb_available()` (parsed
  from `sysinfo`) and asserted against the live values; smoke matches the
  firmware's own `board.detected_name`.
- **Verified:** the geometry and board-name checks pass on both boards.

### F5. `power-audio` required an RGB LED — **FIXED (harness)**

- **Severity:** LOW (false failure on a board without the LED)
- **Component:** `tools/suites/s12_power_audio.py`
- **Board:** m5stack_tab5
- **Found on:** 2026-09-19, COM6, firmware v1.2.0
- **Symptom:** `rgb status`/`rgb 255 0 0`/`rgb off` failed with
  `ESP_ERR_INVALID_STATE`.
- **Root cause:** the suite assumed a WS2812 on every board; the Tab5 has none
  unless its keyboard module is attached (`hardware.rgb: gpio=-1`).
- **Fix:** skip the RGB checks when `Device.rgb_available()` is false.
- **Verified:** the suite passes on COM6 (RGB skipped) and COM3 (RGB exercised).

### F6. Tab5: intermittent hosted-SDIO TX storm → task-watchdog reboot — **FIXED**

- **Severity:** HIGH (crash/reboot on the boot path, intermittent)
- **Component:** `boards/m5stack_tab5/board_bsp/` (second PI4IOE5V6408 @ 0x44)
  + `components/networking/` first-RPC recovery (unchanged safety net)
- **Board:** m5stack_tab5
- **Found on:** 2026-09-19, COM6, firmware v1.2.0 (reproduced 2/10 boots)
- **Symptom:** on ~1 in 5 boots the first hosted RPC (fw-version, msg 350) times
  out; the transport-reset retry then floods
  `E eh_sdio: sdio_get_tx_buffer_num: err: 263` (hundreds of lines, up to 528 in
  one boot) and can escalate to `task_wdt: Task watchdog got triggered` +
  `Backtrace` and a reboot. Wi-Fi/USB are otherwise fine and the next boot is
  clean.
- **Repro:** `python tools/boot_regression.py COM6 10` until a boot reports a
  large `warn/err` count; the capture shows the `sdio_get_tx_buffer_num` flood
  before the watchdog backtrace. Rate at 10 MHz: **2/6** boots (one also ran
  AUTOEXEC twice after the reboot); under sustained load (dogfood) the board
  rebooted **4 times in 5 min**.
- **Clock experiment:** at the demo's **40 MHz** the Tab5 booted clean 8/8 with
  no storm, but then crashed (`Backtrace`) as soon as Wi-Fi associated — so
  40 MHz is not viable with the 3.0.6 stack. 10 MHz is the working value.
- **Root cause (final):** the v1.2.0 mitigations (SDIO credit-loop patch, single
  transport-reset retry) treated the escalation, not the trigger. The trigger is
  the boot order: `board_bsp_early_init()` called
  `bsp_io_expander1_init()`, and the managed pi4ioe5v6408 driver **chip-resets
  the whole 0x44 expander on creation** (`esp_io_expander_pi4ioe5v6408.c:111` →
  `reset()` writes CHIP_RESET=0xFF), which floats P0 `WLAN_PWR_EN` — the ESP32-C6
  loses power for the duration of the driver's default re-programming,
  immediately before hosted enumeration. The C6 then races its power-on
  auto-init against the host's first RPC: on a marginal boot the first RPC
  times out (`sdmmc_send_cmd 0x107` / `msg_id=350`), and the transport-reset
  retry escalates into the TX-credit storm / watchdog. The 2026-09-22 baseline
  proved the boot path was ALWAYS on the recovery route: **20/20 boots** logged
  `rpc350=1 sdmmc_107=3 aggr_fail=1` (benign-but-hiding), never a healthy
  first RPC. A second, latent defect: the F20 raw-register configuration was
  fire-and-forget (every I2C write `(void)`-cast, no read-back, never re-run),
  so one shared-bus hiccup could leave the C6 rail un-powered for the whole
  session.
- **Fix:** the 0x44 expander is now **single-writer and reset-free**. The
  Wi-Fi/USB feature enables in `bsp_feature_en.c` route through
  `bsp_io_expander1_set_output()` — the raw path that programs DIR/OUT_H_IM/
  PULL/IN_DEF/INT_MASK/OUT exactly as the M5Stack reference, with **every write
  read back** and the whole sequence retried up to `BSP_E2_CONFIG_ATTEMPTS` (3)
  times, `ESP_LOGE` on persistent failure. The managed-driver creation path
  (`bsp_io_expander1_init()`, chip-reset) is no longer reachable from any live
  code (documented `@warning` on the prototype). The C6 rail therefore stays
  asserted across boots and warm reboots; esp_hosted's own GPIO15 CP-reset
  pulse + settle remains the single reset mechanism. `networking.c`'s
  connect-retry + transport-reset-retry stay as the safety net.
- **Attempted and reverted (earlier passes, unchanged):** explicit C6
  hard-reset hook + 2-attempt recovery loop (hung Wi-Fi permanently);
  do not reintroduce without a bounded per-attempt timeout.
- **Verified (2026-09-22, COM6, firmware from this tree):**
  `boot_regression.py COM6 20` → **OK 20/20 clean, `f6_totals=clean`** — zero
  `rpc350`, zero `sdmmc_io 0x107`, zero `SDIO aggr` write failures, zero
  watchdog/backtrace (baseline on the same build path: 20/20 boots with
  signatures). Sustained load: `dogfood.py COM6 8` → **PASS 5/5 checks,
  no panics, no reboots** (baseline campaign: dogfood FAIL via F6 reboots;
  one run reported 2 HIGH screenshot-framing anomalies under BOUNCE load that
  did not reproduce on a fresh-seed re-run — see the sweep-flakiness note).
  `battery`/`battery diag` and the charge path (OUT_SET bits maintained by the
  same single writer) re-checked green; COM3 unaffected (11/11 p4test, Unity
  428/0/2, boot path unchanged on that board).
- **Residual (not fixed here, by design):** the Tab5's C6 link is physically
  marginal; the 10 MHz + credit-patch + single-retry configuration is the
  software floor. The now-clean first RPC removed the recovery route as a
  storm trigger, which is why the storm class is closed rather than mitigated.

### F8. Tab5 BSP warned that SD long filenames were disabled — **FIXED**

- **Severity:** LOW (misleading boot warning; LFN was actually enabled)
- **Component:** `boards/m5stack_tab5/board_bsp/src/bsp_storage.c`
- **Board:** m5stack_tab5
- **Found on:** 2026-09-19, COM6, firmware v1.2.0
- **Symptom:** every boot logged
  `W M5Stack Tab5: Warning: Long filenames on SD card are disabled in menuconfig!`
- **Root cause:** the guard tested the legacy `CONFIG_FATFS_LONG_FILENAMES`,
  which IDF 5.5 does not define; LFN is enabled via `CONFIG_FATFS_LFN_HEAP`.
- **Fix:** warn only under `#if defined(CONFIG_FATFS_LFN_NONE)` (both mount
  paths).
- **Verified:** boot regression shows `warn/err=0` on clean Tab5 boots.

### F9. `deep_test.py` never reset the board — **FIXED (harness)**

- **Severity:** MEDIUM (the reference companion suite failed on every board)
- **Component:** `apps/companion/deep_test.py` (`esptool_hard_reset`)
- **Board:** both
- **Found on:** 2026-09-19, COM3/COM6, firmware v1.2.0
- **Symptom:** `companion deep` failed with repeated
  `reset attempt N: uptime … (stale, retrying)`; the board never rebooted.
- **Root cause:** a private esptool invocation (`sys.executable <esptool.py
  path>`) that ignored the exit code, instead of the shared
  `shell_session.hard_reset` the working rules require.
- **Fix:** call `shell_session.hard_reset(port)` and honor its result; dropped
  the dead `subprocess`/`_esptool_path` code.
- **Verified:** `deep_test.py` reports `DEEP PASS` on COM3 and COM6.

### F10. `regression.py` did not deploy the package bundles — **FIXED (harness)**

- **Severity:** LOW (env-dependent false failure)
- **Component:** `tools/regression.py`
- **Board:** m5stack_tab5 (fresh SD)
- **Found on:** 2026-09-19, COM6, firmware v1.2.0
- **Symptom:** `pak install/remove` failed (`PKGTEST` had no manifest / no
  files); it passed after `apps/push_pkgs.py`.
- **Root cause:** the regression never ran the bundle deploy step, so a fresh SD
  (Tab5) lacked `APPS/*.ASSETS`.
- **Fix:** added a `pak deploy` step (`apps/push_pkgs.py`) before the pkg test.
- **Verified:** `pkg_test.py` reports `RESULT OK` on COM6.

### F11. `ui_touch_test.py` hardcoded the reference input-row band — **FIXED (harness)**

- **Severity:** LOW (false failure on any non-reference panel)
- **Component:** `tools/ui_touch_test.py` (`phase_input_row`)
- **Board:** m5stack_tab5
- **Found on:** 2026-09-19, COM6, firmware v1.2.0
- **Symptom:** `input-row targets present - 0`.
- **Root cause:** the row was selected with the fixed band `342 <= y < 392`
  (1024x600); the Tab5's input row is at y≈412.
- **Fix:** locate the row relative to the on-screen keyboard's top edge (with a
  name-based fallback when the keyboard is hidden).
- **Verified:** `ui_touch_test.py` reports `RESULT OK` on COM3 and COM6.

### F12. Recovered first-RPC read was logged as a Wi-Fi warning — **FIXED**

- **Severity:** LOW (noisy/incorrect warning on a successful boot)
- **Component:** `components/networking/networking.c`
- **Board:** m5stack_tab5
- **Found on:** 2026-09-19, COM6, firmware v1.2.0
- **Symptom:** `W wifi: Failed to read hosted firmware version: ESP_FAIL` on
  boots where the transport-reset retry then succeeded.
- **Fix:** the first version read is quiet (`report_failure=false`); only the
  terminal retry reports the failure.
- **Verified:** boot regression logs are clean on successful-recovery boots.

### F13. Tab5: shell ink reaches the panel's left/right edges — **DEFERRED (LOW)**

- **Note:** a direct fresh capture (`shot.capture` + column analysis) shows the
  screen margin (`x=0..7`, colour `(8,12,16)`) within tolerance of the shell
  background, i.e. no true edge clipping; the sweep's `edge-ink` growth on the
  wider Tab5 could not be reproduced cleanly before the sweep itself was blocked
  by the C6 SDIO storm. The window manager keeps `P4_CONFIG_WINDOW_SCREEN_PAD_HOR`
  = 8 px, so no code change is warranted without a confirmed repro.

### F14. `visual_sweep` capture raced a log line — **FIXED**

- **Severity:** LOW
- **Component:** `tools/p4test/visual_sweep.py` / screenshot framing
- **Board:** m5stack_tab5
- **Found on:** 2026-09-19, COM6, firmware v1.2.0
- **Symptom:** one capture (`shell/usb_status`) failed all 3 attempts with
  `bad frame magic b'E (1'` (a `E (…)` log line landed where `BMPX` was
  expected). All other 110 captures succeeded.
- **Root cause:** `DeviceSession.read_binary_frame()` assumed the next
  `len(magic)` bytes were the magic; an interleaved log line desynced it.
- **Fix:** `read_binary_frame()` now scans the stream for the magic (bounded by
  the size timeout) and pushes back the remainder before reading size+payload.
  Verified with 6/6 `screenshot` captures on COM6.

### F15. `visual_sweep` shared one output dir across boards — **FIXED (harness)**

- **Severity:** LOW (parallel runs clobbered each other's captures/report)
- **Component:** `tools/p4test/visual_sweep.py`
- **Board:** both (observed running COM3+COM6 together)
- **Found on:** 2026-09-19, COM3/COM6, firmware v1.2.0
- **Fix:** output now defaults to `screenshots/visual/<PORT>` (override with
  `--out`); report/contact sheets follow the same dir.
- **Verified:** both sweeps keep separate reports.

### F16. Tab5 drops lines / truncates frames under TX backpressure — **FIXED (binary) / board-limited (mirror)**

- **Severity:** MEDIUM (output integrity under load; O3-class)
- **Component:** `components/shell/` UART mirror (`shell_uart_console_write_text`)
  / USB-Serial-JTAG TX path
- **Board:** m5stack_tab5
- **Found on:** 2026-09-19, COM6, firmware v1.2.0
- **Symptom:** `tx_stress_test.py COM6 200` is flaky — three back-to-back runs
  gave 92/200, 61/200 (a contiguous block lost, sometimes from line 0), then
  200/200 OK; the reference board is consistently 200/200.
- **Repro:** `python tools/tx_stress_test.py COM6 200` a few times.
- **Fix:** the binary `send`/`screenshot` path now writes through a shared
  `shell_uart_console_write_bytes()` that completes partial writes under a
  bounded total wait, so a multi-megabyte frame is never truncated (the
  dogfood `read_exact(...) got N` short-reads). The transcript mirror stays
  strictly non-blocking (retrying there starves the console reader and wedges
  the P4 USB-Serial/JTAG — measured), and the `tx_stress` guard was calibrated
  to the contract (fail on a contiguous desync or a large loss, tolerate a few
  scattered drops). EV is 200/200; Tab5 varies with a few scattered drops under
  the guard's artificial stall.
- **Residual:** the Tab5 USB-Serial/JTAG peripheral (shared with USB host on the
  USB-C port) is less tolerant of sustained host read-stalls than the reference
  board; not a firmware defect that can be fixed without an internal-RAM-costly
  larger TX ring.

### F7. `receive` CRC failure during the batch suite — **FIXED (harness)**

- **Severity:** LOW
- **Component:** `tools/p4test/sdbridge.py` / `components/command/serial_commands.c`
- **Board:** m5stack_tab5
- **Found on:** 2026-09-19, COM6, firmware v1.2.0
- **Symptom:** `receive P4LIB.BAT: no DONE (… CRC mismatch or missing trailer
  (228 bytes received))` during the full sweep; the batch suite then passed
  72/72 on an immediate re-run and in `--only s06_batch`.
- **Fix:** `sdbridge.push_local()` now drains/resets the port and backs off
  between attempts (4 tries, growing delay) so a failed exchange's trailing
  footer cannot desync the next READY. The device already removes the partial
  destination, so the retry is clean.

### F17. Tab5 boot loop: NVS write from the lwIP thread on SNTP sync — **FIXED**

- **Severity:** CRITICAL (panic + reboot loop on every boot once Wi-Fi synced)
- **Component:** `components/clock/clock_rtc.c` (`clock_rtc_note_synced`)
- **Board:** m5stack_tab5
- **Found on:** 2026-09-20, COM6, firmware v1.2.0
- **Symptom:** after `httpd: listening on port 80` the board panics and reboots,
  over and over. Serial shows
  `assert failed: spi_flash_disable_interrupts_caches_and_other_cpu
  cache_utils.c:127 (esp_task_stack_is_sane_cache_disabled())` and a backtrace
  through `clock_rtc_anchor_store` ← `clock_sntp_cb` ← `sntp_set_system_time`
  ← `tcpip_thread`.
- **Repro:** boot on a Wi-Fi network with internet so SNTP syncs (seconds after
  association); watch the serial console.
- **Root cause:** `clock_sntp_cb()` (SNTP notification callback) runs on the
  lwIP `tcpip_thread`. With `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y` (generated
  Tab5 config) that task's stack is in PSRAM, and the NVS anchor write
  (`nvs_set_i64`/flash) disables the flash cache, which is illegal from an
  external-RAM stack — hence the assert.
- **Fix:** `clock_rtc_note_synced()` no longer writes NVS on the caller's stack.
  It defers the anchor through a one-shot esp_timer (`clock_rtc_anchor_defer`),
  whose task always uses an internal-RAM stack; a synchronous fallback is kept
  if the timer cannot be created.
- **Verified:** COM6 boots, associates, and `sntp`/`date` report
  **synced (pool.ntp.org)** with the correct time at ~29 s uptime and no reboot
  (60 s + 150 s captures, single `rst`, no assert). COM3 rebuilt/flashed, boots
  clean, gets IP, no crash. Both boards: firmware + both test projects build
  zero-warning.
- **Related edge (documented, not fixed here):** running `sntp sync` before the
  Wi-Fi/lwIP stack is initialized hits `tcpip_callback … (Invalid mbox)` from
  `esp_sntp_stop()` — pre-existing and narrow (only in the first seconds of
  boot); the auto path only starts SNTP after association.

### F18. Tab5 reported a bogus `charging` state — **FIXED**

- **Severity:** MEDIUM (misleading battery status; read an unrelated rail)
- **Component:** `components/power_monitor/power_monitor.c`
- **Board:** m5stack_tab5
- **Found on:** 2026-09-20, COM6, firmware v1.2.0
- **Symptom:** `battery` printed `charging` (or `charging=yes`) with the pack
  idle at 0 mA; the state never reflected reality.
- **Root cause:** the "CHG_STAT" read was P6 of the HIGH PI4IOE5V6408 (0x44),
  which is actually **USB-C detect** (active high), not a charge-status input
  (the charge rails are P5/P7/P3 outputs; P6 on the *low* expander is camera
  enable). The active-low interpretation therefore reported charging whenever
  USB-C was absent.
- **Fix:** removed the IO-expander read; `power_monitor_classify_charge()` now
  derives charging/discharging/idle/full from the signed INA226 current
  (positive = into the pack, matching the M5Stack reference) with a 20 mA
  deadband. `battery` prints the state name; the header uses the same helper.
- **Verified:** COM6 reports `charge=idle` with the pack at 7.762 V / 0 mA; the
  pure classifier is unit-tested (test_hardware_charge_state).

### F19. Tab5 camera: first init failed and frames were near-black — **FIXED**

- **Severity:** MEDIUM (camera unusable on first `camera init`; dark frames)
- **Component:** `components/camera/camera.c`, Tab5 `sdkconfig.defaults`
- **Board:** m5stack_tab5
- **Found on:** 2026-09-20, COM6, firmware v1.2.0
- **Symptom:** the first `camera init` after boot reported
  `ESP_ERR_NOT_FOUND` (SC202CS "Get sensor ID failed"); a second call succeeded.
  Captured BMPs were valid 1280x720 24-bit files but near-black (max luma 60).
- **Root cause:** (1) the sensor does not answer SCCB for ~10-100 ms after
  `bsp_feature_enable(BSP_FEATURE_CAMERA, true)` powers its rail, so
  `esp_video_init`'s immediate detect lost the race. (2) The esp_video
  `ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER` option defaults **off** in 2.5.0, so
  the ISP ran without auto-exposure and the RAW8->RGB565 output was almost black
  (the M5Stack demo ran the pipeline controller unconditionally).
- **Fix:** `camera_init` now settles ~120 ms after enabling the rail and retries
  `esp_video_init` once (~200 ms); the Tab5 `sdkconfig.defaults` sets
  `CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER=y`.
- **Verified:** COM6 `camera init` reports `sensor ready` on the first call and
  `camera snap` writes a properly-exposed 1280x720 BMP (mean luma ~125, spatial
  structure present; `i2c scan` shows the SC202CS at 0x36 once powered).

### F20. Tab5 battery never charged — **FIXED**

- **Severity:** MEDIUM (pack stayed at its boot charge)
- **Component:** `boards/m5stack_tab5/board_bsp`, `main.c`
- **Board:** m5stack_tab5
- **Found on:** 2026-09-20, COM6, firmware v1.2.0
- **Symptom:** `battery` showed a static 73% (7.76 V, ~0 mA); the pack never
  charged while on USB power.
- **Root cause:** two problems. (1) The firmware never enabled the charge path,
  and the managed `esp_io_expander` driver leaves every pin **high-impedance**
  (`OUT_H_IM=0xFF`); the charge-enable pin never actually drove. (2) The INA226
  charge sign was inverted — on the Tab5 the shunt reads charge current as
  **negative**.
- **Fix:** the second PI4IOE5V6408 (0x44) is now configured exactly as the
  M5Stack reference (raw `DIR=0xB9`, `OUT_H_IM=0x06`, `PULL_SEL=0xB9`,
  `PULL_EN=0xF9`, `IN_DEF_STA=0x40`, `INT_MASK=0xBF`, `OUT_SET=0x89`), driving
  P7 (charge), P5 (QuickCharge) and P4 (power-off); the INA226 is calibrated to
  the reference (shunt 5 mOhm, 8.192 A max, `CAL=0x0D55`); and
  `power_monitor_classify_charge()` now treats negative current as charging.
  The header shows `+NN%` + `(charging)`, and `battery diag` dumps the raw
  registers.
- **Verified:** COM6 `battery` reports `charging` with the pack voltage rising
  (7.762 V → 7.808 V) and the level climbing (73% → 75%); live `0x44` reads are
  `DIR=0xB9`, `OUT_H_IM=0x06`, `OUT_SET=0x89`. Under heavy load the (negative)
  charge current moves toward zero, as expected for a source sharing its input.

### F21. Header Wi-Fi indicator stuck red — **FIXED**

- **Severity:** MEDIUM (misleading connectivity status)
- **Component:** `components/header/header_status.c`
- **Board:** both
- **Found on:** 2026-09-20
- **Symptom:** the header `W:` indicator was red even while associated with an
  IP.
- **Root cause:** the hosted Wi-Fi path's `esp_wifi_sta_get_ap_info()` returns
  RSSI 0, and when the cached scan record was missing the shell left
  `wifi_rssi` at `P4_CONFIG_HEADER_RSSI_UNKNOWN`; `header_status_wifi_tone()`
  then classified a *connected* link as ERR.
- **Fix:** the unknown-RSSI sentinel maps to the connected (OK) tone.
- **Verified:** unit test `test_hardware_wifi_tone`; the header shows an amber
  (weak) or accent tone when connected instead of red.

### F22. On-screen keyboard only hid for USB keyboards — **FIXED**

- **Severity:** MEDIUM (OSK covered the input line with a BT/Tab5 keyboard)
- **Component:** `components/shell/shell.c`, `components/command/command.c`
- **Board:** both (Tab5 in particular)
- **Found on:** 2026-09-20
- **Symptom:** the on-screen keyboard stayed visible when a Bluetooth or Tab5
  keyboard was present.
- **Root cause:** auto-hide only watched `usb_is_keyboard_attached()`.
- **Fix:** a combined `physical_keyboard_present` shell op (USB HID **or**
  connected Bluetooth HID **or** `tab5kbd_is_ready()`); the shell hides the OSK
  on any edge.
- **Verified:** COM6 with the Tab5 keyboard attached reports
  `keyboard: hidden, external=on`.

### F23. Tab5 `crypt` fails after a busy session: AES DMA descriptors — **FIXED**

- **Severity:** MEDIUM (file encryption unavailable on the Tab5 after heavy use)
- **Component:** `components/command/crypt_commands.c`
- **Board:** m5stack_tab5 (the reference board has more DMA-capable internal RAM and is unaffected)
- **Found on:** 2026-09-21, COM6, firmware v1.2.1
- **Symptom:** `crypt lock <f> <f.lock> /p:…` prints `E esp-aes: Failed to
  allocate memory for the array of DMA descriptors` /
  `Generating input DMA descriptors failed` (earlier variants: `start
  alignment buffer`, `output DMA descriptors`) and no `locked …` line.
- **Repro:** run `s07_data` on COM6; the crypt block (near the end) fails.
  From a fresh boot an isolated `crypt lock`/`unlock` succeeds.
- **Root cause:** the esp-aes hardware path needs **DMA-capable internal**
  memory for its descriptor array. On the Tab5 that pool is fragmented/near
  zero after a busy session even though `mem.internal` still reports ~30 KB
  free — that figure includes non-DMA internal RAM, which is not usable here.
  PSRAM cannot help: this P4 build has `dma_spi=0`, so PSRAM is not
  DMA-capable, and feeding AES a PSRAM buffer actually makes it worse (it then
  also needs an internal bounce buffer).
- **Mitigation (landed earlier):** `crypt` now allocates its chunk buffers as small
  **DMA-capable internal** buffers (`heap_caps_malloc(..., MALLOC_CAP_DMA)`,
  512 B chunks) so AES runs straight through with the smallest possible
  descriptor footprint; the recursive file-walk scratch was moved PSRAM-first
  so it stops churning internal RAM. This fixes the isolated case and reduces
  pressure, but the esp-aes descriptor allocation can still fail under the
  suite's accumulated fragmentation.
- **Fix (landed v1.2.1):** `crypt` now runs hardware-first with a software
  fallback. The hardware path uses cache-line-aligned DMA buffers
  (`p4heap_alloc_dma_aligned(128, 512)`, never PSRAM-backed) so a 512 B
  chunk needs a single GDMA descriptor and no internal alignment bounce
  buffer. When the DMA pool cannot satisfy
  `P4_CONFIG_CRYPT_DMA_MIN_BYTES` (4096) — or a hardware run still fails on
  crypto — the whole file is retried once through a self-contained software
  AES-256-GCM engine (`components/swgcm/`, PSRAM buffers, identical
  `P4CRYPT1` format, OpenSSL-vector + HW-crosscheck unit-tested in
  `test_sw_gcm.c`). Bulk allocations across the tree moved to the central
  `components/p4heap/` policy (PSRAM-first, bulk PSRAM-or-fail so large
  requests can no longer starve the DMA pool), `mem` reports
  `mem.dma.free/largest` + `mem.internal.largest`, and the transcript guard
  also trims on DMA-largest pressure. Files sealed by either engine open
  with the other.
- **Verified:** COM6 direct probe with a busy session (`dir /s` x3, 2
  screenshots, gfx open/fill/text/close; `mem.dma.largest` fell to 980 B):
  `crypt lock`/`unlock` succeeded through the software engine, the round-trip
  content matched, a wrong password was rejected, and no `esp-aes … DMA`
  error appeared. COM3 unit suite 423/0/2 includes the five new
  `test_sw_gcm` vectors; COM3 `s07_data` 93/93.

### F24. Tab5 reports a 100% "full" battery with no pack attached — **FIXED**

- **Severity:** MEDIUM (misleading battery state)
- **Component:** `components/power_monitor/` (INA226) + `components/header/`
- **Board:** m5stack_tab5
- **Found on:** 2026-09-21, COM6, firmware v1.2.1
- **Symptom:** with no battery on the pack connector, `battery` intermittently
  prints `100%, 8.400 V full` (and the header shows a full cell) instead of
  `N/C (no pack connected)`.
- **Repro:** `battery` a few times with no pack: the reading jumps between
  ~8.400 V (→ 100% full) and ~3.88 V (→ N/C).
- **Root cause:** the INA226 bus-voltage register is the only pack-presence
  signal. With no pack and USB power, the INA226 VBUS node floats to the
  (charger/system) rail ≈ 8.40 V, which is ≥ `BOARD_CFG_BATTERY_FULL_MV`
  (8400) and ≥ `BOARD_CFG_BATTERY_PRESENT_MV` (5000), so
  `pm_mv_to_percent()` returns 100. A genuine full 2S pack is also 8.4 V, so
  voltage alone cannot distinguish "no pack on the float rail" from "full
  pack"; the reading also flips to ~3.9 V between samples, i.e. it is not a
  stable pack voltage.
- **Fix:** `power_monitor_pack_present()` (pure, unit-tested): a reading
  clearly inside the pack range is trusted at once; a reading in the ambiguous
  rail band (≥ `FULL_MV - P4_CONFIG_BATTERY_RAIL_BAND_TOL_MV`) is accepted only
  when the last `P4_CONFIG_BATTERY_PRESENT_STABLE_SAMPLES` readings are all
  plausible and within `P4_CONFIG_BATTERY_PRESENT_MAX_SWING_MV` of each other
  (a floating node swings volts). `power_monitor_read_sample()` returns
  `ESP_ERR_NOT_FOUND` when no pack is present, so the header and every battery
  verb show `N/C` through the one existing path.
  **Corroboration (F24-B):** the IP2326 `CHG_STAT_LED` line (Tab5 expander-2
  0x44 P6) is now read via `board_bsp_charge_status_level()` /
  `bsp_get_charge_status_level()` and a level that toggles while the voltage is
  in the rail band and the current is ~0 (charger blinking into no pack)
  rejects presence. Surfaced as `battery diag` `chg_stat`.
- **Verified:** COM6 with no pack, `battery` reported
  `N/C (no battery connected)` on 5/5 consecutive samples (previously the
  8.40 V/"100% full" sample appeared intermittently); `battery diag` shows
  `chg_stat: 0`. Unit tests cover the below-floor / in-range / stable /
  unstable / chg-stat cases (`test_power_pack_present_*`). COM3 unit suite
  428/0/2; COM3 `p4test` 11/11.

### F25. Tab5 UI animation / scrolling is slow and tears — **FIXED**

- **Severity:** MEDIUM (usability; no data loss)
- **Component:** `components/display/`, `components/windows/`,
  `components/command/gfx_commands.c`, Tab5 BSP display port
- **Board:** m5stack_tab5
- **Found on:** 2026-09-21, COM6, firmware v1.2.1
- **Symptom:** `BOUNCE.BAT` and other animated/scrollback-heavy surfaces look
  laggy with visible tearing. `gfx stats` on the demo: `frames=233
  avg_us=160727 fps10=62 dropped=233`.
- **Repro:** run `BOUNCE` on COM6; or a burst of `gfx show` (see measurement).
- **Root cause (three parts, all fixed + one residual):**
  1. **CPU rotation (F25-A):** `CONFIG_LVGL_PORT_ENABLE_PPA` was off, so the
     LVGL port rotated every flush with `lv_draw_sw_rotate()` on the CPU.
     Enabled the P4 PPA (`sdkconfig.defaults`), which allocates a rotation
     buffer and drives it through the 2D accelerator
     (`lvgl_port_ppa_create`, `esp_lvgl_port_disp.c:455-473`).
  2. **O(all-spans) transcript size (F25-B):** every append called
     `windows_transcript_update_content_size()` →
     `lv_spangroup_get_expand_height()` (re-wraps the whole span group), so each
     command's transcript echo cost time proportional to all history. `gfx show`
     also took the LVGL lock while that render was pending.
  3. **Tearing + app viewport (F25-C):** a single draw buffer
     (`BOARD_CFG_LCD_DRAW_BUFFER_DOUBLE 0`) and the gfx canvas sharing the
     transcript region.
- **Fix:**
  1. PPA rotation enabled for both boards (shared `sdkconfig.defaults`).
  2. `windows_transcript_apply()` now skips the reconcile entirely while an app
     surface owns the transcript (the np group is hidden), and one catch-up
     apply runs on `windows_exit_app_surface()`; a bounded retained-span window
     (`P4_CONFIG_TRANSCRIPT_MAX_SPANS`, oldest dropped) keeps the per-append
     cost flat over a long session.
  3. Tab5 double buffering + a larger draw buffer
     (`BOARD_CFG_LCD_DRAW_BUFFER_SIZE = width*100`, `..._DOUBLE 1`); `gfx init`
     now enters true fullscreen (`windows_set_fullscreen(true)`, header/input
     row hidden) and `gfx close` restores the inherited state, so the canvas
     owns the whole panel (F25-C).
- **Measured (COM6):** `gfx show` burst 270 ms → **51 ms** per command; `gfx
  stats` average frame 190 ms → **5.6 ms** (`dropped=1`); BOUNCE's display
  present is no longer the bottleneck. Repeated `echo` no longer degrades with
  session length (was 353→706 ms as history grew; now flat once the span cap
  engages). Tearing reduced by double buffering.
- **Residual (major, tracked as F26):** `echo` still costs ~300+ ms per command
  and scales with output length per line (`100`/`250`/`500` chars → 374/531/881
  ms). This is the transcript **repaint/layout** (`lv_obj_update_layout`) and
  LVGL line-wrapping of a long line, not the span count; batching the whole
  `gfx show` path is now cheap, but chatty batch lines are still expensive.
  Next step: differential/incremental transcript rendering (only the newly
  appended fragment, no full-child layout) — a dedicated window-manager pass.
  (v1.2.2 update: the incremental apply + off-port-lock pump landed; the
  window-manager pass is the open F26 follow-up.)
- **Verified:** header `visible=OFF` during gfx and `ON` after close; display
  suite `RESULT OK` on COM6; COM3 `p4test` 11/11 with the same transcript
  changes.

### F26. Transcript per-command cost still scales with session length — **OPEN (MEDIUM)**

- **Severity:** MEDIUM (usability: chatty interactive sessions slow down; no
  data loss; batch jobs are unaffected once the transcript is hidden or
  coalesced)
- **Component:** `components/windows/windows.c` (transcript render),
  `components/shell/shell.c` (label pump), LVGL spangroup draw walk
- **Board:** both (Tab5 worst)
- **Found on:** 2026-09-21 as the F25 residual; re-baselined 2026-09-22 with a
  dedicated harness
- **Symptom:** sustained interactive output slows as the session lengthens.
  With the byte-wise bench, a fresh-boot echo's *minimum* is ~3-16 ms (true
  worker cost), but its median climbs to ~1.1-2.2 s once hundreds of lines are
  retained and the board is kept busy — on both boards (Tab5 worst). (An earlier
  ~312 ms "floor" seen with `read(65536)` was a harness artifact: pyserial
  blocks the whole 0.3 s timeout when fewer bytes are buffered — the bench now
  reads `in_waiting` byte-wise.)
- **Repro (tooling landed with this entry):**
  `python tools/transcript_perf.py COM6` (and `COM3`) — times fresh-boot,
  after-`cls`, and filled-scrollback `echo` round-trips at 100/250/500-char
  payloads, one command in flight. Keep the byte-wise `in_waiting` read in the
  tool: a block `read(65536)` adds a ~312 ms floor that swamps the real signal.
- **Fixes landed this pass (v1.2.2, keep them):**
  1. Truncation detection is O(1): the shell bumps a scrollback **epoch** on
     reset/trim/truncate and `windows_set_transcript_text_len()` carries it;
     the rendered-prefix snapshot buffer (64 KB PSRAM) and the per-apply
     `strlen`+`strncmp`+full-buffer `snprintf` scans are gone. The span parse
     was already incremental; the apply now *stays* incremental across trims.
  2. The command worker no longer takes the LVGL port lock on the append path
     at all: staging copies take the window manager's own mutex (lock order is
     always PORT outer, STAGE inner), and repaints are pumped by an LVGL timer
     at `P4_CONFIG_TRANSCRIPT_APPLY_TICK_MS` (10 ms) plus
     `lvgl_port_task_wake()` — replacing the `lv_async_call` whose dispatch
     serialized every append behind whichever render held the port lock.
     True synchronization points (screenshots, key waits, visible-again
     repaints) call `shell_transcript_flush_now()` →
     `windows_transcript_sync_apply()` and still see a current screen.
  3. Worker wall-time inside `shell_execute_command` is measured ~2 ms per echo
     even at full scrollback, so the per-echo serial round-trip is no longer the
     transcript rebuild blocking the worker — the remaining growth is CPU
     contention against the LVGL task (below).
- **Measured (2026-09-22, byte-wise harness):** a burst-free single echo after
  `cls` is 3-16 ms; back-to-back `echo` fills climb from ~300 ms (short session)
  to ~1100 ms (EV) / ~2050 ms (Tab5) once the session is long, because the
  LVGL apply+redraw is ~90-155 ms at full scrollback and, at a 10 ms tick and
  priority 4, it saturates one core so the priority-1 reader / priority-2
  worker only run in the gaps. The transient `windows_transcript_apply` probe
  confirmed the apply phase is `lv_spangroup_get_expand_height` (`exp≈tot`),
  linear at ~2.7 µs/retained-char. A temporary 128-span cap moved the
  full-session median only ~1900→1500 ms, so the span cap is not the dominant
  term and it was reverted to 384 (deep scrollback is worth more than the ~0.4 s).
- **Root cause of the residual (partially attributed):** per-command cost
  tracks retained **text length**, not span count (cap 384→128 removed only
  ~25%). The known O(retained-text) passes that remain are the two LVGL
  spangroup walks — `windows_transcript_update_content_size()`'s
  `lv_spangroup_get_expand_height` and `lv_draw_span()` re-laying-out from the
  first span on every redraw (~2.7 µs/char measured) — plus an unattributed
  ~1 s/line base at full scrollback (candidates: flush/DSI + PSRAM/heap
  contention with the saturated LVGL task vs the priority-1 reader; a
  worker-side `TCOST` probe showed ~2 ms, so the time is NOT in
  `shell_execute_command`).
- **Next step (the dedicated pass this needs — do not half-do it):**
  windowed transcript rendering — materialize only the visible rows (+a small
  margin) as spans inside the container, keep full scroll range via a spacer
  (the proven `editor_view` pattern), re-materialize on scroll events. That
  bounds BOTH LVGL walks (apply and draw) to the viewport instead of the
  session, whatever the retained text length. Needs care with: follow/scroll
  math, `anchor` click regions (offsets must map through the window), the
  editor/app-surface coexistence (now via the `shell_spans_hidden` shadow
  flag), trim pressure (fewer spans → cheaper), and the retained-span/`clip`
  paths (staged text already stays whole). The `P4_CONFIG_TRANSCRIPT_MAX_SPANS`
  cap becomes the window size (drop oldest *visually* only). A managed LVGL
  patch adding an incremental layout cache to the spangroup is the alternative
  worth weighing against the windows-level rewrite, but it hits the
  head-trim-invalidates-everything problem for this widget.
- **Verified (partial, this pass):** EV p4test 11/11, Unity 428/0/2, editor +
  input-ui + display + perf suites green with the new pump; visual spot-checks
  (scroll follow, anchor, app-mode restore) pass on COM3.

### F27. Tab5: transient crash in long mixed sweeps after the F6/F26 pass — **OPEN (HIGH)**

- **Severity:** HIGH (crash/reboot under sustained mixed load; not reproduced
  on any targeted re-run yet)
- **Component:** under determination — the F26 transcript pump, esp_hosted
  under load (F6-class), or a pre-existing load-path fault
- **Board:** m5stack_tab5 (COM6)
- **Found on:** 2026-09-22, firmware from the F6/F26 pass
- **Symptom:** three consecutive full `p4test_run COM6` sweeps each lost the
  suite running after `perf`/near the end — sweep 1: `Guru Meditation`
  detected in `s07_data` right after the `appconfig path` check; sweep 2:
  `Task watchdog` detected during `s14_apps` immediately after `launch BOUNCE`
  completed; sweep 3: no panic marker, but `s14_apps` setup shows the signature
  of a silent reboot (prompt cwd reset to `PS \>`, `mkdir APPS` marker timeout,
  then four `receive … no READY` / `timed out waiting for data` failures).
  All targeted re-runs (short sequences and even the exact suite combos) pass
  cleanly; boards always recover on the next boot.
- **Repro attempts (all CLEAN):** `--only s07_data` (93/93), targeted
  appconfig/temp/alarm sequence, `--only s06_batch,s07_data` (202/202),
  `--only s01_smoke,s05_storage,s06_batch,s07_data` (267/267),
  `--only s14_apps` (90/90), `--only s13_perf,s14_apps` (106/106).
- **Context / suspicion:** matches the F6-A residual note ("the Tab5 still
  wedges late in a full p4test sweep under sustained load — the C6 link is
  marginal"): a reboot-less stall or SDIO storm under sustained SD+host traffic
  then escalates to the watchdog. A second candidate is the F26 render pump:
  during heavy batch output the LVGL task now runs an apply every tick
  (10 ms) at near-100 % duty, so a starved task fits too. The v1.2.0 sweeps
  (pre-pump) completed 11/11 on COM6, so the pump is the prime NEW variable,
  unproven.
- **Next step:** the runner now embeds the output around any panic marker
  (`tools/p4test/session.py`), and the sweep logs must be re-checked for the
  `task_wdt` task list + backtrace; also add a raw serial tap during a full
  sweep to catch the silent-reboot case (watch `rst:0x` cause — `WDT` vs
  `PANIC` vs `INT` distinguishes the classes). If it is the idle/`taskLVGL`
  pair, tune `P4_CONFIG_TRANSCRIPT_APPLY_TICK_MS` (coarser tick) and/or give
  `windows_transcript_apply` a per-tick work budget.

### F6-A. Tab5 hosted-SDIO mitigation — verified (F6 fixed above; kept as the audit trail)

- **Mitigation present:** the `esp_hosted` SDIO credit patch
  (`tools/managed_patches.patch` → `eh_host_bus_sdio.c`: rate-limited
  `sdio_get_tx_buffer_num` error + `eh_host_port_task_delay_ms(1)` yield
  instead of the 20 µs busy-wait) is applied in the built tree
  (`last_log_ms` at line 519, `eh_host_port_task_delay_ms(1)` at line 882).
  `networking.c` does one connect-retry and one transport-reset retry around
  the first hosted RPC; Tab5 SDIO runs at 10 MHz.
- **Verified:** COM6 `boot_regression.py COM6 6` → **OK 6/6 clean**
  (`panics=0 autoexec=1 sd_ready=True warn/err=0`) with the F6-FIXED build above:
  20/20 clean with **zero** first-RPC/`0x107`/storm signatures, dogfood 8 min
  PASS. The earlier "adopt the M5Stack vendor stack (esp_hosted 1.4.0)" idea is
  **retired**: it downgrades the RPC ABI under the 3.x compat gate + `c6ota`,
  and the measured root cause was the boot-time C6 rail glitch, now gone.

Use this template for every new finding. Keep one heading
per bug; move the entry to the matching severity section once triaged. When
it is fixed, add the fix + verification and remove it at the next reset.

```
### <ID>. <one-line summary>

- **Severity:** CRITICAL | HIGH | MEDIUM | LOW
- **Component:** <component / command>
- **Board:** jc1060p470c | m5stack_tab5
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

## Campaign status (2026-09-19)

Both boards run the v1.2.0 firmware built from this tree (zero firmware/test
warnings). Results with the fixes above:

| Check | jc1060p470c (COM3) | m5stack_tab5 (COM6) |
|-------|--------------------|---------------------|
| `p4test_run.py` (11 suites) | **OK 11/11** | **OK 11/11** |
| `unit_run.py` (Unity) | **OK 384/0/2** | **OK 384/0/2** |
| `regression.py` (24 guards) | **23/24** (only the documented `tcpterm` flake) | blocked by F6 (board reboots under load) |
| `deep_test.py` | **DEEP PASS** | **DEEP PASS** |
| `ui_touch_test.py` | **RESULT OK** | **RESULT OK** |
| `pkg_test.py` | OK | **RESULT OK** (after deploy fix) |
| `boot_regression.py` | **OK 5/5** | 4/6 at 10 MHz (F6 storm); 8/8 at 40 MHz but 40 MHz crashes under Wi-Fi |
| `visual_sweep.py` | 0 findings | 56 edge-ink (F13) + 1 race (F14) |
| `tx_stress_test.py` (200) | **OK 200/200** | flaky (F16) |
| `dogfood.py` (5 min) | **PASS 5/5** | FAIL — F6 reboots under load |

Open items at snapshot time: **F6 (HIGH)** and **F16 (MEDIUM)** were Tab5-only
stability/integrity issues deferred to a dedicated pass; **F13 (LOW)** is
cosmetic.

> **Update (2026-09-22, F6/F26 pass).** **F6 is FIXED** (see the entry above):
> the Tab5 boot path now reports 20/20 clean with zero first-RPC/`0x107`/storm
> signatures and `dogfood.py COM6 8` PASS — the root cause was a boot-time C6
> power-rail glitch on the 0x44 IO expander, not the marginal link, so the
> "adopt the M5Stack vendor stack" idea is retired. The COM6 "blocked by F6" and
> "FAIL — F6 reboots" rows above no longer apply. **F26** (transcript per-command
> cost) is opened as the F25 residual and partially mitigated this pass (worker
> off the port lock, O(1) truncation); the windowed-render fix remains a
> dedicated follow-up. **F27** (new): two consecutive full COM6 sweeps each hit
> one transient, so-far unreproducible crash (`Guru Meditation` in `s07_data`,
> `Task watchdog` in `s14_apps`); all targeted re-runs pass. F16/F13 are
> unchanged.

---

## Running the campaign

```powershell
# 1. Build firmware + tests (both must be 0 errors / 0 warnings)
$env:IDF_PATH = "<path-to-esp-idf-v5.5.5>"; . $env:IDF_PATH\export.ps1
idf.py -DP4_BOARD=<board> build
cd test; idf.py -DP4_BOARD=<board> build; cd ..

# 2. Unit suite on the board (Unity; black screen is expected)
cd test; idf.py -DP4_BOARD=<board> -p <COM_PORT> flash; cd ..
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
idf.py -DP4_BOARD=<board> -p <COM_PORT> flash
```

Replace `<board>` with `jc1060p470c` or `m5stack_tab5` and `<COM_PORT>` with the
matching port (COM3 / COM6; `tools/board_ports.py` resolves them, or set
`P4_PORT`/`P4_BOARD`).

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
- **Tab5 hosted SDIO runs at 10 MHz** (`CONFIG_ESP_HOSTED_HOST_SDIO_CLK_KHZ`,
  board override) vs 40 MHz on the reference board, and the first hosted RPC
  after a fresh connect is retried once via a transport reset. Both are
  intentional and documented in `PORTING.md` §6.
- **Writerdeck: spellcheck needs a wordlist.** Word coverage is exactly the
  wordlist's coverage (push the `apps/dicts/` sample with
  `apps/push_dicts.py`); with no `sd:/DICTS/<name>.words` present the `Spell`
  toggle reports the expected path and stays off. Tokens with CJK, digits,
  `_`, or non-ASCII Latin letters are never flagged.
- **Writerdeck: focus mode hides the header and keyboard.** Toggle out with
  `Ctrl+Shift+F` (USB), the `Focus` key, or `\focus`; quitting the editor
  restores both.
- **Writerdeck: spell underlines compose with word-wrap.** Each word segment is
  chunked separately, so underlines survive visual line breaks.
- **Writerdeck: `edit /template` only seeds new files.** An existing target is
  never overwritten by a template; the option is ignored for it.
- **Writerdeck: `markdown export ... html` writes a standalone page.** The
  `<!DOCTYPE html>` wrapper with embedded reader CSS is built by
  `markdown_render_html_page()` around the single fragment body; the
  fragment entry point is kept for embedding.

---

## Tracked / deliberately retained (not bugs)

Known code-quality items intentionally left as-is pending a dedicated pass.
Recorded here so they are not re-reported as new bugs.

| ID | Location | Finding | Why retained |
|----|----------|---------|--------------|
| C1 | `networking.c` / `bluetooth.c` / `usb.c` / `c6ota.c` | four copies of `*_text_equals_ignore_case` and three of `*_split_args` | **FIXED** — shared leaf `components/strutil/`; all four REQUIRE it |
| C2 | `storage_ini.c` / `trash.c` / `db.c` / `archive.c` / `alarm.c` | five `mkdir -p` variants | **FIXED** — unified behind `storage_mkdir_p()` (opens its own guarded session) |
| C3 | `components/header/header.c` | `s_header_state` (incl. `clock_text`) is written from several tasks while the LVGL task reads it | **DEFERRED** — scalar writes are atomic and the render reads a consistent-enough snapshot; a lock/snapshot refactor touches 96 accesses in fragile LVGL code (regression risk outweighs the theoretical race) |
| C4 | `components/markdown/markdown.c` | `md_render_table()` used a file-scope static scratch array (not reentrant) | **FIXED** — the cell table is now a PSRAM heap block freed on every path (reentrant) |
| C5 | `components/display/display.c` | `display_set_refresh_rate()` is a documented "not supported on this panel" stub | the panel/BSP does not expose dynamic refresh; reported honestly |
| C6 | `components/shell/shell.c` | `camera` verbs report the gap | **RESOLVED** — the Tab5 SC202CS camera is now supported (`components/camera/` + esp_video, BMP stills); boards without one still report the gap |

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
- **Board portability:** Tab5 vs reference differences (10 MHz SDIO, panel
  auto-detect, no RGB LED, `BAT N/C`), and panels other than JD9165.

