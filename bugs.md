# P4MiniShell — Bug Report & Test Campaign (v0.38.2, suite 300/0/2)

Date: 2026-08-24
Hardware: ESP32-P4 (rev 1.0) on COM11, ESP32-C6 co-processor, JD9165 display, SD card present
Firmware: **v0.38.2**. Current config baseline: transcript 65536 (`p4minishell_config.h:93`), async 512 (`:146`), internal trim 4096 (`:120`), SD DMA 4096 (`:621`), command worker stack 32768 (`:1467`), batch-file RAM cap 131072 (`:797`), TUI 80x25 (`:325/:328`). Suite: **300/0/2**.
Scope: Debug sweep and stress testing over the UART console (USB-Serial/JTAG, 115200 baud),
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

The v0.33.0 campaign brought up the `apps/companion` batch app — a menu-driven,
pure-batch system helper — which exercised the batch engine and modal runtime hard and
surfaced three more MEDIUM bugs (M8–M10 below): a command-worker stack overflow under
deep batch nesting, `for /f` failing in batch files (quoted options), and silent command
drops under rapid serial input. All fixed and re-verified on board.

The v0.33.0 deep-test sweep (`apps/companion/deep_test.py`, a reactive serial driver
that reboots to a verified uptime between every module) drove the whole companion app —
all five modules, every sub-branch, cancel/timeout and persistence paths — and surfaced
two more MEDIUM bugs (M11–M12 below): a key-wait prompt race that dropped fast input to
`pause`/`choice`/confirm prompts, and the `if COND cmd1 & cmd2` chain split that ran
`cmd2` unconditionally (which broke the guessing game). Both fixed and re-verified; the
full sweep now passes 8/8. The same campaign also added the Palm-OS-style `db` record
store (see readme.md / command.md / SDK.md).

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

### M8. ✅ FIXED — command-worker stack overflow under deep batch nesting (0.33.0)

- **Symptom:** running `COMPANION.BAT` crashed with:
  ```
  Guru Meditation Error: Core 0 panic'ed (Stack protection fault).
  Detected in task "shell_cmd"
  ```
  The crash was ~580 bytes past the 12288-byte worker stack lower bound and
  happened at the very first `call LIB.BAT::load_settings` after `setlocal`.
- **Root cause:** the dispatch path (`shell_execute_batch_file` →
  `shell_execute_command` → `shell_execute_command_core`) is recursive — a
  batch file re-enters it once per line, and nesting multiplies every frame
  by `P4_CONFIG_BATCH_DEPTH_MAX`. `setlocal` + `call <file>::<routine>`
  (which pushes an isolated environment scope) + `for /f` over a pipe adds
  enough frames to overflow the 12 KB stack. The overflow is borderline, not
  a runaway: each new nesting level pushes it further over.
- **Fix:** raised `P4_CONFIG_COMMAND_TASK_STACK` 12288 → 16384 (the same
  "batch + recursive" pressure that caused the earlier 8192 → 12288 raise).
- **Verified:** the full companion app (menu → dashboard → exit), its
  `call ::routine` libraries, and repeated batch `for /f`/pipe runs complete
  with no crash; heap stays ~94% free.

### M9. ✅ FIXED — `for /f` failed in batch files (`for /f: malformed options`) (0.33.0)

- **Symptom:** `for /f "tokens=*" %%v in (file) do set x=%%v` inside a `.bat`
  printed `for /f: malformed options`, while the identical command worked at
  the interactive prompt.
- **Root cause:** the interactive path strips the DOS options string's quotes
  during tokenization, but the batch path passes the raw line (quotes intact)
  to `shell_forf_parse_options`, which then rejects `"tokens=*"` as an unknown
  option. Documented batch syntax that had never been exercised from a file.
- **Fix:** `shell_execute_for_loop` now drops a leading quote and trims a
  trailing quote/space from the options region so both paths feed the parser
  identical text.
- **Verified:** batch `for /f "tokens=*"`, `"tokens=1,2"`, `"skip=1 eol=;"`
  all iterate correctly on board; the on-disk `apps/companion` notes app and
  settings loader use it.

### M10. ✅ FIXED — silent command drops under rapid serial input (0.33.0)

- **Symptom:** sending commands faster than the worker could run them (scripted
  bursts, pasted lines) printed `shell: command queue full, command dropped`
  and silently lost commands. Seen repeatedly while driving the companion app
  from a host script.
- **Root cause:** the worker queue was only 4 deep and `xQueueSend` used a
  0-timeout, so the 5th queued command dropped the instant the worker was busy.
- **Fix:** depth raised to 8 and the submit now waits up to 1 s for a slot
  (still bounded; only a persistently-stuck worker drops, with a clear
  message). The submit runs on the UART console task — never the worker — so
  blocking cannot deadlock the pipeline, and key-waits (`set /p`, `pause`,
  `choice`) use a separate path that is unaffected.
- **Verified:** a burst of 10 rapid `echo` commands completes with 0 drops.

### M11. ✅ FIXED — key-wait prompt race: `pause`/`choice`/confirm dropped fast input (0.33.0)

- **Symptom:** driving the companion app from a host script, a `pause` would
  sit its full 10 s timeout and then the app's default menu action fired —
  the dismiss key the script sent immediately after the prompt appeared was
  never received. Same race in `choice`, the destructive-confirm prompt, the
  xcopy prompts, and the `-- More --` pagers.
- **Root cause:** those commands printed their prompt and *then* armed the key
  wait (`shell_key_wait_begin()`). The prompt text is queued for the UART
  console task, so a key typed as soon as the prompt appeared could reach the
  console reader before the wait armed — and was dispatched as a shell command
  (or lost) instead of answering the wait.
- **Fix:** arm the key wait *before* printing the prompt in
  `shell_command_pause`, `shell_command_choice`, `shell_confirm_destructive`,
  the three xcopy prompts, and both `-- More --` pagers.
- **Verified:** the reactive deep-test driver (`apps/companion/deep_test.py`)
  no longer loses dismiss keys; every `pause`/`choice`/confirm is answered on
  the first key.

### M12. ✅ FIXED — `if COND cmd1 & cmd2` ran `cmd2` unconditionally (0.33.0)

- **Symptom:** the companion's number-guessing game jumped straight back to
  the main menu (which defaulted to the melody) after the very first guess.
  In a minimal repro, `if %tries% GEQ 10 echo BIG & goto done` executed
  `goto done` even though the condition was false.
- **Root cause:** the command-chain splitter (`shell_split_chain`) splits a
  line on unquoted `&`/`&&`/`||` *before* execution, so
  `if %tries% GEQ 10 echo Out of tries! & goto main` became two unconditional
  segments — the `goto main` ran on every iteration regardless of the test.
  Same hazard for `for` bodies containing `&`.
- **Fix:** `shell_split_chain` now returns the whole line as a single segment
  when the first token is `if` or `for`; those commands already join the rest
  of the line as their body and re-enter the pipeline, so the `&` runs only
  when the condition/iteration is taken.
- **Verified:** `if ... & goto` skips correctly when false; the guessing game
  plays to "Correct!" or "Out of tries!".

### M13. ✅ FIXED — Wi-Fi boot `[wifi]` was white instead of cyan (0.35.0)

- **Symptom:** on boot the `C6 hosted firmware version: 3.0.6` line and 13
  peers at `networking.c:407,461,464,481,503,510,954,961,967,980,987,1043,1131`
  plus `c6ota:` at `1080,1109,1114,1118,1127,1142,1147` printed white `[wifi]`
  instead of the cyan prompt accent.
- **Root cause:** `networking.c` used plain `networking_schedulef("[wifi] ...")`
  with no ANSI — so `[wifi]` rendered as `SH_TEXT` white. The palette expects
  `SH_PROMPT` cyan for `[wifi]` and `SH_VAL` bright white for versions.
- **Fix:** all plain `[wifi]` schedules now use
  `networking_schedulef_ansi(SH_PROMPT "[wifi]" SH_RST ... SH_VAL ... SH_RST)`;
  `@C[wifi]@R` expands to `ESC[96m[wifi]ESC[0m` (verified).
- **Verified:** boot `[wifi]` is cyan, versions are bright white on UART and LVGL.

### M14. ✅ FIXED — `main.c:200` progress passed literal `@` to ANSI append (0.35.0)

- **Symptom:** `shell_c6ota_progress_callback` at `main.c:200` built `line`
  with `snprintf` + `SH_*` literals then passed a literal `@` format to
  `shell_transcript_append_ansi` / `shell_schedule_transcript_appendf`, so
  colour codes could be double-escaped or stripped.
- **Root cause:** mixed plain `snprintf` with ANSI-specifier literals and routed
  through the non-ANSI schedule path.
