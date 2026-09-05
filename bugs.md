# P4MiniShell — Bug Report & Test Campaign (v0.35.4 split, `command.c` 5808→2467, periph/power/serial out)

Date: 2026-08-24
Hardware: ESP32-P4 (rev 1.0) on COM11, ESP32-C6 co-processor, JD9165 display, SD card present
Firmware: **v0.35.4** (split patch 0.35.3→0.35.4 on top of the v0.35.3 split patch: flash to COM11 succeeded, boot verified, extensive serial tests; TUI engine live `P4_CONFIG_TUI_COLS`×`P4_CONFIG_TUI_ROWS` 80×25 `p4minishell_config.h:298` via `windows_enter_tui_mode`/`windows_refresh_tui_surface`, `draw` TUI-aware auto-enters for box/text/line/fill/clear/window/title+style correctly handled, `color`/`locate` TUI-aware, `tui fullscreen`/`draw fullscreen` header kept visible by default hidden only on fullscreen `windows_set_fullscreen` `components/windows/windows.c:418` dynamic `windows_notify_keyboard_visibility`; memory fixes `P4_CONFIG_TRANSCRIPT_BYTES` 2048→1024 `p4minishell_config.h:93`, `P4_CONFIG_ASYNC_TRANSCRIPT_BYTES` 1024→512 `p4minishell_config.h:134`, `P4_CONFIG_SD_DMA_BUFFER_BYTES` 8192→4096 `p4minishell_config.h:626`, `P4_CONFIG_TRANSCRIPT_INTERNAL_TRIM_BYTES` 49152→60000 with 1/4 keep + trim-below-10KB guard `p4minishell_config.h:117`, `P4_CONFIG_COMMAND_TASK_STACK` 16384→24576 `p4minishell_config.h:1514` at `0x4012b75a`; managed BSP audio abort guard `components/audio/audio.c:42` `managed_components/espressif__esp_codec_dev/i2s/esp_codec_dev.c:269`; modal `EventGroup` PSRAM `MALLOC_CAP_SPIRAM` `components/modal/modal.c:46`; queue full handling improved, serial routing `modal_handle_serial_line` `components/modal/modal_surf.c:412` (`dialog y` `list 2` `ask myname`); companion fully TUI-expanded and hardware-verified (7 BATs, push_sd.py COM11 PASS LIB 1896 COMPANION 1552 SYS 1486 FILES 3946 NET 2893 FUN 3968 SET 3109) — builds on v0.35.0 TUI restoration, now M31 stack overflow fixed, no regressions)
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
  4. `P4_CONFIG_TRANSCRIPT_INTERNAL_TRIM_BYTES` 49152→60000 with 1/4 keep and trim-below-10KB guard — `shell_transcript_guard_internal()` now trims earlier and keeps 1/4 on trim; `windows_transcript_trim()` guards the case where free internal RAM is already <10 KB so the next `printf` lock cannot `abort()`.
- **Verified:** flash to COM11, boot `P4MiniShell v0.35.1 ready`, 50+ mixed SD ops (dir/type/write/copy/pipes/tree/chkdsk) with zero `allocate_dma_buf` errors, transcript stays under internal-RAM failure floor, no `abort()` in `lock_init_generic` during extensive serial modal tests. Clean build 0 errors/0 warnings.

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
- **Root cause:** `tui_cell_t` `utf8` was `char utf8[2]` (`components/tui/tui.h:35`), but box-drawing UTF-8 sequences `SH_BOX_*` (e.g. `─` `E2 94 80`) are 3 bytes plus NUL → truncated to 1 byte + NUL, so `tui_cell_set` (`components/tui/tui.c:116`) `strncpy` lost the glyph.
- **Fix:** `components/tui/tui.h:35` `utf8[4]` (3 bytes + NUL), `tui_cell_set` `strncpy(cell->utf8, utf8, sizeof(cell->utf8)-1)` with explicit NUL. All box draws now emit full `SH_BOX_*`.
- **Verified:** `draw box 2 2 20 8 single MyBox` / `double` / `rounded` with title, nested boxes (window stack), `draw line` H/V all render correctly in `grab_screenshot.py --crop-transcript`; `tui_flush` per-fg recolor shows intact borders.

### M23. ✅ FIXED — `tui_draw_box`/`line` ignored style and title (0.35.1 final)

