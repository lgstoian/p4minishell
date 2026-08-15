# P4MiniShell Command Reference

Complete reference for all shell commands available in P4MiniShell.

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

## Where Commands Live

| Command group | Implemented in |
|---------------|----------------|
| `help`, `sysinfo`, `version`/`ver`, `about`, `mem`, `debug` | `components/shell/shell.c` |
| `cd`/`chdir`, `dir`, `copy`, `move`, `del`/`erase`, `ren`/`rename`, `md`/`mkdir`, `rd`/`rmdir`, `type`, `write`, `append`, `touch` | `components/storage/storage_commands.c` |
| `attrib`, `label`, `xcopy`, `find`, `findstr`, `more`, `tree`, `fc`, `comp`, `sort` | `components/storage/storage_commands.c` |
| `chkdsk`/`scandisk`, `format` | `components/storage/storage_commands.c` |
| `sd info|ls|stat|cat|mount|eject`, `sdeject` | `components/storage/storage_commands.c` + `storage.c` |
| `set`, `calc`, `path`, `echo`, `call`, `if`, `for`, `goto`, `shift`, `pause`, `choice`, `setlocal`, `endlocal`, `exit` | `components/batch/batch.c` + `components/batch/calc.c` |
| Batch file execution, `:label` scanning, `for` loops, `\|` pipes, setlocal scoping | `components/batch/batch.c` |
| Keypress wait (`pause`, `choice`, `more`) and the `prompt` template engine | `components/shell/shell.c` |
| `brightness`, `rotate`, `battery`, `power`, `sleep`, `deepsleep`, `pwm`, `freq`, `adc`, `i2c`, `spi`, `rgb`, `volume`, `gpio`, `display`, `keyboard`, `windows` | `components/command/command.c` |
| `config` (persistent settings / CONFIG.SYS + factory reset) | `components/command/config_cmd.c` |
| `reboot`, `clear`/`cls`, `prompt` | `components/command/command.c` |
| `date`, `time`, `timezone`, `sntp`/`ntpsync` | `components/clock/clock_commands.c` (dispatched from command.c) |
| `wifi`, `bluetooth`/`bt`, `usb`, `c6ota`, `httpd`, `netstat`, `ipconfig` (family routing) | `components/command/command.c` → owning module |
| `ping`, `dns`/`nslookup`, `httpget`/`wget` (dispatched here, implemented in networking) | `components/command/command.c` → `components/networking/` |

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
| about | Show shell and board summary with build metadata, header description, uptime, task count, the proprietary notice, and a third-party license summary |
| debug | Show last 5 error/warning entries, Wi-Fi state, heap, warning count |
| mem | Show free heap, total heap, minimum heap, internal heap, task count, PSRAM state |

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

`about` adds a **License** section that surfaces the proprietary notice
(`P4_CONFIG_COPYRIGHT_NOTICE`) and a **Third-party components** summary naming
the principal open-source components and their SPDX identifiers (ESP-IDF,
LVGL, esp_hosted, esp_wifi_remote, FreeRTOS, lwIP, FatFs, protobuf-c, USB host
stack). Full license texts ship in the project's `managed_components/`
directories.

### Header long-press

Pressing and holding anywhere on the top status bar shows a transient banner
with the build identity: `P4MiniShell v0.32.7 | built <date> <time> | git
<hash>`. The same identity is reported by `version`, `about`, and `sysinfo`.

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

### windows info
Show window manager layout information: display dimensions, region rectangles for
header, transcript, input row, and keyboard. Uses `windows_get_rect()` and
`windows_get_display_width()`/`windows_get_display_height()`.

### keyboard show|hide|toggle|status
Control the on-screen keyboard visibility. `hide` removes the keyboard and
expands the transcript area; `show` restores it. `toggle` switches between
visible and hidden. `status` reports current visibility, mode, height, and the
external-input state (`external=on` while a USB keyboard is attached and the
OSK is auto-hidden). Tapping the input line — or the transcript, when the OSK
is hidden and no USB keyboard is attached — summons the on-screen keyboard.

The on-screen symbols keyboard covers every printable ASCII character
(0x20-0x7E), including the shell-critical pipe `|`, caret `^` (the shell
escape character), tilde `~`, and backtick, so DOS operators and escaped
characters can be typed directly. Use the `1#` / `abc` mode buttons to switch
between text and symbols.

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

### sleep [seconds]
Enter light sleep. RAM is retained, so the shell resumes with all state
(env, aliases, cwd, variables) intact. With no argument the duration is
`P4_CONFIG_POWER_SLEEP_DEFAULT_SECS` (60 s); `sleep 0` clears the timer and
wakes only from an external source. Before sleeping, the display is blanked
and Wi-Fi/hosted state is torn down through `networking_wifi_shutdown()`.
On wake the wake cause is reported and the display is restored. Light-sleep
Wi-Fi teardown can be disabled with
`P4_CONFIG_POWER_LIGHT_SLEEP_SHUTDOWN_WIFI=0`.