- **Fix:** now uses `ansi_format` + `shell_schedule_transcript_appendf_ansi`
  so SGR codes are correctly parsed and the async flush dispatches via the
  ANSI path.
- **Verified:** progress lines render with correct colours on both console and LVGL.

### M15. ✅ FIXED — `applib.c:58` used `vsnprintf` even when `ansi==true` (0.35.0)

- **Symptom:** `app_vformat_append` in `components/applib/applib.c:58` called
  `vsnprintf` even when `ansi==true`, so `app_printf_ansi` with `@`-specifiers
  was not colour-parsed.
- **Root cause:** missing branch — the ANSI flag was ignored in the formatter.
- **Fix:** when `ansi==true` now calls `ansi_vformat`; otherwise `vsnprintf`.
- **Verified:** `app_printf_ansi("@Ghello@R")` renders green on LVGL and raw SGR on UART.

### M16. ✅ FIXED — async transcript had no ANSI-aware schedule path (0.35.0)

- **Symptom:** background tasks (networking, progress) scheduled coloured output
  via the plain `shell_schedule_transcript_appendf`, so the async flush always
  called `shell_transcript_append_text` and stripped colours.
- **Root cause:** only a plain schedule entry existed; the flush had no
  ANSI detection.
- **Fix:** added `shell_schedule_transcript_appendf_ansi` (in `shell.h`/`shell.c:709`)
  that formats via `ansi_vformat`; `shell_async_transcript_flush_cb` now checks
  `ansi_contains_escapes()` and dispatches to `shell_transcript_append_ansi`
  when ESC is present, otherwise plain. `networking_host_ops_t` notes the ANSI
  schedule hook.
- **Verified:** `networking_schedulef_ansi` and `shell_c6ota_progress` colours survive the async path.

### M17. ✅ FIXED — `dialog`/`list`/`ask` dispatcher was missing (0.35.0)

- **Symptom:** `dialog`, `list`, `ask` were implemented in `components/modal/modal_surf.c`
  but never wired in `components/command/command.c` — typing them at the prompt
  or from a batch file returned `Unknown command`.
- **Root cause:** the 6 TUI surfaces were stubbed (`return -1`) and the dispatcher
  arms for the first three were omitted during the stub phase.
- **Fix:** added `dialog`/`list`/`ask` arms (plus `browse`/`view`/`hexview` and
  `draw`/`color`/`locate`) to `shell_execute_command_core()` with `/t:secs`
  and `/v:NAME` parsing.
- **Verified:** `dialog`, `list`, `ask` return correct ERRORLEVEL/`ASK_RESULT` via touch, USB, and serial, including cancel/timeout.

### M18. ✅ FIXED — `browse`/`view`/`hexview` were stubbed (`return -1`) (0.35.0)

- **Symptom:** `browse`, `view`, `hexview` existed as `return -1` stubs in
  `modal_surf.c` — invoking them always returned errorlevel -1 with no UI.
- **Root cause:** TUI restoration was deferred; the filebrowser/viewer/hexview
  surfaces were placeholder returns pending the modal runtime work.
- **Fix:** restored full implementations on the shared modal runtime: `browse`
  (`filebrowser`) lists `P4_CONFIG_TUI_BROWSE_LIST_LIMIT` entries per directory
  via FatFS, `view` pages `P4_CONFIG_TUI_VIEW_MAX_BYTES` text with 20-line pages,
  `hexview` shows 16-byte hex dumps; all use `windows_enter_editor_mode` so the
  TUI fills the live transcript region and all accept `/t:secs`/`/v:NAME`.
- **Verified:** `browse`/`view`/`hexview` return correct ERRORLEVEL/`BROWSE_RESULT` and cancel/timeout via touch, USB, and serial; companion `FILES.BAT`/`NET.BAT` use them.

### M19. ✅ FIXED — memory pressure under TUI modal load (transcript / async / SD DMA / internal trim) (0.35.1 hardware testing)

- **Symptom:** with the 6 TUI modals restored and `draw`/`color`/`locate` batch verbs active, long serial sessions plus transcript scrollback fragmented the internal DMA-capable heap (shared with WiFi/SDIO/LVGL spans) toward the newlib FILE-lock `abort()` floor seen in M6, and SD multi-block transactions risked `allocate_dma_buf: not enough mem` (M5 pattern).
- **Root cause:** `P4_CONFIG_TRANSCRIPT_BYTES` 2048 still held ~16 KB of span overhead in internal RAM; `P4_CONFIG_ASYNC_TRANSCRIPT_BYTES` 1024 staged background output on the same heap; `P4_CONFIG_SD_DMA_BUFFER_BYTES` 8192 permanently reserved 16 sectors; `P4_CONFIG_TRANSCRIPT_INTERNAL_TRIM_BYTES` 49152 trimmed too late and kept 1/2, so a burst before trim could dip below 10 KB and still `abort()` in `lock_init_generic` (`components/shell/shell.c:144` `shell_transcript_guard_internal()` + `components/windows/windows.c:418` `windows_transcript_trim()`).
- **Fix (4 parts, `p4minishell_config.h:93,117,134,626` + `p4minishell_config.yaml` `buffers`/`transcript_internal_trim_bytes`/`sd_dma_buffer_bytes`):**
  1. `P4_CONFIG_TRANSCRIPT_BYTES` 2048→1024 — halves the span-group ceiling and its internal-RAM overhead.
  2. `P4_CONFIG_ASYNC_TRANSCRIPT_BYTES` 1024→512 — halves the background staging buffer; flush still heap-allocates the drain and dispatches via `shell_schedule_transcript_appendf_ansi` when ESC present (`components/shell/shell.c:709`).
  3. `P4_CONFIG_SD_DMA_BUFFER_BYTES` 8192→4096 — 8 sectors cached on `card->host.dma_aligned_buffer` at mount via `storage_sd_ensure_dma_buffer()`; lower permanent reservation while keeping reuse.
  4. `P4_CONFIG_TRANSCRIPT_INTERNAL_TRIM_BYTES` 49152→4096 with 1/4 keep and trim-below-10KB guard — `shell_transcript_guard_internal()` now trims earlier and keeps 1/4 on trim; `windows_transcript_trim()` guards the case where free internal RAM is already <10 KB so the next `printf` lock cannot `abort()`.
- **Verified:** flash to COM11, boot `P4MiniShell v0.35.7 ready`, 50+ mixed SD ops (dir/type/write/copy/pipes/tree/chkdsk) with zero `allocate_dma_buf` errors, transcript stays under internal-RAM failure floor, no `abort()` in `lock_init_generic` during extensive serial modal tests. Clean build 0 errors/0 warnings.

### M20. ✅ FIXED — `tone`/`wavplay` `abort()` in managed BSP `esp_codec_dev` (0.35.1 hardware testing)

- **Symptom:** `tone 440 200` / `wavplay <file>` intermittently `abort()`ed with `abort() was called at PC ...` in `managed_components/espressif__esp_codec_dev/i2s/esp_codec_dev.c` when the BSP card handle was null (first use after boot / under memory pressure).
- **Root cause:** `audio_codec_new` / `esp_codec_dev_new` dereferenced a null `card_handle` (BSP `esp32_p4_function_ev_board` codec path) without a guard; the I2S `i2s_channel_disable` path also logged `the channel has not been enabled yet` on first tone. `components/audio/audio.c` `audio_play_tone`/`audio_play_wav` background task correctly returned `false` on busy but could not prevent the lower-layer abort.
- **Fix (corrected ref in v0.35.3 — the `esp_codec_dev.c:269` path was stale):** the real guards are `managed_components/espressif__esp32_p4_function_ev_board/esp32_p4_function_ev_board.c:305-380` (graceful `i2s_new_channel`/NULL-`i2s_data_if` errors instead of `assert`/`ESP_ERROR_CHECK`) plus the retry-and-fail-soft `audio_ensure_speaker()` in `components/audio/audio.c:72-93` (3×1s `bsp_audio_codec_speaker_init`, `ESP_FAIL` instead of aborting); `components/audio/audio.c` already heap-allocates tone chunks and bounds `P4_CONFIG_TONE_CHUNK_SAMPLES` / `P4_CONFIG_WAV_MAX_BYTES`. `audio status|stop` and `volume` remain batch-safe with ERRORLEVEL. Both managed patches are backed up in `tools/managed_patches.patch` (re-apply after `idf.py update-dependencies`).
- **Verified:** extensive serial `tone`/`wavplay`/`beep`/`audio status|stop` runs on COM11 produce no `abort()`, first-use benign `i2s_channel_disable` log remains harmless, playback still mixes stereo→mono and decimates 44100→22050 via ES8311. Clean build 0 errors/0 warnings.

