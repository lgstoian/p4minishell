# Hosted Module SDK Guide

This guide describes how `main/main.c` integrates the runtime modules in this workspace: `components/shell`, `components/storage`, `components/batch`, `components/command`, `components/display`, `components/windows`, `components/header`, `components/led`, `components/networking`, `components/usb`, and `components/c6ota`.

## Architecture
- `main/main.c` is the application entry point: boot sequencing, LVGL event callbacks, UI construction, and the c6ota/usb host bridges. It contains no command implementations and no shell state.
- `components/shell` owns the transcript and async buffer, command history, debug log, UART 
console, the input-line prompt contract, the interactive keypress queue, the DOS prompt template engine, the boot 
banner, the system info commands, and the read-only FreeRTOS task introspection (`ps` / `tasks` / `top` via
  `shell_command_ps()`).
- `components/storage` owns the guarded SD session, persistent mount tracking, path resolution, FATFS conversion, size formatting, DOS wildcard matching, the RAM-only current working directory, the output-redirection writer, every DOS file command, and the hidden `.trash` recycle bin (`trash.c`) that `del`/`rd /s` move entries into and `undelete`/`trash` manage.
- `components/batch` owns the batch engine (file execution, `:label`s, `goto`, `call :label`, 
`for` loops, the `|` pipe operator), the RAM-only environment variables and PATH, variable expansion, errorlevel, the
batch language commands, and the DOSKEY-style alias table (`alias` / `unalias`, prompt-only
expansion, SD persistence via `alias /save`).
- `components/command` owns the single dispatcher, the execution pipeline, output-redirection parsing, the worker task, the hardware commands, and the remaining system commands. The display/keyboard/windows UI query handlers live in `components/command/command_ui.c`.
- `components/ansi` owns ANSI/VT SGR escape sequence processing, 16-color palette, and format string builder.
- `components/display` owns all display hardware state: rotation, resolution, refresh rate, brightness, power management, and touch handle.
- `components/windows` owns the LVGL screen layout: named regions, dynamic scaling, rotation-aware layout, and consistent styling.
- `components/header` owns the fixed top-bar LVGL widgets for notifications plus Wi-Fi, battery, Bluetooth, USB, and SD status.
- `components/networking` owns hosted Wi-Fi runtime state, boot restore, diagnostics, OTA
  restore hooks, and the persistent known-network list (`wifi_known.c`, `sd:/WIFI.KNOWN`).
  The known-list module performs all SD I/O through the guarded storage session API
  (`shell_sd_begin` / `shell_sd_end`) and degrades to an empty list on any SD failure.
- `components/networking/bluetooth.c` owns the hosted NimBLE control path for Bluetooth commands.
- `components/usb` owns ESP-IDF USB Host Library bring-up, MSC VFS registration at `/usb0`, and HID keyboard or mouse debug echo.
- `components/c6ota` owns the ESP32-C6 OTA workflow and uses `components/networking` when it needs Wi-Fi readiness or restore behavior.

### Dependency direction

```
main  ->  command  ->  batch  ->  storage  ->  shell  ->  ansi, display, windows, header, keyboard, clock
```

No component may declare `main` as a requirement. When a lower layer needs something an upper
layer owns, extend the matching operations table rather than adding an include:

| Need | Table to extend | Declared in |
|------|-----------------|-------------|
| `components/shell` needs a command-owned service | `shell_command_ops_t` | `shell.h` |
| `components/batch` needs the command pipeline | `batch_command_ops_t` | `batch.h` |

See "Inverting an upward dependency" below.

## Required boot-time integration
1. Include `shell.h`, `command.h`, `ansi.h`, `display.h`, `windows.h`, `header.h`, `networking.h`, `bluetooth.h`, `usb.h`, and `c6ota.h` where those modules are orchestrated.
2. Call `display_init()` first to initialize the display hardware and LVGL port.
3. Call `shell_init()` to bring up the transcript, debug log, boot timestamp, and ANSI palette (it calls `ansi_init()` for you).
4. Call `command_init()`. It brings up `storage_init()` (current working directory, SD mount tracking) and `batch_init()` (environment table, `PATH=sd:/`, errorlevel), then registers `batch_command_ops_t` with the batch engine and `shell_command_ops_t` with the shell core. This must follow `shell_init()`. Do not call `storage_init()` or `batch_init()` yourself; both are idempotent but `command_init()` owns the ordering.
5. Register the UI rebuild callback with `display_register_ui_rebuild_callback(shell_build_ui)` so rotation changes trigger full UI rebuilds.
6. Call `shell_uart_console_start()` to bring up the serial console task.
7. Call `windows_init()` (from `shell_build_ui()`) to build the LVGL shell surface (header, transcript, input row, keyboard).
8. Call `c6ota_init()` once after the transcript path is ready, then register the OTA progress callback with `c6ota_register_progress_callback(...)`.
9. Build a single `networking_host_ops_t` callback table backed by the shell transcript and debug-history functions, then call `networking_init(&host_ops)`.
10. Call `usb_init()` after `networking_init(&host_ops)`, then `usb_register_keyboard_input_callback(...)`.
11. Call `time_init()` for timezone setup. Start SNTP once lwIP is up with
    `time_start_sntp()`, or let the user force it with `sntp sync`
    (`time_force_resync()`). The `date`/`time`/`timezone`/`sntp` commands live in
    the clock component and render through `clock_host_ops_t` — register it with
    `clock_register_host_ops()` from `command_init()`.