Wake sources: the timer is always available; a user-wired button on
`P4_CONFIG_POWER_WAKE_GPIO` wakes via GPIO. Touch wake is not available on
this board because the GT911 interrupt line is not wired
(`BOARD_CFG_LCD_TOUCH_INT_GPIO = GPIO_NUM_NC`) — `sleep` reports this
honestly. For an always-on touch/USB-wake screen use `power idle` instead.

### deepsleep [seconds]
Enter deep sleep. RAM is lost, so on wake the device boots fresh (same path
as `reboot`). With `seconds` the chip wakes on a timer; without it, wake
requires an external wake source. Battery level is reported before sleeping.

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

### history | history /save [file] | history /load [file] | history /clear
The recall history is heap-backed (up to `P4_CONFIG_COMMAND_HISTORY_DEPTH`
commands, capped at `P4_CONFIG_HISTORY_TOTAL_BYTES`) and Up/Down arrows recall
it.

| Form | Effect |
|------|--------|
| history | List the numbered recall buffer (redirectable: `history > hist.txt`). |
| history /save [file] | Write the history to SD (default `P4_CONFIG_HISTORY_PROFILE`), atomic with partial-file cleanup. |
| history /load [file] | Restore history from SD (append, dedupe). |
| history /clear | Clear the RAM history. |

ERRORLEVEL: 0 ok / 1 missing card or file / 2 usage.

### Tab completion (USB keyboard)
Pressing **Tab** completes the current word at the end of the input line. The
first token completes command names, aliases, and `.bat` files; any later
token completes SD file/directory paths (directories get a trailing `/`). A
unique match fills in, repeated Tab cycles the matches, and the first Tab with
several matches lists them in the transcript. Command lines accept up to
`P4_CONFIG_COMMAND_BYTES` (4096) characters.

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

### rgb status | rgb off | rgb <r> <g> <b> | rgb #RRGGBB | rgb <effect> [speed] | rgb auto <on|off>
Controls the WS2812 (NeoPixel) RGB status LED on the JC1060P470 back panel,
wired to GPIO26. `components/led` owns the driver (espressif/led_strip over
RMT), a small animation task, and the auto status layer.

- `rgb status` — report driver pin, mode (auto/manual), effect, colour, speed,
  and brightness.
- `rgb off` — turn the LED off.
- `rgb <r> <g> <b>` — solid colour, each channel 0-255 (switches to manual).
- `rgb #RRGGBB` — solid colour from a hex value.
- `rgb <effect> [speed]` — `rainbow`, `breath`, `pulse`, `blink`, or `solid`;
  speed 1 (slow) .. 10 (fast).
- `rgb auto <on|off>` — enable/disable the status-driven colour layer. In auto
  mode the LED follows Wi-Fi state (amber while connecting, green when
  connected, red blink when disconnected, red pulse on watchdog timeout) and
  flashes blue when the HTTP server starts. A green flash confirms boot.

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
| path | Show current batch PATH |
| path <dir1>;<dir2>;... | Replace PATH for .bat lookup |
| echo <text> | Print text after variable expansion |
| echo on / echo off | Enable/disable batch command echoing |
| call <file.bat> [args] | Execute batch file; `%0` is the script name, `%1..%9`/`%*` are the forwarded arguments, and the caller's errorlevel becomes the script's final errorlevel |
| call <file.bat>::<routine> [args] | Call one routine from a shared library of batch routines: execution starts at `:routine` in the external file, isolated from the caller's variables, and returns on `exit /b` / `goto :eof` / EOF |
| if [not] errorlevel N cmd | Run cmd when errorlevel is at least N |
| if [not] exist <file> cmd | Run cmd when the file or directory exists |
| if [/i] [not] "a"=="b" cmd | Run cmd when the strings match; `/i` makes the comparison case-insensitive |
| if [not] a EQU\|NEQ\|LSS\|LEQ\|GTR\|GEQ b cmd | Run cmd on a numeric comparison of `a` and `b` (parsed as decimal; non-numeric reads as 0) |
| goto <label> | Jump to a `:label` in the running batch file |
| goto :eof | Jump to the end of the current batch file, unwinding its open setlocal scopes |
| for %%v in (set) do cmd | Loop over literal tokens or a wildcard pattern |
| for /f "opts" %%v in (file-set) do cmd | Loop over the lines of a file (or the active `< file`/pipe input); options: `eol=c`, `skip=n`, `delims=xyz`, `tokens=a,b,m-n,*` |
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

