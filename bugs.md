# P4MiniShell — Bug Report & Test Campaign

Date: 2026-08-15
Hardware: ESP32-P4 (rev 1.0) on COM11, ESP32-C6 co-processor, JD9165 display, SD card present
Firmware: **v0.32.8** (memory-hardening sweep: cached SDMMC DMA buffer, transcript scrollback
          auto-trim under internal-heap pressure, display rotation thread-safety lock, reduced
          transcript size; supersedes v0.32.2 batch process abstraction)
Scope: Debug sweep and stress testing over the UART console (USB-Serial-JTAG, 115200 baud),
unit suite, and on-board stress runs. Bugs found are ordered by severity; **FIXED** entries
describe the change and its verification.

The v0.31.0 campaign recorded the findings below. The v0.32.0 campaign (calc/for /f/set /p
work) was a development-time fix loop rather than a post-release sweep: the issues found
during bring-up — the calc parser's leading-unary/precedence handling, `&H` hex digit
parsing, `SINH`/`COSH`/`TANH` being shadowed by the `SIN`/`COS`/`TAN` prefix match, DMS$
seconds padding, and the `for /f` `tokens=` append-vs-replace default — were all fixed
before release and are covered by the new unit suites (139 tests total, 0 failures). No
open findings from the v0.32.0 build.

The v0.32.1 batch-file verification (running `for /f`, `calc`, and `set /p < file` from
real `.bat` files on the UART console) found and fixed the two MEDIUM bugs recorded below
(M1, M2); the on-board unit suite is 151 tests, 0 failures.

The v0.32.8 memory-hardening sweep re-ran the full command list twice, the batch verbs
and every batch file on the SD twice, the applib/appconfig/appmode paths, and the 158-test
unit suite. It found and fixed three MEDIUM bugs (M5–M7 below) plus a thread-safety hazard
in `display_set_rotation`; the SD card was also cleaned of all prior test artifacts
(leaving only CONFIG.SYS, AUTOEXEC.BAT, WIFI.KNOWN, and the C6 slave image).

---

## CRITICAL

*(none so far)*

---

## HIGH

*(none so far)*

---

## MEDIUM

### M1. ✅ FIXED — `calc NAME=<expr>` failed from batch files and the prompt (0.32.1)

- **Symptom:** running `calc y = 6 * 7` (in a `.bat` or at the prompt)
  reported `calc: invalid variable name or environment is full` and never
  stored the variable.
- **Root cause:** `shell_command_calc_line()` copied the *whole*
  `NAME = expr` text into the variable name before calling
  `shell_env_set()`, which rejects names containing spaces — so the name was
  `"y = 6 * 7"` instead of `"y"`.
- **Fix:** extract only the bytes before the top-level `=` and trim them.
- **Verified:** `calc y = 6 * 7` in a batch file sets `y=42` on board;
  regression test `test_calc_command_assignment` (shell suite, 0 failures).

### M2. ✅ FIXED — `for /f` silently did nothing (interactive and in batch) (0.32.1)

- **Symptom:** `for /f "delims=," %%a in (data.txt) do echo ITEM=%%a` printed
  nothing — no error, no iterations — both at the prompt and from a `.bat`.
- **Root cause:** `shell_execute_for_loop()` parsed the `/f` options but never
  advanced the cursor past them, so the loop-variable check
  `if (*for_ptr != '%')` always failed and the function returned without
  iterating.
- **Fix:** advance `for_ptr` to the loop variable after the options region.
- **Verified:** on-board batch check prints `ITEM=apple`; interactive
  `tokens=2` → `banana`, `tokens=1,*` → `K=apple REST=banana,cherry`,
  `skip=1` → nothing. The option parser / line splitter unit suites still pass.

### M3. ✅ FIXED — `cmd1 | cmd2 > out.txt` wrote an empty file (0.32.2)

- **Symptom:** `echo hello | sort > out.txt` produced an empty `out.txt`
  (and `>>` nesting lost content), so a redirected pipeline lost its output.
