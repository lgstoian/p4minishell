# P4MiniShell AI Context Rules

## Project Identity
- **Name**: P4MiniShell
- **Type**: Embedded DOS-style command shell
- **Target**: ESP32-P4 (host) + ESP32-C6 (co-processor over ESP-Hosted SDIO)
- **Framework**: ESP-IDF v5.5.5
- **UI**: LVGL 9.4.0 with JD9165 1024x600 display + GT911 touch

## Mandatory Reading Before Any Change
1. changelog.md - version history and recent changes
2. readme.md - project overview and current behavior
3. documentation.md - technical architecture
4. ai-context.md - this file (project rules)
5. board_config.yaml - hardware configuration
6. p4minishell_config.h - centralized config values
7. p4minishell_config.yaml - config documentation
8. command.md - command reference
9. sdkconfig - current build configuration
10. main/idf_component.yml - component dependencies
11. For roadmap work: also read roadmap.md and licence.md

## Configuration Rules
- ALL tunable values MUST live in p4minishell_config.h, never hardcoded in source
- p4minishell_config.yaml MUST be updated whenever a config value changes
- Use P4_CONFIG_* macros for new code; SHELL_* aliases exist for backward compatibility
- USB HID key codes are standard USB HID usage table values and stay local to usb.c
- board_config.h remains the hardware-pin source of truth (GPIO assignments, display timing)
- sdkconfig remains the ESP-IDF build-configuration source of truth

## Source Code Rules

### Comments
- All major sections need descriptive comments explaining purpose and behavior
- Use Doxygen-style @file, @brief, @param for public APIs
- Section markers should describe what follows, not just repeat function names
- NEVER use `// AI:` prefix in comments; use plain descriptive text
- Existing `// AI:` comments must be replaced with equivalent descriptive text

### Audit and Hardening Rules
- Before every release: scan all source files for `// AI:` comments and replace them
- Build must produce ZERO errors and ZERO warnings on target toolchain
- No build artifacts in repository; .gitignore must cover build/, sdkconfig, and generated files
- All hardcoded strings must reference p4minishell_config.h macros
- Public API functions must have Doxygen documentation in their header files
- Cross-component calls must go through documented bridge functions (p4minishell.h)
- Semaphore/mutex acquisition must always have a corresponding release path
- Buffer operations must bounds-check before memcpy/snprintf
- NULL pointer checks required before dereference in all public API functions
- All `lv_async_call` return values must be checked; free payload on failure
- All `lv_textarea_get_text()` return values must be NULL-checked before use
- Forward declarations without implementations are a critical bug; never commit them

### Header Bar Rules
- All `header_update_*()` functions MUST update internal state immediately before scheduling async render
- Header state writes (bool/int) are atomic on this platform and safe from any task context
- If LVGL async dispatch fails (lv_async_call returns error), fall back to synchronous header_render()
- If allocation fails for the async payload, fall back to synchronous header_render()
- SD indicator uses consistent HEADER_SD_SYMBOL in both mounted and unmounted states
- SD indicator shows persistent state (NO/INS/ON/ERR)
- Battery is ALWAYS visible — shows "BAT N/C" with muted styling when ADC is not connected
- System panel (MEM | CPU | BAT) is on the far right, all dynamically linked to FreeRTOS runtime stats
- header_update_battery(int percent, bool adc_ready) — pass adc_ready=false for N/C display
- header_update_mem(uint32_t free_heap, uint32_t total_heap) — real-time from heap_caps
- header_update_cpu(int percent, uint32_t task_count) — real-time from FreeRTOS runtime stats
- header_update_uptime(uint32_t seconds) — system uptime from esp_timer_get_time()
- Touch init failure must not prevent header rendering (BSP touch is optional)

### Module Layering Rules
- Component dependencies flow ONE WAY: `main` -> `command` -> `batch` -> `storage` -> `shell` -> (`ansi`, `display`, `windows`, `header`, `keyboard`, `clock`)
- `components/shell/` MUST NOT depend on `components/command/`. When the shell core needs a
  command-owned service, add it to `shell_command_ops_t` in `shell.h` and register it from
  `command_init()` via `shell_register_command_ops()`
- `components/batch/` MUST NOT depend on `components/command/`. When the batch engine needs the
  full command pipeline (nested `if`/`for`/pipe/batch lines), route it through
  `batch_command_ops_t` in `batch.h`, registered from `command_init()` via
  `batch_register_command_ops()`
- `components/storage/` MUST NOT depend on `components/batch/` or `components/command/`. It is
  the lowest shell-facing layer and only calls into `components/shell/` and `components/header/`
- Every `shell_command_ops_t` and `batch_command_ops_t` hook MUST be NULL-checked before use;
  both modules must degrade gracefully if called before `command_init()`
- No component may declare `main` as a requirement; that is a layering inversion
- Bridge trampolines (`shell_bridge_*`) are FORBIDDEN. Put the implementation in the owning
  module instead of forwarding across a layer boundary.
- `components/shell/` MUST NOT include `networking.h`, `bluetooth.h`, `usb.h`, or `c6ota.h`.
  External-module state is read through the `shell_command_ops_t` accessors (wifi_*,
  bluetooth_*, usb_*, c6ota_*). If a needed value is missing, add an accessor to the ops
  table rather than reaching into the driver.

### Shell Core Rules
- ALL transcript output MUST go through `shell_transcript_append_text()` / `shell_transcript_appendf()` from `components/shell/`
- ANSI-colored transcript output MUST go through `shell_transcript_append_ansi()` / `shell_transcript_appendf_ansi()` using `@`-prefixed format specifiers
- ANSI color palette is defined in `p4minishell_config.h` via `P4_CONFIG_ANSI_*` macros; never hardcode ANSI color values
- Command output MUST use the semantic palette in `components/ansi/ansi_palette.h`. Never pick
  a raw `@`-specifier at a call site: use `SH_HEAD`, `SH_LBL`, `SH_OK`, `SH_ERR`, `SH_WARN`,
  `SH_MUTE`, `SH_VAL`, `SH_NUM`, `SH_PATH`, and the rest.
