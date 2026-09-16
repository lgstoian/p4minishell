# Hosted Module API

This document lists the C API each component exposes to the rest of the
firmware, organized by module and mirroring the headers under `components/`.

> **Current public API reference (v1.0.0).** For the working rules and
> invariants behind these functions see [`ai-context.md`](ai-context.md); for
> integration examples see [`SDK.md`](SDK.md); for the user-facing command
> surface see [`command.md`](command.md). Current verified test baselines live
> in [`test/README.md`](test/README.md).

## Shared integration pattern
- `main/main.c` is the application entry point: boot sequencing, LVGL event callbacks, UI construction, and the c6ota/usb host bridges. It holds no command implementations and no shell state.
- `components/shell` owns the transcript and async buffer, command history, debug log, UART console, the input-line prompt contract, and the system info commands.
- `components/storage` owns the guarded SD session, persistent mount tracking, path resolution, FATFS conversion, size formatting, DOS wildcard matching, the RAM-only current working directory, the output-redirection writer, and every DOS file command.
- `components/batch` owns the batch engine (file execution, `:label`s, `goto`, `call :label`, `for` loops, the `|` pipe operator), the RAM-only environment variables and PATH, variable expansion, errorlevel, and the batch language commands.
- `components/command` owns the single dispatcher, the execution pipeline, output-redirection parsing, the worker task, the hardware commands, the UI query commands, and the remaining system commands.
- `components/ansi` owns the ANSI/VT escape sequence processing: SGR color palette, format string builder, text processing.
- `components/display` owns all display hardware state: rotation, resolution, refresh rate, brightness, power management, and touch handle.
- `components/windows` owns the LVGL screen layout: named regions, dynamic scaling, rotation-aware layout, and consistent styling.
- `components/header` owns the fixed top-bar UI for notifications and passive status display.
- `components/editor` owns the DOS-style `edit` text editor: the byte-preserving document model, the modal LVGL surface (syntax-coloured spans, block cursor, selection overlay, status-bar prompts), and the worker-task session. It is a surface on the shared modal runtime.
- `components/modal` owns the shared modal runtime (`modal.{h,c}`) and the ready-made batch surfaces `dialog`, `list`, and `ask` (`modal_surf.{h,c}`).
- `components/led` owns the WS2812 RGB status LED on GPIO26 (espressif/led_strip over RMT), the
  animation task, and the auto status / event notification engine.
- `components/networking` owns hosted Wi-Fi runtime state and also bootstraps the hosted Bluetooth module.
- `components/usb` owns USB Host Library state, USB MSC storage, and USB HID keyboard or mouse debug behavior.
- `components/c6ota` owns the shell-visible ESP32-C6 OTA workflow and depends on `components/networking` for Wi-Fi wait and restore hooks.
- `components/tui` owns the 80x25 TUI cell buffer, the drawing primitives, and `tui_flush`; it is reached by the `draw`/`tui` verbs and by `components/windows`.
- `components/gfx` owns the pure RGB565 raster core (surface/pixel/line/rect/circle/blit), BMP header parse + decode, and 565→888 row conversion. It has no LVGL dependency; the `gfx` command glue is `components/command/gfx_commands.c`.
- `components/db` owns the Palm-OS-style SD record store (`sd:/DBS/<name>.DB`); the `db` command lives in `components/command/db_commands.c`.
- `components/alarm` owns the SD alarm/event store and the single background checker; the `alarm`/`cal` commands live in `components/command/alarm_commands.c`.
- `components/filetype` owns the one extension→kind registry (batch/markdown/json/text) used by the editor, viewer, `launch`, `dir`, and the batch resolver.
- `components/markdown` owns the CommonMark-subset renderer used by the `markdown` verb and `view *.md`.
- `components/font` owns the font registry (roles/sizes/fallbacks), the SD TTF loader, CJK auto-attach, and the theme table.
- `components/boot` owns the `CONFIG.SYS` parser and `AUTOEXEC.BAT` runner (plus default-file generation).
- `components/storage/storage_csv.c` owns the one RFC-4180-subset CSV parser (`csv_split_line`) shared by the `csv` and `export` command bodies; `components/db/` owns the one `k=v` field parser (`db_field_get`).
- `components/networking/tcpterm.c` owns the one-shot TCP terminal (`tcpterm`), with the other lwIP socket surfaces.
- `components/usb/userial.c` owns the lazy CDC-ACM class driver + byte API/RX ring; the `usb userial` verbs live in `components/command/userial_commands.c`.
- `components/clock/clock_timer.c` owns the named stopwatch slots behind `timer`/`stopwatch`.
- `components/gfx/gfx_view.c` owns the world-coordinate viewport (map/clip/nice-step) behind `plot`.
- `components/clock` owns time/SNTP/timezone services and the `date`/`time`/`timezone`/`sntp` verbs.
- `components/audio` owns the ES8311 codec path, volume, and the background tone/WAV engine; the verbs parse in `components/command/audio_commands.c`.
- `components/applib` is the native-app runtime/ABI library (umbrella `applib.h` over the lean `applib_*.h` headers).
- All modules keep user-visible behavior in the shell transcript or fixed status header instead of returning rich status objects to the caller.

### Dependency direction

```
main  ->  command  ->  batch  ->  storage  ->  shell  ->  ansi, display, windows, header, keyboard, clock
                 modal <- editor
```

No component declares `main` as a requirement. Two registered function tables invert the only
upward dependencies rather than creating include cycles — the same pattern
`components/networking` uses with `networking_host_ops_t`:

| Table | Declared in | Registered by | Lets the lower layer reach |
|-------|-------------|---------------|-----------------------------|
| `shell_command_ops_t` | `shell.h` | `command_init()` | dispatch, cwd, volume, SD mount state, battery |
| `batch_command_ops_t` | `batch.h` | `command_init()` | the full command pipeline for nested lines |

## Shell Core API

Declared in `components/shell/shell.h`.

### Command module hooks

```c
typedef struct {
    void        (*execute_command)(char *command);
    const char *(*get_cwd)(void);
    int         (*get_volume_percent)(void);
    bool        (*sd_is_mounted)(void);
    esp_err_t   (*battery_read)(int *battery_mv, int *percent, int *raw, int *gpio_mv);
    /* External-module accessors (all NULL-checked before use): */
    bool        (*wifi_is_connected)(void);
    bool        (*wifi_get_rssi)(int *rssi_out);
    const char *(*wifi_state_string)(void);
    void        (*append_sysinfo_summary)(void);
    bool        (*bluetooth_is_enabled)(void);
    bool        (*bluetooth_is_connected)(void);
    bool        (*usb_is_connected)(void);
    bool        (*usb_is_keyboard_attached)(void);
    bool        (*bg_jobs_running)(void);
    bool        (*usb_key_to_ascii)(uint8_t key_code, uint8_t modifiers, char *out);
    bool        (*c6ota_is_pending)(void);
    bool        (*c6ota_is_busy)(void);
    void        (*pm_notify_activity)(void);
    int         (*pm_ms_until_idle_off)(void);
    bool        (*time_auto_sync)(void);
    int         (*complete_line)(const char *line, int match_index,
                                 char *out, size_t out_size);
    int         (*ghost_line)(const char *line, char *out, size_t out_size);
} shell_command_ops_t;

void shell_register_command_ops(const shell_command_ops_t *ops);
```
- Registered once by `command_init()`. Passing `NULL` clears the table.
- Every hook is NULL-checked at the call site, so the shell degrades gracefully if used
  before the command module initializes.
- The external-module accessors let `shell.c` read state from the networking, Bluetooth,
  USB, and C6 OTA modules without including their headers, keeping the dependency
  direction one-way.
- `complete_line` backs USB Tab completion: it parses the whole `line` and fills `out` with the
  `match_index`-th match (0-based) for its last word — help-table command names, aliases,
  installed apps, usage-derived subcommands/flags, and SD paths — returning the total match
  count (capped by `P4_CONFIG_COMPLETION_MAX_MATCHES`). Implemented by `command_complete_line()`.
- `ghost_line` backs the inline ghost suggestion (`P4_CONFIG_COMPLETION_GHOST`): it is SD-free
  (command names + usage tokens only) so it can run on every keystroke.
  Implemented by `command_ghost_line()`.

### Semantic output helpers
```c
void shell_print_heading(const char *format, ...);
void shell_print_field(const char *label, const char *format, ...);
void shell_print_field_num(const char *label, long value);
void shell_print_ok(const char *format, ...);
void shell_print_error(const char *format, ...);
void shell_print_warning(const char *format, ...);
void shell_print_muted(const char *format, ...);
void shell_print_usage(const char *format, ...);
```
- Apply the shared palette from `components/ansi/ansi_palette.h`, so commands never choose
  colours themselves. Each emits its own reset and trailing newline.
- The caller's text is rendered with real `vsnprintf` before the colour wrap, so printf
  flags work and a value containing `@` cannot be mistaken for a colour specifier.
- Prefer these over raw `shell_transcript_appendf_ansi()` for headings, labelled fields,
  and the success / error / warning / usage shapes. Use `shell_transcript_appendf_ansi()`
  with `SH_*` macros directly only for composite lines that mix several categories.

### Transcript
```c
void        shell_transcript_append_text(const char *text);
void        shell_transcript_appendf(const char *format, ...);
void        shell_transcript_append_ansi(const char *text);
void        shell_transcript_appendf_ansi(const char *format, ...);
void        shell_schedule_transcript_appendf(const char *format, ...);
void        shell_schedule_transcript_appendf_ansi(const char *format, ...);
void        shell_transcript_reset(void);
void        shell_history_transcript_scroll_to_end(void);
void        shell_force_transcript_scroll_to_end(void);
size_t      shell_transcript_get_length(void);
const char *shell_transcript_get_text_from(size_t offset);
```
- `shell_schedule_transcript_appendf()` is the safe entry point from non-LVGL tasks
  (plain); `shell_schedule_transcript_appendf_ansi()` is the ANSI variant — it
  formats via `ansi_vformat` and the async flush dispatches to
  `shell_transcript_append_ansi` when the staged text contains `ESC` (`ansi_contains_escapes()`),
  otherwise to the plain append. Use the `_ansi` form for any coloured output
  from a non-LVGL task (e.g. networking, `main.c` progress).
- `shell_history_transcript_scroll_to_end()` follows only when the view is near the bottom
  (used after background/async output). `shell_force_transcript_scroll_to_end()` pins the
  view to the newest output regardless of position and is the command-submission path.
- `shell_transcript_get_length()` / `shell_transcript_get_text_from()` let the command
  module capture the output delta produced by a single command for `>` / `>>` redirection.

### Command history
```c
void        shell_store_command_history(const char *command);
void        shell_recall_history(int direction);   /* -1 older, +1 newer */
const char *shell_get_history_draft(void);
void        shell_reset_history_cursor(void);
bool        shell_command_should_store_history(const char *command);
void        shell_format_command_for_transcript(const char *command, char *output, size_t output_size);
size_t      shell_history_get_count(void);
const char *shell_history_get(size_t index);       /* 0 = oldest */
void        shell_history_clear(void);
```
- `shell_command_should_store_history()` returns false for `wifi connect <ssid> <password>`
  and while a C6 OTA confirmation is pending.
