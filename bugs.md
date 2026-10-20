# P4MiniShell — Bug Log (v1.4.0)

This file is the working log for bug hunting and hardware stress testing. It
was reset for the v1.2.0 campaign (the two-board release) and carried into
v1.3.0, so it reflects what is **still open**.

Fixed entries are removed at each reset (their history lives in
[`changelog.md`](changelog.md) and git).

- **Firmware:** v1.4.0 (ESP-IDF v5.5.5)
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

---

## Fixed findings (v1.2.0 two-board campaign)

All fixed. One-line summaries; full details live in `changelog.md` and git.

| ID | Summary | Board |
|----|---------|-------|
| F1 | `about.display` hardcoded reference panel on Tab5 | Tab5 |
| F2 | Synthetic touch taps hit mirror key on rotated display | Tab5 |
| F3 | `board_ports.py` never resolved board id (harness) | both |
| F4 | Suites assumed reference board's panel/name (harness) | Tab5 |
| F5 | `power-audio` required RGB LED (harness) | Tab5 |
| F6 | Tab5 intermittent hosted-SDIO TX storm → task-WDT reboot | Tab5 |
| F7 | `receive` CRC failure during batch suite (harness) | Tab5 |
| F8 | Tab5 BSP warned SD long filenames disabled (misleading) | Tab5 |
| F9 | `deep_test.py` never reset the board (harness) | both |
| F10 | `regression.py` did not deploy package bundles (harness) | Tab5 |
| F11 | `ui_touch_test.py` hardcoded reference input-row band (harness) | Tab5 |
| F12 | Recovered first-RPC read logged as Wi-Fi warning | Tab5 |
| F13 | Tab5 shell ink reaches panel edges — deferred (cosmetic) | Tab5 |
| F14 | `visual_sweep` capture raced a log line (harness) | Tab5 |
| F15 | `visual_sweep` shared one output dir across boards (harness) | both |
| F16 | Tab5 drops lines/truncates frames under TX backpressure | Tab5 |
| F17 | Tab5 boot loop: NVS write from lwIP thread on SNTP sync | Tab5 |
| F18 | Tab5 reported bogus `charging` state | Tab5 |
| F19 | Tab5 camera first init failed / near-black frames | Tab5 |
| F20 | Tab5 battery never charged | Tab5 |
| F21 | Header Wi-Fi indicator stuck red | both |
| F22 | On-screen keyboard only hid for USB keyboards | both |
| F23 | Tab5 `crypt` fails after busy session (AES DMA descriptors) | Tab5 |
| F24 | Tab5 reports 100% "full" battery with no pack attached | Tab5 |
| F25 | Tab5 UI animation/scrolling slow and tears | Tab5 |
| F26 | Transcript per-command cost scales with session length | both |

---

## Fixed findings (v1.4.0 campaign)

### F45. Browser pages drew bottom-up and leaked `<head>` text - **FIXED, verified on hardware**

`browser_render_dom` pushed siblings first-to-last on a LIFO stack, so every
document rendered in reverse document order (the text-presence tests could not
see it). Separately, `dom_parse` only looked for `head`/`body` among the root's
direct children; a normal `<html><head>…</head><body>…</body></html>` left
`tree->body` NULL, so the renderer walked the root and printed `<title>` and
raw `<style>` CSS as page text. Siblings now push `last_child` backwards and
head/body are resolved with `dom_find_first_by_tag`. Verified by
`docs/assets/browser-website.png` (`s20_website`).

### F46. Browser CSS ruleset + DOM recursion forced a 32 KB worker - **FIXED, verified on hardware**

`browser_render_html` kept `css_ruleset_t` (64 x 448 B = ~28 KB) on the worker
stack; 32 KB then starved the boot-time SDMMC/C6 DMA allocations. The ruleset
now lives in PSRAM, DOM parse depth is capped at 64, and the worker is 28 KB.
The remaining internal budget required the SD history save to move off the
`esp_timer` task (4 KB timer stack) and a 10 KB Wi-Fi init task.

### F47. `httpget` freed an uninitialized result on early errors - **FIXED, verified on hardware**

`networking_http_request_internal` zeroed `result` after its argument checks,
so an early return (e.g. `extra_headers != NULL` with count 0) left the caller's
`networking_http_result_free()` freeing garbage (CORRUPT HEAP). Zeroing moved
before every return; `httpget` also initializes the struct and passes NULL when
there are no extra headers.

### F48. `js eval/run/call` used the subcommand as code/filename - **FIXED, verified on hardware**

The helpers indexed argv from 1 while the dispatcher passed the full argv, so
`js run file.js` opened a file named `run` and `js eval code` prefixed `eval`.
Dispatch now narrows argv. The QuickJS runtime is created on the main task and
used on the worker, so `JS_UpdateStackTop()` re-anchors the stack check before
each eval; script paths resolve through the shell path rules and an undefined
result no longer prints `undefined`.

### F49. Second I2S tone faulted when the DMA heap was fragmented - **FIXED, verified on hardware**

The managed codec reconfigures the I2S slot on every open, freeing and
reallocating the DMA descriptors; on a fragmented heap the allocation failed,
IDF left a NULL descriptor and the following enable load-faulted. Fixes:
skip an identical format reconfig and keep the cached format across close;
refuse playback with a clear message when the DMA heap is too small; tear down
both I2S channels after a failed BSP init. Managed patches are tracked in
`tools/managed_patches.patch`.

### F50. `del` stalled for tens of seconds on a nearly-full trash - **FIXED, verified on hardware**

`storage_trash_enforce_limits` deleted one victim per full directory walk and
re-read every `.meta`; a bin near the 7-day age limit made one `del` ~20 s.
Enforcement is now one `readdir`+`stat` pass, sorts by the epoch in the entry
name, and deletes the computed victims in one batch.

### F51. TUI-page screenshots showed the transcript - **FIXED, verified on hardware**

`screenshot` printed "capturing…" and flushed the transcript before taking the
snapshot; both repaint the transcript over a TUI page. The command now prints
nothing before the snapshot and skips the flush while `tui_is_active()`.