### M21. ✅ FIXED — modal `EventGroup` internal-heap fragmentation (PSRAM fix) (0.35.1 hardware testing)

- **Symptom:** with transcript/TUI pressure, the shared modal runtime's `EventGroup` (`components/modal/modal.c:46` `xEventGroupCreate`) allocated from the internal DMA-capable heap, competing with LVGL spans and SD DMA and contributing to fragmentation toward the M6/M19 abort floor.
- **Root cause:** `xEventGroupCreate` defaults to `MALLOC_CAP_DEFAULT` (internal) on this P4; under transcript load the small EventGroup allocation fragmented the same heap that `lock_init_generic` needs for stdio FILE locks.
- **Fix:** `components/modal/modal.c:46` now creates the group with `xEventGroupCreateWithCaps(MALLOC_CAP_SPIRAM)` (PSRAM) with internal fallback; `modal_surface_run` session loop + `MODAL_EVENT_CLOSE_REQUEST` / `MODAL_EVENT_CLOSED` + `/t:secs` one-shot timer unchanged. `P4_CONFIG_TUI_COLS`×`P4_CONFIG_TUI_ROWS` 80×25 grid still maps via `windows_enter_editor_mode`/`windows_refresh_editor_surface`.
- **Verified:** modal `dialog`/`list`/`ask`/`browse`/`view`/`hexview` with `/t:secs` timeout and serial input all close with correct ERRORLEVEL (`dialog` 0/1/255, `list` 0-based/255, `ask` 0/1 with `ASK_RESULT`/NAME), no LVGL assert or heap corruption, `draw` remains TUI-aware. Clean build 0 errors/0 warnings.

### M22. ✅ FIXED — `tui_cell_t` truncation broke box glyphs (0.35.1 final hardware bug hunting)

- **Symptom:** `draw box` and `tui_draw_box` rendered `?`/truncated bytes instead of `─│┌┐└┘` — box borders were garbled on the 1024x510 transcript rect, and `tui status` showed `80×25` but `grab_screenshot.py --crop-transcript` captured broken glyphs.
- **Root cause:** `tui_cell_t` `utf8` was `char utf8[2]` (`components/tui/tui.h:35`), but box-drawing UTF-8 sequences `SH_BOX_*` (e.g. `─` `E2 94 80`) are 3 bytes plus NUL → truncated to 1 byte + NUL, so `tui_cell_set` (`components/tui/tui.c:129`) `strncpy` lost the glyph.
- **Fix:** `components/tui/tui.h:35` `utf8[4]` (3 bytes + NUL), `tui_cell_set` `strncpy(cell->utf8, utf8, sizeof(cell->utf8)-1)` with explicit NUL. All box draws now emit full `SH_BOX_*`.
- **Verified:** `draw box 2 2 20 8 single MyBox` / `double` / `rounded` with title, nested boxes (window stack), `draw line` H/V all render correctly in `grab_screenshot.py --crop-transcript`; `tui_flush` per-fg recolor shows intact borders.

### M23. ✅ FIXED — `tui_draw_box`/`line` ignored style and title (0.35.1 final)

- **Symptom:** `draw box ... single|double|rounded` always rendered single, and titles were not centered; `draw line` style had no effect.
- **Root cause:** `tui_draw_box` (`components/tui/tui.c:241`) and `tui_draw_line` (`components/tui/tui.c:296`) wrote ASCII `+|-` instead of `SH_BOX_*` UTF-8 and did not branch on `style`/`title`.
- **Fix:** `tui_draw_box` now selects `SH_BOX_TL`/`H`/`V` vs `SH_BOX_TL2`/`H2`/`V2` vs `SH_BOX_TLR`/`TRR`/`BLR`/`BRR` per `strcasecmp(style, "double"/"rounded")` and centers `title` with surrounding spaces via `tui_print_at` with `tui_cell_set`; `tui_draw_line` selects `SH_BOX_H`/`V` vs `H2`/`V2` vs `HL`/`VL` for `heavy`. All honor fg/bg.
- **Verified:** `draw box 2 2 20 8 double T` and `rounded` show `╔═╗`/`╭─╮` correctly with title centered, nested `draw window` stacks, `draw line 1 5 80 5 double` horizontal double line, no abort.

### M24. ✅ FIXED — `tui_flush` did not render fg/bg (0.35.1 final)

- **Symptom:** `color 0A` / `draw box ...` with fg did not color the TUI; transcript text was monochrome, `color`/`locate` appeared to do nothing on the `80×25` grid.
- **Root cause:** `tui_flush` (`components/tui/tui.c:620`) wrote plain `utf8` without LVGL recolor; `lv_label_set_recolor` was false and no `#RRGGBB` tags were emitted, and palette duplication risk existed.
- **Fix:** `tui_flush` now coalesces by fg, emits `#RRGGBB ` per run via `ansi_get_palette_color` (`components/ansi/ansi.c`) PowerShell palette (no duplicate), wraps each run and closes with `#`, enables `lv_label_set_recolor(true)` on `s_tui_label`, sets text via `lv_label_set_text` under `lvgl_port_lock`. Default fg 16 emits no tag.
- **Verified:** `color 0A` then `draw box` shows bright green border, `color`/`locate` compose correctly, per-fg runs verified via `grab_screenshot.py`.

### M25. ✅ FIXED — header occluded TUI (header not hidden in fullscreen) (0.35.1 final)

- **Symptom:** `draw fullscreen on` did not hide the header; TUI boxes overlapped the header bar, and `tui status` reported incorrect available height.
- **Root cause:** `tui_enter_fullscreen` (`components/tui/tui.c:417`) did not call `windows_set_fullscreen`/`header_set_visible` (`components/windows/windows.c:418`), so the fixed top header bar remained visible over the `1024x510` transcript rect.
- **Fix:** `tui_enter_fullscreen`/`tui_exit_fullscreen` now call `windows_set_fullscreen(true/false)` which toggles `header_set_visible`, hides input row/keyboard, and reflows via `windows_notify_keyboard_visibility`/`windows_refresh_tui_surface`; header is now hidden completely when fullscreen, kept visible by default otherwise.
- **Verified:** `draw fullscreen on` → header hidden completely (pixel-perfect `grab_screenshot.py --crop-transcript`), `draw fullscreen off` and `tui fullscreen off` restore header; no overlap between TUI and shell text in either mode, no watchdog.

### M26. ✅ FIXED — keyboard did not rescale TUI (0.35.1 final)

- **Symptom:** showing/hiding the on-screen keyboard left the TUI sized to the old rect, leaving a gap or clipping the `80×25` grid.
- **Root cause:** `windows_notify_keyboard_visibility` (`components/windows/windows.c:312`) was not invoked on keyboard show/hide while TUI was active, so `s_tui_container` height was stale.
- **Fix:** `windows_notify_keyboard_visibility` now recalculates the transcript rect and calls `windows_refresh_tui_surface` + `tui_flush`; `tui_refresh_surface` (`components/tui/tui.c:408`) is also called on rotation. Keyboard scaling is now dynamic.
- **Verified:** keyboard show/hide while `draw box 1 1 80 25 rounded` is active correctly resizes to `1024x510` minus keyboard, no gap, `tui status` updates rect.

### M27. ✅ FIXED — prompt not shown in all inputs (0.35.1 final)

- **Symptom:** input line showed `> ` instead of `PS \path> `, `ask` placeholder was empty, and shell echo lost the situational color (`SH_PROMPT`).
- **Root cause:** `main.c:112` echoed `SHELL_PROMPT` literal instead of `shell_prompt_render_plain()` (`components/shell/shell.c:412`); `modal_surf.c:412` `ask` did not set placeholder via `shell_prompt_render_plain()` + `keyboard_bind_textarea`; shell echo used plain text.
- **Fix:** `main.c:112` now `shell_prompt_render_plain()`, `modal_surf.c:412` `ask` sets placeholder to `shell_prompt_render_plain()` and calls `keyboard_bind_textarea`, shell echo uses `SH_PROMPT` situational color.
- **Verified:** input line, `ask "prompt"` placeholder, and serial echo all show `PS \path> ` honoring `PROMPT=` (`$p $g` etc).

### M28. ✅ FIXED — audio abort via `bsp_audio_init` (0.35.1 final)