- For the common shapes prefer the helpers over hand-built format strings:
  `shell_print_heading()`, `shell_print_field()`, `shell_print_field_num()`,
  `shell_print_ok()`, `shell_print_error()`, `shell_print_warning()`,
  `shell_print_muted()`, `shell_print_usage()`. Each emits its own reset and newline.
- `@k` is pure BLACK and is nearly invisible on the default background. Muted text MUST use
  `SH_MUTE` (`@K`, bright black). This was a real readability bug fixed in v0.22.0.
- Other specifier traps: `@B` is bold not blue, `@E` is bright red, `@L` is bright blue
- Apply colour AFTER any width or padding calculation. Escape bytes count toward `strlen()`,
  so colouring before padding silently breaks column alignment in listings and reports.
- Machine-readable output (`dir /b`, redirected pipeline stages) MUST stay uncoloured
- Adding a new colour category means adding a macro to `ansi_palette.h`, never inlining a
  specifier. The palette is compile-time only: no runtime theming or user configuration.
- Plain transcript appends mirror to UART; ANSI appends must NOT mirror the stripped copy or
  every colored line prints twice on the serial console
- The on-screen transcript is an LVGL span group (`lv_spangroup`), not a textarea. Coloured
  output reaches it through `windows_set_transcript_text()` (in `components/windows/`), which
  parses the raw ANSI text into per-colour spans. That rebuild deletes/recreates spans, so it
  is DEFERRED to an `lv_async_call` to avoid a use-after-free when the rebuild is triggered from
  an LVGL event or during a redraw pass. Never call LVGL textarea APIs on the transcript, and
  never rebuild the span group synchronously from an LVGL event context.
- The transcript is a scrollable CONTAINER holding the span group. The span group is sized to
  its exact wrapped content height (`windows_transcript_update_content_size()`), never
  `LV_SIZE_CONTENT`: a content-sized child inside a scrollable container makes
  `lv_obj_update_layout()` loop forever (the `scr->scr_layout_inv` while-loop), freezing the
  LVGL task. An explicit pixel height keeps the widget in `LV_SPAN_MODE_FIXED` so layout
  converges in one pass while the container still scrolls.
- Transcript follow policy: new output pins the view to the bottom only when the view is within
  `P4_CONFIG_TRANSCRIPT_SCROLL_FOLLOW_PX` of the bottom. Submitting a command
  (`shell_force_transcript_scroll_to_end()`) sets a one-shot force-follow flag so the command's
  output is always visible; the flag is consumed by the next apply so later background output
  reverts to near-bottom following. Scrolling is available from the input-row `Up`/`Dn` buttons,
  USB keyboard PageUp/PageDown (`windows_scroll_transcript_by()`), and the USB mouse wheel
  (routed to the LVGL task through the `usb_host_scroll_transcript()` bridge in main.c).
- ALL debug logging MUST use `shell_record_errorf()` / `shell_record_warningf()` / `shell_record_infof()`
- Command history MUST use `shell_store_command_history()` / `shell_recall_history()`
- UART console MUST use `shell_uart_console_start()` / `shell_uart_console_write_text()`
- The UART console task assembles partial reads: USB-Serial-JTAG delivers one logical line across
  several reads (its RX FIFO is 64 bytes), so a read without a line terminator is NOT a complete
  command. Buffer the fragment and keep reading. During a key wait, forward the first newly-read
  character immediately (do not wait for the terminator).
- Input line text MUST go through `shell_input_line_set_text()` / `shell_input_line_reset()` /
  `shell_extract_input_text()`; never manipulate the prompt prefix directly
- System info commands (help, sysinfo, version, about, mem, debug) live in `components/shell/`
- Interactive keypress waits MUST use `shell_key_wait_begin()` / `shell_wait_for_key()` /
  `shell_key_wait_end()`. Every `begin` needs a matching `end` on every return path.
- Every keypress wait MUST be bounded. Pass `P4_CONFIG_KEY_WAIT_TIMEOUT_MS` unless the command
  defines its own timeout, and check `shell_key_input_available()` first so a headless board
  falls back to a timed path instead of stalling.
- Input sources (UART reader, USB HID bridge, LVGL input line) MUST check
  `shell_key_wait_is_active()` and route to `shell_key_wait_submit()` instead of the command
  line while a wait is running
- The prompt is a runtime template owned by `components/shell/`. Never hardcode the prompt
  string in a new surface; call `shell_prompt_render_plain()` (plain) or let
  `shell_uart_console_print_prompt()` handle the colored form.
- `shell_input_line_set_text()` snapshots the prefix it paints. Extraction and repair compare
  against that snapshot, never against a freshly rendered prompt.
- The async transcript buffer MUST be drained into a local copy before appending; never hold
  `s_async_transcript_lock` across an LVGL call
- The async transcript flush scratch is heap-allocated (it can be up to
  `P4_CONFIG_ASYNC_TRANSCRIPT_BYTES`, which is too large for the main task stack). Before the UI
  exists (`windows_get_transcript()` returns NULL — unit tests, very early boot) the flush runs
  synchronously instead of via `lv_async_call`, so an uninitialized LVGL heap is never touched.
- Overlapping array slots MUST use `memmove()`, never `snprintf()` (triggers `-Werror=restrict`)
- `shell_transcript_append_internal()`, `shell_transcript_reset()`,
  `shell_history_transcript_scroll_to_end()`, and `shell_input_line_set_text()` all call LVGL
  functions internally. These functions MUST acquire `lvgl_port_lock(0)` around their LVGL
  sections when called from non-LVGL tasks. The mutex is recursive, so the LVGL event path,
  async transcript flush callback, and UART-console command path — which already hold the
  lock — nest without deadlock. The lock is only taken when the LVGL port is initialized
  (guarded by transcript/input-line non-NULL checks).

### Command Rules
- `main.c` must contain NO command implementations
- There is exactly ONE dispatcher: `shell_execute_command_core()` in `components/command/command.c`.
  Never add a second dispatch table.