- `shell_format_command_for_transcript()` masks the password argument.
- The history is heap-backed (`strdup`'d lines, depth + total-byte cap); the accessors feed
  the `history` command's SD save/load.

### Input line
```c
void shell_input_line_set_text(const char *command_text);
void shell_input_line_reset(void);
void shell_extract_input_text(char *output, size_t output_size);
void shell_input_line_repair_prompt(const char *text);
void shell_input_line_paste(const char *text);
```
- The input line always renders the shell prompt as a literal prefix. These helpers are the
  only place that knows about that contract.
- `shell_input_line_paste()` inserts text at the cursor via `lv_textarea_add_text` (LVGL-locked,
  safe from any task); it is what `paste` calls.

### RAM clipboard
```c
void        shell_clipboard_set(const char *text);
void        shell_clipboard_set_file(const char *path);
const char *shell_clipboard_get(void);
bool        shell_clipboard_is_file(void);
bool        shell_clipboard_copy_transcript(int n_lines);
```
- Shell-core clipboard backing `clip` / `paste`. `shell_clipboard_copy_transcript(n)` copies
  the last n lines of the plain transcript (the same read the `>` redirection path uses).
  The `clip`/`paste` commands in `components/command/` call these and do the file-side work
  (`clip read`, `paste <dest>` copy) with storage helpers.

### Interactive keypress wait
```c
void shell_key_wait_begin(void);
void shell_key_wait_end(void);
bool shell_key_wait_is_active(void);
bool shell_wait_for_key(uint32_t timeout_ms, char *key_out);
bool shell_key_input_available(void);
bool shell_key_wait_submit(char key);
bool shell_read_line(char *output, size_t output_size, uint32_t timeout_ms);
```
- `shell_key_wait_begin()` flushes stale keys and marks the shell as waiting; every input
  source then routes keystrokes into the key queue instead of the command line. Pair it with
  `shell_key_wait_end()` on every return path.
- `shell_wait_for_key()` blocks until a key arrives or the timeout elapses. It returns false
  immediately when no wait is active, so a caller that forgets `begin` cannot hang.
- `shell_key_input_available()` reports whether the UART console is running or a USB keyboard
  is attached. Commands check it first and fall back to a timed path on a headless board.
- `shell_key_wait_submit()` is what the UART reader, the USB HID bridge, and `main.c`'s LVGL
  input-line callback call. It is a no-op when no wait is active.
- `shell_read_line()` collects a whole line, echoing as it types and opening its own keypress
  wait so input never reaches the command dispatcher. Backspace edits, ESC cancels, Enter
  submits. Returns false on cancel, timeout, or when no key source is attached. Backs `set /p`.

### Runtime prompt template
```c
void        shell_prompt_set_template(const char *template_text);
const char *shell_prompt_get_template(void);
const char *shell_prompt_render_plain(void);
void        shell_prompt_reset(void);
```
- One template drives both the UART console prompt and the LVGL input line.
- Supported metacharacters: `$p` path, `$g` `>`, `$l` `<`, `$b` `|`, `$n` drive, `$d` date,
  `$t` time, `$v` version, `$s` space, `$_` newline, `$q` `=`, `$$` `$`, `$a` `&`, `$c` `(`,
  `$f` `)`, `$e` ESC, `$h` destructive backspace. Codes are case-insensitive and an unknown
  code renders literally, both matching COMMAND.COM.
- `shell_prompt_render_plain()` omits `$e` because an LVGL textarea cannot render escapes.
- Passing NULL or an empty string to `shell_prompt_set_template()` restores the default.

### Debug log
```c
void   shell_debug_log_push(const char *tag, const char *message);
void   shell_record_errorf(const char *tag, int error, const char *format, ...);
void   shell_record_warningf(const char *tag, const char *format, ...);
void   shell_record_infof(const char *tag, const char *format, ...);
size_t shell_get_warning_count(void);
void   shell_command_debug(void);
int    shell_command_ps(int argc, char **argv);   /* 0 ok, 2 usage; /O: sort */
```
- `shell_command_ps()` implements `ps` / `tasks` / `top`: a read-only FreeRTOS
  task table (name, state, priority, core, stack high-water mark, per-task
  CPU% since the previous sample). It reads a heap-allocated
  `uxTaskGetSystemState()` snapshot capped by `P4_CONFIG_TASK_SNAPSHOT_MAX` and
  never mutates tasks. `/b` emits uncoloured machine-parsable rows; `/O:` sorts
  by `N` name / `C` CPU / `S` stack / `P` priority / `T` state (`-` reverses,
  name tie-break) with `top` defaulting to CPU-descending. Returns an
  ERRORLEVEL the dispatcher records (0 ok / 2 usage).
- Sorting uses `shell_task_row_compare()` (pure, unit-tested): the normalized
  row type is `shell_task_row_t` with the `shell_task_sort_key_t` enum, both in
  shell.h.

### Quoting and escaping
```c
typedef enum { SHELL_QUOTE_NONE, SHELL_QUOTE_DOUBLE, SHELL_QUOTE_SINGLE } shell_quote_state_t;

char *shell_find_unquoted_char(const char *text, char target);
char *shell_find_unquoted_any(const char *text, const char *targets);
bool  shell_has_unquoted_char(const char *text, char target);
char *shell_unescape_in_place(char *text);
```
- One scanner backs the argument tokenizer, redirection parsing, pipe splitting, chain
  splitting, and variable expansion, so those surfaces cannot disagree about syntax versus data.
- Rules: `"text"` groups and still expands variables, `'text'` groups literally with no
  expansion, and `^c` makes any single character literal (`^&`, `^|`, `^>`, `^"`, `^^`).
- `shell_unescape_in_place()` removes the markup once an extent is known. Redirection targets
  and every tokenized argument pass through it, so handlers receive literal values.
- Any new code that searches a command line for an operator must use these rather than a local
  quote check.

### Command chaining
```c
typedef enum {
    SHELL_CHAIN_FIRST,        /* first segment, always runs */
    SHELL_CHAIN_ALWAYS,       /* preceded by &  */
    SHELL_CHAIN_ON_SUCCESS,   /* preceded by && */
    SHELL_CHAIN_ON_FAILURE,   /* preceded by || */
} shell_chain_op_t;

typedef struct {
    char            *command;
    shell_chain_op_t op;
} shell_chain_segment_t;

int shell_split_chain(char *text, shell_chain_segment_t *segments,
                      int max_segments, bool *truncated_out);
```
- Splits on unquoted `&`, `&&`, and `||`. A line with no separator yields one
  `SHELL_CHAIN_FIRST` segment, so callers need no special case.
- A single `|` is deliberately **not** a separator; it is left in place for the pipeline
  splitter, which is what allows a pipeline to be one link in a chain.
- `truncated_out` reports that the line had more separators than `max_segments` allows.
- The command module owns the execution loop and the success test; this function only splits.

### Lifecycle and utilities
```c
void        shell_init(void);
bool        shell_is_initialized(void);
void        shell_uart_console_start(void);
void        shell_header_status_refresh(void);
uint32_t    shell_header_refresh_interval_ms(void);
bool        shell_text_equals_ignore_case(const char *left, const char *right);
char       *shell_trim(char *text);
int         shell_split_args(char *text, char **argv, int max_args);  /* quote/escape aware */
bool        shell_parse_percentage_arg(const char *text, int *percentage_out);
bool        shell_parse_size_arg(const char *text, size_t min, size_t max, size_t *out);
void        shell_join_args(char **argv, int start, int argc, char *out, size_t out_size);
void        shell_header_notify(const char *text, uint32_t timeout_ms);
void        shell_header_notify_level(const char *text, uint32_t timeout_ms,
                                      header_notify_level_t level);
void shell_get_cwd_for_prompt(char *buf, size_t buf_size);
```

`shell_header_status_refresh()` samples cheap status every call but throttles the
expensive telemetry (heap/CPU/battery) to `P4_CONFIG_HEADER_TELEMETRY_PERIOD_MS`,
and does nothing while the display is off by idle. `shell_header_refresh_interval_ms()`
returns the next adaptive poll interval (idle-off wake, busy OTA/bg job, Wi-Fi
bring-up, startup, clock minute boundary, idle-off deadline) for main to apply
with `lv_timer_set_period()`. The policy is the pure, unit-tested `header_refresh.c`.

## Command Module API

Declared in `components/command/command.h`.

### Execution
```c
void shell_execute_command(char *command);        /* expand + redirect + dispatch */
bool shell_execute_command_core(char *command);   /* dispatch only */
void shell_execute_command_async(char *command);  /* queue on the worker task */
bool shell_command_ota_is_pending(void);
```
- `shell_execute_command()` is the full pipeline and the correct entry point for nested
  contexts (`if`, `for`, pipes, batch lines) so they inherit expansion and redirection. The
  batch engine reaches it through `batch_command_ops_t` rather than including `command.h`.
- `shell_execute_command_core()` skips expansion and redirection; use it only when the caller
  has already performed both.
- `shell_execute_command_async()` is what the LVGL input path uses, so heavy commands never
  run on the event-callback stack.

### State accessors
```c
int         command_get_volume_percent(void);
esp_err_t   command_battery_read(int *battery_mv, int *percent, int *raw, int *gpio_mv);

/* Power / idle display-off (components/command) */
void        shell_power_set_idle_timeout(int seconds);
int         shell_power_get_idle_timeout(void);
void        shell_power_notify_activity(void);
void        shell_power_idle_tick(void);
```
- These are the concrete implementations behind two of the `shell_command_ops_t` hooks. The
  `get_cwd` and `sd_is_mounted` hooks are satisfied by `shell_get_cwd()` and
  `storage_sd_is_mounted()` from `components/storage/`. Any output pointer passed to
  `command_battery_read()` may be `NULL`.
- `shell_power_notify_activity()` is also the implementation of the
  `shell_command_ops_t.pm_notify_activity` hook (used by the UART console submit path);
  `shell_power_idle_tick()` runs on the LVGL task from the header-refresh timer.

#### TUI / GFX / asset verbs (split command files)
```c
/* components/command/tui_commands.c */
bool shell_command_draw(int argc, char **argv);   /* box|line|fill|text|bar|table|list|clear|
                                                     window|cursor|hold|alt-screen|save|restore|
                                                     close|refresh|fullscreen */
void shell_command_tui(int argc, char **argv);
void shell_command_color(int argc, char **argv);
void shell_command_locate(int argc, char **argv);
bool shell_command_anchor(int argc, char **argv);
bool draw_hold_active(void);                      /* `draw hold` state (unit-test hook) */

/* components/command/gfx_commands.c  (raster core: components/gfx/) */
bool shell_command_gfx(int argc, char **argv);    /* init|close|status|clear|pixel|line|rect|circle|
                                                     hline|vline|triangle|ellipse|polygon|fill|text|
                                                     show|load|blit|free|slots|save */

/* components/command/asset_commands.c */
void shell_command_crc32(int argc, char **argv);
void shell_command_asset(int argc, char **argv);  /* check|list <app> */
uint32_t shell_crc32_update(uint32_t crc, const uint8_t *data, size_t len);  /* one CRC-32 */
uint32_t asset_crc32_data(const uint8_t *data, size_t len);
bool     asset_parse_line(const char *line, char *path_out, size_t path_size, uint32_t *crc_out);
bool     asset_app_ok(const char *app);            /* [A-Za-z0-9_-]+ name rule (shared) */
bool     asset_crc_file(const char *resolved, uint32_t *crc_out);
int      asset_verify_app(const char *tag, const char *app, bool list_only);  /* 0 ok, 1 fail */

/* components/command/pkg_commands.c  (packaged SD apps: PKGS/<APP>/ -> APPS/) */
void shell_command_pkg(int argc, char **argv);     /* list|info|verify|check|install|remove */
bool pkg_app_name_from_appinfo(const char *filename, char *out, size_t size);  /* pure, unit-tested */

/* components/command/plot_commands.c  (coordinate layer over gfx canvas / TUI) */
bool shell_command_plot(int argc, char **argv);    /* tui|window|auto|axes|func|polar|para|
                                                      data|bar|table|line|point|clear|status */
```
- `gfx`, `plot`, and the mutating `draw`/`tui`/`color`/`locate`/`anchor` verbs REFUSE inside a `start`
  background job (shared display) with ERRORLEVEL `1` — except read-only
  `plot status` and text-only `plot table`, which are background-safe.
- `shell_crc32_update()` is the firmware's single CRC-32 (shared by `receive` and the `crc32`/`asset`
  verbs); start the accumulator at `0xFFFFFFFF` and invert at the end (zlib parity).
- `draw table`/`draw list` use `tui_draw_table_ex()` and `tui_table_parse_cursor()` /
  `tui_table_parse_sel()` (see the TUI Module API).

### Gfx raster core (`components/gfx/`)
Pure, LVGL-free RGB565 buffer math (`gfx.c`, unit-tested headless) plus a committed
8x8 ASCII font (`gfx_font.c`, generated by `tools/gen_gfx_font.py` from the
public-domain unscii-8 TTF):
```c
uint16_t gfx_rgb_to_565(uint32_t rgb);
bool gfx_surface_alloc(gfx_surface_t *s, int w, int h);
void gfx_surface_free(gfx_surface_t *s);
void gfx_surface_clear(gfx_surface_t *s, uint16_t color);
void gfx_surface_pixel(gfx_surface_t *s, int x, int y, uint16_t color);
uint16_t gfx_surface_get(const gfx_surface_t *s, int x, int y);
void gfx_surface_line(gfx_surface_t *s, int x1, int y1, int x2, int y2, uint16_t color);
void gfx_surface_rect(gfx_surface_t *s, int x, int y, int w, int h, uint16_t color, bool fill);
void gfx_surface_circle(gfx_surface_t *s, int cx, int cy, int r, uint16_t color, bool fill);
void gfx_surface_hline(gfx_surface_t *s, int x, int y, int w, uint16_t color);
void gfx_surface_vline(gfx_surface_t *s, int x, int y, int h, uint16_t color);
void gfx_surface_triangle(gfx_surface_t *s, int x1, int y1, int x2, int y2,
                          int x3, int y3, uint16_t color, bool fill);
void gfx_surface_polygon(gfx_surface_t *s, const int *xy, int n, uint16_t color, bool fill);
void gfx_surface_ellipse(gfx_surface_t *s, int cx, int cy, int rx, int ry, uint16_t color, bool fill);
int  gfx_surface_flood_fill(gfx_surface_t *s, int x, int y, uint16_t color);   /* pixels filled */
int  gfx_surface_text(gfx_surface_t *s, int x, int y, const char *text,
                      uint16_t color, uint16_t bg, bool use_bg, int scale);     /* advance width */
int  gfx_text_width(const char *text, int scale);
void gfx_surface_blit(gfx_surface_t *dst, const gfx_surface_t *src, int x, int y,
                      bool use_transparent, uint16_t transparent);
void gfx_565_to_888_row(uint8_t *dst, const uint16_t *src, int w);
bool gfx_bmp_parse_header_ex(const uint8_t *buf, size_t len, gfx_bmp_info_t *out,
                             int max_w, int max_h);   /* 24/32-bit, either orientation */
bool gfx_bmp_parse_header(const uint8_t *buf, size_t len, gfx_bmp_info_t *out); /* strict 24-bit bottom-up */
bool gfx_image_surface_alloc(gfx_surface_t *s, int w, int h);  /* up to GFX_IMAGE_MAX */
bool gfx_bmp_decode_scaled_565(const uint8_t *buf, size_t len, const gfx_bmp_info_t *info,
                               int dst_w, int dst_h, gfx_surface_t *out);
bool gfx_bmp_decode_565(const uint8_t *buf, size_t len, const gfx_bmp_info_t *info, gfx_surface_t *out);
void gfx_surface_blit_scaled(gfx_surface_t *dst, const gfx_surface_t *src, int x, int y,
                             int dw, int dh, bool use_transparent, uint16_t transparent);
void gfx_bmp_fit(int src_w, int src_h, int max_w, int max_h, int *out_w, int *out_h);
extern const uint8_t gfx_font8x8[GFX_FONT_GLYPHS][GFX_FONT_H];  /* index = ch - 0x20 */
```
- All raster ops clip (out-of-bounds writes are ignored, never an error); `gfx_surface_get`
  returns 0 outside. `gfx_surface_flood_fill` grows a PSRAM seed stack and returns the pixel
  count; `gfx_text_width` is `strlen(text) * GFX_FONT_W * scale`.
- `gfx_bmp_parse_header_ex` accepts 24/32-bit BI_RGB with positive (bottom-up) or negative
  (top-down) height within `max_w`x`max_h`; the strict `gfx_bmp_parse_header` is the sprite
  wrapper (24-bit bottom-up, `GFX_SPR_MAX`). `gfx_bmp_decode_scaled_565` decodes straight to a
  target size (the source is never materialized), and `gfx_bmp_fit` is the single
  never-upscaling aspect-fit used by the viewer/TUI/canvas. All pure and unit-tested.

### Plot viewport (`components/gfx/gfx_view.c`)
World-coordinate mapping shared by the canvas and TUI plot targets (pure,
headless-safe, unit-tested). The integer rect is origin-agnostic, so the same
code drives 0-based canvas pixels and 1-based TUI cells:
```c
void gfx_view_set(gfx_view_t *v, double xmin, double xmax, double ymin,
                  double ymax, int px, int py, int pw, int ph);
bool gfx_view_map(const gfx_view_t *v, double x, double y, int *sx, int *sy);
void gfx_view_point(const gfx_view_t *v, gfx_surface_t *s, double x, double y,
                    uint16_t color);
bool gfx_view_clip_line(const gfx_view_t *v, double x1, double y1, double x2,
                        double y2, int *ax, int *ay, int *bx, int *by);
bool gfx_view_line(const gfx_view_t *v, gfx_surface_t *s, double x1, double y1,
                   double x2, double y2, uint16_t color);
double gfx_view_nice_step(double range, int ticks);  /* 1/2/5x10^n */
```
- Window edges land exactly on the border indices (`(pw-1)`/`(ph-1)` scaling);
  rasters narrower than 2 wide/tall are degenerate (map/clip return false).
- Clipping is Cohen–Sutherland with trivial-reject first, so fully-outside
  segments return false and no giant coordinates reach the rasterizer.

### Shared canvas/draw accessors (`command.h`, for `plot`)
```c
uint16_t gfx_canvas_parse_color(const char *s, uint16_t fallback);  /* gfx_commands.c */
bool     gfx_canvas_is_open(void);
gfx_surface_t *gfx_canvas_surface(void);  /* NULL when closed */
bool     draw_require_foreground(const char *verb);  /* tui_commands.c */
uint8_t  draw_color_arg(const char *s, uint8_t fallback);
void     draw_maybe_flush(void);  /* hold-aware TUI flush */
```

### Theme registry (`components/font/theme.c`)
Pure data + selection (no LVGL/IO), unit-tested:
```c
const theme_t *theme_current(void);          /* active; never NULL */
const theme_t *theme_get(const char *name);  /* case-insensitive; NULL if unknown */
int  theme_builtin_count(void);
const theme_t *theme_builtin_at(int index);  /* NULL out of range */
bool theme_set(const char *name);            /* false leaves the active theme unchanged */
int  theme_active_index(void);
```
- `theme_t` holds the chrome colors (screen/transcript/input-row/keyboard bg, accent/body/
  muted/warn text, header panel tints, modal border/title/message) and the font roles.