### F52. Deep batch apps overflowed the 28 KB worker - **FIXED, verified on hardware**

MOOD's `while … do call` sampler kept the while frame for the whole tick and
overflowed by ~200 B; it now uses a `goto` loop. The APPLIB probe is invoked
with `if "%P4_APP_FEATURES%"=="1" gosub …` so it costs no batch frame when
disabled. `s14_apps` is 187/187 on both boards.

## Open findings

### F53. Long-sweep heap exhaustion / editor unnamed-buffer fault (v1.4.0) - **OPEN**

- **Severity:** HIGH (release gate; not a data-loss bug, reproduced only under
  long mixed sweeps / the unnamed editor path)
- **Components:** `components/command` (worker heap), `components/batch/svc.c`
  (crash-restart supervisor), `components/editor` (unnamed buffer)
- **Boards:** Tab5 (COM6) for the heap abort; COM3 and COM6 for the editor
  fault (COM6 blocked earlier by the heap abort).
- **Symptom 1 (Tab5):** `s06_batch`, after "crash restarts nonzero", aborts in
  newlib `__sfp` (`abort() was called ... 0x40232baa`) while the crashing
  service is being stopped/removed. The service restarts in a tight loop and
  the internal heap drops to ~1.7 KB; a `fopen` for the service/history INI
  then cannot allocate a `FILE`.
- **Symptom 2 (both boards):** `s09_editor` opens the first file, then
  `edit` (unnamed) faults with `MEPC 0xcececece` (instruction access fault;
  a poisoned/corrupted function pointer) or leaves the editor open and the
  following `type _S09C.TXT` marker times out. Standalone repro:
  `python tools/p4test_run.py COM3 --only s09_editor`.
- **Mitigations already in v1.4.0:** `ps` snapshots and `ftype` launch tables
  now allocate in PSRAM; httpd auto-start requires 16 KB of internal heap and
  skips itself on these boards (explicit `httpd start` is unaffected); the
  service test stops/removes the crash service immediately; editor save-as
  delays were widened.
- **Next experiment:** free the remaining internal-RAM consumers (autostart
  httpd already gated; consider stopping the C6 RPC tasks after disconnect),
  then bisect `edit`'s unnamed path (a `0xcececece` jump usually means a
  struct function pointer was freed while the editor kept the pointer; check
  the PSRAM document/buffer lifetime around `edit` with no path).
- **Release note:** treat the Tab5 long-sweep (s06 crash-restart section) and
  the unnamed-editor path as known issues; all individual feature suites and
  the boot/unit gates are green.

### F43. Firmware "brick"/boot-loop: application BSS relocated to PSRAM by a local ESP-IDF patch — **FIXED, verified on hardware**

- **Severity:** CRITICAL (every board reset-looped after flashing; the USB-Serial/JTAG
  console re-enumerated instead of booting; boards had to be kept in download mode to
  stay connected)
- **Component:** build — the local ESP-IDF tree
  (`components/esp_system/ld/ld.dram.sections`), plus a redundant, inert
  `components/event_bus/linker.lf`
- **Board:** `jc1060p470c` (COM3), `m5stack_tab5` (COM6)
- **Found on:** 2026-10-19, recovery pass after a v1.3.2 feature round
- **Symptom:** after flashing, the board never reached the shell. A hard reset dropped
  the chip into download mode (`rst:0x17 (CHIP_USB_UART_RESET)`,
  `boot:0x206 (DOWNLOAD(USB/UART0/SPI))`, `waiting for download`); with the port open
  the console produced no app output. On the console-less path the USB device
  re-enumerated repeatedly ("connect/disconnect").
- **Root cause:** to work around an internal-RAM BSS overflow, the `.dram1.bss` block in
  the local ESP-IDF linker script was rewritten to
  `*(.bss .bss.* … .sbss …) > ext_ram_seg`, moving **all** application BSS (284 KB) from
  internal `dram_high_seg` (0x4FF40000) to PSRAM `ext_ram_seg` (0x48000000). The linked
  ELF then carried a `LOAD 0x48000000 filesz 0 memsz 0x45588` segment — but a NOLOAD
  segment is not emitted into the app image, so the 2nd-stage bootloader saw no PSRAM
  load target and never brought PSRAM up. `call_start_cpu0` then faulted while zeroing
  `_bss_start_high`. Placing `.sbss` in PSRAM is also invalid (gp window). A second copy
  of the same idea lived in `components/event_bus/linker.lf`, which is **inert**: ESP-IDF
  only applies a component fragment declared via `LDFRAGMENTS` in `idf_component_register`.
- **Fix:**
  - Restored `components/esp_system/ld/ld.dram.sections` to the pristine ESP-IDF v5.5.5
    content (`.dram1.bss > dram_high_seg` with `mapping[dram0_bss] mutable[dram0_bss]`).
  - Deleted the inert `components/event_bus/linker.lf`.
  - Kept every feature, but moved the new fixed-capacity tables out of `.bss` into
    lazily-allocated PSRAM (`p4heap_calloc_any()`, PSRAM-first with internal fallback):
    the event-bus topic/subscriber pools, the state-machine pool, the HTTP cookie jar,
    and the browser history/bookmark tables.
- **Verification:** firmware **and** `test/` build 0 errors / 0 warnings on both boards;
  COM3 boots `boot:0x20f (SPI_FAST_FLASH_BOOT)`, SD ready, AUTOEXEC runs, Wi-Fi starts,
  and the boot log carries no `E (`/`W (` lines; internal BSS is now
  `.dram0.bss` 26,640 B + `.dram1.bss` 228,240 B (~34 KB `sram_high` headroom) instead of
  284 KB in PSRAM.

### F44. Intermittent boot Wi-Fi/SDIO failure from the async boot-script task — **FIXED, verified on hardware**

- **Severity:** HIGH (Wi-Fi and the hosted C6 link silently failed to start on ~10–30 % of
  boots; `E wifi: Failed to start Wi-Fi background task` and a storm of
  `E sdmmc_common: sdmmc_allocate_aligned_buf: not enough mem` /
  `eh_host_port_sdio: sdmmc_card_init failed`)