- Command implementations live with the module that owns their domain:
  - Filesystem verbs (`cd`, `dir`, `copy`, `move`, `del`, `ren`, `mkdir`, `rmdir`, `type`,
    `write`, `append`, `touch`, `attrib`, `label`, `xcopy`, `find`, `findstr`, `more`, `tree`,
    `fc`, `comp`, `sort`, `sd`, `undelete`, `restore`, `trash`, `recycle`) -> `components/storage/storage_commands.c`
  - `xcopy` implements the full DOS 6.x switch set (`/S /E /I /Y /-Y /D[:date] /H /R /K /C /Q
    /T /F /L /A /M /U /P /W /N /V`); its recursive walker keeps each level's state in ONE heap
    block and must never re-enter the command. `findstr` and `comp` are separate commands
    (never split `find`); their pure matcher helpers (`shell_fsre_search`,
    `shell_findstr_match_line`, `shell_comp_first_diff`) are exposed in `storage_commands.h`
    for the unit tests. All of `find`, `findstr`, `more`, `fc`, `comp`, `sort`, and `xcopy`
    return an int ERRORLEVEL (0 ok/found, 1 not found/different, 2 usage) that the dispatcher
    records with `batch_set_errorlevel()`.
  - `del`/`erase` and `rd /s` move entries into the hidden `.trash` recycle bin
    (`components/storage/trash.c`) by default; `/p`/`/f` delete permanently. All of
    `del`, `rd`, `format`, `disk`, `undelete`, `trash` return an int ERRORLEVEL (0/1/2)
    that the dispatcher records with `batch_set_errorlevel()`. Recursive and
    volume-destructive operations are gated by `shell_confirm_destructive()`
    (`P4_CONFIG_DESTRUCTIVE_CONFIRM_WORD`, default `YES`); the gate refuses when
    `shell_key_input_available()` is false so batch files can never trigger it.
  - `find` is ONE command with two modes: the classic text search (`/I /N /C /V`) and a
    recursive file-discovery mode selected automatically by any discovery switch
    (`/NAME:`, `/SIZE:`, `/NEWER:`, `/OLDER:`, `/DIRS`, `/B`, `/S`). Never split it into
    separate commands. The `/SIZE:` primary syntax MUST use `N-M`/`N-`/`-M`/`N` ranges
    (redirection-safe) because bare `>`/`<` are the shell's input/output operators. The
    discovery walker reuses the `dir /s` FATFS primitives, keeps each recursion level's state
    in one heap block, respects `P4_CONFIG_DIR_RECURSE_DEPTH_MAX`, and caps output at
    `P4_CONFIG_FIND_MATCH_MAX`.
  - Batch language verbs (`set`, `path`, `echo`, `call`, `if`, `goto`, `shift`, `pause`,
    `choice`, `setlocal`, `endlocal`, `exit`) -> `components/batch/batch.c`
  - Alias verbs (`alias`, `unalias`) -> `components/batch/batch.c` (the alias table, the
    `alias`/`unalias` commands, `shell_alias_get`/`set`, and
    `batch_alias_expand_command()` all live here; the command module only dispatches and
    calls the expander at the top of `shell_execute_command()`). Aliases expand ONLY at the
    interactive prompt (`s_active_batch_frame == NULL`), never in batch files. `alias /save`
    writes the profile via the guarded storage session; boot.c auto-runs it after CONFIG.SYS.
    The profile path is `P4_CONFIG_ALIAS_PROFILE` (`ALIASES.BAT`), values are quoted on save,
    and values containing `"` are skipped.
- System info verbs (`help`, `sysinfo`, `version`, `about`, `mem`, `debug`) -> `components/shell/shell.c`
- Task introspection verbs (`ps`, `tasks`, `top`) -> `components/shell/shell.c` (`shell_command_ps`)
- Time / SNTP verbs (`date`, `time`, `timezone`, `sntp`/`ntpsync`) -> `components/clock/clock_commands.c`
- Hardware, UI-query, and remaining system verbs -> `components/command/command.c`
- Screenshot/capture/scr (LVGL screen capture as BMP) -> `components/command/command.c`
- ALL command dispatch MUST go through `shell_execute_command()` (full pipeline) or
  `shell_execute_command_core()` (dispatch only) from `components/command/`
- Variable expansion (`%VAR%`, `%0`, `%1`..`%9`, `%*`) is implemented in `components/batch/` and
  called by `shell_execute_command()`; redirection (`>`, `>>`, `<`) is parsed in command.c, the
  output side is written by `components/storage/`, and the input side is published to the storage
  input-redirection slot
- Pipe stages and the `<` operator MUST publish through `storage_set_input_redirect()`. Never add
  a per-command input argument; the text-processing commands resolve their source through
  `storage_resolve_input_source()` so all three spellings share one code path.
- ANY code that looks for an operator character in a command line MUST use
  `shell_find_unquoted_char()` / `shell_find_unquoted_any()` / `shell_has_unquoted_char()`.
  Never hand-roll a quote check: five surfaces depend on agreeing about what is syntax, and a
  local scan will miss single quotes and caret escapes.
- Quoting rules are `"text"` (groups, expands), `'text'` (groups, literal), `^c` (escapes one
  character). Strip markup with `shell_unescape_in_place()`, never by hand.
- Chain separators MUST be split before variable expansion, and expansion MUST run per segment.
  Expanding first would let a variable containing `&` inject a command. This ordering is a
  security property, not a style choice.
- A single `|` is the pipe operator and MUST NOT be treated as a chain separator, or pipelines
  stop reaching the pipe executor.
- Chain success MUST be tracked per link from the segment executor's return value, never
  re-read from global errorlevel, which may hold a stale value from an earlier line.
- The input-redirection slot belongs to exactly one command. Clear it after dispatch on every
  path, including failure.
- Nested execution contexts (`if`, `for`, pipes, batch lines) MUST re-enter the full pipeline via
  `batch_command_ops_t.execute_command` so they inherit expansion and redirection
- Batch argument mapping is COMMAND.COM-style: the frame stores the script path in `args[0]` and
  the caller's arguments in `args[1]..`, so `%0` = script name, `%1`..`%9` = the arguments, and
  `%*` = the arguments from `%1` onward (never the script name). Any change to the frame layout
  MUST update `shell_expand_variables()` in lockstep.
