# P4MiniShell — Bug Log (v1.3.1)

This file is the working log for bug hunting and hardware stress testing. It
was reset for the v1.2.0 campaign (the two-board release) and carried into
v1.3.0, so it reflects what is **still open**.

Fixed entries are removed at each reset (their history lives in
[`changelog.md`](changelog.md) and git).

- **Firmware:** v1.3.1 (ESP-IDF v5.5.5)
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

## Open findings

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

---

## Tracked / deliberately retained (not bugs)

Known code-quality items intentionally left as-is pending a dedicated pass.
Recorded here so they are not re-reported as new bugs.

| ID | Location | Finding | Status |
|----|----------|---------|--------|
| C3 | `components/header/header.c` | `s_header_state` written from several tasks while LVGL reads | **DEFERRED** — scalar writes are atomic; lock refactor touches 96 accesses in fragile LVGL code |
| C5 | `components/display/display.c` | `display_set_refresh_rate()` is a stub | panel/BSP does not expose dynamic refresh |
