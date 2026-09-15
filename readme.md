# P4MiniShell

<img src="icon/icon-512.png" alt="P4MiniShell icon" width="256">

Embedded DOS-style command shell for the ESP32-P4 host with ESP32-C6 co-processor over ESP-Hosted SDIO.

**Version:** 0.38.3 | **Target:** ESP32-P4 + ESP32-C6 | **Display:** JD9165 1024x600 MIPI-DSI

## Overview

P4MiniShell replaces the default LVGL demo UI with a persistent DOS-style shell surface built on LVGL 9.5.0 (esp_lvgl_port 2.9.0). It provides a locked transcript UI, RAM-only shell state, SD-backed file workflows, batch-file execution, ESP-Hosted Wi-Fi and Bluetooth on the C6, USB host support, and a real OTA maintenance path for the co-processor.

**Color-coded output** - Every command uses the same built-in colour scheme by default, with
nothing to configure. Colours are defined once in `components/ansi/ansi_palette.h` and applied
automatically to both the on-screen transcript and the serial console. On the display the
transcript is an LVGL span group (`lv_spangroup`) where each coloured run of the ANSI text is
rendered as its own span with an explicit text colour; the UART console receives the raw SGR
escape sequences natively:
- Bright green section headings, cyan field labels, bright white important values
- Bright magenta numbers, sizes, and percentages; grey muted and secondary text
- Green for success and connected, red for errors, yellow for warnings
- `dir` colours entries by kind: bold blue directories, green `.bat` files, white files

The current firmware is not a desktop DOS clone and is not yet an MS-DOS-compatible runtime. It provides the embedded foundation for that direction.

**Current verified state (v0.38.3):** unit **300/0/2** on COM3
(ESP-IDF v5.5.5) with the host regression green: companion deep, db, alarm, app smoke, package,
gfx toolkit, plot, header, keyboard, editor, ui touch, completion, tx stress, boot regression, and
new HW drivers for `timer`/`csv`/`export`/`bind`/`crypt`/`tcpterm`/`usb userial`. Added: `timer`/
`stopwatch`, `csv` grid + `=EXPR` eval, `export` interchange, `crypt`, `tcpterm`, `usb userial`
(lazy CDC-ACM), `bind` F-key bindings, `calc` unit/base functions, `db` `/field:`/`/sort:`, and
`sleep` Wi-Fi auto-restore. The TUI is an 80x25 cell grid (`components/tui/`) filling the transcript region; `draw` (box/line/fill/text/bar/table/list/clear/window/cursor/hold/alt-screen/fullscreen) and the exclusive `gfx` RGB565 canvas (`init/close/status/clear/pixel/line/rect/circle/hline/vline/triangle/ellipse/polygon/fill/text/show/load/blit/free/slots/save`, 8 sprite slots to 64x64, built-in 8x8 text font) drive batch TUIs and games; `crc32`/`asset` verify SD bundles and `pkg` installs/removes them from `PKGS/<APP>/`; `start`/`taskkill` run background jobs; batch files execute from a 128 KB RAM image so `goto` loops stay off the SD card. Reference apps: companion (10 BATs incl. SVC/AGENDA), ADVENT, NOTES, MOOD, BOUNCE, GFXTOOL, PLOT, SNAKE, TCMD, ELITE, PICS.

## Architecture

```
main/main.c                 App entry point, LVGL event callbacks, UI construction, host bridges (prompt via shell_prompt_render_plain)
p4minishell_config.h        Centralized configuration (all tunable values, P4_CONFIG_TUI_COLS 80×25)
p4minishell_config.yaml     Configuration documentation (YAML source of truth)
components/ansi/            ANSI/VT escape sequence module (SGR colors, attributes, formatting) + semantic palette (ansi_get_palette_color)
components/display/         Display manager (rotation, resolution, refresh, brightness, power)
components/windows/         Window manager (LVGL screen layout, dynamic scaling, styling, fullscreen header_set_visible, windows_notify_keyboard_visibility)
components/clock/           Clock manager + clock commands (SNTP time sync, timezone, local/UTC formatting, date/time/timezone/sntp, timer/stopwatch in clock_timer.c)
components/keyboard/        Keyboard manager (LVGL keyboard, visibility, modes, keyboard_bind_textarea)
components/shell/           Shell core (transcript, history, debug log, UART console, input line, sysinfo, prompt situational color, F-key bind dispatch)
components/storage/         SD sessions, path resolution, FATFS conversion, cwd, DOS file commands, CSV parser (storage_csv.c)
components/batch/           Batch engine, labels, for loops, pipes, environment variables, PATH, aliases, F-key binds
components/command/         Command module (parser, dispatcher, output-redirection parsing, worker task + background-job pool, and the split `*_commands.c` verb files: tui/gfx/image/serial/font/md/json/asset/pkg/db/alarm/config/power/periph/audio/csv/export/crypt/userial)
components/tui/             TUI cell buffer (tui_cell_t utf8[4], tui_draw_box/line/bar/table via tui_cell_set, tui_flush recolor #RRGGBB per fg run)
components/gfx/             RGB565 raster core (surface/pixel/line/rect/circle/blit, BMP parse/decode, row convert) for the `gfx` canvas verbs
components/db/              Palm-OS-style SD record store (sd:/DBS/<name>.DB)
components/alarm/           SD alarm store + background checker (sd:/ALARMS, /run: batch action)
components/filetype/        Central extension -> kind registry (.bat/.cmd/.md/.json/text)
components/markdown/        CommonMark-subset renderer for `markdown` and `view *.md`
components/font/            Font registry (roles/sizes/fallbacks), SD TTF loader, CJK attach, theme table
components/boot/            CONFIG.SYS parser + AUTOEXEC.BAT runner + default-file generation
components/editor/          DOS-style `edit` editor: document model + LVGL surface (a modal surface on the shared runtime)
components/modal/           Shared modal runtime + ready-made surfaces (dialog, list, ask, filebrowser, viewer, hexview, EventGroup PSRAM)
components/audio/           ES8311 speaker: codec init (bsp_audio_init guard), volume, background playback engine for beep/tone/wavplay
components/header/          Fixed top status bar (Wi-Fi, battery, Bluetooth, USB, SD, activity, clock/notification queue, adaptive refresh)
components/led/             WS2812 RGB status LED driver + auto status / event notification engine (GPIO26)
components/networking/      Sole owner of ESP-Hosted + esp_wifi_remote: Wi-Fi station lifecycle, hosted NimBLE, status accessors, known-network storage (wifi_known.c), TCP terminal (tcpterm.c)
components/usb/             USB Host MSC storage (/usb0) + HID keyboard/mouse + lazy CDC-ACM serial (userial.c)
components/c6ota/           ESP32-C6 firmware OTA via ESP-Hosted SDIO
coprocessor/esp32c6_slave/  ESP32-C6 hosted slave firmware project
grab_screenshot.py           Screenshot debug loop (--port/--out/--crop-transcript, transcript rect 1024x510)
capture_tui.py              TUI capture helper (paired with grab_screenshot.py, tui status 80×25)
managed_components/lvgl__lvgl/src/font/lv_font_unscii_16.c  Extended unscii_16 font (384 glyphs U+2500-U+257F, U+2600-U+26FF, cmaps 3, CONFIG_LV_FONT_UNSCII_16=y)
```