- `goto :eof` is the implicit end-of-file label and ends only the current batch frame. A pending
  `goto`/`goto :eof` MUST be cleared when a frame returns so it cannot leak into the caller's
  line loop when issued from inside a `for` or `if` body.
- `for` loop sets support literal token lists and a single wildcard pattern; both re-enter the
  pipeline per iteration, so the substituted body buffer MUST be heap-allocated (the loop is on
  the recursive batch path).
- The batch executor and the label scanner MUST agree on where a logical line ends. Both apply
  the same odd-trailing-caret continuation rule; changing one without the other lets a
  continued line register a phantom `:label` and silently corrupt `goto` targets.
- `set /a` operator parsing MUST NOT consume a shell chain separator. A lone `&` is bitwise and,
  a lone `|` is bitwise or, and the doubled forms `&&`/`||` belong to the logical level of the
  expression grammar (and to command chaining at the shell level). The bitwise levels refuse
  `&&`/`||`; the logical levels consume them. From the shell, an expression using `&&`/`||`/`&`/
  `|`/`<`/`>` must be quoted (unquoted they are split as chain/pipe/redirection before the
  command runs). Comparisons `== != < > <= >=` yield 1/0 and sit between the logical and bitwise
  levels; `shell_expr_find_assignment()` splits `NAME[OP]=expr` while skipping the `=` of a
  comparison so `set /a x=5==5` assigns x=1 rather than splitting on the `==`.
- Command execution from the LVGL path MUST use `shell_execute_command_async()` to protect the LVGL stack
- Module-routed commands (wifi, bluetooth, usb, c6ota, sd) receive the original unsplit command text
- State ownership: cwd and SD mount tracking belong to `components/storage/` (reset by
  `storage_init()`); environment, PATH, batch frame, and errorlevel belong to `components/batch/`
  (reset by `batch_init()`). `command_init()` calls both before registering the ops tables.

### Clock Rules
- The clock component (`components/clock/`) owns ALL time/SNTP behaviour: the
  C-library clock, timezone, SNTP client, AND the `date`, `time`, `timezone`,
  and `sntp`/`ntpsync` command bodies (`clock_commands.c`). Never implement a
  time command in `command.c` — dispatch there only.
- The clock component is a LEAF (shell -> clock). Its commands must NOT call
  shell helpers directly; they render through `clock_host_ops_t`
  (`clock_register_host_ops()`), registered by `command_init()` with wrappers
  that map onto `shell_print_*` / `shell_record_*` / `shell_text_equals_ignore_case`.
  Pass clock-supplied text to the wrappers as a `%s` argument so a literal
  `%`/`@` stays data. NULL-check every op before use.
- SNTP server hostname is `P4_CONFIG_NTP_SERVER`; the timezone buffer size is
  `P4_CONFIG_TIMEZONE_BYTES`. `time_start_sntp()` is intended to run once lwIP
  is ready; `sntp sync` calls `time_force_resync()` for an immediate exchange.

### Keyboard Manager Rules
- ALL keyboard operations MUST go through `components/keyboard/` — never call LVGL keyboard APIs directly from main.c
- `keyboard_init()` MUST be called after `display_init()` and from the LVGL task context
- Keyboard visibility changes trigger automatic UI reflow via the window manager callback
- When keyboard is hidden, the transcript area expands to fill the freed space
- Keyboard height is configurable via `P4_CONFIG_KEYBOARD_*` macros
- Keyboard modes (text_lower, text_upper, number, symbols) are managed by the keyboard component

### Window Manager Rules
- ALL LVGL screen layout MUST go through `components/windows/` — never create screen-level widgets directly in main.c
- `windows_init()` MUST be called after `display_init()` and from the LVGL task context (inside `bsp_display_lock`)
- `windows_deinit()` MUST be called before rebuilding the UI after rotation changes
- Window objects MUST be accessed via `windows_get_*()` accessors, never stored as static variables in main.c
- All window region dimensions MUST use config macros (`P4_CONFIG_WINDOW_*`), never hardcoded
- Colors MUST be accessed through `windows_get_color()` with semantic names, never raw hex values
- The window manager delegates header rendering to `components/header/` via `header_init()`/`header_deinit()`
- The window manager queries display resolution from `components/display/` via `display_get_width()`/`display_get_height()`

### Display and Touch
- Display init MUST use `display_init()` (which wraps `bsp_display_start_with_config()` with BOARD_CFG_* values)
- ALL display operations MUST go through `components/display/` public API — never call BSP display functions directly from main.c
- Touch init MUST use the managed GT911 driver through BSP (handled internally by display manager)
- Never replace the BSP display/touch path with raw driver calls
- After `display_set_rotation()`, the display manager schedules a UI rebuild via the registered callback
- `shell_build_ui()` MUST call `header_deinit()` before `lv_obj_clean()` for clean header re-init
- Touch rotation mapping is handled automatically by the display manager on rotation change
- Keyboard height scales dynamically (~35% of vertical resolution, clamped 180-280px)
- Input row height scales dynamically (~8% of vertical resolution, clamped 40-56px)
- Header height is rotation-aware (uses correct resolution axis for 90/270)
- Display state variables (rotation, brightness, power) are owned by the display manager and accessed through its public API
- `display_register_ui_rebuild_callback()` MUST be called after `display_init()` to register the shell's UI rebuild function
- The display manager's `display_info_t` struct is the canonical source for sysinfo display diagnostics

### Wi-Fi Rules
- ALL ESP-Hosted, esp_wifi_remote, esp_netif, NimBLE, lwIP-connectivity (ping/DNS), and
  esp_http_client/mbedTLS/TLS calls MUST live inside `components/networking/`. The only
  sanctioned exceptions are `components/c6ota/`, which drives `esp_hosted_slave_ota_*` and its
  own esp_http_client download because co-processor update is its purpose.