- The command layer (`shell_command_theme`) drives `theme list|show|set [/save]`; applying a
  theme calls `windows_refresh_theme()`, `keyboard_refresh_theme()`, and
  `header_refresh_theme()`. Modal surfaces read `theme_current()` when they open. Persistence
  is the `theme` key in `sd:/APPS/SHELL.INI` (restored by `font_restore_saved()`).

### Responsive header (`components/header/`)
Measurement-driven layout with no overlap on any resolution or rotation:
```c
/* Pure policy, no LVGL/IO (header_layout.c), unit-tested: */
void header_layout_compute(header_layout_t *out, int avail_w, int gap,
                           const int status_w[HEADER_LEVEL_COUNT],
                           const int sys_w[HEADER_LEVEL_COUNT],
                           int center_min);
/* Widgets + live policy (header.c): */
int  header_get_height(void);            /* single height authority */
void header_set_mode(header_mode_t mode);
header_mode_t header_get_mode(void);
const char *header_mode_name(header_mode_t mode);
bool header_mode_parse(const char *text, header_mode_t *out);
void header_relayout(void);              /* safe from any task */
void header_get_metrics(header_metrics_t *out);  /* wanted vs actual widths */
/* Shell surface (header_commands.c): */
void shell_command_header(int argc, char **argv);  /* [status]|mode|show|hide */
void header_restore_saved(void);                   /* SHELL.INI header_mode */
```
- Every render measures the live labels and fits the three panels into the
  real content width (full → abbreviations → dynamic font step → notification
  yields → panel dropped, in that order); panels get explicit absolute
  geometry and the notification label is pinned to its container width.
- `font_restore_saved()` reports success; a failed first-mount restore re-arms
  via `storage_sd_first_mount_reset()` so the next mount retries instead of
  skipping the boot restore.

### Background jobs (`start` / `taskkill`)
```c
void shell_command_start(int argc, char **argv);      /* pooled worker, PSRAM stack */
void shell_command_taskkill(int argc, char **argv);
```
- Lives in `components/command/command.c`. Pool size `P4_CONFIG_BG_TASKS`; workers are created
  suspended at init with PSRAM stacks via `xTaskCreateStatic` and resumed per `start`.
- Cooperative kill: `batch_bg_request_kill()` sets a flag polled per batch line and every 100 ms of
  `delay`. `start` is refused while a C6 OTA is pending; `c6ota` is refused while a job runs.

### Persistent settings (`config` command)
`components/command/config_cmd.c` implements `config` (declared in `config_cmd.h`):
```c
void shell_command_config(int argc, char **argv);
int  config_directive_get(const char *text, const char *key, char *out, size_t out_size);
bool config_directive_upsert(char *text, size_t cap, const char *key, const char *value);
bool config_directive_remove(char *text, size_t cap, const char *key);
```
- The directive helpers are pure text functions (unit-tested, no I/O): they read, upsert
  (replace-all + append), and remove `KEY=value` lines in a CONFIG.SYS text buffer while
  preserving comments, blank lines, and unknown directives.
- `config` is the ONLY runtime writer of CONFIG.SYS. It persists brightness/rotation/volume/
  prompt/wifi-autoconnect/display-timeout plus the `OSK` and `HEADER` boot prefs through the
  owning modules' public APIs, and `config factory` deletes CONFIG.SYS, AUTOEXEC.BAT,
  WIFI.KNOWN, ALIASES.BAT, and HISTORY.TXT after a destructive confirmation.

### Lifecycle
```c
void command_init(void);           /* storage_init() + batch_init(), then registers both ops tables */
bool command_is_initialized(void);
```
- `command_init()` brings up `components/storage/` and `components/batch/` before registering
  `batch_command_ops_t` and `shell_command_ops_t`, so no dispatch can observe uninitialized state.

## Audio Module API

Declared in `components/audio/audio.h`. All audio logic lives in `components/audio/audio.c`
(codec init, speaker volume, and the background playback task); the `beep`/`tone`/`wavplay`/
`audio`/`volume` commands in `components/command/` only parse arguments and call these.
```c
esp_err_t audio_init(void);
bool audio_play_tone(int freq_hz, uint32_t duration_ms);   /* false = audio busy */
bool audio_play_wav(const char *resolved_path);            /* false = audio busy */
void audio_stop(void);
bool audio_busy(void);
esp_err_t audio_set_volume(int percent);
int  audio_get_volume(void);
```
- The codec is mono 16-bit at 22050 Hz (BSP default). Tones are generated in heap chunks with
  a short fade; WAVs (16-bit PCM mono/stereo at 22050/44100 Hz) stream from SD, stereo mixed to
  mono and 44100 decimated. Playback is background and single-slot; `audio_stop()` is also
  called by `sleep`/`deepsleep`. `command_set_volume()` / `command_get_volume_percent()` in
  command.c are thin wrappers over `audio_set_volume()` / `audio_get_volume()`.

## Storage Module API

Declared in `components/storage/storage.h` and `components/storage/storage_commands.h`.

### SD session
```c
typedef struct { bool mounted_here; } shell_sd_session_t;

esp_err_t shell_sd_begin(shell_sd_session_t *session);
void      shell_sd_end(shell_sd_session_t *session, const char *operation);
bool      storage_sd_is_mounted(void);
void      storage_register_sd_first_mount_callback(void (*callback)(void));
void      shell_command_sd_eject(void);
void      shell_command_sd_mount(void);
```
- `shell_sd_begin()` is the only mount path in the firmware. It returns `ESP_OK` when the card is
  usable, `ESP_ERR_INVALID_STATE` when the card was explicitly ejected, or the BSP mount error.
- The mount is persistent: `shell_sd_end()` deliberately does not unmount. It exists so every
  command has a symmetric cleanup point. Only `shell_command_sd_eject()` tears the mount down.
- Every `shell_sd_begin()` must have a matching `shell_sd_end()` on every return path.
- `storage_register_sd_first_mount_callback()` installs a one-shot callback invoked once per boot
  the first time the card mounts (startup, or a freshly-inserted card mounted by the first SD
  command). `main.c` wires it to `boot_on_sd_first_mount()`, which generates default boot files
  and prints the first-run welcome.
- `shell_command_sd_mount()` clears the eject latch and mounts, so a card re-inserted after
  `sdeject` works without rebooting.

### Path resolution and conversion
```c
esp_err_t   shell_sd_resolve_path(const char *input, char *output, size_t output_size);
esp_err_t   shell_fs_resolve_path(const char *input, char *output, size_t output_size);
esp_err_t   shell_resolve_target_from_source(const char *source_path, const char *target_input,
                                             char *target_path, size_t target_path_size);
esp_err_t   shell_sd_stat_path(const char *path, struct stat *st);
esp_err_t   shell_sd_fresult_to_esp_err(FRESULT result);
esp_err_t   shell_sd_vfs_to_fatfs_path(const char *vfs_path, char *fatfs_path, size_t size);
bool        shell_path_has_directory_component(const char *path);
bool        shell_path_has_extension(const char *path, const char *extension);
```
- `shell_sd_resolve_path()` resolves against the SD root; `shell_fs_resolve_path()` resolves
  against the current working directory and collapses `.` and `..`.
- `shell_sd_vfs_to_fatfs_path()` is required for listings because the direct FATFS API exposes
  long file names and the 8.3 alternate name that POSIX `dirent` does not.

### Formatting, matching, and cwd
```c
void        shell_sd_format_size(uint64_t size_bytes, char *output, size_t output_size);
const char *shell_sd_entry_type(const struct stat *st);
bool        shell_wildcard_match(const char *pattern, const char *name);
const char *shell_get_cwd(void);
void        storage_set_cwd(const char *absolute_path);
void        shell_fs_print_cwd(void);
```

### Shared file operations
```c
esp_err_t shell_fs_copy_file(const char *source_path, const char *dest_path);
esp_err_t storage_copy_attributes(const char *source_path, const char *dest_path);
esp_err_t shell_list_directory_path(const char *normalized_path);
esp_err_t shell_print_file_text(const char *normalized_path);
esp_err_t shell_write_redirect_output(const char *path, const char *text, bool append_mode);
```
- Each opens and closes its own guarded SD session.
- `shell_write_redirect_output()` is what the command pipeline calls for `>` and `>>`.
- `storage_copy_attributes()` carries R/H/S/A between two files. `shell_fs_copy_file()` calls
  it automatically after the data is written, so every copy path inherits it; call it directly
  only when copying something the shared helper does not handle, such as a directory.

### Volume capacity and guardrails
```c
typedef struct {
    uint64_t total_bytes;
    uint64_t free_bytes;
    uint64_t used_bytes;
    uint32_t cluster_bytes;
    uint32_t total_clusters;
    uint32_t free_clusters;
} storage_space_info_t;

esp_err_t storage_get_space_info(storage_space_info_t *info_out);
bool      storage_check_free_space(uint64_t needed_bytes, uint64_t reclaim_bytes,
                                   const char *operation);
bool      storage_paths_are_same(const char *left, const char *right);
uint64_t  storage_get_file_size(const char *resolved_path);
```
- `storage_get_space_info()` opens its own guarded session, so the caller does not need one.
  Backed by `f_getfree()`; handles both FATFS sector-size build configurations.
- `storage_check_free_space()` enforces `P4_CONFIG_STORAGE_FREE_MARGIN_BYTES`. Pass the size of
  a destination about to be truncated as `reclaim_bytes` so an in-place overwrite is not
  charged twice. Prints the refusal message itself and returns false. When the capacity query
  fails it returns true rather than blocking a legitimate write on a diagnostic failure.
- `storage_paths_are_same()` compares two already-resolved paths case-insensitively, matching
  FAT. Every copy-like command must call it before opening the destination for truncation.
- Any new command that writes a file must use these; see the rules in `ai-context.md`.

### Input redirection
```c
void        storage_set_input_redirect(const char *resolved_path);
const char *storage_get_input_redirect(void);
void        storage_clear_input_redirect(void);
esp_err_t   storage_resolve_input_source(const char *argument, char *output, size_t output_size);
```
- One slot serves both the `<` operator and each `|` pipe stage, so `sort < f.txt` and
  `type f.txt | sort` reach the same code path in the text-processing commands.
- `storage_resolve_input_source()` prefers an explicit filename argument and falls back to the
  pending redirection, returning `ESP_ERR_NOT_FOUND` when neither exists.
- The slot belongs to exactly one command. `shell_execute_command()` clears it after dispatch,
  including when the dispatch fails.

### DOS file commands
Declared in `storage_commands.h`; all are called directly by the dispatcher.
```c
void shell_command_cd(int argc, char **argv);
void shell_command_dir(int argc, char **argv);
void shell_command_tree(int argc, char **argv);
void shell_command_copy(int argc, char **argv);
void shell_command_move(int argc, char **argv);
int  shell_command_del(int argc, char **argv);   /* 0 ok, 1 fail/cancelled, 2 usage */
void shell_command_rename(int argc, char **argv, const char *verb);
void shell_command_mkdir(int argc, char **argv);
int  shell_command_rmdir(int argc, char **argv); /* 0 ok, 1 fail/cancelled, 2 usage */
void shell_command_type_file(int argc, char **argv);
void shell_command_write_file(int argc, char **argv, bool append_mode);
void shell_command_touch(int argc, char **argv);
void shell_command_attrib(int argc, char **argv);
void shell_command_label(int argc, char **argv);
int  shell_command_xcopy(int argc, char **argv);  /* full DOS 6.x switch set; 0/1/2 */
int  shell_command_find(int argc, char **argv);    /* text search /I /N /C /V + recursive file discovery /NAME: /SIZE: /NEWER: /OLDER: /DIRS /B; 0/1/2 */
int  shell_command_findstr(int argc, char **argv); /* literal + regex-lite, /R /C /I /N /V /X /E /B /L /S /M /F /G; 0 found / 1 none / 2 usage */
int  shell_command_more(int argc, char **argv);    /* keypress paging, Q quits; 0/1/2 */
int  shell_command_fc(int argc, char **argv);      /* 0 identical, 1 differences, 2 usage */
int  shell_command_comp(int argc, char **argv);    /* byte compare /D /A /L /N /C; 0 identical, 1 different, 2 usage */
int  shell_command_sort(int argc, char **argv);    /* /R /I /U; 0/1/2 */
void shell_command_sd(char *command);   /* receives the original unsplit line */

void shell_command_chkdsk(int argc, char **argv);  /* [path] [/F] */
int  shell_command_format(int argc, char **argv);  /* [/FS:FAT|FAT32] [/A:size] [/V:label] [/Q]; 0/1/2 */
int  shell_command_disk(char *command);            /* diskpart-style; 0/1/2 */

/* Recycle bin. `undelete` is aliased to `restore`; `trash` to `recycle`. */
int  shell_command_undelete(int argc, char **argv);   /* restore <name|index> */
int  shell_command_trash(int argc, char **argv);      /* list|info|restore|purge|empty */

/* Pure helpers exposed for the unit tests (findstr regex engine and comp
 * byte comparison). No I/O; declared in storage_commands.h. */
int  shell_fsre_search(const char *pattern, const char *text, bool icase, int *end_out);
bool shell_findstr_match_line(const char *pattern, bool is_regex, const char *line,
                              bool icase, bool beg, bool end, bool whole);
bool shell_comp_first_diff(const uint8_t *a, size_t an, const uint8_t *b, size_t bn,
                           bool icase, size_t *pos, uint8_t *va, uint8_t *vb);
```
- `shell_command_dir()` accepts `/W` `/P` `/S` `/B` `/L` `/A:attrs` `/O:order`, buffers each
  level on the heap for sorting, and closes with per-directory counts, a `/S` grand total, and
  free space.
- `shell_command_chkdsk()` is read-only: it reports capacity and, with `/F`, verifies every
  directory is readable. It never rewrites FAT structures.
- `shell_command_format()` requires the exact `P4_CONFIG_DESTRUCTIVE_CONFIRM_WORD` through the
  shell key queue and refuses when `shell_key_input_available()` is false. `/FS:` accepts
  `FAT`/`FAT32` (size-appropriate auto-selection via `esp_vfs_fat_sdcard_format_cfg`; `EXFAT`
  is not available in this build and warns, falling back to FAT32), `/A:` sets the cluster
  size, `/V:` sets the label, `/Q` is accepted for DOS familiarity.
- `shell_command_disk()` is the diskpart-style family: `list`, `detail`, `clean`,
  `create partition primary [size=N]`, `delete partition N`, `format`. It routes to the
  volume services in `storage.c`, which are parameterized by `storage_volume_t` so a future
  USB OTG MSC volume can be added without changing the command surface.
- `shell_command_xcopy()` implements the full DOS 6.x switch set and walks directory trees
  with one heap block per recursion level (`P4_CONFIG_DIR_RECURSE_DEPTH_MAX` bound), never
  re-entering itself. Overwriting prompts when interactive and falls back to silent `/Y`
  behaviour when headless, so batch files are unaffected.
- `shell_command_findstr()` and `shell_command_comp()` are the classic DOS text tools; their
  pure matcher helpers (`shell_fsre_search`, `shell_findstr_match_line`, `shell_comp_first_diff`)
  are exposed for the unit tests.
- All text tools (`find`, `findstr`, `more`, `fc`, `comp`, `sort`) and `xcopy` return an int
  ERRORLEVEL (0 ok / found / identical, 1 not found / differences, 2 usage) that the batch
  dispatcher records, so `if errorlevel` and `&&` / `||` work uniformly.