### set /a — integer arithmetic

Evaluates a 32-bit signed integer expression. With an assignment the result is
stored; without one it is printed. Comparison and logical operators yield `1`
when true and `0` when false, so a boolean can be stored and tested later.

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
```

Grammar: `+ - * / ^` (right-associative power), the BASIC `MOD` keyword, unary
`- +`, parentheses, `&H`/`0x` hex literals, `PI`, `RAN#[(seed)]`, string
literals (`'` or `"`) with `+` concatenation, and environment-variable
references (an undefined variable reads as 0, matching `set /a`).

Functions (BASIC names; `ASIN`/`ACOS`/`ATAN` are accepted for `ASN`/`ACS`/
`ATN`):

| Function | Meaning |
|----------|---------|
| `ABS` `SGN` | Absolute value; sign (-1/0/1) |
| `INT` `FIX` `FRAC` `ROUND` | Floor; truncate; fractional part; round to N decimals |
| `SQR` `EXP` `LN` `LOG` | Square root; e^x; natural log; base-10 log |
| `SIN` `COS` `TAN` `SINH` `COSH` `TANH` | Trig and hyperbolic trig (current angle mode) |
| `ASN`/`ASIN` `ACS`/`ACOS` `ATN`/`ATAN` | Inverse trig (result in current angle mode) |
| `FACT` `NCR` `NPR` | Factorial; combinations; permutations |
| `MOD(a,b)` | Floor-modulo, result sign follows the divisor |
| `POL` `REC` | Polar↔rectangular; stores both results in the X/Y variables |
| `DMS` `DMS$` | Decimal degrees → D.MMSS number / formatted `Dd MM' SS"` string |
| `VAL` `VALF` `STR$` `HEX$` | String→number (leading parse); number→string; integer→hex string |
| `ASC` `CHR$` `LEN` | Char→code; code→char; string length |
| `LEFT$` `MID$` `RIGHT$` | 1-based string slices |