12. Start a periodic LVGL timer that calls `shell_header_status_refresh()`.

## Inverting an upward dependency
`components/shell` must never include `command.h`, and `components/batch` must never include
`command.h`. To expose an upper-layer service downward:

1. Add the function pointer to the matching ops struct (`shell_command_ops_t` in
   `components/shell/shell.h`, or `batch_command_ops_t` in `components/batch/batch.h`).
2. Implement the function in the owning module and add it to the static table inside
   `command_init()`. The implementation does not have to live in `command.c` — the `get_cwd`
   and `sd_is_mounted` hooks point straight at `components/storage/`.
3. In the consuming module, call it through the stored table and NULL-check first so the
   module still works before `command_init()` runs.

```c
/* components/command/command.c */
void command_init(void)
{
    static const shell_command_ops_t shell_ops = {
        .execute_command    = shell_execute_command,
        .get_cwd            = shell_get_cwd,            /* components/storage */
        .get_volume_percent = command_get_volume_percent,
        .sd_is_mounted      = storage_sd_is_mounted,    /* components/storage */
        .battery_read       = command_battery_read,
    };
    static const batch_command_ops_t batch_ops = {
        .execute_command = shell_execute_command,
    };

    if (s_initialized) {
        return;
    }

    storage_init();
    batch_init();

    batch_register_command_ops(&batch_ops);
    shell_register_command_ops(&shell_ops);

    s_initialized = true;
}
```

```c
/* components/batch/batch.c — every nested line goes through the hook */
static void batch_run_nested(char *command)
{
    if (command == NULL) {
        return;
    }

    if (s_command_ops.execute_command == NULL) {
        shell_transcript_append_text("batch: command pipeline is not available yet\n");
        shell_record_warningf("batch", "Nested execution attempted before command_init()");
        return;
    }

    s_command_ops.execute_command(command);
}
```

## Networking ownership

Every `esp_hosted_*`, `esp_wifi_*`, `esp_netif_*`, NimBLE, and HTTP call belongs in
`components/networking/`. The one sanctioned exception is `components/c6ota/`, which
drives `esp_hosted_slave_ota_*` because co-processor firmware update is its purpose.

Two more modules extend the same ownership boundary:
- `components/networking/http_server.c` owns the `esp_http_server` surface (the `httpd`
  SD file server). It serves files through guarded storage sessions
  (`shell_sd_begin`/`shell_sd_end`) and VFS `opendir`/`fopen`/`fread`, and its lifecycle
  is driven by the Wi-Fi event hooks `networking_httpd_maybe_autostart()` /
  `networking_httpd_maybe_stop()`.
- `components/networking/netdiag.c` owns the lwIP diagnostics (`netstat` / `ipconfig`).
  It walks `netif_list`, the DNS servers, and the TCP/UDP PCB lists read-only under
  `LOCK_TCPIP_CORE()` when core locking is enabled. Never iterate or modify PCBs from
  another context without the lock.
- `components/led/led.c` owns the WS2812 RGB status LED on GPIO26 (espressif/led_strip over
  RMT) and the animation task. `networking` pushes Wi-Fi/HTTP events with `led_notify()`;
  the `rgb` command (in `components/command`) and the CONFIG.SYS `RGB=` directive drive it.
  It is a leaf, so `command`, `networking`, and `main` can all depend on it.

When another layer needs networking state, add an accessor rather than reaching into the
driver:

```c
/* Correct: the module owns the driver call and the disconnected fast path. */
int rssi = P4_CONFIG_HEADER_RSSI_UNKNOWN;
(void)networking_wifi_get_rssi(&rssi);

/* Wrong: an esp_wifi_* call outside components/networking/. */
wifi_ap_record_t ap;
if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) { rssi = ap.rssi; }
```

Only the official Espressif path is permitted: `espressif/esp_hosted` for transport and
`espressif/esp_wifi_remote` for the Wi-Fi API. Do not add custom RPC, an alternative
transport, or a re-implemented control plane.

The initialization order inside `networking_wifi_start_runtime()` is load-bearing —
hosted transport, then slave connect, then the version gate, then NVS, then netif and
the event loop, then `esp_wifi_init()`, then STA mode and start. Transport before NVS
matters so a dead or mismatched C6 reports as a transport fault rather than surfacing
later as a confusing Wi-Fi init error.

## Shared host callback model
`networking_host_ops_t` is the common host-to-module bridge for Wi-Fi and Bluetooth:

```c
static const networking_host_ops_t host_ops = {
    .transcript_append_text = shell_transcript_append_text,
    .schedule_transcript_append_text = shell_networking_schedule_text,
    .record_error = shell_networking_record_error,
    .record_warning = shell_networking_record_warning,
    .record_info = shell_networking_record_info,
};
```

- `networking_init(...)` stores this table.
- `networking_init(...)` also calls `bluetooth_init(&s_host_ops)`, so Bluetooth uses the same transcript and debug hooks automatically.
- `c6ota` uses dedicated host bridge functions plus a progress callback registration step, rather than consuming `networking_host_ops_t` directly.
- `components/usb` also uses dedicated host bridge functions, mirroring the transcript and debug behavior already used by `c6ota`.
- The `c6ota_host_*` and `usb_host_*` bridge functions are implemented in `main/main.c`, and the `shell_networking_*` adapters in `main/p4minishell.c`. They live in the app component because ESP-IDF resolves those `extern` symbols from there; they are thin one-line adapters onto the `components/shell` API and contain no logic.
- `components/header` is display-only and is updated through its public `header_update_*` calls rather than consuming the transcript callback surface directly.
- Wi-Fi also uses the `notify_header` host callback for immediate header notices on key connection lifecycle events, while USB and `c6ota` use their dedicated host bridge functions for the same purpose.

## Command dispatch integration
All of this lives in `components/command/command.c`; the list documents the contract rather than
work the integrator has to perform.

1. `wifi ...` commands route to `networking_handle_wifi_command(...)`.
2. `bluetooth ...` / `bt ...` commands route to `bluetooth_handle_command(...)`.
3. `usb ...` commands route to `usb_handle_command(...)`.
4. `c6ota ...` requests route to `c6ota_perform(source)`.
5. Family handlers receive the original unsplit command text, because they re-tokenize it themselves.
6. Before any command lookup, raw input passes through `c6ota_try_handle_input(...)` so pending `YES` or `NO` responses stay in the OTA confirmation path.
7. `shell_command_should_store_history()` calls `c6ota_is_confirmation_pending()` to keep confirmation replies out of command history.
8. `c6ota_is_busy()` is used by `sysinfo` to report OTA state.

### Adding a new command
1. Pick the owning module by domain:

   | Command kind | File | Visibility |
   |--------------|------|------------|
   | Filesystem / SD | `components/storage/storage_commands.c` | declare in `storage_commands.h` |
   | Batch language | `components/batch/batch.c` | declare in `batch.h` |
   | System info | `components/shell/shell.c` | declare in `shell.h` |
   | Hardware, UI query, other system | `components/command/command.c` | keep `static` |

2. Implement the handler there. Handlers outside `command.c` must be non-`static` and declared
   in that module's public header so the dispatcher can call them; handlers inside `command.c`
   stay `static` with a forward declaration in the block at the top of the file.
3. Add the dispatch arm to `shell_execute_command_core()` in `components/command/command.c`.
   That is the only dispatcher — never add a second one.
4. If the new module needs a component it does not already require, add it to that component's
   `CMakeLists.txt` `REQUIRES`, and add the component directory to `test/CMakeLists.txt`.
5. Add the usage line to `shell_command_help()` in `components/shell/shell.c`.
6. Document it in `command.md`, including the "Where Commands Live" table.
7. Put any new literal (buffer size, limit, delay) in `p4minishell_config.h` as a `P4_CONFIG_*`
   macro and document it in `p4minishell_config.yaml`.