- **Component:** `components/boot` (async boot-script task) vs `components/networking`
  (Wi-Fi/C6 bring-up), root-caused with a temporary boot-RAM probe in `main.c`
- **Board:** `jc1060p470c` (COM3), `m5stack_tab5` (COM6)
- **Found on:** 2026-10-19, boot-RAM pass after F43
- **Symptom:** most boots were clean, but some failed the Wi-Fi background task creation
  (`xTaskCreate` of a 12 KB internal stack) and/or the hosted-C6 SDIO card init
  (`sdmmc_allocate_aligned_buf: not enough mem`). `boot_regression` was 7–9/10.
- **Root cause:** `boot_on_sd_first_mount` runs CONFIG.SYS/AUTOEXEC.BAT on a separate
  `bootscript` task with a **16 KB internal stack**, so a slow script never blocks the
  mounting task. That stack is reclaimed by the idle task only after the task
  self-deletes — which can overlap `networking_init()`. The probe showed internal free
  dropping from 28,008 B (after `usb_init`) to 8,440 B (inside `boot_run_startup`) and back
  to 26,332 B (before `networking_init`) asynchronously; when the script ran long, the
    12 KB Wi-Fi task and the C6 SDIO DMA scratch buffer could not be allocated.
- **Fix:**
  - `boot_wait_script_idle()` (`components/boot/boot.c`, declared in `boot.h`): a bounded
    wait for the boot-script task to finish, called from `main.c` right after
    `boot_run_startup()` and before `networking_init()`, so the C6 bring-up starts on a
    settled heap.
  - `networking_start_task_retry()` (`components/networking/networking.c`): the Wi-Fi
    background/connect task creation retries briefly (up to 1 s) instead of dropping Wi-Fi
    for the whole boot when internal RAM is transiently held.
- **Verification:** `boot_regression COM3 10` and `COM6 10` both **10/10 clean** (and 6/6
  each concurrently); unit suite **585/0** on both boards; 0 build warnings.

### F28. Constant white noise from the Tab5 speaker since v1.3.0 — **FIXED, verified on hardware**

- **Severity:** HIGH (speaker unusable at any volume; survived `volume` and factory reset)
- **Component:** `components/audio/` + Tab5 `board_bsp` + vendored `esp_codec_dev`
- **Board:** `m5stack_tab5` (COM6); reference board (COM3) unaffected by the hiss
  (its amp is codec-driver-owned) but shared the duplex defect below
- **Symptom:** constant white noise from boot; `volume` and factory reset had no effect.
- **Root cause (two coupled v1.3.0 changes):**
  1. The new `audio output` routing applied the resolved route on every codec
     ensure, and the boot pre-warm (`audio_init()` in `main.c`) plus
     `bsp_audio_codec_speaker_init()` latched the NS4150B amp ON from boot —
     while the I2S double-disable fix in the same release left the I2S clocks
     stopped while idle. A powered-but-clockless ES8388 idles as hiss.
  2. Found while verifying with the new mic probe: a microphone record parks
     the shared TX channel (duplex clock master) and never stood it down, so
     the next `tone`/`wavplay` open reconfigured a running channel and failed
     (`I2S_IF: Failed to reconfig STD slot`, playback dead until reboot).
- **Fix:**
  - Amp gated to the playback window only (mute-at-boot invariant, pure
    `audio_amp_should_enable()` predicate, unit-tested): enabled after
    `esp_codec_dev_open()`, muted before every close; `volume`/mode/config
    changes at rest never power the amp; `audio stop` mutes immediately; the
    Tab5 `bsp_audio_codec_speaker_init()` no longer touches the amp.
  - Playback standardized on the M5Stack-proven 48 kHz stereo path
    (`P4_CONFIG_TONE_SAMPLE_RATE_HZ`, stereo-duplicated PCM; WAVs play at
    file rate); post-open the Tab5 programs M5Unified's ES8388 speaker
    register sequence (separate ADC/DAC clocks, DAC mixers, LOUT volumes)
    through a cached I2C handle with read-back (`bsp_audio_codec_speaker_
    post_open`, no-op `NOT_SUPPORTED` on other boards), plus an explicit
    output unmute per play.
  - New `mic level [ms]` / `mic hear <freq> [ms]` / `mic audit [freq]` /
    `mic selftest` verbs (ES7210 on the Tab5, ES8311 on the reference
    board): room levels, a Goertzel tone detector, a lag-immune end-to-end
    speaker audit (waits for actual streaming, retries up to 3 takes) and an
    amp-on vs amp-off hiss verdict that always leaves the amp off; plus
    `audio diag` (live ES8388 register dump).
  - Vendored `audio_codec_data_i2s.c` duplex park/unpark patch (tracked in
    `tools/managed_patches.patch`): the record path still parks TX for the
    duplex clocks but stands it down on record stop (and a stale park is
    dropped before any playback reconfig), with no disable-idle log spam.
- **Verification (COM6 + COM3, focused runs only):** all four builds
  (firmware + `test/`, both boards) 0 errors/0 warnings; fresh-boot serial
  logs 0 ESP_LOG W/E on both boards; boot is silent (operator-confirmed);
  `tone 440` + `tone 880` both audible by ear; `mic audit` reports
  `speaker plays` repeatedly and `audio diag` shows the M5 register state;
  `wavplay`→`mic`→`tone`→`selftest`→`wavplay` in every order with zero `E()`
  lines on both boards; COM3 `mic selftest` reports clean.
- **Known residual (observation, not a bug):** playback start can jitter by
  ~1 s+ on a loaded system and host serial round-trips lag under the same
  load, so a single fixed-window acoustic check can miss the tone; `mic
  audit` compensates (streaming handshake + best-of-3). The 440 Hz band
  couples weakly into the onboard mics in some acoustics, so the audit
  defaults to 880 Hz.

### F27. Transient crash in long mixed sweeps — **RESOLVED (v1.3.0)**