- `shell_command_del()` / `shell_command_rmdir()` move entries into the recycle bin by
  default (`/p`/`/f` delete permanently); `/s` recursion is gated by the exact confirmation
  word. `shell_command_undelete()` restores one entry; `shell_command_trash()` runs the
  `list`/`info`/`restore`/`purge`/`empty` subcommands. `del`, `rd`, `format`, `disk`,
  `undelete`, and `trash` return 0/1/2 (success/failure-cancelled/usage) that the batch
  dispatcher records as ERRORLEVEL.
- `shell_command_tree()` accepts `/F` (include files) and `/A` (ASCII connectors), recurses to
  `P4_CONFIG_TREE_DEPTH_MAX`, and buffers each directory level on the heap.
- `find`, `more`, and `sort` read the pending input-redirection source when no filename
  argument is supplied.
- All of them resolve relative paths and run inside a guarded SD session.

### Disk and partition services (storage.c)
```c
typedef enum { STORAGE_VOLUME_SD = 0 } storage_volume_t;   /* USB OTG MSC is a future target */

esp_err_t storage_disk_get_info(storage_volume_t vol, storage_disk_info_t *out);
esp_err_t storage_disk_read_mbr(storage_volume_t vol, storage_mbr_t *out);
esp_err_t storage_disk_clean(storage_volume_t vol);
esp_err_t storage_disk_create_primary_partition(storage_volume_t vol, uint64_t size_bytes);
esp_err_t storage_disk_delete_partition(storage_volume_t vol, unsigned partition_index);
esp_err_t storage_format_volume(storage_volume_t vol, const storage_format_opts_t *opts);
const char *storage_get_fat_type(void);
```
- `storage_format_volume()` uses `esp_vfs_fat_sdcard_format_cfg()` (unmount, format, remount),
  applies the requested label, and reports FAT type/geometry.
- `storage_disk_*` read and write the MBR partition table through `sdmmc_read_sectors` /
  `sdmmc_write_sectors` on the BSP card handle, unmounting the FATFS volume
  (`f_mount(NULL, "0:", 0)`) first so no stale partition cache survives.
- Every function opens its own guarded SD session; callers do not need one.

### Recycle bin (storage.c + trash.c)
Declared in `storage.h`; the `trash_*` implementations live in `components/storage/trash.c`.
```c
bool storage_trash_enabled(void);
const char *storage_trash_path(void);
esp_err_t storage_trash_delete_file(const char *resolved_path, bool permanent);
esp_err_t storage_trash_delete_pattern(const char *resolved_dir, const char *pattern,
                                       bool recursive, bool permanent, int *deleted_out);
esp_err_t storage_trash_remove_tree(const char *resolved_path, bool permanent);
esp_err_t storage_trash_restore(const char *name_or_index);
esp_err_t storage_trash_purge(const char *name_or_index);
esp_err_t storage_trash_empty(void);
esp_err_t storage_trash_list(void);
esp_err_t storage_trash_info(void);
void storage_trash_enforce_limits(void);
```
- `storage_trash_delete_file()` / `storage_trash_remove_tree()` move a file or whole tree into
  the hidden `.trash` folder (renaming to a unique `<epoch>_<name>` and writing a `.meta`
  side-car first, so a failed rename leaves the original intact). `permanent` unlinks
  instead. `storage_trash_delete_pattern()` matches a DOS wildcard, optionally recursing
  (never into `.trash` itself).
- `storage_trash_restore()` renames an entry back to the original path, recreating missing
  parent directories and returning `ESP_ERR_INVALID_STATE` rather than overwriting an
  occupied destination. Entries resolve by unique name, original path, basename, or 1-based
  index.
- `storage_trash_enforce_limits()` purges the oldest entries first when the byte / age /
  entry-count caps (`P4_CONFIG_TRASH_MAX_*`) are exceeded; every public operation calls it.
- All I/O runs inside guarded SD sessions; `ESP_ERR_NOT_SUPPORTED` is returned when the
  recycle bin is disabled and `ESP_ERR_INVALID_ARG` when a path is inside `.trash`.

### Lifecycle
```c
void storage_init(void);          /* resets cwd to the SD mount point and clears mount tracking */
bool storage_is_initialized(void);
```

### INI-style persistent state + temp files (storage_ini.c)
```c
int  storage_ini_get_value(const char *text, const char *key, char *out, size_t out_size);
bool storage_ini_upsert(char *text, size_t cap, const char *key, const char *value);
bool storage_ini_remove(char *text, size_t cap, const char *key);

esp_err_t storage_ini_file_get(const char *path, const char *key, char *value, size_t value_size);
esp_err_t storage_ini_file_set(const char *path, const char *key, const char *value);
esp_err_t storage_ini_file_delete(const char *path, const char *key);
esp_err_t storage_ini_file_foreach(const char *path,
                                   bool (*cb)(const char *key, const char *value, void *ctx),
                                   void *ctx);
esp_err_t storage_write_text_file(const char *path, const char *text);
esp_err_t storage_temp_path(char *buf, size_t size, const char *ext);
esp_err_t storage_temp_cleanup(void);
```
- Pure `KEY=VALUE` line editors on a text buffer (comment/blank aware) and
  file-level INI operations on the SD card through guarded sessions (atomic
  temp+rename writes, parent-directory creation, free-space guardrail).
- `storage_temp_path` creates a unique temp file under `sd:/tmp`;
  `storage_temp_cleanup` empties the temp directory. All storage lives on the
  SD card. The batch `ini` / `appconfig` / `temp` commands and the applib
  state group share this core; the `config` command's `config_directive_*`
  helpers are thin wrappers over `storage_ini_get_value/upsert/remove`.

## TUI Module API (`components/tui/` + `components/modal/`)

Declared in `components/tui/tui.h` (leaf: `REQUIRES shell, windows, ansi, display`). Logical `P4_CONFIG_TUI_COLS`×`P4_CONFIG_TUI_ROWS` `80×25` (`p4minishell_config.h:325`) heap cell buffer (`utf8[4]` `tui_cell_t`) mapped to the live transcript region `1024x510` via `windows_enter_tui_mode`/`windows_refresh_tui_surface`/`windows_notify_keyboard_visibility` (`components/windows/windows.c:312`). Font: extended `unscii_16` in-place (`managed_components/lvgl__lvgl/src/font/lv_font_unscii_16.c`, 384 glyphs U+2500-U+257F/U+2600-U+26FF, cmaps 3, `CONFIG_LV_FONT_UNSCII_16=y` `sdkconfig.defaults:33`).

```c
typedef struct {
    char    utf8[4];   /**< UTF-8 sequence, NUL-terminated (box chars are 3 bytes). */
    uint8_t fg;        /**< DOS 0-15 foreground (16 = default). */
    uint8_t bg;        /**< DOS 0-15 background (16 = default). */
    uint8_t attr;      /**< Bitmask: 1=bold,2=dim,4=italic,8=underline. */
} tui_cell_t;  /* components/tui/tui.h:34 */

bool tui_init(void);                          /* alloc PSRAM cells, enter windows TUI mode, clear+flush */
void tui_deinit(void);                        /* delete label, exit TUI mode, free cells */
bool tui_is_active(void);
void tui_clear(void);                         /* ED 2J with default colors via tui_cell_set */
void tui_clear_line(int mode);                /* EL 2K: 0 from cursor, 1 to cursor, 2 entire */
void tui_set_cursor(int row, int col);        /* 1-based clamped to 80×25 */
void tui_get_cursor(int *row_out, int *col_out);
void tui_save_cursor(void); void tui_restore_cursor(void); /* SCP/RCP */
void tui_set_cursor_visible(bool visible);    /* DECTCEM */
void tui_putc(char ch);                       /* at cursor advancing with wrap, \\n/\\r honored */
void tui_print_at(int col, int row, const char *text, uint8_t fg, uint8_t bg); /* UTF-8 codepoint loop */
void tui_draw_box(int x, int y, int w, int h, const char *style, uint8_t fg, uint8_t bg, const char *title);
                                             /* x,y,w,h 1-based clamped, style single/double/rounded via SH_BOX_* UTF-8 + tui_cell_set, title centered with spaces, interior cleared — window stack via nested boxes */
void tui_draw_line(int x1, int y1, int x2, int y2, const char *style, uint8_t fg, uint8_t bg);
                                             /* H/V only, style single/double/heavy via SH_BOX_H/V/H2/V2/HL/VL */
void tui_fill(int x, int y, int w, int h, char ch, uint8_t fg, uint8_t bg);
void tui_set_default_color(uint8_t fg, uint8_t bg); /* 16 = default, backs color */
void tui_get_default_color(uint8_t *fg_out, uint8_t *bg_out);
void tui_alt_enter(void); void tui_alt_leave(void); /* ESC[?1049h/l save/restore heap copy */
void tui_flush(void);                         /* coalesce per fg run, emit #RRGGBB per run via ansi_get_palette_color (PowerShell palette, no duplicate), lv_label recolor on s_tui_label */
void tui_refresh_surface(void);               /* windows_refresh_tui_surface + flush on rotation/keyboard */
void tui_enter_fullscreen(void); void tui_exit_fullscreen(void); bool tui_is_fullscreen(void);
                                             /* hide/restore header via windows_set_fullscreen/header_set_visible, keep header visible by default, dynamic keyboard scaling via windows_notify_keyboard_visibility */
```

- `tui_draw_box`/`tui_draw_line` honor style and title via `tui_cell_set` (`components/tui/tui.c:129`) with full UTF-8 `SH_BOX_*`; `tui_flush` (`components/tui/tui.c:620`) renders fg/bg via `lv_label` recolor `#RRGGBB` per fg run using `ansi_get_palette_color`.
- `tui status` (command layer) reports `transcript rect 1024x510, cols 80 rows 25, fullscreen, font unscii_16 384 glyphs`, `tui clear`/`tui fullscreen on|off`/`tui refresh` dispatch here.
- `draw` batch verb (`components/command/command.c`) auto-enters TUI via `tui_init` when no TUI/modal surface is active, otherwise reuses the active buffer; batch verbs `draw box`/`line`/`fill`/`text`/`clear`/`window`/`fullscreen` map directly to these primitives.
- Prompt fixed: `main.c:112` input line and `modal_surf.c:412` `ask` placeholder use `shell_prompt_render_plain()` (`components/shell/shell.c:412`); screenshot debug loop `grab_screenshot.py --port COM11 --out out.png --crop-transcript` + `capture_tui.py` crops to transcript rect for pixel-perfect verification.

## Batch Module API

Declared in `components/batch/batch.h`.

### Command pipeline hook
```c
typedef struct {
    void (*execute_command)(char *command);
} batch_command_ops_t;

void batch_register_command_ops(const batch_command_ops_t *ops);
```
- Registered once by `command_init()`. Passing `NULL` clears the table.
- Every nested context (`if` bodies, `for` bodies, pipe stages, batch file lines) runs through
  this hook so it inherits variable expansion and output redirection. The hook is NULL-checked;
  if it is unset the batch engine prints an explicit message instead of crashing.

### Environment and expansion
```c
const char *shell_env_get(const char *name);
esp_err_t   shell_env_set(const char *name, const char *value);
void        shell_expand_variables(const char *input, char *output, size_t output_size);
```
- Names are normalized to upper case and must be alphanumeric plus underscore. Setting an empty
  or `NULL` value clears the slot. `shell_env_set()` returns `ESP_ERR_NO_MEM` when all 24 slots
  are used.
- `shell_expand_variables()` handles `%VAR%`, `%0` (script name), `%1`..`%9` (the caller's
  arguments), `%*` (every argument from `%1` onward), `%%` → `%`, and the dynamic
  pseudo-variables `%DATE%` (`MM-DD-YYYY`), `%TIME%` (`HH:MM:SS`), `%RANDOM%`
  (`0..32767`), `%CD%` (current directory), and `%ERRORLEVEL%`. An **undefined `%VAR%`
  expands to the empty string** (cmd.exe parity), so `if "%var%"==""` works. Single-quoted
  runs stay literal and `^%` is a literal percent. With no active batch frame
  `%0`/`%1`..`%9`/`%*` expand to empty.

### Arithmetic expressions
```c
bool shell_expr_evaluate(const char *expression, int32_t *result_out, const char **error_out);

/* --- Aliases (DOSKEY-style macros) --- */
const char *shell_alias_get(const char *name);
esp_err_t shell_alias_set(const char *name, const char *value);
int shell_alias_count(void);
bool shell_alias_get_by_index(int index, char *name_out, size_t name_size,
                              char *value_out, size_t value_size);
bool batch_alias_expand_command(const char *command, char *out, size_t out_size);
void shell_command_alias(int argc, char **argv);
void shell_command_unalias(int argc, char **argv);
```
- Recursive-descent evaluator over 32-bit signed integers with cmd.exe precedence:
  `||`, `&&`, comparisons `== != < > <= >=` (each yielding 1/0), then `|`, `^`, `&`,
  `<< >>`, `+ -`, `* / %`, unary `- ~ !`, and parentheses.
- A bare identifier reads an environment variable; an undefined name evaluates to 0, as in
  DOS. Numbers accept decimal, `0x` hex, and leading-zero octal.
- Returns false with a static reason in `error_out` on divide by zero, `INT32_MIN / -1`
  overflow, unbalanced parentheses, a malformed number, or trailing characters.
- Backs `set /a`, including its compound assignment operators. The `&`/`|`/`&&`/`||` levels
  never swallow a shell chain separator; an expression using `&&`/`||`/`&`/`|`/`<`/`>` must be
  quoted at the shell level.

### Calculator (`calc` command, components/batch/calc.c)
```c
typedef struct { bool is_string; double num; char str[P4_CONFIG_CALC_STR_BYTES]; } calc_value_t;
bool calc_evaluate(const char *expression, calc_value_t *result_out, const char **error_out);
bool calc_angle_is_degrees(void);
void calc_set_angle_mode(bool degrees);
void calc_format_number(double value, char *out, size_t size);
int shell_command_calc_line(const char *line);
```
- `calc_evaluate` is a pure recursive-descent evaluator over double/fixed-string values with
  the FX-870P/VX-4 functions (ABS, SIN/COS/TAN, SINH/COSH/TANH, ASINH/ACOSH/ATANH,
  ASN/ACS/ATN, SQR, EXP, LN, LOG, FACT, NCR, NPR, INT, FIX, FRAC, ROUND, MOD, PI,
  RAN#, POL, REC, DMS/DMS$, DEG, CUR, VAL/VALF, STR$, HEX$, ASC, CHR$, LEN,
  LEFT$/MID$/RIGHT$), `&H`/`0x` hex literals, string concatenation with `+`, and
  env-var references (undefined reads as 0). `POL`/`REC` store their two results
  in the X/Y environment variables.
- `shell_command_calc_line` is the batch verb (ERRORLEVEL 0/1/2); the dispatcher feeds it the
  raw unsplit line so quoted string arguments survive the shell tokenizer.

