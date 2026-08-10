# P4MiniShell Technical Documentation

## Architecture Overview

P4MiniShell is a modular embedded shell application for ESP32-P4 with an ESP32-C6 co-processor. The codebase is organized into a shell orchestration layer and dedicated component modules.

### Module Layout

```
main/main.c                     App entry point, LVGL event callbacks, UI construction, host bridges
p4minishell_config.h            Centralized configuration header
p4minishell_config.yaml         Configuration documentation (YAML)
components/ansi/ansi.c          ANSI/VT escape sequence module (SGR colors, attributes, formatting)
components/display/display.c    Display manager (rotation, resolution, refresh, brightness, power)
components/windows/windows.c    Window manager (LVGL screen layout, dynamic scaling, styling)
components/keyboard/keyboard.c  Keyboard manager (LVGL keyboard, visibility, modes)
components/shell/shell.c        Shell core (transcript, history, debug log, UART console, input line, sysinfo)
components/storage/storage.c    SD sessions, path resolution, FATFS conversion, size formatting, cwd
components/storage/storage_commands.c  DOS file commands, extended DOS tools, text utilities, sd family
components/batch/batch.c        Batch engine, labels, for loops, pipes, environment variables, PATH
components/boot/boot.c            DOS-style boot scripting (CONFIG.SYS parser, AUTOEXEC.BAT runner)
components/command/command.c    Command module (dispatcher, worker task, execution pipeline, hardware and system commands)
components/command/command_ui.c  UI query commands (display, keyboard, windows subcommands)
components/header/header.c      Fixed top status bar (LVGL widgets)
components/networking/networking.c  Hosted Wi-Fi runtime (ESP-Hosted + esp_wifi_remote)
components/networking/bluetooth.c   Hosted NimBLE Bluetooth (VHCI on C6)
components/usb/usb.c            USB Host (MSC storage + HID keyboard/mouse)
components/c6ota/c6ota.c        ESP32-C6 OTA updates (ESP-Hosted SDIO)
coprocessor/esp32c6_slave/      ESP32-C6 hosted slave firmware project
```

### Configuration System

All tunable values are centralized in `p4minishell_config.h`. The header is organized
by subsystem with `P4_CONFIG_` prefixed macros. The companion `p4minishell_config.yaml`
documents every value with type, description, and valid range.

Backward-compatible `SHELL_*`, `NETWORKING_*`, `BLUETOOTH_*`, `HEADER_*`, `C6OTA_*`,
and `USB_*` aliases are defined in each source file that needs them.

Three config sources exist, each with a distinct role:
- `p4minishell_config.h` � C-level tunable values (buffer sizes, limits, colors, stack sizes)
- `board_config.h` � Hardware pin assignments and display timing (from board_config.yaml)
- `sdkconfig` � ESP-IDF build configuration (Kconfig-driven)

### Layering

Component dependencies flow strictly one way:

```
main  ->  command  ->  batch  ->  storage  ->  shell  ->  ansi, display, windows, header, keyboard, clock
```

Two registration tables invert the only upward dependencies:

- `shell.c` needs a few services that the command layer owns (command dispatch, current working
  directory, speaker volume, SD mount state, battery telemetry). Rather than creating a
  dependency cycle, `command_init()` registers a `shell_command_ops_t` function table with
  `shell_register_command_ops()`. The `get_cwd` and `sd_is_mounted` hooks point at the
  `components/storage/` implementations; the shell core never learns that.
- `batch.c` needs the full command pipeline for every nested line it runs (`if` bodies, `for`
  bodies, pipe stages, batch file lines) so those inherit variable expansion and output
  redirection. `command_init()` registers a `batch_command_ops_t` table with
  `batch_register_command_ops()`.

Both mirror the `networking_host_ops_t` pattern used by `components/networking`. Every hook is
NULL-checked, so the shell and the batch engine degrade gracefully if used before
`command_init()` runs.

**External-module accessors.** `shell.c` also needs state from the networking, Bluetooth, USB,
and C6 OTA modules (for the header status refresh and the `debug` command). Rather than including
those modules' headers — which would make the shell core depend on every subsystem — the shell
reads that state through function pointers in `shell_command_ops_t`. The 11 accessors
(`wifi_is_connected`, `wifi_get_rssi`, `wifi_state_string`, `append_sysinfo_summary`,
`bluetooth_is_enabled`, `bluetooth_is_connected`, `usb_is_connected`, `usb_is_keyboard_attached`,
`usb_key_to_ascii`, `c6ota_is_pending`, `c6ota_is_busy`) are all registered by `command_init()`.
Every hook is NULL-checked before use, so a missing module simply skips that status line.

### Application Layer (main/main.c)

Intentionally thin (~400 lines). It owns only:

- **Boot sequencing**: `display_init()`, `shell_init()`, `command_init()`, UART console,
  UI construction, then the c6ota, networking, usb, and clock modules
- **LVGL event callbacks**: transcript focus, input line (submit, prompt repair, history keys),
  history buttons, and on-screen keyboard events
- **UI construction**: `shell_build_ui()`, also registered with the display manager as the
  rotation rebuild callback
- **Header refresh timer**: an LVGL timer that calls `shell_header_status_refresh()`
- **Host bridge callbacks**: the `c6ota_host_*` and `usb_host_*` symbols those components
  declare `extern`; ESP-IDF requires them to be defined in the app component

It contains no command implementations, no shell state, and no forward declarations for
local functions.

### Shell Core (components/shell)

Owns the shell's runtime surface and output plumbing:

- **Transcript system**: Scrollable textarea backed by an 8 KB buffer with overflow protection.
  When full, the oldest half is dropped and a `[history truncated]` marker is inserted.
