# P4MiniShell Command Reference

Complete reference for all shell commands available in P4MiniShell.

> **Applies to firmware v1.1.0** (ESP-IDF v5.5.5, ESP32-P4 + ESP32-C6). This is
> the authoritative command reference; the on-device `help /all` mirrors it.
> Current verified test baselines live in [`test/README.md`](test/README.md).
> Related docs: [`readme.md`](readme.md) (overview),
> [`documentation.md`](documentation.md) (architecture),
> [`SDK.md`](SDK.md) (integration).

## ANSI/VT Color Support

Every command uses the same built-in colour scheme by default. There is nothing to
configure: the palette is compiled in and applied automatically on both the LVGL
transcript and the serial console.

Colours are defined once in `components/ansi/ansi_palette.h` as named `SH_*` macros.
Commands compose output from those names rather than choosing colours themselves, so
the whole shell stays consistent and a palette change is a single-file edit.

### Default colour scheme

| Category | Colour | Palette macro |
|----------|--------|---------------|
| Section headings and titles | Bright green | `SH_HEAD` |
| Sub-headings | Bright yellow | `SH_SUBHEAD` |
| Field labels and keys | Cyan | `SH_LBL` |
| Body text | Bright green | `SH_TEXT` |
| Muted, secondary, timestamps | Bright black (grey) | `SH_MUTE` |
| Success, connected, OK | Green / bright green | `SH_OK`, `SH_OK_HI` |
| Errors and failures | Red / bright red | `SH_ERR`, `SH_ERR_HI` |
| Warnings | Yellow / bright yellow | `SH_WARN`, `SH_WARN_HI` |
| Important values (SSID, IP, MAC) | Bright white | `SH_VAL` |
| Numbers, sizes, percentages | Bright magenta | `SH_NUM` |
| Paths and filenames | Bright blue | `SH_PATH` |
| Prompt and command names | Bright cyan | `SH_PROMPT`, `SH_CMD` |
| Usage syntax | Yellow | `SH_USAGE` |
| Help descriptions | White | `SH_DESC` |
| Directory entries | Bold bright blue | `SH_DIR` |
| File entries | White | `SH_FILE` |
| Executable / `.bat` entries | Bright green | `SH_EXE` |
| Listing sizes | Bright magenta | `SH_SIZE` |
| Listing timestamps | Grey | `SH_TIME` |
| Wi-Fi connected / disconnected | Green / grey | `SH_NET_UP`, `SH_NET_DOWN` |
| Bluetooth | Magenta | `SH_BT` |
| USB attached / detached | Green / grey | `SH_USB_UP`, `SH_USB_DOWN` |
| OTA progress and success | Bright green | `SH_OTA` |
| Reset | Default | `SH_RST` |

### Where colour shows up

- **`dir` and `sd ls`** colour each entry by kind: directories in bold bright blue,
  runnable `.bat` files in bright green, ordinary files in white, with grey timestamps
  and magenta sizes. `dir /b` is deliberately left uncoloured so redirected or piped
  output stays machine-parsable.
- **Status commands** (`sysinfo`, `wifi status`, `usb status`, `chkdsk`) use cyan
  labels with bright-white values and bright-magenta numbers.
- **Errors, warnings, and usage lines** are consistent across every command, because
  they all go through the same helpers.
- **`c6ota`** shows progress in bright green and the destructive YES confirmation in
  bright red.

### ANSI format specifiers

These are the low-level codes the palette macros expand to. Prefer the `SH_*` names in
new code; these are listed for reference and for the rare case that needs a raw colour.

| Specifier | Meaning |
|-----------|---------|
| `@R` | Reset all attributes |
| `@B` | **Bold** (not blue) |
| `@D` | Dim |
| `@I` | Italic |
| `@U` | Underline |
| `@k` | Foreground black — **near-invisible on the dark background; use `@K` for grey** |
| `@r` | Foreground red |
| `@g` | Foreground green |
| `@y` | Foreground yellow |
| `@b` | Foreground blue |
| `@m` | Foreground magenta |
| `@c` | Foreground cyan |
| `@w` | Foreground white |
| `@K` | Foreground bright black (grey) |
| `@E` | Foreground bright red |
| `@G` | Foreground bright green |
| `@Y` | Foreground bright yellow |
| `@L` | Foreground bright blue |
| `@M` | Foreground bright magenta |
| `@C` | Foreground bright cyan |
| `@W` | Foreground bright white |
| `@@` | A literal `@` |

Standard printf specifiers work alongside these, including flags, width, and
precision: `@c%-10s@R` and `@M%8.2f@R` behave as expected.

## UI Model

- Fixed top header bar with status icons (Wi-Fi, Bluetooth, USB, SD) and system panel (MEM, CPU, BAT) dynamically linked to FreeRTOS
- Scrollable transcript (LVGL span group) for coloured command output (read-only). It keeps
  a multi-screenful history and jumps to the output of the command you just submitted.
  Scrolling: drag on the transcript, the input-row `Up`/`Dn` buttons, USB keyboard
  `PageUp`/`PageDown` (one viewport per press), or the USB mouse wheel
  (`P4_CONFIG_TRANSCRIPT_SCROLL_STEP` pixels per notch).
- Single-line input textarea with prompt for command entry
- On-screen LVGL keyboard attached to input line
- Prev/Next buttons for 10-command recall history
- Serial console: idf.py monitor accepts same commands via UART/USB-Serial-JTAG
- Command submission: LV_EVENT_READY on input line (touch) or Enter (serial)
- Heavy commands run on dedicated worker task (not LVGL input callback stack)
- Native modal surfaces (`dialog`, `list`, `ask`, `browse`, `view`, `hexview`) take over the transcript area
  using the shared modal runtime in `components/modal/`. All 6 modals fill the live transcript region (resizes with rotation and keyboard visibility), accept `/t:secs`, and work both interactively and in batch — the `dialog`/`list`/`ask` dispatcher was fixed to handle both paths. Batch files remain the
  apps; these commands are the polished UI layer a batch app can drop into.

## Where Commands Live

| Command group | Implemented in |
|---------------|----------------|
| `help`, `sysinfo`, `version`/`ver`, `about`, `mem`, `debug` | `components/shell/shell.c` (`debug save` export lives in `components/command/command.c` beside `history /save`) |
| `cd`/`chdir`, `dir`, `tree` | `components/storage/storage_nav.c` |
| `copy`, `move`, `del`/`erase`, `ren`/`rename`, `md`/`mkdir`, `rd`/`rmdir`, `type`, `write`, `append`, `touch`, `undelete`, `trash`, `attrib`, `label`, `xcopy` | `components/storage/storage_files.c` |
| `find`, `findstr`, `more`, `fc`, `comp`, `sort` | `components/storage/storage_text.c` |
| `chkdsk`/`scandisk`, `format` | `components/storage/storage_disk.c` |
| `sd info|ls|stat|cat|mount|eject`, `sdeject`, `disk` family | `components/storage/storage_fam.c` + `storage.c` |
| `set`, `calc`, `path`, `echo`, `call`, `if`, `for`, `goto`, `shift`, `pause`, `choice`, `setlocal`, `endlocal`, `exit`, `delay`, `notify`, `appmode` | `components/batch/batch.c` + `components/batch/batch_expr.c` (`set /a` evaluator) + `components/batch/calc.c` (TUI/modal verbs dispatch from `components/command/`) |
| `dialog`, `list`, `ask`, `browse`, `view`, `hexview` | `components/command/tui_commands.c` + `components/modal/modal_surf.c` (dispatched from `components/command/command.c`) |
| `draw` (`box`/`line`/`fill`/`text`/`clear`/`window`/`close`/`refresh`/`fullscreen`) | `components/tui/tui.c` (`tui_draw_box` `components/tui/tui.c:241` title+style via `tui_cell_set` `components/tui/tui.c:129` `utf8[4]` `components/tui/tui.h:35` single `SH_BOX_TL`/`H`/`V` double `SH_BOX_TL2`/`H2`/`V2` rounded `SH_BOX_TLR`/`TRR`/`BLR`/`BRR`, `tui_draw_line` `components/tui/tui.c:296` single/double/heavy, `tui_fill` `tui_print_at`, `tui_flush` `components/tui/tui.c:620` recolor `#RRGGBB` per fg run `ansi_get_palette_color`, `tui_enter_fullscreen` `components/tui/tui.c:417` / `windows_set_fullscreen` `components/windows/windows.c:418`, `windows_notify_keyboard_visibility` → `windows_refresh_tui_surface`, `tui_hide_for_modal`) + `components/command/command.c` dispatcher (auto-enters TUI for box/text/line/fill/clear/window `tui_init` `components/tui/tui.c:56`) |
| `tui` (`status`/`clear`/`fullscreen`/`refresh`) | `components/tui/tui.c` (`tui_status` rect `1024x510` cols `80` rows `25` `p4minishell_config.h:325`, `tui_enter_fullscreen`/`tui_exit_fullscreen` `components/tui/tui.c:417`, `tui_refresh_surface` `components/tui/tui.c:408`, `tui_flush` `components/tui/tui.c:620`) + `components/windows/windows.c` (`windows_enter_tui_mode` keeps header visible by default, `windows_set_fullscreen`/`header_set_visible` `components/windows/windows.c:418` hides header only on fullscreen, `windows_notify_keyboard_visibility` / `windows_refresh_tui_surface` `components/windows/windows.c:312`, `tui_hide_for_modal`) |
| `color`, `locate`, `tui`, `draw`, `anchor` | `components/command/tui_commands.c` + `components/tui/tui.c` (`tui_set_default_color` `color` DOS parity, `tui_set_cursor` `locate` DOS parity, both TUI-aware via `tui_cell_set` `utf8[4]` and `tui_flush` recolor `#RRGGBB` per fg run) |
| `gfx` (`init`/`close`/`status`/`clear`/`pixel`/`line`/`rect`/`circle`/`hline`/`vline`/`triangle`/`ellipse`/`polygon`/`fill`/`text`/`show`/`load`/`blit`/`free`/`slots`/`save`) | `components/command/gfx_commands.c` + `components/gfx/gfx.c` (pure RGB565 raster, `gfx_font.c` 8x8 ASCII font) |
| `plot` (`tui`/`window`/`auto`/`axes`/`func`/`polar`/`para`/`data`/`bar`/`table`/`line`/`point`/`clear`/`status`) | `components/command/plot_commands.c` + `components/gfx/gfx_view.c` (pure viewport math) + `components/batch/calc.c` (expression sampling) |
| `beep`, `tone`, `wavplay`, `audio`, `volume` | `components/command/audio_commands.c` + `components/audio/audio.c` (parsing here, codec/playback in `audio`) |
| Batch file execution, `:label` scanning, `for` loops, `\|` pipes, setlocal scoping | `components/batch/batch.c` |
| Keypress wait (`pause`, `choice`, `more`) and the `prompt` template engine | `components/shell/shell.c` |
| `brightness`, `rotate`, `battery`, `power`, `sleep`, `deepsleep` | `components/command/power_commands.c` |
| `gpio`, `pwm`, `freq`, `adc`, `i2c`, `spi`, `rgb`, `camera` | `components/command/periph_commands.c` (+ board GPIO table/gate) |
| `screenshot`, `receive`, `send` | `components/command/serial_commands.c` |
| `display`, `keyboard`, `windows` (UI query) | `components/command/command_ui.c` (dispatched from `components/command/command.c`) |
| `config` (persistent settings / CONFIG.SYS + factory reset) | `components/command/config_cmd.c` |
| `crc32`, `asset check|list` + package install/verify (`pkg`) | `components/command/asset_commands.c` + `components/command/pkg_commands.c` |
| `db` (record store verbs, incl. `/field:`/`/sort:`) | `components/command/db_commands.c` (dispatched from `components/command/command.c`; field parser in `components/db/db.c`) |
| `alarm`/`cal` (alarm + calendar verbs) | `components/command/alarm_commands.c` (dispatched from `components/command/command.c`) |
| `gfind` (Palm-style global find over db + alarms) | `components/command/gfind_commands.c` (dispatched from `components/command/command.c`) |
| `export` (portable store interchange) | `components/command/export_commands.c` (dispatched from `components/command/command.c`) |
| `csv` (grid substrate) | `components/command/csv_commands.c`; pure parser `components/storage/storage_csv.c` |
| `crypt` (password file encryption) | `components/command/crypt_commands.c` (mbedTLS AES-256-GCM) |
| `usb userial` (CDC-ACM serial) | `components/command/userial_commands.c` → byte API in `components/usb/userial.c` |
| `timer`/`stopwatch` | `components/clock/clock_timer.c` + `clock_commands.c` (dispatched from command.c) |
| `bind`/`unbind` (F-key bindings; shell hook) | `components/batch/batch.c` + `components/shell/shell.c` key path |
| `reboot`, `clear`/`cls`, `prompt`, `launch`, `apps` | `components/command/command.c` |
| `date`, `time`, `timezone`, `sntp`/`ntpsync` | `components/clock/clock_commands.c` (dispatched from command.c) |
| `wifi`, `bluetooth`/`bt`, `usb`, `c6ota`, `httpd`, `netstat`, `ipconfig` (family routing) | `components/command/command.c` → owning module |
| `ping`, `dns`/`nslookup`, `httpget`/`wget`, `tcpterm` (dispatched here, implemented in networking) | `components/command/command.c` → `components/networking/` |

Every command is reached through the single dispatcher `shell_execute_command_core()` in
`components/command/command.c`. `main/main.c` contains no command implementations. It routes
LVGL input events into `shell_execute_command_async()`, which queues the line on the command
worker task.

## System Commands

| Command | Description |
|---------|-------------|
| help | Show built-in command list; `help /all` prints the full offline reference; `help <command>` prints one entry |
| sysinfo | Show board, display, storage, heap, FreeRTOS tasks, uptime, Wi-Fi, and OTA state (includes build date/time and Git hash) |
| clear / cls | Clear transcript history and redraw prompt |
| reboot | Restart the board |
| version / ver | Show app banner, version, build date/time, Git hash, IDF version, chip info, uptime, heap, and task count |
| about | Show shell and board summary with build metadata, header description, uptime, task count, the copyright/MIT notice, and a third-party license summary |
| debug | Show last 5 error/warning entries, Wi-Fi state, heap, warning count (`debug save [file] [txt\|csv\|json]` exports the ring for `tools/parse_debuglog.py`) |
| mem | Show free heap, total heap, minimum heap, internal heap, task count, PSRAM state |

### apps

List the registered native apps (the applib ABI table). Each row shows the
command name and its one-line description. Native apps are C functions linked
into the firmware and registered via `app_register()`; invoking one dispatches
to it with `argc`/`argv` (see `SDK.md`, "Native-app ABI"). ERRORLEVEL: 0.

### launch

Discover, list, and run the script apps installed on the shell. An app is any
`*.bat` or `*.cmd` file in a PATH directory or in the conventional `sd:/APPS`
directory (up to `P4_CONFIG_LAUNCH_MAX` entries, `.bat` first per directory);
its optional metadata lives in `sd:/APPS/<name>.APPINFO` (INI format:
`title=`, `description=`, `version=`), shown when present. Typing an app name runs it
directly (extensionless names probe `.bat` then `.cmd`); `launch` does the
same by name.

- `launch` — list the installed apps as a numbered menu and run the chosen one.
- `launch <name>` — run an app by name (PATH resolution, then `sd:/APPS`).
- `launch /list` — bare list (`name  -  title`), for scripting.

ERRORLEVEL: 0 ok, 1 not found, 2 usage.

Example (the companion app carries an APPINFO file):
```
launch /list        ->  COMPANION  -  P4 Companion
launch COMPANION    ->  runs COMPANION.BAT
```

The boot flow can offer an app automatically: a CONFIG.SYS `LAUNCH_APP=<app>`
directive asks `run <app> now? (Y/N)` once after AUTOEXEC.BAT and launches it
on `Y` (a timeout declines). Manage it with `config LAUNCH_APP=<app>` /
`config reset LAUNCH_APP`.

### help [command | /all]

`help` prints a quick command summary. It is also a full offline command
reference:

- `help /all` (or `help /?`) — prints every built-in command with a one-line
  usage/description, so the reference is always available without a network or
  a host doc.
- `help <command>` — prints a single entry, e.g. `help wifi`; an unknown name
  prints an error and points at `help /all`.

The entries mirror the authoritative `command.md` reference in this repository.

### about — license and third-party notice

`about` adds a **License** section that surfaces the copyright/MIT notice
(`P4_CONFIG_COPYRIGHT_NOTICE`) and a **Third-party components** summary naming
the principal open-source components and their SPDX identifiers (ESP-IDF,
LVGL, esp_hosted, esp_wifi_remote, FreeRTOS, lwIP, FatFs, protobuf-c, USB host
stack). The project's own license is in [`LICENSE`](LICENSE) and
[`licence.md`](licence.md); full third-party license texts ship in the
project's `managed_components/` directories.

### Header long-press

Pressing and holding anywhere on the top status bar shows a transient banner
with the build identity: `P4MiniShell v0.32.7 | built <date> <time> | git
<hash>`. The same identity is reported by `version`, `about`, and `sysinfo`.

### header — responsive status bar

The header (WiFi/BT/USB/SD status, notification center, MEM/CPU/BAT system
panel, uptime) lays itself out responsively: every render measures the live
content and fits the three panels into the display width with no overlap.
When space runs out it abbreviates labels (`WiFi HI`→`W:HI`, `MEM 12.3M`→
`M12.3M`), drops the CPU sparkline/bars and separators, may step the header
text to a smaller font, and hides the notification center before
dropping any indicator. Portrait rotations compact automatically.

Status indicators are **color-coded** in both styles. The default
`P4_CONFIG_HEADER_STATUS_STYLE=glyph` shows one compact letter per indicator
(`W BT U S` and `M C B`), colored by state — green healthy, amber degraded,
red off/failed, muted absent (SD slot empty) — which returns the reclaimed
width to the notification area. Set `status_style=0` to restore the verbose
words (`WiFi HI`, `USB ON`). An `A` activity indicator appears only while a C6
OTA or a background job is running. **Tap** an indicator to show a one-line
detail in the notification area; **long-press** to run that subsystem's status
command (`wifi status`, `bluetooth status`, `usb status`, `sd info`, `ps`,
`mem`, `top`, `battery`).

The center shows the local clock (`HH:MM`, `--:--` until SNTP syncs) when there
is no active notification. Notifications queue FIFO and are colored by severity
(info/warn/error), so an alarm or OTA alert is not lost behind a later trivial
message. The header poll is adaptive (roughly 150 ms while the display is off,
1 s while an OTA/job runs, 2 s idle), with expensive telemetry still sampled at
5 s.

| Form | Meaning |
|------|---------|
| `header` / `header status` | Mode, height, visibility, font step, content levels, and wanted vs actual panel widths. |
| `header mode [auto\|full\|compact] [/save]` | Layout density (`auto` = fit, `full` = prefer full labels, `compact` = abbreviated); `/save` persists to `SHELL.INI`, restored at boot. |
| `header show\|hide` | Show/hide the bar (rebuilds the layout). |

CONFIG.SYS accepts `HEADER=ON|OFF` and `HEADER_MODE=AUTO|FULL|COMPACT`.
ERRORLEVEL: `0` ok, `2` usage.

### ps | tasks | top [/b] [/O:key]

Read-only FreeRTOS task introspection. `ps` and `tasks` print a colour-coded
table; `top` prints the same table plus a summary line with the live task
count, free heap, and uptime. Each row shows:

- **Name** — FreeRTOS task name
- **Sta** — state (`RUN` running, `RDY` ready, `BLK` blocked, `SUS` suspended, `DEL` deleted)
- **Prio** — current priority
- **Core** — pinned core, or `-1` when the task has no core affinity (unpinned)
- **HeadB** — stack high-water mark: the minimum free stack bytes remaining
  since the task was created (lower = closer to a stack overflow)
- **CPU%** — the task's CPU share since the previous `ps`/`top`/`tasks` call,
  computed by diffing FreeRTOS run-time counters

`/b` emits uncoloured machine-parsable rows (`name state prio core headb cpu`)
suitable for redirection and pipes, e.g. `ps /b > tasks.txt`.

**Sorting** — `/O:` uses the same switch syntax as `dir`:

| Key | Sorts by |
|-----|----------|
| N | Name (default tie-breaker) |
| C | CPU % |
| S | Stack high-water |
| P | Priority |
| T | State |

Prefix with `-` to reverse, e.g. `/O:-C`. Bare `/O` sorts by name. `top`
defaults to CPU descending (real `top` behaviour); `ps`/`tasks` keep the
FreeRTOS order unless `/O:` is given.

All three verbs set an ERRORLEVEL: 0 ok, 2 usage (e.g. an unknown `/O:` key),
so `top && echo ok`, `if errorlevel 2`, and `top /b /O:-C | findstr /V IDLE`
work in batch files and pipes.

The command is read-only — it never suspends, deletes, or reprioritises tasks.

## Hardware Commands

### brightness <0-100>
Set LCD backlight brightness through the display manager's PWM path (BSP LEDC).
Uses `display_set_brightness()` from `components/display/`.

### rotate <0|90|180|270>
Rotate display and remap GT911 touch orientation to match.
Uses `display_set_rotation()` from `components/display/`. Triggers full UI rebuild
via the window manager's `windows_deinit()` + `windows_init()` cycle through the
registered callback. Touch remapping is handled automatically by the display manager.

### display info
Show comprehensive display information from the display manager: resolution, rotation,
brightness, refresh rate, power state, panel driver, touch driver, timing parameters,
buffer configuration, and MIPI DSI lane info. Uses `display_get_info()` / `display_print_info()`.

### display resolution
Show current effective and native display resolution. Uses `display_get_resolution()`.

### display refresh
Show current display refresh rate configuration. Uses `display_get_refresh_config()`.

### display power <on|sleep|off>
Control display power state. Uses `display_set_power_state()`. Sleep/off turns off
backlight; on restores it.

### display stress <on|off>
Force continuous full-screen LVGL redraws (invalidate `lv_screen_active()` every
`P4_CONFIG_DISPLAY_STRESS_PERIOD_MS`) to reproduce DSI-underrun display glitches.
Used with `tools/display_glitch_watch.py --stress` to verify the "BSOD" fix
(see changelog). Off by default; not persisted.

### windows info
Show window manager layout information: display dimensions, region rectangles for
header, transcript, input row, and keyboard. Uses `windows_get_rect()` and
`windows_get_display_width()`/`windows_get_display_height()`.

### keyboard show|hide|toggle|status|mode
Control the on-screen keyboard visibility. `hide` removes the keyboard and
expands the transcript area; `show` restores it. `toggle` switches between
visible and hidden. `status` reports current visibility, mode, height, and the
external-input state (`external=on` while a USB keyboard is attached and the
OSK is auto-hidden). Tapping the input line — or the transcript, when the OSK
is hidden and no USB keyboard is attached — summons the on-screen keyboard.

`keyboard mode` prints the current page (`keyboard.page=<name>`) and
`keyboard mode <page>` switches it. Pages: `text_lower` (alias `letters`),
`text_upper` (`caps`), `number` (`num`), `symbols` (`special`), `nav`, and
`nav2` (`edit`). The two `nav` pages are the editor control pages; the editor
selects `nav` itself when it opens (see [editor.md](editor.md)). The page is
**not persisted**: the touch keyboard always starts on the letters page at boot,
and every ready-made modal (`dialog`/`list`/`ask`/`browse`/`view`/`hexview`/
`image`/`form`) resets it to letters when it opens.

`keyboard nav [on|off]` controls the symbols/edit page **`Nav` key
availability**. The `Nav` key is only useful where the editor navigation page
is reachable, so it is greyed out (disabled, and it never fires) in every other
context — the shell prompt, batch apps, non-editor modals. The shell enables it
automatically while the editor is open; batch apps request it explicitly:

```
keyboard nav on     rem enable the Nav key while my app drives the editor pages
...                 rem use the navigation page
keyboard nav off    rem release it
```

Requests are reference-counted and ORed with the shell context, so nested
requesters are safe. `keyboard nav` (no argument) prints `keyboard.nav=on|off`.
The `ui state` line also reports the current page's key as `nav=on` (usable),
`nav=off` (greyed), or `nav=na` (page has no Nav key).

The on-screen symbols keyboard covers every printable ASCII character
(0x20-0x7E), including the shell-critical pipe `|`, caret `^` (the shell
escape character), tilde `~`, and backtick, so DOS operators and escaped
characters can be typed directly. Use the `1#` / `abc` mode buttons to switch
between text and symbols. The shell input row also has a `Tab` button that runs
the same completion as the USB `Tab` key.

### ui tap|longpress|swipe|press|move|release|key|target|targets|hit|state
Synthetic touch automation and UI inspection (firmware `components/uitest/` +
`components/command/ui_commands.c`). A second LVGL pointer indev is driven by
the command, so `ui` taps go through the normal hit-testing/event path — the
same code a finger exercises.

- `ui tap <x> <y> [ms]` — press+release at a display pixel.
- `ui longpress <x> <y> [ms]` — hold past the LVGL long-press time (default 800).
- `ui swipe <x1> <y1> <x2> <y2> [ms] [steps]` — paced drag then release.
- `ui press <x> <y>` / `ui move <x> <y>` / `ui release` — low-level hold/drag.
- `ui key <label> [ms]` — tap an on-screen-keyboard key by its label.
- `ui targets [/b] [/v:NAME]` — list every tappable widget as
  `id x y w h name` (buttonmatrices expand to one row per key, `kbd:<label>`).
  A trailing ` [C]` marks a clipped widget that extends outside an ancestor's
  bounds (e.g. an off-view scroll-list row) and therefore is not visible or
  tappable where reported.
- `ui target <id>` — activate a listed target (its own click/value event).
- `ui hit <x> <y> [/v:NAME]` — report the target id under a point.
- `ui state [/b] [/v:NAME]` — active modal, keyboard visibility/mode, editor
  document state, the shell input-line text, the inline `ghost=` completion
  suffix, the active `search=` reverse-search query, and the current page's
  `nav=on|off|na` capability-key state.

Batch-friendly: `ERRORLEVEL` 0/1/2, `/b` bare output, `/v:NAME` result
variables, redirection. While a modal blocks the command worker a `ui` line is
handled by the console-reader task, so the editor/dialog/list UI can be driven
over serial (this is what `tools/ui_touch_test.py` uses). Reference batch demo:
`apps/uitest/UITEST.BAT`.