### `for /f` helpers
```c
typedef struct { const char *start; size_t len; } shell_forf_tok_t;
typedef struct {
    char delims[P4_CONFIG_FORF_DELIMS_BYTES];
    int  token_list[P4_CONFIG_FORF_TOKEN_MAX];
    int  token_count;
    int  skip;
    char eol;
    bool star;
    bool usebackq;
} shell_forf_options_t;
void shell_forf_options_default(shell_forf_options_t *opts);
bool shell_forf_parse_options(const char *text, size_t len, shell_forf_options_t *opts);
int  shell_forf_split_line(const char *line, const char *delims,
                           shell_forf_tok_t *tokens, int max_tokens);
```
- Pure option parser / line splitter behind `for /f "eol=c skip=n delims=xyz tokens=a,b,m-n"
  %%v in (file-set) do cmd`; `tokens=` replaces the default token 1, `*` binds the rest of the
  line. Unit-tested in test/main/test_calc.c.

### Errorlevel
```c
int  batch_get_errorlevel(void);
void batch_set_errorlevel(int level);
```
- `choice` sets it to the 1-based index of the chosen key; `if errorlevel N` reads it.
- `set /a` and `set /p` set it to 1 on failure and 0 on success.

### Batch engine
```c
bool      shell_resolve_batch_path(const char *command_name, char *resolved_path, size_t size);
esp_err_t shell_execute_batch_file(const char *path, int argc, char **argv);
void      shell_execute_pipe(char *command);
```
- `shell_resolve_batch_path()` tries the literal name, then `<name>.bat`, then each
  `;`-separated PATH entry with both forms.
- `shell_execute_batch_file()` returns `ESP_ERR_INVALID_STATE` past 4 nesting levels and
  `ESP_ERR_NOT_FOUND` when the file cannot be opened. On return it unwinds any `setlocal`
  scope the file left open and resolves the pending `exit` stop mode. A file at or below
  `P4_CONFIG_BATCH_FILE_MAX_BYTES` (131072) is loaded into a PSRAM RAM image and executed via
  `shell_frame_fgets`/`tell`/`seek` (byte-for-byte `fgets` semantics), keeping `goto` loops off
  the SD card; larger files stream from the SD.
- `shell_execute_pipe()` splits on unquoted `|` into up to `P4_CONFIG_PIPE_STAGE_MAX` stages,
  spools each stage but the last to its own file, hands it to the next stage through the
  storage input-redirection slot, and removes every spool file on every exit path.

### Background task slots (`start` pool)
```c
int  batch_bg_alloc(void);                 /* claim a free slot (>= 1) or -1 */
void batch_bg_bind(int slot);              /* the worker binds itself on entry */
void batch_bg_release(int slot);           /* release when the worker exits */
void batch_bg_request_kill(int slot);      /* cooperative stop request */
bool batch_bg_kill_requested(void);        /* polled per batch line and per 100 ms of delay */
bool batch_bg_is_background(void);         /* true on a pooled worker */
```
- The main worker owns slot 0; each `start`ed job owns one of the pool (size
  `P4_CONFIG_BG_TASKS`). Each slot carries its own batch context, errorlevel, goto state,
  `setlocal` depth, and `%*` scratch (`batch_task_ctx_t`).

### Batch language commands
```c
void shell_command_set(int argc, char **argv);   /* also handles /a and /p */
void shell_command_path(int argc, char **argv);
void shell_command_echo(int argc, char **argv);
void shell_command_call(int argc, char **argv);
void shell_command_if(int argc, char **argv);
void shell_command_for(int argc, char **argv);    /* classic tokens/wildcards + for /f */
void shell_command_goto(int argc, char **argv);
void shell_command_shift(int argc, char **argv);
void shell_command_pause(int argc, char **argv);      /* blocks on a real keypress */
void shell_command_choice(int argc, char **argv);     /* /C:list /N /T:c,secs /S */
void shell_command_setlocal(int argc, char **argv);   /* pushes an environment snapshot */
void shell_command_endlocal(int argc, char **argv);   /* pops the snapshot */
void shell_command_exit(int argc, char **argv);       /* exit [/b] [code] */
int  shell_command_calc_line(const char *line);       /* `calc` (see "Calculator" above) */
```
- `pause` and `choice` use the shell core's key queue and fall back to a timed path when
  `shell_key_input_available()` is false.
- `setlocal` / `endlocal` maintain a stack of full environment snapshots up to
  `P4_CONFIG_SETLOCAL_DEPTH_MAX`. Snapshotting the whole table means a restore correctly
  reverts creations, modifications, and deletions in one step.
- `exit /b [code]` leaves one batch file; a bare `exit [code]` unwinds every nested level.
- `if` supports `[not] errorlevel N` (true when errorlevel ≥ N), `[not] exist <path>`
  (files and directories), `[/i] [not] "a"=="b"` (case-insensitive with `/i`), the cmd.exe
  numeric keywords `[not] a EQU|NEQ|LSS|LEQ|GTR|GEQ b` (operands parsed as decimal,
  non-numeric reads as 0), and `not` for each form. `goto :eof` jumps to the end of the
  current batch file, unwinding its open setlocal scopes. `call` propagates the called
  script's final errorlevel to the caller.
- `set /p NAME=< file` (and a pipe stage) reads one line from the active input-redirection
  source instead of the interactive key queue; `for /f` iterates file lines with the
  `eol=`/`skip=`/`delims=`/`tokens=` options (see "`for /f` helpers" above).

### Lifecycle
```c
void batch_init(void);          /* clears the environment, sets PATH=sd:/, resets errorlevel */
bool batch_is_initialized(void);
```

## Applib Module API (components/applib, native-app runtime)

The stable runtime surface native apps link against. Depends only on `shell`,
`clock`, `storage`, `db`, `tui`, and the FreeRTOS/heap/esp_timer IDF components; it never includes
`networking.h` (Wi-Fi state routes through `applib_net_ops_t`). The headers
are lean: each service group declares its functions in its own
`applib_*.h` (`applib_console.h`, `applib_mem.h`, `applib_time.h`, `applib_net.h`,
`applib_input.h`, `applib_state.h`, `applib_ui.h`, `applib_app.h`, `applib_env.h`,
`applib_db.h`, `applib_tui.h`), and the umbrella `applib.h` includes all of them.

### Console output (applib_console.h) (stdout = transcript = the redirection layer)
```c
int  app_printf(const char *format, ...);
int  app_vprintf(const char *format, va_list args);
int  app_printf_ansi(const char *format, ...);
int  app_vprintf_ansi(const char *format, va_list args);
void app_print_heading(const char *format, ...);
void app_print_field(const char *label, const char *format, ...);
void app_print_ok(const char *format, ...);
void app_print_error(const char *format, ...);
void app_print_warning(const char *format, ...);
void app_print_muted(const char *format, ...);
void app_print_usage(const char *format, ...);

/* Menu/form primitive: text wrapped in ANSI SGR codes (reverse, bold, color). */
int  app_print_styled(const char *sgr_codes, const char *format, ...);
```
- Output lands on the transcript, which `>`/`>>` capture around a command
  dispatch, so an app's stdout participates in redirection and pipes.

### Memory allocation policy + error reporting (applib_mem.h)
```c
void  *app_alloc(size_t size);
void  *app_calloc(size_t count, size_t size);
void  *app_realloc(void *ptr, size_t size);
char  *app_strdup(const char *s);
char  *app_strndup(const char *s, size_t n);
void   app_free(void *ptr);
void   app_report_error(const char *tag, int error, const char *format, ...);
void   app_report_warning(const char *tag, const char *format, ...);
void   app_report_info(const char *tag, const char *format, ...);
```
- Blocks of `P4_CONFIG_APPLIB_PSRAM_THRESHOLD_BYTES` or more prefer PSRAM
  (`heap_caps` SPIRAM) with an internal-heap fallback; smaller blocks use the
  internal heap. All calls return NULL on failure; `app_free` matches any
  `app_*` allocation.

### Time / timers / sleep / system information (applib_time.h)
```c
time_t    app_time(void);
struct tm app_time_local(void);
struct tm app_time_utc(void);
uint32_t  app_uptime_sec(void);
int64_t   app_now_ms(void);
void      app_delay_ms(uint32_t ms);
bool      app_time_synced(void);
void      app_uptime_formatted(char *buf, size_t buflen);
void      app_sysinfo(char *buf, size_t buflen);
```

### Networking helpers (applib_net.h) (Wi-Fi state via the registered ops table)
```c
typedef struct {
    bool (*wifi_is_connected)(void);
    bool (*wifi_get_rssi)(int *rssi_out);
    const char *(*wifi_state_string)(void);
} applib_net_ops_t;
void        applib_register_net_ops(const applib_net_ops_t *ops);
bool        app_wifi_is_connected(void);
int         app_wifi_get_rssi(void);
const char *app_wifi_state_string(void);
```
- `command_init()` registers the table with `networking_*` wrappers; every
  hook is NULL-checked, so the helpers degrade gracefully before registration.

### Input with timeout (applib_input.h)
```c
bool app_wait_key(uint32_t timeout_ms, char *key_out);
bool app_read_line(char *buf, size_t size, uint32_t timeout_ms);
bool app_read_password(char *buf, size_t size, uint32_t timeout_ms);
int  app_menu(const char *title, const char **items, int count, uint32_t timeout_ms);
```
- Bounded keypress / line reads for native apps, backed by the shell key
  queue (the primitive behind `pause` / `choice /T` / `set /p /T`). Both
  return false immediately on a headless board (no interactive key source)
  instead of stalling.
- `app_read_password` reads a line without echoing (password mode, the app
  equivalent of `set /p /P`).
- `app_menu` renders a numbered form in the transcript and returns the chosen
  1-based index (0 on cancel / timeout / invalid entry), the app-side
  equivalent of the batch `menu` command.

### Persistent state (applib_state.h)
```c
bool app_ini_get(const char *path, const char *key, char *value, size_t value_size);
bool app_ini_set(const char *path, const char *key, const char *value);
bool app_ini_delete(const char *path, const char *key);
bool app_temp_path(char *buf, size_t size, const char *ext);
bool app_temp_cleanup(void);
```
- INI-style `KEY=VALUE` config files on the SD card (`app_ini_*`) and
  SD-backed temporary files (`app_temp_*`, under `sd:/tmp`), wrapping the
  shared `storage_ini.c` core (the same mechanics the batch `ini` /
  `appconfig` / `temp` commands use). All storage lives on the SD card.

### App mode + notifications (applib_ui.h)
```c
bool app_mode_enter(bool full_screen);
bool app_mode_exit(void);
void app_notify(const char *text);
```
- `app_mode_enter` / `app_mode_exit` save the current screen, optionally hide
  the shell input widgets for a full-screen app surface, and restore the saved
  screen on exit. Wraps the shared shell app-mode primitives (the batch
  `appmode` command is its batch-side equivalent; the batch variant also
  auto-restores on `exit /b`).
- `app_notify(text)` shows `text` in the header notification area for the
  configured notify timeout. Passing `NULL` or an empty string clears the
  current notification.

### Native-app ABI (applib_app.h + applib_env.h)

A native app is a C function with the signature `app_main_t` registered via
`app_register()`. Once registered it becomes a shell command: the dispatcher
invokes it with `argc`/`argv` (`argv[0]` = the app name) after every built-in
and `.bat` lookup fails, and the return value becomes ERRORLEVEL (branchable
with `if errorlevel N` from batch). This is the native equivalent of the batch
contract (`%0..%9`/`%*`, the env table, the storage cwd).

```c
/* applib_app.h — the app entry point + registry */
typedef int (*app_main_t)(int argc, char **argv);
bool app_register(const char *name, const char *description, app_main_t entry);
bool app_find(const char *name);
bool app_dispatch(int argc, char **argv, int *errorlevel_out);
bool app_get(int index, char *name_out, size_t name_size,
             char *desc_out, size_t desc_size);

/* applib_env.h — the process environment (through a registered ops table) */
typedef struct {
    const char *(*get_env)(const char *name);
    esp_err_t   (*set_env)(const char *name, const char *value);
    const char *(*get_cwd)(void);
} applib_env_ops_t;
void applib_register_env_ops(const applib_env_ops_t *ops);

const char *app_env_get(const char *name);
bool        app_env_set(const char *name, const char *value);
const char *app_get_cwd(void);
```

- `app_register()` caps the app name at `P4_CONFIG_APP_NAME_BYTES` and the
  description at `P4_CONFIG_APP_DESC_BYTES`; the table holds up to
  `P4_CONFIG_APP_MAX` apps. Returns `false` when the table is full or the
  arguments are invalid.
- `app_dispatch()` runs the matching app and reports its return value through
  `errorlevel_out`; `command_init()` calls it from the shell dispatcher.
- `app_env_get`/`app_env_set` read and write the shell's shared RAM
  environment table (the same one `set`/`set /p`/`calc` use), and
  `app_get_cwd` returns the storage-owned current directory. They are routed
  through `applib_env_ops_t`, registered by `command_init()` with
  `shell_env_get`/`shell_env_set`/`shell_get_cwd`, so `applib` stays a leaf.
- The `apps` shell command lists the registered apps.
- Sample: `main/native_apps.c` registers `hello`, which echoes its argv and
  reads cwd + PATH.

## ANSI/VT Module API

The ANSI module (`components/ansi/`) provides SGR (Select Graphic Rendition) escape sequence processing for colored terminal output. It owns the 16-color palette, format string builder, and ANSI text processing state machine.

### Lifecycle
- `void ansi_init(void)` — Initialize the color palette from `p4minishell_config.h`. Called once during `shell_init()`.
- `bool ansi_is_initialized(void)` — Check if the ANSI module is initialized.

### Color Palette
- `uint32_t ansi_get_palette_color(ansi_color_index_t index)` — Get a palette color by index.
- `uint32_t ansi_get_default_fg(void)` / `uint32_t ansi_get_default_bg(void)` — Get default foreground/background colors.
- `void ansi_set_palette_color(ansi_color_index_t index, uint32_t color)` — Modify a palette entry at runtime.

### Format String Builder
- `int ansi_format(char *dst, size_t dst_size, const char *format, ...)` — Build an ANSI-formatted string with `@`-prefixed color/attribute specifiers.
- `int ansi_vformat(char *dst, size_t dst_size, const char *format, va_list args)` — Variadic version.

### Text Processing
- `void ansi_process_text(const char *text, ansi_segment_fn_t segment_fn, void *user_data)` — Parse ANSI escape sequences and emit plain-text segments with style state.
- `int ansi_strip_to_plain(char *dst, size_t dst_size, const char *src)` — Strip all ANSI escape sequences, returning plain text only.
- `bool ansi_contains_escapes(const char *text)` — Check if text contains ANSI escape sequences.

### Quick Formatters
- `int ansi_fg_text(char *dst, size_t dst_size, int fg_code, const char *text)` — Wrap text in foreground color SGR codes.
- `int ansi_fg_bg_text(char *dst, size_t dst_size, int fg_code, int bg_code, const char *text)` — Wrap text in foreground + background SGR codes.
- `int ansi_attr_text(char *dst, size_t dst_size, int attr_code, const char *text)` — Wrap text in attribute SGR codes.

### Semantic Palette (`ansi_palette.h`)

The single source of truth for what colour each category of shell output uses. Named
macros expand to the `@`-specifiers below, so no command picks a colour by hand.