- **Symptom:** `tone 440 200` / `wavplay` could `abort()` on first use after boot via `managed_components/espressif__esp_codec_dev/i2s/esp_codec_dev.c:269` `audio_codec_new` null `card_handle`.
- **Root cause:** `bsp_audio_init` path (`components/audio/audio.c:42`) did not guard `card_handle == NULL` before `audio_codec_new`; fixed in v0.35.1 but re-verified after TUI font/buffer changes increased pressure.
- **Fix:** guard `card_handle` null → `ESP_ERR_INVALID_STATE` instead of `abort()`; `audio status|stop`/`volume` remain batch-safe; memory pressure fixes (`P4_CONFIG_TRANSCRIPT_BYTES` 1024, `P4_CONFIG_SD_DMA_BUFFER_BYTES` 4096, `P4_CONFIG_TRANSCRIPT_INTERNAL_TRIM_BYTES` 60000 with 1/4 keep + trim-below-10KB) keep `bsp_audio_init` off the failure floor.
- **Verified:** extensive serial `tone`/`wavplay`/`beep`/`audio status|stop` without `abort()` on COM11.

### M29. ✅ FIXED — dialog/list/ask serial routing (0.35.1 final)

- **Symptom:** `ask` typed answer on serial was not consumed into `ASK_RESULT` without `/t:secs`; `list` without timeout closed immediately with 255.
- **Root cause:** serial lines were routed to `shell_key_wait_submit` instead of `modal_handle_serial_line` (`components/modal/modal_surf.c:412`) when a modal was active; `dialog`/`list`/`ask` dispatcher now uses `shell_command_ops_t.modal_*` hooks consistently.
- **Fix:** all 6 modals now accept `/t:secs` auto-cancel and serial lines are forwarded verbatim to `modal_handle_serial_line`; `ask` stores `ASK_RESULT`/`/v:NAME` correctly even without `/t:secs`; `draw` auto-enters TUI (`components/tui/tui.c:56`) so `dialog`/`list`/`ask` work outside `draw`.
- **Verified:** `dialog`/`list`/`ask` with `/t:secs` timeout → 255/1 and via serial without timeout → `*RESULT`/`/v:NAME` correct, no input loss, hardware-verified on COM11.

### M30. ✅ FIXED — `draw` did not auto-enter TUI (0.35.1 final)

- **Symptom:** `draw box` without a prior `tui`/`draw` or modal surface did nothing (fell through to transcript).
- **Root cause:** `shell_command_draw` (`components/command/command.c`) required an active TUI/modal; no auto-enter existed.
- **Fix:** `shell_command_draw` now calls `tui_init()` (`components/tui/tui.c:56` via `windows_enter_tui_mode`) when `!tui_is_active()` && `!windows_tui_mode_active()`, then draws; `windows_set_fullscreen`/`windows_notify_keyboard_visibility` keep sizing correct.
- **Verified:** first `draw box 2 2 20 8 single` after boot auto-enters TUI and renders, subsequent `draw` reuses buffer, `tui status` confirms `80×25`.

### M31. ✅ FIXED — command-worker stack overflow at `0x4012b75a` when companion TUI BATs run (0.35.1 companion expansion)

- **Symptom:** running companion TUI batch files (`COMPANION.BAT` → `SYS.BAT`/`FILES.BAT`/`FUN.BAT`) crashed with:
  ```
  Guru Meditation Error: Core 0 panic'ed (Stack protection fault).
  Detected in task "shell_cmd" at 0x4012b75a
  ```
  Overflow occurred after `P4_CONFIG_COMMAND_TASK_STACK` 16384 raised from 12288 in v0.33.0 was still insufficient for deep TUI nesting (`draw` + `tui fullscreen` + `list`/`dialog` + `browse`/`view` modal stack).
- **Root cause:** `P4_CONFIG_COMMAND_TASK_STACK` 16384 (`p4minishell_config.h:1514`) shared by `shell_execute_batch_file` → `shell_execute_command` → `shell_execute_command_core` recursion plus TUI cell buffer and modal surfaces; expanded companion nesting (7 BATs, many `draw box`/`tui fullscreen`/`list` calls) pushed the 16384 budget over the guard at `0x4012b75a`.
- **Fix:** raised `P4_CONFIG_COMMAND_TASK_STACK` 16384→32768 (`p4minishell_config.h:1514` + `p4minishell_config.yaml` `command_task_stack`), command queue full handling improved (bounded 1 s wait instead of silent `queue full` drop, survives burst `draw`/`list` calls), `windows_enter_tui_mode` keeps header visible by default (only `draw fullscreen on`/`tui fullscreen on` hide via `windows_set_fullscreen`/`header_set_visible` `components/windows/windows.c:418`), TUI does not overlap shell text (`tui_hide_for_modal`).
- **Verified:** flash to COM11, boot verified, extensive serial tests of all 7 companion BATs (COMPANION 1552, SYS 1486, FILES 3946, NET 2893, FUN 3968, SET 3109, LIB 1896 bytes) pushed via `push_sd.py` COM11 PASS, each BAT exercised via serial (`list` selections, `browse`/`view`/`hexview`, `tui fullscreen` `draw` boxes) — no abort, no watchdog, no overlap, no `0x4012b75a` overflow; `draw`/`list`/`ask` serial routing verified (`dialog y` → 0, `list 2` → 2, `ask myname` → `myname` via `modal_handle_serial_line` `components/modal/modal_surf.c:412` `shell.c`), 50+ mixed SD ops clean, clean build 0 errors/0 warnings.

### M32. ✅ FIXED — root clutter, browse duplication, layering and config drift (0.35.2 cleanup)

- **Symptom:** 97 one-shot `*.py` patch scripts + 7 `*.txt` dumps crowded the repo root; `browse` existed twice (`components/command/command.c:2413` 4 KB stack + `"File Browser"` vs `components/batch/batch.c:3610` config buffer + `"Browse"`); `shell_launch_app` (`components/command/command.c:2434`) used `char command[256]`; `components/shell/CMakeLists.txt` required `networking`/`c6ota`/`p4_usb` despite ops-table decoupling; root `CMakeLists.txt:5` listed 14/23 components; `p4minishell_config.yaml` missed `P4_CONFIG_PS_COLOR_PARAMETER`/`SUBSYSTEM`/`HEADING` (`p4minishell_config.h:1390,1402,1411`) + `P4_CONFIG_DB_FLAG_SECRET` (`p4minishell_config.h:1660`); `sdkconfig.defaults` did not pin LVGL demos off; `.gitignore` did not cover `sdkconfig`.
- **Root cause:** incremental hardware-bug-hunting workflow committed every patch helper to the root; parallel `browse` implementations diverged; build/config lists not updated when `alarm`/`db`/`tui`/`modal` landed.
- **Fix:** 16 harnesses → `tools/harness/`, rest + dumps deleted; `browse` unified on `P4_CONFIG_TUI_BROWSE_PATH_BYTES` + `"Browse"`; `shell_launch_app` heap `P4_CONFIG_COMMAND_BYTES`; `GFIND_DB_MAX`/`GFIND_ALARM_MAX` → `P4_CONFIG_*` (`components/command/gfind_commands.c:30`); shell `REQUIRES` trimmed; root `EXTRA_COMPONENT_DIRS` = 23; yaml + `sdkconfig.defaults` + `.gitignore` fixed.
- **Verified:** static checks only (no `char [4096]`/`[2048]` in `command.c`, 0 root scratch scripts, 23/23 dirs listed). Full build + board pass deferred.

---

### M33. ✅ FIXED — memory-baseline numbers in M19/M31 went stale (0.35.3 reconcile)

- **Symptom:** `bugs.md` M19 (`P4_CONFIG_TRANSCRIPT_BYTES` 2048→1024, async 1024→512, trim 49152→4096 1/4 keep) and M31 (`P4_CONFIG_COMMAND_TASK_STACK` 16384→32768) no longer match the code: the tree now runs transcript 65536 (`p4minishell_config.h:93`, PSRAM-backed per `p4minishell_config.yaml` `buffers:transcript_bytes`), recolor == transcript (`p4minishell_config.h:101`), trim threshold 4096 (`p4minishell_config.h:120`), async 512 (`p4minishell_config.h:137`), SD DMA 4096 (`p4minishell_config.h:634`), worker stack 32768 (`p4minishell_config.h:1522`).
- **Root cause:** incremental PSRAM relief (spans/staging/scratch out of internal RAM) moved the values on without updating the bug narrative; M19/M31 describe the 0.35.1 hardware state, not the current tree.
- **Fix:** docs follow code — M19/M31 kept as the 0.35.1 history, this entry records the current baseline above; `changelog.md` `## [0.35.3]` carries the same table. No code change.
- **Verified:** header values re-read at `p4minishell_config.h:93,101,120,137,634,1522`; yaml `buffers:` descriptions agree.

---

### M34. ✅ FIXED — `command.c` god-file remainder split (0.35.4)