Nested execution contexts (`if`, `for`, pipes, batch lines) must re-enter the full pipeline, not
`shell_execute_command_core()`, so the nested line still gets variable expansion and output
redirection. Inside `components/batch/` that means calling through
`batch_command_ops_t.execute_command`; elsewhere it means calling `shell_execute_command()`.

Any new command that touches the SD card must open a guarded session with `shell_sd_begin()` and
close it with `shell_sd_end()` on every return path, and must resolve user-supplied paths through
`shell_fs_resolve_path()` rather than passing the raw argument to `fopen()`.

### Adding a command that writes files

Writes need three guardrails. Skipping any of them risks data loss:

```c
/* 1. Refuse a self-copy. Opening the destination with "wb" truncates it,
 *    so without this check `copy a.txt a.txt` destroys the source. */
if (storage_paths_are_same(source_path, dest_path)) {
    shell_transcript_append_text("mycmd: source and destination are the same file\n");
    return;
}

/* 2. Precheck capacity BEFORE opening the destination. The second argument
 *    credits the space a truncated destination gives back. */
if (!storage_check_free_space(source_bytes, storage_get_file_size(dest_path), "mycmd")) {
    return;   /* The refusal message is already printed. */
}

/* 3. On a write failure, remove the partial destination. A truncated file
 *    that looks complete is worse than no file at all. */
if (fwrite(...) != expected) {
    fclose(dest);
    (void)unlink(dest_path);
    shell_transcript_appendf("mycmd: write failed, removed the partial %s\n", dest_path);
    return;
}

/* 4. If this is a copy, carry the attributes AFTER closing the destination.
 *    Doing it earlier fails, because a read-only file cannot be written. */
fclose(dest);
(void)storage_copy_attributes(source_path, dest_path);
```

Prefer `shell_fs_copy_file()` over rolling your own loop: it already does all four steps.

### Adding a destructive volume command

Anything that can lose user data must be unattended-proof:

```c
/* Shared helper in storage_commands.c used by format, disk clean/delete,
 * recursive del/rd, and trash empty/purge. Collects the exact confirmation
 * word through the key queue and refuses when nobody can answer. */
if (!shell_confirm_destructive("mycmd", "WARNING: ...", detail_lines)) {
    return;   /* return 1: cancelled counts as a non-zero ERRORLEVEL */
}

shell_transcript_appendf("Type %s to continue: ", P4_CONFIG_DESTRUCTIVE_CONFIRM_WORD);
/* Collect the word through the key queue so the reply never reaches the
 * command dispatcher. See shell_confirm_destructive() for the loop. */
```

Over serial each key of the confirmation word must be sent on its own line (the
UART console submits one key per line to the key queue); the on-screen keyboard
types the word naturally. Serial commands now run on the command worker task,
so the UART console stays free to read the reply while a key wait is active.

Integrity tools must stay read-only. This firmware does not rewrite FAT structures: report a
problem and let the user repair the card on a host, rather than risking an in-place "fix" that
makes the damage worse.

### Adding a recursive directory walker

Recursive walkers multiply their stack frame by `P4_CONFIG_DIR_RECURSE_DEPTH_MAX` on the shared
8 KB worker stack. A `FF_DIR`, a `FILINFO`, and a path buffer together exceed 700 bytes, so
declaring them as locals overflows the stack before the depth limit is reached. Put the whole
per-level state in one heap block and release it before descending:

```c
struct level_scratch {
    char    fatfs_path[P4_CONFIG_SD_PATH_BYTES];
    FF_DIR  dir;
    FILINFO info;
} *scratch = calloc(1, sizeof(*scratch));

/* ... read this level, collect the subdirectory names ... */

free(scratch);          /* Release BEFORE recursing. */
scratch = NULL;

for (i = 0; i < subdir_count; i++) {
    my_walk(child_path, depth + 1, ctx);
}
```

`shell_dir_list_one()`, `shell_tree_walk()`, and `shell_chkdsk_walk()` all follow this shape.

The `xcopy` walker (`shell_xcopy_walk`) and the `findstr /S` walker
(`shell_findstr_walk`) follow the same rule; `xcopy` must never re-enter the
`xcopy` command for subdirectories — that is how the old recursive copy
stacked frames.

### Pure, unit-testable text logic

The findstr regex engine and the comp byte-comparison core have no I/O and
are declared in `storage_commands.h` so `test/` can exercise them directly:
`shell_fsre_search()`, `shell_findstr_match_line()`, and
`shell_comp_first_diff()`. When adding a text filter, keep the matcher pure
and expose it the same way instead of burying the logic behind the transcript.

### Colouring command output