- **Async transcript buffer**: Thread-safe 2 KB staging buffer for background-task output,
  flushed via `lv_async_call`. Oldest bytes are dropped first when full, so the newest module
  output always reaches the user. The buffer is drained into a heap copy (never a line-sized
  stack buffer — it can be 2 KB) before the LVGL append so no LVGL work runs inside a critical
  section. Before the UI exists (`windows_get_transcript()` is NULL — unit tests, early boot)
  the flush runs synchronously instead of through `lv_async_call`, so an uninitialized LVGL
  heap is never touched.
- **ANSI output**: `shell_transcript_append_ansi()` strips escape sequences for the LVGL
  textarea and forwards the raw sequences to the serial console for native rendering
- **Semantic print helpers**: `shell_print_heading()`, `shell_print_field()`,
  `shell_print_field_num()`, `shell_print_ok()`, `shell_print_error()`,
  `shell_print_warning()`, `shell_print_muted()`, and `shell_print_usage()` wrap the ANSI
  append with the palette from `components/ansi/ansi_palette.h`. Each emits its own reset and
  newline, so a command never has to think about colour. They render the caller's text with
  real `vsnprintf` before wrapping, so a value containing '@' cannot be misread as a specifier.
- **Command history**: 10-entry recall buffer with password masking for `wifi connect`.
  Masking is applied both at the caller (`shell_command_should_store_history()`) and inside
  `shell_store_command_history()` so no path can persist a password.
- **Serial console bridge**: stdin/stdout routed through the same shell path as the touch UI,
  with LVGL locking and a submission mutex
- **Input line**: Owns the prompt-prefix contract — `shell_input_line_set_text()`,
  `shell_input_line_reset()`, `shell_extract_input_text()`, and
  `shell_input_line_repair_prompt()` (which restores the prompt after a keyboard backspace
  deletes into it)
- **Debug log**: 5-entry circular buffer surfaced via the `debug` command. Errors are also
  echoed to the transcript so on-screen users see failures without running `debug`.
- **Interactive keypress queue**: `shell_key_wait_begin()` / `shell_wait_for_key()` /
  `shell_key_wait_end()` let `pause`, `choice`, and `more` block on a real keystroke. All three
  input sources (UART console reader, USB HID bridge, LVGL on-screen keyboard) feed the queue
  through `shell_key_wait_submit()`, and every source suppresses command-line handling while a
  wait is active so an answer is never dispatched as a command. Waits are bounded by
  `P4_CONFIG_KEY_WAIT_TIMEOUT_MS`, and `shell_key_input_available()` lets commands fall back to
  a timed delay on a headless board.
- **Line input**: `shell_read_line()` collects a typed line through the key queue, echoing as
  it goes. Backspace edits, ESC cancels, Enter submits. Backs `set /p`.
- **UART console line assembly**: the console reader (`shell_uart_console_task`) uses
  line-buffered `fgets` on unbuffered stdin, and USB-Serial-JTAG delivers one logical line
  across several reads (its RX FIFO is 64 bytes). The task assembles fragments until a line
  terminator (or the 256-byte command buffer fills) before submitting, so a long command is
  never split into two. During an active key wait the first newly-read character is still
  answered immediately, without waiting for the terminator.
- **DOS prompt template engine**: `shell_prompt_set_template()` stores the template the
  `prompt` command supplies; `shell_prompt_render_plain()` expands `$p $g $l $b $n $d $t $v $s
  $_ $q $$ $a $c $f $e $h` against live state. One template drives both the UART console prompt
  and the LVGL input line. The input line snapshots the prefix it painted so a template or path
  change between two LVGL events cannot corrupt command extraction.
- **System info commands**: `help`, `sysinfo`, `version`, `about`, `mem`, `debug`
- **Header status refresh**: Batches every header field into one async render to avoid
  flicker; also drives USB keyboard auto-detect and SD insert/remove notifications
- **Quote and escape scanner**: `shell_find_unquoted_char()`, `shell_find_unquoted_any()`,
  `shell_has_unquoted_char()`, and `shell_unescape_in_place()`. One implementation backs five
  surfaces — the argument tokenizer, redirection parsing, pipe splitting, chain splitting, and
  variable expansion — so they can never disagree about whether a character is syntax or data.
  Rules: `"text"` groups with expansion, `'text'` groups literally, `^c` escapes one character.
- **Chain splitting**: `shell_split_chain()` divides a line on unquoted `&`, `&&`, and `||`
  into `shell_chain_segment_t` entries. A single `|` is deliberately left in place so the
  pipeline splitter still sees it, which is what lets a pipeline be one link in a chain.
- **Utilities**: `shell_trim()`, `shell_split_args()` (quote- and escape-aware, strips markup),
  `shell_text_equals_ignore_case()`, `shell_parse_percentage_arg()`, `shell_parse_size_arg()`,
  `shell_join_args()`
- **USB keyboard injection**: `shell_usb_keyboard_input()` dispatches HID events into the
  input line via `lv_async_call`

### Storage Module (components/storage)

Owns everything that sits between the shell commands and the SD card. Split into two files:

**`storage.c` — services and state**

- **Guarded SD sessions**: `shell_sd_begin()` / `shell_sd_end()` are the single mount path, so a
  failure can never leak mounted state or dereference missing card metadata. The mount is
  persistent — only `sd eject` / `sdeject` tears it down. Mounting updates the header SD icon.
- **Mount tracking**: `storage_sd_is_mounted()` reports the persistent flag rather than calling
  `stat()`, which only succeeds while the card is already mounted. An eject latch blocks
  auto-remount until the user asks for it.
- **Path resolution**: `shell_sd_resolve_path()` (SD-rooted), `shell_fs_resolve_path()`
  (cwd-relative with `.` / `..` collapsing), and `shell_resolve_target_from_source()` (DOS
  destination-relative-to-source semantics for `ren` and `move`)