- **Symptom:** after v0.35.3, `command.c` still held three coherent clusters (~3300 lines): the peripheral toolkit with its GPIO table/gate, the display/power/battery verbs with ADC/idle state, and the screenshot/serial verbs.
- **Root cause:** incremental growth; single dispatcher file owned unrelated domains.
- **Fix:** new `periph_commands.c` (1527 lines, incl. table/gate/toolkit defines), `power_commands.c` (897 lines, incl. ADC/idle state + ops backings), `serial_commands.c` (1015 lines); `command.c` 5808→2467. `http_body` stays (inline httpget arm). Function inventory HEAD vs work: zero lost, zero new. Declared in `command.h` (PERIPH/POWER/SERIAL sections), `SRCS` updated.
- **Verified:** static checks only (single definitions, `command.h` call sites, LF endings). Full build + COM11 deferred.

---

### M35. ✅ FIXED — truncation/OOB + OOM immediacy + tests + LVGL locks (0.35.5 hardening)

- **Symptom:** audit found two real OOB risks (`tui_flush` recolor `size_t` underflow `components/tui/tui.c:382`, hexview pager `components/modal/modal_surf.c:908-917`), silent-truncation paths (tree child paths, trash unique names, db path builders), 10 delayed-NULL alloc sites (dispatch snapshots, async submit, sd copy, repair, init, subdirs), one 2048B worker-stack local (`clip`), zero unit coverage for tui/modal/power/serial/clipboard/history-file, and an unlocked LVGL fallback (`windows.c:779` contradicting its own doc comment).
- **Root cause:** incremental growth without a hardening pass; copy-pasted `/t:` parsing ×9; tests only covered pure helpers that happened to exist.
- **Fix:** fail-closed clamps/guards (fitting inputs unchanged), immediate OOM checks with errorlevel, clip heap buffer, shared `modal_parse_*` helpers + promoted pure functions with 22 new suites, recursive-lock on the two proven cross-task LVGL paths. See `changelog.md` `## [0.35.5]` for the file:line list.
- **Verified:** hardware session 2026-09-05 — `idf.py build` 0/0, runner **180 pass / 0 fail / 0 skip**, COM11 sweep clean (see `changelog.md` `## [0.35.5]`).

---

### M36. ✅ FIXED — storage/batch remainder splits (0.35.6; shell intentionally whole)

- **Symptom:** `storage_commands.c` (~6.1k) and `batch.c` (~4.9k) still monolithic after v0.35.4.
- **Root cause:** incremental growth; single files owned unrelated verb groups.
- **Fix:** five storage files (nav/files/disk/text/fam) + `batch_expr.c`, all verbatim with 4 helper promotions (`shell_parse_alloc_unit`, `shell_format_execute`, `shell_dir_format_stamp`, `shell_find_parse_date`) and shared aliases to `storage_commands.h`; 19 `s_errorlevel` writes → `batch_set_errorlevel()`. Wider batch split stopped (porous engine/verb statics); shell split stopped (ops-table hub + shared transcript pointers in 7+ sections). See `changelog.md` `## [0.35.6]`.
- **Verified:** static checks only (inventories zero lost/zero new, cross-file statics clean, endings preserved). Full build + COM11 deferred.

---

### M37. ✅ FIXED — `ask` serial answer clobbered by empty textarea (0.35.7)

- **Symptom:** serial `ask` logged `ask serial line: 'blue'` but `ASK_RESULT` stayed empty.
- **Root cause:** `ask_surface_close()` (`modal_surf.c`) unconditionally copied the on-screen textarea over the serial answer.
- **Fix:** `serial_answered` flag; close captures textarea only without a serial answer. Verified `RESULT=[blue]` on hardware.

### M38. ✅ FIXED — alarm checker never started; eager start breaks USB (0.35.7)

- **Symptom:** `checker stopped`, `catchup pending`, fire test failed though commands worked.
- **Root cause:** `alarm_init()`/`alarm_register_host_ops()` never called. First fix (eager in `command_init()`) broke USB HCD bring-up (heap fragmentation before `usb_init()`, A/B-verified on hardware).
- **Fix:** ops registered in `command_init()`, init lazily on first `alarm`/`cal`. Verified `checker running`, alarm suite 25/25, USB healthy.

### M39. ✅ FIXED — modal LVGL layout-loop watchdog (0.35.7)

- **Symptom:** `COMPANION.BAT` → `list` hung `shell_cmd` in `lv_obj_update_layout` → task-watchdog abort (decoded via addr2line: worker → `list_surface_open` → layout loop).
- **Root cause:** all six modal open functions refreshed outside the port lock, racing the render task.
- **Fix:** refreshes moved inside locked regions. Verified: no watchdog, menu select works.

### M40. ✅ FIXED — box-glyph tofu from dead config + latent font syntax errors (0.35.7)

- **Symptom:** TUI borders rendered as placeholder boxes (screenshot-verified).
- **Root cause:** stale generated `sdkconfig` lost `CONFIG_LV_FONT_UNSCII_16` → ASCII fallback; enabling it exposed two missing commas in the in-place font extension (previously compiled to empty).
- **Fix:** unconditional font use, config regenerated, commas fixed. Pixel-verified.

---

### M41. ✅ FIXED — `launch`/`apps`/`delay`/`notify`/`gfind` verbs + applib env/app dispatch lost in splits (found 2026-09-07 hardware session, COM3)

- **Symptom:** `apps`, `launch /list`, `hello`, `delay 1000`, `notify x`, `gfind x` all returned `Unknown command`, although `command.md` documents every one, `shell_launch_app()` (boot `LAUNCH_APP` hook) shells out to `launch`, and `main/native_apps.c` registers `hello`. `hello` showed `cwd=(unavailable) PATH=(unset)`.
- **Root cause:** the v0.35.3–v0.35.6 god-file splits dropped the dispatcher arms (and the `applib_env_ops` registration + the `app_dispatch` tail hook) while keeping the implementations, configs (`P4_CONFIG_LAUNCH_MAX`, `P4_CONFIG_APP_MAX`, `P4_CONFIG_DELAY_MAX_MS`), docs, and callers. `git log -S` confirms the arms predate the split history present in this repo.
- **Fix:** `components/command/command.c`: new `launch` verb (heap discovery table over PATH + `sd:/APPS` via `storage_expand_wildcard`, APPINFO titles via `storage_ini_file_get`, `/list` + `<name> [args]` + numbered menu with bounded `shell_read_line`, 0/1/2 ERRORLEVEL, slash-tolerant join, APPS fallback with traversal guard), new `apps` verb (`app_get` loop), restored `delay` (overflow-safe digit parse, clamped to `P4_CONFIG_DELAY_MAX_MS`, 0/2), restored `notify`/`gfind` arms, `app_dispatch` tail hook after the `.bat` lookup (never shadows built-ins or batch files), `applib_env_ops` registration in `command_init()`; completion table + both `help` surfaces updated (`shell.c`).
- **Verified:** `idf.py build` 0/0, `launch /list` shows `COMPANION  -  P4 Companion`, `launch COMPANION` runs the app end to end, `launch` menu cancels on 0, `launch NOPE` → 1, `apps` lists `hello`, `hello test` shows real cwd/PATH and sets `HELLO_RESULT`, `delay xyz` → usage/2, `notify`/`gfind`/`delay` respond, errorlevels match `command.md`.

### M42. ✅ FIXED — test project link failure on `lv_font_unscii_16` + stale generated `test/sdkconfig` (same session)