- The persistent known-network list (`wifi_known.c` / `wifi_known.h`) belongs to
  `components/networking/`. ALL its SD I/O goes through the guarded storage session API
  (`shell_sd_begin` / `shell_sd_end`) and must tolerate failure at every step: no card,
  missing/corrupt file, read-only or full disk → empty list, no crash, no freeze, no infinite
  retry. The file (`P4_CONFIG_WIFI_KNOWN_FILE`, default `sd:/WIFI.KNOWN`) is written
  atomically (temp + rename) with the storage free-space pre-check and partial-file cleanup;
  FATFS `f_rename` refuses to overwrite, so remove the target and retry before giving up.
  Passwords are stored in the file but NEVER printed to transcript, history, debug log, or
  UART. Keep large buffers off the command-worker / event-task stacks (a ~2 KB array in the
  loader overflowed the 8192-byte worker stack).
- Boot auto-connect runs in the background task after the STA runtime is up, only when
  `WIFI_AUTOCONNECT=ON` (the CONFIG.SYS master switch): load the known list, scan, connect to
  the best visible known network (preferred / highest priority / strongest RSSI), then fall
  back to the classic single-credential path (sdkconfig default or CONFIG.SYS target). The
  chosen target feeds the existing watchdog so retries stay bounded.
- `WIFI_SSID=` + `WIFI_PASSWORD=` in CONFIG.SYS must assemble the pair: the boot-credential
  setter preserves the other field when one argument is empty and seeds the known list when a
  target is configured. Do not break the existing directives.
- `ping` and `dns`/`nslookup` are implemented in `components/networking/networking.c` over the
  lwIP `esp_ping` session and `getaddrinfo`; `components/command/command.c` only dispatches
  them and maps the returned `esp_err_t` onto ERRORLEVEL (0 success, 1 failure, 2 usage). They
  MUST set errorlevel and MUST stay redirectable/pipable like every other command. The ping
  worker-side wait is always bounded by `count * (timeout + interval) + margin`.
- `httpget` / `wget` is implemented as `networking_http_get()` in
  `components/networking/networking.c` over the same `esp_http_client` stack c6ota uses. The
  command layer only dispatches and writes the returned body to SD through the storage write
  path (free-space precheck, partial-destination cleanup). It MUST set ERRORLEVEL (0 = HTTP
  2xx, 1 = failure, 2 = usage), MUST be redirectable/pipable, MUST require an active
  connection (clear DOS-style error when disconnected), MUST buffer the body in PSRAM capped by
  `P4_CONFIG_HTTP_MAX_BODY_BYTES`, and MUST be timeout-bounded so the worker task never hangs.
  Never call `esp_http_client` / mbedTLS / socket APIs from shell/, command/, batch/, or main/.
- When another layer needs networking state, add a status accessor to `networking.h`.
  Never call `esp_wifi_*` from `shell/`, `command/`, `storage/`, `batch/`, or `main/`.
- Only the official path is permitted: `espressif/esp_hosted` + `espressif/esp_wifi_remote`.
  Never introduce custom RPC, an alternative transport, or a re-implemented control plane.
- The initialization order in `networking_wifi_start_runtime()` is load-bearing and MUST
  be preserved: hosted init -> connect to slave -> version gate -> NVS -> netif ->
  event loop -> default STA netif -> esp_wifi_init -> handlers -> STA mode -> start.
  Transport before NVS matters: a dead C6 must report as a transport fault, not as a
  confusing Wi-Fi init error later.
- Station-only is enforced twice: the code always calls `esp_wifi_set_mode(WIFI_MODE_STA)`,
  AND both `CONFIG_ESP_WIFI_SOFTAP_SUPPORT` and `CONFIG_WIFI_RMT_SOFTAP_SUPPORT` are
  disabled. Both symbols are required — esp_wifi_remote mirrors the Wi-Fi Kconfig under
  its own `WIFI_RMT_` prefix, and on the hosted path that mirror is authoritative.
- Wi-Fi startup MUST follow sdkconfig only
- Auto-start in background task on normal boot
- Restore after successful c6ota
- ESP-Hosted + esp_wifi_remote for C6 SDIO path
- Version compatibility gate: refuse init if C6 firmware major/minor != host 2.12.x
- NVS initialized before esp_wifi_init() with erase-and-retry recovery
- Station-only profile; no SoftAP, WPA3, or enterprise
- Password masking in transcript and command history
- ALL shared Wi-Fi state (s_wifi_state, s_wifi_connected, s_wifi_target_ssid, etc.) MUST be accessed through wifi_lock()/wifi_unlock() mutex
- Wi-Fi init task claiming MUST use wifi_try_claim_init_task()/wifi_release_init_task() (TOCTOU-safe atomic)
- Persistent watchdog (networking_wifi_watchdog_task) retries disconnected Wi-Fi with exponential backoff (1s→30s cap, 120s total timeout)
- Watchdog starts automatically on WIFI_EVENT_STA_DISCONNECTED; stops on successful connection or timeout

### Network Services Rules (httpd / netstat / ipconfig)
- The HTTP file server (`components/networking/http_server.c`) is the sole owner of the
  `esp_http_server` surface, exactly as `networking.c` owns `esp_http_client`. It serves the
  SD card via guarded sessions (`shell_sd_begin`/`shell_sd_end`) and VFS `opendir`/`fopen`/
  `fread` chunk streaming (the same SD pattern c6ota uses), never touching FATFS internals
  directly.
- Server lifecycle is tied to Wi-Fi events: auto-start on `IP_EVENT_STA_GOT_IP` (gated by
  `P4_CONFIG_HTTPD_AUTOSTART`) and stop on `WIFI_EVENT_STA_DISCONNECTED`. Every request is
  bounded: auth check first (optional Basic auth, constant-time compare), `..` path segments
  rejected with 400, directory listings capped at `P4_CONFIG_HTTPD_LISTING_MAX`, file reads
  through a heap buffer with `recv`/`send` timeouts. All limits come from `P4_CONFIG_HTTPD_*`.
- `httpget` sends HTTP Basic auth when the URL carries a `user:pass@` prefix
  (`networking_http_url_has_userinfo()` sets `HTTP_AUTH_TYPE_BASIC`), so authenticated
  endpoints can be fetched from the shell.
