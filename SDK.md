# Hosted Module SDK Guide (v0.35.7 hardware bring-up: suite 193/0/2, watchdog + alarm + font fixed)

> **v0.35.7 hardware bring-up (0.35.6->0.35.7) on top of the v0.35.6 split patch:** flashed to COM11, boot verified (`P4MiniShell v0.35.1 ready`, 1024x510 80x25 via `tui status` 80x25 p4minishell_config.h:298), extensive serial tests (draw box single SH_BOX_TL/H/V double SH_BOX_TL2/H2/V2 rounded SH_BOX_TLR/TRR/BLR/BRR with title+style correctly handled tui_draw_box components/tui/tui.c:228 / tui_cell_set components/tui/tui.c:116 utf8[4] components/tui/tui.h:35, draw line/fill/text/clear/window/close/refresh/fullscreen, draw fullscreen on|off (global) + tui fullscreen on|off (per-app) header kept visible by default windows_enter_tui_mode hidden only on fullscreen windows_set_fullscreen/header_set_visible components/windows/windows.c:418 / tui_enter_fullscreen components/tui/tui.c:417 dynamic windows_notify_keyboard_visibility -> windows_refresh_tui_surface, TUI does not overlap shell text tui_hide_for_modal, tui_flush components/tui/tui.c:356 recolor #RRGGBB per fg run via ansi_get_palette_color PowerShell palette no duplicate, color/locate TUI-aware, prompt shell_prompt_render_plain() main.c:112/components/shell/shell.c:412 + modal_surf.c:412 keyboard_bind_textarea situational SH_PROMPT, screenshot grab_screenshot.py --port/--out/--crop-transcript + capture_tui.py rect 1024x510) without abort/watchdog/overlap. Font extended in-place managed_components/lvgl__lvgl/src/font/lv_font_unscii_16.c 384 glyphs U+2500-U+257F/U+2600-U+26FF cmaps 3 no duplication CONFIG_LV_FONT_UNSCII_16=y; tui_cell_t utf8[4] SH_BOX_* via tui_cell_set, tui_flush recolor #RRGGBB per fg run ansi_get_palette_color; draw auto-enters TUI tui_init components/tui/tui.c:56; memory P4_CONFIG_TRANSCRIPT_BYTES 1024 p4minishell_config.h:93 P4_CONFIG_ASYNC_TRANSCRIPT_BYTES 512 p4minishell_config.h:134 P4_CONFIG_SD_DMA_BUFFER_BYTES 4096 p4minishell_config.h:626 P4_CONFIG_TRANSCRIPT_INTERNAL_TRIM_BYTES 60000 1/4 keep trim-below-10KB P4_CONFIG_COMMAND_TASK_STACK 16384->24576 p4minishell_config.h:1514 at 0x4012b75a, audio bsp_audio_init components/audio/audio.c:42 managed_components/espressif__esp_codec_dev/i2s/esp_codec_dev.c:269, modal EventGroup PSRAM MALLOC_CAP_SPIRAM components/modal/modal.c:46, queue full handling improved, serial routing modal_handle_serial_line components/modal/modal_surf.c:412 (dialog y list 2 ask myname correctly routed via shell.c). Companion fully TUI-expanded and hardware-verified: 7 BATs (COMPANION.BAT draw fullscreen double, SYS.BAT tui fullscreen draw boxes, FILES.BAT browse/view/hexview + draw + tui fullscreen, NET.BAT draw boxes, FUN.BAT tui demo, SET.BAT tui demo, LIB.BAT tui helpers) pushed via push_sd.py COM11 PASS (LIB 1896, COMPANION 1552, SYS 1486, FILES 3946, NET 2893, FUN 3968, SET 3109), bugs M19-M31 all fixed.

This guide describes how `main/main.c` integrates the runtime modules in this workspace: `components/shell`, `components/storage`, `components/batch`, `components/command`, `components/display`, `components/windows`, `components/header`, `components/led`, `components/networking`, `components/usb`, and `components/c6ota`.

## Architecture
- `main/main.c` is the application entry point: boot sequencing, LVGL event callbacks, UI construction, and the c6ota/usb host bridges. It contains no command implementations and no shell state.
- `components/shell` owns the transcript and async buffer, command history, debug log, UART 
console, the input-line prompt contract, the interactive keypress queue, the DOS prompt template engine, the boot 
banner, the system info commands, and the read-only FreeRTOS task introspection (`ps` / `tasks` / `top` via
  `shell_command_ps()`).
- `components/storage` owns the guarded SD session, persistent mount tracking, path resolution, FATFS conversion, size formatting, DOS wildcard matching, the RAM-only current working directory, the output-redirection writer, every DOS file command, and the hidden `.trash` recycle bin (`trash.c`) that `del`/`rd /s` move entries into and `undelete`/`trash` manage.
- `components/batch` owns the batch engine (file execution, `:label`s, `goto`, `call :label`, 
`for` loops incl. `for /f` file-line iteration, the `|` pipe operator), the RAM-only environment variables and PATH, variable expansion, errorlevel, the
batch language commands, the `calc` float calculator (`calc.c`), and the DOSKEY-style alias table (`alias` / `unalias`, prompt-only
expansion, SD persistence via `alias /save`).
- `components/command` owns the single dispatcher, the execution pipeline, output-redirection parsing, the worker task, the hardware commands, and the remaining system commands. The display/keyboard/windows UI query handlers live in `components/command/command_ui.c`.
- `components/ansi` owns ANSI/VT SGR escape sequence processing, 16-color palette, and format string builder.
- `components/display` owns all display hardware state: rotation, resolution, refresh rate, brightness, power management, and touch handle.
- `components/windows` owns the LVGL screen layout: named regions, dynamic scaling, rotation-aware layout, and consistent styling.
- `components/header` owns the fixed top-bar LVGL widgets for notifications plus Wi-Fi, battery, Bluetooth, USB, and SD status.
- `components/editor` owns the DOS-style `edit` text editor: a byte-preserving
  document model (`editor.c`), a modal LVGL surface with syntax-coloured
  spans, a block cursor, a selection overlay, and status-bar prompts
  (`editor_view.c`), and a worker-task session that keeps file I/O off the
  LVGL task. It is now a surface on the shared modal runtime in
  `components/modal/`; file I/O still runs on the command worker task.
- `components/modal` owns the shared modal runtime (`modal.c`) and the
  ready-made batch surfaces `dialog`, `list`, and `ask` (`modal_surf.c`).
  It generalises the editor pattern into one session loop + input-routing
  layer used by every native modal surface.
- `components/applib` owns the native-app runtime library (`applib.h`): the
  formal app `printf`/stdout contract mapped onto the transcript (the
  redirection layer), the shared memory-allocation policy and debug-log error
  reporting, time/timer/sleep/system-info helpers, and Wi-Fi state accessors
  that route through the registered `applib_net_ops_t` table. It is a leaf
  (depends only on `shell`, `clock`, and the FreeRTOS/heap/esp_timer IDF
  components) and never includes `networking.h`.
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
                 modal <- editor
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
7. Call `windows_init()` (from `shell_build_ui()`) to build the LVGL shell surface (header, transcript, input row, keyboard). TUI surfaces (`dialog`/`list`/`ask`/`browse`/`view`/`hexview`) need no separate init — they reuse the shared modal runtime and `windows_enter_editor_mode()` / `windows_refresh_editor_surface()` to map the logical `P4_CONFIG_TUI_COLS`×`ROWS` (`80×25`) grid onto the live transcript region.
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

## Modal app surfaces (the `edit` pattern)

`edit` was the first native "app" that runs on top of the shell core. Since
v0.33.0 the pattern is generalised in `components/modal/`: a single shared
runtime owns the session loop and input routing, and surfaces only implement
open/service/close + input handlers. A modal app surface takes over the
transcript region and the input row,
captures every input source, and yields the shell back cleanly when it
closes. The pieces an app needs:

1. **A `modal_surface_t` descriptor.** The surface provides `open`, `service`,
   `close`, `handle_usb_key`, and `handle_serial_line` callbacks. The runtime
   creates the event group, calls `open`, services custom bits, and tears the
   surface down on `MODAL_EVENT_CLOSE_REQUEST`.
2. **A worker/LVGL split.** The surface's session runs on the command worker
   task inside `modal_surface_run()`. File I/O and waits happen there; the
   LVGL surface is opened with `lv_async_call` and every widget operation
   stays on the LVGL task. A `lv_async_call` failure path frees everything and
   returns an error, so a dead LVGL heap can never be touched.
3. **The window-manager handoff.** `windows_enter_editor_mode()` hides the
   shell input widgets and aliases the transcript container as the app
   surface; `windows_exit_editor_mode()` restores them. The app hides the
   shell's span group (`windows_get_transcript_spans()`), mounts its own
   content, and re-shows the shell output on close.
3. **Ops-table input routing.** The shell core and UART reader reach the app
   through `shell_command_ops_t` hooks (the editor registers
   `editor_is_active`, `editor_handle_usb_key`, `editor_handle_serial_line`).
   The single keyboard `LV_EVENT_VALUE_CHANGED` handler in `main.c` owns mode
   switching and routes every OSK button either to the shell input line or to
   the active app. Serial lines are forwarded to the app verbatim so text
   typed on UART is never misrouted to the dispatcher.
4. **Guarded SD I/O.** All file access goes through
   `shell_sd_begin`/`shell_sd_end`, prechecking free space and removing a
   partial destination on a failed write (the same guardrails as `copy`).
5. **A stateful status bar.** The input row becomes the app's status line; an
   inline prompt system (Find/Replace/Go-to/Save-As/confirm) reuses that bar
   instead of popping dialogs.
6. **Graceful close.** Quit signals the worker through the session event
   group; the worker tears the view down and waits for the LVGL callback
   before freeing the document. A rotation rebuild closes the view and wakes
   the worker instead of leaving dangling widgets.
7. **Pure logic, unit tested.** The document model (lines, cursor, selection,
   undo, find/replace) is LVGL-free and covered by `test/main/test_editor.c` —
   the same "pure logic in the module, hardware on the board" split the text
   tools use.

Built-in surfaces in `components/modal/modal_surf.c` — `dialog`, `list`, `ask`,
`browse`/`filebrowser`, `view`, and `hexview` (6 surfaces) — are exposed as
batch commands and demonstrate the contract. A third-party `.app` can register
its own `modal_surface_t` instead of duplicating the session/input plumbing.

### TUI Integration (v0.35.1 — hardware-verified on COM11, final TUI state)

The TUI layer is live and hardware-verified (flash to COM11, boot `1024x510` transcript rect, extensive serial verification of `draw box single/double/rounded` with title + nested window stack, `draw line`/`fill`/`text`/`clear`/`window`, `draw fullscreen on|off` + `tui fullscreen on|off`, `color`/`locate`, `dialog`/`list`/`ask` with timeout + serial input, `browse`/`view`; no abort/watchdog/overlap, header kept unless fullscreen) for batch apps:

- **Logical grid** (`components/tui/tui.h:35` `tui_cell_t utf8[4]`, `components/tui/tui.c:116` `tui_cell_set`): `P4_CONFIG_TUI_COLS`×`P4_CONFIG_TUI_ROWS` (`80×25` `p4minishell_config.h:298`) heap cell buffer (PSRAM `MALLOC_CAP_SPIRAM`, `utf8[4]` holds full 3-byte box UTF-8 `SH_BOX_*` single `SH_BOX_TL`/`H`/`V` double `SH_BOX_TL2`/`H2`/`V2` rounded `SH_BOX_TLR`/`TRR`/`BLR`/`BRR`) with fg/bg/attribute per cell, mapped to the *live* transcript region `1024x510` via `windows_enter_tui_mode()` / `windows_refresh_tui_surface()` / `windows_notify_keyboard_visibility` (`components/windows/windows.c:312`). The pixel rect follows rotation and on-screen-keyboard visibility; the logical grid is clamped to `80×25`, never to pixels. `P4_CONFIG_TUI_*` is the single source of truth. `tui status` shows `rect 1024x510 cols 80 rows 25`.
- **Font** (`managed_components/lvgl__lvgl/src/font/lv_font_unscii_16.c`, `sdkconfig.defaults:33` `CONFIG_LV_FONT_UNSCII_16=y`): extended `unscii_16` in-place with box-drawing U+2500-U+257F and symbols U+2600-U+26FF (384 glyphs, cmaps 3, no duplication); `windows_get_terminal_font()` returns it for `s_tui_label` (`lv_label_set_recolor true`).
- **Batch TUI verbs** (`components/tui/tui.c:228` `tui_draw_box` honors style+title, `components/tui/tui.c:283` `tui_draw_line`, `components/tui/tui.c:356` `tui_flush`): `draw box` single/double/rounded with title + nested window stack, `draw line`/`fill`/`text`/`clear`/`window`, `draw fullscreen on|off` (global) + `tui fullscreen on|off` (per-app, header hidden completely via `windows_set_fullscreen`/`header_set_visible` `components/windows/windows.c:418`, kept visible by default, dynamic keyboard scaling via `windows_notify_keyboard_visibility`); `color` (`tui_set_default_color`) / `locate` (`tui_set_cursor`) compose on the cell buffer; `ansi`/`menu` SGR codes share `components/ansi/ansi.c` (CSI parsing) and `tui_flush` coalesces per-fg-run `#RRGGBB ` recolor via `ansi_get_palette_color` PowerShell palette (no duplicate). `draw` auto-enters TUI (`tui_init` `components/tui/tui.c:56`) when no TUI/modal surface is active.
- **Prompt** (`main/main.c:112` `shell_prompt_render_plain()` `components/shell/shell.c:412`, `components/modal/modal_surf.c:412` `ask` placeholder + `keyboard_bind_textarea`): all inputs honor the DOS prompt template (`PROMPT=` `$p $g` etc) with situational color (`SH_PROMPT`).
- **Screenshot debug loop** (`grab_screenshot.py --port COM11 --out out.png --crop-transcript` + `capture_tui.py`, `tui status`): crops to transcript rect for pixel-perfect verification (used during hardware bug hunting).
- **Modal surfaces for TUI**: `browse`/`view`/`hexview` are `modal_surface_t` surfaces on the same runtime — they hide the shell span group, mount their own content, and restore on close. New full-screen surfaces MUST be `modal_surface_t` descriptors (never a private loop) and MUST use the `windows_enter_tui_mode` handoff pattern (or `windows_set_fullscreen` for fullscreen) and MUST be routed through the generic `shell_command_ops_t.modal_*` hooks.
- **Native TUI SDK stub**: `components/applib/applib_tui.h` declares `tui_create/destroy/box/print_at/refresh/clear` (heap cell buffer, same logical grid); stubbed with a warning in v0.35.1 and implemented after companion batch verification (fully TUI-expanded and hardware-verified on COM11 (7 BATs, push_sd.py PASS, no abort/watchdog/overlap, M31 stack overflow at 0x4012b75a fixed)). Included via `applib.h` umbrella.

## applib — the native-app runtime

`components/applib` is the stable runtime surface a native app links against.
It is organized as **lean, focused headers** — an app includes only the groups
it uses (the umbrella `applib.h` pulls all of them in):
`applib_console.h`, `applib_mem.h`, `applib_time.h`, `applib_net.h`,
`applib_input.h`. It depends only on `shell`, `clock`, and the FreeRTOS/heap/
esp_timer IDF components, so apps never reach into shell/clock/networking
internals. The service groups:

1. **Console output** (`applib_console.h`) — `app_printf` / `app_vprintf` /
   `app_printf_ansi` / `app_vprintf_ansi`, the semantic helpers
   (`app_print_heading`, `app_print_field`, `app_print_ok`,
   `app_print_error`, `app_print_warning`, `app_print_muted`,
   `app_print_usage`), and `app_print_styled(sgr_codes, ...)` (menu/form
   primitive: text wrapped in ANSI SGR codes such as reverse video, bold, or
   colour). An app's stdout is the transcript, which is the redirection layer:
   an app invoked inside a command dispatch is captured by `>` / `>>` exactly
   like a built-in command.
2. **Memory + error reporting** (`applib_mem.h`) — `app_alloc`/`app_calloc`/
   `app_realloc`/`app_strdup`/`app_strndup`/`app_free` implement the shared
   policy: blocks of `P4_CONFIG_APPLIB_PSRAM_THRESHOLD_BYTES` (512) or more
   prefer PSRAM with an internal-heap fallback; smaller blocks use the
   internal heap. Every call returns NULL on failure — check it.
   `app_report_error`/`app_report_warning`/`app_report_info` route
   diagnostics into the shell debug log.
3. **Time, timers, sleep, sysinfo** (`applib_time.h`) — `app_time`,
   `app_time_local`/`app_time_utc`, `app_uptime_sec`, `app_now_ms`,
   `app_delay_ms`, `app_time_synced`, `app_uptime_formatted`, `app_sysinfo`.
4. **Networking state** (`applib_net.h`) — `app_wifi_is_connected`,
   `app_wifi_get_rssi`, `app_wifi_state_string` read through the registered
   `applib_net_ops_t` table. `command_init()` registers the hooks; every hook
   is NULL-checked, so the helpers degrade to safe defaults before
   registration.
5. **Input with timeout** (`applib_input.h`) — `app_wait_key(timeout_ms,
   &key)`, `app_read_line(buf, size, timeout_ms)`, and
   `app_read_password(buf, size, timeout_ms)` give apps bounded keypress /
   line / password reads (the primitives behind `pause` / `choice /T` / `set
   /p /T` / `set /p /P`), returning false on a headless board instead of
   stalling, and `app_menu(title, items, count, timeout_ms)` renders a
   numbered form and returns the chosen index (0 on cancel).
6. **Persistent state** (`applib_state.h`) — `app_ini_get` / `app_ini_set` /
   `app_ini_delete` read and update `KEY=VALUE` INI files on the SD card and
   `app_temp_path` / `app_temp_cleanup` manage SD-backed temporary files
   (under `sd:/tmp`), all wrapping the shared `storage_ini.c` core.
7. **App mode** (`applib_ui.h`) — `app_mode_enter(full_screen)` /
   `app_mode_exit()` save and restore the screen (and optionally hide the
   shell input widgets for a full-screen app surface), the same primitives
   the batch `appmode` command uses.

When another layer needs a service an app should see, extend the matching
`applib_*.h` header (declare it once, in exactly one header) and implement it
in `components/applib` (or route it through an ops table when the owner lives
higher in the stack), then add the component to the root and test
`CMakeLists.txt` `EXTRA_COMPONENT_DIRS`.

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
   | Batch language | `components/batch/batch.c` (+ `components/batch/batch_expr.c` for `set /a`, `components/batch/calc.c` for `calc`) | declare in `batch.h` / `calc.h` |
   | Storage verbs | `components/storage/storage_nav.c` / `storage_files.c` / `storage_disk.c` / `storage_text.c` / `storage_fam.c` | declare in `storage_commands.h` |
   | Audio verbs | `components/command/audio_commands.c` | declare in `command.h` |
   | TUI / modal verbs | `components/command/tui_commands.c` | declare in `command.h` |
   | Peripheral toolkit | `components/command/periph_commands.c` | declare in `command.h` |
   | Power / display / battery | `components/command/power_commands.c` | declare in `command.h` |
   | Screenshot / serial | `components/command/serial_commands.c` | declare in `command.h` |
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

The `ps`/`tasks`/`top` `/O:` sort comparator follows the same rule:
`shell_task_row_compare()` (declared in `shell.h`) is a pure function over
the normalized `shell_task_row_t` rows and is unit-tested. New sort keys are
one enum value plus one `case` in the comparator and the `/O:` parser.

The same rule covers non-text logic: the modal `/t:`/`/v:` option parsers
(`modal_parse_timeout_arg()` / `modal_parse_var_arg()`, declared in
`modal_surf.h`), the power seconds/wake-cause helpers and the BMP header
writer (declared in `command.h`), and the history file round-trip
(`shell_history_save_lines()` / `shell_history_load_lines()`, declared in
`shell.h`). When an option parser is copy-pasted at three or more call
sites, extract it once and unit-test it (`test_modal.c` is the example).

### Power idle integration

To treat a new input source as shell activity (so it resets the `power idle`
clock and wakes a dimmed display), call `shell_power_notify_activity()` from
the input-injection point. The UART path routes through the
`shell_command_ops_t.pm_notify_activity` hook, so keep the dependency one-way;
USB and touch wake through main.c and the LVGL indev scan in
`shell_power_idle_tick()`. Idle-off must only toggle the backlight via
`display_set_power_state()` — never halt the panel or touch shell state.

### Audio playback

All audio lives in `components/audio/` (`audio.h`). To play a tone or WAV,
call `audio_play_tone()` / `audio_play_wav()` — they post a request to the
component's background task and return immediately; check `audio_busy()` to
avoid the single-slot refusal, and `audio_stop()` to cancel. The command layer
(`components/command/command.c`) only parses `beep`/`tone`/`wavplay`/`audio`/
`volume` and calls these — do not reimplement codec or playback logic there.
Chunk buffers stay on the heap, never on the audio task's stack.

### Clipboard

The RAM clipboard is shell-core state (`components/shell/`): use
`shell_clipboard_set()` / `shell_clipboard_set_file()` to store text or a file
reference, `shell_clipboard_get()` / `shell_clipboard_is_file()` to read it,
`shell_clipboard_copy_transcript(n)` to grab the last n transcript lines, and
`shell_input_line_paste()` to insert into the input line. New clipboard verbs
should dispatch from `components/command/` and call the `shell.h` API, keeping
file work in the command layer — never add clipboard state to command.c.

### Long command lines, completion, and history

Command lines are up to `P4_CONFIG_COMMAND_BYTES` (4096); any new function on
the worker/UART/LVGL path must heap-allocate command-sized locals (never a
`SHELL_COMMAND_BYTES` array on the stack). Tab completion routes through
`shell_command_ops_t.complete_word` — the provider stays in command.c and
touches the SD via storage. History is heap-backed with
`shell_history_get_count/get/clear`; the `history /save`/`/load` verbs write
`P4_CONFIG_HISTORY_PROFILE` through the guarded storage session (atomic temp +
rename).

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

## Batch process model

A batch file runs as a command stream on the single command worker task, but
its I/O and state contract is fully defined (see also `command.md`, "Batch
process model"):

| Concern | Definition |
|---------|------------|
| stdout | Every command's transcript output, captured by `>` / `>>`. A batch line inherits the caller's redirect. |
| stderr | Not a separate stream. Errors interleave on the transcript (and the redirect); failure is signalled by ERRORLEVEL. |
| stdin | The storage input-redirection slot (a `< file`, a pipe stage, or an explicit filename). Consumed by the text tools via `storage_resolve_input_source()`, by `for /f` over an empty set, and by `set /p NAME=< file` (one line). The interactive key queue backs `set /p`/`pause`/`choice` when no redirect is active. |
| argv | `%0` = script name, `%1`..`%9` = caller arguments, `%*` = everything from `%1`. `call`/`call :label` push a fresh frame; `shift` slides it. |
| cwd | RAM-only current working directory owned by `components/storage/` (`storage_set_cwd()` / `shell_get_cwd()`); relative paths resolve against it at run time. |
| PATH | RAM-only `PATH` environment variable (default `sd:/`); `shell_resolve_batch_path()` tries the literal name, `name.bat`, then each `;`-separated PATH entry with both forms. |
| environment | The 24-slot RAM table is session-global; `set`/`set /a`/`set /p`/`calc` mutate it, `call` hands it to the callee, `setlocal`/`endlocal` snapshot/restore it (a scope left open is unwound when its frame returns). |
| errorlevel | `batch_get_errorlevel()` / `batch_set_errorlevel()`, read by `if errorlevel N` and `&&`/`||`. |

There is no process isolation: the file shares the worker task, the
environment table, and the transcript with the whole shell. Batch files are
therefore a scripting surface, not an app ABI — native apps use the modal
app-surface pattern above.

Batch files are first-class **pipe processes**: a stage's output is spooled
through an SD temp file and the next stage reads it via
`storage_resolve_input_source()` / `storage_get_input_redirect()`, so a `.bat`
can sit in the middle of a pipeline (`echo x | filter.bat | findstr ...`)
reading its stdin with `for /f ... in ()` or `set /p NAME=<` and writing its
stdout through `echo`/`>`/`>>`. The redirection capture is re-entrant, so an
outer `>`/`>>` on a pipeline still receives the whole output. The process
itself is introspectable with `proc` (`/args` `/name` `/depth`
`/errorlevel` `/echo` `/stdin`) and the current exit code expands as
`%ERRORLEVEL%`.

**Shared libraries of batch routines** give scripts the same modularity
applib gives native apps: `call <file.bat>::<routine> [args]` loads an
external `.bat` and starts it at `:routine`, returning on `exit /b` /
`goto :eof` / EOF. Routine calls run in an automatically-pushed environment
scope (variable isolation beyond `setlocal`), so a library routine's
temporary variables never leak into the caller; its arguments arrive as
`%1`..`%9`/`%*` and its final errorlevel propagates. See `command.md`
("Shared library of batch routines") for the authoring pattern.

**Persistent state** for batch apps is provided by the `ini`, `appconfig`,
and `temp` commands (all storage on the SD card), backed by the same
`storage_ini.c` core applib wraps. `ini` reads/updates any `KEY=VALUE` file
and imports/exports the environment; `appconfig <app>` gives an app its own
`sd:/APPS/<APP>.INI` settings file without hand-rolling parsing; `temp`
creates and cleans SD-backed temporary files (`sd:/tmp`). A batch app keeps
state exactly the way DOS apps did: environment variables, temp files, and a
simple INI settings file.

## Authoring and deploying batch files

Batch files are plain text stored on the SD card; there is no compilation
step. Host-side rules that make a file behave correctly on the firmware:

- **Encoding and line endings.** UTF-8 text (FATFS is configured for UTF-8
  long file names). Both CRLF and LF line endings are accepted by the batch
  reader; files round-trip through `edit` byte-preserving.
- **Extension and placement.** A file must be named `*.bat` (any folder).
  Invoke it by name at the prompt (`myscript.bat` or `myscript`), or by
  PATH (`path sd:/scripts;...`). `AUTOEXEC.BAT` on the SD root runs at boot.
- **Line and size limits.** Each line is capped at
  `P4_CONFIG_BATCH_LINE_BYTES` (384); a trailing `^` joins up to
  `P4_CONFIG_LINE_CONTINUATION_MAX` (8) physical lines into one logical
  line. Up to `P4_CONFIG_BATCH_LABEL_MAX` (32) `:label` targets per file,
  labels capped at `P4_CONFIG_BATCH_LABEL_BYTES` (48) bytes.
- **Arguments.** `%0`..`%9` and `%*`; `shift` slides them. At most
  `P4_CONFIG_BATCH_ARGS_MAX` (9) arguments are captured.
- **Quoting and escaping.** `"text"` groups with expansion, `'text'` groups
  literally, `^c` escapes one character, `%%` is a literal `%` inside a
  batch file. Any line using `&`/`|`/`<`/`>` as *shell* syntax must be
  written carefully — a lone `&`/`|` splits the chain/pipeline before the
  command runs, so arithmetic `set /a` and `calc` expressions using those
  operators must be quoted.
- **Environment hygiene.** Variables are capped at `P4_CONFIG_ENV_VAR_MAX`
  (24) with names `[A-Za-z0-9_]` (upper-cased); a `setlocal` block that
  creates variables should close with `endlocal` so the 24-slot table is
  not exhausted by long scripts.
- **Validation before deploying.** Run the file once under `echo on` (or
  `tron`-style line-by-line) from the UART console, check the ERRORLEVEL of
  each step with `if errorlevel`, and keep a copy on the host — the shell
  is a runtime, not an editor for recovery (though `edit` can fix a broken
  file in place).

There is no binary packaging step for batch files: writing the `.bat` to the
card (via the shell's `write`/`append`, `edit`, or copying from a host) is
the whole deployment flow. `alias /save` and `history /save` write the same
kind of plain-text batch-compatible profiles (`ALIASES.BAT`, `HISTORY.TXT`)
through the guarded storage session.

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