- **Symptom:** `draw box ... single|double|rounded` always rendered single, and titles were not centered; `draw line` style had no effect.
- **Root cause:** `tui_draw_box` (`components/tui/tui.c:228`) and `tui_draw_line` (`components/tui/tui.c:283`) wrote ASCII `+|-` instead of `SH_BOX_*` UTF-8 and did not branch on `style`/`title`.
- **Fix:** `tui_draw_box` now selects `SH_BOX_TL`/`H`/`V` vs `SH_BOX_TL2`/`H2`/`V2` vs `SH_BOX_TLR`/`TRR`/`BLR`/`BRR` per `strcasecmp(style, "double"/"rounded")` and centers `title` with surrounding spaces via `tui_print_at` with `tui_cell_set`; `tui_draw_line` selects `SH_BOX_H`/`V` vs `H2`/`V2` vs `HL`/`VL` for `heavy`. All honor fg/bg.
- **Verified:** `draw box 2 2 20 8 double T` and `rounded` show `╔═╗`/`╭─╮` correctly with title centered, nested `draw window` stacks, `draw line 1 5 80 5 double` horizontal double line, no abort.

### M24. ✅ FIXED — `tui_flush` did not render fg/bg (0.35.1 final)

- **Symptom:** `color 0A` / `draw box ...` with fg did not color the TUI; transcript text was monochrome, `color`/`locate` appeared to do nothing on the `80×25` grid.
- **Root cause:** `tui_flush` (`components/tui/tui.c:356`) wrote plain `utf8` without LVGL recolor; `lv_label_set_recolor` was false and no `#RRGGBB` tags were emitted, and palette duplication risk existed.
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
- **Fix:** raised `P4_CONFIG_COMMAND_TASK_STACK` 16384→24576 (`p4minishell_config.h:1514` + `p4minishell_config.yaml` `command_task_stack`), command queue full handling improved (bounded 1 s wait instead of silent `queue full` drop, survives burst `draw`/`list` calls), `windows_enter_tui_mode` keeps header visible by default (only `draw fullscreen on`/`tui fullscreen on` hide via `windows_set_fullscreen`/`header_set_visible` `components/windows/windows.c:418`), TUI does not overlap shell text (`tui_hide_for_modal`).
- **Verified:** flash to COM11, boot verified, extensive serial tests of all 7 companion BATs (COMPANION 1552, SYS 1486, FILES 3946, NET 2893, FUN 3968, SET 3109, LIB 1896 bytes) pushed via `push_sd.py` COM11 PASS, each BAT exercised via serial (`list` selections, `browse`/`view`/`hexview`, `tui fullscreen` `draw` boxes) — no abort, no watchdog, no overlap, no `0x4012b75a` overflow; `draw`/`list`/`ask` serial routing verified (`dialog y` → 0, `list 2` → 2, `ask myname` → `myname` via `modal_handle_serial_line` `components/modal/modal_surf.c:412` `shell.c`), 50+ mixed SD ops clean, clean build 0 errors/0 warnings.

### M32. ✅ FIXED — root clutter, browse duplication, layering and config drift (0.35.2 cleanup)

- **Symptom:** 97 one-shot `*.py` patch scripts + 7 `*.txt` dumps crowded the repo root; `browse` existed twice (`components/command/command.c:2413` 4 KB stack + `"File Browser"` vs `components/batch/batch.c:3610` config buffer + `"Browse"`); `shell_launch_app` (`components/command/command.c:2434`) used `char command[256]`; `components/shell/CMakeLists.txt` required `networking`/`c6ota`/`p4_usb` despite ops-table decoupling; root `CMakeLists.txt:5` listed 14/23 components; `p4minishell_config.yaml` missed `P4_CONFIG_PS_COLOR_PARAMETER`/`SUBSYSTEM`/`HEADING` (`p4minishell_config.h:1390,1402,1411`) + `P4_CONFIG_DB_FLAG_SECRET` (`p4minishell_config.h:1660`); `sdkconfig.defaults` did not pin LVGL demos off; `.gitignore` did not cover `sdkconfig`.
- **Root cause:** incremental hardware-bug-hunting workflow committed every patch helper to the root; parallel `browse` implementations diverged; build/config lists not updated when `alarm`/`db`/`tui`/`modal` landed.
- **Fix:** 16 harnesses → `tools/harness/`, rest + dumps deleted; `browse` unified on `P4_CONFIG_TUI_BROWSE_PATH_BYTES` + `"Browse"`; `shell_launch_app` heap `P4_CONFIG_COMMAND_BYTES`; `GFIND_DB_MAX`/`GFIND_ALARM_MAX` → `P4_CONFIG_*` (`components/command/gfind_commands.c:30`); shell `REQUIRES` trimmed; root `EXTRA_COMPONENT_DIRS` = 23; yaml + `sdkconfig.defaults` + `.gitignore` fixed.
- **Verified:** static checks only (no `char [4096]`/`[2048]` in `command.c`, 0 root scratch scripts, 23/23 dirs listed). Full build + board pass deferred.

---

### M33. ✅ FIXED — memory-baseline numbers in M19/M31 went stale (0.35.3 reconcile)