- **Symptom:** `test/` build failed linking `windows_get_terminal_font` (`undefined reference to lv_font_unscii_16`).
- **Root cause:** M40 made the font unconditional, but `test/sdkconfig.defaults` never pinned `CONFIG_LV_FONT_UNSCII_16`; worse, the COMMITTED generated `test/sdkconfig` (stale, author's machine) silently wins over defaults on reconfigure, so adding the default alone changed nothing.
- **Fix:** pinned the option in `test/sdkconfig.defaults`, deleted the stale generated `test/sdkconfig` so kconfig regenerates from defaults. Suite links and runs.
- **Verified:** test build 0/0, on-board runner **196 pass / 0 fail / 2 ignore** (193 + 3 new: `test_forf_command_set`, `test_arg_apply_modifiers`, `test_variable_expansion_tilde_modifiers`; ignores are the by-design tmpfile guards).

### M43. ✅ FIXED — `dependencies.lock` machine paths blocked every fresh build (same session)

- **Symptom:** first `idf.py build` on a new machine died in CMake: `The "path" field in the manifest file "D:\p4minishell\managed_components\..." does not point to a directory`.
- **Root cause:** both `dependencies.lock` files pin `type: local` components to the author's absolute `D:\p4minishell\...` paths (traced to `idf_component_tools/sources/local.py` `_path` validation).
- **Fix:** rewrote the prefix to this checkout's root in both locks (backup in temp). Managed tree verified intact afterwards (384-glyph font + both M40 commas + BSP guards present; `reapply_managed_patches.ps1 -Check` correctly reports nothing-to-do on a patched tree).
- **Note for maintainers:** any new checkout must re-point or regenerate the locks (`idf.py update-dependencies` + reapply shim).

### M44. ✅ FIXED — companion `list` branches used 1-based numbers against 0-based ERRORLEVEL (same session)

- **Symptom:** deep driver selections landed on wrong items (SYS save→back, FILES back→hex demo, notes submenu fully shifted, NET connect/known swapped, FUN rgb/tui-demo swapped, SET reset/tui-demo swapped). Menu walk passed because cancel paths + items 0/1 masked it.
- **Root cause:** `list` returns the 0-based index, but most submenu `if errorlevel N` chains were written as if it returned the typed 1-based number.
- **Fix:** corrected every submenu chain in `SYS/FILES/NET/FUN/SET.BAT` to 0-based (`255 → back/cancel` unchanged); verified each mapping against its item list. Added serial-visible echo markers (`[C-MENU]`, `[M-<MOD>]`, `[M-<MOD>-BACK]`, `[C-EXIT]`, `[M-SYS-SAVED]`, `[M-FILES-NOTED]`, `[M-NET-FETCHED]`) so the reactive driver keys on markers instead of TUI screen text (modals print nothing on serial by design).
- **Verified:** `deep_test.py` **8/8 PASS** on board (baseline, menu 5/5, sys + snapshot artifact, files + note artifact, net offline, fun + calc 42, set + persist across reboot).

### M45. ✅ FIXED — `serial_sweep2.py` wedged itself on the `list` modal (same session)

- **Symptom:** `ask serial` FAIL (and everything after it suspect).
- **Root cause:** the sweep opened `list` without ever selecting — the modal stayed open and swallowed all later lines. Firmware was correct throughout (proven by direct repro: modal survives 25 s idle, `RESULT=[slowanswer]`).
- **Fix:** sweep now selects `1` to close the list before continuing. `fails: 0`.

### M46. ✅ FIXED — M21 modal PSRAM event group missing; documented API absent in IDF 5.5.5 (same session)

- **Symptom:** `modal_surface_run` used plain `xEventGroupCreate()` (internal heap) despite M21; the documented `xEventGroupCreateWithCaps` does not exist in IDF 5.5.5 headers.
- **Root cause:** split/history loss plus a stale API reference.
- **Fix:** create the group statically in a PSRAM block (`xEventGroupCreateStatic` over `heap_caps_malloc(MALLOC_CAP_SPIRAM|8BIT)`), internal-heap fallback, buffer freed on both exit paths (static groups never self-free). No layering change.
- **Verified:** both projects build 0/0, unit suite still 196/0/2, modal flows green in deep 8/8.

### M47. ✅ FIXED — `calc VAL` prefix guard captured `VALB` (palmtop-parity session)

- **Symptom:** `calc valb('FF',16)` failed with `VAL takes 1 argument`; the
  same call worked only when it happened to render as a prefix-free name.
- **Root cause:** the older `VAL` arm matched by prefix (`strncasecmp(name,
  "VAL", 3) == 0 && name[3] != 'F'`). The added `VALB` shares the prefix, and
  the guard compared `name[3]` against the uppercase `'B'` so lowercase
  `valb` slipped into the `VAL` arm.
- **Fix:** `VAL`, `VALF`, and `VALB` are now matched with exact `strcasecmp`.
- **Verified:** on-board unit suite `test_calc_base_and_units` (317/0/2);
  `calc BIN$(255)`/`OCT$(255)`/`VALB('FF',16)`/`C2F(100)` confirmed on COM3.

### M48. ✅ FIXED — eager CDC-ACM install broke hosted-SDIO bring-up (M38/O8 pattern)

- **Symptom:** with `usb userial` added, `boot_regression` fell to **2/8**
  clean: the boot log repeated
  `sdmmc_allocate_aligned_buf: not enough mem, err=0x101` /
  `eh_host_port_sdio: sdmmc_card_init failed`, and `sd_ready` was false on
  most boots.
- **Root cause:** the CDC-ACM class driver was installed eagerly as a third
  stage of `usb_install_host_stack()`. Its task stack plus internal buffers
  consumed the boot-time internal RAM the ESP-Hosted SDIO transport needs —
  the same failure class as M38 (eager alarm init) and O8 (boot internal-RAM
  peak).
- **Fix:** install the CDC driver **lazily** on first `usb userial open`
  (`userial_install_driver()` from `userial_open()`); the boot path installs
  only MSC and HID as before.
- **Verified:** `boot_regression` **8/8** clean; `usb status` reports
  `usb.host: ready`; `tools/userial_test.py` RESULT OK; unit suite 317/0/2.

---

## RESOLVED — BSOD (O9) + boot first-mount/SD-C6 ordering (O10) fixed; httpd-needs-WiFi (see below)

- **O3. ✅ FIXED — per-append transcript span rebuilds went O(buffer); deferred batching landed (2026-09-08 session).** Root cause as before (`shell_transcript_append_internal()` repainted LVGL spans O(buffer) per append; drain bench 0.9 KB/s full-buffer). Fix shipped: `P4_CONFIG_TRANSCRIPT_FLUSH_MS=200` deferral (`shell_transcript_defer_begin/end`, `shell_transcript_flush_now`, `shell_transcript_maybe_update_label`), PORT-outer/BUF-inner buffer mutex, segment batching in `command.c` — drain now 19–29 KB/s both runs, deep 8/8, modal/edit/db green. Follow-up 30-min `tools/stall_catch.py` run (740 probes, echo-vs-output timing, glue-tolerant matcher): zero queue-fulls, zero heap warns, db/sd typically 0.0–0.2 s, three output-absence episodes — two traced to bad expects (`db.databases`/`fs_total` vary by output shape), one genuine single-output loss (~1/740, TX-heavy window, self-healed). **2026-09-12 (v0.37.1): residual root-caused and fixed.** The IDF USB-Serial/JTAG `usb_serial_jtag_is_connected()` is a SOF-tick monitor that can falsely report "disconnected" for ~4 ms under host/load pressure; both our mirror's early-return and the IDF VFS write path (`usb_serial_jtag_vfs.c:185`) drop the whole line when it flips — the exact single-line loss. `shell_uart_console_write_text()` now mirrors through the driver API (`usb_serial_jtag_write_bytes`, which does not consult the monitor) with CRLF preserved, and gates only on a *persistent* disconnect (2 s grace) so a transient flip never drops a line while a real no-host case still stops the mirror without blocking. `tools/tx_stress_test.py` (200+ numbered lines with deliberate read-pause backpressure) shows zero loss. The old-queue-full storm signature (db5/db6, pre-fix firmware) has not recurred.
- **O4. ✅ FIXED — host tooling rebooted the board on every port open (2026-09-07 session).** Opening COMx with pyserial asserted DTR and the P4 reset on the transition (proven: uptime 2m53s → 0m1s across a bare reopen); the existing `setDTR(False)`/`setRTS(False)` calls ran *after* `open()`, too late to stop it. **2026-09-12 (v0.37.1): fixed** — `shell_session.open_port()` now constructs the `serial.Serial()` object, sets `dtr=False`/`rts=False`, and only then calls `open()`, so the OS opens the port with both lines low and the board keeps running. Every driver (tools + apps + harness) routes through it; the duplicated inline `serial.Serial(...)+setDTR` blocks were removed. Verified: two consecutive opens leave uptime advancing (no `rst:`/`esp_image` banner). Tools that need a fresh boot call the new `shell_session.hard_reset()` (esptool).
- **O5. ✅ FIXED (trigger removed + firmware recovery) — silent-boot wedge needed a power cycle (2026-09-07 session).** After hours of unclean DTR resets the board stopped past ROM `entry` on both firmware images (black display, USB alive, esptool functional) — suspected a wedged external I2C/PSRAM slave holding a bus; chip resets do not clear powered slaves. Physical unplug/replug recovered fully. **2026-09-12 (v0.37.1):** the O4 fix removes the trigger entirely (host sessions no longer reset the board, so the "hours of unclean DTR resets" cannot accumulate), and `display_init()` now runs a standard I2C bus recovery on the touch/codec bus (clock SCL up to 9 times until SDA releases, then a STOP) *before* the BSP claims the pins, releasing a slave that is holding SDA low across resets. The recovery is a no-op when the bus is idle, so normal boots are unchanged. A wedge inside PSRAM init (bootloader, before app code) remains hardware-only and is out of firmware reach.
- **O6. ✅ FIXED — synchronous SD ops no longer starve behind hosted SDIO (2026-09-12 session).** Root cause proven in the vendor stack: slot 0 (SD) and slot 1 (hosted C6) share one SDMMC controller whose transactions all take a single global mutex with an unbounded wait, while hosted tasks run at 22 vs the shell worker at 2 — so the worker loses every arbitration under sustained hosted traffic (60 s+ stalls with live console echo). Fix: `shell_sd_begin()` boosts the caller to 23 for the session (restored on end + worker backstop); `storage_get_space_info()` retries idempotent reads; `sd info` prints `unavailable` instead of omitting lines on persistent failure. Verified: 20-min soak 1 SD timeout → 0 SD stalls on confirmation soak; suites green. See changelog `## [0.36.0]`.
- **O7. ✅ FIXED — boot heap corruption (LVGL timer-list panic) from a header async double free (2026-09-12 session).** `header_schedule()` freed the async payload when `lv_async_call()` failed and all eight `header_update_*` callers freed it again — the second free corrupted the heap free list, and the next `lv_timer_create` node landed in the damaged region, so `lv_timer_handler -> lv_ll_get_next` jumped to garbage (`0x4800001c` / `Illegal instruction`). Comprehensive heap poisoning also tripped on the same corruption. Fix: `header_schedule()` no longer frees (caller owns the payload and frees on failure; `header_set_notification()` updated to match), plus `header_deinit()` now deletes the armed one-shot timer (LVGL timers are not reclaimed by `lv_obj_clean()`), `header_set_visible()` takes the port lock, and `editor_view_set_blink_ms()` checks `open` under the lock. Verified 0 panics / 0 poisoning asserts in 20+ fresh boots under the load that previously panicked ~1/10. Root cause of the *boot* SD/USB `ESP_ERR_NO_MEM` errors is separate (shared SDMMC DMA/LDO contention) and is addressed in the changelog `[0.36.1]`.
- **O8. ✅ FIXED — boot-time internal DMA heap cliff (v0.37.0).** The board ran with only ~1 KB DMA-capable internal RAM free and a 188-byte largest block at the tightest boot point (after Wi-Fi/hosted + USB init), which injected latency into every late allocation (USB HCD, SD `send_scr`, hosted SDIO). PSRAM is not `MALLOC_CAP_DMA` on this build (`dma_spi=0`), so `MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA` requests silently fall back to internal RAM. Fix: move the app-owned task stacks that never touch host flash (USB `usb_host_lib`/`usb_module`, `c6ota`, `audio`, `alarm`, `led`, `shell_uart`) to PSRAM via `xTaskCreate*WithCaps(..., MALLOC_CAP_SPIRAM)` and use `vTaskDeleteWithCaps` for their self-deletion. The command worker, Wi-Fi tasks and LVGL stay internal (they run NVS/flash-writing commands or are timing-sensitive). Measured tightest point: DMA free **1.0 KB → 10.7 KB**, largest DMA block **188 B → 8.7 KB**, internal free **10.0 KB → 40.0 KB**. Boot scripting also moved off the Wi-Fi event task onto a short-lived dedicated `bootscript` task (`P4_CONFIG_BOOT_SCRIPT_TASK_STACK`). 12-min concurrent Wi-Fi+SD soak: 0 SD stalls (the earlier `sdmmc_read_sectors` timeout residual is below detection); `tools/boot_regression.py` 8/8 clean boots.

- **O9. ✅ FIXED — "BSOD" (full-screen blue flash) was a MIPI-DSI bridge underrun caused by the header telemetry running `uxTaskGetSystemState()` on the LVGL task (2026-09-15 session).** The status bar sampled heap/CPU/battery every `P4_CONFIG_HEADER_TELEMETRY_PERIOD_MS` (5 s) *on the LVGL render task*. `uxTaskGetSystemState()` (a full task-list walk) suspends scheduling long enough to delay the DSI bridge DMA refill ISR; the panel then reads a starved framebuffer and the bridge paints it blue — IDF's own ISR says "when an underrun happens, the LCD display may already becomes blue" (`components/esp_lcd/dsi/esp_lcd_panel_dpi.c`), and IDF 5.5.5 never programs the P4 AXI-ICM QoS. Proven with a new camera detector, `tools/display_glitch_watch.py` (mean blue minus red over the panel ROI): telemetry period → 600000 ms, or the CPU snapshot disabled, gives 0 events/60 s; the shipped firmware glitched ~1–4/60 s and 12/90 s. Fix: (1) `shell_sample_cpu_percent()` now derives load from the idle task's run-time counter (`ulTaskGetIdleRunTimeCounter()` + `esp_timer` deltas — the stats clock is esp_timer microseconds) instead of the full task-list walk; (2) heap/battery sampling moved off the LVGL task to a pinned-core-0 background `sheltlm` task (`P4_CONFIG_TELEMETRY_TASK_STACK`; internal stack for OTA safety), so the header render only reads cached statics; (3) `ps`/`top` keep the on-demand full snapshot. Also added AXI-ICM QoS hardening (`display_apply_axi_icm_qos()`, `P4_CONFIG_DISPLAY_ICM_QOS_*`) and a `display stress on|off` reproducer. Verified 0 events in 120 s + 240 s idle and a full boot (was 12/90 s).
- **O10. ✅ FIXED — "SD card ready" welcome and the first-mount boot work (default files / font / CJK / history) were silently skipped (2026-09-15 session).** `security_init()` read the CONFIG.SYS single store via `config_get_saved()` at init, which mounted the SD card during `command_init()` — before `main` registered the first-mount callback (`storage_register_sd_first_mount_callback`). `storage_sd_mark_mounted()` then set the one-shot flag with a NULL handler, so `boot_on_sd_first_mount()` never ran (baseline `tools/boot_regression.py` 0/10, `sd_ready=False`, `autoexec=1`). Fix: (1) `security_init()` now sets defaults only; the CONFIG.SYS reads moved to `security_load_saved()`, called from `boot_script_apply()` just before `security_engage_boot_lock()`; (2) `storage` re-arms the one-shot when fired with no handler and fires the deferred work on registration; (3) `networking_init()` moved to *after* the boot script, so the SD card mounts before the ESP-Hosted C6 transport claims the shared SDMMC host/DMA buffers — otherwise the C6 SDIO card init retries `sdmmc_allocate_aligned_buf: not enough mem` (the card and the C6 share the SDMMC controller and its DMA-capable internal buffers; see O6/O8). Verified `boot_regression.py` 10/10 clean, Wi-Fi associates + gets an IP, `usb status`/`mem` sane, and 0 display glitches.

- **W1 clarification.** The historical "W1" heap-overrun label was **not** an ESP-Hosted overrun on this build. The corruption that crashed LVGL was the header async double free (O7). Static analysis of esp_hosted 3.0.6 found the SDIO path has out-of-bounds *reads* before bounds checks (not writes), a latent `copy_payload` double free that is gated off by `esp_wifi_remote >= 1.3.1` (we ship 1.6.4), a 32→16-bit DMA length truncation only reachable for >64 KB streaming transfers, and an unbounded fragment-reassembly buffer that is never freed on deinit — none of which the compiler currently reaches. The upstream fixes for these live only in the legacy **2.12.x** line (`2.11.0` buffer double free, `2.12.8` `ESP_HOSTED_MEMPOOL_PREFER_SPIRAM`, `2.12.10` SDIO RX heap corruption, `2.12.11` shared-SDMMC deinit). **3.0.7 (2026-09-05) does not contain them**, and the 3.x branch has no PSRAM transport-buffer Kconfig, so `update-dependencies` to 3.0.7 would drop the tree's local `CONFIG_EH_HOST_PORT_DMA_PREFER_SPIRAM` mitigation and reintroduce the SDIO boot OOM. Decision: stay on 3.0.6; no vendor patches added (app-side mitigation only).

- **O1. ✅ FIXED — `list` immediate close without timeout (0.35.1 companion expansion)** — `list "title" item1 item2` without `/t:secs` previously closed immediately with 255 on serial; now correctly waits for serial number input via `shell.c` `modal_handle_serial_line` `components/modal/modal_surf.c:412` (verified `list 2` → index 2 without timeout, `list` with `/t:secs` → 255 on timeout, companion `FILES.BAT`/`SYS.BAT` list selections all pass).
- **O2. ✅ FIXED — `ask` serial line not consumed (0.35.1 companion expansion)** — `ask "prompt"` typed answer on serial previously remained empty (ERRORLEVEL 1) unless `/t:secs` given; now correctly stores `ASK_RESULT`/`/v:NAME` via `shell.c` `modal_handle_serial_line` (`ask myname` → `myname` `ASK_RESULT`, `/v:NAME` verified, `FILES.BAT`/`NET.BAT` ask prompts all pass). The serial line forward vs key-queue path (`shell_key_wait_submit` vs `modal_handle_serial_line`) is now correctly routed.
- Both O1/O2 fixed in this patch; timeout variants (`/t:secs` + `/v:NAME`) remain the recommended batch form but are no longer required for serial input. No open blockers. Companion fully TUI-expanded and hardware-verified (7 BATs, push_sd.py COM11 PASS), no regressions.

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
- **Boot prints "No SD card detected" yet the card works.** The early boot
  probe can miss while the card still lazy-mounts on first access (batch
  apps run fine on those boots). Cosmetic ordering confusion between the
  eager probe message and the first-mount path, observed 2026-09-10; no
  firmware change.
- **Serial keys need Enter to reach key waits.** The UART console reader is
  `fgets` line-buffered (`shell.c`), so a bare keypress without newline
  never arrives at `choice`/`pause` waits (USB/OSK keyboards deliver raw
  keys). Steering batch games over serial is therefore key+Enter per step
  (proven with SNAKE: `dc=s` turn + `QUIT`); document, don't fix (changing
  stdio buffering is C risk for zero functional gain).