Never choose a colour at the call site. Use the semantic helpers for the common shapes,
and the `SH_*` palette macros for composite lines:

```c
#include "ansi_palette.h"

/* Common shapes: the helper picks the colour and adds the reset and newline. */
shell_print_heading("System Information");
shell_print_field("SSID:", "%s", ssid);
shell_print_field_num("Channel:", channel);
shell_print_ok("connected");
shell_print_error("wifi: connect failed (%s)", esp_err_to_name(err));
shell_print_warning("wifi: weak signal");
shell_print_usage("Usage: wifi connect <ssid> <password>");

/* Composite lines mixing several categories. */
shell_transcript_appendf_ansi(SH_LBL "battery:" SH_RST " " SH_NUM "%d%%" SH_RST "\n", pct);

/* Wrong: hand-picked specifier, and @k is nearly invisible. */
shell_transcript_appendf_ansi("@kbattery: %d%%@R\n", pct);
```

Two rules that are easy to get wrong:

- **Apply colour after width formatting.** Escape bytes count toward `strlen()`, so
  colouring a cell before padding it silently breaks column alignment. Format the text to
  width first, then wrap the finished string.
- **Leave machine-readable output uncoloured.** `dir /b` and anything destined for a
  redirect or a pipe stage must stay plain so the next command can parse it.

Adding a new category means adding a macro to `components/ansi/ansi_palette.h`, not
inlining a specifier.

### Finding operators in a command line

Never hand-roll a quote check. Five surfaces already agree on what counts as syntax, and a
local scan will miss single quotes and caret escapes:

```c
/* Correct: honors "..." , '...' , and ^c */
if (shell_has_unquoted_char(line, '|')) { ... }

char *op = shell_find_unquoted_any(line, "><");

/* Wrong: misses 'a | b' and a^|b */
if (strchr(line, '|') != NULL) { ... }
```

Strip markup with `shell_unescape_in_place()` once an extent is known, never by hand.
`shell_split_args()` already does this for every argument it produces.

### Stack budget on the dispatch path

`shell_execute_batch_file()`, `shell_execute_command()`, and `shell_execute_command_core()`
form a recursive cycle: a batch file re-enters the pipeline once per line, and nesting
multiplies every frame by `P4_CONFIG_BATCH_DEPTH_MAX`. The whole cycle shares the
`P4_CONFIG_COMMAND_TASK_STACK` (8 KB) worker task stack.

Do not add a line-sized or larger local buffer to any function on that cycle:

```c
/* Wrong on the recursion path: 768 bytes x 4 nesting levels */
char work[P4_CONFIG_BATCH_LINE_BYTES * 2];

/* Correct: heap-allocated, freed on every exit path */
char *work = malloc(P4_CONFIG_BATCH_LINE_BYTES * 2);
if (work == NULL) {
    shell_record_errorf("shell", ESP_ERR_NO_MEM, "Out of memory");
    return false;
}
...
free(work);
```

The same applies to structures stored per batch frame: a field sized at the full command
width multiplies by the slot count. Sizing 32 label slots at 256 bytes each produced an 8 KB
table and a stack-overflow crash, fixed in v0.19.0.

After changing anything on that path, confirm the frame size from the disassembly rather than
by inspection:

```sh
riscv32-esp-elf-objdump -d build/esp-idf/batch/CMakeFiles/__idf_batch.dir/batch.c.obj
# read the "addi sp,sp,-N" in the function prologue
```

### Adding a command that reads piped or redirected input

Text-processing commands must accept their input from three places interchangeably: an explicit
filename, a `<` redirection, and a `|` pipe stage. Resolve all three with one call:

```c
char resolved[P4_CONFIG_SD_PATH_BYTES];
esp_err_t error = storage_resolve_input_source(argc >= 2 ? argv[1] : NULL,
                                               resolved, sizeof(resolved));
if (error == ESP_ERR_NOT_FOUND) {
    shell_transcript_append_text("mycmd: no input file given and no input redirection active\n");
    return;
}
```

Never add a per-command input argument or read the slot directly; the shared helper is what
keeps `mycmd f.txt`, `mycmd < f.txt`, and `type f.txt | mycmd` on one code path.

### Adding a command that waits for a keypress

Check for an interactive source first so a headless board falls back instead of stalling, then
bracket the wait so input routing is always restored:

```c
if (!shell_key_input_available()) {
    /* Bounded fallback: no UART console and no USB keyboard is attached. */
    vTaskDelay(pdMS_TO_TICKS(P4_CONFIG_PAUSE_DELAY_MS));
    return;
}

shell_key_wait_begin();
{
    char key = '\0';

    if (!shell_wait_for_key(P4_CONFIG_KEY_WAIT_TIMEOUT_MS, &key)) {
        shell_transcript_append_text("mycmd: timed out waiting for a key\n");
    }
}
shell_key_wait_end();   /* Required on every return path. */
```

Never call `shell_wait_for_key()` with an unbounded timeout, and never leave a wait open: while
one is active every input source stops accepting commands.

## Example boot integration
```c
static void shell_header_status_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    /* The shell core batches every header field into one async render,
     * and also drives USB keyboard auto-detect and SD notifications. */
    shell_header_status_refresh();
}

static void shell_c6ota_progress(int percent, const char *msg)
{
    if (msg == NULL || msg[0] == '\0') {
        return;
    }
    /* percent == -1 marks a synchronous message that must land immediately;
     * everything else arrives from the OTA worker task and is staged. */
    if (percent == -1) {
        shell_transcript_append_text(msg);
    } else {
        shell_schedule_transcript_appendf("%s", msg);
    }
}

void app_main(void)
{
    /* 1. Display manager must be initialized first */
    if (display_init() != ESP_OK) {
        return;
    }

    /* 2. Shell core, then the command module (which registers its ops table) */
    shell_init();
    command_init();

    /* 3. Rotation rebuild hook and the serial console */
    display_register_ui_rebuild_callback(shell_rebuild_ui_callback);
    shell_uart_console_start();

    /* 4. Window manager builds the LVGL shell surface */
    bsp_display_lock(0);
    shell_build_ui();          /* calls windows_deinit() + windows_init() */
    bsp_display_unlock();

    /* 5. Modules */
    c6ota_init();
    c6ota_register_progress_callback(shell_c6ota_progress);

    networking_init(&(networking_host_ops_t){
        .transcript_append_text = shell_transcript_append_text,
        .schedule_transcript_append_text = shell_networking_schedule_text,
        .transcript_append_ansi = shell_transcript_append_ansi,
        .record_error = shell_networking_record_error,
        .record_warning = shell_networking_record_warning,
        .record_info = shell_networking_record_info,
        .notify_header = shell_header_notify,
    });

    usb_init();
    usb_register_keyboard_input_callback(shell_usb_keyboard_cb);

    time_init();

    /* 6. Periodic header refresh */
    bsp_display_lock(0);
    shell_header_status_refresh();
    lv_timer_create(shell_header_status_timer_cb, P4_CONFIG_HEADER_REFRESH_PERIOD_MS, NULL);
    bsp_display_unlock();
}
```

## Example input line integration
The LVGL input line always renders the shell prompt as a literal prefix. Use the
`components/shell` helpers rather than manipulating the textarea text directly.

```c
static void shell_input_line_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);

    if (code == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t *il = windows_get_input_line();
        const char *text = il != NULL ? lv_textarea_get_text(il) : NULL;
        /* No-op while the prompt prefix is intact. */
        shell_input_line_repair_prompt(text);
        return;
    }

    if (code == LV_EVENT_READY) {
        char command[P4_CONFIG_COMMAND_BYTES];
        char transcript_command[P4_CONFIG_COMMAND_BYTES];

        shell_extract_input_text(command, sizeof(command));
        shell_format_command_for_transcript(command, transcript_command, sizeof(transcript_command));
        shell_transcript_appendf("%s%s\n", P4_CONFIG_SHELL_PROMPT, transcript_command);

        if (shell_command_should_store_history(command)) {
            shell_store_command_history(command);
        }
        shell_reset_history_cursor();
        shell_input_line_reset();

        /* Async so heavy commands never run on the LVGL callback stack. */
        shell_execute_command_async(command);

        /* Jump to the output of the submitted command even when the user was
         * reading earlier history. Use shell_history_transcript_scroll_to_end()
         * instead for background/async output, which only follows when the view
         * is already near the bottom. */
        shell_force_transcript_scroll_to_end();
    }
}
```

## ANSI/VT color integration notes
- The ANSI module (`components/ansi/`) is initialized automatically by `shell_init()`.
- Use `shell_transcript_appendf_ansi()` for colored command output with `@`-prefixed format specifiers.
- Color scheme: `@G` (bright green) for headers, `@C` (cyan) for field labels, `@g` (green) for success, `@r` (red) for errors, `@y` (yellow) for warnings.
- The ANSI palette is configurable via `P4_CONFIG_ANSI_*` macros in `p4minishell_config.h`.
- LVGL transcript is a span group (`lv_spangroup`) that renders the ANSI colours as per-span
  text colours; UART console receives the raw ANSI for native terminal rendering.