| Macro | Category | Macro | Category |
|-------|----------|-------|----------|
| `SH_HEAD` | Section heading | `SH_VAL` | Important value |
| `SH_SUBHEAD` | Sub-heading | `SH_NUM` | Number / size / percent |
| `SH_LBL` | Field label | `SH_PATH` | Path or filename |
| `SH_TEXT` | Body text | `SH_STR` | Quoted string |
| `SH_MUTE` | Muted / timestamp | `SH_PROMPT` | Prompt accent |
| `SH_OK` / `SH_OK_HI` | Success | `SH_CMD` | Command name |
| `SH_ERR` / `SH_ERR_HI` | Error | `SH_USAGE` | Usage syntax |
| `SH_WARN` / `SH_WARN_HI` | Warning | `SH_DESC` | Help description |
| `SH_DIR` | Directory entry | `SH_NET_UP` / `SH_NET_DOWN` | Wi-Fi state |
| `SH_FILE` | File entry | `SH_BT` | Bluetooth |
| `SH_EXE` | Executable entry | `SH_USB_UP` / `SH_USB_DOWN` | USB state |
| `SH_SIZE` / `SH_TIME` | Listing size / time | `SH_OTA` | OTA accent |
| `SH_RST` | Reset | `SH_BOLD` | Bold |

The macros are plain string literals, so the header adds no dependencies and the ansi
module remains a leaf. Colours are compile-time defaults; there is no runtime theming.

**Specifier traps the palette exists to hide**: `@k` is pure black and nearly invisible
on the default background — muted text must use `@K`. `@B` is bold, not blue. `@E` is
bright red. `@L` is bright blue.

### Shell Integration
- `void shell_transcript_append_ansi(const char *text)` — Append ANSI-formatted text to transcript (strips ANSI for LVGL, passes through to UART).
- `void shell_transcript_appendf_ansi(const char *format, ...)` — Append printf-style ANSI-formatted text to transcript. Honours printf flags, width, precision, and `l`/`ll` modifiers.

### ANSI Format Specifiers
| Specifier | SGR Code | Meaning |
|-----------|----------|---------|
| `@R` | 0 | Reset all attributes |
| `@B` | 1 | Bold on |
| `@D` | 2 | Dim on |
| `@I` | 3 | Italic on |
| `@U` | 4 | Underline on |
| `@k` | 30 | Foreground black |
| `@r` | 31 | Foreground red |
| `@g` | 32 | Foreground green |
| `@y` | 33 | Foreground yellow |
| `@b` | 34 | Foreground blue |
| `@m` | 35 | Foreground magenta |
| `@c` | 36 | Foreground cyan |
| `@w` | 37 | Foreground white |
| `@K` | 90 | Foreground bright black |
| `@Rr` | 91 | Foreground bright red |
| `@G` | 92 | Foreground bright green |
| `@Y` | 93 | Foreground bright yellow |
| `@L` | 94 | Foreground bright blue |
| `@M` | 95 | Foreground bright magenta |
| `@C` | 96 | Foreground bright cyan |
| `@W` | 97 | Foreground bright white |

## Window Manager API

The window manager (`components/windows/`) is the central layout controller for the LVGL shell UI. It owns the screen region partitioning, dynamic scaling, and consistent styling. All LVGL screen-level widgets are created and owned by this module.

### Lifecycle
- `esp_err_t windows_init(void)` — Build all UI windows on the LVGL screen. Must be called after `display_init()`.
- `void windows_deinit(void)` — Tear down all windows. Calls `header_deinit()` internally.
- `bool windows_is_initialized(void)` — Check if the window manager is initialized.

### Window Object Accessors
- `lv_obj_t *windows_get_transcript(void)` — Scrollable command output (LVGL span group)
- `void windows_set_transcript_text(const char *text)` — Set transcript from ANSI text
  (parses it into per-colour spans; the rebuild is deferred to the LVGL task)
- `void windows_set_transcript_text_len(const char *text, size_t len)` — Length-aware variant used
  by the transcript append/trim paths so no `strlen`/`snprintf("%s")` scans the 64 KB buffer
- `void windows_apply_transcript_height(void)` — Rebound the transcript to its computed slot
- `void windows_scroll_transcript_to_end(void)` — Follow to the bottom only when near it
- `void windows_force_scroll_transcript_to_end(void)` — Jump to the bottom unconditionally
  (command submission); the next repaint consumes the one-shot force-follow flag
- `void windows_scroll_transcript_by(int32_t pixels)` — Scroll by a pixel delta (positive =
  toward newer output). Must run on the LVGL task.
- `void windows_scroll_transcript_to_top(void)` — Jump to the oldest retained output
- `lv_obj_t *windows_get_input_line(void)` — Single-line command entry textarea
- `lv_obj_t *windows_get_keyboard(void)` — On-screen LVGL keyboard
- `lv_obj_t *windows_get_prev_button(void)` — Previous history button
- `lv_obj_t *windows_get_next_button(void)` — Next history button
- `lv_obj_t *windows_get_scroll_up_button(void)` — Transcript scroll-up button (input row)
- `lv_obj_t *windows_get_scroll_down_button(void)` — Transcript scroll-down button (input row)
- `lv_obj_t *windows_get_input_row(void)` — Input row container
- `lv_obj_t *windows_get_screen(void)` — Active LVGL screen

### Dimension & Scaling
- `lv_coord_t windows_get_display_width(void)` — Current display width from display.c
- `lv_coord_t windows_get_display_height(void)` — Current display height from display.c
- `lv_coord_t windows_scale_height_percent(int pct, lv_coord_t min, lv_coord_t max)` — Scale height as percentage of display height
- `lv_coord_t windows_scale_width_percent(int pct, lv_coord_t min, lv_coord_t max)` — Scale width as percentage of display width
- `window_rect_t windows_get_rect(window_region_t region)` — Get bounding rectangle for a named region

### Styling
- `lv_color_t windows_get_color(const char *name)` — Get color by semantic name (`bg_screen`, `bg_transcript`, `bg_input_row`, `bg_keyboard`, `text`, `text_muted`)
- `const lv_font_t *windows_get_terminal_font(void)` — Get the terminal font

### Helpers
- `void windows_show_boot_banner(const char *message)` — Show boot message in transcript
- `void windows_reset_input_line(const char *prompt)` — Reset input line to prompt

### Thread Safety
All LVGL object creation/destruction must happen on the LVGL task. Public accessors return raw LVGL object pointers — callers must use from LVGL task context or via `lv_async_call`.

## Display API

The display manager (`components/display/`) is the central controller for all display hardware. It owns rotation, resolution, refresh rate, brightness, power state, and touch handle management. All display-related shell commands route through this module.

### Initialization & Lifecycle

- `esp_err_t display_init(void)`
  - Initialize the display manager and physical display hardware.
  - Wraps `bsp_display_start_with_config()` with `BOARD_CFG_*` values.
  - Turns on backlight by default. Returns `ESP_FAIL` if display init fails.

- `void display_deinit(void)`
  - Deinitialize the display manager (resets internal state tracking).
  - Does NOT power off the display hardware.

- `bool display_is_initialized(void)`
  - Returns true if `display_init()` completed successfully.

- `lv_display_t *display_get_lvgl_handle(void)`
  - Get the LVGL display handle for direct LVGL operations.

- `void *display_get_touch_handle(void)`
  - Get the touch handle for direct touch operations.

- `void display_register_ui_rebuild_callback(void (*rebuild_fn)(void))`
  - Register a callback invoked via `lv_async_call` after rotation changes.
  - The shell registers `shell_build_ui()` here so the display manager can trigger full UI rebuilds.

- `void display_schedule_ui_rebuild(void)`
  - Schedule the registered UI rebuild on the LVGL task via `lv_async_call`. Safe from any task
    context; used when a runtime setting that changes the layout (e.g. `config HEADER=OFF`) takes
    effect.

### Rotation Control

- `display_rotation_t display_get_rotation(void)`
  - Get the current display rotation (0, 90, 180, or 270).

- `esp_err_t display_set_rotation(display_rotation_t rotation)`
  - Apply a new display rotation. Triggers LVGL software rotation, touch controller remapping, and schedules UI rebuild via registered callback.

- `esp_err_t display_rotation_parse(const char *str, display_rotation_t *rotation_out)`
  - Parse a rotation string ("0", "90", "180", "270") into `display_rotation_t`.

- `const char *display_rotation_to_string(display_rotation_t rotation)`
  - Get the rotation as a human-readable string.

- `lv_display_rotation_t display_rotation_to_lvgl(display_rotation_t rotation)`
- `display_rotation_t display_rotation_from_lvgl(lv_display_rotation_t lvgl_rotation)`
  - Convert between display manager and LVGL rotation enums.

### Resolution

- `display_resolution_t display_get_resolution(void)`
  - Get current effective resolution accounting for rotation.

- `display_resolution_t display_get_native_resolution(void)`
  - Get native panel resolution (1024x600).

### Refresh Rate

- `display_refresh_config_t display_get_refresh_config(void)`
  - Get current refresh rate configuration (estimated ~60 Hz from panel timing).

- `esp_err_t display_set_refresh_rate(uint32_t target_hz)`
  - Set target refresh rate. Returns `ESP_ERR_NOT_SUPPORTED` on JD9165 panel (fixed timing).

### Brightness

- `int display_get_brightness(void)`
  - Get current backlight brightness percentage (0-100).

- `esp_err_t display_set_brightness(int percent)`
  - Set backlight brightness via BSP PWM path. Returns `ESP_ERR_INVALID_ARG` if out of range.

### Power Management

- `display_power_state_t display_get_power_state(void)`
  - Get current display power state (on/sleep/off).

- `esp_err_t display_set_power_state(display_power_state_t state)`
  - Set display power state. Controls backlight on/off.

- `esp_err_t display_sleep(void)` / `esp_err_t display_wake(void)`
  - Convenience functions for sleep/wake transitions.

### Display Info & Diagnostics

- `display_info_t display_get_info(void)`
  - Get comprehensive display information (resolution, rotation, refresh, brightness, timing, buffer config, panel/touch driver names).

- `void display_print_info(void (*print_fn)(const char *format, ...))`
  - Print formatted display info through a caller-provided print function (e.g., `shell_transcript_appendf`).

### Thread Safety

All display manager state is protected by a `portMUX_TYPE` spinlock. Public API functions use `portENTER_CRITICAL`/`portEXIT_CRITICAL` for atomic access. LVGL operations are dispatched via `lv_async_call` when called from non-LVGL task contexts.

## Clock API

The clock component (`components/clock/`) owns the C-library system clock, the
timezone, the SNTP client, and the `date`/`time`/`timezone`/`sntp` command
bodies. It is a leaf (shell -> clock).

### Clock core (`clock.c`)
```c
void        time_init(void);
void        time_start_sntp(void);
void        time_force_resync(void);
bool        time_is_initialized(void);
bool        time_is_synchronized(void);
struct tm   time_get_local(void);
struct tm   time_get_utc(void);
time_t      time_get_unix(void);
uint32_t    time_get_uptime_sec(void);
void        time_get_uptime_formatted(char *buf, size_t buflen);
const char *time_get_formatted(void);
const char *time_get_formatted_utc(void);
void        time_set_timezone(const char *tz_string);
const char *time_get_timezone(void);
const char *time_get_timezone_label(void);
void        time_set_utc_offset(int offset_seconds, const char *label);
const char *time_get_ntp_server(void);
```
- `time_get_formatted()` / `time_get_formatted_utc()` return `YYYY-MM-DD
  HH:MM:SS` in local / UTC time. `time_get_uptime_formatted()` renders
  `Xd HH:MM:SS` (days omitted when zero).
- `time_set_timezone()` accepts a POSIX TZ string; the local time re-renders
  immediately. `time_force_resync()` stops and restarts the SNTP client for an
  immediate exchange against `P4_CONFIG_NTP_SERVER`.
- `time_set_utc_offset(seconds, label)` is the automatic-timezone path: it
  builds the POSIX TZ string from a network-detected UTC offset (positive =
  east) and keeps `label` (an IANA name) for `time_get_timezone_label()`.
  `time_is_set()` is true after SNTP or any manual set at/after
  `P4_CONFIG_CLOCK_VALID_EPOCH`.

### Clock command surface (`clock_commands.c`)
```c
void clock_command_date(int argc, char **argv);
void clock_command_time(int argc, char **argv);
void clock_command_sntp(int argc, char **argv);
void clock_command_timezone(int argc, char **argv);
```
Dispatched from `command.c`. The bodies live in the clock component and render
through `clock_host_ops_t`, registered by `command_init()`:
```c
typedef struct {
    void (*emit_text)(const char *text);
    void (*print_heading)(const char *text);
    void (*print_field)(const char *label, const char *value);
    void (*print_ok)(const char *text);
    void (*print_error)(const char *text);
    void (*print_warning)(const char *text);
    void (*print_muted)(const char *text);
    void (*print_usage)(const char *text);
    void (*record_error)(const char *domain, esp_err_t error, const char *message);
    void (*record_warning)(const char *domain, const char *message);
    bool (*equals_ignore_case)(const char *left, const char *right);
} clock_host_ops_t;
void clock_register_host_ops(const clock_host_ops_t *ops);
```
- `clock_register_host_ops(NULL)` clears the table; every entry is NULL-checked,
  so the command surface degrades gracefully before registration.

## Header API

All `header_update_*()` functions are **safe to call from any task context** (LVGL task, shell worker, timer callback, interrupt handler). They:
1. Update internal state immediately (atomic bool/int writes)
2. Schedule an LVGL async render callback
3. Fall back to synchronous `header_render()` if async dispatch fails or allocation fails

- `void header_init(void)`
  - Call once after LVGL is ready and before the transcript widgets are created.
  - Builds the fixed non-scrollable top bar, scales its height from the active display resolution, and places status icons left-to-right with the notification area in the center.
  - System panel (MEM | CPU | BAT) is on the far right, all dynamically linked to FreeRTOS runtime stats.

- `void header_set_visible(bool visible)` / `bool header_get_visible(void)`
  - Show or hide the whole header bar. The flag survives `header_deinit()`/`header_init()`,
    so a bar hidden by `config HEADER=OFF` stays hidden across a UI rebuild. The window manager
    reports a zero-height header region while hidden (transcript expands). The caller schedules
    the relayout via `display_schedule_ui_rebuild()` after a runtime toggle.

- `void header_update_status(void)`
  - Request a header re-render from the currently cached state.
  - Useful after a batch of `header_update_*` calls when the caller wants one final refresh point.

- `void header_notify(header_notify_level_t level, const char *text, uint32_t timeout_ms)`
  - Queue a notification in the center of the fixed header with "!" icon prefix.
  - Notifications are shown FIFO (depth `P4_CONFIG_HEADER_NOTIFY_QUEUE`); a busy
    center queues a new alert instead of dropping it, and overflow drops the
    oldest *queued* entry. An empty `text` flushes the queue and clears the center
    (`notify -`); `timeout_ms == 0` shows the message until the next notification.
  - `level` drives the color and icon: `HEADER_NOTIFY_INFO` (normal),
    `WARN` (amber), `ERR` (red).
  - The queue mechanics are pure and unit-tested in `header_notify_queue.c`.
  - Uses LVGL async dispatch so callers can invoke it from shell worker tasks or other non-LVGL contexts.

- `void header_set_notification(const char *text, uint32_t timeout_ms)`
  - Thin INFO wrapper over `header_notify()` kept for the original callers.

- `void header_update_batch(const header_batch_t *in)`
  - Set every header field at once (Wi-Fi, battery, BT, USB, SD, heap, CPU,
    task count, uptime, `clock_text`, `c6ota_busy`, `bg_jobs_running`) and
    schedule exactly one async render. `in` is copied (including `clock_text`),
    so the caller's buffers may be transient.
  - `clock_text` (e.g. `"14:05"` or `"--:--"`) is shown, muted, in the center
    when no notification is displayed. `c6ota_busy`/`bg_jobs_running` drive the
    conditional `A` activity indicator.