### Module Ownership

Dependencies flow one way: `command` → `batch` → `storage` → `shell` → (`ansi`, `display`, `windows`, `header`, `keyboard`, `clock`). `components/modal/` is a shared runtime used by `editor` and by the batch `dialog`/`list`/`ask`/`browse`/`view`/`hexview` commands (reached through `command`/`batch`, never from `shell`). The TUI logical grid (`P4_CONFIG_TUI_COLS`×`ROWS`) maps to the live transcript region, so every TUI surface fills exactly the same space as the shell and scales with rotation/keyboard.

| Module | Owns |
|--------|------|
| `main/main.c` | `app_main()`, boot sequencing, LVGL event callbacks, UI construction, c6ota/usb host bridges |
| `components/shell/` | Transcript + async buffer, command history, debug log, UART console, input line prompt contract, the interactive keypress queue, the DOS prompt template engine (`shell_prompt_render_plain()` `components/shell/shell.c:412`), system info commands (`help`, `sysinfo`, `version`, `about`, `mem`, `debug`, `tui status`) |
| `components/storage/` | Guarded SD sessions, persistent mount tracking, path resolution, FATFS conversion, size formatting, DOS wildcard matching, current working directory, volume capacity queries and write guardrails, input/output redirection plumbing, and every DOS file, text, and volume command |
| `components/batch/` | Batch file execution, `:label` scanning, `goto`, `call :label`, `for` loops (incl. `for /f` file-line iteration), multi-stage `\|` pipes, environment variables, PATH, variable expansion, errorlevel, `setlocal`/`endlocal` scoping, the `calc` float calculator (`calc.c`), and the batch language commands |
| `components/applib/` | Native-app runtime library (`applib.h`): app `printf`/stdout onto the transcript (the redirection layer), a shared memory-allocation policy, time/timer/sleep/system-info helpers, and Wi-Fi state accessors through a registered ops table |
| `components/tui/` | TUI cell buffer (`tui_cell_t utf8[4]` `components/tui/tui.h:35`, `tui_draw_box`/`line` via `tui_cell_set` `components/tui/tui.c:129`, `tui_flush` `components/tui/tui.c:620` recolor `#RRGGBB` per fg run via `ansi_get_palette_color`, `draw fullscreen`/`tui fullscreen` via `windows_set_fullscreen`) |
| `components/command/` | Command dispatch and worker task, the execution pipeline, output redirection parsing, hardware commands, UI query commands, system commands, hardware telemetry, TUI window stack (`tui_draw_box` with title, nested boxes) |
| `components/modal/` | Shared modal-surface runtime (`modal_surface_run`, input routing) plus the `dialog` / `list` / `ask` surfaces used by batch files |
| `components/editor/` | The DOS-style `edit` editor: a byte-preserving document model and its LVGL surface, run as a modal surface on `components/modal/` |
| `components/audio/` | ES8311 speaker: codec initialization, speaker volume, and the background playback engine behind `beep`/`tone`/`wavplay`/`audio` (the commands themselves dispatch from `components/command/`) |
| `components/gfx/` | Pure RGB565 raster surface (alloc/pixel/line/rect/circle/hline/vline/triangle/ellipse/polygon/flood-fill/text/get, `gfx_font.c` 8x8 ASCII font), general BMP parse/decode (24/32-bit, either orientation) with scaled decode + nearest scaling + aspect-fit, and 565→888 row conversion; the display glue is `gfx_commands.c` |
| `components/db/` | The Palm-OS-style SD record store (`sd:/DBS/<name>.DB`): records, categories, secret/redacted payloads, soft-delete + purge, and the pure `k=v` field parser (`db_field_get`) behind `db /field:`/`/sort:` |
| `components/alarm/` | SD-persisted alarms + calendar store and the single background checker that fires header/LED/speaker notifications and queues `/run:` batch actions |
| `components/storage/storage_csv.c` | The one RFC-4180-subset CSV parser (`csv_split_line`) behind the `csv` verb and `export` CSV output |
| `components/command/csv_commands.c` | `csv rows|cols|cell|eval` verbs (grid + `=EXPR` eval via `calc`); CSV field formatter `csv_format_field` shared with `export` |
| `components/command/export_commands.c` | `export <db|alarms> <csv|json|txt> <file>` portable store interchange through the existing store APIs |
| `components/command/crypt_commands.c` | `crypt lock|unlock` AES-256-GCM + PBKDF2 file encryption (mbedTLS), streaming and atomic |
| `components/usb/userial.c` | Lazy CDC-ACM class driver + byte API + RX ring (leaf; verbs elsewhere) |
| `components/command/userial_commands.c` | `usb userial` verbs over the byte API (uses the shell key queue + SD input source) |
| `components/networking/tcpterm.c` | `tcpterm` one-shot TCP request/response (owned with the other socket surfaces) |
| `components/clock/clock_timer.c` | Pure named stopwatch slots behind `timer`/`stopwatch` |
| `components/filetype/` | The one extension→kind registry (`batch`/`markdown`/`json`/`text`) consumed by the editor, viewer, `launch`, `dir`, and the batch resolver |
| `components/markdown/` | CommonMark-subset renderer behind the `markdown` verb and `view *.md` |
| `components/font/` | Font registry (roles/sizes/fallbacks), SD TTF loading, CJK auto-attach, and the runtime UI theme registry (`theme.c`: `default`/`amber`/`ice`/`mono`, live-switchable + persisted) |
| `components/boot/` | `CONFIG.SYS` directive parser + `AUTOEXEC.BAT` runner + SD default-file generation |

Two registration tables invert the only upward dependencies:
`shell.c` reaches command-owned services (dispatch, cwd, volume, SD mount state, battery) through
the `shell_command_ops_t` table, and `batch.c` re-enters the command pipeline through
`batch_command_ops_t`. Both are registered by `command_init()`, so neither the shell core nor the
batch engine depends on the command module at include time.

## Configuration

All tunable values live in `p4minishell_config.h`, organized by subsystem:
shell identity, buffer sizes, UI layout, Wi-Fi, Bluetooth, SD card, batch engine,
GPIO, header visuals, USB host, C6 OTA, and task stacks.

The companion `p4minishell_config.yaml` documents every value with type,
description, and valid range. To change a value, edit the header then update
the YAML to match.

## Hardware Baseline