- `netstat`/`ipconfig` (`components/networking/netdiag.c`) read lwIP state only: `netif_list`,
  `dns_getserver`, and the TCP/UDP PCB lists. The PCB globals are declared locally (not via
  `lwip/priv/*` headers) and traversed read-only under `LOCK_TCPIP_CORE()` when
  `LWIP_TCPIP_CORE_LOCKING` is enabled (no-op otherwise), capped by
  `P4_CONFIG_NETSTAT_ROW_MAX`. Never iterate PCBs without the core lock in a context that
  could deadlock, and never modify them.

### Bluetooth Rules
- Hosted NimBLE on C6 over ESP-Hosted VHCI (not Bluedroid)
- Stateful lifecycle: enable once, reuse for scan/advertise
- bt is alias for bluetooth
- Supported: status, scan [limit], advertise on [name]|off
- A scan run MUST be bounded by P4_CONFIG_BT_SCAN_DURATION_MS (passed as the ble_gap_disc
  duration), so `bluetooth scan` always terminates and the worker task never hangs
- `bluetooth advertise on [name]` stores the name in s_bluetooth_state.session_advertise_name;
  it is session-only and must never be persisted across boots

### SD Card Rules
- All SD access goes through `components/storage/`. No other module may call `bsp_sdcard_mount()`
  or `bsp_sdcard_unmount()` directly.
- All SD commands use the shared guarded session: `shell_sd_begin()` / `shell_sd_end()`. Every
  `shell_sd_begin()` MUST have a matching `shell_sd_end()` on every return path.
- The mount is persistent; only `sd eject` / `sdeject` unmounts
- Any command that WRITES a file MUST precheck capacity with `storage_check_free_space()`
  before opening the destination, so a truncating overwrite cannot destroy the existing
  contents and then fail for lack of room
- Any command that copies MUST reject a self-copy via `storage_paths_are_same()`. Opening a
  destination with `"wb"` truncates it, so `copy a.txt a.txt` destroys the source.
- Any command that copies MUST carry attributes with `storage_copy_attributes()`, and MUST do
  so AFTER writing the data — a read-only destination cannot be opened for writing. Treat a
  failure as non-fatal: the data is already correct.
- A failed write MUST remove its partial destination. A truncated file that looks complete is
  worse than no file.
- Capacity queries go through `storage_get_space_info()`; never call `f_getfree()` directly
- `chkdsk` and any future integrity tool MUST stay read-only. This firmware does not rewrite
  FAT structures: report a problem, never attempt an in-place repair.
- Destructive volume operations (`format`) MUST require the exact confirmation word through
  the shell key queue AND refuse to run when `shell_key_input_available()` is false, so they
  can never execute unattended from a batch file
- FATFS LFN enabled with heap-backed buffers, MAX_LFN=255, UTF-8 encoding
- Bounded output: 128 entries max for listings, 8192 bytes max for sd cat
- SD VO4 LDO explicitly acquired at 3300 mV before mounts
- Path resolution handles sd:/, /sdcard/, and relative paths

### USB Rules
- USB Host Library with MSC and HID class drivers
- MSC mounts at /usb0 via VFS/FATFS
- HID echo is opt-in (keyboard/mouse on/off)
- Follows same bounded transcript style as SD commands
- USB keyboard auto-detect: automatically hides on-screen keyboard when USB keyboard attached
- USB keystrokes injected into shell CLI input line via shell_usb_keyboard_input() bridge
- Full US keyboard layout supported (60+ HID key codes with modifier-aware mapping)
- Auto-detect runs in periodic header refresh timer; state transitions trigger notifications
- keyboard_set_external_input() / keyboard_clear_force_visible() for manual control

### OTA Rules (c6ota)
- Lives in components/c6ota with stable public API
- Exact confirmation: WARNING: This will reboot the C6. Type YES to continue
- Validates ESP-IDF app magic 0xE9 + ESP32-C6 chip ID 0x000D
- 1500-byte chunks, progress every 5%
- Wi-Fi stopped before transfer, kept alive during (no esp_hosted_deinit())
- Factory v2.3.0 needs one-time standalone tool first

### Stack Discipline
- The command worker task stack is `P4_CONFIG_COMMAND_TASK_STACK` (8192 bytes) and is shared by
  the whole dispatch path
- `shell_execute_batch_file()`, `shell_execute_command()`, and `shell_execute_command_core()`
  form a RECURSIVE cycle: a batch file re-enters the pipeline for every line it runs. Their
  stack frames are multiplied by `P4_CONFIG_BATCH_DEPTH_MAX`.
- NEVER add a line-sized or larger buffer as a local variable in any function on that cycle.
  Allocate it on the heap and free it on every exit path. A 12 KB batch frame on the 8 KB stack
  was a real crash fixed in v0.19.0.
- This includes the batch command handlers themselves: `shell_resolve_batch_path`,
  `shell_command_set`/`set /a`/`set /p`, `shell_command_path`, `shell_command_echo`,
  `shell_command_if`, and `shell_command_choice` all run on the recursive batch path and keep
  their SD-path/line-sized scratch on the heap. A nested `call` whose callee runs `set`/`echo`
  stacked these frames and overflowed the worker stack (fixed in v0.24.8).
- When changing anything on that path, verify the frame size from the disassembly
  (`riscv32-esp-elf-objdump -d <obj>`, read the `addi sp,sp,-N` prologue), not by inspection
- Structures stored per batch frame (label table, argument copies) must be sized deliberately;
  a field sized at the full command width multiplies by the slot count
- The recursive directory walkers (`shell_dir_list_one`, `shell_tree_walk`,
  `shell_chkdsk_walk`) are subject to the same rule, multiplied by
  `P4_CONFIG_DIR_RECURSE_DEPTH_MAX`. Keep their per-level state in ONE heap block and free it
  before descending, so only one level's buffer exists at a time.
- Never declare a `FF_DIR`, `FILINFO`, or path-sized array as a local in a recursive walker;
  those three alone are over 700 bytes

### Command Execution
- Heavy commands run on dedicated worker task (not LVGL input callback stack)
- Preserve original unsplit command text for family handlers (wifi, sd, c6ota)
- LV_EVENT_READY on input line is the confirmed submission path
- Serial console reuses same shell path (stdin to submit, stdout from transcript)

