# P4MiniShell Technical Documentation

This is the technical reference for how P4MiniShell is built: the module
layout, the boot sequence, the data flow, and the rules each subsystem
follows. It is aimed at developers extending or porting the firmware.

If you are new, start with [`readme.md`](readme.md) (what it is and how to run
it) and [`tutorial_getting_started.md`](tutorial_getting_started.md). To add
code, see [`SDK.md`](SDK.md); for the command surface, see
[`command.md`](command.md); for the API, see [`API.md`](API.md); for the
working rules, see [`ai-context.md`](ai-context.md).

- **Version:** v1.1.0 · **Target:** ESP32-P4 + ESP32-C6 · **ESP-IDF:** v5.5.5
- **UI:** LVGL 9.5.0 / esp_lvgl_port 2.9.0, JD9165 1024x600 + GT911 touch
- **License:** MIT (see [`licence.md`](licence.md))

## System overview

P4MiniShell is a layered embedded application: one process, one LVGL UI, and a
single command pipeline connect everything.

1. `app_main()` (`main/main.c`) brings up the display, window manager, shell
   UI, and command module, then starts the background subsystems (boot script,
   networking, USB, clock).
2. The SD card mounts first; the boot script (`CONFIG.SYS` / `AUTOEXEC.BAT`)
   runs before the ESP-Hosted C6 transport starts, because the card and the C6
   share the SDMMC host and its DMA-capable internal buffers.
3. The shell owns the transcript (an LVGL span group) and the input line. The
   UART console and the on-screen/USB keyboards all feed the same pipeline.
4. Commands run on a dedicated worker task. The pipeline handles quoting,
   variable expansion, redirection, pipes, and chaining before dispatch.
5. Subsystems push state to the header/LED/notification surfaces; the header's
   adaptive timer resamples telemetry off the LVGL task.