### font info | font coverage | font list | font set | font size
Font roles and live switching (`components/font/` registry: terminal role
for monospace surfaces, UI role for chrome with a Montserrat icon fallback,
and a **reading** role for the `view` viewer and the editor Markdown
preview, where proportional/serif faces are allowed).
`font info` shows roles, line heights, fallback state. `font coverage`
prints labeled glyph rows whose serial bytes are exact. `font list` shows
built-ins plus `sd:/FONTS/*.ttf|*.otf` with per-role `name@px`.
`font set <terminal|ui|reading> <name> [/save]` switches live (`/save`
persists to CONFIG.SYS, restored at first SD mount; terminal refuses
proportional fonts and sizes that break the 80x25 grid, naming the max that
fits; reading defaults to the proportional UI face until an SD serif TTF is
selected). The 80-column TUI grid automatically falls back to the committed 8 px
extended unscii (`components/tui/tui_font_8.c`, generated by
`tools/gen_tui_font.py`) whenever the active terminal font's cell advance would
overflow the transcript width; the shell transcript itself keeps the terminal
font. `font size <terminal|ui|reading> <px 10..28> [/save]` resizes TTF-backed
roles (bitmaps refuse — they are fixed-size). Missing/corrupt TTFs fail with
an errorlevel and never disturb the current fonts; without SD the built-ins
carry the full UI. NotoSansSC auto-attaches as a CJK fallback tail whenever
present (never a selectable terminal primary). Known limits: stb picks one
cmap subtable, so U+2600/2601 miss via the SC tail (DejaVu covers them);
very long CJK rows can wrap-mangle ~3 chars (short lines are exact). The
reading role ships a serif face (`DejaVuSerif` in `assets/fonts/`, pushed to
`sd:/FONTS/`; auto-selected at first mount) plus reader line spacing
(`P4_CONFIG_READING_LINE_SPACING`).
### theme list | show [name] | set <name> [/save]

Switch the UI color theme. Four built-ins ship: `default` (forest green CRT,
pixel-identical to the compiled palette), `amber` (amber phosphor), `ice`
(cool blue), and `mono` (neutral grey). Each table holds the chrome colors
(screen/transcript/input-row/keyboard backgrounds, accent/body/muted/warn
text, header panel tints, modal border/title/message) and the font roles.

- `theme list` — one line per built-in (the active one marked `*`).
- `theme show [name]` — print the active table (or a named one): colors as
  `RRGGBB` and the font roles/sizes.
- `theme set <name> [/save]` — swap the active table and re-apply it live to
  the screen/transcript/input row, the keyboard, and the header. `/save`
  writes the choice to `sd:/APPS/SHELL.INI` (`theme=<name>`) and it restores
  at the next boot's first SD mount. Modal surfaces pick up the table when
  they next open.

Fonts stay under the `font` verb; `theme set` changes colors only. ERRORLEVEL:
`0` ok, `1` unknown theme (the active theme is unchanged) or a `/save` that
could not write, `2` usage. Companion UI: **Settings ‣ Theme**. The
`sd:/APPS/THEME.INI` file format is reserved for a future per-key override;
today the choice lives in `SHELL.INI`.

### cursor [block|bar] [blink <ms 0..2000|off>]
Input-line cursor style (session-only): block (editor-like full cell) or
thin bar, plus blink period (`off`/0 = steady). Blink also retimes an open
editor. Defaults from `P4_CONFIG_CURSOR_BLINK_MS` (shared with the editor
timer). USB keyboard: arrows move, Ctrl+arrows word-jump, Home/End jump.

### markdown <file> | markdown -e <text> | markdown on | off | markdown export <src> <out> [text|html|print]
Render Markdown (CommonMark-ish subset: headings, nested lists, quotes,
fences, GFM tables with alignment, task lists, bold/italic/strike/code,
links as `text (url)`) through the ANSI SGR pipeline, so spans style it on
screen (bold/italic via DejaVu TTF variants with bright fallback,
underline/strike decor) and serial terminals render it natively.
`markdown on|off` toggles auto-render of `echo`/`type` output (default on).
Auto-render is per-line and flanking-safe: `2 * 3`, `foo_bar`, and `*ptr`
pass through untouched (no closer, no style); `echo /raw ...` bypasses per
invocation. Batch scripts with literal `#`/`*` lines should use `/raw`.
`type` renders `.md` per line (tables align only in document mode, i.e. the
`markdown` command, which buffers full tables). Tables measure display width
(ASCII 1, CJK 2) so CJK columns align.

**`markdown export <src> <out> [text|html|print]`** writes `src` to `out` as a
portable document (writerdeck): `text` renders then strips SGR (the same
plain-text bridge the viewer uses); `html` writes a standalone
`<!DOCTYPE html>` reader page (embedded CSS, `<title>` from the source
basename, body = headings, lists, quotes, fences, bold/italic/strike/code,
links, escaped text) for sharing without extra assets; `print` lays the text
 out in fixed pages
 (`P4_CONFIG_PRINT_COLUMNS` x `P4_CONFIG_PRINT_ROWS`, default 80x60) with a
 `title   Page N` header and a form feed between pages — a PDF-less document for
 printing. Sources past 64 KB are refused, not truncated
 (`markdown export: ... is larger than 64 KB - not exported`); output that
 would expand past `P4_CONFIG_MD_EXPORT_MAX_BYTES` (256 KB) is likewise
 refused unwritten. The write is atomic (`storage_write_text_file`). Pass
 exactly one format (`text|html|print`); naming two is a usage error. An
 exported `.html` serves over `httpd`
 (`httpd start`, then `http://<ip>/<name>.HTML`). ERRORLEVEL: `0` ok,
 `1` open/write/refused failure, `2` usage/unknown format.

### markdown / spellcheck / templates (writerdeck)

The writing-oriented surfaces share one theme:

- **Focus / typewriter** — `edit <file> /focus` hides the header and keyboard
  and keeps the caret centred (toggle `Ctrl+Shift+F` / `Focus` / `\focus`).
- **Word count** — the editor status bar shows `W n` live.
- **Reading typography** — `view` (for `.md`) and the editor's Markdown
  preview render in the `reading` font role (proportional/serif allowed):
  `font set reading <name>` / `font size reading <px>` (persisted like the
  other roles). A proportional **serif** face (`DejaVuSerif`, vendored in
  `assets/fonts/`) is auto-selected at first SD mount when present, and
  `P4_CONFIG_READING_LINE_SPACING` adds reader line spacing. The terminal role
  stays monospace-only.
- **Spellcheck** — see below.
- **Templates** — see below.

### Spellcheck

The editor underlines misspellings using an offline wordlist on the SD card.
The default is `sd:/DICTS/<name>.words` (`<name>` = `P4_CONFIG_SPELL_DICT_NAME`,
default `en`): one lower-case word per line. A curated `en` sample ships in
`apps/dicts/` — push it with `python apps/push_dicts.py <COM_PORT>`. Toggle
with the nav/edit `Spell` key, `Ctrl+Shift+S`, or the serial verb `\spell`;
the status bar shows `SPELL` while on. With no dictionary present the toggle
reports the expected path and stays off. The wordlist is loaded once per
session into PSRAM (read through an internal DMA bounce buffer, since PSRAM
is not DMA-capable) and bounded by `P4_CONFIG_SPELL_MAX_BYTES` /
`P4_CONFIG_SPELL_MAX_WORDS`; lookups themselves accept any token length,
single-character words included. Tokenization is UTF-8 aware: in-word
`'`/`-` (plus U+2019) keep `don't`/`well-known` whole (a contraction passes
when its parts do, so `don't` needs `don`); tokens with CJK, digits, `_`, or
non-ASCII Latin letters are never flagged, since the wordlist cannot cover
them. Underlines compose with word-wrap. `P4_CONFIG_SPELL_ENABLE=0` compiles
the engine out and makes the toggle report that it is disabled.

### Document templates

New-file seeds live in `sd:/TEMPLATES/<name>.MD`. `edit <file> /template <name>`
starts a **new** buffer pre-filled from that template (the target file must not
already exist — an existing file is never overwritten). Templates ship in
`apps/templates/` (`LETTER.MD`, `NOTE.MD`, `LOG.MD`) and are pushed with
`python apps/push_templates.py <COM_PORT>`. The `WRITER` reference app
(`apps/writer/WRITER.BAT`) exercises the flow without an interactive session
(template seed → build → `markdown` render → all three `markdown export`
formats → spell-wordlist check → share over `httpd`) and prints the
interactive commands (`/focus`, preview, `Spell`) it cannot drive itself.

### config [KEY=VALUE | KEY value | save | reset [key] | factory]
Read and write the persistent settings stored in `sd:/CONFIG.SYS` — the same
file the boot component parses at startup, so a saved setting is re-applied on
every boot with no extra boot code.

- `config` — show every tracked setting: current value, factory default, and
  whether it is saved in CONFIG.SYS.
- `config BRIGHTNESS` — show one setting.
- `config BRIGHTNESS=40` (or `config BRIGHTNESS 40`) — apply the setting now
  and write `BRIGHTNESS=40` into CONFIG.SYS. A value may span several tokens
  (e.g. `config PROMPT PS $p$g`).
- `config save` — persist the current value of every tracked setting.
- `config reset` — restore every tracked default in RAM and remove all
  tracked directive lines. `config reset <key>` does the same for one key.
- `config factory` — full factory reset: restores every default, clears
  history/aliases/known-networks, and deletes CONFIG.SYS, AUTOEXEC.BAT,
  WIFI.KNOWN, ALIASES.BAT, and HISTORY.TXT (the default boot files are
  regenerated at the next boot). Gated by the same interactive "YES"
  confirmation as `format` and is refused from batch files.

Tracked settings and their CONFIG.SYS directive:

| Key | Directive | Default | Applies to |
|-----|-----------|---------|------------|
| `BRIGHTNESS` | `BRIGHTNESS=0-100` | `P4_CONFIG_DISPLAY_DEFAULT_BRIGHTNESS` | backlight |
| `ROTATE` | `ROTATE=0\|90\|180\|270` | `0` | display rotation |
| `VOLUME` | `VOLUME=0-100` | `P4_CONFIG_VOLUME_DEFAULT_PCT` | speaker |
| `PROMPT` | `PROMPT=template` | `P4_CONFIG_PROMPT_DEFAULT_TEMPLATE` | prompt line |
| `WIFI_AUTOCONNECT` | `WIFI_AUTOCONNECT=ON\|OFF` | `ON` | watchdog auto-retry |
| `DISPLAY_TIMEOUT` | `DISPLAY_TIMEOUT=<secs\|OFF>` | `0` | idle display-off |
| `OSK` | `OSK=ON\|OFF` | `ON` | on-screen keyboard at boot |
| `HEADER` | `HEADER=ON\|OFF` | `ON` | header status bar at boot |
| `HEADER_MODE` | `HEADER_MODE=AUTO\|FULL\|COMPACT` | `AUTO` | header layout density at boot |
| `LAUNCH_APP` | `LAUNCH_APP=<app>` | *(none)* | offer to run a `.bat` app after boot |

CONFIG.SYS is edited in place with a guarded, atomic temp-file+rename write;
comments, blank lines, and unknown directives (e.g. `WIFI_SSID=`,
`GPIO ...`, `SET ...`) are preserved. `P4_CONFIG_CONFIG_MAX_BYTES` (16 KB)
bounds the file size the command handles. Errors set ERRORLEVEL 1, usage
errors 2; output is transcript-based, so `config > file` and pipes work.

### battery
Read battery ADC pin (GPIO53, 2:1 divider), show scaled voltage, estimated percentage (3.3V-4.2V range), raw ADC data, and light-sleep state.

### battery sleep <on|off|status>
Request or inspect light sleep. Only available when CONFIG_PM_ENABLE is enabled in sdkconfig.

### power [status] | power idle [seconds|off]
Report power-management state: PM enabled status, the automatic light sleep
request (`battery sleep on|off`), display power state, the idle display-off
timeout, Wi-Fi link state, battery level/voltage, the configured wake GPIO,
and the last sleep wake-up cause.

`power idle <seconds>` turns the display backlight off after that many seconds
without user input (touch, USB keyboard/mouse, or a serial command); `power
idle 0` / `power idle off` disables; `power idle` prints the setting. A fresh
touch, keypress, mouse wheel, or serial command wakes the display back to the
live shell. The timeout is capped at `P4_CONFIG_POWER_IDLE_DISPLAY_MAX_SECS`
and can be set at boot with the CONFIG.SYS `DISPLAY_TIMEOUT=` directive.
Returns an ERRORLEVEL (0 ok / 2 usage).

### delay <ms>
A pure, deterministic wait of `<ms>` milliseconds. Unlike `sleep` (light
sleep: blanks the display, tears down Wi-Fi), `delay` simply blocks the
command worker — the right tool for melodies, animations, and pacing in batch
files. Clamped to `P4_CONFIG_DELAY_MAX_MS`. In 100 ms chunks, so a
background job (`start`) stays killable during long waits. ERRORLEVEL:
0 ok / 1 stopped by `taskkill` / 2 usage.

```
delay 250
tone 523 180 & delay 450   (spaces notes out so they don't overlap)
```

### start <command> [args] | taskkill <job>
`start` runs a command line or batch file as a background job on a pooled
worker task (`bg0`..`bgN`, pool size `P4_CONFIG_BG_TASKS`), then returns
immediately — the shell stays interactive while the job runs. A resolvable
batch file runs as a script, anything else through the normal pipeline:

```
start ADVENT          (adventure game in the background)
start MOOD.BAT happy
taskkill bg0          (cooperative stop: batch lines and `delay` exit early)
```

Each job owns a private batch context (frames, errorlevel, `%*`, goto),
transcript defer slot, and storage redirect slot; env/alias writes stay
shared and locked. Modal/key-wait/appmode verbs (`ask`, `dialog`, `choice`,
`pause`, `view`, fullscreen TUI) refuse headlessly in a bg job instead of
blocking the shared UI. Stops are cooperative: `taskkill` sets the slot's
kill flag, checked per batch line and per 100 ms of `delay`; a job inside a
single long native command runs it to completion. Pool stacks live in PSRAM
(32 KB internal no longer fits at runtime), so `start` is refused while a
C6 OTA is pending and `c6ota` is refused while a job runs — flash writes
make PSRAM briefly inaccessible. ERRORLEVEL (`start`): 0 started /
1 pool full|unavailable|OTA pending / 2 usage. ERRORLEVEL (`taskkill`):
0 stop requested / 1 no such job|already finished / 2 usage.

### Background apps: services, alarms, calendar

A "background app" is an ordinary batch file started with `start` (or fired
by the alarm scheduler), so it runs on the background worker while the shell
stays live. The rules are the reverse of the display rule: background code
uses only **headless-safe** verbs — file/DB/INI/calendar I/O, `notify`,
`tone`/`beep`, `led`, `delay`, `if`/`for`/`goto` — and must never call
`draw`, `gfx`, `dialog`/`list`/`ask`/`browse`/`view`/`hexview`, `choice`,
`pause`, `anchor`, or `appmode` (they refuse in bg with ERRORLEVEL 1, or fall
back to a delay for `pause`/`choice`). Use `delay` (killable in 100 ms
slices) rather than `sleep`, which tears Wi-Fi down.

Two schedulers exist, and they compose:

- **`start`** — manual, immediate, one pooled slot (`bg0` here). Ideal for a
  standing service loop that polls. Reference: `apps/companion/SVC.BAT`
  (start with `start SVC`, stop with `taskkill bg0`); it ticks `cal next`
  every 20 s and reports through the shared transcript. Companion exposes it
  under **Live System ‣ Services** (Status / Start / Stop / List alarms /
  Run agenda now).
- **`alarm`** — time-based and persistent (`sd:/ALARMS`), polled every
  `P4_CONFIG_ALARM_POLL_MS` (30 s) with boot catch-up. `alarm add DATE TIME
  /run:APP.BAT [/daily|/weekly:mask|/monthly|/yearly] [/day:D] [/month:M]
  [/byw:N|last] [/beep] [/led] [/silent]` queues
  `call APP.BAT` on the command worker when the time arrives, so a batch
  file can be the action. `cal [today|week|next|YYYY-MM]` reads the same store.
  Reference job: `apps/companion/AGENDA.BAT` (`cal today` + `cal next` +
  `notify`), runnable foreground or scheduled.

`notify TEXT [/t:secs]` posts the header notification (bg-safe), and
`notify -` clears it. Because background stdout is deferred into the shared
transcript, a service can log with plain `echo`. One display writer at a
time is still the rule, so a background app has no UI of its own — it reacts
and logs; the UI belongs to the foreground shell and `draw list`/`draw
table` apps such as TCMD.

### sleep [seconds]
Enter light sleep. RAM is retained, so the shell resumes with all state
(env, aliases, cwd, variables) intact. With no argument the duration is
`P4_CONFIG_POWER_SLEEP_DEFAULT_SECS` (60 s); `sleep 0` clears the timer and
wakes only from an external source. Before sleeping, the display is blanked
and Wi-Fi/hosted state is torn down through `networking_wifi_shutdown()`.
On wake the wake cause is reported and the display is restored. When Wi-Fi
was connected before sleeping, the wake path re-establishes it automatically
in the background (`networking_wifi_request_wake_restore()`, the same request
shape as the post-OTA restore) instead of leaving a manual `wifi connect` —
the line-current palmtop behaviour. Light-sleep Wi-Fi teardown can be
disabled with `P4_CONFIG_POWER_LIGHT_SLEEP_SHUTDOWN_WIFI=0`.

Wake sources: the timer is always available; a user-wired button on
`P4_CONFIG_POWER_WAKE_GPIO` wakes via GPIO. Touch wake is not available on
this board because the GT911 interrupt line is not wired
(`BOARD_CFG_LCD_TOUCH_INT_GPIO = GPIO_NUM_NC`) — `sleep` reports this
honestly. For an always-on touch/USB-wake screen use `power idle` instead.

### deepsleep [seconds]
Enter deep sleep. RAM is lost, so on wake the device boots fresh (same path
as `reboot`). With `seconds` the chip wakes on a timer; without it, wake
requires an external wake source. Battery level is reported before sleeping.

### shutdown (alias poweroff)
Shut the board down: flush recall history and the wall-time anchor, stop Wi-Fi,
blank the panel/audio, darken every status LED, then cut power. On the M5Stack
Tab5 this pulses the PMIC power-off latch (PI4IOE5V6408 P4 on 0x44); on a board
with no software power latch (the JC1060P470 reference) it falls back to deep
sleep. Returns only if the latch fails (then it deep-sleeps anyway).

### imu [read] | imu status | imu rotate <on|off>
Reads the M5Stack Tab5 BMI270 six-axis IMU (`components/imu/`, SYS I2C 0x68).

- `imu` / `imu read` — prints acceleration (m/s²), angular rate (deg/s),
  pitch/roll, and the classified held orientation. It also publishes the sample
  to the environment (`IMU_AX/AY/AZ`, `IMU_GX/GY/GZ`, `IMU_PITCH`, `IMU_ROLL`,
  `IMU_ORIENT` = rotation degrees) so batch files can consume it.
- `imu status` — presence, orientation, and auto-rotate state.
- `imu rotate on|off` — enable/disable rotating the display as the board is
  turned (gravity-based, with hysteresis; off by default). A manual `rotate`
  does not disable auto-rotate.

On boards without an IMU every form reports the gap honestly (ERRORLEVEL 1).

### camera init | camera snap <file.bmp>
Powers and initialises the M5Stack Tab5 MIPI-CSI camera (SC202CS at SCCB 0x36)
through the managed `espressif/esp_video` stack. `camera snap` captures one
still and writes a **24-bit BMP** (the only format supported; non-`.bmp` names
are rejected). Live preview is future work. On boards without a camera the
command reports the gap (ERRORLEVEL 1); a detached camera module fails sensor
detection and reports NOT_FOUND.

### volume [<0-100>]
Set speaker volume through the ES8311 codec path. `volume` with no argument
prints the current level (`volume: <pct>%`); `volume <0-100>` sets it. All
audio output (`beep`, `tone`, `wavplay`) rides on this volume. Returns an
ERRORLEVEL (0 ok / 2 usage).

### beep
Play a short default tone (`P4_CONFIG_BEEP_FREQ_HZ` 880 Hz for
`P4_CONFIG_BEEP_DURATION_MS` 100 ms). Returns immediately (background
playback). ERRORLEVEL: 0 started, 1 busy, 2 usage.

### tone <freq> [ms]
Play a sine wave at `freq` (20..`P4_CONFIG_TONE_FREQ_MAX` Hz) for `ms`
(10..`P4_CONFIG_TONE_DURATION_MAX_MS`, default
`P4_CONFIG_TONE_DURATION_DEFAULT_MS`). Background playback with a short
fade-in/out. ERRORLEVEL: 0 started, 1 busy, 2 usage.

### wavplay <file>
Stream a short 16-bit PCM WAV from the SD card through the speaker. Mono or
stereo at 22050 or 44100 Hz are accepted (stereo is mixed to mono, 44100 is
decimated to 22050); other formats are rejected. Files are bounded by
`P4_CONFIG_WAV_MAX_BYTES`. Background playback. ERRORLEVEL: 0 started,
1 busy / file not found, 2 usage.

### audio status | audio stop
`audio status` reports `playing` or `idle`; `audio stop` cuts the current
background playback short (useful for a long tone or WAV). ERRORLEVEL 0/2.

### clip [text] | clip copy [N] | clip file <path> | clip read <file>
RAM clipboard for text, transcript lines, and files.

| Form | Effect |
|------|--------|
| clip | Print the clipboard (`clipboard: <text>` / `(file: <path>)` / `(empty)`). Redirectable: `clip > note.txt` saves it. |
| clip <text> | Set the clipboard to the text (`clip hello world`). |
| clip copy [N] | Copy the last N transcript lines (default 1, cap `P4_CONFIG_CLIP_COPY_LINES_MAX`) into the clipboard. |
| clip file <path> | Store a file reference in the clipboard (for `paste <dest>`). |
| clip read <file> | Load a text file's contents into the clipboard (bounded by `P4_CONFIG_CLIPBOARD_BYTES`). |

ERRORLEVEL: 0 ok / 1 empty, missing, or too-large / 2 usage.