### Task Introspection (ps / tasks / top)
- `ps`, `tasks`, and `top` are READ-ONLY: they only read `uxTaskGetSystemState()`
  snapshots and must never call `vTaskSuspend` / `vTaskDelete` / priority changes.
  Do not add kill/suspend verbs.
- The `TaskStatus_t` snapshot array MUST be heap-allocated (an entry is ~40 bytes;
  `P4_CONFIG_TASK_SNAPSHOT_MAX` entries can exceed the 8 KB worker stack) and freed on
  every path. Cap the count with `P4_CONFIG_TASK_SNAPSHOT_MAX` so a task-creation burst
  cannot blow the allocation or flood the transcript.
- Per-task CPU% comes from diffing `ulRunTimeCounter` against the previous sample keyed by
  `xTaskNumber` (unsigned arithmetic handles counter wrap; a new/deleted task starts fresh).
  Unpinned tasks report `tskNO_AFFINITY`; display that as `-1`. Guard `xCoreID` access with
  `#if configTASKLIST_INCLUDE_COREID`.

### Shell State
- RAM-only: current working directory, environment variables, PATH, batch args
- Current working directory owned by `components/storage/storage.c`; exposed read-only via
  `shell_get_cwd()` and mutated only through `storage_set_cwd()`
- Environment variables, PATH, batch frame stack, and errorlevel owned by
  `components/batch/batch.c`
- No persistence across boots
- Environment variables: max 24, names alphanumeric + underscore
- Batch depth: max 4 nested calls
- Batch labels: max 32 per file (`P4_CONFIG_BATCH_LABEL_MAX`)
- `set /a` parenthesis nesting: max 16 (`P4_CONFIG_SET_EXPR_DEPTH_MAX`)
- `set /p` input: max 128 bytes (`P4_CONFIG_SET_PROMPT_INPUT_BYTES`)
- Line continuations: max 8 joined lines (`P4_CONFIG_LINE_CONTINUATION_MAX`)
- setlocal scopes: max 8 nested (`P4_CONFIG_SETLOCAL_DEPTH_MAX`). Every scope a batch frame
  leaves open MUST be unwound when that frame returns, or the snapshot allocation leaks and the
  caller's environment is corrupted.
- Chained commands: max 8 (`P4_CONFIG_CHAIN_SEGMENT_MAX`); truncation MUST be reported, never
  silent
- Batch label names: max 48 bytes (`P4_CONFIG_BATCH_LABEL_BYTES`). This is deliberately small
  because the label table is `LABEL_MAX * LABEL_BYTES` and lives in the batch frame.
- Pipeline stages: max 4 (`P4_CONFIG_PIPE_STAGE_MAX`). Every spool file MUST be removed on
  every exit path, including a stage failure.
- Prompt template: max 64 bytes (`P4_CONFIG_PROMPT_TEMPLATE_BYTES`)

### Boot Scripting (CONFIG.SYS / AUTOEXEC.BAT)
- The firmware looks for `CONFIG.SYS` and `AUTOEXEC.BAT` on the SD card root at every boot.
- When either file is missing and `P4_CONFIG_BOOT_GENERATE_DEFAULTS` is set, default files are written once.
- `CONFIG.SYS` directives are parsed line-by-line by `components/boot/boot.c`. Supported: `SET`,
  `PATH=`, `PROMPT=`, `ECHO ON|OFF`, `ROTATE=`, `BRIGHTNESS=`, `DISPLAY_POWER=`, `VOLUME=`,
  `RGB=` (WS2812 LED: `<r>,<g>,<b>` / `#RRGGBB` / `<effect>[,speed]` / `OFF` / `AUTO,<ON|OFF>`),
  `WIFI_SSID=`, `WIFI_PASSWORD=`, `WIFI_AUTOCONNECT=`, `WIFI=ON|OFF`, `BLUETOOTH=ON|OFF`,
  `BT_ADVERTISE=ON|OFF`, `USB_KEYBOARD=ON|OFF`, `USB_MOUSE=ON|OFF`, `GPIO <n> = OUT [HIGH|LOW]`.
- Any unrecognized `NAME=VALUE` line is applied as a batch environment variable (same effect as
  `SET`), so CONFIG.SYS can carry project variables. Only unknown keywords *without* a value warn.
- Hardware directives are applied by executing their command-line equivalent through the batch
  pipeline (`batch_boot_execute_command()`), reusing existing validation. State-only directives
  (Wi-Fi credentials, autoconnect policy, echo default) use accessors exposed by the owning modules.
- `AUTOEXEC.BAT` runs through the normal batch pipeline (`shell_execute_batch_file()`), with cwd =
  SD root. Non-zero errorlevel is a warning only; the shell continues.
- Unknown or malformed directives produce a single muted warning and are never fatal.
- Safe with no SD card (silent skip), empty files, or read-only/full media.
- Wi-Fi password from `WIFI_PASSWORD=` is never echoed to transcript, history, or debug log.
- GPIO directives are delegated to the existing `gpio set` safety check; reserved pins are refused.
- All tunable values live in `p4minishell_config.h` and are documented in `p4minishell_config.yaml`
  under `boot_scripting`.

### Kconfig Rules
- `Kconfig.projbuild` MUST live with the component that consumes its options, not in `main/`.
  This keeps the symbols available to any project that includes the component, including `test/`.
- `CONFIG_P4MINISHELL_WIFI_DEFAULT_SSID` / `_PASSWORD` are defined in `components/networking/`

### Unit Test Rules
- `test/` is a standalone ESP-IDF project and MUST build with zero errors and zero warnings
- `test/CMakeLists.txt` pins `IDF_TARGET` to `esp32p4` and stages `board_config.h` and
  `p4minishell_config.h` into the generated config directory
- `test/main/idf_component.yml` MUST pin the same managed component versions as `main/idf_component.yml`
- `test/sdkconfig.defaults` MUST mirror the firmware's build-affecting options (FATFS LFN,
  ESP-Hosted, NimBLE, USB host) or the shared components will not compile