- **`list` serial selection is 1-based, its ERRORLEVEL 0-based.** `q`
  cancels (255). Count items twice when writing `if errorlevel` chains —
  TCMD shipped an 11-item list with a 12-item chain and Quit silently
  refreshed (caught on HW, fixed).
- **`dir` parses a leading-`/` path as switches** (`dir /APPS` reads `/A`
  attributes). Absolute SD paths need the `sd:` prefix or a `cd` first.
  Quirk, documented in `command.md`; TCMD dropped its per-refresh counts
  over it (panes list via `browse`).
- **APM-560 operational note (ESP32-P4 errata, this board rev v1.x).**
  Unauthorized concurrent AHB access to PSRAM/flash wedges later traffic
  until system reset — so one display writer at a time (bg display refusal),
  no OTA overlapping PSRAM bg stacks, serialized SDMMC bring-up (N1 gate),
  sequential host tools. I2C-308 (slave-only), RMT-176 (IDF-bypassed),
  ECDSA-837 (unused flow) need no action.
- **`set /a` prints its result in this shell** (by design, even with `@echo
  off`), so a `for`-loop counter (`for /f %%a in (f) do set /a N+=1`) prints
  one line per iteration and floods the transcript. The file manager instead
  asks the `draw list /count:VAR /countonly` verb for the line count.