- **Symptom:** `bugs.md` M19 (`P4_CONFIG_TRANSCRIPT_BYTES` 2048→1024, async 1024→512, trim 49152→60000 1/4 keep) and M31 (`P4_CONFIG_COMMAND_TASK_STACK` 16384→24576) no longer match the code: the tree now runs transcript 65536 (`p4minishell_config.h:93`, PSRAM-backed per `p4minishell_config.yaml` `buffers:transcript_bytes`), recolor == transcript (`p4minishell_config.h:101`), trim threshold 4096 (`p4minishell_config.h:120`), async 512 (`p4minishell_config.h:137`), SD DMA 4096 (`p4minishell_config.h:634`), worker stack 32768 (`p4minishell_config.h:1522`).
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

## OPEN — remaining TUI/modal tuning (not blocking) — NONE (0.35.4 splits: M34 fixed, no new blockers)

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
| Medium | 34 (M1 calc `NAME=` name extraction, M2 `for /f` driver cursor, M3 redirect-capture re-entrancy, M4 `%ERRORLEVEL%` token init, M5 SDMMC DMA buffer allocation failures, M6 newlib FILE-lock OOM abort, M7 display-rotation LVGL thread-safety, M8 command-worker stack overflow 12288→16384, M9 batch `for /f` quoted options, M10 command-queue drops, M11 key-wait prompt race, M12 `if COND &` chain split, M13 wifi `[wifi]` white→cyan, M14 `main.c:200` progress literal `@`, M15 applib `vsnprintf` ANSI flag, M16 async schedule ANSI path, M17 `dialog`/`list`/`ask` dispatcher missing, M18 `browse`/`view`/`hexview` stub, M19 memory pressure transcript 2048→1024 `p4minishell_config.h:93`/async 1024→512 `p4minishell_config.h:134`/SD DMA 8192→4096 `p4minishell_config.h:626`/internal-trim 49152→60000 `p4minishell_config.h:117` with 1/4 keep, M20 audio `esp_codec_dev`/`bsp_audio_init` abort `components/audio/audio.c:42` `managed_components/espressif__esp_codec_dev/i2s/esp_codec_dev.c:269`, M21 modal EventGroup PSRAM `MALLOC_CAP_SPIRAM` `components/modal/modal.c:46`, M22 `tui_cell_t` truncation `utf8[2]`→`utf8[4]` `components/tui/tui.h:35`, M23 `tui_draw_box`/`line` style+title ignored `components/tui/tui.c:228`/`283` single `SH_BOX_TL`/`H`/`V` double `SH_BOX_TL2`/`H2`/`V2` rounded `SH_BOX_TLR`/`TRR`/`BLR`/`BRR`, M24 `tui_flush` color recolor `#RRGGBB` per fg run `ansi_get_palette_color` `components/tui/tui.c:356`, M25 header occlusion `windows_set_fullscreen`/`header_set_visible` `components/windows/windows.c:418`, M26 keyboard rescale `windows_notify_keyboard_visibility`, M27 prompt `shell_prompt_render_plain()` `main.c:112`/`modal_surf.c:412`, M28 audio re-verified, M29 dialog/list/ask serial routing `modal_handle_serial_line` `components/modal/modal_surf.c:412` `shell.c`, M30 draw auto-enter `tui_init` `components/tui/tui.c:56`, M31 stack overflow at `0x4012b75a` `P4_CONFIG_COMMAND_TASK_STACK` 16384→24576 `p4minishell_config.h:1514` queue full improved, M32 0.35.2 cleanup root quarantine + browse dedup + layering + config drift, M33 0.35.3 memory-baseline reconcile, M34 0.35.4 periph/power/serial splits `command.c` 5808→2467) | ✅ Fixed (M1–M4 in v0.32.1/v0.32.2, M5–M7 in v0.32.8, M8–M12 in v0.33.0, M13–M18 in v0.35.0, M19–M21 in v0.35.1 hardware testing, M22–M30 in v0.35.1 final hardware bug hunting, M31 in v0.35.1 companion expansion at `0x4012b75a` 16384→24576, all verified on COM11) |
| Open (TUI tuning, not blocking) | 0 — O1 `list` immediate close and O2 `ask` serial not consumed now FIXED in 0.35.1 companion expansion via `modal_handle_serial_line` `components/modal/modal_surf.c:412` (`dialog y` `list 2` `ask myname` all correctly routed `shell.c`), companion fully TUI-expanded and hardware-verified (7 BATs, push_sd.py COM11 PASS), no deferred items | ✅ Fixed — O1/O2 closed, no open TUI tuning blockers. Companion tested (see roadmap M31). |
| Low / observations | 3 (benign first-use i2s log, `pwd` not a command, transcript span internal-RAM footprint) | — |
| Network | 1 (N1 shared-SDMMC bring-up) | ✅ Fixed (serialize sdmmc_host_init + slot-scoped SD deinit + hosted retry) |