A `NAME=<expr>` assignment stores the result (numbers as a trimmed decimal
string, strings verbatim); `calc /hex` prints an integral result as `&H` hex.
`POL`/`REC` overwrite the X and Y environment variables exactly like the
calculator's BASIC. ERRORLEVEL: 0 ok, 1 domain/syntax/store error, 2 usage.

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
- %VAR% environment variable expansion
- rem and :: comment lines
- @ line prefix to suppress echo for one line
- echo on/off flow control
- `:label` targets for `goto` and `call :label`, including the implicit `:eof` end-of-file label
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
for /f "tokens=1,* delims= " %%k in (pairs.txt) do echo KEY=%%k VAL=%%l
```

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
| `usebackq` | Accepted for DOS parity (the quoted-command form is unsupported) |

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

### pause, choice, and more without a keyboard

`pause`, `choice`, and `more` block on a real keypress delivered by the UART console,
a USB keyboard, or the on-screen keyboard. When none of those is attached the commands
fall back to their configured delay (or the first choice) and say so, so a headless
board never stalls a batch file. Every wait is also bounded by a 30-second timeout.

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
(if/or/goto/call, pipes, redirection, chaining).

All limits and filenames are configurable in p4minishell_config.h
(P4_CONFIG_BOOT_*) and documented in p4minishell_config.yaml under
oot_scripting.

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

- **Line numbers**: a right-aligned gutter shows each line's 1-based number
  (`P4_CONFIG_EDITOR_LINE_NUMBER_WIDTH_CHARS` digits, muted), and the cursor's
  current line is softly highlighted (`P4_CONFIG_EDITOR_CURRENT_LINE`).
  `Go to line` (`Ctrl+G` / `\g` / nav-page `Goto`) jumps straight to any
  numbered line.

- **Editing**: insert, backspace, Delete (forward), Enter (new line), Tab
  (spaces to the next tab stop, width `P4_CONFIG_EDITOR_TAB_WIDTH`), Home / End,
  Up / Down, PageUp / PageDown, word navigation (`Ctrl+Left` / `Ctrl+Right`),
  document start/end (`Ctrl+Home` / `Ctrl+End`), delete line (`Ctrl+Y`), and an
  Insert/overwrite toggle (`Insert` / `Ins`). Cursor movement is byte-precise
  over tabs and 8-bit characters; files round-trip unchanged (CRLF vs LF is
  preserved, and a trailing newline is only written when the original file had
  one).
- **Selection**: Shift+arrows / Ctrl+A (USB) or long-press-and-drag (touch)
  selects text; Ctrl+C / Ctrl+X / Ctrl+V copy, cut, and paste through the RAM
  clipboard. A background overlay highlights the selection, and a blinking
  block cursor marks the caret.
- **Find / Replace / Go to**: Ctrl+F (or `Find`) searches forward from the
  cursor, wrapping; F3 / Enter repeats the last search; Ctrl+H (or `Rep`)
  replaces one match at a time (Enter repeats); Ctrl+G (or `Goto`) jumps to a
  line number. The search strings are typed into the status bar and cancelled
  with Esc.
- **Undo / Redo**: Ctrl+Z / Ctrl+Shift+Z (USB) or `\u` / `\r` (serial).
- **Save / Quit**: Ctrl+S / F2 saves to the source path; Ctrl+O / `SaveAs`
  saves to a new path (unnamed buffers are prompted for a name); Esc / `\q`
  (serial) quits, with a `Y/N` confirmation whenever there are unsaved
  changes. A failed save removes the partial destination and keeps your edits
  in memory.
- **Touch keyboard**: the symbol page (reachable via `1#`) adds a `Nav`
  button that opens a navigation page (Tab, Ins, Del, arrows, Home/End,
  PgUp/PgDn, Find, Next, Rep, Goto, Undo, Redo, Save, SaveAs, Quit), and a
  second `Nav2` page adds the clipboard and advanced editing (Copy, Cut,
  Paste, SelAll, WdL/WdR word nav, DocH/DocE, DelLn, DelE). Every editor
  feature is reachable from the touch keyboard alone. Mode switching
  (abc / ABC / 1# / Nav / Nav1 / Nav2) is handled by the shell keyboard
  callback.
- **Serial console**: while the editor is open, UART lines are fed to the
  editor (`\q` quit, `\s` save, `\f` find, `\g` go-to-line, `\o` save-as,
  `\u` undo, `\r` redo, `\a` select-all; any other line is typed).
- **Syntax**: batch `.bat`/`.cmd` files are syntax-highlighted by the batch
  lexer (commands, comments, labels, `%VAR%`, strings, operators).

Editor limits are `P4_CONFIG_EDITOR_MAX_BYTES` (64 KB) and
`P4_CONFIG_EDITOR_MAX_LINES` (2048); files beyond these are refused with an
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

### Wi-Fi Behavior
- Boot-time startup in background task (does not block shell UI)
- ESP-Hosted version compatibility gate: refuses init if C6 firmware != host 2.12.x
- Recovery guidance points to coprocessor/esp32c6_slave or c6ota default
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
| bluetooth advertise on [name] | Start non-connectable BLE advertising. `name` is session-only (RAM-only, never persisted); without it the configured default name is used |
| bluetooth advertise off | Stop BLE advertising |
| bt ... | Alias for bluetooth command family |

### Bluetooth Lifecycle
- bluetooth enable initializes hosted controller and NimBLE host once
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

### USB Keyboard Auto-Detect
- Plug in a USB HID keyboard to automatically type commands into the shell
- On-screen keyboard is automatically hidden when USB keyboard is detected
- On-screen keyboard is restored when USB keyboard is unplugged
- Full US keyboard layout supported: letters, numbers, symbols, keypad, navigation keys, function keys
- Modifier keys (Shift, Ctrl, Alt, GUI) are tracked for proper character mapping
- Special keys: Enter (submit command), Backspace, ESC (clear line), Tab, arrows (cursor/history), Delete, Home, End
- Use `keyboard show` to force the on-screen keyboard visible even with USB keyboard attached
- Use `keyboard hide` to hide it again; auto-detect resumes on next plug/unplug event

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
(no printer), `LOCATE` (no transcript cursor positioning), `MODE` (covered by
`display`/`keyboard`/`power`), `PASS` (no program lock), `DEFCHR$` (LVGL
fonts, not a character LCD), `DEFSEG`/`DEFM`/`PEEK`/`POKE`/`PBLOAD`/`PBGET`
(no memory pokes; hardware access is `gpio read`/`set`), `CALC$`/`CALCJMP`
(internal calculator ROM), `RENUM` (batch has no line numbers), `CONT` (no
program suspension), `VERIFY` (FATFS write verification is not exposed),
`OPEN`/`CLOSE`/`EOF` (batch files auto-open/close; `for /f` handles end of
file), `NEW` (no in-memory program; a fresh shell session), `DSKF` (reported
by `chkdsk`).

## Unsupported Commands

These commands are registered in the dispatcher but only print an error message.
They are listed here for reference and to prevent confusion if typed.

| Command | Reason |
|---------|--------|
| rgb led <color> | No RGB LED wiring declared in board metadata; stub prints an error |
| rgb <r> <g> <b> | No RGB LED wiring declared in board metadata; stub prints an error |
| camera init | No camera stack in current workspace; stub prints an error |
| camera snap <filename> | No camera stack in current workspace; stub prints an error |

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