- **Resolution (2026-09-30, v1.3.0 hardware pass):** two deterministic faults
  were found and fixed, and they account for the F27 class:
  1. **Boot crash applying `CURSOR=`/`CURSOR_BLINK=` from CONFIG.SYS** — a
     `Load access fault` in `get_local_style` ← `windows_input_cursor_style`
     (`components/windows/windows.c`) from sampling the input-line pointer
     before taking the LVGL port lock (TOCTOU against a concurrent UI rebuild).
     Both cursor accessors now read under the lock. This was 5/6 boots
     panicking; now 20/20 clean on both boards.
  2. **CPU-bound batch app starved IDLE0 into a task-watchdog abort** — a tight
     `for`/`while` loop or gfx animation ran the command worker without ever
     blocking, so IDLE0 never ran and the watchdog fired mid-app
     (`launch GFXTOOL` reproduced it). `batch_run_nested` now yields at most
     once a second. This removed the `s14_apps` task-watchdog seen here.
  With both fixes the full sweep is clean **3 consecutive** on COM6 and COM3,
  `boot_regression.py COMX 20` is 20/20 clean on both, and `dogfood.py` passes
  on both. The historical detail below is kept for the record.
- **Severity:** HIGH (crash/reboot under sustained mixed load; not reproduced
  on any targeted re-run yet)
- **Component:** under determination — the F26 transcript pump, esp_hosted
  under load (F6-class), or a pre-existing load-path fault
- **Board:** m5stack_tab5 (COM6)
- **Found on:** 2026-09-22, firmware from the F6/F26 pass
- **Bring-up update (2026-09-29, v1.3.0 hardware pass):** the full
  `tools/p4test_run.py COM6` sweep is now generally clean (13/13 suites) and
  `s14_apps` passes on repeated isolated runs; one full sweep still wedged in
  `s14_apps`' `_deploy` (the board stopped answering `receive` with no panic or
  reset, `!= RX READY`, then recovered on the next boot). No ESP_LOG W/E and no
  reset cause were captured. Several consecutive clean full COM6 sweeps after
  that (including 15/15 clean boots) mean the fault is still intermittent and
  not yet root-caused. The new firmware fixes (Tab5 PSRAM-stack bring-up tasks,
  batch/delayed-expansion bugs) removed the deterministic failure modes found
  during the pass; this residual stall remains the exit gate.
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
- **Sweep 1 (2026-09-23, F26-windowed build):** full `p4test_run COM6` —
  **no F27 crash** (zero `rst:0x`, no panic/WDT). Suite FAILED on 3 non-F27
  issues that are now fixed:
  1. **Batch engine bug:** `call :label` in `while`/`for` loop bodies set
     `s_goto_pending`, which loops misinterpreted as "break" — only 1 iteration.
     Fixed by adding `shell_execute_call_label_inline()` that executes label
     bodies directly (bypassing the goto mechanism). BOUNCE now runs 240 frames;
     GFXTOOL reaches `[M-GFXTOOL-DONE]`.
  2. **s14_apps TypeError:** `marker in tail` with `str`/`bytes` mismatch in
     `_palm` (line 507). Fixed: `marker.encode() in tail`.
  3. **s13_perf `_stat_line`:** returned first `gfx stats:` match (= reset
     echo), not the real stats line. Fixed: returns last match.
  4. **s12_power_audio:** expected `battery.detail:` on INA226 boards that print
     `battery.gauge:`. Fixed: accepts either format.
- **Sweep 2 (post-fix):** 8/11 suites passed with zero failures (smoke through
  connectivity); sweep timed out before reaching power-audio/perf/apps.
- **Next step:** 3/3 fresh full `p4test_run COM6` sweeps on the fixed build.
  The reset-cause instrumentation (`session.rst_cause_strings` → `boot_causes`)
  is in place; if a crash reproduces, the cause is classified (WDT vs PANIC vs
  INT) before tuning `P4_CONFIG_TRANSCRIPT_APPLY_TICK_MS` or adding an apply
  work budget.