| Component | Detail |
|-----------|--------|
| **Host MCU** | ESP32-P4 |
| **Co-processor** | ESP32-C6 over ESP-Hosted SDIO |
| **Display** | JD9165 1024x600 MIPI-DSI via LVGL 9.5.0 (esp_lvgl_port 2.9.0) |
| **Touch** | GT911 via I2C |
| **Storage** | FATFS on SD with LFN support (255 chars) |
| **Audio** | ES8311 codec via I2S |
| **Battery** | ADC on GPIO53 with 2:1 divider (3.3V-4.2V range) |
| **RGB LED** | WS2812 (NeoPixel) status LED on GPIO26 (back panel) |
| **Hosted SDIO** | CLK=18 CMD=19 D0=14 D1=15 D2=16 D3=17, reset GPIO54 |
| **USB Host** | MSC storage at /usb0 + HID keyboard/mouse |
| **ESP-IDF** | v5.5.5 |
| **Build target** | esp32p4 |

## Key Features

- **Touch-first shell UI**: Transcript span group, prompt input line, on-screen keyboard, and persistent recall history (Up/Down, Ctrl+R reverse search, auto-saved to `HISTORY.TXT`). Tab completes from the help table (commands, flags, aliases, installed apps, paths) and the input line shows the completion as muted ghost text. The symbols keyboard covers every printable ASCII character, including the shell-critical pipe `|`, caret `^`, tilde `~`, and backtick.
- **Scrollable transcript**: A scrollable transcript that holds a multi-screenful history and always jumps to the output of the command you just submitted. Scroll with the input-row `Up`/`Dn` buttons, touch drag, USB keyboard `PageUp`/`PageDown`, or the USB mouse wheel.
- **ANSI/VT color support**: PowerShell-inspired 16-color palette with SGR escape sequences (ESC[...m) for colored command output on both LVGL transcript (per-span colours) and UART console
- **Serial console bridge**: `idf.py monitor` acts as interactive shell endpoint over UART/USB-Serial-JTAG
- **Worker-task execution**: Heavy commands run off LVGL event stack to prevent overflow
- **Guarded SD access**: All SD operations use shared mount/unmount with validation and bounded output
- **DOS-style file commands**: `cd`, `dir`, `copy`, `move`, `del`, `ren`, `mkdir`, `rmdir`, `type`, `write`, `append`, `touch`
- **Full `dir` option set**: `/W` wide, `/P` paged, `/S` recursive, `/B` bare, `/L` lowercase, `/A` attribute filter, `/O` sort order
- **Volume management**: `chkdsk`/`scandisk` capacity and integrity report, `format` behind an exact confirmation word
- **diskpart-style disk tools**: `disk list`/`detail`/`clean`/`create partition primary`/`delete partition`/`format` for MBR partition-table management and FORMAT.COM-style formatting with `/FS:` `/A:` `/V:` `/Q`
- **DOS-style boot scripting**: `CONFIG.SYS` directives (SET, PATH, PROMPT, ECHO, ROTATE, BRIGHTNESS, VOLUME, WIFI_*, BLUETOOTH, USB_*, GPIO, LAUNCH_APP) and `AUTOEXEC.BAT` execution at boot, with default-file generation; unrecognized `KEY=VALUE` lines set environment variables
- **App discovery + launch**: `launch` finds the installed `.bat` apps (on PATH + the conventional `sd:/APPS`), shows them with optional `APPINFO` metadata (`sd:/APPS/<name>.APPINFO`, INI `title=`/`description=`), and runs the chosen one via a menu or by name (`launch`, `launch /list`, `launch <name>`). A CONFIG.SYS `LAUNCH_APP=<app>` directive offers to run an app after boot.
- **Storage guardrails**: free-space prechecks, self-copy protection, partial-destination cleanup, copy progress
- **RAM-only shell state**: Environment variables, PATH, current working directory, batch arguments
- **Batch file engine**: `.bat` execution with `%0`/`%1`..`%9`/`%*` expansion, `:label` targets, `goto` (including the implicit `:eof` label), `call` (with argument forwarding and errorlevel propagation), `for %%var in (set) do ...` loops (literal tokens or a wildcard pattern), `rem` comments, `echo on/off`
- **Aliases / macros**: DOSKEY-style `alias` / `unalias` commands define a RAM-only macro table; typing an alias at the prompt expands its leading word (`alias ll=dir /s`, then `ll` runs `dir /s`). Persist with `alias /save` to `sd:/ALIASES.BAT`, which boot.c auto-loads after CONFIG.SYS. Aliases never expand inside batch files.
- **Real batch control flow**: `pause` and `choice` block on an actual keypress, `setlocal`/`endlocal` scope the environment, `exit /b` leaves one batch file, `if` with `errorlevel N` (≥), `exist <path>`, case-insensitive `/i` string tests, and `not`
- **Batch expressions**: `set /a` integer arithmetic with the full DOS operator set plus comparison (`== != < > <= >=`) and logical (`&& ||`) operators that yield 1/0, `set /p` prompted input, trailing `^` line continuation
- **`calc` calculator**: `calc [NAME=] <expr>` evaluates a floating-point expression with the FX-870P/VX-4 math and string functions (ABS, SIN, COS, TAN, SINH/COSH/TANH, ASINH/ACOSH/ATANH, ASN/ACS/ATN, SQR, EXP, LN, LOG, FACT, NCR, NPR, INT, FIX, FRAC, ROUND, SGN, MOD, PI, RAN#, POL/REC, DMS/DMS$, DEG, CUR, VAL/VALF, STR$, HEX$, ASC, CHR$, LEN, LEFT$/MID$/RIGHT$, `&H`/`0x` hex, string concatenation with `+`) and stores results into environment variables for batch use. `calc /deg|/rad|/angle` set the trig angle mode; `calc /hex` prints an integral result as `&H` hex. POL/REC store their two results in the X/Y variables (calculator BASIC parity). Sets ERRORLEVEL.
- **Batch file input**: `set /p NAME=< file` (and `echo x | set /p var=`) reads one line from the input source instead of prompting, and `for /f "eol=c skip=n delims=xyz tokens=a,b,m-n" %%v in (file-set) do cmd` iterates over a file's lines — the cmd.exe mechanisms behind the BASIC `INPUT#` / `LINE INPUT#` / `READ` / `DATA` verbs.
- **Batch process abstraction**: `proc` introspects the active batch process stack (`/args` `/name` `/depth` `/errorlevel` `/echo` `/stdin`), `%ERRORLEVEL%` expands to the current exit code, and a `.bat` is a first-class pipe process (`echo x | filter.bat | findstr ...` reads the pipe spool via `for /f in ()` / `set /p <` and writes its stdout into the pipeline). Pipe stages spool through SD; the redirection capture is re-entrant so `cmd1 | cmd2 > out.txt` round-trips.
- **Dynamic pseudo-variables**: `%DATE%` (`MM-DD-YYYY`), `%TIME%` (`HH:MM:SS`), `%RANDOM%` (`0..32767`), and `%CD%` (current directory) expand like cmd.exe, and an undefined `%VAR%` expands to the empty string so the DOS `if "%var%"==""` idiom works. `%%` still yields a literal `%`.
- **`delay <ms>`**: a pure deterministic wait (unlike `sleep`, which is light-sleep) for melodies and demos; `if [not] [/i] defined VAR` checks whether a variable is set (cmd.exe parity).
- **Shared batch libraries + input timeouts**: `call <file.bat>::<routine> [args]` calls a routine from a shared library of batch routines with automatic variable isolation (beyond `setlocal`); `set /p NAME=<prompt> /T:secs` gives prompted input a timeout alongside `choice /T`. Native apps get the same primitives through applib's lean headers (`applib.h` umbrella over `applib_console/mem/time/net/input/state.h`) plus `app_wait_key` / `app_read_line` with timeouts.
- **Easy persistent state (all on the SD card)**: DOS-style environment + temp files + INI files — `ini get/set/del/list/load/save` reads/writes `KEY=VALUE` files (and imports/exports the environment), `appconfig <app>` gives each batch app its own `sd:/APPS/<APP>.INI` settings file without hand-rolling parsing, and `temp new/clean` manages SD-backed temporary files under `sd:/tmp`. The same core powers the applib state group (`app_ini_get/set/delete`, `app_temp_path/cleanup`) for native apps.
- **Palm-OS-style `db` record store (all on the SD card)**: named databases under `sd:/DBS/<name>.DB/` with monotonic record ids, 16 categories, secret/redacted payloads, and soft-delete + purge. The `db` command family (`create/list/info/drop`, `open/close/current`, `categories`, `add/get/set/del/purge`, `count/find`, `export/import`) is batch-friendly (ERRORLEVEL 0/1/2, `/b` bare output for `for /f`), and native apps get the same store through `applib_db.h` (`app_db_*`). Payloads can hold `name=value;name=value` fields: `db find /field:k=v`, `db sort <field|key|id>`, and `db get <id> /field:name` add fielded queries without a schema (`components/db/db.c` `db_field_get`).
- **`export`-portable store interchange**: `export <db NAME|alarms> <csv|json|txt> <file>` renders the `db` and `alarm` stores through their existing APIs and writes atomically; CSV reuses the same RFC-4180 quoting as `csv` (secret payloads included — a backup is complete or useless). `components/command/export_commands.c`.
- **`csv` grid substrate (spreadsheet-lite)**: `csv rows|cols|cell|eval` parses RFC-4180-subset CSV and resolves `=EXPR` cells through the `calc` engine with `R<row>C<col>` references (iterative, so forward/chained refs work). The pure parser is `components/storage/storage_csv.c`; verbs in `components/command/csv_commands.c`. Enables 3rd-party sheet apps without re-implementing parsing.
- **`crypt` password file encryption**: `crypt lock|unlock <src> <dst> [/p:pass|/ask]` seals files with AES-256-GCM under a PBKDF2-SHA256 key, streaming in 4 KB chunks with atomic writes; secrets never print and `/p:` passwords are masked in history/transcript. The memo-password analogue for the SD card. `components/command/crypt_commands.c`.
- **USB CDC-ACM serial (`usb userial`)**: raw serial to external gear (GPS/MCU/scope/console) via `usb userial open <vid:pid> [baud=..]`, `send`, `recv`, and an interactive `term`. The class driver installs LAZILY on first use (boot never pays for it — see `bugs.md`), keeping the boot-time internal-RAM headroom that the hosted SDIO bring-up needs. Driver/ring in `components/usb/userial.c`, verbs in `components/command/userial_commands.c`.
- **`tcpterm` TCP terminal**: one-shot TCP request/response (`tcpterm <host> <port> [/t:secs] [text...]`) over hosted Wi-Fi — the modern Datacomm. Bounded connect/send/idle budgets, sanitized reply printing that preserves remote SGR colours, and `< file`/pipe request sourcing. `components/networking/tcpterm.c`.
- **`timer`/`stopwatch`**: named stopwatch runs over `esp_timer` (`start/stop/lap/status`, `/b` rows, `/v:NAME` millisecond results) for batch timing. `components/clock/clock_timer.c`.
- **`bind` F-key bindings**: `bind F5 <line>` fires a command line from a USB HID function key at the prompt; persisted to `sd:/BIND.BAT` and restored at boot after the alias profile. `components/batch/batch.c`.
- **`calc` unit/base functions**: `BIN$`, `OCT$`, `VALB(str,base)`, `C2F`/`F2C`, `IN2MM`/`MM2IN`, `LB2KG`/`KG2LB` extend the calculator for palmtop-style conversion work.
- **`sleep` Wi-Fi auto-restore**: when light sleep tore Wi-Fi down, the wake path re-establishes it in the background (the same request shape as the post-OTA restore) instead of requiring a manual `wifi connect`.
- **SD-persisted alarms + calendar (`alarm`, `cal`)**: a background checker fires due events through the existing header notification, LED, and speaker — no second notification loop — and can queue a `/run:` batch file onto the command worker. Events persist on the SD (`sd:/ALARMS/`), support one-shot / daily / weekly-weekday recurrence, soft-delete + purge, and boot catch-up. `cal today` / `cal next` / `cal YYYY-MM` give a thin calendar view.
- **Menu / form primitives (CHOICE + ANSI)**: `ansi <sgr-codes> [text...]` emits text styled with ANSI SGR codes (reverse video, bold, color) and `menu <item> ...` renders a numbered form whose choice becomes ERRORLEVEL — all rendered in the transcript display and readable from touch, USB keyboard, or serial. Native apps get the same through `app_print_styled` and `app_menu`.
- **Native modal surfaces (hardware-verified on COM11)**: batch files can drop into polished LVGL dialogs instead of the transcript — `dialog "title" "message" [btn1] [btn2]` (ERRORLEVEL 0/1/255), `list [/t:secs] [/v:NAME] "title" item...` (ERRORLEVEL = selected 0-based index, or the label in `NAME`), `ask [/t:secs] [/v:NAME] [/p] "prompt" [default]` (answer stored in `ASK_RESULT` or `NAME`, `/p` masks it, placeholder + `keyboard_bind_textarea` `components/modal/modal_surf.c:412`), `browse [/t:secs] [/v:NAME] [path]` (selected file in `BROWSE_RESULT` or `NAME`), `view <file>` and `hexview <file>` pagers for text/binary files. All accept `/t:secs` to auto-cancel. They run on a shared modal runtime (`components/modal/` `MALLOC_CAP_SPIRAM` `components/modal/modal.c:46`) that the `edit` editor also uses. Each surface fills the live transcript region (`1024x510` via `tui status`) and resizes with rotation/keyboard via `windows_notify_keyboard_visibility`. Verified on hardware: `dialog`/`list` with `/t:secs` timeout and serial input, `ask` via serial, `browse`/`view`/`hexview` fill the `80×25` region and `draw` is TUI-aware. **Modals are capturable**: bare `screenshot` runs on the console-reader task while a modal blocks the worker, so `tools/harness/grab_screenshot.py` captures an open `dialog`/`list`/`ask`/`browse`/`view`/`image show`/`hexview` or the `edit` editor.
- **TUI drawing + extras (hardware-verified on COM3)**: `draw box` single/double/rounded with title, `draw line`/`fill`/`text`/`clear`/`window`/`bar`/`table`/`list`/`hold`, `draw fullscreen on|off` (global) + `tui fullscreen on|off` (per-app), `color`/`locate`, and `ansi`/`menu` compose full-screen batch TUIs; `browse`/`view`/`hexview` give file pickers and pagers. Logical 80x25 (`P4_CONFIG_TUI_COLS`/`ROWS`) maps to the live transcript rect; drawing renders through the cell buffer and `tui_flush`. Font: extended `unscii_16` in-place (384 glyphs U+2500-U+257F/U+2600-U+26FF, `CONFIG_LV_FONT_UNSCII_16=y`). Screenshot loop: `tools/harness/grab_screenshot.py`.
- **GFX pixel canvas + sprites**: `gfx init/clear/pixel/line/rect/circle/show` draw to a PSRAM RGB565 canvas (max 320x240) shown in the transcript region; `gfx load <slot> <path>` ingests a 24/32-bit BI_RGB BMP into one of 8 sprite slots (max 64x64), `gfx blit` stamps it (clipped, optional transparency), **`gfx image <path> [x y [w h]]` blits a scaled BMP onto the canvas**, `gfx save` writes the canvas back as BMP, and `gfx free`/`gfx slots` manage the bank. Exclusive with TUI mode and refused in background jobs. Reference: `apps/gfxdemo/BOUNCE.BAT`.
- **BMP image support everywhere**: one decoder in `components/gfx` (24/32-bit BI_RGB, top-down or bottom-up) drives three surfaces — `view`/`open`/`image show` open a fit-to-screen full-color viewer (modal), `draw image <file> <x> <y> <w> <h>` renders into the 80x25 TUI as 16-color luminance-ramp glyphs, and `gfx image` blits onto the pixel canvas; `image info` prints WxH/bpp/orientation for batch scripts. The source is decoded straight to the target size, so large images stay memory-bounded (`P4_CONFIG_IMAGE_MAX_BYTES`, `GFX_IMAGE_MAX_W/H`). `.bmp`/`.dib` are routed by the central filetype registry. Reference: `apps/pics/PICS.BAT`.
- **GFX toolkit (B2)**: raster primitives `gfx hline/vline/triangle/ellipse/polygon/fill` (filled or outline; even-odd scanline polygon and a 4-way flood fill) plus on-canvas text `gfx text [/scale:1..16] [/bg:<color>] <x> <y> <color> <text...>` rendered from a committed 8x8 ASCII font (`components/gfx/gfx_font.c`, generated from public-domain unscii-8). Reference: `apps/gfxdemo/GFXTOOL.BAT`.
- **Plot/graph layer**: `plot window|axes|func|polar|para|data|bar|table|line|point` renders world-coordinate math onto the `gfx` canvas or the TUI grid through one shared viewport, sampling `calc` expressions (`y=f(X)`, `r=f(T)`), files, or inline values; axes with nice-step ticks, asymptote-aware polylines, and calculator TABLE listings. Reference: `apps/gfxdemo/PLOT.BAT`.
- **UI themes (B3)**: `theme list | show [name] | set <name> [/save]` switches the chrome palette among four built-ins (`default`/`amber`/`ice`/`mono`) and re-applies it live to the screen, transcript, input row, keyboard, and header; `/save` persists to `SHELL.INI` and restores at boot. Companion **Settings > Theme**.
- **Responsive header**: measurement-driven layout fits status/notification/system panels with no overlap on any resolution or rotation — abbreviations, dynamic font step, bounded scrolling notification, uptime indicator; `header [status]|mode|show|hide` verb, `HEADER_MODE=` boot directive, `SHELL.INI` persistence.
- **Color-coded compact status**: WiFi/BT/USB/SD (and MEM/CPU/BAT) render as one compact ASCII glyph colored by state — green healthy, amber degraded, red off/failed, muted absent — instead of words, returning the width to the notification area (set `P4_CONFIG_HEADER_STATUS_STYLE=words` for the verbose labels). A conditional `A` appears while a C6 OTA or a background job runs. Tap an indicator for a one-line detail; long-press runs the matching `wifi|bluetooth|usb|sd|ps|mem|top|battery` status command. Classification lives once in `components/header/header_status.c` (unit-tested).
- **Header clock + notification queue**: the center shows the local time (`HH:MM`) when idle. The timezone is **auto-detected from the network** (no hardcoded zone): once Wi-Fi associates, a background task looks up the location over HTTP, applies the offset, and starts SNTP, re-checking periodically so DST/travel stay correct. Notifications are shown FIFO with a severity that colors them info/warn/error, so an alarm or OTA alert is never lost behind a later trivial message; `notify -` flushes the queue (`header_notify_queue.c`, unit-tested).
- **Situation-aware header refresh**: one header timer is rescheduled each tick by a pure policy (`header_refresh.c`) — ~150 ms while the display is idle-off (prompt touch wake), 1 s while an OTA/background job runs, 2 s idle, aligned to the clock minute and the idle-off deadline — while expensive heap/CPU/battery telemetry stays throttled to 5 s.
- **Selectable TUI grids**: `draw table` (with `/cursor:N` and `/sel:a,b`) and the file-manager primitive `draw list <x> <y> <w> <h> <file> [/top /cursor /sel /title /count:NAME /countonly]` render scrollable, cursored, selectable panels; `draw hold on|off` coalesces a frame's verbs into one `draw refresh`.
- **Asset manifests + checksums**: `crc32 <path>` prints a file's CRC-32 (the same primitive `receive` uses), and `asset check|list <app>` verifies `APPS/<APP>.ASSETS` (`path=HEXCRC` manifest lines). `apps/push_assets.py` generates PIL sprites + manifests; `asset check` is green for every bundled app.
- **Packaged SD apps**: `pkg list|info|verify|check|install|remove` installs apps from CRC-checked `PKGS/<APP>/` bundles into `APPS/` (metadata `APPS/<APP>.APPINFO` with `title=`/`description=`/`version=`, manifest `APPS/<APP>.ASSETS`). `apps/push_pkgs.py` builds bundles; a Companion **Live System > Packages** menu drives it; `pkg remove` trashes files so `undelete` can recover.
- **Background jobs + services**: `start <command>` runs a line or batch file on a pooled background worker (`bg0`), `taskkill <job>` stops it cooperatively (per batch line and per 100 ms of `delay`), and background display verbs refuse loudly. Companion's `SVC.BAT` service loop and `AGENDA.BAT` job (with a Live System > Services menu) show alarms/calendar/notification work running in the background.
- **Reference apps + games**: `apps/tcmd/TCMD.BAT` (dual-pane commander over `draw list`), `apps/snake/SNAKE.BAT` (TUI snake, `choice /T` steering), `apps/elite/ELITE.BAT` (turn-based trader), `apps/gfxdemo/BOUNCE.BAT` (gfx bouncing ball), `apps/gfxdemo/GFXTOOL.BAT` (raster toolkit + text demo), plus companion/adventure/notes/mood; deployed by `apps/push_apps.py` and `apps/push_assets.py`.
- **Header notifications**: `notify [/t:secs] <text>` (and `notify -` to clear) shows a message in the header from a batch file; native apps get `app_notify`.
- **Sample batch app**: `apps/companion/` is a complete, pure-batch menu-driven system helper (Live System dashboard + Services, file tools + note pad, network tools, games/demos, persisted settings) that exercises the whole batch language - `call ::routine` libraries, `for /f`, pipes, `appmode`, `ask`/`dialog`/`list`, `%DATE% %TIME% %RANDOM%`, background `start`/`taskkill`, and graceful offline network handling. Push it with `apps/companion/push_sd.py COM3` (10 BATs now incl. `SVC.BAT`/`AGENDA.BAT`) and run `COMPANION.BAT`. Hardware-verified (deep 8/8); the worker-stack overflow fix moved `P4_CONFIG_COMMAND_TASK_STACK` to 32768.
- **Password input**: `set /p NAME=<prompt> /P` collects a line without echoing it (combines with `/T:secs`); the app-side `app_read_password` gives native apps the same no-echo input.
- **App mode**: `appmode on [/full] [/clear]` saves the screen and takes over the shell (optionally full-screen + cleared), `appmode off` restores it — and the screen is restored automatically when the batch file exits via `exit /b` / `goto :eof`. Native apps get `app_mode_enter` / `app_mode_exit`.
- **BASIC-to-DOS batch mapping**: the FX-870P/VX-4 command surface maps onto existing DOS verbs (`GOSUB`→`call :label`, `RETURN`→`goto :eof`, `CHAIN`→`call`, `FILES`→`dir`, `KILL`→`del`, `NAME`→`ren`, `PRINT`→`echo`, `INPUT`→`set /p`, `LIST`→`type`/`findstr /n`, `VARLIST`→`set`, `DSKF`→`chkdsk`, `STOP`→`pause`, `END`→`exit /b`, `ON ERROR`→`if errorlevel`/`||`). See `command.md` for the full table.
- **applib native-app runtime** (`components/applib`): a stable SDK surface for native apps — `app_printf`/`app_printf_ansi` stdout onto the transcript (so `myapp > out.txt` works), a shared memory-allocation policy (`app_alloc`/`app_strdup`/…, PSRAM-aware), `app_report_*` debug-log reporting, time/timer/sleep/system-info helpers (`app_time`, `app_uptime_sec`, `app_delay_ms`, `app_sysinfo`), Wi-Fi state accessors (`app_wifi_is_connected`, `app_wifi_get_rssi`) routed through a registered ops table, and the **native-app ABI**.
- **Native-app ABI**: a C app registered with `app_register()` becomes a shell command that receives `argc`/`argv` (`argv[0]` = app name), reads/writes the shell environment (`app_env_get`/`app_env_set`) and the current directory (`app_get_cwd`), and returns an ERRORLEVEL a batch file can branch on — the native equivalent of batch's `%0..%9`/`%*` + env table + cwd. `apps` lists registered apps; `main/native_apps.c` registers the reference `hello` sample (echoes argv, cwd, PATH).
- **DOS prompt engine**: `prompt` template with `$p $g $t $d $v $n` and more, driving both the UART console and the on-screen input line
- **Text utilities**: `find` (text search `/I /N /C /V`, plus a recursive file-discovery mode by name/size/date via `/NAME:` `/SIZE:` `/NEWER:` `/OLDER:` `/DIRS` `/B`), `findstr` (literal or regex-lite search, case-sensitive by default, `/R /C /I /N /V /X /E /B /L /S /M /F /G`), `more` (keypress paging), `tree` (recursive, `/F /A`), `fc`, `comp` (byte compare `/D /A /L /N /C`), `sort` (`/R /I /U`). All text tools set a DOS ERRORLEVEL (0 ok / found, 1 not found / different, 2 usage) for `if errorlevel` and `&&`/`||`.
- **Redirection**: `>`, `>>`, and `<` in any order on one line
- **Multi-stage pipes**: `cmd1 | cmd2 | cmd3` with quote-aware splitting
- **Command chaining**: `a & b` (both), `a && b` (on success), `a || b` (on failure)
- **DOS quoting and escaping**: `"text"` groups with expansion, `'text'` groups literally, `^c` escapes any character
- **Clipboard**: `clip` / `paste` provide a RAM clipboard for the transcript and the SD card — copy the last N lines (`clip copy [N]`), clip a text file (`clip read <file>`) or a file reference (`clip file <path>`), then `paste` into the input line at the cursor or `paste <dest>` to copy the file. Batch-safe, redirectable, ERRORLEVEL.
- **Tab completion + long lines**: USB Tab completes command names, aliases, and SD file/dir paths (with a trailing `/` on directories); command lines accept up to 4096 characters (`P4_CONFIG_COMMAND_BYTES`).
- **History with SD save/restore**: heap-backed recall (32 commands, Up/Down) with `history` list, `history /save [file]` / `history /load [file]` (default `HISTORY.TXT`) and `history /clear`.
- **Hosted Wi-Fi**: ESP-Hosted + esp_wifi_remote on C6 with version compatibility gate. Station-only, enforced in code and by compiling SoftAP out. Every Hosted and wifi_remote call is confined to `components/networking/`. `wifi status` reports SSID/BSSID/channel/RSSI/PHY/IP/DNS/uptime, `wifi scan` is RSSI-sorted with a bare `/b` form, and classic `ping` + `dns`/`nslookup` connectivity commands set ERRORLEVEL for batch use.
- **Known Wi-Fi networks**: A persistent list of previously-used networks lives on the SD card (`sd:/WIFI.KNOWN`, hand-editable plain text). On boot with `WIFI_AUTOCONNECT=ON`, the firmware scans and connects to the best known network in range (preferred / highest priority / strongest RSSI), falling back to the classic single-credential path when the SD card is absent. Manage it with `wifi known`, `wifi save`, `wifi forget`, and `wifi preferred` — passwords are never printed.
- **Basic HTTPS**: `httpget <url> [localfile]` (alias `wget`) performs a simple HTTPS/HTTP GET over the same `esp_http_client` stack c6ota uses, printing the body or saving it to SD with free-space guardrails, setting ERRORLEVEL, and supporting redirection/pipes. A `user:pass@` URL prefix sends HTTP Basic auth. All HTTP/TLS code lives in `components/networking/`.
- **HTTP file server**: `httpd start|stop|status` shares the SD card over the Wi-Fi link via `esp_http_server` — directory listings, file streaming with Content-Type, optional Basic auth, path-traversal rejection, and a lifecycle tied to Wi-Fi (auto-start on DHCP, stop on disconnect). Also `netstat` (interfaces + TCP/UDP endpoints) and `ipconfig` (full per-interface config + DNS). All of it lives in `components/networking/`.
- **Hosted Bluetooth**: NimBLE VHCI on C6 for BLE scan and advertising, with a sorted bounded scan report and session-scoped `advertise on [name]`
- **USB Host**: MSC mass storage at `/usb0`, HID keyboard/mouse with opt-in echo
- **USB Keyboard Auto-Detect**: Plug in a USB keyboard to type commands; on-screen keyboard hides automatically. Full US keyboard layout supported including symbols, keypad, navigation keys, and function keys.
- **C6 OTA updates**: Validated firmware updates from SD or HTTP/S over ESP-Hosted SDIO
- **Hardware controls**: Brightness, rotation, battery telemetry, volume, GPIO inspection
- **Peripheral toolkit**: LEDC PWM (`pwm`) and square waves (`freq`), one-shot ADC reads on any non-reserved pin (`adc`), and an I2C scanner with peek/poke on the shared bus or custom pins (`i2c`). `spi status` reports the SPI configuration; the SPI transaction verbs fail with an honest error because SPI host init on this P4 with the ESP-Hosted SDIO link active stalls the chip. All toolkit commands share one pin-safety gate that refuses the board's active I2C/I2S/SDIO/display/SD lines.
- **Basic audio (hardware-verified, no longer aborts)**: `beep` / `tone <freq> [ms]` play tones and `wavplay <file>` streams a 16-bit PCM WAV from SD through the ES8311 speaker; `audio status|stop` controls the background playback and `volume [<0-100>]` queries or sets the codec level. Managed BSP `esp_codec_dev` abort on `tone`/`wavplay` fixed (null `card_handle` guard in `managed_components/espressif__esp_codec_dev/i2s/esp_codec_dev.c`). All are batch-friendly (background playback, ERRORLEVEL).
- **RGB status LED**: the WS2812 (NeoPixel) LED on the back panel (`rgb` command) doubles as a glanceable status light — amber pulse while Wi-Fi connects, green when connected, red blink when disconnected, red pulse on Wi-Fi failure, blue flash when the HTTP server starts, and a green confirmation flash at boot. Solid colours, `#RRGGBB`, and `rainbow`/`breath`/`pulse`/`blink` effects are available; `rgb auto <on|off>` toggles the status layer. Works in batch files and via the CONFIG.SYS `RGB=` directive. Owned by `components/led` (espressif/led_strip over RMT).
- **Time / SNTP control**: `date`, `time`, `timezone`, and `sntp`/`ntpsync` commands (all owned by `components/clock/`). `sntp sync` synchronizes the clock over Wi-Fi, `timezone <TZ>` sets a POSIX timezone string, and `date`/`time` show a fuller clock panel (local/UTC/unix/timezone/uptime/sync) while still supporting the DOS-style set forms.
- **Idle display-off + wake**: `power idle <seconds>` (and CONFIG.SYS `DISPLAY_TIMEOUT=`) turns the display backlight off after N idle seconds; touch, USB keyboard/mouse, or any serial command wakes it back to the live shell. `power` reports the setting, and `sleep`/`deepsleep` support a user-wired `P4_CONFIG_POWER_WAKE_GPIO` wake (touch wake is honestly reported unavailable — the GT911 INT line isn't wired on this board).
- **DOS-style `edit` editor**: a modal, touch-first text editor for any text
  file on the SD card — inline editing with a line-number gutter and
  current-line highlight, a blinking block cursor with selection overlay,
  cut/copy/paste, snapshot undo/redo, Find / Replace / Go-to-Line prompts,
  Save As, and File > Open, plus a quit confirmation that protects unsaved
  work. The on-screen keyboard opens on the editor Nav page (every command,
  including the new Open/Comment/Match/Wrap/Reload row, is a touch button) and
  switches to letters for text prompts; a USB keyboard or the serial console
  works too. Batch files get syntax highlighting. Large files are supported:
  the document is **PSRAM-backed** (up to 1 MB / 65 536 lines) and only a
  small window of rows is rendered at once. See
  [tutorial_edit.md](tutorial_edit.md) for the complete guide and
  [editor.md](editor.md) for the quick reference.
- **Fixed header bar**: Wi-Fi, battery, Bluetooth, USB, SD status with transient notifications
- **Real-time system panel**: Memory (MEM), CPU usage (CPU bar + %), and Battery (BAT) all dynamically linked to FreeRTOS runtime statistics on the far right of the header
- **FreeRTOS task introspection**: `ps` / `tasks` / `top` list every task (name, state, priority, core, stack high-water mark) with per-task CPU% since the last sample; `top` sorts by CPU descending by default, all three accept `dir /O:`-style sorting (`/O:N` name, `/O:C` CPU, `/O:S` stack, `/O:P` priority, `/O:T` state, `-` reverses), `/b` emits machine-parsable rows for pipes, and they set ERRORLEVEL for batch use. Read-only.
- **Header CPU sparkline**: the header's CPU panel shows a small history graph of recent CPU samples (bars turn amber above the warn threshold) instead of a single-value bar; toggle it off with `P4_CONFIG_HEADER_CPU_GRAPH=0`.
- **Debug history**: 5-entry error/warning buffer surfaced via `debug` command

## Command Set

See [command.md](command.md) for the complete command reference. Quick overview:

| Category | Commands |
|----------|----------|
| **System** | `help`, `sysinfo`, `clear`/`cls`, `reboot`, `version`/`ver`, `about`, `debug`, `mem`, `ps`/`tasks`/`top`, `screenshot`/`scr`/`capture`, `apps`, `launch` |
| **Serial transfer** | `receive <path> <size> [/crc]` (host→device, ACK-paced, optional CRC-32), `send <path> [offset] [count]` (device→host framed stream), `send /diag` (diagnostic report) |
| **TUI / graphics** | `draw box\|line\|fill\|text\|bar\|table\|list\|clear\|window\|cursor\|hold\|alt-screen\|close\|refresh\|fullscreen`, `tui status\|clear\|fullscreen\|refresh`, `color`, `locate`, `anchor`, `gfx init\|close\|status\|clear\|pixel\|line\|rect\|circle\|hline\|vline\|triangle\|ellipse\|polygon\|fill\|text\|show\|load\|blit\|free\|slots\|save`, `plot tui\|window\|auto\|axes\|func\|polar\|para\|data\|bar\|table\|line\|point\|clear\|status` |
| **Files & assets** | `open <file>` (dispatch by type), `view`, `hexview`, `image info\|show <file.bmp>`, `crc32 <path>`, `asset check\|list <app>`, `pkg list\|info\|verify\|check\|install\|remove` |
| **Background jobs** | `start <command> [args]`, `taskkill <job>` |
| **Data / state** | `db …` (incl. `/field:` `/sort:`), `export <db\|alarms> <csv\|json\|txt> <file>`, `csv rows\|cols\|cell\|eval`, `crypt lock\|unlock`, `ini get\|set\|del\|list\|load\|save`, `appconfig <app> …`, `temp new\|clean`, `json validate\|pretty`, `markdown <file>\|-e <text>\|on\|off` |
| **Alarm / calendar** | `alarm add\|list\|del\|enable\|disable\|status\|purge`, `cal today\|next\|YYYY-MM` |
| **Presentation** | `font info\|coverage\|list\|set\|size`, `theme list\|show\|set`, `header [status]\|mode\|show\|hide`, `cursor [block\|bar] [blink <ms|off>]` |
| **Hardware** | `brightness`, `rotate`, `battery`, `volume [<0-100>]`, `beep`, `tone <freq> [ms]`, `wavplay <file>`, `audio status|stop`, `gpio list|status|read|set`, `power`, `sleep`, `deepsleep`, `pwm <pin> <freq> <duty>`, `freq <pin> <hz>`, `adc <pin> [samples]`, `i2c scan|peek|poke`, `spi status`, `rgb <r> <g> <b>` / `#RRGGBB` / `<effect>` / `auto` |
| **Storage** | `cd`/`chdir`, `dir`, `copy`, `move`, `del`/`erase`, `ren`/`rename`, `md`/`mkdir`, `rd`/`rmdir`, `type`, `write`, `append`, `touch` |
| **Volume** | `chkdsk`/`scandisk`, `format`, `label`, `attrib`, `xcopy` (full `/S /E /I /Y /-Y /D /H /R /K /C /Q /T /F /L /A /M /U /P /W /N /V` switch set) |
| **Disk / partitions** | `disk list`, `disk detail`, `disk clean`, `disk create partition primary [size=N]`, `disk delete partition N`, `disk format` |
| **SD Tools** | `sd info`, `sd ls`, `sd stat`, `sd cat` |
| **Wi-Fi** | `wifi status|scan [/b]|diag|connect|disconnect`, `wifi known|save|forget|clear known|preferred` |
| **Connectivity** | `ping <host-or-ip> [count]`, `dns <hostname>` (alias `nslookup`), `httpget <url> [localfile]` (alias `wget`), `tcpterm <host> <port> [/t:secs] [text...]` |
| **Network services** | `httpd start|stop|status` (SD HTTP file server), `netstat`, `ipconfig` |
| **Bluetooth** | `bluetooth status|scan [limit]|advertise <on [name]|off>`, `bt` (alias) |
| **USB** | `usb status|ls|keyboard on|off|mouse on|off`, `usb userial status|open|close|send|recv|term` |
| **Batch** | `set`, `set /a`, `set /p` (incl. `set /p NAME=< file`), `calc`, `path`, `echo on|off`, `call`, `if` (incl. `if defined`), `for` / `for /f`, `goto`, `shift`, `pause`, `choice`, `setlocal`, `endlocal`, `exit [/b]`, `alias`, `unalias`, `bind`, `delay <ms>`, `notify`, `dialog`, `list`, `ask` |
| **Text tools** | `find`, `findstr`, `more`, `tree`, `fc`, `comp`, `sort`, `csv`, `prompt`, `clip`, `paste`, `history` |
| **Time / SNTP** | `date` `[MM-DD-YYYY]`, `time` `[HH:MM[:SS]]`, `timezone` `[TZ]`, `sntp`/`ntpsync` `[sync]`, `timer`/`stopwatch` `start|stop|lap|status` |
| **Redirection** | `>`, `>>`, and `<` to and from SD files |
| **Pipes** | `cmd1 | cmd2 | cmd3` (up to 4 stages) |
| **Chaining** | `a & b`, `a && b`, `a || b` (up to 8 commands) |
| **Quoting** | `"grouped"`, `'literal'`, `^` escapes |
| **OTA** | `c6ota sd:/path|http[s]://url|default` |

## Build and Flash

```sh
# Host-side serial/screenshot/SD harnesses live in tools/harness/
# (grab_screenshot.py, capture_tui.py, wifi_*.py, test_wifi*.py).
# Source ESP-IDF environment
$env:IDF_PATH = "C:\esp\v5.5.5\esp-idf"
. $env:IDF_PATH\export.ps1

# Build
idf.py build

# Flash and monitor
idf.py -p <COM_PORT> flash monitor
```

### Build Constraints

- LVGL examples disabled (esp32p4 image budget)
- Station-only Wi-Fi profile
- Newlib nano formatting
- Warn-level compile-time logging
- PSRAM XIP instruction/rodata mapping disabled
- ESP-Hosted reset policy: `SLAVE_RESET_ON_EVERY_HOST_BOOTUP`

## Documentation

| File | Purpose |
|------|---------|
| [readme.md](readme.md) | Project overview, architecture, build instructions |
| [changelog.md](changelog.md) | Version history with categorized changes |
| [documentation.md](documentation.md) | Technical architecture and module layout |
| [ai-context.md](ai-context.md) | Project rules and constraints for AI-assisted development |
| [command.md](command.md) | Complete command reference |
| [tutorial_edit.md](tutorial_edit.md) | Complete `edit` editor tutorial (every feature) |
| [editor.md](editor.md) | `edit` editor quick reference |
| [roadmap.md](roadmap.md) | Future parity goals and delivery phases |
| [licence.md](licence.md) | Proprietary notice + third-party license summary |
| [API.md](API.md) | Public module API reference |
| [SDK.md](SDK.md) | Module integration guide with examples |
| [board_config.yaml](board_config.yaml) | Hardware configuration single source of truth |
| [apps/companion/README.TXT](apps/companion/README.TXT) | The on-SD sample batch app (system helper) |
| [apps/companion/FINDINGS.md](apps/companion/FINDINGS.md) | On-board testing log: bugs found + fixed, feature gaps |

## License

Project-authored code: Copyright (c) 2026 Stoian Alexandru. All rights reserved.

Third-party components remain under their original licenses (Apache 2.0, MIT, BSD). See [licence.md](licence.md) for details.