- `void header_update_wifi(bool connected, int rssi)`
  - Update the Wi-Fi status indicator. Words style: `WiFi HI/MID/LOW/WEAK/OFF`.
    Glyph style: `W` colored by tone. Classification and thresholds live once in
    `components/header/header_status.c`.
  - State set immediately; render happens via async dispatch or direct fallback.

- `void header_update_battery(int percent, bool adc_ready)`
  - Update the battery icon, bar, and percentage label. Clamped to 0-100.
  - When `adc_ready` is false, shows a muted `B`/`BAT N/C` (battery always visible).
  - State set immediately; render happens via async dispatch or direct fallback.

- `void header_update_bluetooth(bool enabled, bool connected)`
  - Update the Bluetooth indicator (words `BT ON/IDLE/OFF`; glyph `BT`:
    green connected, amber ready/idle, red off).
  - State set immediately; render happens via async dispatch or direct fallback.

- `void header_update_usb(bool connected)`
  - Update the USB indicator (words `USB ON/OFF`; glyph `U` green/red).
  - State set immediately; render happens via async dispatch or direct fallback.

- `void header_update_sd(header_sd_state_t state)`
  - Update the SD indicator with persistent state (words `SD NO/INS/ON/ERR`;
    glyph `S`: green mounted, amber inserted, red error, muted none).
  - State set immediately; render happens via async dispatch or direct fallback.

- `void header_register_status_action(header_status_action_cb_t cb)`
  - Register the long-press handler for a status indicator. The callback receives
    a `header_status_kind_t` and is expected to run the matching full status
    command; the header stays a passive leaf with no command knowledge. Tapping
    an indicator shows a one-line detail in the notification area without calling
    this hook. Safe before `header_init()`; `NULL` clears it.

- `const char *header_status_glyph(header_status_kind_t kind)` /
  `header_tone_t header_status_*_tone(...)` (`components/header/header_status.h`)
  - Pure, LVGL-free classification: the compact glyph and the semantic tone
    (muted/ok/warn/err) for every indicator. `header.c` maps the tone onto the
    active theme color. This is the single source of the Wi-Fi label and of the
    memory/CPU/battery healthy-warn-critical thresholds, and it is unit-tested.

- `void header_update_mem(uint32_t free_heap_bytes, uint32_t total_heap_bytes)`
  - Update the memory display (MEM/MEM LOW + formatted size) from real-time FreeRTOS heap stats.
  - State set immediately; render happens via async dispatch or direct fallback.

- `void header_update_cpu(int cpu_percent, uint32_t task_count)`
  - Update the CPU usage bar and percentage label from real-time FreeRTOS runtime stats.
  - Clamped 0-100; warning color above P4_CONFIG_HEADER_CPU_WARN_PCT (85%).
  - State set immediately; render happens via async dispatch or direct fallback.

- `void header_update_uptime(uint32_t uptime_seconds)`
  - Update the uptime counter (used internally for formatting).
  - State set immediately; render happens via async dispatch or direct fallback.

- `void header_force_render(void)`
  - Force a synchronous header re-render. Call only from LVGL task context.

- `void header_deinit(void)`
  - Deinitialize the header bar, releasing all widgets and resetting state.
  - Call before rebuilding the UI after display rotation or resolution change.
  - Must be called from LVGL task context only.

- `void (*notify_header)(const char *text, uint32_t timeout_ms)` inside `networking_host_ops_t`
  - Optional host callback used by the networking module to surface live Wi-Fi notices directly in the fixed header without moving header ownership into `components/networking`.

## Host callback surface
`components/networking` and `components/bluetooth` share the same host callback table:

- `typedef struct networking_host_ops_t`
  - `void (*transcript_append_text)(const char *text)`
  - `void (*schedule_transcript_append_text)(const char *text)`
  - `void (*record_error)(const char *tag, esp_err_t error, const char *message)`
  - `void (*record_warning)(const char *tag, const char *message)`
  - `void (*record_info)(const char *tag, const char *message)`

`main/main.c` fills this table once and passes it to `networking_init(...)`. `components/networking` stores it and forwards the same callbacks into `bluetooth_init(...)`.

## Networking API
- `void networking_init(const networking_host_ops_t *ops)`
  - Registers the shared shell callback surface.
  - Initializes hosted Bluetooth through `bluetooth_init(...)`.
  - Starts the normal boot-time Wi-Fi restore flow once.

- `esp_err_t networking_handle_wifi_command(char *command)`
  - Entry point for shell-level `wifi ...` command dispatch. Returns `esp_err_t`
    so the command layer maps it onto ERRORLEVEL: ESP_OK = success,
    ESP_ERR_NOT_FOUND = known-list unavailable (no SD card or file),
    ESP_ERR_INVALID_ARG = usage, other = I/O error.

- `void networking_wifi_status(void)`
  - Prints the colour-coded association + IP report (SSID, BSSID, channel, RSSI,
    PHY mode, IPv4, netmask, gateway, DNS, association uptime).
- `void networking_wifi_scan(bool bare)`
  - Prints the RSSI-sorted scan report. `bare=true` emits uncoloured SSID lines
    for redirection / pipes.
- `void networking_wifi_diag(void)`
- `void networking_wifi_disconnect(void)`
  - Shell-facing Wi-Fi helpers used when the parser wants explicit subcommand entry points.

#- `void networking_wifi_set_boot_autoconnect(bool enabled)` /
  `bool networking_wifi_get_boot_autoconnect(void)`
  - Set/read whether the Wi-Fi watchdog retries the stored target SSID (the CONFIG.SYS
    `WIFI_AUTOCONNECT=` policy and the `config WIFI_AUTOCONNECT` setting).

## Persistent known networks (`wifi_known.h`)
```c
void networking_wifi_known_init(const networking_host_ops_t *ops);
esp_err_t networking_wifi_known_load(void);
esp_err_t networking_wifi_known_save(void);
int networking_wifi_known_count(void);
bool networking_wifi_known_get(int index, networking_wifi_known_entry_t *out);
esp_err_t networking_wifi_known_upsert(const char *ssid, const char *password,
                                       int authmode, bool mark_connected);
esp_err_t networking_wifi_known_remove(const char *ssid);
esp_err_t networking_wifi_known_clear(void);
esp_err_t networking_wifi_known_set_preferred(const char *ssid, bool preferred);
int networking_wifi_known_pick_visible(const wifi_ap_record_t *records, uint16_t count);
esp_err_t networking_wifi_known_list(void);
void networking_wifi_known_record_connect(const char *ssid, const char *password);
```
- Loads / saves the known-network list on the SD card (`P4_CONFIG_WIFI_KNOWN_FILE`)
  through the guarded storage session API. Every operation is failure-tolerant:
  no card / mount failure / missing or corrupt file / read-only or full disk
  degrades to an empty list or a non-OK return, never a crash, freeze, or hang.
  The file is written atomically (temp + rename, with remove-and-retry for the
  FATFS overwrite case) with the storage free-space pre-check and partial-file
  cleanup. Passwords are stored in the file but never printed.
- `networking_wifi_known_pick_visible()` selects the best visible known network
  (preferred / highest priority / strongest RSSI) for boot auto-connect.

- `esp_err_t networking_wifi_ping(const char *host, int count)`
  - Classic ICMP echo over the lwIP `esp_ping` session. `count <= 0` selects the
    default (4); values above the hard cap (10) are clamped. Returns ESP_OK when at
    least one reply was received so the dispatcher can map it onto ERRORLEVEL.
- `esp_err_t networking_wifi_dns_lookup(const char *hostname)`
  - Resolves A records through lwIP `getaddrinfo` and prints the IPv4 list.
    Returns ESP_OK when at least one record resolved.

- `esp_err_t networking_http_get(const char *url, networking_http_result_t *result)`
- `void networking_http_result_free(networking_http_result_t *result)`
  - The `httpget` / `wget` engine: a simple HTTPS or HTTP GET over the same
    `esp_http_client` stack c6ota uses. Buffers the body in PSRAM up to
    `P4_CONFIG_HTTP_MAX_BODY_BYTES`, follows redirects per config, prints the
    response header with semantic colours, and returns the body for the
    command layer to print or save to SD. Returns ESP_OK on an HTTP 2xx;
    the caller maps the return value onto ERRORLEVEL. A `user:pass@` URL
    prefix enables HTTP Basic auth. An internal quiet mode suppresses the
    transcript output for background callers.

- `esp_err_t networking_time_detect(int *offset_seconds_out, char *iana_out, size_t iana_size)`
  - Auto-detects the local timezone over the network (no hardcoded zone) by
    quietly fetching `P4_CONFIG_TIMEZONE_URL` and parsing the IANA name + UTC
    offset (seconds, positive = east). Must run with Wi-Fi connected and must
    NOT run on the LVGL task (it blocks up to the HTTP timeout). Drives the
    clock's `time_set_utc_offset()` from the command layer's `timesync` task.

- `esp_err_t networking_httpd_start(void)`
- `esp_err_t networking_httpd_stop(void)`
- `void networking_httpd_status(void)`
- `bool networking_httpd_is_running(void)`
- `void networking_httpd_maybe_autostart(void)`
- `void networking_httpd_maybe_stop(void)`
  - The `httpd` file server: serves the SD card over the Wi-Fi link with the
    `esp_http_server` driver (directory listings, file streaming, optional
    Basic auth, path-traversal rejection). Lifecycle is tied to Wi-Fi events
    via `maybe_autostart()` / `maybe_stop()`; `start` refuses without a
    connection. All limits come from `P4_CONFIG_HTTPD_*`.

- `void networking_netstat(void)`
- `void networking_ipconfig(void)`
  - The `netstat` / `ipconfig` diagnostics: read lwIP state (interfaces, DNS
    servers, TCP/UDP PCBs) read-only under the TCP/IP core lock and print the
    report to the transcript. Redirectable and pipable like every command.

- `const char *networking_wifi_state_string(void)`
- `networking_wifi_state_t networking_wifi_state(void)`
- `esp_err_t networking_wifi_last_error(void)`
- `bool networking_wifi_is_connected(void)`
- `bool networking_wifi_get_rssi(int *rssi_out)`
- `void networking_append_sysinfo_summary(void)`
  - Status helpers used by the shell, the header status bar, `sysinfo`, and `debug`.
  - `networking_wifi_get_rssi()` returns false when not associated or the query fails,
    leaving `*rssi_out` untouched. It exists so no consumer needs to call
    `esp_wifi_sta_get_ap_info()` directly.

### Ownership rule

Every `esp_hosted_*`, `esp_wifi_*`, `esp_netif_*`, NimBLE, lwIP-connectivity (ping / DNS),
and esp_http_client / esp_http_server / mbedTLS / TLS call in the firmware lives inside
`components/networking/` (`http_server.c` owns the server, `netdiag.c` owns the lwIP
diagnostics). The sanctioned exceptions are `components/c6ota/`, which drives
`esp_hosted_slave_ota_*` and its own esp_http_client download because co-processor firmware
update is its entire purpose. Anything else that needs networking state uses the accessors
above; if a needed value is missing, add an accessor rather than reaching into the driver.

Only the official Espressif path is used: `espressif/esp_hosted` for the transport and
`espressif/esp_wifi_remote` for the Wi-Fi API.

### Initialization order

`networking_init()` starts a background task that runs this sequence. The order is
load-bearing and documented in `networking.h`:

```
esp_hosted_init()
  -> esp_hosted_connect_to_slave()
  -> version compatibility gate
  -> nvs_flash_init()            (erase-and-retry on a corrupt partition)
  -> esp_netif_init()
  -> esp_event_loop_create_default()
  -> esp_netif_create_default_wifi_sta()
  -> esp_wifi_init()             (via esp_wifi_remote)
  -> event handler registration
  -> esp_wifi_set_mode(WIFI_MODE_STA)
  -> esp_wifi_start()
```

Hosted NimBLE is initialized separately and lazily by `bluetooth.c`. Station-only is a
hard constraint enforced both in code and by disabling SoftAP in Kconfig.

- `esp_err_t networking_wifi_wait_for_ota(void)`
- `bool networking_wifi_is_starting(void)`
- `void networking_wifi_capture_restore_state(networking_wifi_restore_state_t *restore_state)`
- `esp_err_t networking_wifi_shutdown(void)`
- `esp_err_t networking_wifi_restore_after_ota_failure(const networking_wifi_restore_state_t *restore_state)`
- `void networking_wifi_request_post_ota_restore(const networking_wifi_restore_state_t *restore_state)`
  - OTA-support helpers consumed by `components/c6ota`.

## Bluetooth API
- `void bluetooth_init(const networking_host_ops_t *ops)`
  - Receives the same host callback surface used by networking.
  - Keeps Bluetooth transcript and debug behavior aligned with the Wi-Fi module.

- `void bluetooth_handle_command(char *command)`
  - Entry point for shell-level `bluetooth ...` command dispatch.

- `void bluetooth_status(void)`
- `void bluetooth_scan(int limit)`
  - Bounded scan (P4_CONFIG_BT_SCAN_DURATION_MS); prints devices sorted by RSSI.
    `limit <= 0` selects the configured default cap.
- `void bluetooth_advertise(bool enable, const char *name)`
  - `name` is a session-only advertising name (RAM-only, never persisted); NULL
    or empty falls back to the configured default device name.
  - Focused Bluetooth helpers for shell subcommands.

- `bool bluetooth_is_enabled(void)`
- `bool bluetooth_is_connected(void)`
  - Read-only state helpers used by the header status refresh in `components/shell/` to show hosted BLE readiness without moving Bluetooth ownership out of this module.

## LED API
- `void led_init(void)`
  - Creates the WS2812 strip on `P4_CONFIG_LED_GPIO` (espressif/led_strip over RMT) and
    starts the animation task. Idempotent; on failure every API returns
    `ESP_ERR_INVALID_STATE`. Called early from `main.c` (before boot scripting so a
    CONFIG.SYS `RGB=` directive can drive the LED).
- `bool led_is_initialized(void)`
- `esp_err_t led_set_color(uint8_t red, uint8_t green, uint8_t blue)`
- `esp_err_t led_set_hex(uint32_t rgb)` — packed `0xRRGGBB`.
- `esp_err_t led_off(void)`
- `esp_err_t led_set_effect(led_effect_t effect, uint8_t speed)` — rainbow/breath/pulse/blink.
- `esp_err_t led_set_auto_status(bool enable)`
- `void led_get_state(led_state_t *out)`
- `void led_notify(led_event_t event)` — push a transient event colour; Wi-Fi state events
  become the persistent status colour in auto mode.
- `led_effect_t led_effect_from_name(const char *name)` / `const char *led_effect_name(...)`
  - The `rgb` shell command (in `components/command/command.c`) and the CONFIG.SYS `RGB=`
    directive drive the LED through this API. `networking` pushes Wi-Fi/HTTP events via
    `led_notify()`; `main.c` fires the boot confirmation flash.

## USB API
- `void usb_init(void)`
  - Call once during boot after `networking_init(...)` so the USB module can install the shared host library and both class drivers.

- `void usb_handle_command(char *command)`
  - Entry point for shell-level `usb ...` command dispatch.
  - Preserves the same transcript-first behavior used by the other modular shell families.