- **Root cause:** the output-redirection capture was a single global buffer.
  Each pipe stage's own `> _pipeN.tmp` spool opened a nested capture; its
  `shell_redirect_capture_reset()` freed the buffer the outer capture
  depended on, so the outer write saw NULL → empty file.
- **Fix:** the capture is now a stack (`P4_CONFIG_REDIRECT_CAPTURE_MAX_DEPTH`
  = 8). A nested `capture_begin()` pushes the outer state and starts fresh;
  the inner `capture_reset()` writes its own spool, pops the outer state, and
  resumes recording for it.
- **Verified:** `echo hello | sort > out.txt` → `type out.txt` = `hello`;
  `echo z | findstr z >> out2.txt` twice → `z` / `zz` on board. Unit test
  `test_redirect_capture_nested` (153/153 suite passes).

### M4. ✅ FIXED — `%ERRORLEVEL%` did not expand (0.32.2)

- **Symptom:** `echo code=%errorlevel%` printed the literal `%errorlevel%`
  both at the prompt and in batch files, so a batch process could not read
  its own exit code.
- **Root cause:** `shell_expand_variables()` read the `%VAR%` token buffer
  (`shell_text_equals_ignore_case(token, "ERRORLEVEL")`) before `memcpy`
  filled it, so the comparison ran on uninitialized memory and always failed.
- **Fix:** fill the token buffer first, then test for `ERRORLEVEL` before the
  environment lookup.
- **Verified:** `findstr nope data.txt` → `code=1`; `findstr banana` →
  `ok=0`; nested `call p2.bat` with `exit /b 5` → `P1 code=5` on board.
  Unit test `test_variable_expansion_errorlevel` (153/153 suite passes).

### M5. ✅ FIXED — SD card operations intermittently failed with `allocate_dma_buf: not enough mem` (0.32.8)

- **Symptom:** after roughly 40 seconds of uptime (and more so as the session
  accumulated output), every SD command (`dir`, `type`, `write`, `copy`,
  pipes/redirects, `tree`, `chkdsk`, …) started failing with:
  ```
  E (43555) sdmmc_cmd: allocate_dma_buf: not enough mem, err=0x101
  E (43556) diskio_sdmmc: sdmmc_read_blocks failed (0x101)
  ```
  Once triggered it persisted, and the degraded memory state cascaded into
  other failures (`volume` panicked with a Load-access fault, `move` aborted,
  `pwd`/`dir` refused to run) in the long sweep.
- **Root cause:** the IDF sdmmc driver allocates a temporary DMA-capable
  buffer for every multi-block card transaction when the host has no cached
  buffer (`card->host.dma_aligned_buffer == NULL`). On the P4,
  `MALLOC_CAP_DMA` resolves to the internal SRAM heap only (PSRAM does not
  carry the DMA cap in this build), which is also shared with the
  WiFi/SDIO transport and LVGL. Once the internal heap fragments under load,
  the per-transaction allocation (4 KB minimum) fails and takes every SD
  command down with it.
- **Fix:** pre-allocate a cached DMA scratch buffer on the SDMMC host at mount
  time (when internal RAM is abundant) and set
  `unaligned_multi_block_rw_max_chunk_size` from it, so every later
  transaction reuses the buffer and can never fail on memory. New tunable
  `P4_CONFIG_SD_DMA_BUFFER_BYTES` (8192 = 16 × 512 B sectors); released on
  `sdeject`.
- **Verified:** 50+ mixed SD operations (dir/type/write/copy/pipes/tree/
  chkdsk) twice with **0** `allocate_dma_buf` errors; previously every SD op
  failed after ~40 s. The previously-crashing `volume`/`move`/`ren` commands
  run clean under low internal RAM.

### M6. ✅ FIXED — `abort()` in newlib `lock_init_generic` (stdio FILE lock OOM) under long sessions (0.32.8)

- **Symptom:** intermittently (in a long multi-command sweep) a command aborted
  with `abort() was called at PC 0x4ff020f1` on core 0/1 and a backtrace
  `… lock_init_generic ← __sbprintf` (newlib stdio). Different commands
  (`brightness`, `dir /b`, `fc`, `more`, `write`, `sort`) crashed at random
  points once the session had accumulated a lot of output.