- **`draw table` dropped the last column (fixed).** The pad-short-rows loop
  reused the split cursor `c` after it had been left on the last filled
  index, so it blanked cell `ncols-1` for every full row — a 3-column table
  rendered 2. Fixed by advancing `c` before padding; caught by measuring a
  HW screenshot, not by unit tests (which only covered widths).
- **Batch files are now RAM-resident during execution (128 KB cap).** Each
  frame reads its whole script into PSRAM and executes from memory, so
  `goto`-heavy loops stop hitting SD. The reader mirrors `fgets` semantics
  (stop after size-1 bytes, newline, or EOF) and `tell`/`seek` are byte
  offsets, so line-continuation, `call :label` resume, and label positions
  behave identically; larger files transparently stream from SD. Cost: up to
  `P4_CONFIG_BATCH_FILE_MAX_BYTES` PSRAM per nested frame (capped at depth 4).
- **`alarm_test` barrier flake.** One run reported `FAIL(scheduled)` though
  the alarm line was `alarm: 3 scheduled ...` (the DONE barrier matched
  before the SD-bound `alarm add` reply flushed); it passed 25/25 on the
  immediate re-run. Same O6 slow-SD family, not a regression. The B1 `pkg`
  round-trip run reproduced the same family once (`FAIL(deleted)` on the
  first `alarm del all` of `alarm_test`, green on re-run).
- **`pkg install` verifies before it copies; `pkg remove` trashes.** Pass 1
  CRC-checks every `PKGS/<APP>/` payload and aborts without touching installed
  files on any miss/mismatch; pass 2 copies payloads at their install-relative
  paths, then copies `<APP>.APPINFO` + `<APP>.ASSETS` into `APPS/`. `pkg
  remove` sends every manifest payload plus the two metadata files through
  `storage_trash_delete_file()`, so `undelete` recovers an uninstalled app.
  `pkg info <app>` on a missing manifest prints `manifest: ... (missing)` and
  returns EL 1, doubling as an existence probe; a manifest that lists
  `APPS/<APP>.APPINFO` as a payload makes the metadata copy idempotent.
- **Gfx toolkit text is a committed bitmap font; flood fill grows a PSRAM
  stack.** `gfx text` renders an 8x8 ASCII table (`components/gfx/gfx_font.c`,
  generated by `tools/gen_gfx_font.py` from the public-domain unscii-8 TTF)
  rather than an LVGL font, so the raster core stays LVGL-free and unit-tested
  and `gfx save` includes the glyphs. `gfx fill` allocates its seed stack from
  PSRAM and grows it (OOM stops the fill but never corrupts the canvas). The
  HW pixel test compares against the RGB565 round-trip (`docs`: `gfx` keeps
  full-565 precision, unlike `draw`), so expected values are quantized, not
  the raw 24-bit input.
- **Theme switching re-applies the themed surfaces; two minor bits lag.**
  `theme set` recolors the screen/transcript/input row/keyboard and the header
  bar/panels and re-renders the header labels immediately. The two `|`
  separators between the header panels and any already-open modal keep their
  old colors until the header is rebuilt / the modal reopened. No functional
  impact; noted so post-switch screenshots are read correctly.
- **`p4_usb` removed; `components/usb` owns both halves.** The shell-facing
  `usb.c`/`usb.h` used to be compiled by a separate `p4_usb` CMake wrapper over
  `../usb/usb.c`. It now compiles inside `components/usb` (alongside the
  vendored host stack) with `ansi`/`usb_host_hid`/`usb_host_msc` in REQUIRES;
  `usb status` reports `usb.host: ready` on hardware.
- **Black screen after a unit-test flash is the test app, not a regression.**
  `test/` builds `p4minishell_tests`, which speaks only over serial (Unity
  output) and never builds the LVGL shell UI. If the display is black but the
  `PS /sdcard>` prompt answers on UART, check the boot banner's project name
  first (`p4minishell_tests` vs `p4minishell`) and reflash from the repo root.
  Seen 2026-09-11: board still ran the test app; reflashed main, display back.
- **`plot` sampling binds X/T through the environment (restored after).**
  Function/polar/parametric sampling sets the `X`/`T` env var per sample and
  restores the prior value, so a pre-existing `X`/`T` survives a plot. Sample
  phase matters for exact-pixel checks: with N samples the curve crosses
  integer pixels between samples, so HW pixel tests assert regions/counts
  near crossings rather than single pixels. DOS bright colors apply on canvas
  (e.g. 11 is `0x55FFFF`, not `0x00FFFF`) — pixel expectations must go through
  the RGB565 quantization (`q()` in the drivers).
- **Boot UI restore is retry-safe now.** `font_restore_saved()` reports success;
  a failed first-mount restore re-arms the one-shot (`storage_sd_first_mount_reset()`)
  so the next mount retries, and `app_main` applies the saved theme/header
  mode/fonts again after boot scripting. Previously a single flaky VFS read at
  boot silently skipped the restore for the whole session (`theme_test`
  persistence flaked because of it).
- **Screenshot captures can show a horizontal wrap artifact** (right-edge
  pixels appearing at the far left). Seen on `grab_screenshot.py` BMPX frames
  while LVGL metrics prove the layout is correct — treat screenshots as
  approximate for absolute positions and rely on `header status` metrics for
  geometry assertions.

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
| Medium | 48, all fixed (M1-M48). Individual entries below carry the root cause and fix; the long enumeration was trimmed from this summary row. |
| Open | 0 (O3 O4 O5 all fixed in v0.37.1; see OPEN section) | O3 single-output loss fixed (driver-API mirror bypasses the SOF false-disconnect); O4 fixed (no-reset port open); O5 trigger removed (O4) + I2C bus recovery. O1/O2/O6/O7/O8 fixed; W1 clarified as the header double free, not a hosted overrun. |
| Low / observations | ~20 (benign i2s log, `pwd`, transcript span footprint, SD lazy-mount message, serial key+Enter, `list` 1-based vs EL, `dir` leading-`/`, APM-560 note, `set /a` prints, `draw table` column fix, batch RAM cap, `alarm_test` flake, `pkg` install/remove semantics, gfx toolkit font/flood-fill, theme re-apply lag, `p4_usb` fold, test-flash black screen, plot sampling, boot-restore retry, screenshot wrap) | see LOW / OBSERVATIONS |
| Network | 1 (N1 shared-SDMMC bring-up) | Fixed (serialize sdmmc_host_init + slot-scoped SD deinit + hosted retry) |