### paste [<destination>]
`paste` inserts the clipboard into the input line at the cursor, so a copied
path or line can be edited and submitted immediately. With a destination
argument and a file-reference clipboard (`clip file`), `paste <dest>` copies
that file to the destination (a directory keeps the file's name) — file
copy-paste on the SD card. ERRORLEVEL: 0 ok / 1 empty or copy failure /
2 usage.

Examples:
```
clip copy 3          copy the last three transcript lines
paste                insert them into the input line
clip file CONFIG.SYS
paste backup\        copy CONFIG.SYS into backup\
clip read CONFIG.SYS
clip > out.txt       save the file text
```

### history | history /save [file] | history /load [file] | history /search <text> | history /clear
The recall history is heap-backed (up to `P4_CONFIG_COMMAND_HISTORY_DEPTH`
commands, capped at `P4_CONFIG_HISTORY_TOTAL_BYTES`) and Up/Down arrows recall
it. With `P4_CONFIG_HISTORY_AUTOSAVE` the profile (`P4_CONFIG_HISTORY_PROFILE`,
default `HISTORY.TXT`) is auto-loaded at the first SD mount and auto-saved 5 s
after the ring last changed; `reboot` flushes it before reset.

| Form | Effect |
|------|--------|
| history | List the numbered recall buffer (redirectable: `history > hist.txt`). |
| history /save [file] | Write the history to SD (default `P4_CONFIG_HISTORY_PROFILE`), atomic with partial-file cleanup. |
| history /load [file] | Restore history from SD (append, dedupe). |
| history /search <text> | Print matching entries (newest first, case-insensitive substring). Its own command line is skipped so the query cannot self-match. |
| history /clear | Clear the RAM history. |

ERRORLEVEL: 0 ok / 1 missing card or file or no match / 2 usage.

### Tab completion (USB keyboard)
Pressing **Tab** completes the current word at the end of the input line. The
candidate set comes from a **single source**: the shell help table. The first
token completes every help-listed command name, aliases, installed
`APPS/*.APPINFO` app names, and `.bat` files; any later token completes
subcommands/flags tokenized from that command's usage string, plus the
`launch`/`open`/`run` app names, `pkg` bundle names, and SD file/directory
paths (directories get a trailing `/`). A unique match fills in, repeated Tab
cycles up to `P4_CONFIG_COMPLETION_MAX_MATCHES` matches, and the first Tab with
several matches lists them in the transcript. Command lines accept up to
`P4_CONFIG_COMMAND_BYTES` (4096) characters.

### Ghost completion (typing ahead)
With `P4_CONFIG_COMPLETION_GHOST` the best completion's remaining suffix is
drawn muted immediately after the caret as you type; pressing **Right** or
**Tab** accepts it. The suggestion is SD-free (help table + aliases only, no
I/O), and is hidden when the caret is not at the end of the line, a modal owns
the screen, or the line already fills the visible width. It applies to the
shell input line only (not modal prompts).

### Ctrl+R reverse history search
Inside the shell input line, **Ctrl+R** starts an incremental,
case-insensitive search over the recall ring. Typed characters extend the
query (shown in a small overlay in the input row), the best/newest match fills
the line, **Ctrl+R** again cycles to older matches, **Enter** accepts the
current match, and **Esc** cancels and restores the draft. The same
substring test backs `history /search`.


### pwm <pin> <freq_hz> <duty_pct> | pwm stop <pin> | pwm status
Drive a non-reserved GPIO with an LEDC PWM signal. `pwm <pin> <freq_hz>
<duty_pct>` configures or updates the output (freq 1..`P4_CONFIG_PWM_FREQ_MAX_HZ`,
duty 0..100); `pwm stop <pin>` releases the channel and returns the pin to its
default; `pwm status` lists active outputs. The toolkit uses LEDC timers 0/2/3
and channels excluding the backlight's channel 1, with the same XTAL clock the
backlight auto-selects, so it never disturbs the display. Max concurrent
outputs: `P4_CONFIG_PWM_CHANNEL_MAX` (3). Reserved board lines are refused.

### freq <pin> <hz> | freq stop <pin> | freq status
Square-wave generator at 50% duty — the same LEDC engine as `pwm` at
`P4_CONFIG_PWM_DUTY_DEFAULT_PCT` (50). `freq stop <pin>` and `freq status`
share the `pwm` stop/status paths.

### adc <pin> [samples] | adc status
Read any ADC-capable, non-reserved GPIO with a fresh one-shot unit. `adc
<pin> [samples]` averages 1..`P4_CONFIG_ADC_MAX_SAMPLES` reads and reports the
raw code plus the calibrated voltage (0..~3.3 V at `P4_CONFIG_ADC_ATTEN`). `adc
status` live-samples every SOC ADC pin and lists only the ones that currently
read back, so a busy ADC unit is reported as unavailable. ADC1: GPIO16-23,
ADC2: GPIO49-54 (reserved lines skipped); pins whose ADC unit is held by
another driver get a clear "in use" error.

### i2c status | i2c scan [sda=.. scl=..] | i2c peek <addr> <reg> [sda=.. scl=..] | i2c poke <addr> <reg> <value> [sda=.. scl=..]
I2C bus tool over the `i2c_master` driver. With no pins, `i2c scan` / `peek` /
`poke` reuse the board's shared bus (SDA 7 / SCL 8, 400 kHz) through the BSP
handle, so a scan never conflicts with the GT911 touch; any other `sda=`/`scl=`
pin pair gets a temporary bus on a free port (clock
`P4_CONFIG_I2C_TOOL_CLK_HZ`). Addresses and registers accept hex (`0x68`).
`i2c scan` probes `P4_CONFIG_I2C_SCAN_FIRST_ADDR`..`_LAST_ADDR` using normal
device transactions (a short per-address timeout), so it is fast and does not
reprogram the shared controller. `peek` reads one byte, `poke` writes one byte.
Reserved pin pairs other than the board bus are refused.

### spi status
Reports the toolkit's SPI configuration (host, mode, clock, timeout). The
`loopback` / `peek` / `poke` verbs are recognized but return an honest
"unavailable on this board" error: initializing the SPI host on this P4 with
the ESP-Hosted SDIO link active stalls the chip and drops USB-Serial-JTAG off
the bus, so SPI transactions are deliberately not wired to the SPI master
driver. This follows the same explicit-failure policy as `camera`.

### rgb status | rgb off | rgb <r> <g> <b> | rgb #RRGGBB | rgb <effect> [speed] | rgb auto <on|off> | rgb <1|2> <r> <g> <b>
Controls the status LED(s). On the JC1060P470 that is the WS2812 (NeoPixel) on
GPIO26; on the M5Stack Tab5 it is the **two RGB LEDs on the keyboard module**
(I2C), driven by `components/led` through `components/tab5kbd`. `components/led`
owns the driver, a small animation task, and the auto status layer.

- `rgb status` — report driver pin, mode (auto/manual), effect, colour, speed,
  and brightness; on a two-LED board it also lists the independent LED2 colour.
- `rgb off` — turn the (primary) LED off.
- `rgb <r> <g> <b>` — solid colour, each channel 0-255 (switches to manual).
- `rgb #RRGGBB` — solid colour from a hex value.
- `rgb <effect> [speed]` — `rainbow`, `breath`, `pulse`, `blink`, or `solid`;
  speed 1 (slow) .. 10 (fast).
- `rgb auto <on|off>` — enable/disable the status-driven colour layer. In auto
  mode the LED follows Wi-Fi state (amber while connecting, green when
  connected, red blink when disconnected, red pulse on watchdog timeout) and
  flashes blue when the HTTP server starts. A green flash confirms boot.
- `rgb 1 <r> <g> <b>` / `rgb 2 <r> <g> <b>` — set one LED independently
  (Tab5 keyboard LEDs only). LED1 (index 1) is the status engine; LED2
  (index 2) is an independent user LED, off by default. `rgb 1|2 off` clears one.

**ERRORLEVEL:** 0 success, 1 failure (LED driver unavailable), 2 usage.
Works in batch files (`rgb 255 0 0 && echo led-red`, `if errorlevel 1 ...`) and
is redirectable/pipable. GPIO26 is reserved, so `pwm`/`freq`/`adc`/`i2c`/`spi`
refuse it.

CONFIG.SYS supports an `RGB=` directive: `RGB=<r>,<g>,<b> | #RRGGBB |
<effect>[,speed] | OFF | AUTO,<ON|OFF>` (commas separate arguments), applied at
boot through the `rgb` command.

### date [MM-DD-YYYY]
Show the full clock panel or set the system date. With no argument, prints the
local time, UTC time, Unix timestamp, timezone, uptime, and NTP sync status.
`date 08-11-2026` (also accepts `/` separators) sets the date; the C library
clock is updated and a later SNTP sync overrides it. Implemented in
`components/clock/clock_commands.c`.

### time [HH:MM[:SS]]
Show the full clock panel or set the system time. With no argument, prints the
same panel as `date`. `time 14:30:00` (or `14:30`) sets the time. Implemented
in `components/clock/clock_commands.c`.

### timezone [TZ]
Show the current POSIX timezone string, or set a new one. `timezone` prints the
active TZ and the local time; `timezone UTC` restores UTC, and `timezone
CET-1CEST,M3.5.0,M10.5.0/3` selects Central European Time with DST rules. The
local date/time re-renders immediately. Implemented in
`components/clock/clock_commands.c`.

### sntp / ntpsync [sync]
Show the NTP synchronization state or force a fresh exchange. `sntp` prints the
configured server, whether the clock is synced, and the local time. `sntp sync`
(alias `ntpsync sync`) restarts the SNTP client against
`P4_CONFIG_NTP_SERVER` (default `pool.ntp.org`); once Wi-Fi is connected the
clock jumps to the network time. Implemented in
`components/clock/clock_commands.c`.

### rtc [anchor]
RTC backup status: how wall time survives reboots without SNTP. The ESP32-P4
keeps its microsecond RTC counter across resets (and across power loss with a
VBAT coin cell); every anchor stores `{unix, rtc_us}` in NVS, replayed at
boot as `unix + (rtc_now - rtc_anchor)/1e6`. Anchors land on SNTP sync,
manual `date`/`time` set, `reboot`, and every `P4_CONFIG_RTC_ANCHOR_PERIOD_S`
(1 h). `rtc` prints the source (`none|anchor|stale|manual|sntp|ext|preset`),
validity (a counter reset without VBAT applies last-known time as *stale*,
honestly `--:--`), the live counter, anchor age, ext-chip presence, and SNTP
state; `rtc anchor` forces a write now. An optional external DS3231-class
I2C chip (`P4_CONFIG_RTC_EXT_*`, default off) is read first at boot and
rewritten on sync/set. Implemented in `components/clock/clock_rtc.c`.

### timer | stopwatch start|stop|lap|status [name] [/b] [/v:NAME]
Named stopwatch runs over `esp_timer_get_time()` (HP palmtop stopwatch parity),
owned by `components/clock/clock_timer.c` and surfaced by
`clock_commands.c`. Names default to `default`; up to
`P4_CONFIG_TIMER_SLOTS` (8) runs are held, and starting an existing run
restarts it from zero.

- `timer start [name]` — start (or restart) a run.
- `timer stop [name] [/v:NAME]` — freeze the run; `NAME` receives the elapsed
  milliseconds.
- `timer lap [name] [/v:NAME]` — record a split; `NAME` receives the running
  total.
- `timer status [name] [/b]` — print one run, or every run when no name is
  given. `/b` emits `name state ms laps` rows for `for /f`.

ERRORLEVEL: 0 ok, 1 no such slot / run / no timers, 2 usage. Headless-safe.

```
timer start build
delay 2500
timer stop build /v:MS      -> MS=2500
timer status /b
```

### gpio list
Show exposed board GPIO table with pin numbers, current levels, write policy, and role descriptions.

### gpio status
Show current levels and role text for all exposed board pins.

### gpio read <pin>
Read current logic level from any GPIO number.

### gpio set <pin> <0|1>
Drive a GPIO output. Only allowed for pins marked safe for writes in the shell pin table.

### screenshot [filename.bmp] (aliases: scr, capture)
Capture the current LVGL screen as a BMP image. Without a filename, streams the
BMP over the UART/USB-Serial-JTAG console with magic markers for host-side
extraction. With a filename, saves to the SD card with free-space precheck and
partial-destination cleanup on failure.

BMP format: 24-bit RGB888, bottom-up, no compression, 96 DPI. Pixel-perfect
capture of the entire screen (1024x600 = 1,228,800 bytes of pixels + 54-byte header).

**Usage:**
```
screenshot                  Stream BMP to serial with magic markers
screenshot shot.bmp         Save BMP to SD card (current directory)
scr shot.bmp                Alias form
capture shot.bmp            Alias form
```

**Serial streaming:** Without a filename the BMP is framed for host-side
extraction exactly like `send`: a 4-byte `BMPX` magic (`P4_CONFIG_SCREENSHOT_BMP_MAGIC`),
a 4-byte little-endian payload size, then the raw BMP bytes. The host reads the
magic, then the size, then exactly that many bytes. The console reader is
suspended for the duration and the bytes are written straight to the
USB-Serial/JTAG driver (no CRLF translation), so the frame is byte-exact.

**Screenshot while a modal is open:** the bare `screenshot` command is handled
by the console-reader task itself (via `shell_command_ops_t.modal_console_command`)
when a modal surface is active, because the modal blocks the command worker.
This lets the host capture `dialog`/`list`/`ask`/`browse`/`view`/`image show`/
`hexview` and the `edit` editor while they are on screen. Only the streaming
(no-argument) form is intercepted; `screenshot <file.bmp>` still runs on the
worker, so while a modal is open it is queued and executes when the modal closes.

**SD card save:** Uses the same storage path as `copy`, `write`, etc. Free-space
is prechecked, the session is guarded, and a partial destination is removed on
write failure.

**ERRORLEVEL:** 0 on success, 1 on failure (snapshot error, PSRAM exhaustion, SD
write error, invalid path), 2 on usage error (too many arguments).

### receive <path> <size> [/crc]
Push a binary from the host into an SD file over the USB-Serial/JTAG console.
The transfer is ACK-paced so the device's small USB RX ring never drops bytes:
the device prints `=== RX READY ===`, reads the payload in chunks, writes it to
the SD card, and echoes `RX <cumulative>` after each chunk; the host sends the
remaining delta based on those counts. `=== RX DONE ===` closes a successful
transfer. The console reader is suspended for the duration so the binary is
never mistaken for command lines.

**Usage:**
```
receive /sdcard/ota.bin 1203888          Push 1,203,888 bytes into ota.bin
receive /sdcard/file.bin 4096 /crc       Same, plus a CRC-32 trailer check
```

**`/crc`:** with this flag the host appends a 4-byte little-endian CRC-32
(IEEE 802.3, matching zlib's `crc32`) after the last data byte. The device
computes the CRC over what it received and compares; a mismatch removes the
partial file, prints a clear error, and sets ERRORLEVEL 1. Use it to guarantee a
byte-exact transfer.

The final size is bounded by `P4_CONFIG_SERIAL_RX_MAX_BYTES` (8 MiB). A host
that stops sending for `P4_CONFIG_SERIAL_XFER_IDLE_TIMEOUT_MS` (4 s) aborts the
transfer and removes the partial file.

**ERRORLEVEL:** 0 success, 1 transfer/IO error or CRC mismatch, 2 usage.

### send <path> [offset] [count]  (and: send /diag)
Stream an SD file (or a byte range of it) back to the host over the
USB-Serial/JTAG console, framed for host-side extraction:
`SDFX` magic + 4-byte little-endian payload size + raw bytes + a 4-byte
little-endian CRC-32 trailer (IEEE 802.3, matching zlib's `crc32` over the
payload) + `=== TX DONE ===`. The host reads the magic, then the size, then
that many bytes, then the CRC, and verifies the CRC to confirm the frame was
not corrupted or interleaved (empty payload => CRC `0x00000000`).

**Usage:**
```
send /sdcard/log.txt                   Stream the whole file
send /sdcard/fw.bin 0 65536            Stream the first 64 KiB
send /sdcard/data.bin 1024 4096        Stream bytes 1024..5119
send /diag                             Stream a diagnostic report
```

`<offset>` and `<count>` are clamped to the file size and to
`P4_CONFIG_SERIAL_SEND_MAX_BYTES` (16 MiB). `send /diag` streams a compact text
report (version, board, IDF, chip, heap free/internal/PSRAM, uptime, task count,
cwd, Wi-Fi state) so a host script can poll device health. The console reader is
suspended during the stream and the bytes go straight to the USB-Serial/JTAG
driver (no CRLF translation). Each raw write is time-bounded by
`P4_CONFIG_SERIAL_SEND_TIMEOUT_MS` (10 s), so a host that stops reading can
never hang the command worker — the send aborts with ERRORLEVEL 1.

**ERRORLEVEL:** 0 success, 1 IO error (SD absent, path invalid, open/size/read
failure), 2 usage.

**Host round-trip example (push a file with CRC, pull it back, verify):**
```batch
rem on the device
receive /sdcard/ota.bin 1203888 /crc
send /sdcard/ota.bin
```
The host sends the 1,203,888 bytes plus the 4-byte CRC trailer, then reads the
`SDFX` frame and compares it to the local copy; a match at both steps proves
the link and the SD card are byte-exact.

**Batch file example:**
```batch
receive /sdcard/fw.bin 4096 /crc
if errorlevel 1 echo FIRMWARE TRANSFER FAILED
if not errorlevel 1 echo FIRMWARE TRANSFER OK
```

**Batch file example:**
```batch
screenshot shot.bmp
if errorlevel 1 echo Screenshot failed
```

## DOS-Style File Commands

All file commands operate on SD card through guarded mount/unmount. Working directory and environment are RAM-only (not persisted across boots).

| Command | Description |
|---------|-------------|
| cd / chdir | Show current working directory |
| cd <path> | Change working directory (sd:/, /sdcard/, relative, ., ..) |
| dir [path] [options] | List directory entries (see the option table below) |
| copy <src> <dst> | Copy file on SD, with free-space and self-copy guards |
| move <src> <dst> | Move or rename file/directory |
| del / erase [opts] <path> | Delete or move to the recycle bin (see below) |
| ren / rename <src> <dst> | Rename file or directory |
| md / mkdir <path> | Create directory |
| rd / rmdir [opts] <path> | Remove directory; `/s` moves to the recycle bin |
| undelete <name\|index> | Restore a recycle-bin entry (alias: `restore`) |
| trash [subcommand] | Manage the recycle bin (alias: `recycle`) |
| type <path> | Print text-safe file preview (no raw binary) |
| write <path> <text> | Create or overwrite text file |
| append <path> <text> | Append text to file |
| touch <path> | Create empty file or refresh timestamp |

### Recycle bin (`del`, `rd`, `undelete`, `trash`)

`del`/`erase` and `rd /s` no longer delete outright. They move files and whole
directory trees into a hidden `.trash` folder (created with the hidden FAT
attribute and suppressed by default in `dir`, like DOS does), and record the
original path in a `.meta` side-car so entries can be restored exactly where
they came from.

- `del <path>` — move the file to the recycle bin.
- `del <pattern> [/s]` — move every matching file, optionally recursing.
- `rd /s <path>` — move the whole directory tree to the recycle bin.
- `/p` / `/f` / `/permanent` — bypass the bin and delete permanently.
- `undelete <name|index>` (or `restore`) — restore one entry to its original
  location; missing parent directories are recreated and an occupied
  destination is refused rather than overwritten.
- `trash list` / `trash info` — list entries or report count/size/oldest age.
- `trash restore <name|index>` — same as `undelete`.
- `trash purge <name|index>` — permanently delete one entry (requires `YES`).
- `trash empty` — permanently delete every entry (requires `YES`).

`del /s` and `rd /s` require the exact confirmation word typed at the prompt
before anything happens, and `del *.* /s` never recurses into `.trash` itself.
Limits (bytes / age / entry count) are enforced on every operation, purging
the oldest entries first. All of `del`, `rd`, `format`, `disk`, `undelete`,
and `trash` set an ERRORLEVEL: 0 success, 1 failure/cancelled, 2 usage.

### dir options

Options may appear before or after the path, and the `:` separator is optional.

| Option | Meaning |
|--------|---------|
| /W | Wide multi-column listing; directories shown as `[name]` |
| /P | Pause after each screenful; Enter or Space continues, `Q` quits |
| /S | Recurse into subdirectories, with a grand total at the end |
| /B | Bare listing, names only. Under `/S` prints full paths so it can be piped |
| /L | Lowercase names |
| /A:attrs | Filter by attribute (see below); bare `/A` shows everything |
| /O:order | Sort order (see below) |

**Attribute filters** for `/A`. Combine letters freely; prefix any letter with `-`
to exclude instead of require. With no `/A` at all, hidden and system entries
are suppressed (DOS behaviour) — this keeps the `.trash` recycle bin out of
ordinary listings; a bare `/A` shows every entry including hidden ones.

| Letter | Matches |
|--------|---------|
| D | Directories |
| H | Hidden |
| S | System |
| R | Read-only |
| A | Archive |

**Sort orders** for `/O`. Prefix with `-` to reverse. Name is always the
tie-breaker, so listings are deterministic.

| Letter | Sorts by |
|--------|----------|
| N | Name |
| S | Size |
| E | Extension |
| D | Date and time |
| G | Directories first, then the other key |

Examples:

```
dir                        Detailed listing with timestamps and free space
dir /w                     Four-column wide listing
dir /s /b                  Every file in the tree, full paths, pipeable
dir *.log /o:-d            Log files, newest first
dir /a:d /o:n              Directories only, sorted by name
dir /a:-h /p               Skip hidden entries, pause each screenful
dir logs\ /s /o:gs         Recurse, directories first then by size
```

Each directory prints its own file and directory counts. A `/S` run adds a
grand total, and every non-bare listing ends with the free space on the volume.
Listings are bounded to 128 entries per directory and recursion to 8 levels.

## Environment and Batch Commands

| Command | Description |
|---------|-------------|
| set | List all RAM-only environment variables |
| set NAME=VALUE | Create or update environment variable |
| set /a NAME=<expr> | Evaluate integer arithmetic and store the result |
| set /p NAME=<prompt> | Prompt the user and store the typed line |
| set /p NAME=<prompt> /T:secs | Prompt with a timeout; on timeout the variable is unchanged and errorlevel is 1 |
| set /p NAME=<prompt> /P | Password mode: the typed line is stored without being echoed |
| set /p NAME=< file | Read one line from the `< file` redirection / pipe source into NAME (cmd.exe behavior) |
| calc [NAME=] <expr> | Evaluate a floating-point expression with the BASIC math/string functions and print it (or store it in NAME) |
| calc /deg \| /rad \| /angle | Set or query the `calc` trig angle mode |
| calc /hex <expr> | Print an integral result as `&H` hex |
| calc /fin \| /date | Print the financial / date function cheatsheets |
| path | Show current batch PATH |
| path <dir1>;<dir2>;... | Replace PATH for .bat lookup |
| echo <text> | Print text after variable expansion |
| echo on / echo off | Enable/disable batch command echoing |
| call <file.bat> [args] | Execute batch file; `%0` is the script name, `%1..%9`/`%*` are the forwarded arguments, and the caller's errorlevel becomes the script's final errorlevel |
| call <file.bat>::<routine> [args] | Call one routine from a shared library of batch routines: execution starts at `:routine` in the external file, isolated from the caller's variables, and returns on `exit /b` / `goto :eof` / EOF |
| call :label [args] | Run a `:label` block in the current file as a subroutine; returns on `return` / `exit /b` / `goto :eof` / EOF |
| gosub :label [args] | BASIC-named sibling of `call :label` |
| gosub <file.bat>::<routine> [args] | BASIC-named sibling of `call <file.bat>::<routine>` |
| return [code] | Return from a `call`/`gosub` scope (or, at the top level of a file, end the frame like `goto :eof`); an optional code sets errorlevel |
| on <expr> goto <label>[,<label>...] | BASIC computed jump: the integer `expr` (via the `set /a` evaluator) selects the 1-based target; out of range falls through |
| on <expr> gosub <label>[,<label>...] | As above, but `gosub`-calls the selected target (`on … call` is an alias) |
| if [not] errorlevel N cmd | Run cmd when errorlevel is at least N |
| if [not] exist <file> cmd | Run cmd when the file or directory exists |
| if [/i] [not] "a"=="b" cmd | Run cmd when the strings match; `/i` makes the comparison case-insensitive |
| if [not] a EQU\|NEQ\|LSS\|LEQ\|GTR\|GEQ b cmd | Run cmd on a numeric comparison of `a` and `b` (parsed as decimal; non-numeric reads as 0) |
| goto <label> | Jump to a `:label` in the running batch file |
| goto :eof | Jump to the end of the current batch file, unwinding its open setlocal scopes |
| for %%v in (set) do cmd | Loop over literal tokens or a wildcard pattern |
| for /f "opts" %%v in (file-set) do cmd | Loop over the lines of a file (or the active `< file`/pipe input); options: `eol=c`, `skip=n`, `delims=xyz`, `tokens=a,b,m-n,*`; a single-quoted set `in ('command')` iterates command output |
| shift | Shift batch arguments left by one position |
| pause | Wait for a keypress |
| choice [/C:keys] [/N] [/T:c,secs] [/S] [text] | Wait for one of the listed keys |
| setlocal | Push a copy of the environment; later changes are local |
| endlocal | Pop the most recent setlocal scope |
| exit [code] | Leave every nested batch file, setting errorlevel |
| exit /b [code] | Leave only the current batch file |
| proc | List every active batch process (script, depth, args, echo state) |
| proc /args \| /name \| /depth \| /errorlevel \| /echo \| /stdin | Report the current batch process's arguments, name (%0), depth, exit code, echo state, or active pipe/`<` input source |
| ini list <file> | List every `KEY=VALUE` line in an INI file on the SD card |
| ini get <file> <key> | Print the value of `<key>` in an INI file |
| ini set <file> <key> <value> | Create or update `<key>` in an INI file |
| ini del <file> <key> | Remove `<key>` from an INI file (alias `delete`) |
| ini load <file> | Import every `KEY=VALUE` into the environment |
| ini save <file> | Export the whole environment to an INI file |
| appconfig <app> [path\|list\|get\|set\|del] [key] [value] | Per-app settings file `sd:/APPS/<APP>.INI` without hand-rolling parsing |
| ansi <sgr-codes> [text...] | Emit text styled with ANSI SGR codes (reverse video, bold, color) into the transcript |
| menu <item> [item...] | Render a numbered menu and read a numeric choice; ERRORLEVEL = chosen index (0 = cancel) |
| appmode on [/full] [/clear] | Enter app mode: save the screen, optionally full-screen + clear; restored automatically on exit /b |
| appmode off | Restore the saved screen and leave full-screen |
| appmode status | Show whether app mode is active |
| temp | Print the SD temp directory (`sd:/tmp`) |
| temp new [ext] | Create a unique SD-backed temporary file and print its path |
| temp clean | Delete every SD temporary file |
| db create <name> [/cr:XXXX] [/tp:XXXX] [/vr:N] | Create an SD-backed database (Palm-OS style) |
| db list \| db info <name> | List databases / show one database's header |
| db drop <name> | Delete an entire database |
| db open <name> / db close / db current | Set / clear / show the current database |
| db categories <name> list\|set <n> <label>\|clear <n> | Manage the 16 category labels |
| db add [<name>] [/cat:N] [/key:K] [/secret] [text...] | Add a record (or a file's contents); prints the id |
| db get [<name>] <id> [/b] [/reveal] | Read a record; secret payload redacted unless `/reveal` |
| db set [<name>] <id> [/cat:N] [/key:K] [text...] | Update a record (id unchanged) |
| db del [<name>] <id> [/p] | Soft-delete a record (`/p` = permanent) |
| db purge <name> | Physically remove soft-deleted records |
| db count [<name>] [/cat:N] | Count live records |
| db find [<name>] [/cat:N] [/key:K] [/text:P] [/b] | Linear scan; `/b` = bare `id\|cat\|key` |
| db export [<name>] [file] / db import [<name>] [file] | Dump / restore records as text |
| alarm add <YYYY-MM-DD> <HH:MM> [title] [/msg:..] [/daily\|/weekly:mask\|/monthly\|/yearly] [/day:D] [/month:M] [/byw:N\|last] [/beep] [/led] [/run:file.bat] [/silent] | Add an SD-persisted alarm |
| alarm snooze <id> [minutes] | Push the next due time (default 10, max 1440) |
| alarm list [/b] | List events (bare `id\|datetime\|title\|flags\|recur\|action` with `/b`) |
| alarm status | Store summary: count, enabled, next due, checker running |
| alarm enable <id> \| alarm disable <id> | Arm / disarm one event |
| alarm del <id\|all> | Soft-delete one event; `all` wipes the store |
| alarm purge | Physically remove soft-deleted events |
| cal today \| cal next \| cal YYYY-MM | Thin calendar view / next event / month count |
| alias | List every alias |
| alias name | Show one alias |
| alias name=value | Define (or update) an alias; `name=` clears it |
| alias /clear | Clear every alias |
| alias /save [file] | Write the alias table to the SD profile (default `ALIASES.BAT`) |
| alias /load [file] | Reload the alias profile from SD |
| unalias name | Remove one alias |

### alias / unalias — DOSKEY-style macros

Typing an alias at the prompt expands its leading word to the stored value
before the line is parsed, exactly like DOSKEY macros:

```
alias ll=dir /s
ll            ->  dir /s
ll *.txt      ->  dir /s *.txt
alias ls="dir /b"
ls            ->  dir /b
```

- Alias names are case-insensitive (stored uppercase), alphanumeric plus
  `_`, up to `P4_CONFIG_ALIAS_NAME_BYTES`; values up to
  `P4_CONFIG_ALIAS_VALUE_BYTES`; at most `P4_CONFIG_ALIAS_MAX` aliases.
- Expansion happens only at the interactive prompt — **never inside a batch
  file** — so an alias cannot shadow a batch verb (`set`, `call`, `echo`, ...).
- `alias /save` persists the table as `alias name="value"` lines to
  `sd:/ALIASES.BAT` (a batch file). boot.c auto-runs that profile after
  CONFIG.SYS, so saved aliases are restored every boot without editing
  AUTOEXEC.BAT. `alias /load` reloads manually. Values containing a double
  quote are skipped on save so the profile always round-trips.

### bind — USB key bindings (F-keys + Ctrl chords)

Binds a USB HID function key (F1..F12) or a Ctrl+letter chord (`^A`..`^Z`)
to a command line. Pressing the key submits its line onto the command
worker, exactly like typing it. Owned by `components/batch/batch.c` (the
same module as aliases) with the key hook in the shell.

```
bind F5 sysinfo
bind F6 launch tcmd
bind ^G macro play build.bat
bind                 list every bind
bind unbind F6       clear one
bind /clear          clear all
bind /save           persist to sd:/BIND.BAT
bind /load           reload the profile
```

- At most `P4_CONFIG_BIND_MAX` (12) keys shared between F-keys and chords;
  command lines up to `P4_CONFIG_BIND_VALUE_BYTES`. `^C` is reserved for
  the foreground break and can never be bound.
- F-keys fire only while the prompt is idle; chords additionally fire while
  a non-editor modal (dialog/list) owns the screen, so a macro hotkey works
  from anywhere except the editor (which keeps its Ctrl vocabulary) and
  key waits (`term`/`pause`/`choice` keep every key — a remote session
  always receives `^C` and friends). Never inside a batch file.
- `bind /save` writes `bind Fx <line>` / `bind ^X <line>` lines to
  `sd:/BIND.BAT`; boot.c auto-runs it after the alias profile, so saved
  binds restore every boot. Lines containing a double quote are skipped on
  save.

### macro — command macro recorder

Captures submitted command lines for replay as a batch file (the 95LX macro
story, DOSKEY-style recording with batch-file persistence):

```
macro record demo.bat
dir /b
calc 2^10
macro stop             ->  2 line(s) -> sd:/demo.bat
macro play demo.bat
macro status
```

- `macro record [file]` (default `MACRO.BAT`) captures every submitted line
  from both typed surfaces and touch-tap actions; `macro ...` control
  lines are never recorded. The buffer holds `P4_CONFIG_MACRO_BYTES`
  (4 KB); filling it auto-stops with an overflow mark.
- `macro stop` writes the capture atomically; `macro play <file>` runs it
  via `call`; `macro status` shows the state. Combine with chords:
  `bind ^G macro play build.bat` replays on one keypress, from the prompt
  or any non-editor modal.

### set /a — integer arithmetic

Evaluates a 32-bit signed integer expression. With an assignment the result is
stored; without one it is printed. Comparison and logical operators yield `1`
when true and `0` when false, so a boolean can be stored and tested later.
Intermediate overflow wraps (32-bit): scale heap-byte-sized values down
before multiplying — e.g. `set /a unit=%total% / 100` then
`set /a pct=%free% / %unit%` instead of `%free% * 100 / %total%`.

| Precedence | Operators |
|------------|-----------|
| Lowest | `\|\|` logical or |
| | `&&` logical and |
| | `==` `!=` `<` `>` `<=` `>=` comparison (1/0) |
| | `\|` bitwise or |
| | `^` bitwise xor |
| | `&` bitwise and |
| | `<<` `>>` shifts |
| | `+` `-` add, subtract |
| | `*` `/` `%` multiply, divide, remainder |
| | `-` `~` `!` unary minus, bitwise not, logical not |
| Highest | `( )` grouping |

Compound assignments are supported: `+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`,
`^=`, `<<=`, `>>=`.

Numbers may be decimal, `0x` hexadecimal, or leading-zero octal. A bare name
reads an environment variable, and **an undefined variable evaluates to 0**, so
a counter works on first use without initialising it.

```
set /a total=2+3*4          total=14
set /a total=(2+3)*4        total=20
set /a count+=1             Increment, works even when count is undefined
set /a mask=0xF0 << 4       mask=3840
set /a 100/7                Prints 14 without storing anything
set /a x=5==5               x=1   (comparison result)
set /a x=5!=5               x=0
set /a ok=(5>3)&&(2<4)      ok=1  (logical and)
set /a (1||0)               Prints 1
set /a x=17%%5              x=2   (%% is a literal % in a batch file)
```

**Quoting operators that the shell also uses.** `&&`, `||`, `&`, `|`, `<`,
`>` and `<<` / `>>` are command-chain, pipe, or redirection operators at the
shell level, so an expression containing them must be quoted (or the character
escaped) or the line is split before `set /a` runs:

```
set /a "x=5<6"              x=1
set /a "x=(%total%==14)&&(%total%>10)"
set /a "x=6&3"              x=2   (bitwise and)
set /a "x=5^3"              x=6   (^ is the escape character; ^^ inside
                                   quotes is a literal ^)
```

`==` and `!=` contain no shell operator, so they work unquoted (`set /a x=5==5`).
An expression with no assignment (for example `set /a 5==3`) is evaluated and
the result is printed. `set /a x==5` therefore prints the value of the
comparison `x==5` rather than assigning it; write `set /a x=(x==5)` to store
it. Both operands of `&&`/`||` are always evaluated (no short-circuiting),
matching cmd.exe, so `set /a "(0&&1/0)"` reports a divide-by-zero error.

Divide by zero, unbalanced parentheses, and malformed input are reported and
set errorlevel to 1 rather than producing a wrong answer.

### if — numeric comparison keywords

`if` accepts the cmd.exe-style numeric keywords in the form
`if [not] [/i] <operand1> <OP> <operand2> <command>`. The operands are parsed
as decimal integers (a non-numeric operand reads as 0), so the result of a
`set /a` computation can be branched on directly:

| Keyword | Meaning |
|---------|---------|
| `EQU` | equal to |
| `NEQ` | not equal to |
| `LSS` | less than |
| `LEQ` | less than or equal to |
| `GTR` | greater than |
| `GEQ` | greater than or equal to |

```
set /a n=42
if %n% EQU 42 echo n_is_42
if %n% GTR 10 echo n_is_large
if 5 LSS 3 echo never
if not 2 GTR 1 echo never
if abc EQU 0 echo non_numeric_reads_as_zero
```

`/i` is ignored for the numeric form (numbers have no case). The keyword form
is separate from the `==` string comparison, so `if a==b` stays a string test
while `if a EQU b` compares numerically.

**All `if` forms.** `if [not] [/i]` combines with any of these conditions:
`errorlevel N` (true when errorlevel ≥ N), `exist <path>` (file/dir exists),
`defined <name>` (variable is set, cmd.exe parity), the numeric keywords above,
and `"a"=="b"` string tests. An undefined variable in a numeric operand reads
as 0, matching DOS.

### set /p — prompted input

Prints the prompt and reads a line from the user.

```
set /p name=Enter your name:
set /p answer=Continue? 
```

Backspace edits, ESC cancels, Enter submits. **An empty line leaves the
variable unchanged**, matching DOS. Errorlevel is set to 1 when the input was
cancelled, timed out, or no interactive key source is attached, so a batch file
can branch on it. Over serial the full line is typed naturally and submitted
with Enter.

**Reading a file line (cmd.exe behavior).** When a `< file` redirection or a
pipe stage is active, `set /p` reads one line from that source instead of
prompting:

```
set /p line=< data.txt
echo line %line%
echo hello | set /p v=      ->  v = "hello"
```

The source belongs to exactly this command and is consumed once. An empty line
from the source leaves the variable unchanged, matching the interactive form.

**Timeout.** `set /p NAME=<prompt> /T:secs` prompts with a wait bound: after
`secs` seconds with no input the variable is left unchanged, `set` reports
"no input received" and errorlevel is 1. Without `/T` the default is
`P4_CONFIG_KEY_WAIT_TIMEOUT_MS`. The `/T:secs` token is stripped from the
displayed prompt.

**Password mode.** `set /p NAME=<prompt> /P` collects the line without
echoing it (for passwords and secrets) — Backspace erases silently, ESC
cancels, Enter completes. It combines with `/T:secs`:

```
set /p pass=Password: /P
set /p pin=PIN: /P /T:2
```

### Shared library of batch routines

An external `.bat` can act as a library of callable routines. Each routine is
a `:label` block; `call <file.bat>::<routine> [args]` starts execution at
that label and returns to the caller on `exit /b`, `goto :eof`, or end of
file. The routine receives the arguments as `%1`..`%9`/`%*` and its final
errorlevel propagates to the caller.

```
:add
set /a result = %1 + %2
echo ADD result=%result%
goto :eof

:shout
echo SHOUT %*
goto :eof
```

```
call lib.bat::add 3 4      ->  ADD result=7
call lib.bat::shout hello  ->  SHOUT hello
if errorlevel 2 echo failed
```

Routine calls are **isolated automatically** (variable isolation beyond
`setlocal`): the callee runs in its own environment scope, so a routine's
temporary variables (`result`, `tmp`, ...) never leak into the caller, and the
caller does not have to write `setlocal`/`endlocal`. `call <file.bat>` (whole
file) keeps the shared-environment behavior, matching DOS.

### Local subroutines and computed dispatch (`call :label`, `gosub`, `return`, `on`)

A `:label` block **in the current file** can be entered as a subroutine with
`call :label [args]` or its BASIC-named twin `gosub :label [args]`. The block
runs until `return [code]` (or `exit /b` / `goto :eof` / end of file) and then
execution resumes at the line after the call. `gosub <file.bat>::<routine>`
is the BASIC name for the shared-library call above, and `on … call` is an
alias of `on … gosub`.

`return` outside any `call`/`gosub` scope ends the current frame exactly like
`goto :eof` (so a file can be documented in BASIC terms without changing its
flow). A numeric argument sets errorlevel before returning.

`on` is BASIC's computed jump. The integer expression (evaluated by the same
engine as `set /a`) picks the 1-based target from a comma-separated list; an
out-of-range index **falls through** rather than erroring:

```
set n=2
on %n% goto :one,:two,:three   rem -> jumps to :two
on 1+1 gosub :sub,:sub         rem -> gosub-calls :sub
on 9 goto :a,:b                rem -> 9 is out of range, continues
```

```
:add
set /a sum=%1+%2
echo ADD %sum%
return 0

:main
gosub :add 3 4                 ->  ADD 7
echo sum=%sum%
```

A `goto` to a missing label prints `The system cannot find the batch label
specified - <name>` and **aborts the current batch file** (cmd.exe parity).
A `call`/`gosub`/`on` to a missing label sets errorlevel 1 and **continues**.

### Persistent state (`ini`, `appconfig`, `temp`)

DOS-like apps kept state in environment variables, temporary files, and simple
`KEY=VALUE` INI files. All of it lives on the SD card here.

**`ini`** reads and updates any `KEY=VALUE` file (comment lines starting with
`;`/`#`/`REM` and blank lines are skipped; writes are atomic):

```
ini set settings.ini theme dark
ini set settings.ini volume 70
ini get settings.ini theme        ->  dark
ini list settings.ini             ->  theme=dark / volume=70
ini del settings.ini volume
ini save state.ini                export the whole environment
set MYSTATE=hello
ini save state.ini                save it
set MYSTATE=
ini load state.ini                restore it
```

ERRORLEVEL: 0 ok, 1 missing file/key or I/O error, 2 usage.

**`appconfig`** gives each batch app its own namespaced settings file without
hand-rolling parsing — `sd:/APPS/<APP>.INI` (the `APPS` directory is created
on demand):

```
appconfig myapp set theme ocean
appconfig myapp get theme         ->  ocean
appconfig myapp set sound on
appconfig myapp                   ->  theme=ocean / sound=on
appconfig myapp path              ->  /sdcard/APPS/myapp.INI
appconfig myapp del sound
```

App names must be plain identifiers (no `/`, `\`, `.` or `..`). ERRORLEVEL
0/1/2.

**`temp`** manages SD-backed temporary files under `sd:/tmp`:

```
temp                ->  temp.dir=/sdcard/tmp
temp new csv        ->  temp.path=/sdcard/tmp/_app0.csv
temp new            ->  temp.path=/sdcard/tmp/_app1.tmp
temp clean          ->  temp: cleaned the SD temp directory
```

### Database (`db`) — Palm-OS-style SD record store

`db` is a named, SD-backed record store under `sd:/DBS/<name>.DB/`. Each
database has a header (`HEADER.INI`: name, creator, type, version), 16
categories (`CATEGORIES.INI`), a text index (`INDEX.TXT`), and one
`RECORDS/R<id>.DAT` file per record payload. Records carry a monotonic 32-bit
id (never reused), a 0..15 category, a flags byte, an optional key, and a
text/binary payload. Soft-deleted records stay in the index until `purge`.
All database data lives on the SD card; every write is atomic (temp+rename)
behind a free-space pre-check.

```
db create contacts /cr:APP /tp:CNTC /vr:7   create a database
db list                                     list every database
db info contacts                            header/statistics for one database
db drop contacts                            delete an entire database
db open contacts / db close                 set/clear the current database (RAM)
db current                                  show the current database
db categories contacts list                 list the 16 category labels
db categories contacts set 7 Family         set a category label (empty clears)
db categories contacts clear 7              clear a category label
```

Records:

```
db add contacts /cat:1 /key:alice Alice Smith    add a record; prints its id
db add contacts /cat:2 /secret p4ssw0rd          add a secret (redacted) record
db add contacts notes.txt                        payload from a file's contents
db get contacts 1                                read a record (secret payload
                                                 is redacted unless /reveal)
db get contacts 3 /reveal                        reveal a secret payload
db set contacts 2 /key:robert Robert Brown       update a record (id unchanged)
db del contacts 2                                soft-delete
db del contacts 2 /p                             permanent delete
db purge contacts                                physically remove soft-deleted
db count contacts [/cat:N]                       count live records
db find contacts [/cat:N] [/key:K] [/text:P]     linear scan; prints matches
db find contacts /b                              bare "id|cat|key" for /f
db find contacts /field:city=Paris               field filter (payload k=v)
db find contacts /sort:name                      sort by field (or key/id)
db get contacts 1 /field:name                    read one field of a record
```

**Record fields.** A payload can hold `name=value;name=value` pairs. The
`/field:` and `/sort:` options read them without a schema: `db add contacts
/key:a name=Alice;city=Paris`, then `db find contacts /field:city=Paris
/sort:name`. Names match case-insensitively, surrounding spaces are trimmed,
a value may not contain `;`, and segments without `=` are ignored — so a
fieldless payload (the default) still works and is searchable by `/text:`.
`db get <id> /field:name` prints just that value (bare with `/b`) for a
`for /f`/`set /p` pipeline, and sets ERRORLEVEL 1 when the record has no such
field. Sorting preserves insertion order for equal keys and is capped at
`P4_CONFIG_DB_FIND_MAX` matches.

```
db add contacts /key:b name=Amy /field-style payloads are just text
db find contacts /field:city=Paris /sort:name /b
db get 1 /field:name /b        -> Alice
```

When a current database is set (`db open`), the `<name>` argument is optional
and a bare numeric id is treated as a record id: `db get 5`, `db count`,
`db find /key:x` all use the current database.

Portability:

```
db export contacts [file]    dump live records to a text file
                             (default sd:/DBS/<name>.EXPORT), one per line
db import contacts [file]    append records from an export file (fresh ids)
```

Options can appear anywhere on the line: `/cat:N`, `/key:K`, `/text:P`,
`/cr:XXXX`, `/tp:XXXX`, `/vr:N`, `/secret`, `/reveal`, `/p`, `/b` (bare,
uncoloured, pipe/for-friendly output). ERRORLEVEL: 0 ok/found, 1 not-found /
empty, 2 usage / I/O error, so `db find ... && if not errorlevel 1 (...)` and
`for /f "tokens=1-3 delims=|" %%r in ('db find contacts /b') do ...` both work.
Every `db` operation opens its own guarded SD session and heap-allocates its
buffers (the batch path never gets a large stack local).

### export <db NAME | alarms> <csv|json|txt|vcf|ics> <file>

Portable interchange out of the structured stores through their existing APIs
(`db_find`/`db_get`, `alarm_list`) — no parallel readers. This is the ASCII
counterpart of the native `db export`: the 95LX saved every app file in both a
native and a text form. (`db` takes csv|json|txt|vcf; `alarms` takes
csv|json|txt|ics.)

```
export db contacts csv contacts.csv
export db contacts json contacts.json
export db contacts vcf contacts.vcf
export alarms csv alarms.csv
export alarms txt alarms.txt
export alarms ics alarms.ics
```

- `db` rows are `id,cat,key,payload` (CSV), `{id, cat, key, payload}` (JSON),
  or `#id cat=N key=K` blocks (TXT). Secret payloads ARE included — a backup
  is complete or it is useless. `vcf` writes one vCard 3.0 per record from
  the `k=v` fields (name/tel/email/org/note; FN falls back to the record key,
  NOTE to the whole payload).
- `alarms` rows carry `id,when,title,msg,recur,flags` (CSV/JSON) or a header
  line plus title/message (TXT). `ics` writes one VEVENT per event (floating
  local DTSTART, `RRULE` for daily/weekly recurrences).
- Files are written atomically (temp+rename) behind a free-space pre-check,
  bounded by `P4_CONFIG_DB_EXPORT_MAX_BYTES` (256 KB). CSV uses the same
  RFC-4180 quoting as the `csv` verb; binary payload bytes escape as `\u00XX`
  in JSON and print as `.` in TXT (keep binary records in the native
  `db export` form).

ERRORLEVEL: 0 exported, 1 empty store / over the cap, 2 usage / I-O.

### import db <name> <csv|json|vcf> <file> / import alarms <csv|json|ics> <file>

Portable interchange back into the stores — argument order mirrors `export`,
and the csv/json shapes match `export` output exactly, so an export
round-trips. Records and events always get fresh ids (the exported id column
is ignored); imported alarms arm notify-only and keep ENABLED (FIRED is
cleared).

```
import db contacts csv contacts.csv
import db contacts vcf contacts.vcf
import alarms ics alarms.ics
```

- `csv` accepts the `export` header (`id,cat,key,payload` / `id,when,title,
  msg,recur,flags`) or bare data rows, plus an optional 5th `secret` column
  for `db`. A row must be single-line: payloads holding raw newlines
  round-trip via `json` or the native `db export` form instead (such rows are
  skipped and counted, never truncated).
- `json` parses the `export` array shape (key order and extra keys tolerated;
  `unix` wins over `when` for alarms).
- `vcf` reads vCard 3.0 contacts (FN/N/TEL/EMAIL/ORG/TITLE/NOTE, continuation
  lines unfolded) into `name=..;tel=..;email=..;org=..;note=..` records;
  `;` in values becomes `,` (the `k=v` convention) and binary properties
  (PHOTO/...) are skipped.
- `ics` reads VEVENTs (`DTSTART`, `SUMMARY`, `DESCRIPTION`, `RRULE`
  FREQ=DAILY/WEEKLY with BYDAY); a trailing `Z` (UTC) is read as
  device-local time.
- Rows are capped by `P4_CONFIG_DB_EXPORT_MAX_RECORDS` (db) and the alarm
  store size (alarms); the summary reports `imported` vs `skipped`.

ERRORLEVEL: 0 imported, 1 nothing imported, 2 usage / I-O.

### archive create|extract|list|verify, backup

USTAR (`.p4a`) backups with a CRC manifest trailer — the 95LX backup story:
store-only POSIX tar (no compression, zero new dependencies, streamable,
host-extractable) plus a trailing `P4CRC.MANIFEST` member with one
`crc32 size path` line per file. Member paths stay relative under each
source's basename; names beyond the USTAR 100+155 split are skipped and
counted, never truncated; absolute or `..`-escaping members are refused on
extract; directory mtimes are stored for host fidelity but not restored on
the device.

```
archive create sys.p4a sd:/DBS sd:/ALARMS
archive list sys.p4a
archive verify sys.p4a
archive extract sys.p4a sd:/RESTORE
backup sys.p4a
```

- `archive create <file> <path> [paths...]` — streams to `<file>.tmp`,
  then renames over; free space is pre-checked from a size pre-walk.
- `archive extract <file> [dest]` (dest defaults to the cwd) — recreates
  directories, writes each member through temp+rename with a per-file
  space guard; the manifest is consumed, never materialized.
- `archive list <file> [/b]` — members with sizes (`/b` bare names).
- `archive verify <file>` — re-hashes every member against the manifest
  (foreign tars without one fail honestly).
- `backup <file> [paths...]` — `create` with a friendlier name; bare
  `backup <file>` archives `sd:/DBS` + the alarm store (absent sources
  skipped). (`restore` is NOT aliased — it already means trash-undelete.)
- Transfers reuse the existing verbs: an archive on SD downloads over
  Wi-Fi via `httpd`, pulls via `httpget`, moves over USB-serial via
  `send`/`receive`. Implemented in `components/archive/` (leaf) +
  `components/command/archive_commands.c`, capped by
  `P4_CONFIG_ARCHIVE_MAX_ENTRIES` (512) / `P4_CONFIG_ARCHIVE_CHUNK_BYTES`.

ERRORLEVEL: 0 ok, 1 nothing archived / CRC mismatch, 2 usage / I-O.

### crypt lock|unlock <src> <dst> [/p:pass | /ask]

Password file encryption — the memo-password analogue for the SD card.
AES-256-GCM with a key derived from the password via PBKDF2-HMAC-SHA256
(10 000 iterations, 16-byte salt, 12-byte nonce), implemented on top of the
IDF mbedTLS port in `components/command/crypt_commands.c`.

- `crypt lock secret.txt secret.lock /p:s3cr3t` — seal a file.
- `crypt unlock secret.lock secret.out /p:s3cr3t` — open it.
- `/ask` prompts for the password with no echo (refuses headless); an inline
  `/p:` password is masked in the transcript echo and command history (the
  `wifi connect` precedent).

The envelope is `P4CRYPT1` + salt + nonce + ciphertext + 16-byte tag. Files
stream in 4 KB chunks through internal (DMA-safe) buffers, so multi-megabyte
files never hand PSRAM pointers to FATFS. Writes are atomic; a failed run (or
a tag mismatch) removes the partial. A wrong password and a corrupt file are
reported identically (`wrong password or corrupt file`). The password buffer
and key are zeroed after every run.

```
crypt lock notes.txt notes.enc /ask
crypt unlock notes.enc notes.txt /p:s3cr3t
```

ERRORLEVEL: 0 ok, 1 password/IO failure, 2 usage.

### Alarm / calendar (`alarm`, `cal`) — SD-persisted events

`alarm` is a small, batch-friendly event store backed by the SD card
(`sd:/ALARMS/`): `INDEX.INI` plus one `E<id>.INI` per event (when, title, msg,
flags, recurrence, actions). A single background checker task polls the store
every `P4_CONFIG_ALARM_POLL_MS` (30 s by default) and, when an event is due,
reuses the EXISTING surfaces: the header notification area, the RGB LED, the
speaker, and — for the `/run:` action — the command worker (never the checker
stack). There is no private notification loop. Recurrence is one-shot, daily,
a weekly weekday bitmask, monthly (by day-of-month or by nth weekday), or
yearly.

```
alarm add 2030-01-01 09:00 Standup /msg:Team call /beep /led
alarm add 2030-06-02 08:00 Weekly /weekly:0x7F
alarm add 2030-03-15 09:00 Payday /monthly
alarm add 2030-03-01 09:00 Board /monthly /byw:2      2nd weekday of the month
alarm add 2030-03-01 09:00 Retro /monthly /byw:last   last weekday of the month
alarm add 2030-03-01 09:00 Renew /yearly /month:3 /day:1
alarm add 1970-01-01 00:00:01 Now /beep /run:sd:/APPS/NOTIFY.BAT
alarm list                     list events (enabled/fired + actions + recur)
alarm list /b                  bare "id|YYYY-MM-DD HH:MM|title|flags|recur|action"
alarm status                   count, enabled, next due, checker running
alarm enable <id> | alarm disable <id>
alarm snooze <id> [minutes]    push the next due time (default 10, max 1440)
alarm del <id>                 soft-delete (kept until purge)
alarm del all                  wipe the whole store (fresh id space)
alarm purge                    physically remove soft-deleted events
cal today | cal week | cal next | cal YYYY-MM   calendar grid / week agenda
```

Options may appear anywhere: `/msg:text`, `/daily`, `/weekly:mask` (7-bit
weekday bitmask, bit 0 = Sunday), `/monthly`, `/yearly`, `/day:1..31`,
`/month:1..12`, `/byw:1..5|last` (monthly nth weekday), `/beep`, `/led`,
`/run:file.bat`, `/silent` (suppress sound/LED, header notify only), `/b`.
`/day:`/`/month:`/`/byw:` require `/monthly` or `/yearly`; `/byw:` is
monthly-only and snaps the start date forward to the matching nth weekday.
Monthly by-day skips short months (`/day:31` fires only where a 31st exists);
yearly Feb 29 fires only in leap years. Titles/messages use normal quoting
(`"..."`, `^`). Times are parsed as local `YYYY-MM-DD HH:MM` (timezone-aware
via `mktime`, matching the `date`/`time` commands). ERRORLEVEL: 0 ok,
1 not found / none due, 2 usage / I/O.

`cal YYYY-MM` prints a classic `Su Mo Tu We Th Fr Sa` grid (event days marked
`*`) followed by the event list; `cal week` prints a 7-day agenda. Firing
behaviour: when an event becomes due, the checker marks it fired (one-shot)
or advances it to the next occurrence (recurring), persists, then notifies /
beeps / pulses the LED, and optionally queues `call <file>` onto the command
worker for the `/run:` action. Alarms that become due while the device is
powered off are fired once on the next boot (catch-up, `P4_CONFIG_ALARM_CATCHUP_ON_BOOT`);
light sleep suspends the checker until the device wakes. Accuracy is bounded
by the poll interval plus clock quality (SNTP helps).

### Menu / form primitives (`ansi` + `menu`, with `choice`)

DOS-style interactive apps were built from `choice` (single-key selection)
plus ANSI escape codes (colors, bold, reverse video). The primitives are
rendered in the shell transcript (the display area) and readable from touch,
USB keyboard, or serial.

**`ansi <sgr-codes> [text...]`** wraps the text in `ESC[<codes>m ... ESC[0m`
(the DOS `7m` and bare `7` spellings both work). Codes are the SGR
parameters: `7` reverse video, `1` bold, `31` red, `36` cyan, `90` muted,
`0` reset, or combinations like `1;36`:

```
ansi 1;36m **************************
ansi 1;36m *  P4 APP MENU           *
ansi 7m > SELECTED                  reverse-video highlight
ansi 0m
```

**`menu <item> [item...]`** renders a numbered form and reads a numeric
choice; ERRORLEVEL is the chosen item's 1-based index (0 on cancel / timeout
/ an invalid entry), so a batch app branches with `if errorlevel`:

```
menu "Start game" "Load save" "Settings" "Quit"
2
if errorlevel 2 echo Load save selected
```

`echo.` (the DOS blank-line idiom) is also recognized. A complete menu app
combines the primitives: an `ansi` banner, `echo.` spacing, `echo` items,
and a `choice /C:123` selection — see the `appmenu.bat` example in the
simulation.

### App mode (`appmode`)

A clean way for a batch app to take over the shell and hand it back:

```
appmode on /full /clear      save the screen, hide the shell input widgets,
                             and clear the transcript for a full-screen app
... print the app's full-screen UI (ansi, echo, menu, choice, ...) ...
appmode off                  restore the saved screen
```

`appmode on [/full] [/clear]` saves the current transcript (colours
preserved), optionally hides the input line and scroll buttons (`/full`) and
clears the screen (`/clear`). The on-screen keyboard can still be shown for
app input. `appmode off` restores the saved screen; `appmode status` reports
the state.

**Cleanup on `exit /b`** — if the batch file that entered app mode returns
via `exit /b`, `goto :eof`, or end of file, the saved screen is restored
automatically, so an app can never leave the shell stuck in app mode. Native
apps get the same through `app_mode_enter` / `app_mode_exit` (applib).

### calc — floating-point calculator

`calc` evaluates a floating-point expression and either prints the result or
stores it in an environment variable, giving batch files real math:

```
calc 2^10                  ->  1024
calc 7 MOD 3               ->  1
calc x = sin(30)           ->  x = 0.5        (DEG mode is the default)
calc &HFF + 1              ->  256
calc len('hello')          ->  5
calc hex$(255)             ->  FF
calc /hex 255              ->  &HFF
calc /angle                ->  calc.angle=degrees
calc /rad                  ->  calc.angle=radians
calc cur(27)               ->  3
calc cur(-8)               ->  -2
calc deg(30.1530)          ->  30.258333...    (30°15'30" = 30 + 15/60 + 30/3600)
calc asinh(1)              ->  0.881373587...
calc acosh(2)              ->  1.316957896...
calc atanh(0.5)            ->  0.549306144...
calc bin$(255)             ->  11111111
calc oct$(255)             ->  377
calc valb('FF',16)         ->  255
calc c2f(100)              ->  212
calc in2mm(1)              ->  25.4
calc kg2lb(1)              ->  2.20462262185
```

Grammar: `+ - * / ^` (right-associative power), the BASIC `MOD` keyword, unary
`- +`, parentheses, `&H`/`0x` hex literals, `PI`, `RAN#[(seed)]`, string
literals (`'` or `"`) with `+` concatenation, and environment-variable
references (an undefined variable reads as 0, matching `set /a`).

Functions (BASIC names; `ASIN`/`ACOS`/`ATAN` are accepted for `ASN`/`ACS`/
`ATN`; `ASINH`/`ACOSH`/`ATANH` for `HYP ASN`/`HYP ACS`/`HYP ATN`):

| Function | Meaning |
|----------|---------|
| `ABS` `SGN` | Absolute value; sign (-1/0/1) |
| `INT` `FIX` `FRAC` `ROUND` | Floor; truncate; fractional part; round to N decimals |
| `SQR` `EXP` `LN` `LOG` | Square root; e^x; natural log; base-10 log |
| `SIN` `COS` `TAN` `SINH` `COSH` `TANH` | Trig and hyperbolic trig (current angle mode) |
| `ASN`/`ASIN` `ACS`/`ACOS` `ATN`/`ATAN` | Inverse trig (result in current angle mode) |
| `ASINH` `ACOSH` `ATANH` | Inverse hyperbolic trig (radians) |
| `FACT` `NCR` `NPR` | Factorial; combinations; permutations |
| `MOD(a,b)` | Floor-modulo, result sign follows the divisor |
| `POL` `REC` | Polar↔rectangular; stores both results in the X/Y variables |
| `DMS` `DMS$` | Decimal degrees → D.MMSS number / formatted `Dd MM' SS"` string |
| `DEG` | Sexagesimal `D.MMSS` → decimal degrees (inverse of `DMS`) |
| `CUR` | Cube root |
| `VAL` `VALF` `STR$` `HEX$` | String→number (leading parse); number→string; integer→hex string |
| `BIN$` `OCT$` `VALB` | Integer→binary/octal string; string→number in base 2..36 |
| `C2F` `F2C` `IN2MM` `MM2IN` `LB2KG` `KG2LB` | Unit conversions (temperature, length, mass) |
| `ASC` `CHR$` `LEN` | Char→code; code→char; string length |
| `LEFT$` `MID$` `RIGHT$` | 1-based string slices |
| `PV` `FV` `PMT` `NPER` `RATE` | Time value of money, HP-12C conventions (cash out is negative; optional `fv`/`pv`, `type` 0=end/1=beginning, `RATE` optional guess) |
| `NPV(rate,v0,v1,...)` `IRR(v0,v1,...)` | Net present value; internal rate of return (up to `P4_CONFIG_CALC_ARG_MAX` args) |
| `SLN` `SYD` `DB` | Straight-line / sum-of-years-digits / declining-balance depreciation (`DB` optional first-year `month`) |
| `DATE` `YEAR` `MONTH` `DAY` `DOW` `TODAY` | Epoch-day serials (days since 1970-01-01); `DOW` 0=Sunday..6=Saturday; `TODAY` follows the device clock |
| `DATEADD` `DAYS` `EOMONTH` | Add days; `b-a` in days; last day of the month `months` away |
| `DATEVALUE` `DATESTR` | `'YYYY-MM-DD'`→serial; serial→`'YYYY-MM-DD'` |

`calc /fin` and `calc /date` print the two cheatsheets above at the prompt.

A `NAME=<expr>` assignment stores the result (numbers as a trimmed decimal
string, strings verbatim); `calc /hex` prints an integral result as `&H` hex.
`POL`/`REC` overwrite the X and Y environment variables exactly like the
calculator's BASIC. ERRORLEVEL: 0 ok, 1 domain/syntax/store error, 2 usage.

Batch graphing over `calc` expressions lives in `plot`: world-coordinate
windows, axes, function/polar/parametric curves, data/bar charts, and value
tables rendered onto the `gfx` canvas or the TUI grid.

As with `set /a`, an expression that uses shell syntax (`^ & | < >`) must be
quoted at the prompt so the chain/pipe/redirect splitter does not consume it.

### Line continuation

A trailing `^` joins the next physical line, letting a long command be split
for readability:

```
copy "very long source name.txt" ^
     "very long destination name.txt"
```

Up to 8 lines may be joined. A doubled `^^` at end of line is an escaped
literal caret, not a continuation.

### Batch File Features
- %0 (script name), %1 through %9, and %* (all arguments, from %1 onward) expansion
- `%~[fdpnx]N` argument modifiers: `%~1` strips surrounding quotes, `f` = full
  path, `d` = drive (always empty on FATFS), `p` = directory with trailing
  `/`, `n` = base name without extension, `x` = extension with dot
  (`%~dpnx1` combines them; `s` is accepted and ignored — no short names)
- %VAR% environment variable expansion
- Dynamic pseudo-variables (always win over a user variable of the same name):
  - `%ERRORLEVEL%` — current errorlevel as a decimal string
  - `%DATE%` — current date as `MM-DD-YYYY`
  - `%TIME%` — current time as `HH:MM:SS`
  - `%RANDOM%` — a random integer `0..32767` (cmd.exe parity)
  - `%CD%` — the current working directory
- An undefined `%VAR%` expands to the empty string (cmd.exe parity), so
  `if "%var%"==""` detects an unset variable, and `if defined VAR` checks
  whether a variable has been set.
- `if [not] [/i] defined VAR` — true when `VAR` is set (cmd.exe parity).
- rem and :: comment lines (opaque to end of line: no expansion, chaining,
  pipes, or redirection inside a comment — cmd.exe parity)
- @ line prefix to suppress echo for one line
- echo on/off flow control
- `:label` targets for `goto`, `gosub`/`call :label` and `on`, including the
  implicit `:eof` end-of-file label. A target may be written `:label` or bare
  `label`. At most 128 labels per file, each up to 64 bytes
  (`P4_CONFIG_BATCH_LABEL_MAX` / `P4_CONFIG_BATCH_LABEL_BYTES`); extra labels
  are ignored with a `batch: too many labels` warning, so keep big apps under
  the cap.
- `for %%var in (set) do command` loops over literal tokens or a single wildcard pattern (for example `for %%F in (*.txt) do echo %%F`)

### for — loops (interactive and batch)
`for %var in (set) do command` runs `command` once per element of `set`, with
`%var` substituted. `set` is a whitespace-separated token list or a single
wildcard pattern expanded against the current directory:

- `for %i in (a b c) do echo ITEM %i` — prints `ITEM a`, `ITEM b`, `ITEM c`
- `for %f in (*.txt) do echo F %f` — runs once per matching `.txt` file
- `for %i in (1 2 3) do set /a total=total+%i` — nested commands re-enter the
  pipeline, so pipes/redirection/variables work per-iteration

At the interactive prompt the variable is written `%var`; inside batch files it
is `%%var` (the batch-file escape for a literal `%`). The body may not contain
the chain/pipe operators that the shell splits on before `for` runs.

### for /f — file-line loops

`for /f` iterates over the lines of a file, binding the requested token indices
to consecutive loop-variable letters:

```
for /f "delims=, tokens=1,2" %%a in (data.csv) do echo A=%%a B=%%b
for /f "skip=1 eol=;" %%l in (notes.txt) do echo %%l
for /f "tokens=1,*" %%k in (pairs.txt) do echo KEY=%%k VAL=%%l
```

Option values are space-separated, so a space cannot appear inside a custom
`delims=` list (it terminates the value — rely on the default space+tab by
omitting `delims`, or split on other characters; e.g. `mem` output parses
with `delims==,b` + a label compare, see `apps/mood/MOOD.BAT`).

The file-set may be an explicit filename, a wildcard (`(*.txt)` processes every
matching file's lines), or empty `()` when a `< file` redirection / pipe stage
is active:

```
type data.txt | for /f "tokens=2" %%t in () do echo %%t
set /p v=< data.txt
```

| Option | Meaning |
|--------|---------|
| `eol=c` | Lines starting with `c` are ignored (comment marker) |
| `skip=n` | Skip the first `n` lines |
| `delims=xyz` | Delimiter characters used to split each line (default space+tab) |
| `tokens=a,b,m-n,*` | 1-based token indices to bind to `%%a`, `%%b`, ...; `*` binds the rest of the line |
| `usebackq` | Selects the backquote command form (`` in (`cmd`) `` instead of `in ('cmd')`) |

A single-quoted set runs the inner command and iterates its output — the
cmd.exe mechanism for parsing command output (batch files; the interactive
prompt strips single-quote markup before `for` runs, so prefer files):

```
for /f "tokens=*" %%v in ('echo hello') do echo GOT=%%v
for /f "tokens=2 delims=," %%a in ('db find contacts /b') do echo %%a %%b
```

The inner command runs through the full pipeline with the re-entrant
redirection capture, so its own `>`/`>>` nests correctly; like pipe stages,
its output also remains visible on the transcript. `skip`/`eol`/`delims`/
`tokens` apply exactly as in the file form, lines are capped at
`P4_CONFIG_FORF_LINE_MAX`, and an over-long capture warns instead of
silently truncating.

Each line is read through the same pipeline as a batch line, so variables,
pipes, and redirection work per iteration. This is the mechanism behind the
BASIC `READ`/`DATA`/`INPUT#` verbs.

### Batch process model

A batch file is a command stream, not a separate process, but its I/O and
state model is fully defined so scripts behave predictably:

- **stdout** — every command's transcript output, captured by `>` / `>>`
  redirection (the whole delta the command prints, including `echo`,
  `calc`, `type`, and the text tools). A batch line inherits the redirect a
  caller set up, exactly like DOS.
- **stderr** — not a separate stream: errors are interleaved on the
  transcript (and therefore the redirect). A batch file distinguishes
  failure with ERRORLEVEL, never by parsing stderr.
- **stdin** — the input-redirection slot (a `< file`, a pipe stage, or an
  explicit filename resolved through `storage_resolve_input_source`).
  Consumed by the text tools (`sort`, `find`, `findstr`, `more`, `fc`,
  `comp`), by `for /f` over an empty set, and by `set /p NAME=< file`, which
  reads exactly one line. A batch file is a first-class pipe stage: `echo
  hello | filter.bat` feeds the spool file into the batch's `for /f ... in
  ()` / `set /p` lines, and its own output flows to the next stage or the
  transcript. The interactive key queue is the fallback source for `set /p`,
  `pause`, and `choice` when no redirect is active.
- **argv** — `%0` is the script name, `%1`..`%9` are the caller's
  arguments, and `%*` is everything from `%1` onward. `call` and `call
  :label` push a fresh argument frame; `shift` slides it left.
- **exit code** — `errorlevel`, read by `if errorlevel N` / `&&` / `||` and
  expandable as `%ERRORLEVEL%` (a decimal string, so `set code=%errorlevel%`
  and `if %errorlevel%==5 ...` work). `exit /b [code]` sets it for the
  current process; `call` propagates the callee's final value.
- **process introspection** — `proc` lists every nested batch process and its
  `/args` `/name` `/depth` `/errorlevel` `/echo` `/stdin` forms report the
  current one, so a batch file can branch on its own depth or arguments and a
  user can see what a pipe stage is reading.
- **cwd** — the RAM-only current working directory owned by
  `components/storage/`; `cd`/`chdir` change it, and every relative path in
  the file resolves against it at run time.
- **PATH** — the RAM-only `PATH` environment variable (default `sd:/`),
  used by `shell_resolve_batch_path` to find `.bat` files: the literal
  name, then `name.bat`, then each `;`-separated PATH entry with both forms.
- **environment propagation** — the 24-slot RAM table is shared across the
  whole session. `set NAME=value`, `set /a`, `set /p`, and `calc` mutate it;
  `call` hands the full table to the callee; `setlocal`/`endlocal` push and
  pop snapshots (a scope left open is unwound when its frame returns); a
  bare `exit` unwinds every nested frame.
- **errorlevel** — set by `set`/`set /a`/`set /p`/`calc`, `choice`,
  `del`/`rd`/`format`/`disk`, `find`/`findstr`/`fc`/`comp`/`sort`/`more`,
  `ping`/`dns`/`httpget`/`httpd`, `receive`/`send`, and `exit`; read by
  `if errorlevel N`, `&&`, and `||`.
- PATH-based .bat lookup
- Nested calls up to 4 levels deep; `call` forwards arguments and propagates the callee's errorlevel
- setlocal/endlocal scoping up to 8 levels deep, auto-unwound when a file returns
- `set /a` integer arithmetic and `set /p` prompted input
- Trailing `^` line continuation, up to 8 joined lines

### choice

Waits for one of the allowed keys and sets errorlevel to that key's 1-based index.
Unmatched keys are ignored.

| Switch | Meaning |
|--------|---------|
| /C:list | Allowed keys, default `YN`. Example: `/C:YNC` |
| /N | Do not display the key list |
| /T:c,secs | Default to key `c` after `secs` seconds |
| /S | Case-sensitive key matching |

Example: `choice /C:YNC /T:N,10 Overwrite the file` prints
`Overwrite the file [Y,N,C]?` and defaults to `N` after ten seconds.

### notify

`notify [/t:secs] <text>` queues `<text>` in the header notification area for the
duration configured by `P4_CONFIG_HEADER_NOTIFY_TIMEOUT_MS` (or `/t:secs`
seconds when given). Notifications are shown FIFO (depth
`P4_CONFIG_HEADER_NOTIFY_QUEUE`), so a busy center queues a new message rather
than dropping it. `notify -` flushes the queue and clears the center
immediately.

### dialog

`dialog [/t:secs] "title" "message" [button1] [button2]` opens a native modal
dialog that fills the live transcript region (`P4_CONFIG_TUI_COLS`×`P4_CONFIG_TUI_ROWS` 80×25 via `windows_enter_editor_mode`/`windows_refresh_editor_surface`; hardware-tested via serial on COM11). With one button the dialog is an OK dialog;
with two buttons the caller gets a binary choice. `/t:secs` auto-cancels the
dialog after `secs` seconds if nobody interacts (like `choice /T`, hardware-tested: `dialog /t:2` → 255 after timeout), so an
unattended batch script cannot hang on a dialog. ERRORLEVEL is `0` for the
first button, `1` for the second button, or `255` for cancel/Esc/timeout. Draw is TUI-aware when a dialog is active.

### list

`list [/t:secs] [/v:NAME] "title" item1 [item2...]` opens a scrollable
native list selector that fills the live transcript region (`P4_CONFIG_TUI_COLS`×`P4_CONFIG_TUI_ROWS` 80×25 via `windows_enter_editor_mode`; hardware-tested via serial on COM11, including `/t:secs` timeout). Tap an item (or use Up/Down + Enter on a USB keyboard,
or type the 1-based number on the serial console) to select it. `/t:secs`
auto-cancels after `secs` seconds (hardware-tested: `list /t:2` → 255). ERRORLEVEL is the 0-based index of the
selected item, or `255` for cancel/Esc/timeout. With `/v:NAME` the selected
item's label is stored in the `NAME` environment variable. Fills the transcript region and resizes with rotation/keyboard.

### ask

`ask [/t:secs] [/v:NAME] [/p] "prompt" [default]` opens a native text prompt
that fills the live transcript region (`P4_CONFIG_TUI_COLS`×`P4_CONFIG_TUI_ROWS` 80×25; hardware-tested via serial on COM11 — serial line is consumed as the answer). With the on-screen keyboard. `/t:secs` auto-cancels after `secs` seconds (hardware-tested: `ask /t:2` → 1 with `ASK_RESULT`/NAME unchanged on timeout).
The answer is stored in the `ASK_RESULT` environment variable, or in `NAME`
when `/v:NAME` is given. `/p` masks the input (password mode; the mask covers
the on-screen keyboard — serial input still echos). ERRORLEVEL is `0` on OK,
`1` on cancel/Esc/timeout, or `2` for usage errors. Fills transcript region via `windows_enter_editor_mode`.

> **TUI restoration:** the `dialog`/`list`/`ask` dispatcher was fixed (now works interactively at the prompt and in batch files — previously batch-only), and `browse`/`view`/`hexview` restore the remaining modals. All 6 modals use the shared modal runtime in `components/modal/`, fill the live transcript region (resizes with display rotation and keyboard visibility), and accept `/t:secs` for unattended timeout.

### form

`form [/t:secs] "title" "Label=type[:arg]:VAR" ...` opens a multi-field modal
editor (the batch-app form primitive) on the shared modal runtime. A field
spec is `Label=type[:arg]:VAR` where `type` is one of:

| type | Meaning | arg |
|------|---------|-----|
| `text` | single-line text field | — |
| `password` | masked text field | — |
| `check` | checkbox (value `1`/`0`) | — |
| `select` | roller list | `a\|b\|c` |
| `range` | slider | `min-max` |

`VAR` is prefilled from the environment when set and receives the accepted
value on OK. OK sets ERRORLEVEL `0`; cancel/Esc/timeout sets `255`; a missing
title or no fields is usage (`2`). Serial input is `ok`/`y` to accept or
`cancel`/`q` to cancel. Example:

```
set theme=amber
form "Preferences" "Theme=select:default|amber|ice|mono:theme" "Lock=check:lock"
```

### owner

`owner [show]` prints the device owner identity; `owner name|company|phone
<value>` sets one field. Values persist as `OWNER_*` directives in CONFIG.SYS.
ERRORLEVEL 0 ok / 2 usage.

### security

`security [status]` reports lock state, passcode presence, conceal mode,
auto-lock, and boot-lock. Subcommands:

| Subcommand | Meaning |
|------------|---------|
| `conceal show\|mask\|hide` | private-record policy (`hide` skips them entirely) |
| `setpass` | set/replace the passcode (password modal, confirmed twice) |
| `clearpass` | clear the passcode (asks for the current one) |
| `lock` / `unlock` | lock now / unlock with the passcode |
| `autolock <secs\|off>` | auto-lock after idle |
| `bootlock on\|off` | lock the dispatcher at boot |

The passcode is a PBKDF2-SHA256 salted hash stored as hex in CONFIG.SYS
(`SECURITY_PASS_SALT`/`SECURITY_PASS_HASH`); it is never stored or shown in
cleartext. While locked, only `security`/`unlock`/`help`/`cls`/`clear`/
`version`/`about` run; `db /reveal`, `gfind`, and `export` also honor the
lock/conceal policy. Recovery is deleting the `SECURITY_*` lines on the SD card
(or `config factory`).

### browse

`browse [/t:secs] [/v:NAME] [path]` opens a native file browser modal that fills the live transcript region (`P4_CONFIG_TUI_COLS`×`P4_CONFIG_TUI_ROWS` 80×25 via `windows_enter_editor_mode`/`windows_refresh_editor_surface`; hardware-tested via serial on COM11) using the shared modal runtime in `components/modal/` (resizes with rotation and keyboard visibility). The browser starts at `[path]` (default: current directory) and shows directories and files with the shell colour scheme. Touch: tap a file to select, tap a directory to enter, Back to go up; USB keyboard: Up/Down + Enter, Backspace to go up, Esc to cancel; serial console: type the 1-based number or `..` / `q` (hardware-tested). With `/v:NAME` the selected path is stored in the `NAME` environment variable (default `BROWSE_RESULT`). `/t:secs` auto-cancels after `secs` seconds if nobody interacts. ERRORLEVEL is `0` on selection, `1` on cancel/Esc/timeout, or `2` for usage errors.

### view

`view [/t:secs] [--raw] <file>` opens a native pager that fills the live transcript region (`P4_CONFIG_TUI_COLS`×`P4_CONFIG_TUI_ROWS` 80×25 via `windows_enter_editor_mode`; hardware-tested via serial on COM11) using the shared modal runtime in `components/modal/`. For text, a paginated read-only preview with scrolling: drag on the transcript, Up/Dn buttons, USB keyboard PageUp/PageDown or arrows, mouse wheel (`P4_CONFIG_TRANSCRIPT_SCROLL_STEP` per notch). Shows the file with line numbers and shell colours; keyboard/serial controls match `browse`. `/t:secs` auto-cancels after `secs` seconds. `.md`/`.markdown`/`.mkd` files render rendered-plain (markup stripped, tables aligned) unless `--raw` is given. **`.bmp`/`.dib` files open the image viewer** (fit-to-screen, `Esc`/`q`/Close) instead of the text pager — this is routed by the central `components/filetype/` registry. ERRORLEVEL is `0` on close (OK), `1` on cancel/Esc/timeout or a bad image, or `2` for usage or missing file. `draw` is TUI-aware when `view` is active.

### hexview

`hexview [/t:secs] <file>` opens a native hex dump viewer that fills the live transcript region (`P4_CONFIG_TUI_COLS`×`P4_CONFIG_TUI_ROWS` 80×25 via `windows_enter_editor_mode`; hardware-tested via serial on COM11) using the shared modal runtime in `components/modal/`. Shows the file as 16-byte rows with hex and ASCII columns (e.g. `00000000  48 65 6C 6C 6F ...  |Hello...|`), scrollable with the same drag/keyboard/wheel controls as `view`. `/t:secs` auto-cancels after `secs` seconds. Keyboard/serial controls match `view`/`browse`. ERRORLEVEL is `0` on close, `1` on cancel/Esc/timeout, or `2` for usage or missing file.

### open

`open [/t:secs] [--raw] <file>` does the right thing per file type (central
registry `components/filetype/`): scripts (`.bat`/`.cmd`) open in the editor
as source (never execute — typing the name runs them), Markdown renders,
**BMP images (`.bmp`/`.dib`) open the image viewer**, everything else views as
text. Batch-callable with the same ERRORLEVELs as `view`/`edit`. `open` is the
batch-file-friendly way to present a file: `if exist README.MD open README.MD`.

### image

`image` is the scriptable BMP entry point; `view`/`open` route images to the
same viewer through the filetype registry.

| Form | Meaning |
|------|---------|
| `image info <file.bmp>` | Print `image: <path> <W>x<H> <bpp>bpp <top-down\|bottom-up> <bytes>` (single machine-parsable line for `for /f`). Accepts 24-bit and 32-bit `BI_RGB`, either orientation, up to `GFX_IMAGE_MAX_W`×`GFX_IMAGE_MAX_H` and `P4_CONFIG_IMAGE_MAX_BYTES`. |
| `image show [/t:secs] <file.bmp>` | The fit-to-screen image viewer (same surface as `view`; `P4_CONFIG_IMAGE_VIEWER_FIT`, `Esc`/`q`/Close, `/t:secs` auto-close). |

ERRORLEVEL: `0` ok, `1` missing/undecodable file, `2` usage. `image info` is
batch-friendly: `for /f "tokens=2" %%a in ('image info PIC.BMP') do ...`.

### json

`json validate <file>` checks JSON structure and reports `invalid: <msg> at
line L col C` (ERRORLEVEL 1) or `json: valid (N bytes)` (ERRORLEVEL 0).
`json pretty <file>` prints 2-space-indented JSON. Limits: 64 KB files,
nesting depth 64, single JSON value per file (trailing data is an error).

### draw — TUI drawing primitives (hardware-verified on COM11)

`draw` composes on the `80×25` TUI cell buffer (`components/tui/tui.c` `tui_draw_box`/`tui_draw_line`/`tui_fill`/`tui_print_at` via `tui_cell_set` `utf8[4]` `components/tui/tui.h:35`, `tui_flush` recolor `#RRGGBB` per fg run via `ansi_get_palette_color`). It auto-enters TUI (`tui_init` `components/tui/tui.c:56` → `windows_enter_tui_mode`) when no TUI/modal surface is active; inside a TUI/modal surface it reuses the active buffer. Every primitive clamps to `P4_CONFIG_TUI_COLS`×`P4_CONFIG_TUI_ROWS` (`p4minishell_config.h:325`), coordinates are 1-based DOS style.

| Form | Meaning |
|------|---------|
| `draw box <x> <y> <w> <h> [single\|double\|rounded] [fg] [bg] [title]` | Draw box border at x,y,w,h with style (default `single`) and optional centered title. Honors style via `SH_BOX_*` UTF-8 (`SH_BOX_TL`/`H`/`V` vs `SH_BOX_TL2`/`H2`/`V2` vs `SH_BOX_TLR`/`TRR`/`BLR`/`BRR`) through `tui_cell_set`; nested boxes form the window stack. |
| `draw line <x1> <y1> <x2> <y2> [fg] [bg]` | Draw H/V line (only horizontal `y1==y2` or vertical `x1==x2`). |
| `draw fill <x> <y> <w> <h> <char> [fg] [bg]` | Fill rect at x,y,w,h with `<char>` (required) using current fg/bg. |
| `draw text <x> <y> <text> [fg] [bg]` | Print text at x,y (UTF-8 aware, respects cell `utf8[4]`). |
| `draw bar <x> <y> <w> <pct> [fillch] [emptych] [fg] [bg]` | Progress bar `[fill...empty...]` of width w at x,y, pct clamped 0..100 (defaults `#`/`-`). Redraw with a new pct for animations. |
| `draw table <x> <y> <fg> <bg> "h1\|h2\|..." [row "c1\|c2\|..." ...] [/cursor:N] [/sel:a,b,...]` | Bordered table with T-junctions; first row is the header (bold on TUI). Column widths auto-fit content (max 40 each, table must fit 80 cols, max 16 cols × 32 rows). Short rows pad with empty cells; extra cells glue to the last column. `/cursor:N` (1-based data row, header excluded) renders that row bright-white bold; `/sel:a,b` renders those data rows bold (both clip silently out of range). |
| `draw list <x> <y> <w> <h> <file> [fg] [bg] [/top:N] [/cursor:N] [/sel:a,b] [/title:T] [/count:NAME] [/countonly]` | Renders a file's lines in a bordered, selectable panel — the file-manager primitive (`dir /b > file` + `draw list`). `/top` is the 1-based first line shown (scroll), `/cursor` the 1-based highlighted line (bright), `/sel` marks lines with `*`, `/title` sets the box title. `/count:NAME` publishes the total line count to env var `NAME`; `/countonly` does just that and skips drawing (batch's `set /a` echoes and floods, so this is the non-flooding counter). Off-TUI it prints `>`/`*`-marked plain lines. Caps: 256 lines, 96 bytes/line; an empty file returns ERRORLEVEL `1`. Hardware-verified (`apps/tcmd/TCMD.BAT`). |
| `draw clear [screen\|line\|eol\|eos]` | Clear target (default `screen`): whole grid, cursor line, cursor-to-end-of-line, or cursor-to-end-of-screen. Outside TUI emits the matching ANSI sequence (`ESC[2J`/`ESC[2K`/`ESC[0K`/`ESC[0J`). |
| `draw image <file.bmp> <x> <y> <w> <h>` | Render a BMP into the cell grid at x,y spanning w×h cells (1-based). Each cell is an ASCII glyph from a luminance ramp coloured with the nearest DOS palette entry (foreground only — the TUI label has no per-cell background). Auto-enters TUI; reuses the single decoder in `components/gfx` (`gfx_bmp_decode_scaled_565`). The 80×25 16-color model posterizes the image; use `view` for full color. ERRORLEVEL `0`/`1`/`2`. |
| `draw window <id> <x> <y> <w> <h> [title]` | Box with window-stack semantics; `<id>` is accepted and ignored. Requires an active TUI (error `1` otherwise). |
| `draw save` / `draw restore` | Save / restore the TUI cursor (`ESC[s` / `ESC[u]` outside TUI). |
| `draw cursor on\|off` | Show / hide the TUI cursor (`ESC[?25h` / `ESC[?25l` outside TUI). |
| `draw alt-screen on\|off` | Enter / leave the TUI alternate screen (`ESC[?1049h` / `ESC[?1049l` outside TUI). |
| `draw close` | Leave TUI mode (`tui_deinit`); error `1` when no TUI is active. Resets `hold`. |
| `draw refresh` | Re-flush the TUI grid; always succeeds (`0`). The frame-closing verb when `hold` is on. |
| `draw hold on\|off` | Frame coalescing for batch animation loops: `on` suppresses the per-verb `tui_flush()` so N verbs cost one LVGL label rebuild; `off` resumes and flushes. `draw close` resets. Always pair with `draw refresh` per frame (`apps/snake/SNAKE.BAT` pattern). |
| `draw fullscreen on\|off` | Global fullscreen: `on` hides header completely via `windows_set_fullscreen(true)`/`header_set_visible(false)` `components/windows/windows.c:418`; `off` restores header. Header kept visible by default; dynamic keyboard scaling via `windows_notify_keyboard_visibility`. |

Examples (all pass on COM11 serial without abort/watchdog/overlap, header kept unless fullscreen):
```
draw box 2 2 20 8 single MyBox
draw box 4 4 12 4 double Inner
draw box 1 1 80 25 rounded Full
draw line 1 5 80 5 single
draw fill 10 10 5 3 X
draw text 5 5 Hello
draw text 5 6 Hi 15 1
draw bar 5 8 40 65
draw bar 5 8 40 90 # . 10 1
draw table 5 10 15 1 "Name|Score|Level" "Bob|1250|7" "Ada|980|5"
draw table 2 4 7 0 "Name|Size" "A.BAT|2 KiB" /cursor:1 /sel:1
dir /o:gn /b sd:/ > sd:/tmp/_L.txt
draw list 1 4 39 17 sd:/tmp/_L.txt 15 16 /top:1 /cursor:2 /title:/
draw window 1 10 6 30 10 Nested
draw fullscreen on
draw clear
draw clear line
draw cursor off
draw alt-screen on
```
ERRORLEVEL: `0` ok, `1` TUI-only verb without an active TUI, `2` usage.

Shared-display rule: `draw`, `tui` (except read-only `tui status`), mutating
`color`, `locate`, `anchor`, and `gfx` are refused inside `start` background
jobs with `<verb>: not available in background jobs (shared display)` +
ERRORLEVEL `1` (BOUNCE-style loud refusal, hardware-verified; bare `color`
and `tui status` stay readable from bg). Modal verbs (`dialog`, `list`,
`ask`, `browse`, `view`, `hexview`) refuse the same way via the modal
runtime. Background jobs do compute/files/net; all display stays foreground.

Colors on `draw` verbs are DOS 0-15 when the value is ≤ 16 (`16` = default),
otherwise 24-bit RGB quantized to the nearest DOS color
(`tui_rgb_to_dos` over the CGA table). So `draw text 1 1 Hi 14 1` is yellow
on blue, and `draw box 2 2 20 8 double 0xFFAA00 0x000000 T` maps orange to
the nearest cell color. Only fg renders on the TUI label today (LVGL recolor
has no per-span background); bg is stored per cell and honored off-TUI.
A literal `#` in cell text (e.g. bar `#` fills) is recolor-escaped by
`tui_flush` — it splits colored runs so LVGL never misparses a tag.

### tui — TUI control

| Form | Meaning |
|------|---------|
| `tui status` | Show transcript rect, cols/rows (`80x25`), fullscreen state and header visibility. The pixel cell is chosen from the committed cell fonts (`components/tui/tui_fonts.c`, generated by `tools/gen_tui_font.py`) to fit the live transcript region. |
| `tui stats` | Print TUI frame pacing (see below), machine-readable. |
| `tui stats reset` | Clear the TUI frame stats and reset the target frame rate to `P4_CONFIG_TUI_TARGET_FPS`. |
| `tui stats target <fps\|off>` | Set the target frame rate used to count dropped frames. |
| `tui clear` | Clear TUI grid (same as `draw clear`). |
| `tui fullscreen on\|off` | Per-app fullscreen: `on` hides header (`windows_set_fullscreen`), `off` restores; kept visible by default. |
| `tui refresh` | Re-apply transcript rect via `windows_notify_keyboard_visibility` / `windows_refresh_tui_surface` (call after rotation/keyboard). |

`tui` and `draw fullscreen` both route to `components/tui/tui.c:417` `tui_enter_fullscreen`/`tui_exit_fullscreen`; `tui` is the per-app alias, `draw fullscreen` is the global batch verb. Screenshot debug: `grab_screenshot.py --port COM11 --out out.png --crop-transcript` + `capture_tui.py` crops to transcript rect for pixel-perfect verification.

### gfx — pixel canvas for batch games (hardware-verified on COM3)

`gfx` owns one RGB565 canvas (`components/gfx/gfx.c` pure raster, PSRAM
buffer, max 320×240 = 150 KB) shown as an `lv_canvas` in the transcript
region. Raster verbs mutate the buffer; nothing reaches the display until
`gfx show`, so animations compose flicker-free. Every op clips (out-of-bounds
pixels are ignored, never an error).

| Form | Meaning |
|------|---------|
| `gfx init <w> <h>` | Allocate w×h canvas (1..320 × 1..240) and show it, scaled uniformly to **fit (contain) and centre** in the live transcript region (the on-screen keyboard is hidden while the app owns it and restored on close); re-fits on keyboard/orientation changes. Refused when TUI is active (`draw close` first) or inside a `start` background job (shared display, same rule as modal verbs). |
| `gfx close` | Delete the canvas, restore the transcript. |
| `gfx status` | Show canvas dims + PSRAM bytes, or "no canvas open". |
| `gfx stats` | Print canvas frame pacing (see below), machine-readable. |
| `gfx stats reset` | Clear the frame stats and reset the target frame rate to 30 fps. |
| `gfx stats target <fps\|off>` | Set the target frame rate used to count dropped frames. |
| `gfx clear [color]` | Fill the canvas (default black). |
| `gfx pixel <x> <y> <color>` | Plot one pixel. |
| `gfx line <x1> <y1> <x2> <y2> <color>` | Bresenham line. |
| `gfx rect <x> <y> <w> <h> <color> [fill]` | Rectangle outline, or filled with `fill`. |
| `gfx circle <x> <y> <r> <color> [fill]` | Midpoint circle outline (`r=0` plots one pixel), or filled disc with `fill`. |
| `gfx hline <x> <y> <w> <color>` | Filled horizontal span (clipped in one call). |
| `gfx vline <x> <y> <h> <color>` | Filled vertical span (clipped). |
| `gfx triangle <x1> <y1> <x2> <y2> <x3> <y3> <color> [fill]` | Triangle outline, or edge-function filled interior with `fill` (degenerate triangles fall back to the outline). |
| `gfx ellipse <cx> <cy> <rx> <ry> <color> [fill]` | Axis-aligned ellipse outline, or filled disc with `fill` (`rx`/`ry` 0 degenerates to a line/pixel). |
| `gfx polygon <color> <fill\|line> <x1> <y1> <x2> <y2> ...` | Closed polygon: `line` outline, or even-odd scanline `fill` (handles concave shapes); at least 3 vertices, at most `GFX_POLY_MAX_PTS` (64). |
| `gfx fill <x> <y> <color>` | 4-way flood fill of the seed pixel's connected color (bounded by the canvas); prints `gfx: filled <n> pixel(s)`. |
| `gfx text [/bg:<color>] [/scale:<n>] <x> <y> <color> <text...>` | Draw 8×8 ASCII text (all remaining words are joined with spaces); `/scale:1..16` integer pixel multiplier, `/bg:<color>` fills the glyph cells (else transparent). |
| `gfx show` | Push the buffer to the display. |
| `gfx image <path> [x y [w h]]` | Decode a 24/32-bit `BI_RGB` BMP (either orientation) straight to the target rect and blit it onto the canvas. Defaults to native size aspect-fit to the canvas at 0,0. Reuses the shared decoder; ERRORLEVEL `1` on a bad file, `2` usage. |
| `gfx load <slot 0..7> <path>` | Ingest a 24/32-bit uncompressed BMP (`BI_RGB`, the exact format `screenshot <file>` writes, at most 64×64) into a sprite slot (PSRAM). Rejects other bit depths, RLE, and oversize art with ERRORLEVEL `1`. |
| `gfx blit <slot> <x> <y> [transparent]` | Stamp a sprite onto the canvas (clipped; needs `gfx show`). Optional transparent color skips matching pixels. ERRORLEVEL `1` when the slot is empty. |
| `gfx free <slot>` | Release one sprite slot. |
| `gfx slots` | List live slots as `gfx.slot: <n> <w>x<h>` (batch `for /f`-friendly). |
| `gfx save <path>` | Write the canvas as a 24-bit BMP (same layout `screenshot <file>` produces: shared `screenshot_write_bmp_headers`, guarded SD session, free-space precheck, partial removed on failure). |

**Frame pacing metrics.** `gfx show` and `tui_flush` are the two present
points; each records a timestamp into the pure `gfx_frame_stats_t` core
(`components/gfx/gfx.c`, unit-tested). `gfx stats` / `tui stats` print one
integer-only machine-readable line (newlib-nano has no `printf` float):

```
gfx stats: frames=150 min_us=27862 avg_us=33333 max_us=41000 jitter_us=1800 dropped=1 fps10=300 target_fps=30
tui stats: frames=42 min_us=1238 avg_us=23809 max_us=70123 jitter_us=9000 dropped=0 fps10=420 target_fps=30
```

`frames` counts measured intervals; `jitter_us` is their standard deviation;
`dropped` counts intervals over 150% of `target_us`; `fps10` is the average
rate ×10. `tools/p4test/perf.py:parse_perf_report` parses this into a
`FrameStats` (with a 0..100 smoothness score) and the perf suite asserts on it.

Sprite bank: 8 slots × max 64×64 RGB565 (8 KB each, 64 KB worst case —
SNES-class 16-bit assets; `GFX_SPR_SLOTS`/`GFX_SPR_MAX` in
`components/gfx/gfx.h`). `gfx close` frees the canvas and all sprites, so
sessions never leak PSRAM. Sample art: `apps/push_assets.py` generates
`SHIP.BMP` (48×48) + `BALL.BMP` (16×16) with PIL (no binary blobs in the
repo) and pushes them with CRC manifests, plus `PHOTO.BMP` (96×64) used by the
`picture` reference app (`apps/pics/PICS.BAT`: `view`, `draw image`, `gfx
image`, `image info`). Generic image caps: `GFX_IMAGE_MAX_W/H`
(`components/gfx/gfx.h`) and `P4_CONFIG_IMAGE_MAX_BYTES`.

Colors are DOS 0-15 from the CGA table (`tui_dos_color_rgb`, so pixel colors
match TUI cell colors), `16` = black, anything larger is 24-bit RGB hex used
at full RGB565 precision (no quantization — unlike `draw`). Reference apps:
`apps/gfxdemo/BOUNCE.BAT` (batch `set /a` ball physics, 240-frame killable
loop at ~30 fps, final-frame `screenshot BOUNCE.BMP` after the loop — the
render loop itself never blocks on I/O; run `launch bounce`) and
`apps/gfxdemo/GFXTOOL.BAT` (all toolkit primitives + scaled text, saves
`GFXTOOL.BMP`; run `launch gfxtool`).

The toolkit's text verb uses the built-in 8×8 ASCII font in
`components/gfx/gfx_font.c` (glyphs 0x20..0x7E, generated by
`tools/gen_gfx_font.py` from the public-domain unscii-8 TTF bundled with
LVGL; committed, no runtime font dependency). Advance is
`strlen(text) * 8 * scale` (`gfx_text_width`). The raster primitives are pure
buffer math (no LVGL) and unit-tested in `test/main/test_gfx.c`.

ERRORLEVEL: `0` ok (clipped pixels included), `1` no canvas / already open /
TUI active / background job / no memory / empty slot / unreadable or non-24/32-bit
BMP, `2` usage / bad slot / invalid path.

### plot — world-coordinate graphs, charts, drawings (hardware-verified on COM3)

`plot` is the calculator's graph mode: a thin coordinate layer that renders
math onto the `gfx` pixel canvas (default) or the TUI cell grid
(`plot tui on`). One shared viewport (`xmin..xmax`, `ymin..ymax`, y up) maps
to either target (`components/gfx/gfx_view.c`, pure + unit-tested). Function
sampling evaluates `calc` expressions (`y=f(X)`, `r=f(T)`, `x/y=f(T)`), so the
full `calc` function set, `PI`, and variables work; trig follows the current
`calc` angle mode (`calc /rad` for radian plots). Canvas plots never
auto-show — compose several verbs, then one `gfx show`. Foreground only
(same shared-display refusal as `gfx`/`draw`), except `plot status` (read-only)
and `plot table` (text output, background-safe).

| Form | Meaning |
|------|---------|
| `plot tui on\|off` | Select the render target: TUI cells (`on`, auto-enters TUI like `draw`) or the `gfx` canvas (`off`, needs `gfx init`). |
| `plot window <xmin> <xmax> <ymin> <ymax> [/rect:x:y:w:h]` | Set the world window (canvas rect is 0-based pixels, TUI rect 1-based cells; default is the full surface). |
| `plot auto [func <expr> \| data <file> \| bar <src>] [<xmin> <xmax>]` | Fit the y-range to a source (bare form refits the last plotted source); prints the fitted window. |
| `plot axes [color] [/grid] [/ticks:N]` | Axes at the origin (or window edges) with nice-step ticks + labels; `/grid` adds full grid lines. |
| `plot func <expr> [color] [/samples:N] [/auto]` | Sample `y=f(X)` across the window as a clipped polyline (default samples = view width, cap `P4_CONFIG_PLOT_SAMPLES`); breaks across asymptotes/non-finite. |
| `plot polar <expr> [color] [/samples:N] [/auto]` | `r=f(T)`, T in 0..360 degrees. |
| `plot para <xexpr> <yexpr> [color] [/samples:N] [/auto]` | Parametric curve, T in 0..360 degrees. |
| `plot data <file> [color] [/dots] [/auto]` | `x,y` (comma/space) per line (`#`/`;` comments skipped, cap `P4_CONFIG_PLOT_MAX_POINTS`): polyline, or scatter with `/dots`. |
| `plot bar <file\|v1,v2,..> [color] [/auto]` | Bar chart from one value per line or an inline comma list (baseline at y=0 when in range). |
| `plot table <expr> /from:<a> /to:<b> /step:<s>` | Calculator TABLE listing: `plot.table: <x> <y>` rows (cap `P4_CONFIG_PLOT_MAX_POINTS`). |
| `plot line <x1> <y1> <x2> <y2> [color]` | World-coordinate segment (exact Cohen–Sutherland clip). |
| `plot point <x> <y> [color]` | World-coordinate dot. |
| `plot clear [color]` | Wipe the active surface (canvas fill / whole TUI grid). |
| `plot status` | Target, window, canvas/TUI state, angle mode. |

Sampling binds `X`/`T` through the environment and restores the prior value
afterwards. Multi-word expressions must be quoted (`plot func "x^2 + 1"`;
`^ & | < >` are shell operators). Reference app: `apps/gfxdemo/PLOT.BAT`
(axes+grid, sin/cos, world line; run `launch plot`). ERRORLEVEL: `0` ok,
`1` no canvas / background job / domain (no finite values) / unreadable file
/ nothing to fit, `2` usage.

### crc32, asset — file checksums and asset manifests (hardware-verified on COM3)

`crc32 <path>` streams a file through the firmware's single CRC-32 primitive
(`shell_crc32_update`, shared with `receive` transfer verification) and prints
`crc32: <resolved> <HEX>` (zlib parity, verified against host `zlib.crc32`).
ERRORLEVEL `0` ok / `1` unreadable / `2` usage.

`asset check|list <app>` verifies/lists `APPS/<APP>.ASSETS`, a text manifest of
`path=HEXCRC` lines (`#`/`;` comments and blanks skipped, 16 KB cap). `list`
prints `asset: <path> <HEX>` per entry; `check` prints `MISSING`/`MISMATCH`
lines and ends `asset: OK n/m ok` (ERRORLEVEL `0`) or `asset: FAIL n/m ok`
(ERRORLEVEL `1`). Manifests are generated host-side by `apps/push_assets.py`
(PIL sprites + `zlib`, same receive/CRC push path as everything else) and
verified on-device — the whole reference set (`SPR`, `BOUNCE`, `SNAKE`,
`TCMD`, `ELITE`, `ADVENT`, `NOTES`, `MOOD`) checks `OK`. App names are
`[A-Za-z0-9_-]+`; manifest paths must be SD-relative (no `..`, no leading
`/`). ERRORLEVEL `2` on usage/bad name; an empty manifest prints
`asset: manifest empty (nothing to check)` and returns ERRORLEVEL `1`.

### pkg — packaged SD applications (B1, hardware-verified on COM3)

A *package* is an app shipped as a self-contained, CRC-checked bundle.
Installing it is a verify-then-copy; removing it is a trash (undelete-able)
delete, so `undelete` can recover an uninstalled payload.

- **Installed side** (`sd:/APPS/`): `<APP>.APPINFO` (metadata: `title=`,
  `description=`, `version=`, plus `type=` — `batch` when absent, or `native`
  with `abi=`/`arch=`/`entry=`, see `docs/native_packaging.md`) and
  `<APP>.ASSETS` (the `path=HEXCRC` manifest —
  the same format and checker as `asset`).
- **Bundle side** (`sd:/PKGS/<APP>/`): `<APP>.ASSETS` (manifest),
  `<APP>.APPINFO` (metadata), and payload files at their install-relative
  paths. A root payload `FOO.BAT` installs to `sd:/FOO.BAT`; a payload
  `sub/x` installs to `sd:/sub/x`. Bundles are built host-side by
  `apps/push_pkgs.py` (same PIL/`zlib`/receive path as the other push tools).

| Command | Description |
|---------|-------------|
| `pkg list` | One line per installed app: `APP  title  vVERSION  N file(s)` (` [native]` suffix for native bundles) |
| `pkg info <app>` | Title/description/version/type (+abi) plus each manifest entry as `ok`/`MISSING`/`BAD` |
| `pkg verify <app>` | CRC-check the installed manifest (`pkg: OK n/m ok` / `FAIL`) |
| `pkg check` | Run the verify check over every installed app and summarise `pkg: n/m package(s) ok` |
| `pkg install <app>` | Two-pass copy of `PKGS/<app>/`: pass 1 verifies every payload CRC, pass 2 copies payloads, then the APPINFO and manifest into `APPS/` |
| `pkg remove <app>` | Trash every manifest payload plus `<APP>.APPINFO` and `<APP>.ASSETS` |

`pkg install` aborts without touching installed files if any bundle payload is
missing or corrupt (`pkg: install aborted (n bad file(s))`), or if the bundle
`type=` is anything but `batch`/`native`. Native bundles (`type=native`)
install **store-only** in v1.1 — verified and copied, but not executable
(`pkg: <app> is a native package (stored only, …)`); an `abi=` mismatch with
`P4_CONFIG_NATIVE_ABI` warns. `launch` never offers native apps. `pkg` reuses the
`asset` manifest parser/checker (`asset_parse_line`, `asset_crc_file`,
`asset_verify_app`) and `shell_fs_copy_file`, so an app name is the same
`[A-Za-z0-9_-]+` rule as `asset`. `pkg info` on a missing manifest prints
`manifest: APPS/<APP>.ASSETS (missing)`. ERRORLEVEL: `0` ok, `1`
missing/corrupt/empty-manifest, `2` usage or bad app name. Companion UI:
**Live System ‣ Packages** in `apps/companion/SYS.BAT`; HW driver
`tools/pkg_test.py`.

### color

`color [fg] [bg]` — DOS `COLOR` parity (hardware-verified on COM11; TUI-aware via `components/tui/tui.c:324` `tui_set_default_color` and `tui_flush` per-fg recolor). With no arguments prints the current default colours (`color: fg=X bg=Y`). With one or two hex digits (`0`-`F`, case-insensitive) sets the default transcript/TUI colours used for subsequent output, matching DOS `COLOR` semantics (e.g. `color 0A` bright green on black, `color 07` light grey on black, `color 1E` yellow on blue). Values are validated; a missing or invalid colour sets ERRORLEVEL `2`, success sets `0`. The mutating form is refused in background jobs (shared display, ERRORLEVEL `1`); bare `color` reads fine anywhere. The palette itself remains the compiled-in `SH_*` scheme in `components/ansi/ansi_palette.h` via `ansi_get_palette_color` — `color` only selects the default foreground/background pair. TUI grid `P4_CONFIG_TUI_COLS`×`P4_CONFIG_TUI_ROWS` 80×25.

### locate

`locate <row> <col>` — DOS `LOCATE` parity (hardware-verified on COM11; TUI-aware via `components/tui/tui.c:160` `tui_set_cursor`/`tui_get_cursor`). Emits the ANSI cursor-position sequence `ESC[<row>;<col>H` to move the cursor, with `row` clamped to `1..25` and `col` to `1..80` bounded by `P4_CONFIG_TUI_ROWS` / `P4_CONFIG_TUI_COLS` (`p4minishell_config.h:325`). Used with `echo` and `ansi` to position text in TUI batch apps (e.g. `locate 5 10 && echo Hello`). Row and column must both be present; missing or non-numeric arguments set ERRORLEVEL `2`, success sets `0`. Refused in background jobs (shared display, ERRORLEVEL `1`). Coordinates are 1-based on the `80×25` transcript region (`1024x510`).

### anchor

`anchor <label> <command> [continue_line]` — register a named transcript anchor region bound to a command (`continue_line` accepts `true`/non-zero). Prints `anchor registered: <label> -> <command>`.
Refused in background jobs (shared display).

ERRORLEVEL: `0` ok, `1` background job, `2` usage.

### pause, choice, and more without a keyboard

`pause`, `choice`, and `more` block on a real keypress delivered by the UART console,
a USB keyboard, or the on-screen keyboard. When none of those is attached the commands
fall back to their configured delay (or the first choice) and say so, so a headless
board never stalls a batch file. Every wait is also bounded by a 30-second timeout.

Serial input note: the UART console reader is line-buffered (`fgets`), so over
serial a key must be followed by Enter to reach a key wait (USB/OSK keyboards
deliver raw keys). `choice` is therefore steered as `key+Enter` per step from
a terminal — see `apps/snake/SNAKE.BAT` (`choice /C:wasdq /N /T:%dc%,1` with a
per-direction default so timeouts keep the game moving).

### Writing batch games and TUI apps

Proven patterns (all hardware-verified on COM3; reference apps:
`apps/snake/SNAKE.BAT`, `apps/tcmd/TCMD.BAT`, `apps/elite/ELITE.BAT`,
`apps/gfxdemo/BOUNCE.BAT`):

- Foreground only: open with `draw hold on` / `gfx init` and branch to a
  clean message on ERRORLEVEL `1` — `start`ed copies refuse loudly.
- One flush per frame: `draw hold on` at entry, compose, `draw refresh`;
  `draw hold off` + `draw close` at every exit (`:end`, `:quit`, death).
- Batch has no arrays, indirection, substrings, or delayed expansion, and
  env caps at 24 vars (`P4_CONFIG_ENV_VAR_MAX`, one ambient `PATH` at boot):
  fixed literal slots (`s0`..) plus packed numbers (`y*64+x`, split only for
  checks/draws with `set /a`) cover snake-class state in ~22 vars.
- `list` serial numbers are 1-based while its ERRORLEVEL is 0-based; `q`
  cancels (255). Count items twice when writing `if errorlevel` chains —
  an off-by-one silently remaps every selection (caught live in TCMD).
- Never put `>` (or backticks) in `rem` comments: redirect parsing runs
  before `rem` sees the line and the "comment" becomes a failed redirect.
- `dir` treats a leading-`/` path as switches (`dir /APPS` parses `/A`),
  so absolute SD paths need the `sd:` prefix or a `cd` first.
- Keep HUDs at rows ≤ 21: the on-screen keyboard covers rows 22+ unless
  hidden; `draw fullscreen on` reclaims the header too.
- Save games as executable `set` lines (`ELITE.SAV` pattern) and reload
  with `call`; verify art/data with `asset check <app>`.
- File managers: `dir /o:gn /b <sd:path> > sd:/tmp/_L.txt`, then
  `draw list … /count:LN /countonly` to size the page (batch's `set /a`
  echoes, so a `for`-loop counter floods) and `draw list … /top:T
  /cursor:C` to draw it. `draw fullscreen on` reclaims the header/keyboard
  rows for a full 80×25 layout; `draw fullscreen off` on exit. Reference:
  `apps/tcmd/TCMD.BAT` (dual panes, j/k move, t pane, o open, x menu,
  s swap, 1/2 root, p snapshot, q quit).

### prompt

`prompt` sets a DOS-style template that drives both the UART console prompt and the
LVGL input line.

| Code | Expands to | Code | Expands to |
|------|------------|------|------------|
| `$p` | Current path | `$g` | `>` |
| `$n` | Drive letter | `$l` | `<` |
| `$d` | Current date | `$b` | `\|` |
| `$t` | Current time | `$q` | `=` |
| `$v` | Firmware version | `$a` | `&` |
| `$s` | Space | `$c` | `(` |
| `$_` | Newline | `$f` | `)` |
| `$$` | `$` | `$e` | ESC (UART only) |
| `$h` | Backspace over the previous character | | |

Codes are case-insensitive. An unknown code renders literally. `prompt` with no argument
shows the stored template and its rendered form; `prompt /?` lists the codes.

Example: `prompt $t $p$g ` renders as `14:32:07 /sdcard/logs> `.

### Quoting and escaping

| Form | Meaning |
|------|---------|
| `"text"` | Groups text into one argument. Variables still expand inside. |
| `'text'` | Groups text into one argument literally. Variables do NOT expand. |
| `^c` | Makes the next character literal, including `^&`, `^\|`, `^>`, `^<`, `^"`, `^%`, `^^` |

Quoting and escape markup is removed before a command sees its arguments, so
`echo "hello world"`, `echo 'hello world'`, and `echo hello^ world` all print
`hello world`.

Use quoting when a value may contain spaces or operators:

```
copy "my notes.txt" backup\          Space in a filename
echo "a | b"                         Pipe as data, not a pipeline
echo 'literal %PATH% text'           Percent signs kept verbatim
echo a^&b                            Ampersand as data, not a chain separator
write log.txt "value ^"quoted^""     Escaped quotes inside a quoted run
```

The same rules apply everywhere the shell looks for an operator: redirection targets,
pipe stages, and chain separators.

### Redirection

| Operator | Meaning |
|----------|---------|
| `> file` | Write command transcript output to an SD file (overwrite) |
| `>> file` | Append command transcript output to an SD file |
| `< file` | Read a file as the command's input |

All three may appear on one line in any order, for example
`sort < unsorted.txt > sorted.txt`. An operator that is quoted or caret-escaped is
treated as data.

### Command chaining

| Operator | Meaning |
|----------|---------|
| `a & b` | Run `b` after `a`, regardless of the outcome |
| `a && b` | Run `b` only if `a` succeeded |
| `a \|\| b` | Run `b` only if `a` failed |

Operators mix freely on one line, and up to 8 commands may be chained:

```
cd logs && dir                       List only if the directory change worked
type missing.txt || echo not found   Report only on failure
md backup & copy *.txt backup\       Both run regardless
badcmd || echo fallback              Unknown commands set errorlevel 9009
```

A command counts as failed when it is unrecognized or leaves a new non-zero
errorlevel behind.

A single `|` is the pipe operator, not a chain separator, so a pipeline can be one
link in a chain: `type f.txt | sort && echo sorted`.

Chain splitting happens before variable expansion, so a variable whose value contains
`&` cannot inject an extra command.

### Pipes

`cmd1 | cmd2 | cmd3` chains up to four stages. Each stage spools its output to a
temporary file on SD that the next stage reads, because the shell runs one command at a
time on a single worker task. A `|` inside double quotes is data, not a separator.

Example: `type log.txt | find "error" /I | sort /U`

The text-processing commands (`find`, `more`, `sort`) read the piped or redirected input
whenever no filename argument is supplied.

## DOS-Style Extended Commands

| Command | Description |
|---------|-------------|
| attrib [path] | Show file/directory attributes (R/H/S/A) |
| attrib +R\|-R <file> | Set or clear read-only attribute |
| attrib +H\|-H <file> | Set or clear hidden attribute |
| attrib +S\|-S <file> | Set or clear system attribute |
| attrib +A\|-A <file> | Set or clear archive attribute |
| label | Show current FAT volume label |
| label <name> | Set volume label (max 11 chars) |
| xcopy <src> <dst> [/S] [/E] [/I] [/Y\|/-Y] [/D[:date]] [/H] [/R] [/K] [/C] [/Q] [/T] [/F] [/L] [/A] [/M] [/U] [/P] [/W] [/N] [/V] | Copy files and directory trees with the full DOS 6.x switch set |

### Wildcard Support
- `*` matches any sequence of characters
- `?` matches any single character
- Supported in: `dir`, `del`, `copy`
- Example: `dir *.txt`, `del *.bak`, `copy *.c backup\`

## Volume Management

| Command | Description |
|---------|-------------|
| chkdsk [path] [/F] | Report volume capacity; `/F` also verifies every directory is readable |
| scandisk | Alias for `chkdsk` |
| format [/FS:FAT\|FAT32] [/A:size] [/V:label] [/Q] | Reformat the SD card, destroying all data |
| disk list / detail / clean | Physical-disk info, MBR partition table, remove partitions |
| disk create partition primary [size=N] | Create a primary MBR partition (N in MB) |
| disk delete partition N | Delete MBR partition N (1-4) |
| disk format [fs=...] [label=...] [au=...] [quick] | diskpart-style format of the volume |

### chkdsk

Reports the volume label, total, used and free space, the allocation unit size,
and the total and free cluster counts.

`/F` additionally walks every directory verifying that each entry can be read,
reporting file and directory counts plus any directory it could not open.
Recursion is bounded to 8 levels.

**The check is read-only.** This firmware never rewrites FAT structures. A card
with genuine corruption should be imaged and repaired on a host machine, so
`chkdsk` reports problems rather than attempting an in-place fix that could make
the damage worse.

```
chkdsk
chkdsk /F
chkdsk logs\ /F
```

### format

Reformats the SD card. **All data is destroyed.**

| Option | Meaning |
|--------|---------|
| /FS:type | Filesystem type: `FAT` or `FAT32`. Both use the standard ESP-IDF
  helper's size-appropriate selection (FAT12/16 for small volumes, FAT32 for
  modern SD cards). `EXFAT` is not available in this firmware build; requesting
  it warns and formats as FAT32 instead. |
| /A:size | Allocation unit (cluster) size in bytes, with an optional K/M suffix
  (e.g. `/A:32K`). Range `P4_CONFIG_FORMAT_ALLOC_UNIT_MIN`..`_MAX`. |
| /V:label | Volume label to apply afterwards, 11 characters or fewer |
| /Q | Accepted for DOS familiarity; the underlying operation is always quick |

The command prints a warning and requires the exact confirmation word
(`P4_CONFIG_DESTRUCTIVE_CONFIRM_WORD`, default `YES`) typed at the prompt
before anything is written. The confirmation is read one key at a time
through the shell's key queue, so the reply never reaches the command
dispatcher. (Over serial, type the whole word — `YES` — and press Enter on
one line; the on-screen keyboard types it naturally.)

If no interactive input source is attached (no UART console and no USB
keyboard) the command **refuses outright** rather than proceeding, so a batch
file can never silently wipe a card. The same exact-word gate protects `disk
clean`, `disk delete partition`, recursive `del /s`, recursive `rd /s`,
`trash empty`, and `trash purge`.

After formatting, the FAT type, label, capacity and allocation unit size are
reported. The card must be initialized (mounted, or left initialized by a prior
`disk` command in the same session); a card the BSP cannot mount after a reboot
must be recovered on a host machine or with `BOARD_CFG_SD_FORMAT_ON_MOUNT_FAIL`.

```
format
format /FS:FAT32 /V:DATA
format /FS:FAT32 /A:64K /V:ROOT
```

### disk

diskpart-style physical-disk and partition management for the SD card.

| Subcommand | Meaning |
|------------|---------|
| disk list | Show the physical disk(s): card name, capacity, sectors, sector size |
| disk detail | Show disk geometry and the MBR partition table (boot flag, type, start, size) |
| disk clean | Remove all MBR partitions (destructive, requires `YES`) |
| disk create partition primary [size=N] | Create a primary FAT32 partition aligned to 1 MiB; `size` in MB (default: rest of card) |
| disk delete partition N | Delete MBR partition N (1-4), destructive, requires `YES` |
| disk format [fs=FAT32] [label=X] [au=size] [quick] | diskpart-style format of the volume |

After `disk clean` or `disk create partition primary`, run `format` (or
`disk format`) to create the filesystem. `disk clean`/`delete`/`format` require
the exact `YES` confirmation word like `format`.

```
disk list
disk detail
disk clean
disk create partition primary
disk create partition primary size=2048
disk delete partition 1
disk format fs=fat32 label=DATA au=32K quick
```

## Boot Scripting

At every boot the firmware looks for CONFIG.SYS and AUTOEXEC.BAT on the SD
card root (components/boot). Missing files are generated once from built-in
templates when P4_CONFIG_BOOT_GENERATE_DEFAULTS is set. With no SD card the
boot path prints a muted "No SD card detected" line on the display and serial
console plus a header notification, instead of being a silent no-op.

The SD card is mounted lazily by the first SD command (`shell_sd_begin()`).
The first mount of each boot fires a one-shot hook
(`storage_register_sd_first_mount_callback`, wired in main to
`boot_on_sd_first_mount`) that generates the default boot files when they are
missing and prints an "SD card ready" welcome. Existing user files are never
touched.

CONFIG.SYS is parsed line-by-line. Blank lines and REM/; comments are
skipped; keywords are case-insensitive. Directives:

| Directive | Effect |
|-----------|--------|
| SET name=value | Set a batch environment variable |
| PATH=dir1;dir2;... | Set the command search PATH |
| PROMPT=template | Set the DOS prompt template |
| ECHO ON\|OFF | Set the batch echo default |
| ROTATE=0\|90\|180\|270 | Set display rotation |
| BRIGHTNESS=0-100 | Set backlight brightness |
| DISPLAY_POWER=ON\|OFF\|SLEEP | Set display power state |
| VOLUME=0-100 | Set speaker volume |
| RGB=r,g,b \| #RRGGBB \| <effect>[,speed] \| OFF \| AUTO,<ON\|OFF> | Set the WS2812 status LED (commas separate arguments) |
| WIFI_SSID=... WIFI_PASSWORD=... | Store auto-connect target (password never echoed) |
| WIFI=ON\|OFF WIFI_AUTOCONNECT=ON\|OFF | Wi-Fi runtime policy |
| BLUETOOTH=ON\|OFF BT_ADVERTISE=ON\|OFF | Hosted BLE policy |
| USB_KEYBOARD=ON\|OFF USB_MOUSE=ON\|OFF | USB HID policy |
| GPIO <n> = OUT [HIGH\|LOW] | Initial level at a safe output pin (reserved pins refused) |
| *any* NAME=VALUE | Any unrecognized KEY=VALUE sets a batch environment variable |

Unknown keywords without a value print a single muted warning and are skipped. AUTOEXEC.BAT
then runs through the normal batch pipeline with full batch power
(if/for/goto/call, pipes, redirection, chaining).

All limits and filenames are configurable in p4minishell_config.h
(P4_CONFIG_BOOT_*) and documented in p4minishell_config.yaml under
boot_scripting.

## Storage Guardrails

Larger file operations check the volume before they start, so a failure is
reported up front instead of leaving a half-written file behind.

- **Free-space precheck** — `copy`, `move`, `write`, and `append` refuse an
  operation that would leave less than 64 KB free. An overwrite credits the
  space the destination already occupies, so replacing a file in place does not
  need double the room.
- **Self-copy protection** — `copy a.txt a.txt` and the equivalent `move` are
  refused. Without this the destination would be opened for truncation and the
  source destroyed before the first read. The comparison is case-insensitive
  because FAT is.
- **Partial-destination cleanup** — if a copy fails mid-transfer the incomplete
  destination is deleted and the removal is reported, so a truncated file never
  masquerades as a complete one.
- **Progress reporting** — copies of files larger than 256 KB report percentage
  progress in 10% steps.
- **Capacity reporting** — `sd info`, `chkdsk`, and the end of every `dir`
  listing show current free space.
- **Attribute preservation** — `copy`, `move`, and `xcopy` carry the source's
  R/H/S/A attributes to the destination, and `xcopy /S` carries them onto the
  directories it creates. Attributes are applied after the data is written,
  because a read-only destination cannot be opened for writing. A failure to
  set them is logged but does not fail an otherwise-complete copy. Copying onto
  an existing read-only file reports `use attrib -R to clear it`.

## Text and Directory Utilities

All of these resolve relative paths against the current directory and run inside a
guarded SD session.

| Command | Description |
|---------|-------------|
| edit <path> | Open a DOS-style inline text editor (see below) |
| find <text> [file] [/I] [/N] [/C] [/V] | Search a file for a literal substring |
| find [path] [/NAME:pat] [/SIZE:spec] [/NEWER:date] [/OLDER:date] [/DIRS] [/B] | Recursively list files by name / size / date |
| findstr [switches] <search> [file...] | Classic DOS text search, literal or regex-lite |
| more [file] | Page a text file, waiting for a key between pages |
| tree [path] [/F] [/A] | Draw a recursive directory outline |

### edit

Opens a modal, touch-first text editor for any byte-preserving file: batch
scripts, `.txt`, `.sys`, or any other extension (`*.*`). `edit <path>` loads the
file (or starts a new buffer when it does not exist); `edit` with no path opens
an unnamed buffer. The editor surface is exactly the size of the shell
transcript (the keyboard stays at the bottom of the screen), and it is fully
usable from the touch keyboard, a USB keyboard, or the serial console. See
[tutorial_edit.md](tutorial_edit.md) for the complete tutorial and
[editor.md](editor.md) for the quick reference.

Options (writerdeck):

| Option | Effect |
|--------|--------|
| `/focus` | Start in **focus / typewriter mode**: the header and on-screen keyboard are hidden and the caret stays vertically centred. Toggle in-session with `Ctrl+Shift+F` / the nav-page `Focus` key / `\focus`. |
| `/template <name>` | Seed a **new** buffer from `sd:/TEMPLATES/<name>.MD` (see [templates](#document-templates)). Ignored for an existing file (never overwritten). |

The status bar shows the live **word count** (`W n`, `P4_CONFIG_EDITOR_WORD_COUNT`)
plus the `FOCUS` and `SPELL` session flags. **Spellcheck** underlines
misspellings from an SD wordlist (`sd:/DICTS/<name>.words`); toggle with the
`Spell` key or `Ctrl+Shift+S` / `\spell` (see [spellcheck](#spellcheck)).

- **Line numbers**: a right-aligned gutter shows each line's 1-based number
  (`P4_CONFIG_EDITOR_LINE_NUMBER_WIDTH_CHARS` digits, muted), and the cursor's
  current line is softly highlighted (`P4_CONFIG_EDITOR_CURRENT_LINE`).
  `Go to line` (`Ctrl+G` / `\g` / nav-page `Goto`) jumps straight to any
  numbered line.

- **Editing**: insert, backspace, Delete (forward), Enter (new line, carrying
  the current indent), Tab
  (spaces to the next tab stop, width `P4_CONFIG_EDITOR_TAB_WIDTH`), Home / End,
  Up / Down, PageUp / PageDown, word navigation (`Ctrl+Left` / `Ctrl+Right`),
  document start/end (`Ctrl+Home` / `Ctrl+End`), delete line (`Ctrl+Y`), and an
  Insert/overwrite toggle (`Insert` / `Ins`). Bracket jump (`Ctrl+B` / `\b`)
  hops between matching parens (nesting-aware, skips strings/comments) and
  `%var%` pairs. Cursor movement is byte-precise
  over tabs and 8-bit characters; files round-trip unchanged (CRLF vs LF is
  preserved, and a trailing newline is only written when the original file had
  one).
- **Selection**: Shift+arrows / Ctrl+A (USB) or long-press-and-drag (touch)
  selects text; Ctrl+C / Ctrl+X / Ctrl+V copy, cut, and paste through the RAM
  clipboard. A background overlay highlights the selection, and a blinking
  block cursor marks the caret.
- **Find / Replace / Go to**: Ctrl+F (or `Find`) searches forward from the
  cursor, wrapping; F3 / Enter repeats the last search; Ctrl+H (or `Rep`)
  replaces one match at a time (Enter repeats); Ctrl+R (or `All`) replaces
  every match in one undo step; Ctrl+T (or `Case`) toggles case sensitivity
  (session, default insensitive); Ctrl+G (or `Goto`) jumps to a
  line number. The search strings are typed into the status bar and cancelled
  with Esc.
- **Undo / Redo**: Ctrl+Z / Ctrl+Shift+Z (USB) or `\u` / `\r` (serial).
  Undo restores the dirty flag too: undoing back to the opened state clears
  the mark, so `\q` quits without a prompt.
- **Save / Quit / Reload**: Ctrl+S / F2 saves to the source path (keeping a
  `<file>.bak` copy of the previous version); Ctrl+O / `SaveAs`
  saves to a new path (unnamed buffers are prompted for a name); Ctrl+L /
  `\l` reloads from disk, discarding edits; Esc / `\q`
  (serial) quits, with a `Y/N` confirmation whenever there are unsaved
  changes. Read-only files (FATFS `+R`) refuse saves with a status message —
  use Save As. A failed save removes the partial destination and keeps your
  edits in memory.
- **Wrap / comment**: Ctrl+W (or `\w`) toggles word wrap for long rows
  (cursor, selection, scroll, and touch all follow visual rows); Ctrl+/
  (or `\co`) toggles line comments over the selection or cursor row
  (`rem ` for batch, `// ` for JSON, `<!-- ... -->` for Markdown).
- **Status bar**: path, `*` modified flag, `Ln`/`Col`, `INS`/`OVR`, syntax
  (`bat`/`md`/`json`/`txt`), `CRLF` when applicable, `RO` for read-only,
  plus `PREVIEW`/`WRAP` modes.
- **Markdown**: `.md`/`.markdown`/`.mkd` files get Markdown highlighting
  (headings bold, code spans yellow, links cyan+underline, markers green).
  `Ctrl+P` (USB), nav-page `Prev` (touch), or `\p` (serial) toggles a
   read-only rendered preview on the same surface (status shows `PREVIEW`;
   navigation scrolls, edits are discarded, `Ctrl+P`/`\p` returns to source,
   Esc quits). Preview renders Markdown files only (other syntaxes report
   `preview needs a Markdown file`) and refuses documents past 96 KB
   (`too large to preview`). The first render can take tens of seconds after heavy
  transcript use (TTF variant load over the SD bus shares the O6 latency
  tail); the status line shows `rendering preview...` meanwhile.
- **Touch keyboard**: the symbol page (reachable via `1#`) adds a `Nav`
  button that opens a navigation page (Tab, Ins, Del, arrows, Home/End,
  PgUp/PgDn, Find, Next, Rep, All, Case, Goto, Undo, Redo, Save, SaveAs, Quit), and a
  second `Nav2` page adds the clipboard and advanced editing (Copy, Cut,
  Paste, SelAll, WdL/WdR word nav, DocH/DocE, DelLn, DelE). Every editor
  feature is reachable from the touch keyboard alone. Mode switching
  (abc / ABC / 1# / Nav / Nav1 / Nav2) is handled by the shell keyboard
  callback.
- **Serial console**: while the editor is open, UART lines are fed to the
  editor (`\q` quit, `\s` save, `\f` find, `\g` go-to-line, `\o` save-as,
  `\u` undo, `\r` redo, `\a` select-all, `\p` preview, `\all` replace-all,
  `\c` case toggle, `\b` match jump, `\co` comment, `\w` wrap, `\l` reload;
  any other line is typed).
- **Syntax**: batch `.bat`/`.cmd` files are syntax-highlighted by the batch
  lexer (commands, comments, labels, `%VAR%`, strings, operators); `.json`
  files by the JSON lexer (keys, strings, numbers, literals); Markdown by
  the Markdown lexer. The mapping lives in the central file-type registry
  (`components/filetype/`), shared with the viewer, `launch`, `dir`
  colours, and batch resolution.

Editor limits are `P4_CONFIG_EDITOR_MAX_BYTES` (1 MB) and
`P4_CONFIG_EDITOR_MAX_LINES` (65536); files beyond these are refused with an
error instead of being truncated. The editor is safe against data loss: an
existing file that cannot be loaded never silently opens as an empty buffer.
| fc <file1> <file2> | Compare two text files line by line |
| comp <file1> <file2> [/D] [/A] [/L] [/N=n] [/C] | Classic DOS byte-for-byte comparison |
| sort [file] [/R] [/I] [/U] | Print a file with its lines sorted |

All text tools set a DOS ERRORLEVEL: `find`, `findstr`, and `comp` return 0
when something is found / identical, 1 when not found / different, and 2 on
usage or an error; `more`, `fc`, and `sort` return 0 on success, 1 on
differences / failure, and 2 on usage. This makes `if errorlevel`, `&&`, and
`||` work with every text command.

### find

`find` has two modes. The presence of any discovery switch selects file
discovery; with the classic switches only it is the original text search.

**Text search:** `find <text> [file]`

| Switch | Meaning |
|--------|---------|
| /I | Case-insensitive match |
| /N | Prefix each match with its line number |
| /C | Print only the match count |
| /V | Print the lines that do NOT match |

With no file argument, `find` reads the pending `<` or pipe input source.

Example: `find "timeout" boot.log /I /N`

**File discovery:** `find [path] [/NAME:pattern] [/SIZE:spec] [/NEWER:date] [/OLDER:date] [/DIRS] [/B]`

Recursively walks the starting directory (default: current directory) and
prints entries that pass every supplied filter.

| Switch | Meaning |
|--------|---------|
| /NAME:pattern | Filename wildcard (e.g. `*.log`, `*config*`). A wildcard in the path argument also splits into directory + pattern, like `dir`. |
| /SIZE:spec | Byte size filter with an optional K/M/G suffix. Use the redirection-safe range syntax `N-M` (range), `N-` (at least), `-M` (at most), or `N` (exact). The comparison forms `>N` / `>=N` / `<N` / `<=N` are also accepted when quoted (bare `>`/`<` are the shell's input/output operators). |
| /NEWER:date | Only entries modified on/after `YYYY-MM-DD` |
| /OLDER:date | Only entries modified on/before `YYYY-MM-DD` |
| /DIRS | Include directories as well as files |
| /B | Bare: full paths only, no colour — redirectable / pipable |

Examples:
```
find /NAME:*.log
find /sdcard /NAME:*config* /SIZE:1K-64K
find /logs /NAME:*.txt /NEWER:2026-01-01 /B
find /DIRS /B
```
Recursion is depth-bounded by `P4_CONFIG_DIR_RECURSE_DEPTH_MAX` and matches are
capped by `P4_CONFIG_FIND_MATCH_MAX` (256), with a clear truncation note.

### more

Prints 20 lines per page (`P4_CONFIG_MORE_PAGE_LINES`), then waits at the
`-- More --` prompt. Enter or Space advances one page; `Q` stops. With no file argument
it reads the pending `<` or pipe input source.

### tree

Recursive directory outline with DOS box-drawing connectors.

| Switch | Meaning |
|--------|---------|
| /F | Include files as well as directories (DOS default is directories only) |
| /A | Use plain ASCII connectors |

Recursion is bounded to 8 levels (`P4_CONFIG_TREE_DEPTH_MAX`) and the total entry count
to 128 (`P4_CONFIG_SD_LIST_LIMIT`).

Example output:

```
Folder PATH listing for volume A:
/sdcard/projects
+---src\
|   +---main.c
|   \---util.c
\---docs\
    \---readme.txt

2 directories, 3 file(s)
```

### fc

Reports each differing line with both files' contents, and flags lines present in only
one file when the lengths differ. Prints `FC: no differences encountered` when the files
match.

### sort

| Switch | Meaning |
|--------|---------|
| /R | Reverse (descending) order |
| /I | Case-insensitive comparison |
| /U | Drop duplicate lines after sorting |

Holds up to 1024 lines (`P4_CONFIG_SORT_LINE_MAX`) and reports when the input is
truncated. With no file argument it reads the pending `<` or pipe input source.

Example: `sort names.txt /I /U > unique.txt`

### findstr

Classic DOS text search. Case-sensitive by default (the opposite of `find`),
with optional limited regular expressions. Reads the pending `<` or pipe input
source when no file is given.

Usage: `findstr [switches] <search> [file...]`

| Switch | Meaning |
|--------|---------|
| /R | Treat the search as a regular expression |
| /C:"string" | A literal search string (space-safe; always literal even with /R) |
| /I | Case-insensitive match |
| /N | Prefix each match with its line number |
| /V | Print the lines that do NOT match |
| /X | Match only whole lines |
| /E | Match only lines that end with the string |
| /B | Match only lines that begin with the string |
| /L | Treat the search literally (default) |
| /S | Recurse into subdirectories |
| /M | Print only the names of files that contain a match |
| /F:file | Read the file list to search from `file` (one path per line) |
| /G:file | Read additional search strings from `file` (one per line) |

Multiple search strings (from `/C:`, `/G:file`, and one bare string) OR
together. The first bare argument is the single search string; any further
bare arguments are files, matching DOS findstr.

**Regex subset** (the engine implements only what DOS findstr supports):
`.` matches any character, `*` matches zero or more of the preceding atom,
`^` / `$` anchor the start / end of the line, `[class]` / `[^class]` /
`[a-z]` are character classes, `\<` / `\>` match word boundaries, and `\c`
escapes a metacharacter as a literal.

When more than one file is searched, each matching line is prefixed with the
filename (plus its line number under `/N`).

Examples:
```
findstr timeout boot.log /I /N
findstr /R "b[0-9]+" config.txt      (the engine's subset has no +; use b[0-9][0-9])
findstr /C:"timeout exceeded" *.log
findstr /S /M error src
findstr /G:patterns.txt file.txt
```

ERRORLEVEL: 0 = at least one match, 1 = no match, 2 = usage / error.

### csv rows|cols|cell|get|set|eval <file> [row col] [/b] [/v:NAME]

Minimal spreadsheet substrate for batch apps: RFC-4180-subset parsing (comma
separators, `"quoted"` fields, `""` escapes) over a guarded SD read, plus
`=EXPR` formula evaluation through the `calc` engine with `R<row>C<col>`
references and range aggregates. The parser core lives in
`components/storage/storage_csv.c`; the verbs are
`components/command/csv_commands.c`.

- `csv rows <file>` / `csv cols <file>` — the first row's field count.
- `csv cell|get <file> <row> <col> [/v:NAME]` — one field (1-based row/col).
- `csv set <file> <row> <col> <value...>` — replace one cell in place
  (ragged rows extend, past-EOF rows append as counted fillers, untouched
  rows stream through byte-for-byte, atomic temp+rename behind a space
  estimate; multi-line or over-wide fields are refused, never corrupted).
- `csv eval <file>` — resolve every `=EXPR` cell iteratively (up to
  `P4_CONFIG_CSV_PASSES` passes, so forward and chained references work) and
  print the grid aligned, or re-quoted CSV under `/b`.

Range aggregates inside `=EXPR` (either corner order, spaces tolerated):
`SUM`/`AVG`/`MIN`/`MAX` over `R1C1:R2C2` fold VAL-semantics numbers
(blanks read as 0, out-of-grid clipped, empty folds to 0) and `COUNT`
tallies non-empty cells.

An explicit file wins; otherwise the active `< file` / pipe source is read
(`set` needs an explicit file). References out of range or non-numeric read
as 0. Cells that never resolve (bad expression or a reference cycle) are
reported as warnings and left as their source text.

```
csv rows prices.csv
csv cell prices.csv 2 3 /v:PRICE
csv set prices.csv 2 3 19.95
csv eval sheet.csv            # aligned
csv eval sheet.csv /b         # machine CSV for another tool
```

A sheet is `a,b,c` newline `x,2,=R2C2*10` newline `p,q,=R2C3+R3C3`;
`=SUM(R2C1:R3C3)` totals a block; `eval` turns the formula cells into their
computed values.

ERRORLEVEL: 0 ok, 1 empty/out-of-range/unresolved, 2 usage.

### gfind

Palm-style global find across the structured stores: the `db` record databases
and the `alarm`/calendar store.

Usage: `gfind <text> [/b] [/i] [/count] [/cat:N] [/field:k=v] [/db:name] [/noalarms] [/nodb]`

| Switch | Meaning |
|--------|---------|
| `/b` | Bare output (pipe/`for /f` friendly) |
| `/i` | Case-insensitive match |
| `/count` | Print `gfind.db`/`gfind.alarms`/`gfind.total` counts instead of rows |
| `/cat:N` | Restrict database matches to category `N` |
| `/field:k=v` | Require a `k=v` payload field to match |
| `/db:name` | Search only the named database (default: all databases) |
| `/noalarms` | Skip the alarm store |
| `/nodb` | Skip the databases |

The search text is a single positional argument — quote it when it contains
spaces (`gfind "team call"`); more than one positional is a usage error.
Prints `gfind.matches` with the number of matches (unless `/b`). Secret records
are only searched/revealed while the device is unlocked; `conceal hide` skips
them entirely. Memory of the note app (`db` records) and the calendar are both
covered, so `gfind` is the Palm-style global find.

Examples:
```
gfind Standup
gfind /i /b meeting /db:contacts
```

ERRORLEVEL: 0 = at least one match, 1 = no match, 2 = usage / error.

### comp

Classic DOS byte-for-byte file comparison.

Usage: `comp <file1> <file2> [/D] [/A] [/L] [/N=number] [/C]`

| Switch | Meaning |
|--------|---------|
| /D | Show byte offsets in decimal |
| /A | Show the differing bytes as ASCII characters |
| /L | Show line numbers instead of byte offsets |
| /N=number | Compare only the first `number` lines |
| /C | Ignore case when comparing |

Reports each mismatch with its offset and both byte values, stopping after
`P4_CONFIG_COMP_MISMATCH_MAX` (10) mismatches with `N mismatches - ending
compare`. Identical files print `Files compare OK`.

ERRORLEVEL: 0 = identical, 1 = different, 2 = usage / error.

## Wi-Fi Commands

| Command | Description |
|---------|-------------|
| wifi status | Multi-line colour-coded report: state, target SSID, SSID, BSSID, channel, RSSI, PHY mode, IPv4, netmask, gateway, DNS, association uptime |
| wifi scan | Scan for nearby access points, sorted by RSSI (strongest first), column-aligned, capped at `P4_CONFIG_WIFI_SCAN_LIMIT` |
| wifi scan /b | Bare scan: one SSID per line, no colour — redirectable, machine-parsable |
| wifi diag | Run full diagnostic: connection state, IP status, network scan |
| wifi connect | Connect using sdkconfig default credentials |
| wifi connect <ssid> <pass> | Connect with runtime credentials (password masked) |
| wifi disconnect | Disconnect current station session |
| wifi known | Show saved networks (SSIDs only — never passwords) with preferred / priority / last-used markers |
| wifi save [ssid] | Save the connected (or given) network to the SD known-list |
| wifi forget <ssid> / wifi delete <ssid> | Remove one saved network |
| wifi forget all / wifi clear known | Clear the entire known-list |
| wifi preferred <ssid> | Mark a saved network as preferred for auto-connect |
| wifi throughput tx <host> [port=N] [mb=N] [udp] | TCP/UDP send bench to `host`; prints Mbit/s (host peer: `tools/wifi_bench.py`) |
| wifi throughput rx [port=N] [mb=N] [udp] | TCP/UDP receive bench; the host connects and sends (same-subnet host required) |

### Wi-Fi Behavior
- Boot-time startup in background task (does not block shell UI)
- ESP-Hosted version compatibility gate: compares the C6-reported major against `P4_CONFIG_HOSTED_COMPAT_MAJOR` (3.x froze its public compat macros at the 2.12.6 baseline)
- If the C6 version read fails, the transport is reset (`esp_hosted_deinit()` + init + connect) and the read is retried once before Wi-Fi is declared failed — this recovers the first-RPC SDIO timeout seen on the M5Stack Tab5 without affecting boards where the first read succeeds
- Hosted SDIO link runs at `CONFIG_ESP_HOSTED_HOST_SDIO_CLK_KHZ`: 40000 on the reference board (soak-verified), 10000 on the M5Stack Tab5
- Recovery guidance points to coprocessor/esp32c6_slave or c6ota default; a factory `v2.3.0` C6 (as shipped on the Tab5) needs a one-time standalone flasher first (see `PORTING.md` §6)
- Restores automatically after successful c6ota
- Transcript-facing diagnostics on boot and post-OTA restore

### Persistent Known Wi-Fi Networks
- The known-list is a plain, hand-editable file at `sd:/WIFI.KNOWN` (see
  `P4_CONFIG_WIFI_KNOWN_FILE`), one network per line:
  `SSID|PASSWORD|AUTH|PRIORITY|PREFERRED|LAST_CONNECTED|CONNECT_COUNT`.
- On boot with `WIFI_AUTOCONNECT=ON`, the firmware scans and connects to the best
  visible known network (preferred / highest priority / strongest RSSI), then
  falls back to the classic single-credential path when the list is empty or no
  known network is in range. `WIFI_AUTOCONNECT` remains the master switch.
- Successful connections (`wifi connect`, CONFIG.SYS `WIFI_SSID=`/`WIFI_PASSWORD=`,
  or auto-connect) update the list automatically when the SD card is present.
- When the SD card is absent, ejected, read-only, full, or the file is missing or
  corrupt, every known-list command reports a clear message and sets
  ERRORLEVEL 1; boot and normal operation continue unchanged. Passwords are
  stored in the file but never echoed to the transcript, history, or debug log.

## Connectivity Commands

### ping <host-or-ip> [count]

Classic ICMP echo. Sends `count` echo requests (default 4, hard maximum 10),
prints each reply line and a DOS-style statistics summary, then sets
ERRORLEVEL so batch files can branch:

- **ERRORLEVEL 0** — at least one reply was received (success)
- **ERRORLEVEL 1** — every request timed out, the host did not resolve, or
  Wi-Fi is not started / not connected
- **ERRORLEVEL 2** — usage error (missing or malformed arguments)

```
ping 8.8.8.8
ping 192.168.1.1 5
ping example.com
```

Example output:

```
Reply from 192.168.1.1: bytes=32 time=2ms TTL=64

--- 192.168.1.1 ping statistics ---
    Packets: Sent = 4, Received = 4, Lost = 0 (0% loss),
Approximate round trip times in milli-seconds:
    Minimum = 2ms, Maximum = 4ms, Average = 3ms
```

The command participates fully in the pipeline:

- **Redirection**: `ping 8.8.8.8 > ping.txt` writes the plain-text report to
  an SD file (ANSI colour is stripped for redirected output).
- **Pipes**: `ping 192.168.1.1 | find "Reply"` feeds the reply lines to the
  next stage.
- **Chaining**: `ping 8.8.8.8 && echo network_up`, `ping 10.0.0.1 || echo down`.
- **Batch files / AUTOEXEC.BAT**: `if errorlevel 1 goto nolink` works exactly
  as in DOS; the session runs on its own task and the worker task blocks only
  for a bounded total (`count` × (timeout + interval) + margin), so it never
  hangs.

A count above 10 is clamped with a warning; timings and packet loss are
reported in classic `Sent/Received/Lost` + `Minimum/Maximum/Average` form.

### dns <hostname> (alias nslookup)

Resolves the A records of a hostname through lwIP DNS and prints the IPv4
address list.

```
dns example.com
nslookup example.com
```

- **ERRORLEVEL 0** — resolved at least one A record
- **ERRORLEVEL 1** — could not resolve, or Wi-Fi is not started
- **ERRORLEVEL 2** — usage error

Redirectable and piped like any other command: `dns example.com > dns.txt`.
With this build's lwIP DNS cache (`CONFIG_LWIP_DNS_MAX_HOST_IP=1`) a name
normally resolves to a single A record.

### httpget <url> [localfile] (alias wget)

Performs a simple HTTPS (or HTTP) GET over the same `esp_http_client` stack
used by `c6ota`. No headers to set, no methods beyond GET — this is the
deliberately minimal diagnostic/scraping tool.

- `httpget https://example.com` — prints the response header (HTTP status,
  content-type, size) and then the response body to the transcript. The body
  print is bounded (`P4_CONFIG_HTTP_PRINT_BODY_BYTES`, 4 KiB) and sanitized so
  a binary payload cannot corrupt the transcript.
- `httpget https://example.com/page.html` — saves the **exact** body to the
  current working directory / SD card through the storage write path: cwd-
  relative resolution, guarded SD session, free-space precheck (an overwrite
  reclaims the old file's space), and partial-destination cleanup if the write
  fails. Prints `httpget: saved N bytes to <path>` on success.

```
httpget https://example.com
httpget http://192.168.1.1/status > status.txt
httpget https://example.com/page.html downloaded.html
```

**ERRORLEVEL** (works in batch files and AUTOEXEC.BAT):

- **0** — an HTTP 2xx was received and (with a localfile) the body saved.
- **1** — any failure: non-2xx status, connection refused / TLS failure,
  unreachable host or timeout, body over the size cap, SD write failure, or
  Wi-Fi not connected.
- **2** — usage error (missing URL or unsupported URL scheme).

A URL with a `user:pass@` prefix (e.g. `httpget http://user:pass@host/page`)
sends HTTP Basic authentication, so password-protected endpoints (including
this firmware's own `httpd` file server) can be fetched.

### tcpterm <host> <port> [/t:secs] [text...]

One-shot TCP request/response session (the modern Datacomm: the 95LX spoke
RS-232/modem; this board has no RS-232 peer, so the wire endpoint is TCP). It
resolves the host, connects with a bounded budget, sends one request, half-
closes, and prints the reply. Owned by `components/networking/tcpterm.c`
(the same module that owns `ping`/`dns`/`httpget`).

- `text...` joins as the request payload with `\r` `\n` `\t` `\\` escapes, so
  an HTTP probe reads naturally.
- With no text, the active `< file` or pipe stage feeds the request instead
  (assembled before the call), so `type req.txt | tcpterm host 80` works.
- `/t:secs` sets the idle timeout (default `P4_CONFIG_TCP_IDLE_TIMEOUT_MS`,
  5 s), refreshed per received byte; the connect budget is
  `P4_CONFIG_TCP_CONNECT_TIMEOUT_MS`.
- Replies print sanitized like `httpget` bodies **except** that ESC passes
  through, so a remote terminal's SGR colours render on screen and over
  serial; other control bytes become `.`.
- The session ends with a `[tcpterm: host:port closed, N byte(s) in M out]`
  summary. Requires an active Wi-Fi connection.

```
tcpterm 127.0.0.1 80 /t:10 GET / HTTP/1.0\r\n\r\n
echo "PING" | tcpterm 192.168.1.50 5000
tcpterm example.com 80 "HEAD / HTTP/1.0\r\n\r\n" > head.txt
```

ERRORLEVEL: 0 session completed, 1 resolve/connect/send/Wi-Fi failure, 2 usage.

### httpd start | httpd stop | httpd status

Drives the HTTP file server that shares the SD card over the Wi-Fi link.

- `httpd start` — starts the server on `P4_CONFIG_HTTPD_PORT` (80). Refuses
  (ERRORLEVEL 1) when Wi-Fi is not connected.
- `httpd stop` — stops the server.
- `httpd status` — reports state (stopped/running/failed), port, auth on/off,
  docroot, open/max sockets, request count, and SD mount state.

The server maps URL paths onto files under the SD mount point; directories
return an HTML listing (bounded by `P4_CONFIG_HTTPD_LISTING_MAX`), files are
streamed with a Content-Type chosen from the extension, and unknown paths
return 404. `..` path traversal is refused with 400. When
`P4_CONFIG_HTTPD_AUTH_USERNAME` is non-empty, requests must send HTTP Basic
credentials (401 + `WWW-Authenticate` otherwise). The server auto-starts when
the station receives an IP and stops on disconnect
(`P4_CONFIG_HTTPD_AUTOSTART`).

**ERRORLEVEL:** 0 on success, 1 on failure (e.g. `httpd start` without a
connection), 2 on usage.

### netstat

Reports the network state by walking lwIP: the interface list (state, IPv4,
netmask, gateway, MTU, MAC), active TCP connections (local/remote `ip:port`
and state), TCP listeners, and UDP endpoints. Read-only, bounded by
`P4_CONFIG_NETSTAT_ROW_MAX`.

### ipconfig

Reports the full per-interface IP configuration: state, MAC, IPv4, netmask,
gateway, MTU, the default-route marker, and the configured DNS servers.

Examples in a batch file:

```
httpget https://example.com/api/v1/status > srv.json
if errorlevel 1 echo server_down
httpget https://example.com/page.html page.html && echo downloaded
```

**Redirection and pipes.** Output goes through the transcript appenders, so
`httpget url > file` captures the printed header + body and
`httpget url | find "200"` pipes it. For the complete untruncated body, use
the localfile form rather than `>` redirection.

**Config.** `P4_CONFIG_HTTP_TIMEOUT_MS` (default 15 s), 
`P4_CONFIG_HTTP_MAX_BODY_BYTES` (512 KiB PSRAM cap; larger responses are
refused with a clear error), `P4_CONFIG_HTTP_FOLLOW_REDIRECTS` (1 = follow up
to 3 redirects), and `P4_CONFIG_HTTP_USER_AGENT`. All HTTP/TLS code lives in
`components/networking/`; no socket, mbedTLS, or esp_http_client call exists
outside it. Requires an active Wi-Fi connection (`wifi connect` first); with
no connection it prints a clear DOS-style error and sets a non-zero
errorlevel. Timeouts are bounded so the worker task is never hung.

## Bluetooth Commands

| Command | Description |
|---------|-------------|
| bluetooth status | Colour-coded report: hosted readiness, controller, NimBLE, sync, scan, advertising (with active name), C6 firmware version, last error |
| bluetooth scan [limit] | Bounded BLE scan (default 8 s) through hosted NimBLE on C6; prints name + address + RSSI sorted strongest-first, up to `limit` (default 8) |
| bluetooth enable | Initialize the hosted controller + NimBLE host and report readiness (idempotent; scan/advertise also bring it up on demand) |
| bluetooth advertise on [name] | Start non-connectable BLE advertising. `name` is session-only (RAM-only, never persisted); without it the configured default name is used |
| bluetooth advertise off | Stop BLE advertising |
| bt ... | Alias for bluetooth command family |

### Bluetooth Lifecycle
- The hosted controller and NimBLE host initialize once on first use
- Subsequent scan/advertise commands reuse active session
- Hosted NimBLE VHCI on ESP32-C6 over ESP-Hosted SDIO
- `bluetooth scan` is always bounded by `P4_CONFIG_BT_SCAN_DURATION_MS`, so it
  always terminates and the worker task never hangs

## USB Commands

| Command | Description |
|---------|-------------|
| usb status | Show USB host, MSC, and HID state |
| usb ls [path] | Mount MSC drive at /usb0 and list files |
| usb keyboard on | Enable transcript echo for HID boot keyboard |
| usb keyboard off | Disable keyboard transcript echo |
| usb mouse on | Enable transcript echo for HID boot mouse |
| usb mouse off | Disable mouse transcript echo |
| usb userial status | Report the open CDC-ACM serial device (or none) |
| usb userial open <vid:pid> [baud=..] [data=..] [parity=..] [stop=..] | Open a CDC-ACM/virtual-COM device |
| usb userial close | Close the open device |
| usb userial send <text...> | Write to the device (or the `< file` / pipe source) |
| usb userial recv [/n] | Drain the RX ring (up to `n` bytes) to the transcript |
| usb userial term [/t:secs] [/raw] | VT100 terminal on the TUI grid (ESC exits, idle timeout; /raw = legacy transcript) |

### usb userial - USB CDC-ACM serial

Raw serial to external gear (GPS pucks, microcontrollers, scopes, serial
consoles) that presents a CDC-ACM / virtual-COM interface. One open device at
a time. The class driver and RX ring live in `components/usb/userial.c`; the
verbs live in `components/command/userial_commands.c` and use only the byte
API, so the `usb` component stays a leaf.

- `usb userial open 303a:1001 baud=9600 parity=E` — the VID/PID is hex; the
  open blocks up to `P4_CONFIG_USERIAL_OPEN_TIMEOUT_MS` for a matching device,
  then applies the line coding (default 115200 8N1).
- `usb userial send hello` or `type cmd.txt | usb userial send` — inline text
  wins; otherwise the `< file` / pipe source feeds the write.
- `usb userial recv` — drains up to `P4_CONFIG_USERIAL_RING_BYTES`; replies
  print sanitized (ESC passes for remote SGR colours).
- `usb userial term` — VT100 screen on the TUI grid (80x25): remote SGR
  colours, cursor motion (CUP/CUU/CUD/CUF/CUB), erase (ED/EL), save/restore,
  show/hide cursor, and the alt-screen buffer render into the grid, which
  scrolls instead of clamping at the last row. Split escape sequences across
  RX reads are reassembled (`P4_CONFIG_VT100_PENDING_BYTES`). Keys pump to
  the device (Enter goes as CR, ESC exits); the idle timeout (`/t:secs`,
  default `P4_CONFIG_USERIAL_TERM_IDLE_MS`) still applies. `/raw` keeps the
  legacy sanitized-transcript passthrough; `P4_CONFIG_VT100_ENABLE=0` makes
  `/raw` the default. Refuses headless (needs a key source). File transfer
  stays on the existing verbs — `send`/`receive` (USB-serial console) and
  `usb userial send`/`recv` (CDC-ACM device).

ERRORLEVEL: 0 ok, 1 no device/open failure/SD or write error, 2 usage.

### USB Keyboard Auto-Detect
- Plug in a USB HID keyboard to automatically type commands into the shell
- On-screen keyboard is automatically hidden when USB keyboard is detected
- On-screen keyboard is restored when USB keyboard is unplugged
- Full US keyboard layout supported: letters, numbers, symbols, keypad, navigation keys, function keys
- Modifier keys (Shift, Ctrl, Alt, GUI) are tracked for proper character mapping
- Special keys: Enter (submit command), Backspace, ESC (clear line), Tab, arrows (cursor/history), Delete, Home, End
- Use `keyboard show` to force the on-screen keyboard visible even with USB keyboard attached
- Use `keyboard hide` to hide it again; auto-detect resumes on next plug/unplug event

### Foreground break (Ctrl+C / Stop button)
A runaway foreground job (`for` loop, `delay`, runaway batch) stops
cooperatively with `^C`, exactly one message per press:

- **USB Ctrl+C** with no key-wait active requests the break (a running
  command unwinds at the next batch line, `for` body, or 100 ms `delay`
  chunk); at an idle prompt it clears the input line, DOS-style.
- **Input-row Stop button** (touch) shows only while a command runs and
  requests the same break; it stays hidden in editor/app/TUI modes.
- Key-wait sessions own their keys: a remote `term` session still receives
  `^C` (ESC exits), `pause`/`choice`/`menu` answer normally, the editor
  keeps Ctrl+C for copy, and reverse-search keeps its keys.
- One break unwinds the whole foreground job (`delay: stopped` inside
  `delay`, `^C` elsewhere); background jobs still stop via `taskkill`.

## SD Tools

| Command | Description |
|---------|-------------|
| sd | Show SD status and available subcommands |
| sd info | Mount SD; report card metadata, root availability, and filesystem total/used/free |
| sd ls [path] | List directory with full long filenames, entry types, sizes |
| sd stat <path> | Show resolved path, type, size, mode for file/directory |
| sd cat <path> [max_bytes] | Text-safe file preview (1-8192 bytes, non-printable sanitized) |
| sd mount | Mount the SD card, clearing the eject latch so a re-inserted card works without rebooting |
| sd eject / sdeject | Safe unmount before card removal |

## C6 OTA Commands

### c6ota <source>
Perform ESP-Hosted SDIO OTA update for ESP32-C6.

Sources:
- sd:/path/to/firmware.bin - Load image from SD card
- http://host/path or https://host/path - Download then transfer
- default - Auto-load esp32c6_hosted_slave.bin or network_adapter.bin from SD root

### OTA Flow
1. Validate source and show factory warning if needed (v2.3.0)
2. For HTTP/S: wait for Wi-Fi, download full image
3. Validate ESP-IDF app header (magic 0xE9 + chip ID 0x000D)
4. Prompt: WARNING: This will reboot the C6. Type YES to continue
5. Stop Wi-Fi, keep hosted transport alive
6. Transfer in 1500-byte chunks over SDIO
7. Progress: C6 OTA: XX% (YYYY KB / ZZZZ KB) every 5%
8. Success: C6 OTA completed successfully! Type reboot to activate new firmware.
9. Restore Wi-Fi on failure, request post-OTA restore on success

## BASIC-to-DOS Batch Mapping (FX-870P / VX-4)

The CASIO FX-870P/VX-4 BASIC command surface is provided through the existing
DOS-style batch language. Each verb maps to a DOS command (`calc`, `for /f`,
and `set /p < file` are the new additions that carry the math, file-input, and
string functionality):

| BASIC | DOS mapping |
|-------|-------------|
| ABS, ACS, ASN, ATN, COS, SIN, TAN, HYP, SQR, EXP, LN, LOG, FACT, NCR, NPR, INT, FIX, FRAC, ROUND, SGN, MOD, PI, RAN#, POL, REC, DMS/DMS$, VAL/VALF, STR$, HEX$, ASC, CHR$, LEN, LEFT$/MID$/RIGHT$ | `calc` functions |
| AMP_H (`&H` hex) | `calc &H..` / `0x..` literals |
| ANGLE | `calc /deg` / `calc /rad` / `calc /angle` |
| BEEP | `beep` |
| CHAIN, GOSUB, RETURN | `call`, `call :label`, `goto :eof` |
| CLEAR | `setlocal`/`endlocal` (scope restore) |
| CLS | `cls` / `clear` |
| DATA, READ, RESTORE | `for /f` over a data file; `set /p v=< file` |
| DELETE, KILL | `del` / `erase` |
| EDIT | `edit` |
| END | `exit /b` (end of the current batch file) |
| FILES | `dir` |
| FOR/NEXT | `for %%v in (…) do` (NEXT is the closing `)`) |
| GOTO | `goto` |
| IF/THEN/ELSE | `if cond (cmd) else (cmd)` |
| INPUT | `set /p` |
| INPUT#, LINE INPUT# | `set /p v=< file` / a pipe stage |
| LET | `set NAME=value` |
| LIST | `type file` / `findstr /n "^" file` |
| LOAD | `call file.bat` |
| MERGE | `copy` / `append` / `>>` |
| NAME | `ren` / `rename` |
| ON ERROR, RESUME | `if errorlevel N`, `\|\|`, `if not exist` |
| PRINT | `echo` |
| PRINT#, WRITE# | `write` / `append` / `>` / `>>` |
| REM | `rem` / `::` |
| RUN | invoke the `.bat` by name at the prompt |
| SAVE | `write` / `edit` (persist to SD) |
| STOP | `pause` |
| SYSTEM | `reboot` |
| VARLIST | `set` |
| TRON/TROFF | `echo on` / `echo off` |

Not applicable to this firmware (no stub, no equivalent): `LLIST`/`LPRINT`
(no printer), `MODE` (covered by
`display`/`keyboard`/`power`), `PASS` (no program lock), `DEFCHR$` (LVGL
fonts, not a character LCD), `DEFSEG`/`DEFM`/`PEEK`/`POKE`/`PBLOAD`/`PBGET`
(no memory pokes; hardware access is `gpio read`/`set`), `CALC$`/`CALCJMP`
(internal calculator ROM), `RENUM` (batch has no line numbers), `CONT` (no
program suspension), `VERIFY` (FATFS write verification is not exposed),
`OPEN`/`CLOSE`/`EOF` (batch files auto-open/close; `for /f` handles end of
file), `NEW` (no in-memory program; a fresh shell session), `DSKF` (reported
by `chkdsk`).

## Unsupported Commands

Stubs registered in the dispatcher that only print an error message are listed
here for reference and to prevent confusion if typed. (Implemented commands
with their own sections, like rgb, are not stubs.)

| Command | Reason |
|---------|--------|
| rgb <#RRGGBB|r g b|effect|auto> | WS2812 status LED (GPIO26) with auto status layer -- see Hardware Commands |
| camera init | Power and initialise the MIPI-CSI camera (Tab5 SC202CS via esp_video) |
| camera snap <file.bmp> | Capture a still to a 24-bit BMP on SD (BMP only) |
| imu [read] | Read the BMI270 accel/gyro + orientation; publishes IMU_* env vars |
| imu status | IMU presence, orientation, and auto-rotate state |
| imu rotate <on\|off> | Enable/disable tilt-based display rotation |
| shutdown (poweroff) | Flush, darken LEDs, cut board power (deep sleep when no latch) |

## Runtime Notes

- sd ls shows full long filenames (FATFS LFN, 255-char limit, UTF-8)
- Directory listings bounded to 128 entries per directory
- Directory recursion (`dir /s`, `tree`, `chkdsk /F`) bounded to 8 levels
- sd cat preview bounded to 8192 bytes
- **Recursive walkers keep their per-level buffers on the heap**, not the worker task stack.
  `dir /s`, `tree`, and `chkdsk /F` each release one level's buffer before descending, so
  memory use stays flat with depth and the 8 KB task stack is never at risk.
- **Free-space guardrails**: writes that would leave under 64 KB free are refused before any
  file is opened, so an overwrite cannot destroy existing contents and then fail
- **`format` requires interactive confirmation** and refuses to run when no key source is
  attached, so it can never execute unattended from a batch file
- **`chkdsk` is read-only** and never rewrites FAT structures
- SD VO4 LDO acquired at 3300 mV before mounts (no ldo warning spam)
- Commands run on worker task (prevents LVGL stack overflow)
- Family commands (wifi, sd, c6ota) receive full unsplit command text
- Serial prompt stateful: no P4Shell> spam during idle polling
- Password masking in transcript and command recall history (applied at both the caller and inside the history store, so no path can persist a Wi-Fi password)
- **Variable expansion & redirection**: Environment variables (`%VAR%`), batch script name (`%0`), batch arguments (`%1`..`%9`), all arguments (`%*`), output redirection (`>` / `>>`), and input redirection (`<`) are handled by `shell_execute_command()` in `components/command/command.c` before dispatch
- Nested execution contexts (`if`, `for`, pipes, batch lines) re-enter the full pipeline, so they inherit variable expansion and redirection
- Redirection captures exactly the transcript delta produced by the command, using
  `shell_transcript_get_length()` before dispatch and `shell_transcript_get_text_from()` after
- Input redirection and pipe stages both publish through one slot in `components/storage/`, so
  `sort < f.txt` and `type f.txt | sort` reach the same code in the text-processing commands
- Pipe stages spool through SD rather than streaming: the shell runs one command at a time on a
  single worker task, so there is no second process to stream into. Every spool file is removed
  on every exit path.
- **Keypress wait**: `pause`, `choice`, and `more` block on a real keystroke from the UART
  console, a USB keyboard, or the on-screen keyboard. While a wait is active every input source
  routes keys to the wait instead of the command line, so an answer is never dispatched as a
  command. Waits are bounded by a 30-second timeout, and fall back to a timed delay when no
  interactive key source is attached.
- **Prompt template**: `prompt` drives both the UART console and the LVGL input line from one
  template. The input line snapshots the prefix it painted, so a template or path change between
  two LVGL events cannot corrupt command extraction.
- **One quote/escape scanner**: the argument tokenizer, redirection parsing, pipe splitting,
  chain splitting, and variable expansion all call the same helpers in `components/shell/`, so
  they can never disagree about whether a character is syntax or data
- **Parse order**: chain separators are found before variable expansion, and expansion runs per
  chain segment. A variable whose value contains `&`, `|`, or `>` therefore cannot inject
  syntax into the line — its contents are always data.
- **Chain success test**: a link succeeds when it was recognized and did not leave a new
  non-zero errorlevel. The outcome is tracked per link rather than re-read from the global
  errorlevel, so a stale value from an earlier line cannot pick the wrong branch.