The dependency direction is strictly one way; the only upward dependencies are
inverted through small function tables (see [Layering](#layering) below).

## Module map

```
main/main.c                     App entry point, LVGL event callbacks, UI construction, host bridges
p4minishell_config.h/.yaml      Centralized tunables (C source of truth + documentation)
boards/<name>/board_config.h/.yaml  Board profile: pins, display timing (default jc1060p470c)
components/ansi/                ANSI/VT SGR processing, 16-colour palette, format builder, semantic palette
components/display/             Display manager (rotation, resolution, refresh, brightness, power)
components/windows/             Window manager (screen layout, dynamic scaling, styling, surface modes)
components/keyboard/            Keyboard manager (LVGL keyboard, modes, capabilities, external input)
components/shell/               Shell core (transcript, history, debug log, UART console, input line, sysinfo)
components/storage/             SD sessions, path resolution, FATFS conversion, cwd, DOS file/volume commands
  storage_nav.c / storage_files.c / storage_disk.c / storage_text.c / storage_fam.c
  storage_ini.c                 Shared INI/settings + temp-file core
  storage_csv.c                 RFC-4180-subset CSV parser (shared by `csv` and `export`)
  trash.c                       Recycle bin
components/batch/               Batch engine (labels, for/for /f, pipes, env, PATH, aliases, binds)
  batch_expr.c / calc.c         Integer-expression evaluator + floating-point calculator
components/boot/                CONFIG.SYS parser + AUTOEXEC.BAT runner + default-file generation
components/command/             Command dispatch, worker task, pipeline, and the split *_commands.c verb files
components/applib/              Native-app runtime library (transcript stdout, memory, time, input, state, ABI)
components/modal/               Shared modal runtime + ready-made surfaces (dialog/list/ask/browse/view/hexview/imageview/form)
components/editor/              `edit` editor: byte-preserving document model + LVGL surface + focus/spell/templates
components/tui/                 TUI 80x25 cell buffer + draw primitives + tui_flush
components/gfx/                 RGB565 raster core + BMP parse/decode/scale + 8x8 font + plot viewport
components/filetype/            Central extension -> kind registry
components/markdown/            CommonMark-subset renderer (ANSI) + HTML serializer + print paginator
components/font/                Font registry (roles/sizes/fallbacks), SD TTF loader, CJK attach, themes
components/db/                  Palm-OS-style SD record store (sd:/DBS/<name>.DB)
components/alarm/               SD alarm store + single background checker (sd:/ALARMS)
components/audio/               ES8311 codec path + background tone/WAV playback engine
components/clock/               Time/SNTP/timezone services + date/time/timezone/sntp + named timers
components/header/              Fixed top status bar (layout/status/notify/refresh pure helpers + widgets)
components/led/                 WS2812 RGB status LED driver + auto status/event engine
components/networking/          Sole owner of ESP-Hosted + esp_wifi_remote, BLE, HTTP client/server, netdiag
components/usb/                 USB host (MSC storage at /usb0 + HID keyboard/mouse + lazy CDC-ACM serial)
components/c6ota/               ESP32-C6 firmware OTA via ESP-Hosted SDIO
samples/whoami/               Sample native app component (`whoami`, applib-only)
coprocessor/esp32c6_slave/      ESP32-C6 hosted slave firmware project
```

The command module's verb bodies live in focused files under
`components/command/` (`tui_commands.c`, `gfx_commands.c`, `image_commands.c`,
`plot_commands.c`, `serial_commands.c`, `periph_commands.c`,
`power_commands.c`, `audio_commands.c`, `font_commands.c`, `md_commands.c`,
`json_commands.c`, `asset_commands.c`, `pkg_commands.c`, `db_commands.c`,
`alarm_commands.c`, `config_cmd.c`, `gfind_commands.c`, `csv_commands.c`,
`export_commands.c`, `crypt_commands.c`, `userial_commands.c`,
`security_commands.c`, `header_commands.c`, `ui_commands.c`).

## Configuration system

All tunable values are centralized in `p4minishell_config.h`, organized by
subsystem with `P4_CONFIG_`-prefixed macros. The companion
`p4minishell_config.yaml` documents every value with type, description, and
range. Backward-compatible `SHELL_*`, `NETWORKING_*`, `BLUETOOTH_*`,
`HEADER_*`, `C6OTA_*`, and `USB_*` aliases are defined in each source file that
needs them.

Three configuration sources exist, each with a distinct role:

- `p4minishell_config.h` - C-level tunable values (buffer sizes, limits, colours, stack sizes)
- `board_config.h` - hardware pin assignments and display timing (from the
  active `boards/<name>/board_config.yaml` profile, default
  `boards/jc1060p470c/`)
- `sdkconfig` - ESP-IDF build configuration (Kconfig-driven)

## Architecture

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
reads that state through function pointers in `shell_command_ops_t`. The accessors
(`wifi_is_connected`, `wifi_get_rssi`, `wifi_state_string`, `append_sysinfo_summary`,
`bluetooth_is_enabled`, `bluetooth_is_connected`, `usb_is_connected`, `usb_is_keyboard_attached`,
`bg_jobs_running`, `usb_key_to_ascii`, `c6ota_is_pending`, `c6ota_is_busy`,
`pm_notify_activity`, `pm_ms_until_idle_off`) are all registered by `command_init()`.
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

- **Transcript system**: Scrollable LVGL span group backed by a 65536-byte ANSI buffer
  (`P4_CONFIG_TRANSCRIPT_BYTES`, PSRAM) with overflow protection. Appends track the buffer length
  (`s_transcript_len`/`s_transcript_ansi_len`) so no `strlen` scan is needed per line; the
  memory-pressure guard trims the oldest scrollback (keeping the newest three quarters) and a
  `[history truncated]` / `[history trimmed under memory pressure]` marker is inserted. New output
  auto-follows the view only while it is
  near the bottom (`P4_CONFIG_TRANSCRIPT_SCROLL_FOLLOW_PX`); submitting a command forces a
  jump to the newest output so the user always sees the result of what they ran.
- **Memory-pressure auto-trim**: the LVGL span objects that render the scrollback carry
  per-span overhead in the internal (DMA-capable) heap, which is shared with the WiFi/SDIO
  transport. When free internal RAM drops below
  `P4_CONFIG_TRANSCRIPT_INTERNAL_TRIM_BYTES` (4096 in v0.35.1, trim keeps 1/4 and guards trim-below-10KB), `shell_transcript_guard_internal()` (called at
  every command start and before every append, under the LVGL port lock) drops the oldest
  portion of the scrollback and frees its spans synchronously, so a tiny stdio allocation can
  never abort the board mid-command (see bugs.md M6, M19).
- **Async transcript buffer**: Thread-safe 512-byte staging buffer for background-task output (1024→512 in v0.35.1),
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
- **Command history**: heap-backed recall buffer (`P4_CONFIG_COMMAND_HISTORY_DEPTH`, capped at
  `P4_CONFIG_HISTORY_TOTAL_BYTES`) with password masking for `wifi connect`, Up/Down recall, and
  the `history` command for SD save/load (`P4_CONFIG_HISTORY_PROFILE`).
  Masking is applied both at the caller (`shell_command_should_store_history()`) and inside
  `shell_store_command_history()` so no path can persist a password.
- **Serial console bridge**: stdin/stdout routed through the same shell path as the touch UI,
  with LVGL locking and a submission mutex
- **Input line**: Owns the prompt-prefix contract — `shell_input_line_set_text()`,
  `shell_input_line_reset()`, `shell_extract_input_text()`,
  `shell_input_line_repair_prompt()` (which restores the prompt after a keyboard backspace
  deletes into it), and `shell_input_line_paste()`. The line and the whole command pipeline
  accept up to `P4_CONFIG_COMMAND_BYTES` (4096); every command-sized transient buffer is
  heap-allocated so the worker/UART/LVGL stacks stay small. USB Tab runs completion through the
  `shell_command_ops_t.complete_line` hook (help-table commands/aliases/apps, usage-derived
  subcommands/flags, and SD paths, `P4_CONFIG_COMPLETION_MAX_MATCHES` cap); the inline ghost
  suffix uses the SD-free `ghost_line` hook (`P4_CONFIG_COMPLETION_GHOST`).
- **Debug log**: 5-entry circular buffer surfaced via the `debug` command. Errors are also
  echoed to the transcript so on-screen users see failures without running `debug`.
- **RAM clipboard** (`clip` / `paste`): a shell-core clipboard (`P4_CONFIG_CLIPBOARD_BYTES`)
  holding text (copied transcript lines via `shell_clipboard_copy_transcript`, literal text via
  `clip <text>`, or a text file via `clip read`), or a file reference (`clip file`) that
  `paste <dest>` copies. `paste` injects the clipboard into the input line at the cursor via
  `shell_input_line_paste()` (`lv_textarea_add_text`). The commands live in
  `components/command/` and call the `shell.h` clipboard API; all are batch-safe,
  redirectable, and set an ERRORLEVEL.
- **Interactive keypress queue**: `shell_key_wait_begin()` / `shell_wait_for_key()` /
  `shell_key_wait_end()` let `pause`, `choice`, and `more` block on a real keystroke. All three
  input sources (UART console reader, USB HID bridge, LVGL on-screen keyboard) feed the queue
  through `shell_key_wait_submit()`, and every source suppresses command-line handling while a
  wait is active so an answer is never dispatched as a command. Waits are bounded by
  `P4_CONFIG_KEY_WAIT_TIMEOUT_MS`, and `shell_key_input_available()` lets commands fall back to  a timed delay on a headless board.
- **Line input**: `shell_read_line()` collects a typed line through the key queue, echoing as
  it goes. Backspace edits, ESC cancels, Enter submits. Backs `set /p`.
- **UART console line assembly**: the console reader (`shell_uart_console_task`) uses
  line-buffered `fgets` on unbuffered stdin, and USB-Serial-JTAG delivers one logical line
  across several reads (its RX FIFO is 64 bytes). The task assembles fragments until a line
  terminator (or the 256-byte command buffer fills) before submitting, so a long command is
  never split into two. During an active key wait the first newly-read character is still
  answered immediately, without waiting for the terminator. When a native modal surface is
  active the worker is blocked, so the reader first offers the line to
  `shell_command_ops_t.modal_console_command` (the streaming `screenshot` runs here), then to
  the modal's own serial handler, and only then queues it to the worker. `shell_uart_console_rx_begin/end`
  skip their `vTaskSuspend/Resume` when the console task is the caller (the reader running a
  capture), so the task never suspends itself; a worker-side `receive`/`send` still freezes the
  reader as before.
- **DOS prompt template engine**: `shell_prompt_set_template()` stores the template the
  `prompt` command supplies; `shell_prompt_render_plain()` expands `$p $g $l $b $n $d $t $v $s
  $_ $q $$ $a $c $f $e $h` against live state. One template drives both the UART console prompt
  and the LVGL input line. The input line snapshots the prefix it painted so a template or path
  change between two LVGL events cannot corrupt command extraction.
- **System info commands**: `help`, `sysinfo`, `version`, `about`, `mem`, `debug`
- **Task introspection** (`ps` / `tasks` / `top`): read-only FreeRTOS task listing from a
  heap-allocated `uxTaskGetSystemState()` snapshot (capped by `P4_CONFIG_TASK_SNAPSHOT_MAX`).
  Prints name, state, priority, core, stack high-water mark, and per-task CPU% (diffed from the
  previous sample via `ulRunTimeCounter`). The snapshot array lives on the heap so the 8 KB
  command-worker stack is never at risk; `/b` emits uncoloured rows for pipes. `dir /O:`-style
  sorting (`/O:N|C|S|P|T`, `-` reverses; `top` defaults to CPU descending) sorts normalized
  heap rows with the same qsort comparator pattern, and the pure `shell_task_row_compare()`
  helper is unit-tested. All three verbs set an ERRORLEVEL (0 ok / 2 usage).
- **Header CPU sparkline**: the header system panel shows a small LVGL chart of the recent CPU
  samples (bars amber above `P4_CONFIG_HEADER_CPU_WARN_PCT`), replacing the single-value bar
  when `P4_CONFIG_HEADER_CPU_GRAPH` is set; the ring advances on the periodic header refresh.
- **Header status refresh**: Batches every header field (including the clock text and
  the OTA/bg-job activity flags) into one async render to avoid flicker; also drives
  USB keyboard auto-detect and SD insert/remove notifications. Cheap status is read
  every adaptive tick while heap/CPU/battery telemetry is throttled to
  `P4_CONFIG_HEADER_TELEMETRY_PERIOD_MS`, and the whole pass is skipped while the
  display is off by idle.
- **Adaptive header poll**: `shell_header_refresh_interval_ms()` (shell) feeds
  `header_refresh_interval_ms()` (pure, `components/header/header_refresh.c`) and
  main reschedules its single header timer. `shell_power_ms_until_idle_off()`
  bounds the idle interval so the display-off deadline is never missed.
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
- **Modal surface routing**: `shell_command_ops_t.modal_is_active` /
  `modal_handle_usb_key` / `modal_handle_serial_line` route USB and serial input
  to whichever native modal surface is active (editor, `dialog`, `list`, `ask`).
  The shell core does not know which surface is open; it only checks the shared
  modal hook.
- **USB keyboard injection**: `shell_usb_keyboard_input()` dispatches HID events into the
  input line via `lv_async_call`; while a modal surface is active the key goes
  to the modal hook instead.

### Storage Module (components/storage)

Owns everything that sits between the shell commands and the SD card. Split across several files:

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

**DOS file and text commands** (implemented in `storage_nav.c` / `storage_files.c` /
`storage_text.c`; their shared declarations live in `storage_commands.h`)

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
- Recycle bin: `del`/`erase` and `rd /s` move matching files and whole directory trees into a
  hidden `.trash` folder (with a `.meta` side-car recording the original path) instead of
  deleting them; `undelete`/`restore` and `trash restore` rename entries back to their
  original location (recreating parent directories, never overwriting), and
  `trash list|info|purge|empty` manage the bin. `/p`/`/f`/`/permanent` bypass the bin and
  delete outright. Byte/age/entry-count limits purge the oldest entries first, and
  `del /s`/`rd /s`/`trash purge`/`trash empty` are gated by the exact confirmation word.
  `dir` hides hidden/system entries by default, so `.trash` stays out of listings unless a
  bare `dir /A` asks for everything. `del`, `rd`, `format`, `disk`, `undelete`, and `trash`
  return a real ERRORLEVEL (0/1/2).
- Disk and partitions: the `disk` family (`list`, `detail`, `clean`, `create partition
  primary [size=N]`, `delete partition N`, `format`) manages the MBR partition table through
  `sdmmc_read_sectors`/`sdmmc_write_sectors` on the BSP card handle, unmounting the FATFS
  volume first. The volume services in `storage.c` are parameterized by `storage_volume_t`
  so a future USB OTG MSC volume can be added without changing the command surface.
- Recursive directory tree: `tree [path] [/F] [/A]` with DOS box-drawing connectors, bounded by
  `P4_CONFIG_TREE_DEPTH_MAX` and the 128-entry listing cap. Each directory level is buffered on
  the heap rather than the worker-task stack, so a wide directory cannot overflow it.
- File manipulation: `copy`, `move`, `del`/`erase`, `ren`/`rename`, `md`/`mkdir`, `rd`/`rmdir`,
  `type`, `write`, `append`, `touch`, `undelete`/`restore`, `trash`/`recycle`
- Extended DOS tools: `attrib` (R/H/S/A via `f_stat`/`f_chmod`), `label` (via
  `f_getlabel`/`f_setlabel`), `xcopy` (recursive with `/S`)
- Text utilities: `find` (classic text search `/I /N /C /V` plus a recursive
  file-discovery mode `/NAME:`, `/SIZE:`, `/NEWER:`, `/OLDER:`, `/DIRS`, `/B` — mode is
  selected automatically by the presence of a discovery switch), `findstr`
  (literal or DOS-subset regex search, case-sensitive by default,
  `/R /C /I /N /V /X /E /B /L /S /M /F /G`), `more` (keypress paging with
  `Q` to quit), `fc` (DOS-style differing-line report), `comp` (byte compare
  with `/D /A /L /N /C`), `sort` (qsort-based, `/R /I /U`,
  1024-line capacity with a single leak-free release path). All resolve relative paths and run
  inside a guarded SD session, and all read the pending input-redirection source when no
  filename is supplied. The discovery walker reuses the `dir /s` FATFS primitives, keeps each
  recursion level in one heap block, is depth-bounded by `P4_CONFIG_DIR_RECURSE_DEPTH_MAX`, and
  caps matches at `P4_CONFIG_FIND_MATCH_MAX` (256). `find`, `findstr`, and `comp` return a DOS
  ERRORLEVEL (0 found / identical, 1 not found / different, 2 usage), and `more`, `fc`, and
  `sort` return 0/1/2 too, so `if errorlevel` and `&&`/`||` work with every text command.
- Recursive copy: `xcopy` supports the full DOS 6.x / WinXP switch set
  (`/S /E /I /Y /-Y /D[:date] /H /R /K /C /Q /T /F /L /A /M /U /P /W /N /V`)
  and walks trees with one heap block per recursion level, never re-entering
  the command, so the 8 KB worker stack is safe. Overwriting asks at the
  prompt when interactive and falls back to `/Y` (silent) when headless.
- SD command family: `sd info|ls|stat|cat|eject`, bounded to 128 listing entries and an 8192-byte
  `sd cat` preview
- **Persistent state + temp files** (`storage_ini.c`): the pure INI line editors
  (`storage_ini_get_value` / `storage_ini_upsert` / `storage_ini_remove`) are the single
  `KEY=VALUE` text editor (the `config` command's `config_directive_*` are thin wrappers);
  `storage_ini_file_*` read/update/iterate INI files through guarded SD sessions with atomic
  temp+rename writes and on-demand parent-directory creation; `storage_write_text_file`
  writes any text file atomically; `storage_temp_path` / `storage_temp_cleanup` keep temp
  files under `sd:/tmp`. All storage lives on the SD card. Shared by the `config`/`ini`/
  `appconfig`/`temp` commands and the applib state group — never re-implemented elsewhere.

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
  (`P4_CONFIG_BATCH_LABEL_MAX` = 128 targets, names up to
  `P4_CONFIG_BATCH_LABEL_BYTES` = 64 bytes), so `goto`, `call :label`, `gosub`
  and `on` are an `fseek()` across the file; a target may be written `:label` or
  bare `label`. `goto :eof` is an implicit end-of-file label: it ends the current
  frame exactly like reaching the end of the file, unwinding any open setlocal
  scopes. A pending goto is always cleared when a frame returns, so a
  `goto`/`goto :eof` issued from inside a `for` or `if` body in a called script
  cannot leak into the caller's line loop. `call :label`/`gosub :label` push a
  label-call scope resumed by `return`/`exit /b`/`goto :eof`; `on <expr> gosub|goto`
  dispatches on a 1-based `set /a` index (out of range falls through). A `goto` to
  a missing label aborts the frame, while a missing `call`/`gosub`/`on` target sets
  errorlevel 1 and continues (cmd.exe parity). `rem`/`::` comments are opaque to
  end of line (`shell_comment_line()` runs before the chain/pipe/redirect parsers).
- **`for` loops**: `for %%var in (set) do command` with per-iteration `%var` substitution.
  The set is either a space-separated literal token list (`for %%I in (a b c) do echo %%I`)
  or a single wildcard pattern (`for %%F in (*.txt) do echo %%F`), which is expanded through
  the storage layer's `storage_expand_wildcard()`. The loop body is expanded into a
  heap-allocated buffer because the loop re-enters the command pipeline on the recursive
  batch path. `for /f "eol=c skip=n delims=xyz tokens=a,b,m-n" %%v in (file-set) do cmd`
  iterates over a file's lines (or the active `< file`/pipe source when the set is empty):
  the pure `shell_forf_parse_options` / `shell_forf_split_line` helpers parse the DOS
  options and split each line without mutating it, token indices bind to consecutive
  loop-variable letters (`%%a %%b ...`), and a `*` captures the rest of the line. This is
  the mechanism behind the BASIC `READ`/`DATA`/`INPUT#` verbs.
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
  `%1`..`%9` (the caller's arguments), `%*` (every argument from `%1` onward), `%%` → `%`,
  and the dynamic pseudo-variables `%DATE%` (`MM-DD-YYYY`), `%TIME%` (`HH:MM:SS`),
  `%RANDOM%` (`0..32767`), `%CD%` (current directory), and `%ERRORLEVEL%`. An undefined
  `%VAR%` expands to the empty string (cmd.exe parity) so the DOS `if "%var%"==""` idiom
  works; single-quoted runs stay literal and `^%` is a literal percent.
- **`if` forms**: `if [not] [/i] errorlevel N`, `if [not] [/i] exist <path>`,
  `if [not] [/i] defined <name>`, the numeric keywords (`EQU NEQ LSS LEQ GTR GEQ`), and
  `==` string tests. An undefined variable in a numeric operand reads as 0 (DOS parity).
- **Aliases (DOSKEY-style macros)**: `shell_alias_get`/`shell_alias_set` drive a RAM-only table
  (`P4_CONFIG_ALIAS_MAX` slots, case-insensitive names). `batch_alias_expand_command()` replaces
  the leading word of an interactive command line with the alias value before parsing; it is
  suppressed inside batch files so an alias cannot shadow a batch verb. `alias`/`unalias`
  manage the table, and `alias /save` persists it as `alias name="value"` lines to
  `sd:/ALIASES.BAT`, which boot.c auto-runs after CONFIG.SYS (`alias /load` reloads manually).
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
  empty line leaves the variable unchanged, as in DOS. When a `< file` redirection or a pipe
  stage is active (`storage_resolve_input_source`), `set /p NAME=< file` reads one line from
  that source instead of the interactive key queue, matching cmd.exe — the BASIC
  `INPUT#`/`LINE INPUT#` surface.
- **`calc` calculator**: `calc [NAME=] <expr>` (components/batch/calc.c) evaluates a
  floating-point expression over `double` / fixed-string values with a recursive-descent
  parser, stores the result in an environment variable when `NAME=` is given, and sets
  ERRORLEVEL (0 ok / 1 domain|syntax / 2 usage). The grammar adds the FX-870P/VX-4 math and
  string functions (`ABS SGN INT FIX FRAC ROUND SQR EXP LN LOG SIN COS TAN SINH COSH TANH
  ASN/ASIN ACS/ACOS ATN/ATAN FACT NCR NPR DMS DMS$ VAL VALF STR$ HEX$ ASC CHR$ LEN LEFT$
  MID$ RIGHT$ MOD POL REC`), `&H`/`0x` hex literals, the `PI` constant, a seeded `RAN#`
  generator, string literals (`'`/`"`) with `+` concatenation, and env-var references
  (undefined reads as 0). `calc /deg|/rad|/angle` set/query the trig angle mode;
  `calc /hex` prints an integral result as `&H` hex. `POL`/`REC` store their two results in
  the X/Y environment variables, the calculator's documented side effect.
- **Line continuation**: a trailing `^` joins the next physical line, bounded by
  `P4_CONFIG_LINE_CONTINUATION_MAX`. An odd-caret-count rule distinguishes a continuation from
  an escaped `^^`. The label scanner applies the same rule, so a continued line cannot
  register a phantom `:label`.
- **Batch language commands**: `set` (with `/a` and `/p`, including the `< file` source
  form and an optional `/T:secs` prompt timeout), `calc`, `path`, `echo` (including the
  `echo.` blank-line idiom), `call`
  (including `call <file.bat>::<routine>` shared-library calls with automatic
  variable isolation), `if`
  (with `errorlevel N` — true when errorlevel ≥ N, `exist <path>`, `/i` case-insensitive
  string comparison, and `not` for all three), `for` (classic and `for /f`), `goto`
  (including `goto :eof`), `call :label`/`gosub`/`return`/`on` (local subroutines and
  BASIC computed dispatch), `shift`, `proc` (process-stack introspection), `ini`
  (persistent `KEY=VALUE` files, import/export the environment), `appconfig` (per-app
  settings file `sd:/APPS/<APP>.INI`), `temp` (SD-backed temporary files), `ansi`
  (menu/form primitive: emit text styled with SGR codes), `menu` (numbered form returning
  the chosen index as ERRORLEVEL),
  `pause`, `choice`, `setlocal`, `endlocal`, `exit`, `delay <ms>` (a pure
  deterministic wait, unlike light-sleep `sleep`), `notify` (header
  notification), and the native modal surfaces `dialog` / `list` / `ask`
  (rendered by `components/modal/`, returning results via ERRORLEVEL and
  variables). `pause` and `choice` block on a real keystroke through the
  shell core's key queue.
- **Nested execution**: every nested line goes back through the full command pipeline via
  `batch_command_ops_t`, so it inherits variable expansion and redirection
- **Batch process model**: a batch file runs as a command stream on the single worker task
  (no process isolation). Its contract is fully defined: **stdout** is the transcript delta
  captured by `>`/`>>`; **stderr** is not a separate stream (errors interleave and failure is
  signalled by ERRORLEVEL); **stdin** is the storage input-redirection slot consumed by the
  text tools, `for /f` over an empty set, and `set /p NAME=< file` (the interactive key
  queue backs `set /p`/`pause`/`choice` when no redirect is active); **argv** is `%0`..`%9`/
  `%*` with `call`/`shift`; **cwd** is the storage-owned current directory; **PATH** is the
  batch-owned environment slot used by `shell_resolve_batch_path()`; and **environment
  propagation** is the shared 24-slot RAM table mutated by `set`/`set /a`/`set /p`/`calc`
  and scoped by `setlocal`/`endlocal` (auto-unwound on frame return). See `command.md`
  "Batch process model" and `SDK.md` "Batch process model" / "Authoring and deploying batch
  files" for the full authoring and deployment rules.

### Applib Module (components/applib)

A leaf component that formalizes the native-app runtime contract (the four
"Runtime services to define" from the roadmap). Its public API is organized
as lean headers — an app includes only the groups it uses; the umbrella
`applib.h` includes all of them:

- **Console output API** (`applib_console.h`): `app_printf`/`app_printf_ansi`
  and the semantic `app_print_*` helpers map an app's stdout onto the shell
  transcript, which is the redirection layer — an app invoked inside a command
  dispatch is captured by `>`/`>>` exactly like a built-in command.
- **Memory allocation policy + error reporting** (`applib_mem.h`):
  `app_alloc`/`app_calloc`/`app_realloc`/`app_strdup`/`app_strndup`/`app_free`
  implement the shared policy (blocks of
  `P4_CONFIG_APPLIB_PSRAM_THRESHOLD_BYTES` or more prefer PSRAM with an
  internal-heap fallback); `app_report_error`/`app_report_warning`/
  `app_report_info` route into the shell debug log.
- **Time / timers / sleep / sysinfo** (`applib_time.h`): `app_time`,
  `app_time_local`/`app_time_utc`, `app_uptime_sec`, `app_now_ms`,
  `app_delay_ms`, `app_time_synced`, `app_uptime_formatted`, `app_sysinfo`
  over the clock component.
- **Networking helpers** (`applib_net.h`): `app_wifi_is_connected`/
  `app_wifi_get_rssi`/`app_wifi_state_string` read Wi-Fi state through the
  registered `applib_net_ops_t` table (registered by `command_init()`), so
  applib never includes `networking.h`.
- **Input with timeout** (`applib_input.h`): `app_wait_key(timeout_ms, &key)`
  and `app_read_line(buf, size, timeout_ms)` give native apps bounded
  keypress/line reads (the primitive behind `pause` / `choice /T` / `set /p
  /T`), returning false on a headless board instead of stalling.
- **Persistent state** (`applib_state.h`): `app_ini_get`/`app_ini_set`/
  `app_ini_delete` read and update `KEY=VALUE` INI files on the SD card and
  `app_temp_path`/`app_temp_cleanup` manage SD-backed temporary files, all
  wrapping the shared `storage_ini.c` core (no duplicated file parsing).
- **App mode + notifications** (`applib_ui.h`): `app_mode_enter`/`app_mode_exit`
  wrap the shell-core screen save/restore primitives (shared with the batch
  `appmode` command), and `app_notify` routes header notifications through the
  same path as the batch `notify` command.
- **Native-app ABI** (`applib_app.h` + `applib_env.h`): an `app_main_t`
  function registered with `app_register()` becomes a shell command that
  receives `argc`/`argv` (`argv[0]` = app name) and returns an ERRORLEVEL a
  batch file can branch on. `app_env_get`/`app_env_set` read/write the shell's
  shared RAM environment table and `app_get_cwd` returns the storage cwd,
  routed through the `applib_env_ops_t` table registered by `command_init()`
  (env is batch-owned, cwd is storage-owned — applib never includes
  `batch.h`). The `apps` shell command lists registered apps; `main/native_apps.c`
  registers the reference `hello` sample.

The module depends only on `shell`, `clock`, `storage`, and the FreeRTOS/
heap/esp_timer IDF components. `components/command` requires it to register
the networking and environment hooks; native apps link `applib` instead of
reaching into module internals.

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
- **Dispatcher**: `shell_execute_command_core()` — 155 distinct `argv[0]` spellings (aliases included) with family routing
  (wifi, bluetooth, usb, c6ota, sd) that receive the original unsplit command text
- **Serial file transfer** (`receive` / `send`, plus the `screenshot` stream):
  binary host<->device transfer over the USB-Serial/JTAG console. Both directions
  suspend the console reader (`shell_uart_console_rx_begin/end`) so the raw byte
  stream is never mistaken for command lines. `receive` is ACK-paced against the
  device's small RX ring and supports an optional CRC-32 trailer; `send` streams
  SD files (or byte ranges, or a `send /diag` report) framed as a 4-byte magic +
  4-byte little-endian size + payload. All three share
  `serial_write_frame_header()` and write payloads through
  `usb_serial_jtag_write_bytes()` in chunks, bypassing the console VFS's CRLF
  translation so binary data is byte-exact. Both commands set ERRORLEVEL for
  batch use. All tunables live in `P4_CONFIG_SERIAL_*`.
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
- **Basic audio** (`beep`, `tone <freq> [ms]`, `wavplay <file>`, `audio status|stop`,
  `volume [<0-100>]`): tones are generated in heap chunks and WAVs (16-bit PCM mono/stereo at
  22050/44100 Hz, stereo mixed to mono and 44100 decimated) stream from SD, both through the
  ES8311 codec (mono 16-bit 22050 Hz). All audio logic — codec init, speaker volume, and the
  background `audio_play` task — lives in `components/audio/` (`audio.h`/`audio.c`); the
  commands only dispatch from `components/command/` and call the `audio.h` API. One sound plays
  at a time (`audio stop` cuts it short) so batch files never block, and every audio command
  sets an ERRORLEVEL. `volume` and the boot `VOLUME=` directive drive the codec output level,
  which all playback rides on.
- **Peripheral toolkit** (`pwm`, `freq`, `adc`, `i2c`, `spi`): LEDC PWM/square waves on
  timers 0/2/3 sharing the backlight's XTAL clock; one-shot ADC reads with SOC channel-map
  enumeration; an I2C scanner/peek-poke that reuses the BSP shared bus handle (or a temporary
  bus on custom pins) via normal device transactions; and `spi status` reporting the SPI
  configuration. SPI transactions are refused with an honest error because SPI host init on
  this P4 with the ESP-Hosted SDIO link active stalls the chip. Every command gates its pins
  through `shell_pin_is_reserved()` (the critical entries of the board GPIO table), so active
  I2C/I2S/SDIO/display/SD lines can never be repurposed.
- **RGB status LED** (`rgb`): `components/led/led.c` owns the WS2812 strip on GPIO26
  (espressif/led_strip over RMT), a small animation task, and a mutex-protected state engine.
  It renders solid colours, effects (`rainbow`/`breath`/`pulse`/`blink`/`solid`), and transient
  event notifications on top of a persistent status colour. The `rgb` command lives in
  `components/command/command.c` (hardware verbs), the Wi-Fi/HTTP event hooks are pushed from
  `components/networking`, and `main.c` fires the boot confirmation flash. GPIO26 is a reserved
  critical line in the board pin table.
- **UI query commands**: `display info|resolution|refresh|power`, `keyboard show|hide|toggle|status`,
  `windows info` — implemented in `command_ui.c` to keep `keyboard.h` and `windows.h` out of
  `command.c`
- **System commands**: `reboot`, `clear`/`cls`, `prompt`, `date`, `time`
- **App discovery + launch**: `launch` (and `apps`) in `command.c` discover
  `.bat` apps on PATH + `sd:/APPS`, read optional `APPINFO` metadata
  (`sd:/APPS/<name>.APPINFO`), and run the chosen one via the batch engine. The
  discovery table is heap-allocated (a stack-resident table overflows the
  worker stack once a launched batch re-enters the dispatcher per line). The
  boot flow's `LAUNCH_APP=` CONFIG.SYS directive offers to launch an app after
  AUTOEXEC.BAT.
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
- **Idle display-off** (`power idle` + CONFIG.SYS `DISPLAY_TIMEOUT=`): the backlight turns
  off after N idle seconds and wakes on touch, USB keyboard/mouse, or a serial command.
  Because idle-off only drops the backlight, the shell state is untouched and wake is a clean
  backlight-on. Tracked in the power module (`shell_power_notify_activity` /
  `shell_power_idle_tick`, fed by the header-refresh LVGL timer and the input injection
  points), with `P4_CONFIG_POWER_IDLE_DISPLAY_OFF_SECS` (0 = disabled) as the default.
- **Sleep wake sources**: `sleep`/`deepsleep` keep the timer wake; a user-wired
  `P4_CONFIG_POWER_WAKE_GPIO` wakes light sleep via GPIO. Touch wake is honestly reported
  unavailable because the GT911 INT line is not wired on this board.
- **Display diagnostics**: Comprehensive `display_info_t` struct with all timing, buffer, and config data
- **Thread safety**: State variables protected by critical sections; LVGL operations dispatched via `lv_async_call`
- **UI rebuild callback**: Registered callback invoked after rotation changes to trigger full UI rebuild
- **Touch handle**: Lazy acquisition of GT911 touch handle from BSP; cached for rotation remapping
- **Public API**: `display_init()`, `display_set_rotation()`, `display_get_rotation()`, `display_set_brightness()`, `display_get_brightness()`, `display_get_resolution()`, `display_get_info()`, `display_set_power_state()`, `display_sleep()`, `display_wake()`, `display_set_refresh_rate()`, `display_register_ui_rebuild_callback()`

### Window Manager (components/windows)

Central layout manager owning the LVGL screen region partitioning and dynamic scaling:

- **Named regions**: HEADER, TRANSCRIPT, INPUT_ROW, KEYBOARD - each with computed bounding rectangles
- **Resolution-aware scaling**: All dimensions derived from display.c's current resolution
- **Rotation-aware**: Recalculates layout on rotation change via the display manager's UI rebuild callback
- **Consistent styling**: All colors accessed through semantic names via `windows_get_color()`
- **Font management**: Centralized terminal font selection via `windows_get_terminal_font()`
- **Lifecycle**: `windows_init()` builds all UI regions; `windows_deinit()` tears down before rotation rebuild
- **Public accessors**: Individual window objects accessible via getter functions for event callback registration
- **Works with display.c**: Queries `display_get_width()`/`display_get_height()` for current resolution
- **Delegates to header.c**: Header region creation delegated to `header_init()`/`header_deinit()`
- **Scrollable transcript**: The TRANSCRIPT region is a scrollable container holding a span
  group sized to its exact content height. New output follows the view only when it is near
  the bottom (`P4_CONFIG_TRANSCRIPT_SCROLL_FOLLOW_PX`); `windows_force_scroll_transcript_to_end()`
  pins the next repaint to the bottom (used on command submission), and
  `windows_scroll_transcript_by()` / `windows_scroll_transcript_to_top()` page it from the
  input-row `Up`/`Dn` buttons, the USB keyboard PageUp/PageDown, and the USB mouse wheel.
  The incremental span append keeps the render cost bounded, and an explicit-content-height
  child avoids the LVGL scrollable-container-with-LV_SIZE_CONTENT layout loop.
- **Editor surface**: the modal `edit` editor renders into the SAME transcript
  container, which stays visible at the transcript-region height while the
  editor hides the shell span group inside it. This keeps the editor area
  exactly as large as the shell transcript and the keyboard at the bottom (LVGL
  flex skips hidden children, so hiding the container would collapse it).
  `windows_refresh_editor_surface()` re-applies the region height on entry and
  keyboard show/hide; `windows_debug_editor_layout()` and
  `windows_editor_surface_height_ok()` make the geometry visible/verifiable.

### Header Module (components/header)

Passive, display-only module that owns the fixed top bar:

- Non-scrollable LVGL flex-row container
- Resolution-scaled height (display_height / 15, clamped 32-56px)
- Status icons (left-to-right): Wi-Fi, Bluetooth, USB, SD, plus a conditional
  activity indicator `A` (shown only while a C6 OTA or a background job runs)
- System panel (far right): MEM (free heap), CPU (bar + percentage), BAT (bar + percentage)
- Two status styles (`P4_CONFIG_HEADER_STATUS_STYLE`): verbose **words**
  (`WiFi HI`, `USB ON`, `SD ON`) or compact colored **glyphs** (`W BT U S A` and
  `M C B`) that hand the reclaimed width to the notification area. Both styles
  color by state: green healthy, amber degraded, red off/failed, muted absent.
- State classification is pure and centralized in `components/header/header_status.c`
  (glyph + tone + thresholds; the Wi-Fi HI/MID/LOW/WEAK label, the memory/CPU/battery
  healthy-warn-critical thresholds, and the on/off/error rules all live there once).
  `header.c` owns the widgets and maps the tone to `theme.text`/`warn`/`err`/`text_muted`.
- Center area shows the local clock (`time_format_hm()`, `"--:--"` until SNTP sync)
  when idle, and the active notification otherwise.
- Notifications are queued FIFO with a severity (`header_notify_queue.c`, pure and
  unit-tested); `header_notify(level, text, timeout)` colors info/warn/error, and an
  empty text flushes the queue. The queue reuses one persistent display timer.
- The poll cadence is adaptive: the pure `components/header/header_refresh.c`
  chooses the next interval from the situation (idle display-off wake, OTA/bg job,
  Wi-Fi bring-up, startup, clock minute boundary, idle-off deadline); `shell`
  assembles the inputs and `main` reschedules its single timer. Expensive
  telemetry is separately throttled by `P4_CONFIG_HEADER_TELEMETRY_PERIOD_MS`.
- Tap an indicator to show a one-line detail in the notification area; long-press
  runs the full status command through `header_register_status_action()`, registered
  by `command_init()` (the header stays a command-free leaf).
- Battery always visible — shows "BAT N/C" when ADC is not connected
- All system panel values dynamically linked to FreeRTOS runtime statistics
- CPU usage calculated from FreeRTOS idle task runtime counter deltas
- All public functions use LVGL async dispatch (safe from any task context)
- SD icon shows persistent state (NO/INS/ON/ERR)

### Clock Module (components/clock)

Leaf component that owns the C-library system clock, the timezone, the SNTP
client, and the time/date shell commands:

- **Clock API** (`clock.c`): `time_init()`, `time_start_sntp()`,
  `time_force_resync()`, `time_is_initialized()`, `time_is_synchronized()`,
  `time_get_local()`, `time_get_utc()`, `time_get_unix()`,
  `time_get_uptime_sec()`, `time_get_uptime_formatted()`,
  `time_get_formatted()`, `time_get_formatted_utc()`,
  `time_format_hm()` + the pure `clock_format_hm_snapshot()`, `time_is_set()`,
  `time_set_timezone()`, `time_get_timezone()`, `time_get_timezone_label()`,
  `time_set_utc_offset()`, `time_get_ntp_server()`.
  `time_is_set()` is true after an SNTP sync OR a manual `date`/`time` set
  (unix time at or above `P4_CONFIG_CLOCK_VALID_EPOCH`), which is what the
  header clock uses to choose between a real time and `--:--`.
- **Automatic timezone** (no hardcoded zone): `time_set_utc_offset(seconds, label)`
  builds the POSIX TZ string from a network-detected UTC offset ("UTC-2" for +2h,
  "UTC-5:30" for +5:30) and keeps the IANA label for display. The lookup itself is
  `networking_time_detect()` (components/networking, the sole HTTP owner) driven by
  the command layer's `timesync` task (`command_time_auto_sync`), which is kicked
  off once Wi-Fi associates and re-runs every `P4_CONFIG_TIMEZONE_RESYNC_SECS` so
  DST and travel stay correct. It NEVER runs on the LVGL task.
  SNTP uses `P4_CONFIG_NTP_SERVER`; the timezone is a POSIX TZ string
  (`P4_CONFIG_TIMEZONE_BYTES` max). `time_start_sntp()` is designed to run once
  lwIP is ready; `time_force_resync()` (used by `sntp sync`) restarts the
  client for an immediate exchange.
- **Command surface** (`clock_commands.c`): the `date`, `time`, `timezone`, and
  `sntp`/`ntpsync` command bodies live here. The clock component stays a leaf
  (shell -> clock), so the commands render through a `clock_host_ops_t` table
  registered by `command_init()`; each entry maps one-to-one onto the shell
  print/record helpers, keeping the on-screen output identical to a command in
  `command.c`. The table is NULL-checked, so the surface degrades gracefully
  before registration.
- **`date` / `time`** with no argument print the full clock panel (local, UTC,
  Unix timestamp, timezone, uptime, NTP sync status); the DOS-style set forms
  (`date MM-DD-YYYY`, `time HH:MM[:SS]`) update the C-library clock via
  `settimeofday`. `timezone [TZ]` shows or sets the POSIX TZ string.
  `sntp|ntpsync [sync]` shows NTP status or forces a re-sync.

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
- **Command dispatch**: `wifi status|scan [/b]|diag|connect|disconnect` with password masking,
  plus the persistent known-network commands `wifi known|save|forget|clear known|preferred`
  (all set ERRORLEVEL via the `esp_err_t` returned by `networking_handle_wifi_command()`).
  `wifi status` prints a multi-line colour-coded report (SSID, BSSID, channel, RSSI, PHY mode
  + bandwidth, IPv4, netmask, gateway, DNS, uptime); `wifi scan` returns results sorted by RSSI
  in aligned columns capped at `P4_CONFIG_WIFI_SCAN_LIMIT`, and `wifi scan /b` emits bare SSID
  lines that stay uncoloured so redirected output is machine-parsable.
- **Known Wi-Fi networks** (`wifi_known.c`): a persistent list of previously-used networks on
  the SD card (`sd:/WIFI.KNOWN`, plain text, hand-editable). The in-memory cache is loaded and
  saved through the guarded storage session API (`shell_sd_begin`/`shell_sd_end`) and tolerates
  failure at every step — no SD card, mount failure, missing/corrupt file, read-only or full
  disk all degrade to an empty list / non-OK return without crashing or blocking boot. The file
  is rewritten atomically (temp + rename) with the storage free-space pre-check and
  partial-file cleanup. On boot with `WIFI_AUTOCONNECT=ON`, the background task scans and
  connects to the best visible known network (preferred / highest priority / strongest RSSI),
  falling back to the classic single-credential path when the list is empty. Successful
  connections auto-update the list when `P4_CONFIG_WIFI_KNOWN_AUTOSAVE` is set. Passwords are
  stored in the file but never printed to the transcript, history, or debug log.
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
  layer to print or save to SD; the return value maps onto ERRORLEVEL (0 = HTTP 2xx). A
  `user:pass@` URL prefix enables HTTP Basic auth (`networking_http_url_has_userinfo()`).
  The same fetch core has a quiet (no-transcript) mode used by
  `networking_time_detect()`, which resolves the local UTC offset + IANA zone for
  the clock's automatic timezone detection (`P4_CONFIG_TIMEZONE_URL`).
- **HTTP file server (`httpd`)**: `components/networking/http_server.c` is the sole owner of
  the `esp_http_server` surface. It serves the SD card (`BSP_SD_MOUNT_POINT`) with HTML
  directory listings, file streaming through a heap read buffer, optional Basic auth
  (`P4_CONFIG_HTTPD_AUTH_*`, constant-time compare of the decoded `Authorization` header),
  and `..` path-traversal rejection. Lifecycle is tied to Wi-Fi events: the
  `networking_httpd_maybe_autostart()` hook fires on `IP_EVENT_STA_GOT_IP` and
  `networking_httpd_maybe_stop()` on `WIFI_EVENT_STA_DISCONNECTED`. Resource limits
  (`P4_CONFIG_HTTPD_*`) bound sockets, backlog, task stack, timeouts, block size, and the
  listing cap. Output goes through the networking host ops like every other module file.
- **Network diagnostics (`netstat` / `ipconfig`)**: `components/networking/netdiag.c` walks
  the lwIP `netif_list`, the DNS servers (`dns_getserver`), and the TCP/UDP PCB lists
  (`tcp_active_pcbs`, `tcp_tw_pcbs`, `tcp_listen_pcbs`, `udp_pcbs`) read-only under the
  TCP/IP core lock (`LOCK_TCPIP_CORE()` when `LWIP_TCPIP_CORE_LOCKING`, no-op otherwise),
  capped by `P4_CONFIG_NETSTAT_ROW_MAX`.
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

### Modal Runtime (components/modal)

Shared session loop and input-routing layer for every native modal surface
that takes over the shell display area. The editor was the first surface;
`dialog`, `list`, `ask`, `browse`/`filebrowser`, `view`, and `hexview` are
built on the same runtime (v0.35.0: 6 surfaces).

- **`modal.c`** owns the active surface pointer, the event group used for
  open/service/close handoffs, and the USB/serial input dispatch. Only one
  surface is active at a time; the shell core routes input through the generic
  `shell_command_ops_t.modal_*` hooks rather than surface-specific hooks.
- **`modal_surf.c`** provides ready-made batch-accessible surfaces:
  - `dialog` — one- or two-button message box (ERRORLEVEL 0/1/255).
  - `list` — scrollable selector returning a 0-based index (or the label via
    `/v:NAME`).
  - `ask` — text prompt with on-screen and USB keyboard support; stores the
    answer in `ASK_RESULT` (or `/v:NAME`), with `/p` password masking.
  - `browse` (`filebrowser`) — SD file picker (`BROWSE_RESULT`/`/v:NAME`, 0/1).
  - `view` — text viewer pager for SD files (20 lines/page); `.bmp`/`.dib`
    route to the image viewer through the `components/filetype/` registry.
  - `imageview` — fit-to-screen BMP viewer (`image show`/`view`/`open`): reuses
    the pure decoder in `components/gfx`, decodes straight to a fit-to-screen
    RGB565 `lv_canvas` (source never materialized), `Esc`/`q`/Close.
  - `hexview` — 16-byte hex dump pager for SD files.
  - All accept `/t:secs` to auto-cancel: each surface starts a FreeRTOS
    one-shot timer in `open` that fires `MODAL_EVENT_CLOSE_REQUEST`, so an
    unattended batch script can never hang on a dialog.
  - **Screen capture:** a modal blocks the command worker for its whole
    lifetime, so the bare `screenshot` command is handled by the console-reader
    task instead — `shell_command_ops_t.modal_console_command` (registered by
    `command_init()`) is tried before the surface's serial handler, and runs the
    streaming capture (`shell_command_screenshot`) on the reader task. This is
    what lets the host capture an open modal. Only the no-argument form is
    claimed; `screenshot <file>` still queues to the worker.
  - `surf_create_container()` scrolls the new panel into view because the
    surface aliases the scrollable transcript container; without it a transcript
    scrolled to its newest output would leave the panel off-screen above the
    viewport.

### TUI Module (`components/tui` + `windows` + `ansi` + font)

Hardware-verified on COM11 (v0.35.1, final TUI state: flash, boot `1024x510` transcript rect, extensive serial `draw`/`color`/`locate`/`dialog`/`list`/`ask`/`browse`/`view` with no abort/watchdog/overlap):

- **Cell buffer** (`components/tui/tui.h:35` `tui_cell_t { char utf8[4]; uint8_t fg/bg/attr; }`, `components/tui/tui.c:129` `tui_cell_set` via `strncpy`): logical `P4_CONFIG_TUI_COLS`×`P4_CONFIG_TUI_ROWS` (`80×25` `p4minishell_config.h:325`, DOS parity) heap cell buffer (PSRAM preferred, `heap_caps_malloc MALLOC_CAP_SPIRAM`) with fg/bg/attribute per cell. Clamped to the logical grid, never to pixels; each cell holds full UTF-8 (3-byte box glyphs). Previously `utf8[2]` truncated box draws — fixed to `utf8[4]`.
- **Drawing primitives** (`components/tui/tui.c:241` `tui_draw_box`, `components/tui/tui.c:296` `tui_draw_line`, `components/tui/tui.c:312` `tui_fill`, `components/tui/tui.c:201` `tui_print_at`): `tui_draw_box` honors `single`/`double`/`rounded` via `SH_BOX_*` UTF-8 (`SH_BOX_TL`/`TR`/`BL`/`BR`/`H`/`V` vs `SH_BOX_TL2`/`H2`/`V2` vs `SH_BOX_TLR`/`TRR`/`BLR`/`BRR`) and optional centered title; `tui_draw_line` honors `single`/`double`/`heavy`; interior cleared; all via `tui_cell_set`.
- **Flush / color** (`components/tui/tui.c:620` `tui_flush`): coalesces cells by fg, emits `#RRGGBB ` prefix per fg run using `ansi_get_palette_color` (`components/ansi/ansi.c`) PowerShell palette (no duplicate palette), wraps run UTF-8 and `#` suffix, rows joined by `\n`, set via `lv_label_set_text` on `s_tui_label` (`lv_label_set_recolor true`) under `lvgl_port_lock`. Default fg 16 emits no tag. `tui_set_default_color` (`components/tui/tui.c:324`) backs `color`; `tui_set_cursor` (`components/tui/tui.c:160`) backs `locate`.
- **Live region mapping**: TUI uses its own `s_tui_container`/`s_tui_label` inside `windows_enter_tui_mode()`/`windows_exit_tui_mode()` (`components/windows/windows.c`). The transcript container is an **app surface** (see "App surfaces" below): the window manager pins it to the surface origin, drops its padding, suppresses transcript follow-to-bottom and auto-hides the on-screen keyboard while the app owns it. `tui_apply_layout()` (`components/tui/tui.c`) maps the logical `80x25` grid onto the live transcript region by picking the largest committed cell font that fits (`components/tui/tui_fonts.c`, generated by `tools/gen_tui_font.py`: 12x24 for fullscreen, 12x20 windowed, plus 10x16/8x12/8x8 fallbacks), sizing the label to the exact grid and centring it; it re-runs on keyboard show/hide, fullscreen and rotation via the window manager's surface-layout callback. Header kept visible by default; `tui_enter_fullscreen`/`tui_exit_fullscreen` (`components/tui/tui.c`) call `windows_set_fullscreen`/`header_set_visible` to hide the header completely when fullscreen. `tui status` reports rect `1024x510`, cols/rows `80x25`, fullscreen, font.
- **Fullscreen**: global `draw fullscreen on|off` and per-app `tui fullscreen on|off` both route to `tui_enter_fullscreen`/`tui_exit_fullscreen`; `windows_is_fullscreen()` guards state. Keyboard scaling remains dynamic via `windows_notify_keyboard_visibility` even when header is hidden.
- **Prompt**: all inputs honor `shell_prompt_render_plain()` (`components/shell/shell.c:412`): `main.c:112` input line echo `SHELL_PROMPT` → `shell_prompt_render_plain()`, `modal_surf.c:412` `ask` placeholder + `keyboard_bind_textarea` (`components/keyboard/keyboard.c:88`), shell echo situational color (`SH_PROMPT`). `PROMPT=` template (`$p $g` etc) renders everywhere.
- **Font** (`managed_components/lvgl__lvgl/src/font/lv_font_unscii_16.c`, `sdkconfig.defaults:33` `CONFIG_LV_FONT_UNSCII_16=y`): extended `unscii_16` in-place with box-drawing U+2500-U+257F (128 glyphs) and symbols U+2600-U+26FF (256 glyphs), 384 glyphs total, cmaps 3, no duplication (previously the `-r` range duplicated the two blocks). `windows_get_terminal_font()` returns this font for the shell transcript. The TUI grid does **not** use it: it uses the generated cell fonts in `components/tui/tui_fonts.c` so the logical `80x25` grid scales to the live region (the terminal font is fixed at 16 px and cannot fit 80 columns).
- **Batch TUI verbs**: `draw box`/`line`/`fill`/`text`/`bar`/`table`/`list`/`image`/`clear`/`window`/`cursor`/`hold`/`alt-screen`/`fullscreen`, `color`, `locate` compose on the cell buffer; the exclusive `gfx` RGB565 canvas (`pixel`/`line`/`rect`/`circle`/`show`/`image`/`load`/`blit`/`save`) and `browse`/`view`/`image`/`hexview` round out the surfaces. `draw table`/`draw list` add cursor + selection rows. `browse`/`view`/`hexview` are native pagers on the shared modal runtime (`components/modal/modal_surf.c`). `draw` auto-enters TUI (`components/tui/tui.c:56` `tui_init` via `windows_enter_tui_mode`) when no TUI/modal surface is active. Alt-screen `ESC[?1049h/l` save/restore is honoured when `P4_CONFIG_TUI_ALT_SCREEN` is set (`components/tui/tui.c:327`).
- **Screenshot debug loop** (`grab_screenshot.py --port COM11 --out out.png --crop-transcript` + `capture_tui.py`): `tui status` shows transcript rect, `grab_screenshot.py` crops to it for pixel-perfect TUI verification (used during hardware bug hunting alongside `windows_debug_editor_layout`).
- **Essential features implemented**: window stack (nested `tui_draw_box` with title), fullscreen (global + per-app header hide), color (`tui_flush` per-fg recolor), prompt (unified `shell_prompt_render_plain`), screenshot debug; hardware tested without overlap (header kept unless fullscreen, TUI does not overlap shell text), no watchdog, no abort.
- Surfaces render into the dedicated TUI container and restore on `tui_deinit`/`windows_exit_tui_mode`.

#### App surfaces (foreground TUI + gfx viewport)

A foreground batch app (the TUI cell buffer or the `gfx` RGB565 canvas) owns the transcript region through the window-manager **app surface** API (`components/windows/windows.h`: `windows_enter_app_surface()` / `windows_exit_app_surface()` / `windows_get_app_viewport()` / `windows_set_surface_layout_cb()` / `windows_refresh_app_surface()`):

- **Placement**: the transcript container is scrollable and normally parked at the bottom of the scrollback, which draws any child above the viewport. Entering the app surface pins the container to the surface origin, drops its padding and suppresses transcript follow-to-bottom (`windows.c`) so the app is visible immediately (V9). Leaving it restores the shell surface.
- **Keyboard**: the on-screen keyboard is auto-hidden on entry and restored on exit if it was visible, so the app gets the full region; showing it again re-runs the surface layout (V9).
- **Scaling**: the surface registers a layout callback that re-maps it to `windows_get_app_viewport()` on every keyboard/fullscreen/rotation change. The TUI re-maps its `80x25` grid to the region (cell fonts, above); the `gfx` canvas is scaled with a uniform "contain" `lv_image_set_scale` and centred (V10).
- **Rotation**: the display rebuild path (`main.c` `shell_build_ui`) calls `command_close_foreground_surfaces()` after `shell_request_abort()`, so an open app is closed gracefully (with a shell errorlevel) instead of being destroyed underneath its owner; the UI then rebuilds at the new resolution.

### Editor Module (components/editor)

Implements the DOS-style `edit` command as a reusable modal surface on the
shared modal runtime (see SDK.md, "Modal app surfaces").

- **`editor.c`** owns the byte-preserving document model: heap lines with
  exact lengths, CRLF/LF EOL tracking, cursor/selection state, snapshot
  undo/redo (`P4_CONFIG_EDITOR_UNDO_DEPTH`), word navigation, delete
  line/EOL, pure find/replace helpers, and the guarded SD load/save
  (a failed save removes its partial destination). It also runs the modal
  session on the command worker: it loads the file, opens the view via
  `lv_async_call`, then services save requests and waits for quit — file I/O
  never runs on the LVGL task.
- **`editor_view.c`** owns the LVGL surface. It renders one row per document
  line into a dedicated span group (with batch syntax colours applied through
  `editor_lex_batch` and a right-aligned line-number gutter via the pure
  `editor_format_line_number` helper), draws a blinking block cursor, a
  current-line highlight bar, and a selection background overlay, and
  implements an inline status-bar prompt system for Find / Replace /
  Go-to-Line / Save-As / Open / quit-confirmation. The cursor, selection, and
  touch mapping are offset by the gutter width so the caret stays byte-aligned
  with the document. A per-row cumulative width table (built with
  `lv_font_get_glyph_width`) maps byte columns to pixels and back in O(log n),
  keeping the caret and touch mapping aligned with the rendered rows. The pure
  `editor_osk_key_from_label()` table is the single label→`editor_key_t` mapper
  shared by the touch handler and `test_editor.c`, and the view selects the OSK
  Nav page on open (`editor_view_open`) and the letters page for text prompts,
  restoring Nav on commit/cancel.
- **Layout integration**: `windows_enter_editor_mode()` aliases the transcript
  container as the editor surface and the input row becomes a status bar.
  The view hides the shell's own span group
  (`windows_get_transcript_spans()`) while open and restores it on close.
- **Input routing**: the editor is a surface on the shared modal runtime in
  `components/modal/`. USB keys and serial lines reach it through the generic
  `shell_command_ops_t.modal_is_active` / `modal_handle_usb_key` /
  `modal_handle_serial_line` hooks; serial lines are forwarded verbatim with the
  verbs `\q \s \f \g \o \open \u \r \all \c \b \co \w \l \p \a`. The
  keyboard's single `LV_EVENT_VALUE_CHANGED` handler in `main.c` owns mode
  switching (`abc`/`ABC`/`1#`/`Nav`) and routes every button to the shell
  input line or the active modal surface.
- **Safety**: an existing file that cannot be loaded is refused with an error
  (never opened as an empty buffer); Esc confirms before discarding unsaved
  changes; rotation during a session closes the view cleanly.
- **Large files / PSRAM**: the document and its caches (line text, line array,
  undo snapshots, width/wrap caches, span scratch, preview) go through
  `editor_mem_alloc/realloc/free` (PSRAM first). Rendering is windowed to
  `P4_CONFIG_EDITOR_RENDER_ROWS` rows with a full-height spacer holding the
  scroll range; SD load/save stream through a small internal `MALLOC_CAP_DMA`
  bounce buffer because PSRAM is not DMA-capable on this P4 build.
- **Tests**: `test/main/test_editor.c` exercises the document model and the
  batch lexer without hardware (the line-cap test builds its document with one
  multi-line insert so it stays linear).

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
  `windows`, plus `samples/whoami`), selects the board profile
  (`-DP4_BOARD=`, default `jc1060p470c`), and stages that profile's
  `board_config.h`
- `main/CMakeLists.txt`: Registers `main.c` and `p4minishell.c` with component dependencies
- `components/command/CMakeLists.txt`: Requires `batch` and `storage` in addition to `shell`
- `components/batch/CMakeLists.txt`: Requires `shell` and `storage`
- `components/storage/CMakeLists.txt`: Requires `shell`, `header`, the BSP, and `fatfs`
- `components/networking/Kconfig.projbuild`: Defines the `P4MINISHELL_WIFI_DEFAULT_*` options
  (lives with its only consumer so any project including the component gets the symbols)
- `test/CMakeLists.txt`: Standalone unit-test project. Pins `IDF_TARGET` to `esp32p4`, stages
  the active `boards/<name>/board_config.h` and `p4minishell_config.h` into the generated config directory the same way
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
- Upstream LVGL sample applications are not built (image budget)
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
prompt. Owned by components/boot (boot.c, boot.h), kept deliberately
small and free of private-state access.

- **Startup:** boot_run_startup() is called once from app_main after all
  modules (storage, batch, command, display, networking, usb) are initialized.
- **File handling:** looks for CONFIG.SYS and AUTOEXEC.BAT on the SD root.
  Missing files are generated once from built-in templates when
  P4_CONFIG_BOOT_GENERATE_DEFAULTS is set. Safe no-op with no SD card.
- **CONFIG.SYS parser:** line-oriented, case-insensitive, skips blank lines and
  REM/; comments. Classic directives: SET, PATH=, PROMPT=, ECHO ON|OFF.
  Modern: ROTATE=, BRIGHTNESS=, DISPLAY_POWER=, VOLUME=, WIFI_SSID=,
  WIFI_PASSWORD=, WIFI_AUTOCONNECT=, WIFI=ON|OFF, BLUETOOTH=ON|OFF,
  BT_ADVERTISE=ON|OFF, USB_KEYBOARD=ON|OFF, USB_MOUSE=ON|OFF,
  GPIO <n> = OUT [HIGH|LOW]. Unknown KEY=VALUE lines set a batch environment
  variable; unknown keywords without a value warn once and are skipped.
- **Application:** hardware directives execute their command-line equivalent
  through the batch pipeline (batch_boot_execute_command()), reusing existing
  validation. State-only directives use module accessors
  (networking_wifi_set_boot_credentials,
  networking_wifi_set_boot_autoconnect, batch_set_default_echo). Wi-Fi password
  is never echoed/logged. GPIO directives
  delegate to the existing gpio set safety check.
- **AUTOEXEC.BAT:** runs through the normal batch pipeline
  (shell_execute_batch_file()), cwd = SD root. Non-zero errorlevel is a warning.
- **Configurability:** all limits and names in p4minishell_config.h
  (P4_CONFIG_BOOT_*), documented in p4minishell_config.yaml under
  boot_scripting.

## Testing and frame metrics

- **Frame pacing.** The pure `gfx_frame_stats_t` core in `components/gfx`
  (`gfx_frame_stats_reset/sample/set_target_fps/avg_us/jitter_us/fps/format`)
  is the one timing implementation. Both present points sample it:
  `gfx show` (`components/command/gfx_commands.c`) and `tui_flush`
  (`components/tui/tui.c`). `gfx stats` / `tui stats` render the shared
  integer-only line parsed by `tools/p4test/perf.py`. There is no second
  timing path and no floating point in the firmware report (newlib-nano).
- **Host test framework.** `tools/p4test/` owns the serial session
  (`session.DeviceSession`), assertions (`asserts.Checklist`), streaming
  screenshots (`screenshot.capture` + a `Bmp` pixel model), SD transfers
  (`sdbridge`), performance helpers (`perf`), the `device.Device` facade, and
  the suite `runner`. Hardware suites live in `tools/suites/` and run with
  `tools/p4test_run.py`. The destructive reset/format paths drive the EXISTING
  firmware `config factory` and `format` commands; nothing is duplicated on the
  host.
- **Autonomous dogfooding.** `tools/dogfood.py` + `tools/p4test/agent.py` run a
  seedable policy that exercises the shell, files, apps, modals and animation
  surfaces, screenshots every action, and journals anomalies (panic, timeout,
  blue DSI-underrun frame, blank frame, static streak, heap decline) plus a
  Markdown report under `screenshots/dogfood/<run>/`.