- For new commands, always use `shell_transcript_appendf_ansi()` with appropriate color specifiers.
- Never hardcode ANSI escape sequences in command output — use the `@`-prefixed format specifiers.
- The `@R` specifier resets all attributes at the end of each output line.
- Available format specifiers: `@R` (reset), `@B` (bold), `@D` (dim), `@I` (italic), `@U` (underline), `@k`-`@w` (standard FG colors), `@K`-`@W` (bright FG colors).

## USB keyboard integration notes
- USB keyboard auto-detection runs in the periodic header status refresh timer.
- When a USB HID keyboard is attached, `keyboard_set_external_input(true)` hides the on-screen keyboard.
- When detached, `keyboard_set_external_input(false)` restores the on-screen keyboard.
- USB keystrokes are routed to the shell CLI via `shell_usb_keyboard_input()` bridge.
- The bridge uses `lv_async_call` to safely manipulate the input line from the LVGL task context.
- Full US keyboard layout is supported: letters, numbers, symbols, keypad, navigation, function keys.
- Modifier keys (Shift, Ctrl, Alt, GUI) are tracked for proper shifted character mapping.
- Users can force the on-screen keyboard visible via `keyboard_force_visible()` / `keyboard_clear_force_visible()`.
- `usb_key_to_ascii_full()` and `usb_key_name_full()` are available for external key mapping.

## Window manager integration notes
- The window manager (`components/windows/`) OWNS all LVGL screen-level widgets.
- `windows_init()` must be called after `display_init()` and from the LVGL task context.
- `windows_deinit()` must be called before rebuilding the UI after rotation.
- All window objects are accessed via `windows_get_*()` accessors — never stored as static variables.
- Window region dimensions are computed dynamically from display resolution via `windows_scale_height_percent()` and `windows_scale_width_percent()`.
- All styling uses semantic color names via `windows_get_color()`.
- The window manager delegates header creation to `header_init()`/`header_deinit()`.
- The window manager queries display resolution from `display_get_width()`/`display_get_height()`.

## Display manager integration notes
- The display manager (`components/display/`) OWNS all display hardware state.
- `display_init()` must be called first, before any LVGL UI construction.
- `display_register_ui_rebuild_callback()` must be called after `display_init()` so rotation changes trigger full UI rebuilds.
- All display operations (brightness, rotation, power, info) go through the display manager's public API.
- The display manager handles touch handle acquisition and rotation remapping internally.
- `display_get_lvgl_handle()` is available if direct LVGL access is needed (e.g., for `lv_display_get_vertical_resolution()`).
- `display_get_info()` provides comprehensive diagnostics for `sysinfo` output.
- Thread safety: all display manager state is protected by critical sections; safe to call from any task context.
- Dynamic refresh rate change is noted as not supported on the current JD9165 panel (fixed 80 MHz pixel clock).
- The display manager does NOT own LVGL widgets or UI layout — that remains the shell's responsibility.

## Header integration notes
- The header is passive and display-only. It must not own Wi-Fi, Bluetooth, USB, SD, or battery runtime behavior.
- All `header_update_*()` functions are safe to call from any task context.
- State is set immediately (atomic bool/int writes); render is scheduled via LVGL async dispatch.
- If async dispatch fails, a synchronous `header_render()` fallback ensures the widget updates.
- Poll Wi-Fi RSSI through `esp_wifi_sta_get_ap_info()`, battery through shell ADC helper.
- `header_set_notification(...)` is async-safe (uses LVGL async dispatch internally).
- Header is non-scrollable, resolution-scaled, left-to-right status icons, notification on far right.
- SD indicator shows persistent state (NO/INS/ON/ERR) with consistent styling.
- Battery is ALWAYS visible — shows "BAT N/C" with muted styling when ADC is not connected.
- Memory (MEM), CPU (CPU bar + %), and Battery (BAT bar + %) are in the system panel on the far right.
- All system panel values (MEM, CPU, BAT) are dynamically linked to FreeRTOS runtime statistics.
- `header_update_battery(int percent, bool adc_ready)` — pass adc_ready=false to show N/C state.
- `header_update_mem(uint32_t free_heap, uint32_t total_heap)` — real-time heap from FreeRTOS.
- `header_update_cpu(int percent, uint32_t task_count)` — real-time CPU from runtime stats.
- `header_update_uptime(uint32_t seconds)` — system uptime in seconds.
- CPU usage is calculated from FreeRTOS idle task runtime counter deltas (CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS).
- Falls back to heap-ratio approximation when runtime stats are unavailable.