- **Context / suspicion:** matches the F6-A residual note ("the Tab5 still
  wedges late in a full p4test sweep under sustained load — the C6 link is
  marginal"): a reboot-less stall or SDIO storm under sustained SD+host traffic
  then escalates to the watchdog. A second candidate is the F26 render pump:
  during heavy batch output the LVGL task now runs an apply every tick
  (10 ms) at near-100 % duty, so a starved task fits too. The v1.2.0 sweeps
  (pre-pump) completed 11/11 on COM6, so the pump is the prime NEW variable,
  unproven.

---

### F29. `comp` builtin shadowed by `alias comp=COMPANION.BAT` — **FIXED, verified on hardware**

- **Severity:** MEDIUM (a DOS file verb silently launched the Companion menu app)
- **Component:** `apps/companion/ALIASES.BAT` (shipped convenience aliases)
- **Board:** jc1060p470c (COM3)
- **Found on:** 2026-10-05, DOS-parity hardware pass
- **Symptom:** typing `comp a.txt b.txt` ran `COMPANION.BAT` (title `P4 Companion`,
  `[C-MENU]`) instead of the byte-compare verb; the menu then owned the console
  until option `6` (`[C-EXIT]`). `fc` worked because no `FC*.BAT` exists.
- **Root cause:** the shipped `ALIASES.BAT` defines `alias comp=COMPANION.BAT`
  and aliases win over builtins. Nothing in the repo used the `comp` short name
  (drivers and suites invoke `COMPANION`; `p4` remains as the short alias).
- **Fix:** removed the `comp=` line from all four shipped `ALIASES.BAT` files
  (`apps/companion|adventure|mood|notes/`, same content) with an explanatory
  comment. Boards pick it up on the next `push_sd` / alias reload;
  live workaround is `alias comp=` (deletes the alias for the session).
- **Verification (COM3):** after `alias comp=`, `comp hwprobe.txt hwprobe2.txt`
  prints the DOS offset/byte report (`Compare error at OFFSET 0`, `10
  mismatches - ending compare`); bare `comp` no longer enters the menu.

### F30. `debug poke` guard unsatisfiable on ESP32-P4 — **FIXED, verified on hardware**

- **Severity:** HIGH (the verb could never succeed: every address refused)
- **Component:** `components/shell/shell.c` (`shell_debug_addr_writable`)
- **Board:** jc1060p470c (COM3), unit suite
- **Found on:** 2026-10-05, DOS-parity hardware pass (via a new unit test)
- **Symptom:** `debug poke <addr> <bytes>` always refused, even for plain DRAM,
  while `debug peek` on the same address worked.
- **Root cause:** the guard used `esp_ptr_executable()` to exclude code, but on
  ESP32-P4 the IRAM0/DRAM0 SRAM window is unified (`SOC_IRAM_* == SOC_DRAM_* ==
  0x4FF00000..0x4FFC0000`), so IDF reports *every* RAM byte executable (ditto
  PSRAM via the SPIRAM branch). The unit test asserting writable-TRUE on a
  `.bss` cell failed on device, proving it.
- **Fix:** the guard now refuses only distinguishable code windows (flash IROM
  + mask ROM + RTC fast memory via `SOC_*` ranges in `shell_debug_addr_known_code()`).
  The YES gate and the audit log remain the protection for ordinary RAM. Header
  docs (`shell.h`) and `command.md` updated to match.
- **Verification:** unit suite 507/507 on COM3 with a new writable-TRUE
  assertion on a DRAM cell (code addresses still refused); `debug poke 0 00`
  still refuses with the audit warning on hardware.

### F31. `find` search header and summary leak into `>` redirects — **FIXED, verified on hardware**

- **Severity:** LOW (cosmetic file pollution when filtering into a file)
- **Component:** `components/storage/storage_text.c` (`find` text search)
- **Board:** jc1060p470c (COM3)
- **Found on:** 2026-10-05, DOS-parity hardware pass
- **Symptom:** `find <text> <file> > out` writes the `---------- <path>` header
  line and the `find: N match(es)` summary line into `out` along with the
  matches. The header also bypasses `/V` (it is printed unconditionally for
  file searches), so `find /V ... > out` still pollutes. During this campaign
  that exact mechanism appended junk lines to CONFIG.SYS (recovered from the
  trash backup; board left pristine).
- **Repro:** any `find <text> <file> > <newfile>`, then `type <newfile>`.
- **Fix:** new `shell_redirect_capture_active()` predicate (`shell.c/h`);
  `find` text search skips the header under redirect, prints a bare count for
  `/C`, and drops the summary otherwise. Interactive output is byte-identical.
- **Verification (COM3):** `find hello f > F.OUT` + `type` shows matches only;
  `find /C … > F.OUT` shows a bare `1`; interactive form unchanged.

### F32. `findstr` regex anchors eaten (`"^find:"` searched `find:`) — **NOT A BUG, documented**

- **Severity:** TRIVIAL (confusing at first sight, no data impact)
- **Component:** shell caret escapes (`components/shell/shell.c`), not findstr
- **Board:** jc1060p470c (COM3)
- **Found on:** 2026-10-05, DOS-parity hardware pass
- **Symptom:** `findstr /R "^find:" CONFIG.SYS` printed `findstr: path not found
  find:` before the real matches — looked like findstr probed the pattern as a
  file.
- **Root cause:** the shell consumes `^c` as a one-character escape *everywhere
  except single quotes* (`shell_unescape_carets_in_place`), so findstr really
  received `find:`. Its "path not found" was then correct (no such file).
- **Resolution:** documented in `command.md` (findstr section): use `^^` for a
  literal caret in double quotes, or single quotes (`findstr /R '^find:'`).
  No code change; caret escapes are intentional DOS-adjacent behavior.

### F33. `DEVICE=<bad>` warns twice at boot — **FIXED, verified on hardware**

- **Severity:** TRIVIAL (one extra boot line for a config typo)
- **Component:** `components/boot/boot.c` (`boot_handle_device` + caller)
- **Board:** jc1060p470c (COM3)
- **Found on:** 2026-10-05, DOS-parity hardware pass
- **Symptom:** `DEVICE=FLOPPY` in CONFIG.SYS prints both the specific refusal
  (`DEVICE: only DEVICE=USB is supported ...`) and the generic
  `config: unknown directive "DEVICE", skipped` + `W boot: Unknown CONFIG.SYS
  directive`. Good values (`DEVICE=USB`) are quiet and verified to set
  `usb.msc.automount: on`.
- **Suggested fix:** skip the generic unknown-directive warning when the DEVICE
  handler already reported (return-value contract already exists).
- **Fix:** removed the `boot_warn_unknown("DEVICE")` call after the specific
  refusal (the dispatcher stays silent for DEVICE, so exactly one line
  remains).
- **Verification (COM3):** `DEVICE=FLOPPY` boot shows only the specific
  refusal line; CONFIG.SYS restored pristine afterwards.

---

### F34. Tab5 command worker creation failure (deterministic boot brick) — **FIXED, verified on hardware**

- **Severity:** CRITICAL (Tab5 booted to a dead shell: every command answered
  `shell: Command worker not initialized`)
- **Component:** `main/main.c` boot order vs Tab5 heap (`components/command/command.c`)
- **Board:** m5stack_tab5 (COM6); jc1060p470c unaffected
- **Found on:** 2026-10-06, deferred-hardware gate (first Tab5 flash of the round)
- **Symptom:** `E p4minishell: Failed to create command worker task` on every
  boot; shell prompt alive but nothing dispatches. Persistent across reboots.
- **Root cause:** the 32 KiB worker stack (`xTaskCreate`) needs one contiguous
  internal-RAM block, but `display_init()` (1280x720 LVGL heap) runs first and
  fragments/eats it — measured 95,943 free / 32,756 largest vs 32,768 needed
  (fails by 12 bytes; any layout shift detonates it). Reference board has
  ~35 KiB+ more headroom (smaller panel) and survived. Unit-test app boots
  fine (516/516 on the same board), exonerating hardware.
- **Fix:** create the shell core + command worker *before* display bring-up in
  `app_main()` (neither depends on the display; transcript memory is
  shell-owned, rendering is lazy). Stack size untouched (no overflow-risk
  change); PSRAM static stacks stay bg-only because the main worker runs OTA
  flash writes, during which PSRAM is inaccessible.
- **Verification:** Tab5 boots to a working shell; 10/10 `boot_regression`
  clean on both boards; full shell sweeps green on both.

### F35. Portal SoftAP brownout-reboots USB-powered boards — **MITIGATED, verified on hardware**

- **Severity:** HIGH (reproducible `E BOD` + `rst:0x3` seconds after AP start)
- **Component:** `components/portal/portal.c` (AP RF power)
- **Board:** jc1060p470c (COM3), 3/3 reproductions
- **Found on:** 2026-10-06, portal hardware bring-up
- **Symptom:** `wifi setup on` prints `captive portal active`, then Brownout
  detector fires and the board reboots. Full-power AP beacons are a continuous
  current draw the USB rail cannot sustain alongside display+SD+PSRAM.
- **Mitigation:** cap AP TX power (`P4_CONFIG_PORTAL_AP_TX_POWER_QDBM`, 13 dBm
  default, pre-value restored on stop); best-effort on hosted stacks. Zero
  brownouts in ~15 min AP-on time after the cap, including a full 300 s
  timeout run. Underlying cause is board power budget, not firmware logic.
- **Also fixed alongside:** `set_config(AP)` ran before `set_mode(APSTA)` and
  failed with ESP_ERR_WIFI_MODE (portal could never start); order corrected.
  `wifi setup on|off` now serializes against the background boot init through
  the init-task claim instead of racing `esp_wifi_init`.

---

### F36. File-loaded PEM never reaches the trust store (`-0x2180`) — **FIXED, verified on hardware**

- **Severity:** HIGH (every `certs add *.pem` / boot-loaded PEM silently inert;
  only DER-converted input worked, because the converter counts the NUL)
- **Component:** `components/certs/certs.c` (`certs_load_sd_store`)
- **Board:** jc1060p470c (COM3)
- **Found on:** 2026-10-06, deferred-hardware round (mosquitto.org CA failed
  to load while its DER twin succeeded)
- **Symptom:** `certs add mosquitto.org.crt` (byte-canonical PEM) →
  `mbedtls_x509_crt_parse of global CA cert returned -0x2180`, store stays
  inactive. The byte-identical DER converts and activates fine.
- **Root cause:** mbedTLS sniffs PEM-vs-DER by testing `buf[len-1] == '\0'`
  (`x509_crt_parse`); the file path passed the file size (ending `\n`), so
  PEM input was DER-parsed. One bad entry poisons the whole `set_global`
  call (all-or-nothing).
- **Fix:** length passed to `esp_tls_set_global_ca_store()` now includes the
  terminator; the heap buffer is oversized by one byte to host it permanently.
- **Verification (COM3):** mosquitto.org CA PEM loads → store ACTIVE;
  `certs info` counts exact.

### F37. Live network E2E blocked by bench topology — **TRACKED (no firmware fault)**

- **Severity:** MEDIUM (feature-complete code without live proof)
- **Component:** bench, not firmware (`net tls`, `pim net`)
- **Found on:** 2026-10-06, deferred-hardware round
- **Symptom:** no TLS endpoint is reachable from any board position: `camera`
  LAN and phone hotspot both allow DNS + ICMP + HTTP:80 only (443/1883/8883
  RST; TLS ClientHello dies with `-0x2700`); board↔dev-PC unrouted on both.
  `test.mosquitto.org` additionally uses a private CA (desktop stores reject
  it too — fetch `mosquitto.org.crt` for any live test).
- **Standing:** unit + refusal-path + CA-store-live coverage holds. Local
  amqtt+Radicale bench tooling exists (`tools/radicale_fixture.py` verified
  locally: 207 + 4 hrefs); rerun the live legs wherever a full-route bench
  appears. Single unexplained board reboot during Wi-Fi bring-up also noted
  (one data point, possibly power; watch for recurrence).

### F38. Double-glyph set in ansi_palette.h duplicated singles — **FIXED, verified on hardware**

- **Severity:** HIGH (wrong box rendering on every TUI draw)
- **Component:** `components/ansi/ansi_palette.h` (double/rounded box macros)
- **Found on:** 2026-10-07, TUI round
- **Symptom:** `SH_BOX_*2` and `SH_BOX_*R` macros duplicated the single-glyph
  bytes (e.g. `X2` had `T2` bytes, `T2`/`B2`/`L2`/`R2` decoded off-spec).
  `tui_draw_box` with `double` or `rounded` rendered single lines.
- **Root cause:** copy-paste error in `ansi_palette.h` when extending the
  box-drawing set; the double/rounded codepoints were not the true CP437
  doubles (U+2550–U+256C).
- **Fix:** replaced all 8 double/rounded macro byte sequences with correct
  UTF-8 for U+2550 (═), U+2551 (║), U+2554 (╔), U+2557 (╗), U+255A (╚),
  U+255D (╝), U+2560 (╠), U+2563 (╣), U+2566 (╦), U+2569 (╩), U+256C (╬),
  and rounded variants; added inline comments with codepoint numbers.
- **Verification:** `tui selftest palette` (unit `test_tui_selftest_palette_clean`)
  now passes on COM3 and COM6; box draws on both boards render correct
  double/rounded lines. 4/4 images zero warnings; unit suite 516/516 both
  boards; boot regression 10/10 both boards; shell sweeps clean.

### F39. Tab5 screenshots exceed the image-viewer caps — **FIXED, verified on hardware**

- **Severity:** MEDIUM (screenshot→view round-trip broken on one board)
- **Component:** image caps (`components/gfx/gfx.h`, `p4minishell_config.h`)
- **Found on:** 2026-10-07, modal round (new `screen run type=image` s06
  coverage failed on COM6 while passing on COM3)
- **Symptom:** `screenshot X.BMP` on the Tab5 captures 1280x720 RGB565
  (~2.7 MB), but `image show` / `view` / `screen run type=image` refused it
  (`image: cannot open`): `GFX_IMAGE_MAX_W/H` (1024x640) and
  `P4_CONFIG_IMAGE_MAX_BYTES` (2 MiB) predate the Tab5 target.
- **Fix:** `GFX_IMAGE_MAX_W/H` → 1280x1280 (covers 1280x720 landscape and
  720x1280 portrait; worst-case decode 3.2 MB PSRAM, well within budget),
  `P4_CONFIG_IMAGE_MAX_BYTES` → 4 MiB. All cap uses are validation bounds +
  PSRAM allocs — no stack/static impact.
- **Verification:** `screen run type=image` over an on-device screenshot now
  opens and auto-closes (EL 0) on COM6; `s06_batch` 142/142 on COM3 and COM6.

### F40. s14 ADVENT moves + TCMD quit fail only in full-suite runs — **FIXED in harness, verified on hardware**

- **Severity:** HIGH (s14 red on hardware; no firmware fault)
- **Component:** harness (`tools/p4test/screenshot.py::capture()`,
  `tools/p4test/device.py::screenshot()`, `tools/suites/s14_apps.py::_screenshot`)
- **Found on:** 2026-10-09, s14 `apps` full-suite runs on COM3 (3/3 failed the
  ADVENT moves and TCMD quit; every standalone and bisect probe passed)
- **Symptom:** in full-suite runs ADVENT printed `[M-ADV]` then every move
  failed as `Unknown command: n` (shell owned the input); TCMD likewise never
  reached `[M-TCMD-DONE]`. Standalone `launch ADVENT` + moves passed 8/8.
- **Root cause:** s14 screenshots ADVENT immediately after `[M-ADV]`, while
  the game still owns the serial input (drawing, pre-`ask`). The swallowed
  screenshot line yields no `streaming` prelude, so the capture retry sent
  Ctrl-C ("foreground break"), which aborts a batch at a line modal: the game
  died (`^C`, shell prompt back), the retry screenshot then succeeded at the
  shell, and all later moves hit the shell. Proven live: Ctrl-C at `ask`
  reproduces the exact signature; a short CONTROL+WRITER+HYBRID+LIB prefix
  reproduced the full-suite death deterministically (heavy transcript slows
  the room draws, so the screenshot line always lands pre-`ask` and is eaten
  when `ask` opens). Singles (WRITER/CONTROL/HYBRID/LIB), SD latency (36 ms
  steady), env pressure (15-16 vars), and the screenshot itself were all
  eliminated as causes.
- **Fix:** `capture()`/`Device.screenshot()` gained `unwind=True` (default —
  dogfood and visual sweep still unwind shell-owned modals); s14 passes
  `unwind=False`, so a swallowed decorative shot is a noted miss and the app
  under test survives. Two attempts, no break, when not unwinding. No
  firmware change: the break semantics are intentional and depended upon
  (`agent.py::_close_surfaces`).
- **Verification:** prefix+immediate-screenshot repro now survives (game
  moves `n` → hall, `---`); Ctrl-C-at-`ask` kill confirmed as the vector;
  full `s14_apps` suite 165/165 on COM3 (ADVENT 8/8, TCMD start/swap/quit/
  shell-return all green).

### F41. `launch`ed batch apps overflow the 32 KB worker stack (MOOD trend) — **FIXED, verified on hardware**

- **Severity:** HIGH (Guru Meditation, stack protection fault in `shell_cmd`)
- **Component:** firmware (`components/command/command.c::shell_command_launch`)
- **Found on:** 2026-10-10, s14 `apps` run on COM3 panicked in MOOD trend
  (first `live_tick`); standalone `launch MOOD` reproduced, direct `MOOD`
  did not
- **Symptom:** `launch MOOD` → trend round 1 prints through `set pct=0`,
  then `Guru Meditation Error: Stack protection fault` in `shell_cmd`
  inside `_svfprintf_r` (SP below the 32 KB worker bounds). Direct `MOOD`
  (same file) completes the trend. Menu size, `rgb`/`tone`, `mem` parsing,
  batch nesting, SD state, and pool size (1→3 revert still crashed) were
  all eliminated by on-board bisection.
- **Root cause:** deep app nesting (`launch` → batch → `while` → `call` →
  `for` → `if`) already runs the worker stack to ~31 KB; `launch` kept
  ~1.3 KB of path scratch (`batch_path` + 3 APPS candidates) on the stack
  for the whole app run — the last straw. (Why the older binary survived:
  its trend path used slightly less stack elsewhere; margin was ~1 KB
  either way.)
- **Fix:** `launch <name>` path buffers are heap-allocated and freed with
  the discovery table after the app returns (the codebase rule: no
  command/line-sized stack locals on the batch recursion path).
- **Verification:** `launch MOOD` trend completes on COM3; full `s14_apps`
  182/182 COM3+COM6; `s01`/`s06`/`s10` green both boards; 4-image
  zero-warning builds; `boot_regression` 10/10 both boards, clean logs.

### F42. `hard_reset()` over USB-Serial-JTAG does not reboot this board

- **Severity:** MEDIUM (harness confusion, not a firmware fault)
- **Component:** harness (`tools/shell_session.py::hard_reset`) vs board
  (COM3 reference)
- **Found on:** 2026-10-11, task-manager debugging: repeated "fresh boot"
  probes ran on stale state (an ancient suspended pool job haunted a full
  day of debugging); a `reboot_check.py` showing zero boot bytes proved
  `hard_reset()` never reset.
- **Symptom:** `hard_reset()` returns normally but no ROM boot text follows;
  pool slots, SD state, and uptime all survive. Only the serial `reboot`
  verb (or a real power cycle) resets.
- **Root cause:** USB-Serial-JTAG has no DTR reset line; the DTR/RTS ritual
  only affects classic USB-serial bridges. `hard_reset()` pre-setting
  DTR/RTS low prevents reset-on-open, but cannot trigger one.
- **Fix:** harness discipline, no code change — "fresh boot" claims must
  show boot text (`ESP-IDF` / `boot complete`); otherwise assume stale
  state. `reboot` verb for a real reset.
- **Verification:** `reboot` verb shows ROM boot text; pool returns to
  boot state (bg0/bg1 SUS-never-launched, bg2 unavailable).

---

## Known quirks / by design

These are intentional design decisions, not bugs. Check here before "fixing"
one.

- **`pwd` is not a command.** Use `cd` with no argument (the prompt shows cwd).
- **Serial keys need Enter.** UART console is line-buffered; USB delivers raw.
- **`list` selection is 1-based; ERRORLEVEL is 0-based.** `q` cancels (255).
- **`dir /path` parses `/` as switches.** Use `sd:` prefix or `cd` first.
- **`set /a` prints its result.** Matches DOS/cmd.exe; use
  `draw list /count:VAR /countonly` for quiet counters.
- **Batch files are RAM-resident** (128 KB cap); larger files stream from SD.
- **`gfx text` uses a bitmap font** (unscii-8), not LVGL; `gfx fill` grows a
  PSRAM seed stack.
- **Theme switch leaves two small bits stale** (header `|` separators, open
  modal) until a rebuild. Cosmetic only.
- **Screenshot captures can show horizontal wrap artifact.** Assert layout
  through `header status` / `tui status`, not pixel positions.
- **Boot "No SD card" sometimes prints even though the card works.** Early
  probe can miss during lazy-mount; batch apps run normally.
- **First `tone` after boot logs a benign `i2s_common` error.** Clears after
  first use.
- **Headphone-detect reads the jack at command/play time (no interrupt).** A
  plug inserted mid-playback takes effect at the next play/volume/status call.
  Live plug in/out transitions are build-verified; on-device confirmation is
  pending.
- **`audio output headphones` on a jackless board plays through the speaker**
  with a note (accepted, ERRORLEVEL 0) — there is no second sink to route to.
- **Transcript span footprint grows with history** and stabilises at the
  scrollback ceiling; `cls` reclaims it. Cost of coloured scrollback, not a leak.
- **Tab5 hosted SDIO runs at 10 MHz** (board override) vs 40 MHz on the
  reference board. First hosted RPC is retried once. Both intentional.
- **Writerdeck: spellcheck needs a wordlist** (`apps/dicts/`). Without one, the
  Spell toggle reports the expected path and stays off.
- **Autosave writes on the first dirty tick, then at most every
  `P4_CONFIG_EDITOR_AUTOSAVE_SECS` seconds.** A session that types anything
  spills once promptly; the interval only gates repeats.
- **Crash files survive `temp clean`** (`sd:/tmp/edit/` is exempt); only
  `recover discard|clear` or a successful save removes them.
- **The user dictionary has no spellcheck of its own:** `spell learn` accepts
  any 1..64-byte token without whitespace; typos learned stay learned until
  `spell forget`.
- **`debug disk` opens the hexview modal on interactive consoles** (serial
  included): the hex text prints first, then the modal owns the session until
  `q`/ESC. Batch/headless runs print text only. Serial scripts must expect
  this (send `q` afterwards).
- **Refused `debug peek`/`poke` attempts log one `W debug:` audit line.**
  That warning is the designed audit trail, not a fault.
- **`pim net` requires inline `/p:<pass>` (history-masked); there is no
  interactive password prompt.** Modal password entry over a headless serial
  link proved racy on hardware (silent prompt, orphaned worker), and batch
  runs need the credential inline anyway — same contract as `wifi connect`.
- **The `debug poke` YES gate reads one key at a time** like `format`/`disk`;
  over serial type the whole word (`YES`) and press Enter on one line.
- **HID mouse is cursorless/clickless until opted in.** A plugged mouse only
  scrolls; `usb|bluetooth mouse cursor|click on` enable tracking and taps
  (tap == touch tap). Cursor and click are one shared state across USB+BLE.
- **Hyperlink regions are approximate (max 32, line/col).** Tap hit-testing
  maps the touch point through the terminal font metrics, so wrapped rows and
  trimmed scrollback can shift a region: a miss only refocuses the prompt
  (never runs anything), and a link hit only previews its exact target —
  the second tap on the same target runs it. Only the first 32 regions of a
  long listing (e.g. `help /all`) are tappable; every row still renders the
  `SH_LINK` style. `link list` / `ui links` show the live dense indices.
- **`suspend` freezes everything the job holds** (mutexes stay owned, SD
  sessions stay boosted, deadlines don't advance while frozen). Freeze is
  for inspection/pacing, not deadlock-breaking; `taskkill` wakes a suspended
  job so the stop applies at once.
- **Idle pool tasks report `SUS` in `ps`.** FreeRTOS parks indefinite-block
  waits on the suspended list, so a worker parked in `ulTaskNotifyTake`
  (and a never-launched pool task) shows `SUS`; `start` slot states
  (`idle`/`running`/`suspended`) remain authoritative and relaunch works.
- **`delay` clamps to `P4_CONFIG_DELAY_MAX_MS` (10 s).** `start delay 30000`
  finishes in 10 s. Test victims that must outlive a flow need an endless
  loop file (s06 `P4VICT.BAT`, s14 inline `TMGRVICT.BAT`), not a long delay.
- **`pkg remove` keeps app data by design** (saves, settings, stores survive
  reinstalls); it names what stayed and how to delete it (`pkg purge`).
  `ASSOC.INI`/`STARTUP.INI` entries pointing at removed apps are kept too
  (warn at use, skip at boot) — delete them with `assoc .ext=` / `startup
  remove`.
- **Unmapped `open` opens the viewer modal** (owns serial input until `q`).
  Serial scripts must send `q` afterwards, like `debug disk`.

---

## Tracked / deliberately retained (not bugs)

Known code-quality items intentionally left as-is pending a dedicated pass.
Recorded here so they are not re-reported as new bugs.

| ID | Location | Finding | Status |
|----|----------|---------|--------|
| C3 | `components/header/header.c` | `s_header_state` written from several tasks while LVGL reads | **DEFERRED** — scalar writes are atomic; lock refactor touches 96 accesses in fragile LVGL code |
| C5 | `components/display/display.c` | `display_set_refresh_rate()` is a stub | panel/BSP does not expose dynamic refresh |