- Adding a component dependency to `shell`, `storage`, `batch`, or `command` requires adding its
  directory to `EXTRA_COMPONENT_DIRS` in `test/CMakeLists.txt`
- A new component under `components/` must be added to BOTH the root `CMakeLists.txt`
  `EXTRA_COMPONENT_DIRS` and `test/CMakeLists.txt`

### Header Bar
- Passive, display-only module in components/header
- Non-scrollable, resolution-scaled height
- Status icons: Wi-Fi, battery, Bluetooth, USB, SD
- SD icon hidden when no card mounted
- Notification area transient (blank when idle)

### Build Constraints
- Every build constraint MUST be pinned in `sdkconfig.defaults`, not only in the
  generated `sdkconfig`. A setting present only in the generated file is silently
  reverted whenever the config is regenerated. Three constraints were lost this way
  before v0.23.0.
- After changing `sdkconfig.defaults`, delete `sdkconfig` and rebuild to confirm the
  intended values actually survive regeneration
- LVGL examples MUST stay disabled (image budget)
- Station-only Wi-Fi profile: `CONFIG_ESP_WIFI_SOFTAP_SUPPORT=n` AND
  `CONFIG_WIFI_RMT_SOFTAP_SUPPORT=n`
- Newlib nano formatting
- Warn-level compile-time logging (`CONFIG_LOG_DEFAULT_LEVEL_WARN=y`)
- Size optimization (`CONFIG_COMPILER_OPTIMIZATION_SIZE=y`); `OPTIMIZATION_PERF` grows
  the image by roughly 280 KB
- PSRAM XIP instruction/rodata mapping MUST stay disabled
  (`CONFIG_SPIRAM_XIP_FROM_PSRAM=n`) or the flash/PSRAM budget overflows at link time
- ESP-Hosted reset: SLAVE_RESET_ON_EVERY_HOST_BOOTUP

### Documentation Updates
After every task, update: changelog.md, readme.md, documentation.md, ai-context.md, board_config.yaml, command.md
For roadmap work, also update: roadmap.md, API.md, SDK.md
When bumping the version, update all three: `p4minishell_config.h` version macros,
`p4minishell_config.yaml` `config_version`, and the `readme.md` version badge

### Hardware Gaps (Do NOT implement)
- Camera: no local camera stack in workspace
- Should fail explicitly with honest messages

### LED Rules (components/led + `rgb`)
- `components/led/led.c` is the single owner of the WS2812 status LED on GPIO26
  (`BOARD_CFG_RGB_LED_GPIO`, `BOARD_CFG_RGB_LED_IS_WS2812`). It creates the strip via the
  `espressif/led_strip` managed component over RMT and drives ALL strip I/O from one small
  animation task; public API calls only mutate a mutex-protected state snapshot, so the
  command worker and the Wi-Fi event handler never block on RMT.
- Consumers push state/events; the module never queries other subsystems, so it is a leaf
  that `command`, `networking`, and `main` can all depend on (no layering cycle). The `rgb`
  command body stays in `components/command/command.c` (hardware verbs); `networking` pushes
  Wi-Fi/HTTP events via `led_notify()`; `main` fires the boot confirmation flash.
- Auto status follows Wi-Fi state (connecting=amber pulse, connected=green, disconnected=red
  blink, watchdog timeout=red pulse); HTTP server start flashes blue. In manual mode events
  are transient (`P4_CONFIG_LED_NOTIFY_MS`). GPIO26 is a reserved critical line in the board
  pin table, so `pwm`/`freq`/`adc`/`i2c`/`spi` refuse it.
- All tunables live in `P4_CONFIG_LED_*` (documented in `p4minishell_config.yaml`).

### Peripheral Toolkit Rules (pwm / freq / adc / i2c / spi)
- The `pwm`, `freq`, `adc`, `i2c`, and `spi` commands live in `components/command/command.c`
  (hardware verbs belong to the command module). All of them gate every pin through
  `shell_pin_is_reserved()` (the `critical` entries of `s_gpio_pins`), so the board's active
  I2C/I2S/SDIO/display/SD lines can never be repurposed.
- PWM/square waves use the legacy LEDC API (`driver/ledc.h`). The toolkit must use LEDC
  timers 0/2/3 and channels excluding `BOARD_CFG_DISPLAY_BRIGHTNESS_LEDC_CH` (the backlight
  owns channel 1 / timer 1). It MUST use the same global LEDC clock the backlight
  auto-selects (`P4_CONFIG_PWM_CLK_SOURCE` = SOC_MOD_CLK_XTAL, 40 MHz) and pick the duty
  resolution from `P4_CONFIG_PWM_SRC_CLK_HZ` (40 MHz); a mismatch or an 80 MHz assumption
  fails with "timer clock conflict" / `div_param=0`.
- `adc <pin>` opens a fresh one-shot unit per read and deletes it (plus calibration) on every
  path. `adc status` enumerates pins from the SOC channel map (`soc/adc_channel.h`), never by
  probing arbitrary GPIOs with `adc_oneshot_io_to_channel` (that logs spurious errors), and
  live-samples each candidate so a busy ADC unit is reported unavailable.
- `i2c` reuses the board's shared bus handle (`bsp_i2c_get_handle()`) when pins match the BSP
  I2C pair, otherwise it creates a temporary `i2c_master` bus on a free port. Scans probe via
  normal device transactions (add device + one-byte transmit), NEVER `i2c_master_probe()`,
  which reprograms the shared controller's timing/interrupts and disrupts the GT911 touch.
- SPI is a reported hardware gap on this board: `spi status` reports the toolkit
  configuration, but the `loopback`/`peek`/`poke` verbs return an honest
  "unavailable on this board" error and MUST NOT call the SPI driver — initializing
  the SPI host on this P4 with the ESP-Hosted SDIO link active stalls the chip and
  drops USB-Serial-JTAG off the bus (verified with both SPI2 and SPI3, DMA on and
  off). Do not re-wire SPI transactions without first proving the host init does
  not stall the chip.

### Error Handling
- Friendly transcript messages for all failures
- 5-entry debug history buffer via debug command
- Healthy boot recorded in debug history, not as warning
- ESP-IDF error names in messages: esp_err_to_name()