- **Root cause:** the on-screen transcript renders the coloured scrollback as
  LVGL span objects whose per-span struct/style/text overhead lives in the
  internal heap (LVGL uses CLIB malloc, and allocations ≤16 KB prefer internal
  RAM). Over a long session the accumulated spans exhaust/fragment the
  internal heap until a tiny stdio allocation — a newlib `FILE` lock mutex
  created by the first `printf` to go through `__sbprintf` — fails
  (`xQueueCreateMutex` returns NULL) and `lock_init_generic` aborts.
- **Fix (3 parts):**
  1. **Auto-trim the transcript scrollback under memory pressure.** New
     `shell_transcript_guard_internal()` (called at the start of every
     command execution AND before every transcript append, under the LVGL
     lock) drops the oldest half of the scrollback and frees the spans
     synchronously whenever free internal RAM falls below
     `P4_CONFIG_TRANSCRIPT_INTERNAL_TRIM_BYTES` (49152), keeping the internal
     heap above the failure floor. `windows_transcript_trim()` performs the
     span teardown and staging-buffer truncation.
  2. **Halve the span ceiling.** `P4_CONFIG_TRANSCRIPT_BYTES` reduced 16384 →
     8192, halving the maximum span-group memory footprint (still ~80 lines of
     history).
- **Verified:** the full command sweep (105 commands × 2 passes) and the batch
  sweep (18 verbs + 27 batch files × 2 passes) complete with **0** crashes
  (previously 6 abort crashes per sweep); internal RAM stays ≥ ~65 KB instead
  of collapsing to ~1 KB. 158-test unit suite still passes.

### M7. ✅ FIXED — `display_set_rotation()` called LVGL APIs from the shell task without the port lock (0.32.8)

- **Symptom:** (no reproducible crash, but a real thread-safety hazard) the
  `rotate` command ran `lv_display_set_rotation()` and the synchronous
  `LV_EVENT_RESOLUTION_CHANGED` callback from the UART/worker task while the
  LVGL render task could be mid-frame, racing LVGL's widget tree.
- **Root cause:** `display_set_rotation` did not hold `lvgl_port_lock` around
  the LVGL access, unlike every other shell-path LVGL caller in the codebase.
  (Note: an earlier "reboot on rotate" report was a false alarm — the
  `P4MiniShell ready` banner and keyboard-audit line are legitimately
  re-printed by the UI rebuild, not by an actual reset.)
- **Fix:** wrap the `lv_display_set_rotation` + touch-rotation + async-rebuild
  scheduling in `lvgl_port_lock(0)` / `lvgl_port_unlock()` (the port mutex is
  recursive, so a call from the LVGL task itself stays safe).
- **Verified:** `rotate 90/180/270` (then back to 0) work cleanly over the
  UART console with no reset; the full sweep includes these without incident.

---

## LOW / OBSERVATIONS

- **`tone` first-use logs a benign codec error.** The first `tone 440 200`
  after boot prints `E i2s_common: i2s_channel_disable: the channel has not
  been enabled yet` while the tone still plays. It comes from the managed
  codec-dev/i2s driver's open path, is harmless, and disappears after the
  first use. No fix needed.
- **`pwd` is not a command** (it is not in the dispatcher); it correctly
  returns `Unknown command`. The v0.32.8 sweep initially included `pwd` as a
  test and those "errors" were the expected response, not a bug.
- **The transcript span group holds a bounded internal-RAM footprint.**
  Rendering the coloured scrollback as LVGL spans carries per-span overhead
  in the internal heap; it grows with accumulated history, stabilises at the
  8 KB scrollback ceiling, and is reclaimed by `cls` (and automatically by
  the M6 trim). This is a design cost of coloured scrollback, not a leak.

---

## NETWORK

### N1. ✅ FIXED — concurrent non-thread-safe `sdmmc_host_init()` wedges the shared SDMMC host at boot