- **FATFS conversion**: `shell_sd_vfs_to_fatfs_path()` and `shell_sd_fresult_to_esp_err()`. The
  direct FATFS API is used for listings because it exposes long file names (MAX_LFN=255) and
  the 8.3 alternate name, which the POSIX dirent path does not surface.
- **Formatting and matching**: `shell_sd_format_size()` (B/KiB/MiB/GiB), `shell_sd_entry_type()`,
  and `shell_wildcard_match()` (case-insensitive DOS `*` and `?`)
- **Current working directory**: RAM-only, rooted at the SD mount point. `shell_get_cwd()`,
  `storage_set_cwd()`, and `shell_fs_print_cwd()` (which renders the root as `\`).
- **Shared file helpers**: `shell_fs_copy_file()`, `shell_list_directory_path()` (bounded to 128
  entries), and `shell_print_file_text()` (non-printable bytes rendered as `.`)
- **Output redirection writes**: `shell_write_redirect_output()` for the `>` and `>>` operators
- **Input redirection slot**: `storage_set_input_redirect()` /
  `storage_resolve_input_source()` / `storage_clear_input_redirect()`. Both the `<` operator and
  each pipe stage publish here, so `sort < f.txt` and `type f.txt | sort` reach one code path in
  the text-processing commands. The slot belongs to exactly one command and is cleared after
  dispatch, including when the dispatch fails.
- **Volume capacity**: `storage_get_space_info()` returns total, used, and free bytes plus the
  allocation unit size and cluster counts via `f_getfree()`. Handles both FATFS sector-size
  build configurations.
- **Write guardrails**: `storage_check_free_space()` refuses an operation that would leave less
  than `P4_CONFIG_STORAGE_FREE_MARGIN_BYTES` free, crediting any space the destination already
  occupies so an in-place overwrite does not need double the room. When the capacity query
  itself fails the write is allowed to proceed rather than blocking on a diagnostic failure.
  `storage_paths_are_same()` backs the self-copy guard, which prevents `copy a.txt a.txt` from
  truncating the source before the first read.
- **Copy semantics**: `shell_fs_copy_file()` prechecks free space, refuses a self-copy, reports
  progress above `P4_CONFIG_COPY_PROGRESS_THRESHOLD`, and removes the partial destination when
  a write fails so a truncated file never looks complete.
- **Attribute preservation**: `storage_copy_attributes()` carries R/H/S/A from source to
  destination. Called from `shell_fs_copy_file()`, so all five copy call sites inherit it.
  Applied after the write, because a read-only destination cannot be opened for writing, and
  treated as non-fatal since the data is already correct. `xcopy /S` also applies it to the
  directories it creates.

**`storage_commands.c` — the DOS file and text commands**

- Navigation and listing: `cd`/`chdir`, `dir`. The listing supports the full DOS option set —
  `/W` wide, `/P` paged, `/S` recursive, `/B` bare, `/L` lowercase, `/A` attribute filter, and
  `/O` sort order — alongside the existing wildcard filtering and `[SFN]` display. Each level
  is buffered so entries can be sorted before printing; the buffer is bounded by
  `P4_CONFIG_DIR_SORT_ENTRY_MAX` and recursion by `P4_CONFIG_DIR_RECURSE_DEPTH_MAX`.
- Volume management: `chkdsk`/`scandisk` reports capacity and cluster geometry, with `/F`
  walking every directory to verify readability. The scan is read-only by design: the firmware
  never rewrites FAT structures, so a genuinely corrupt card is reported rather than modified.
  `format` reformats through `esp_vfs_fat_sdcard_format_cfg()` (honouring `/FS:FAT|FAT32`,
  `/A:size` cluster size, `/V:label`, and `/Q`) behind the exact confirmation word, collected
  through the shell key queue, and refuses when no interactive source is attached.
- Disk and partitions: the `disk` family (`list`, `detail`, `clean`, `create partition
  primary [size=N]`, `delete partition N`, `format`) manages the MBR partition table through
  `sdmmc_read_sectors`/`sdmmc_write_sectors` on the BSP card handle, unmounting the FATFS
  volume first. The volume services in `storage.c` are parameterized by `storage_volume_t`
  so a future USB OTG MSC volume can be added without changing the command surface.
- Recursive directory tree: `tree [path] [/F] [/A]` with DOS box-drawing connectors, bounded by
  `P4_CONFIG_TREE_DEPTH_MAX` and the 128-entry listing cap. Each directory level is buffered on
  the heap rather than the worker-task stack, so a wide directory cannot overflow it.
- File manipulation: `copy`, `move`, `del`/`erase`, `ren`/`rename`, `md`/`mkdir`, `rd`/`rmdir`,
  `type`, `write`, `append`, `touch`
- Extended DOS tools: `attrib` (R/H/S/A via `f_stat`/`f_chmod`), `label` (via
  `f_getlabel`/`f_setlabel`), `xcopy` (recursive with `/S`)
- Text utilities: `find` (`/I /N /C /V`), `more` (keypress paging with `Q` to quit),
  `fc` (DOS-style differing-line report), `sort` (qsort-based, `/R /I /U`, 1024-line capacity
  with a single leak-free release path). All resolve relative paths and run inside a guarded SD
  session, and all read the pending input-redirection source when no filename is supplied.
- SD command family: `sd info|ls|stat|cat|eject`, bounded to 128 listing entries and an 8192-byte
  `sd cat` preview

### Batch Module (components/batch)

Owns the whole `.bat` interpreter and the RAM-only environment:

- **Batch execution**: `shell_execute_batch_file()` pushes a frame (echo state, argument copies,
  parent pointer, label table, file handle) and runs the file line by line. Nesting is capped at
  4 levels. `@` suppresses per-line echo; `rem` and `::` are comments.
  The frame and the line buffer are **heap-allocated**. The frame carries the argument copies
  and the label table, which together are far larger than the command worker task's 8 KB stack
  can hold across nested calls; keeping this recursive function's stack frame small is what
  makes the full nesting depth safe.
- **Batch path resolution**: `shell_resolve_batch_path()` tries the literal name, the name with
  `.bat` appended, then each `;`-separated PATH entry with both forms
- **Labels and jumps**: The label table is built with a full-file scan on load
  (`P4_CONFIG_BATCH_LABEL_MAX` = 32 targets), so `goto` and `call :label` are an `fseek()`
  across the file. `goto :eof` is an implicit end-of-file label: it ends the current frame
  exactly like reaching the end of the file, unwinding any open setlocal scopes. A pending
  goto is always cleared when a frame returns, so a `goto`/`goto :eof` issued from inside a
  `for` or `if` body in a called script cannot leak into the caller's line loop.
- **`for` loops**: `for %%var in (set) do command` with per-iteration `%var` substitution.
  The set is either a space-separated literal token list (`for %%I in (a b c) do echo %%I`)
  or a single wildcard pattern (`for %%F in (*.txt) do echo %%F`), which is expanded through
  the storage layer's `storage_expand_wildcard()`. The loop body is expanded into a
  heap-allocated buffer because the loop re-enters the command pipeline on the recursive
  batch path.
- **Multi-stage pipes**: `shell_execute_pipe()` splits on unquoted `|` into up to
  `P4_CONFIG_PIPE_STAGE_MAX` stages. Each stage but the last spools to its own file, and the
  next stage reads it through the storage input-redirection slot. Spooling rather than streaming
  is the correct model here: the shell runs one command at a time on a single worker task, so
  there is no second process to stream into. Every spool file is removed on every exit path.
- **Environment variables**: 24 RAM-only slots, names normalized to upper case and restricted to
  alphanumerics plus underscore. PATH is one of these slots and defaults to `sd:/`.
- **`setlocal` / `endlocal` scoping**: a stack of full environment snapshots
  (`P4_CONFIG_SETLOCAL_DEPTH_MAX` = 8). Snapshotting the whole table is the honest approach for a
  fixed-size array: restoring it reverts creations, modifications, and deletions in one step. A
  scope a batch file leaves open is unwound when that frame returns, so a child cannot leak
  variables into its caller and no snapshot allocation is ever lost.
- **Variable expansion**: `shell_expand_variables()` handles `%VAR%`, `%0` (script name),
  `%1`..`%9` (the caller's arguments), `%*` (every argument from `%1` onward), and `%%` → `%`
- **Errorlevel**: `batch_get_errorlevel()` / `batch_set_errorlevel()`, consumed by `if errorlevel N`
  and set by `choice` to the 1-based index of the chosen key. `call` propagates the called
  script's final errorlevel back to the caller, matching DOS.
- **Stop modes**: `batch_stop_mode_t` distinguishes `exit /b` (leave one file) from a bare
  `exit` (unwind every nested level). The executor and `for` loops both honor it.
- **Arithmetic expressions**: `shell_expr_evaluate()` is a recursive-descent evaluator over
  32-bit signed integers. Precedence follows cmd.exe: `||` then `&&` then the comparisons
  `== != < > <= >=` (each yielding 1/0) then `|`, `^`, `&`, `<< >>`, `+ -`, `* / %`, unary
  `- ~ !`, and parentheses. Backs `set /a`, including its compound assignment operators
  (`+=` through `>>=`). An undefined variable evaluates to 0, matching DOS, which is what lets
  `set /a n=n+1` work on first use. `shell_expr_find_assignment()` splits `NAME[OP]=expr` while
  skipping the `=` of `==`/`!=`/`<=`/`>=`, so comparisons inside the expression are not mistaken
  for the assignment. The `&`, `|`, `&&` and `||` handlers refuse to consume a chain separator
  that survives as shell syntax, and an expression using those operators must be quoted at the
  shell level (unquoted `&&`/`||`/`&`/`|`/`<`/`>` are split first).
- **Numeric `if` comparisons**: `shell_command_if()` recognises the cmd.exe keywords
  `EQU`, `NEQ`, `LSS`, `LEQ`, `GTR`, `GEQ` between two operands (`if %n% GTR 5 echo ...`),
  parsed as decimal with non-numeric operands reading as 0. This is a separate branch from the
  `==` string comparison.
- **Prompted input**: `set /p` reads a line through the shell core's `shell_read_line()`. An
  empty line leaves the variable unchanged, as in DOS.
- **Line continuation**: a trailing `^` joins the next physical line, bounded by
  `P4_CONFIG_LINE_CONTINUATION_MAX`. An odd-caret-count rule distinguishes a continuation from
  an escaped `^^`. The label scanner applies the same rule, so a continued line cannot
  register a phantom `:label`.
- **Batch language commands**: `set` (with `/a` and `/p`), `path`, `echo`, `call`, `if`
  (with `errorlevel N` — true when errorlevel ≥ N, `exist <path>`, `/i` case-insensitive
  string comparison, and `not` for all three), `goto` (including `goto :eof`), `shift`,
  `pause`, `choice`, `setlocal`, `endlocal`, `exit`. `pause` and `choice`
  block on a real keystroke through the shell core's key queue.
- **Nested execution**: every nested line goes back through the full command pipeline via
  `batch_command_ops_t`, so it inherits variable expansion and redirection

### Command Module (components/command)

Owns the dispatcher, the execution pipeline, and the commands that are not tied to the
filesystem or the batch language:

- **Execution pipeline**: `shell_execute_command()` splits the line into chain segments, then
  for each segment expands variables (through `components/batch/`), parses redirection,
  publishes any `<` source to `components/storage/`, dispatches, and captures the transcript
  delta for `>` / `>>`
- **Chain execution**: `&` always runs the next link, `&&` runs it on success, `||` on failure.
  Splitting deliberately happens *before* expansion and runs per segment, so a variable whose
  value contains `&` cannot inject a command — the same ordering COMMAND.COM uses. Success is
  tracked per link rather than re-read from global errorlevel, so a stale value cannot select
  the wrong branch. An unrecognized command sets errorlevel 9009.
- **Heap-backed line buffers**: the expansion and chain buffers are heap-allocated because this
  function sits on the batch recursion path — a batch file re-enters the pipeline for every
  line, and stack buffers here would overflow the worker task at nesting depth.
- **Dispatcher**: `shell_execute_command_core()` — 65 verbs with family routing
  (wifi, bluetooth, usb, c6ota, sd) that receive the original unsplit command text
- **Worker task**: `shell_execute_command_async()` creates a dedicated FreeRTOS task so heavy
  commands never run on the LVGL event-callback stack
- **Redirection parsing**: a quote-aware two-pass scan handling `>`, `>>`, and `<` in any order
  on one line. The first pass locates every unquoted operator before any is overwritten; the
  second writes a terminator over each, which simultaneously ends the preceding text and
  terminates the previous target. The last occurrence of each direction wins, matching
  COMMAND.COM.
- **Quote-aware pipe detection**: `shell_command_has_pipe()` so `echo "a | b"` is not mistaken
  for a pipeline
- **Hardware controls**: Backlight PWM, display rotation with touch remapping, battery ADC
  with calibration, ES8311 codec volume, and light-sleep requests — display operations are
  routed through `components/display/`
- **UI query commands**: `display info|resolution|refresh|power`, `keyboard show|hide|toggle|status`,
  `windows info` — implemented in `command_ui.c` to keep `keyboard.h` and `windows.h` out of
  `command.c`
- **System commands**: `reboot`, `clear`/`cls`, `prompt`, `date`, `time`
- **Screenshot command**: `screenshot`/`scr`/`capture` — captures the LVGL screen as a BMP image, streams to serial with magic markers or saves to SD card
- **GPIO management**: Pin table with board roles; reads allowed on all pins, writes restricted
  to pins marked safe
- **Module startup**: `command_init()` runs `storage_init()` and `batch_init()`, then registers
  `batch_command_ops_t` and `shell_command_ops_t`
- **Telemetry for the header**: `command_battery_read()` and `storage_sd_is_mounted()` are
  published to the shell core through the operations table

### Display Module (components/display)

Central display controller owning all display hardware state and operations:

- **Rotation control**: 0/90/180/270 degree rotation with automatic GT911 touch remapping
- **Resolution queries**: Native panel resolution (1024x600) and current effective resolution (accounting for rotation)
- **Refresh rate**: Query current refresh rate (~60 Hz from panel timing); dynamic rate change API exists but is noted as not supported on JD9165 panel
- **Backlight brightness**: 0-100% PWM brightness control through BSP LEDC path
- **Power management**: Display on/sleep/off power state transitions with backlight control
- **Display diagnostics**: Comprehensive `display_info_t` struct with all timing, buffer, and config data
- **Thread safety**: State variables protected by critical sections; LVGL operations dispatched via `lv_async_call`
- **UI rebuild callback**: Registered callback invoked after rotation changes to trigger full UI rebuild
- **Touch handle**: Lazy acquisition of GT911 touch handle from BSP; cached for rotation remapping
- **Public API**: `display_init()`, `display_set_rotation()`, `display_get_rotation()`, `display_set_brightness()`, `display_get_brightness()`, `display_get_resolution()`, `display_get_info()`, `display_set_power_state()`, `display_sleep()`, `display_wake()`, `display_set_refresh_rate()`, `display_register_ui_rebuild_callback()`

### Window Manager (components/windows)

Central layout manager owning the LVGL screen region partitioning and dynamic scaling:

- **Named regions**: HEADER, TRANSCRIPT, INPUT_ROW, KEYBOARD � each with computed bounding rectangles
- **Resolution-aware scaling**: All dimensions derived from display.c's current resolution
- **Rotation-aware**: Recalculates layout on rotation change via the display manager's UI rebuild callback
- **Consistent styling**: All colors accessed through semantic names via `windows_get_color()`
- **Font management**: Centralized terminal font selection via `windows_get_terminal_font()`
- **Lifecycle**: `windows_init()` builds all UI regions; `windows_deinit()` tears down before rotation rebuild
- **Public accessors**: Individual window objects accessible via getter functions for event callback registration
- **Works with display.c**: Queries `display_get_width()`/`display_get_height()` for current resolution
- **Delegates to header.c**: Header region creation delegated to `header_init()`/`header_deinit()`

### Header Module (components/header)

Passive, display-only module that owns the fixed top bar:

- Non-scrollable LVGL flex-row container
- Resolution-scaled height (display_height / 15, clamped 32-56px)
- Status icons (left-to-right): Wi-Fi, Bluetooth, USB, SD
- System panel (far right): MEM (free heap), CPU (bar + percentage), BAT (bar + percentage)
- Battery always visible � shows "BAT N/C" when ADC is not connected
- All system panel values dynamically linked to FreeRTOS runtime statistics
- CPU usage calculated from FreeRTOS idle task runtime counter deltas
- Notification area in center for transient module events
- All public functions use LVGL async dispatch (safe from any task context)
- SD icon shows persistent state (NO/INS/ON/ERR)

### ANSI/VT Module (components/ansi)

Provides SGR (Select Graphic Rendition) escape sequence processing for colored terminal output:

- **16-color palette**: PowerShell-inspired color scheme with standard and bright variants
- **SGR parsing**: Full ESC[...m sequence parser with state machine tracking
- **Text attributes**: Bold, dim, italic, underline, blink, reverse, hidden, strikethrough
- **Format string builder**: `ansi_format()` with `@`-prefixed color/attribute specifiers
- **ANSI-to-plain stripping**: `ansi_strip_to_plain()` for plain text/history; the on-screen
  transcript keeps the ANSI and renders per-colour spans (`lv_spangroup`) via `ansi_process_text()`
- **UART pass-through**: Raw ANSI codes forwarded to serial terminal for native rendering
- **Configurable palette**: All 16 colors configurable via `P4_CONFIG_ANSI_*` macros
- **Runtime palette modification**: `ansi_set_palette_color()` for dynamic color changes
- **Thread safety**: Palette is read-only after init; runtime modifications not thread-safe by design
- **Semantic palette** (`ansi_palette.h`): the single source of truth for what colour each
  category of shell output uses. Defines named `SH_*` macros — `SH_HEAD`, `SH_LBL`, `SH_OK`,
  `SH_ERR`, `SH_WARN`, `SH_MUTE`, `SH_VAL`, `SH_NUM`, `SH_PATH`, `SH_DIR`, `SH_FILE`, `SH_EXE`,
  and the subsystem accents — that expand to the `@`-specifiers above. Commands compose output
  from these names rather than choosing colours, so the shell is consistent by construction and
  a palette change is a one-file edit. The macros are plain string literals, so the header adds
  no dependencies and the ansi module stays a leaf.
- **Full printf compatibility**: `ansi_format()` passes captured specifiers to `snprintf`
  verbatim, so flags, width, precision, and `l`/`ll` length modifiers all behave as in printf.
  Column-aligned reports such as `chkdsk` and `dir` depend on this.

### Networking Module (components/networking)

The single owner of every ESP-Hosted and esp_wifi_remote call in the firmware. Only the
official Espressif path is used — `espressif/esp_hosted` for the transport and
`espressif/esp_wifi_remote` for the Wi-Fi API — with no custom RPC, no alternative
transport, and no re-implemented control plane.

**Ownership rule**: no `esp_hosted_*`, `esp_wifi_*`, `esp_netif_*`, or NimBLE call may
appear outside `components/networking/`. The one sanctioned exception is
`components/c6ota/`, which drives `esp_hosted_slave_ota_*` because co-processor firmware
update is its entire purpose. Consumers that need networking state — the header bar,
`sysinfo`, `debug`, and the command handlers — use the module's status helpers.

**Canonical initialization order.** `networking_wifi_start_runtime()` performs these
steps in exactly this order; the order is load-bearing, not stylistic:

1. `esp_hosted_init()`
2. `esp_hosted_connect_to_slave()` — the SDIO link must exist first, because
   `esp_wifi_init()` is the esp_wifi_remote shim and has no peer until the slave is up
3. Version compatibility gate — refuse to continue on a mismatched C6, since proceeding
   produces failures far harder to diagnose
4. `nvs_flash_init()` with erase-and-retry — `esp_wifi_init()` persists calibration there
5. `esp_netif_init()`
6. `esp_event_loop_create_default()`
7. `esp_netif_create_default_wifi_sta()`
8. `esp_wifi_init()` via esp_wifi_remote
9. Event handler registration
10. `esp_wifi_set_mode(WIFI_MODE_STA)` — station only, always
11. `esp_wifi_start()`

Hosted NimBLE is brought up separately and lazily by `bluetooth.c`, which receives the
same host callback surface during `networking_init()`.

**Station-only** is enforced at two levels: the code always sets `WIFI_MODE_STA`, and
`CONFIG_ESP_WIFI_SOFTAP_SUPPORT` / `CONFIG_WIFI_RMT_SOFTAP_SUPPORT` are both disabled so
the AP path is not compiled in. Both symbols matter because esp_wifi_remote mirrors the
Wi-Fi Kconfig under its own `WIFI_RMT_` prefix.

- **Wi-Fi startup**: Background task on boot, version compatibility gate against C6 firmware
- **Hosted transport**: ESP32-C6 over SDIO (CLK=18 CMD=19 D0=14 D1=15 D2=16 D3=17, reset GPIO54)
- **Version gate**: Reads C6 hosted firmware version after SDIO link up; refuses Wi-Fi init if major/minor mismatch
- **Event handling**: WIFI_EVENT and IP_EVENT handlers for connection state tracking; the
  STA_CONNECTED handler also records `s_wifi_associated_at_us` so `wifi status` can report
  association uptime
- **Command dispatch**: `wifi status|scan [/b]|diag|connect|disconnect` with password masking.
  `wifi status` prints a multi-line colour-coded report (SSID, BSSID, channel, RSSI, PHY mode
  + bandwidth, IPv4, netmask, gateway, DNS, uptime); `wifi scan` returns results sorted by RSSI
  in aligned columns capped at `P4_CONFIG_WIFI_SCAN_LIMIT`, and `wifi scan /b` emits bare SSID
  lines that stay uncoloured so redirected output is machine-parsable.
- **Ping**: `networking_wifi_ping()` runs a classic ICMP echo over the lwIP `esp_ping` session
  (this module is the sole owner of the lwIP surface). Resolves the target with `getaddrinfo`,
  prints reply lines and a DOS-style statistics summary, and returns an `esp_err_t` the
  dispatcher maps onto ERRORLEVEL. The session runs on its own task; the worker task blocks for
  a strictly bounded total (`count` x (timeout + interval) + margin).
- **DNS**: `networking_wifi_dns_lookup()` resolves A records through lwIP `getaddrinfo` and
  prints the IPv4 list (bounded by `P4_CONFIG_DNS_RESULT_LIMIT`), also errorlevel-aware.
- **HTTP client (`httpget` / `wget`)**: `networking_http_get()` performs a simple HTTPS or
  HTTP GET over `esp_http_client` (the same stack c6ota uses for firmware downloads), with
  the certificate bundle attached for `https://`, a bounded timeout, redirects following
  `P4_CONFIG_HTTP_FOLLOW_REDIRECTS`, the User-Agent from `P4_CONFIG_HTTP_USER_AGENT`, and the
  body buffered in PSRAM up to `P4_CONFIG_HTTP_MAX_BODY_BYTES`. It prints the response header
  (status / content-type / size) with semantic colours and returns the body for the command
  layer to print or save to SD; the return value maps onto ERRORLEVEL (0 = HTTP 2xx).
- **OTA hooks**: `networking_wifi_wait_for_ota()`, `networking_wifi_shutdown()`, capture/restore state
- **Boot restore**: Automatic Wi-Fi restore after normal boot and after successful `c6ota`
- **Diagnostics**: Transcript-facing status + scan output via `wifi diag`
- **Status accessors**: `networking_wifi_is_connected()`, `networking_wifi_state()`,
  `networking_wifi_state_string()`, `networking_wifi_last_error()`, and
  `networking_wifi_get_rssi()`. The RSSI accessor is what keeps the header status bar
  from calling `esp_wifi_sta_get_ap_info()` directly; it also skips the driver query
  entirely when disconnected, avoiding a needless SDIO round trip each refresh.

### Bluetooth Module (components/networking/bluetooth.c)

Owns hosted NimBLE Bluetooth on ESP32-C6:

- **NimBLE VHCI**: Bluetooth HCI transport over ESP-Hosted SDIO
- **Controller lifecycle**: Stateful - `bluetooth enable` initializes once, subsequent commands reuse
- **BLE scan**: Passive scanning with device name resolution. A scan run is bounded by
  `P4_CONFIG_BT_SCAN_DURATION_MS` (passed as the `ble_gap_disc` duration) so it always
  terminates; results are collected, sorted by RSSI strongest-first, and printed as an aligned
  `NAME  ADDRESS  RSSI` report capped by `P4_CONFIG_BT_SCAN_LIMIT` (or the optional per-run
  limit).
- **BLE advertising**: Non-connectable advertising. `bluetooth advertise on [name]` accepts a
  session-only advertising name stored in `s_bluetooth_state.session_advertise_name` (RAM-only,
  never persisted); without one the configured default device name is used.
- **Status reporting**: Colour-coded report of controller readiness, NimBLE sync state,
  advertising state (with active name), C6 firmware version, and last error
- **Shared callbacks**: Uses same `networking_host_ops_t` as Wi-Fi module

### USB Module (components/usb)

Owns ESP-IDF USB Host Library with two class drivers:

- **MSC (Mass Storage Class)**: VFS/FATFS registration at `/usb0`, mount on demand
- **HID (Human Interface Device)**: Keyboard and mouse with opt-in transcript echo
- **USB keyboard auto-detect**: Automatically detects USB HID keyboard on plug-in; routes keystrokes to CLI input line via `shell_usb_keyboard_input()` bridge
- **Full US key map**: 60+ USB HID key codes including symbols, keypad, navigation, function keys, and modifier-aware shifted characters
- **On-screen keyboard auto-hide**: LVGL keyboard automatically hidden when USB keyboard attached; restored on detach
- **External input mode API**: `keyboard_set_external_input()`, `keyboard_force_visible()`, `keyboard_clear_force_visible()`
- **Input callback**: `usb_register_keyboard_input_callback()` for shell CLI integration
- **Public key mapping**: `usb_key_to_ascii_full()`, `usb_key_name_full()` for external consumers
- **Command family**: `usb status|ls|keyboard on|off|mouse on|off`
- **Bounded output**: Directory listings and file previews mirror SD command style
- **Transcript integration**: Uses dedicated host bridge functions in main.c

### C6 OTA Module (components/c6ota)

Owns the full ESP32-C6 firmware update workflow:

- **Source parsing**: `sd:/path`, `/sdcard/path`, `http[s]://url`, or `default`
- **Default resolution**: LFN-safe SD root lookup for `esp32c6_hosted_slave.bin` or `network_adapter.bin`
- **HTTP download**: Uses `esp_http_client` with Wi-Fi readiness wait
- **Image validation**: ESP-IDF app magic `0xE9` + ESP32-C6 chip ID `0x000D`
- **Factory warning**: C6 firmware `v2.3.0` requires one-time standalone tool first
- **Confirmation flow**: Exact prompt `WARNING: This will reboot the C6. Type YES to continue`
- **Transfer**: 1500-byte chunks over ESP-Hosted SDIO in Wi-Fi-off mode
- **Progress**: `C6 OTA: XX% (YYYY KB / ZZZZ KB)` every 5%
- **Wi-Fi management**: Stop before transfer, restore on failure, request post-OTA restore on success
- **Hosted transport**: Kept alive during transfer (no `esp_hosted_deinit()` to avoid assert)

## Hardware Configuration

### Pin Assignments

| Function | GPIO | Notes |
|----------|------|-------|
| I2C SDA | 7 | Shared bus for GT911 touch and peripherals |
| I2C SCL | 8 | Shared clock |
| I2S DOUT | 9 | Audio codec data out |
| I2S LCLK | 10 | Word-select clock |
| I2S DSIN | 11 | Audio codec data in |
| I2S SCLK | 12 | Bit clock |
| I2S MCLK | 13 | Master clock |
| SDIO D0 | 14 | ESP32-C6 hosted data lane 0 |
| SDIO D1 | 15 | ESP32-C6 hosted data lane 1 |
| SDIO D2 | 16 | ESP32-C6 hosted data lane 2 |
| SDIO D3 | 17 | ESP32-C6 hosted data lane 3 |
| SDIO CLK | 18 | ESP32-C6 hosted clock |
| SDIO CMD | 19 | ESP32-C6 hosted command |
| Power Amp | 20 | Speaker amplifier enable |
| LCD Backlight | 23 | JD9165 panel backlight PWM |
| LCD Reset | 27 | JD9165 panel hardware reset |
| Battery ADC | 53 | Battery divider sense input (2:1) |
| C6 Reset | 54 | ESP32-C6 hosted reset/enable |

### Display Timing

| Parameter | Value |
|-----------|-------|
| Resolution | 1024 x 600 |
| Pixel Clock | 80 MHz |
| H Sync | 1344 |
| H BP | 160 |
| H FP | 160 |
| V Sync | 635 |
| V BP | 23 |
| V FP | 12 |
| MIPI DSI Lanes | 2 |
| DSI Bitrate | 1000 Mbps (macro), 550 Mbps (runtime) |

### Storage

- **SD Card**: FATFS at `/sdcard`, LFN with 255-char limit, heap-backed buffers
- **USB MSC**: VFS/FATFS at `/usb0`, mounted on demand
- **SPIFFS**: Partition `storage` at `/spiffs`, 7 MB

## Build System

### CMake Structure

- Root `CMakeLists.txt`: Sets `EXTRA_COMPONENT_DIRS` for managed components and the project's own
  components (`ansi`, `batch`, `command`, `display`, `keyboard`, `shell`, `storage`, `clock`,
  `windows`), and configures `board_config.h`
- `main/CMakeLists.txt`: Registers `main.c` and `p4minishell.c` with component dependencies
- `components/command/CMakeLists.txt`: Requires `batch` and `storage` in addition to `shell`
- `components/batch/CMakeLists.txt`: Requires `shell` and `storage`
- `components/storage/CMakeLists.txt`: Requires `shell`, `header`, the BSP, and `fatfs`
- `components/networking/Kconfig.projbuild`: Defines the `P4MINISHELL_WIFI_DEFAULT_*` options
  (lives with its only consumer so any project including the component gets the symbols)
- `test/CMakeLists.txt`: Standalone unit-test project. Pins `IDF_TARGET` to `esp32p4`, stages
  `board_config.h` and `p4minishell_config.h` into the generated config directory the same way
  the firmware project does, and lists every shared component directory including `storage`
  and `batch`.
- Component `CMakeLists.txt` files: Standard `idf_component_register()` for each module

A new component under `components/` must be added to BOTH the root `CMakeLists.txt` and
`test/CMakeLists.txt`, or one of the two projects will fail to resolve it.

### sdkconfig Profile

- Target: `esp32p4`
- Flash: 16 MB, QIO mode
- Optimization: Size (`-Os`)
- PSRAM: Enabled, 200 MHz, XIP from PSRAM disabled
- ESP-Hosted: SDIO host interface, 1-bit bus, reset GPIO 54
- Wi-Fi: Station-only, WPA2, nano newlib, warn-level logging
- FATFS: LFN heap-backed, 255 chars, UTF-8 encoding
- Power: Light sleep enabled (`CONFIG_PM_ENABLE`)
- Console: USB-Serial-JTAG
- **ESP-IDF**: v5.5.5

## Runtime Constraints

- ESP-Hosted reset policy: `SLAVE_RESET_ON_EVERY_HOST_BOOTUP` (required for this hardware)
- PSRAM XIP mapping disabled (prevents flash/PSRAM overflow at link)
- LVGL examples disabled (image budget)
- Station-only Wi-Fi (no SoftAP, WPA3, or enterprise)
- Heap-backed FATFS LFN buffers (not stack)
- SD VO4 LDO explicitly acquired at 3300 mV before mounts
- No `esp_hosted_deinit()` before OTA (causes assert on esp32p4)
- Command execution on dedicated worker task (not LVGL input callback stack)
- Original command text preserved for family handlers (wifi, sd, c6ota)
- Password masking in transcript and command history
- SD directory listings bounded to 128 entries
- `sd cat` preview bounded to 8192 bytes

### Boot Scripting (components/boot)

DOS-style boot configuration that runs at every boot before the interactive
prompt. Owned by components/boot (oot.c, oot.h), kept deliberately
small and free of private-state access.

- **Startup:** oot_run_startup() is called once from pp_main after all
  modules (storage, batch, command, display, networking, usb) are initialized.
- **File handling:** looks for CONFIG.SYS and AUTOEXEC.BAT on the SD root.
  Missing files are generated once from built-in templates when
  P4_CONFIG_BOOT_GENERATE_DEFAULTS is set. Safe no-op with no SD card.
- **CONFIG.SYS parser:** line-oriented, case-insensitive, skips blank lines and
  REM/; comments. Classic directives: SET, PATH=, PROMPT=, ECHO ON|OFF.
  Modern: ROTATE=, BRIGHTNESS=, DISPLAY_POWER=, VOLUME=, WIFI_SSID=,
  WIFI_PASSWORD=, WIFI_AUTOCONNECT=, WIFI=ON|OFF, BLUETOOTH=ON|OFF,
  BT_ADVERTISE=ON|OFF, USB_KEYBOARD=ON|OFF, USB_MOUSE=ON|OFF,
  GPIO <n> = OUT [HIGH|LOW]. Unknown directives warn once and are skipped.
- **Application:** hardware directives execute their command-line equivalent
  through the batch pipeline (atch_boot_execute_command()), reusing existing
  validation. State-only directives use module accessors
  (
etworking_wifi_set_boot_credentials, 
etworking_wifi_set_boot_autoconnect,
  atch_set_default_echo). Wi-Fi password is never echoed/logged. GPIO directives
  delegate to the existing gpio set safety check.
- **AUTOEXEC.BAT:** runs through the normal batch pipeline
  (shell_execute_batch_file()), cwd = SD root. Non-zero errorlevel is a warning.
- **Configurability:** all limits and names in p4minishell_config.h
  (P4_CONFIG_BOOT_*), documented in p4minishell_config.yaml under
  oot_scripting.