## Example command dispatch
This is the shape of `shell_execute_command_core()` in `components/command/command.c`.
Note that the module-routed handlers receive `command` (the original unsplit line), not the
tokenized `argv`, because each of them re-parses its own subcommand grammar.

```c
bool shell_execute_command_core(char *command)
{
    char *argv[P4_CONFIG_COMMAND_ARGV_MAX];
    char *trimmed = shell_trim(command);
    int argc;

    /* A pending OTA confirmation swallows the line before any command lookup,
     * so a stray "YES" is never dispatched as a shell command. */
    if (c6ota_try_handle_input(trimmed)) {
        return true;
    }

    if (strchr(trimmed, '|') != NULL) {
        shell_execute_pipe(trimmed);
        return true;
    }

    argc = shell_split_args(trimmed, argv, P4_CONFIG_COMMAND_ARGV_MAX);
    if (argc == 0) {
        return false;
    }

    if (shell_text_equals_ignore_case(argv[0], "wifi")) {
        networking_handle_wifi_command(command);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "bluetooth") ||
        shell_text_equals_ignore_case(argv[0], "bt")) {
        bluetooth_handle_command(command);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "usb")) {
        usb_handle_command(command);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "c6ota")) {
        c6ota_perform(argc >= 2 ? argv[1] : NULL);
        return true;
    }

    /* ... remaining built-ins ... */

    /* Fall through to batch file execution, then report unknown. */
    return false;
}
```

## OTA-specific runtime flow
1. Validate the requested source and queue the exact YES confirmation prompt.
2. For HTTP or HTTPS, wait for Wi-Fi readiness through `networking_wifi_wait_for_ota()` and download the full image first.
3. Validate the ESP-IDF app header and ESP32-C6 chip ID.
4. Capture the current Wi-Fi restore state through `networking_wifi_capture_restore_state(...)`.
5. Stop Wi-Fi completely for OTA, keep the hosted transport alive, and switch into Wi-Fi-off OTA mode.
6. Transfer the image over `esp_hosted_slave_ota_begin/write/end/activate` with the existing fixed chunk size and progress formatting.
7. Request post-OTA Wi-Fi restore on success, or restore Wi-Fi immediately after failures.

## Error reporting expectations
- `networking`, `bluetooth`, and `c6ota` are shell-oriented modules. They report meaningful state through transcript text and debug-history hooks rather than through a large return-value API.
- `usb` follows the same shell-oriented rule: MSC mount or listing and HID enable or disable state stay transcript-visible instead of introducing a separate structured shell protocol.
- OTA failures may include ESP-IDF names such as `ESP_ERR_INVALID_ARG`, `ESP_ERR_INVALID_STATE`, `ESP_ERR_INVALID_SIZE`, `ESP_ERR_NOT_FOUND`, `ESP_ERR_NOT_SUPPORTED`, `ESP_ERR_NO_MEM`, and `ESP_FAIL`.
- Caller code should not rewrite module-owned shell messages if behavior compatibility matters.

## Compatibility rules
- Keep the ESP-Hosted dependency aligned with the current project baseline in `main/idf_component.yml`, and keep `test/main/idf_component.yml` pinned to the same versions.
- Preserve the existing shell-visible command surfaces for `wifi`, `bluetooth`, `usb`, and `c6ota`.
- Preserve the exact OTA confirmation, progress, success, and failure strings.
- Do not reintroduce pre-OTA `esp_hosted_deinit()` on this esp32p4 baseline.
- Keep `c6ota default` aligned with the long-filename-safe SD lookup for `esp32c6_hosted_slave.bin` and `network_adapter.bin`.
- Never add `main` to a component's `REQUIRES` list; that inverts the layering. Use a registered operations table instead.
- Never add `command` to the `REQUIRES` list of `shell`, `storage`, or `batch`, and never add `batch` to `storage`. Those are the same inversion one layer down; use `shell_command_ops_t` or `batch_command_ops_t`.
- Never add bridge trampolines that forward a call from one layer to an implementation in another. Move the implementation to the owning module.
- When a component gains a new dependency, add its directory to `EXTRA_COMPONENT_DIRS` in `test/CMakeLists.txt` so the unit-test project keeps building. A brand-new component must also be added to the root `CMakeLists.txt`.
- Keep `shell_execute_command_core()` the only dispatcher. New commands add a dispatch arm there and put the implementation in the module that owns the domain.