- **Symptoms:** after a `reboot` (or any soft reset), the board occasionally
  failed to bring up BOTH the SD card and the ESP-Hosted C6 on the shared SDMMC
  bus. Boot log showed:
  ```
  E sdmmc_req: sdmmc_host_wait_for_event returned 0x107
  E sdmmc_sd: sdmmc_init_sd_scr: send_scr (1) returned 0x106
  E vfs_fat_sdmmc: sdmmc_card_init failed (0x106).
  No SD card detected - insert a microSD card ...
  E sdmmc_io: sdmmc_io_rw_extended: sdmmc_send_cmd returned 0xffffffff
  E eh_sdio: sdio_read_task: Failed to read data - -1
  [wifi] esp_hosted_connect_to_slave() failed: ERROR (0xffffff8c)
  ```
  Wi-Fi runtime never started that boot. Frequency ~1 in 15 boots; a second
  reboot recovered.
- **Root cause:** the SDMMC host driver is shared between the SD card (slot 0)
  and the ESP-Hosted C6 transport (slot 1). At boot both initialize it from
  different tasks:
  - SD mount: `bsp_sdcard_mount()` → `esp_vfs_fat_sdmmc_mount()` → `host.init()` = `sdmmc_host_init()`
  - Hosted transport: `networking_wifi_runtime_init()` → `esp_hosted_init()` →
    `eh_host_port_sdio_init()` → `sdmmc_host_init()`
  `sdmmc_host_init()` (esp_driver_sdmmc/src/sdmmc_host.c:496) is **not
  thread-safe**: it checks/sets the global `s_host_ctx.intr_handle` without a
  lock, so a concurrent double-call corrupts the host driver state →
  `sdmmc_host_wait_for_event` times out (0x107) and both slots fail.
  Secondary cascade: `esp_vfs_fat_sdmmc_mount` calls whole-host
  `sdmmc_host_deinit()` on any SD mount failure, tearing down the esp_hosted
  slot 1 link even if only the card failed.
- **Fix (3 parts):**
  1. **Serialize host init** — new `storage_sdmmc_host_preinit()` calls
     `sdmmc_host_init()` once, synchronously, from `app_main` before
     `networking_init()`; every later call from fatfs and esp_hosted hits the
     driver's idempotent "already initialized, skip" branch (`sdmmc_host.c:498`)
     so the race can never occur.
  2. **Slot-scoped SD deinit** — patched `bsp_sdcard_mount()` (managed BSP
     `esp32_p4_function_ev_board`) to use a slot-0-only deinit wrapper
     (`sdmmc_host_deinit_slot(0)`) instead of whole-host `sdmmc_host_deinit()`,
     so a failed SD mount / eject only releases slot 0 and never breaks the
     co-processor link (slot 1).
  3. **Hosted bring-up retry** — in `networking_wifi_runtime_init`, if
     `esp_hosted_connect_to_slave()` fails, tear down (`esp_hosted_deinit()`,
     slot-scoped) and retry once after 50 ms.
- **Verified:** 30/30 repeated `reboot` → `wifi connect 4G-CPE_5542
  1234567890` cycles associated and got IP with **zero** hosted-link failures
  (previously ~1/15) and the SD card enumerated every boot. The hosted retry
  never fired (primary fix holds). SD `sdeject` → `wifi status` stays
  "started" (hosted link survives the slot-0 deinit), and `sd mount` re-mounts
  cleanly. Both firmware and test projects build with 0 errors / 0 warnings.

---

## Summary

| Severity | Count | Status |
|----------|-------|--------|
| Critical | 0 | — |
| High | 0 | — |
| Medium | 7 (M1 calc `NAME=` name extraction, M2 `for /f` driver cursor, M3 redirect-capture re-entrancy, M4 `%ERRORLEVEL%` token init, M5 SDMMC DMA buffer allocation failures, M6 newlib FILE-lock OOM abort, M7 display-rotation LVGL thread-safety) | ✅ Fixed (M1–M4 in v0.32.1/v0.32.2, M5–M7 in v0.32.8, all verified on board) |
| Low / observations | 3 (benign first-use i2s log, `pwd` not a command, transcript span internal-RAM footprint) | — |
| Network | 1 (N1 shared-SDMMC bring-up) | ✅ Fixed (serialize sdmmc_host_init + slot-scoped SD deinit + hosted retry) |