- `void usb_status(void)`
  - Prints transcript-visible host, MSC, and HID state.

- `void usb_msc_mount(void)`
  - Mounts a connected MSC device at `/usb0` with `msc_host_vfs_register(...)`.

- `void usb_msc_ls(const char *path)`
  - Lists files from `usb:/...`, `/usb0/...`, or a relative USB-root path.
  - Keeps directory output bounded and user-facing so it matches the existing SD command feel.

- `void usb_hid_keyboard_enable(void)`
- `void usb_hid_keyboard_disable(void)`
- `void usb_hid_mouse_enable(void)`
- `void usb_hid_mouse_disable(void)`
  - Toggle transcript echo for attached HID boot devices without changing the rest of the shell input path.

- `bool usb_is_connected(void)`
- `bool usb_is_mounted(void)`
- `bool usb_is_keyboard_attached(void)`
- `bool usb_is_mouse_attached(void)`
  - Read-only state helpers used by the header status refresh in `components/shell/`.

- `void usb_register_keyboard_input_callback(usb_keyboard_input_cb_t cb)`
  - Register a callback to receive USB keyboard input events (press/release with key code and modifiers).
  - The shell registers `shell_usb_keyboard_input()` here for CLI injection.

- `bool usb_key_to_ascii_full(uint8_t key_code, uint8_t modifiers, char *out)`
  - Convert a USB HID key code and modifiers to an ASCII character.
  - Supports full US keyboard layout: letters, numbers, symbols, keypad, with modifier-aware shifted characters.

- `const char *usb_key_name_full(uint8_t key_code)`
  - Get a human-readable name for any USB HID key code (F1-F12, arrows, navigation, etc.).

## Keyboard API (External Input Mode)

- `void keyboard_set_external_input(bool enabled)`
  - Enable/disable external input mode. When enabled, on-screen keyboard auto-hides.
  - Used by the shell when a USB keyboard is detected/removed.

- `bool keyboard_is_external_input_enabled(void)`
  - Query whether external input mode is active.

- `void keyboard_force_visible(void)`
  - Force on-screen keyboard to stay visible even when external input is enabled.

- `void keyboard_clear_force_visible(void)`
  - Clear force-visible override; re-evaluate auto-hide based on external input state.

## Editor API (components/editor)

The `edit` command is a modal surface: the LVGL view owns the transcript
region while open, and the worker task owns file I/O. All `editor_doc_*`
mutators run on the LVGL task; only load/save touch the filesystem.

### Document lifecycle
```c
editor_doc_t *editor_doc_new(const char *path);   /* "" = unnamed */
editor_doc_t *editor_doc_load(const char *path);   /* NULL on failure */
void          editor_doc_free(editor_doc_t *doc);
void          editor_doc_pick_syntax(editor_doc_t *doc);
bool          editor_file_missing(const char *resolved_path);
esp_err_t     editor_doc_save(editor_doc_t *doc, const char *path);
void          editor_doc_set_path(editor_doc_t *doc, const char *path);
```
- `editor_doc_load` refuses (returns NULL) files over
  `P4_CONFIG_EDITOR_MAX_BYTES` / `P4_CONFIG_EDITOR_MAX_LINES`; callers must
  treat a NULL on an *existing* file as a hard error (see `editor_file_missing`)
  rather than opening an empty buffer.
- `editor_doc_save` writes through a guarded SD session and removes its
  partial destination on failure; CRLF/LF and a trailing newline round-trip
  exactly.

### Accessors and cursor
```c
size_t  editor_doc_line_count(const editor_doc_t *doc);
size_t  editor_doc_line_length(const editor_doc_t *doc, size_t row);
const char *editor_doc_line_text(const editor_doc_t *doc, size_t row);
size_t  editor_doc_cursor_row(const editor_doc_t *doc);
size_t  editor_doc_cursor_col(const editor_doc_t *doc);
bool    editor_doc_is_modified(const editor_doc_t *doc);
```
Cursor movement: `editor_doc_cursor_left/right/up/down/home/end`,
`editor_doc_cursor_word_left/right`, `editor_doc_cursor_doc_home/end`,
`editor_doc_toggle_overwrite`.

### Editing
```c
void editor_doc_insert_char(editor_doc_t *doc, char ch);
void editor_doc_insert_bytes(editor_doc_t *doc, const char *bytes, size_t len);
void editor_doc_newline(editor_doc_t *doc);
void editor_doc_backspace(editor_doc_t *doc);
void editor_doc_delete(editor_doc_t *doc);
void editor_doc_tab(editor_doc_t *doc);
void editor_doc_delete_line(editor_doc_t *doc);
void editor_doc_delete_to_eol(editor_doc_t *doc);
```
- `editor_doc_insert_bytes` is the raw, no-undo, never-overwrite path (paste);
  mutators that want undo call `editor_doc_undo_mark()` first or record their
  own snapshot internally.

### Selection
```c
void editor_doc_selection_begin(editor_doc_t *doc);
void editor_doc_selection_extend(editor_doc_t *doc);
bool editor_doc_has_selection(const editor_doc_t *doc);
void editor_doc_selection_clear(editor_doc_t *doc);
void editor_doc_selection_bounds(const editor_doc_t *doc, ...);
bool editor_doc_selection_copy(const editor_doc_t *doc);
void editor_doc_selection_delete(editor_doc_t *doc);
bool editor_doc_selection_cut(editor_doc_t *doc);
void editor_doc_select_all(editor_doc_t *doc);
void editor_doc_paste(editor_doc_t *doc);
```
- Copy/cut use the shell's RAM clipboard (`shell_clipboard_*`), so editor and
  shell share one clipboard. Multi-line selections copy with the file's own
  EOL style.

### Undo / Redo
```c
void editor_doc_undo_mark(editor_doc_t *doc);  /* snapshot before an edit */
void editor_doc_undo(editor_doc_t *doc);
void editor_doc_redo(editor_doc_t *doc);
```

### Find / Replace (pure)
```c
bool editor_doc_find_next(const editor_doc_t *doc, const char *needle,
                          size_t needle_len, size_t start_row, size_t start_col,
                          bool case_sensitive, bool wrap, size_t *out_row, size_t *out_col);
bool editor_doc_replace_next(editor_doc_t *doc, const char *needle,
                             size_t needle_len, const char *replacement,
                             size_t repl_len, bool case_sensitive,
                             size_t *out_row, size_t *out_col);
```
- `find_next` never mutates the document; `replace_next` replaces one match
  per call from the cursor and leaves the caret past the replacement so a
  repeated call walks the file.

### Batch lexer
```c
size_t editor_lex_batch(const char *text, size_t len,
                        editor_syntax_run_t *runs, size_t capacity);
```

### Session (worker task)
```c
esp_err_t editor_session_run(const char *path, int *errorlevel);
bool      editor_session_is_active(void);
```
- `editor_session_run` blocks the worker until the user quits; the view
  signals saves/quits through the `editor_control_t` event group.
- `editor_control_t.save_as_path` carries a Save-As target: when non-empty the
  worker saves there and re-titles the document.

### View (LVGL task)
```c
bool editor_view_open(editor_doc_t *doc, editor_control_t *control);
void editor_view_close(void);
bool editor_view_is_open(void);
bool editor_view_handle_usb_key(uint8_t key_code, uint8_t modifiers, char ascii);
bool editor_view_handle_osk(const char *label);
void editor_view_notify_saved(bool ok);
void editor_view_scroll_by(int32_t pixels);
void editor_view_set_quit_requested(void);
void editor_view_set_save_requested(void);
```

### Shell bridge hooks (in `shell_command_ops_t`)
The shell core and UART reader reach any active native modal surface through
three NULL-checked hooks registered by `command_init()`. The editor, `dialog`,
`list`, and `ask` all route through this shared layer:
```c
bool (*modal_is_active)(void);
bool (*modal_handle_usb_key)(uint8_t key_code, uint8_t modifiers, char ascii);
bool (*modal_handle_serial_line)(const char *line);
```
- `modal_handle_serial_line` returns true when the line was consumed so serial
  text is never misrouted to the command dispatcher while a modal surface is
  open.

## Modal runtime and native surfaces (`components/modal/`)

Shared session loop + input routing for native modal surfaces that take over
the shell display area.

```c
/* Runtime */
esp_err_t modal_surface_run(const modal_surface_t *surface, void *ctx, int *errorlevel);
bool modal_runtime_is_active(void);
const char *modal_runtime_active_name(void);
bool modal_runtime_handle_usb_key(uint8_t key_code, uint8_t modifiers, char ascii);
bool modal_runtime_handle_serial_line(const char *line);

/* Ready-made surfaces for batch apps */
int modal_dialog_run(const char *title, const char *message,
                     const char *button1, const char *button2,
                     uint32_t timeout_ms);
int modal_list_run(const char *title, const char **items, int count,
                   uint32_t timeout_ms);
int modal_ask_run(const char *prompt, const char *default_text, bool password,
                  uint32_t timeout_ms, char *result, size_t result_size);
```

- `modal_dialog_run` — message box with up to two buttons. Returns `0` for
  `button1`, `1` for `button2`, `-1` for cancel/Esc/timeout.
- `modal_list_run` — scrollable list selector. Returns the 0-based selected
  index or `-1` for cancel/timeout.
- `modal_ask_run` — text prompt with on-screen-keyboard and USB-keyboard
  support. `password` masks the input when true. Writes the answer into
  `result` and returns `0` on OK, `-1` on cancel/timeout.
- All three take a `timeout_ms` (0 = wait forever); on timeout they close as
  a cancel.

## TUI Module API (`components/tui/` + `components/modal/`)

The TUI is an 80x25 heap cell buffer rendered into the live transcript region. Full
primitive list appears in the earlier "TUI Module API" section; the current surface is:

```c
/* Cell buffer + primitives (components/tui/tui.h) */
bool tui_init(void); void tui_deinit(void); bool tui_is_active(void);
void tui_clear(void); void tui_clear_line(int mode);
void tui_set_cursor(int row, int col); void tui_get_cursor(int *row, int *col);
void tui_putc(char ch); void tui_print_at(int col, int row, const char *text, uint8_t fg, uint8_t bg);
void tui_draw_box(int x,int y,int w,int h,const char *style,uint8_t fg,uint8_t bg,const char *title);
void tui_draw_line(int x1,int y1,int x2,int y2,const char *style,uint8_t fg,uint8_t bg);
void tui_fill(int x,int y,int w,int h,char ch,uint8_t fg,uint8_t bg);
void tui_draw_bar(int x,int y,int w,int pct,char fill_ch,char empty_ch,uint8_t fg,uint8_t bg);
void tui_draw_image(int x,int y,int w,int h,const gfx_surface_t *img);  /* nearest-DOS-color cells */

void tui_draw_table(int x,int y,int ncols,const int *widths,int nrows,
                    const char *const *cells,bool header,uint8_t fg,uint8_t bg);
void tui_draw_table_ex(int x,int y,int ncols,const int *widths,int nrows,
                       const char *const *cells,bool header,uint8_t fg,uint8_t bg,
                       int cursor_data_row,uint32_t sel_mask);
int  tui_table_total_width(int ncols, const int *widths);
int  tui_table_parse_cursor(const char *s, int n_data_rows);
uint32_t tui_table_parse_sel(const char *s, int n_data_rows);
uint8_t tui_rgb_to_dos(uint32_t rgb); uint32_t tui_dos_color_rgb(uint8_t index);
void tui_flush(void); void tui_refresh_surface(void);
void tui_enter_fullscreen(void); void tui_exit_fullscreen(void); bool tui_is_fullscreen(void);

/* Modal surfaces (components/modal/modal_surf.h) - seven on one shared runtime */
int modal_dialog_run(const char *title, const char *message,
                     const char *button1, const char *button2, uint32_t timeout_ms);
int modal_list_run(const char *title, const char **items, int count, uint32_t timeout_ms);
int modal_ask_run(const char *prompt, const char *default_text, bool password,
                  uint32_t timeout_ms, char *result, size_t result_size);
int modal_filebrowser_run(const char *path, uint32_t timeout_ms, char *result, size_t result_size);
int modal_viewer_run(const char *title, const char *path, uint32_t timeout_ms);
int modal_viewer_run_raw(const char *title, const char *path, uint32_t timeout_ms, bool raw);
int modal_image_run(const char *title, const char *path, uint32_t timeout_ms, bool fit);
int modal_hexview_run(const char *title, const char *path, uint32_t timeout_ms);
```

- The `draw`/`tui` verbs live in `components/command/tui_commands.c`; the modal verbs in the same
  file route through `components/modal/modal_surf.c`.
- `draw table` and `draw list` map to `tui_draw_table_ex()` with the cursor/selection helpers.
- `components/applib/applib_tui.h` is now only a thin re-export of `tui.h`; there is no separate
  `tui_create` API.

## Shell USB Keyboard Bridge

- `void shell_usb_keyboard_input(uint8_t key_code, uint8_t modifiers, bool pressed)`
  - Handle a USB keyboard input event for CLI injection.
  - Dispatches to LVGL task via `lv_async_call` for safe input line manipulation.
  - Supports: printable characters, Enter (submit), Backspace, ESC (clear), Tab, arrows (cursor/history), Delete, Home, End.

## C6 OTA API
- `void c6ota_init(void)`
  - Call once during boot after the shell transcript path is ready.
  - Resets module-owned OTA state, including pending confirmation and in-progress tracking.

- `void c6ota_perform(const char *source)`
  - Starts the shell-visible `c6ota` flow for `sd:/...`, `/sdcard/...`, `http://...`, `https://...`, or `default`.
  - Preserves the established behavior: source validation, factory warning, exact YES confirmation text, background OTA execution, Wi-Fi stop or restore handling, live progress output, and final success or failure reporting.
  - Passing `NULL` or an unsupported source emits the existing usage text instead of changing shell behavior.

- `void c6ota_register_progress_callback(void (*cb)(int percent, const char *msg))`
  - Registers an optional callback that receives transcript-formatted OTA messages from the module.
  - `percent` is `0..100` for live progress updates.
  - Negative `percent` values are reserved for non-progress shell integration messages so the shell can preserve the same synchronous or asynchronous transcript behavior after the refactor.

- `bool c6ota_try_handle_input(const char *input)`
- `bool c6ota_is_busy(void)`
- `bool c6ota_is_confirmation_pending(void)`
  - Shell-integration helpers used by `components/command/` (confirmation flow) and `components/shell/` (`sysinfo` state) to keep those concerns outside the OTA implementation details.

## Behavioral contract
- Public shell command surfaces remain `wifi ...`, `bluetooth ...`, `usb ...`, and `c6ota <sd:/path/to/firmware.bin|http[s]://host/path.bin|default>`.
- Wi-Fi and Bluetooth continue to report user-facing state through the shared transcript and debug hooks.
- USB continues that same transcript-first contract and mounts MSC storage at `/usb0` instead of changing the existing SD path.
- OTA confirmation text remains `WARNING: This will reboot the C6. Type YES to continue`.
- OTA success text remains `C6 OTA completed successfully! Type reboot to activate new firmware.`.
- OTA progress text remains `C6 OTA: XX% (YYYY KB / ZZZZ KB)`.
- OTA continues to validate ESP-IDF app-image magic `0xE9` and ESP32-C6 chip ID `0x000D` before transfer.