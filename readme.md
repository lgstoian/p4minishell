# P4MiniShell

Embedded DOS-style command shell for the ESP32-P4 host with ESP32-C6 co-processor over ESP-Hosted SDIO.

**Version:** 0.24.18 | **Target:** ESP32-P4 + ESP32-C6 | **Display:** JD9165 1024x600 MIPI-DSI

## Overview

P4MiniShell replaces the default LVGL demo UI with a persistent DOS-style shell surface built on LVGL 9.4.0. It provides a locked transcript UI, RAM-only shell state, SD-backed file workflows, batch-file execution, ESP-Hosted Wi-Fi and Bluetooth on the C6, USB host support, and a real OTA maintenance path for the co-processor.

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

## Architecture

```
main/main.c                 App entry point, LVGL event callbacks, UI construction, host bridges
p4minishell_config.h        Centralized configuration (all tunable values)
p4minishell_config.yaml     Configuration documentation (YAML source of truth)
components/ansi/            ANSI/VT escape sequence module (SGR colors, attributes, formatting) + semantic palette
components/display/         Display manager (rotation, resolution, refresh, brightness, power)
components/windows/         Window manager (LVGL screen layout, dynamic scaling, styling)
components/clock/           Clock manager + clock commands (SNTP time sync, timezone, local/UTC formatting, date/time/timezone/sntp)
components/keyboard/        Keyboard manager (LVGL keyboard, visibility, modes)
components/shell/           Shell core (transcript, history, debug log, UART console, input line, sysinfo)
components/storage/         SD sessions, path resolution, FATFS conversion, cwd, DOS file commands
components/batch/           Batch engine, labels, for loops, pipes, environment variables, PATH
components/command/         Command module (parser, dispatcher, worker task, execution pipeline, hardware and system commands)
components/header/          Fixed top status bar (Wi-Fi, battery, Bluetooth, USB, SD)
components/networking/      Sole owner of ESP-Hosted + esp_wifi_remote: Wi-Fi station lifecycle, hosted NimBLE, status accessors, known-network storage (wifi_known.c)
components/usb/             USB Host MSC storage (/usb0) + HID keyboard/mouse
components/c6ota/           ESP32-C6 firmware OTA via ESP-Hosted SDIO
coprocessor/esp32c6_slave/  ESP32-C6 hosted slave firmware project
```

### Module Ownership

Dependencies flow one way: `command` → `batch` → `storage` → `shell` → (`ansi`, `display`, `windows`, `header`, `keyboard`, `clock`).

| Module | Owns |
|--------|------|
| `main/main.c` | `app_main()`, boot sequencing, LVGL event callbacks, UI construction, c6ota/usb host bridges |
| `components/shell/` | Transcript + async buffer, command history, debug log, UART console, input line prompt contract, the interactive keypress queue, the DOS prompt template engine, system info commands (`help`, `sysinfo`, `version`, `about`, `mem`, `debug`) |
| `components/storage/` | Guarded SD sessions, persistent mount tracking, path resolution, FATFS conversion, size formatting, DOS wildcard matching, current working directory, volume capacity queries and write guardrails, input/output redirection plumbing, and every DOS file, text, and volume command |
| `components/batch/` | Batch file execution, `:label` scanning, `goto`, `call :label`, `for` loops, multi-stage `\|` pipes, environment variables, PATH, variable expansion, errorlevel, `setlocal`/`endlocal` scoping, and the batch language commands |
| `components/command/` | Command dispatch and worker task, the execution pipeline, output redirection parsing, hardware commands, UI query commands, system commands, hardware telemetry |

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
| **Display** | JD9165 1024x600 MIPI-DSI via LVGL 9.4.0 |
| **Touch** | GT911 via I2C |
| **Storage** | FATFS on SD with LFN support (255 chars) |
| **Audio** | ES8311 codec via I2S |
| **Battery** | ADC on GPIO53 with 2:1 divider (3.3V-4.2V range) |
| **Hosted SDIO** | CLK=18 CMD=19 D0=14 D1=15 D2=16 D3=17, reset GPIO54 |
| **USB Host** | MSC storage at /usb0 + HID keyboard/mouse |
| **ESP-IDF** | v5.5.5 |
| **Build target** | esp32p4 |

## Key Features

- **Touch-first shell UI**: Transcript span group, prompt input line, on-screen keyboard, 10-command recall. The symbols keyboard covers every printable ASCII character, including the shell-critical pipe `|`, caret `^`, tilde `~`, and backtick.
- **Scrollable transcript**: A scrollable transcript that holds a multi-screenful history and always jumps to the output of the command you just submitted. Scroll with the input-row `Up`/`Dn` buttons, touch drag, USB keyboard `PageUp`/`PageDown`, or the USB mouse wheel.
- **ANSI/VT color support**: PowerShell-inspired 16-color palette with SGR escape sequences (ESC[...m) for colored command output on both LVGL transcript (per-span colours) and UART console
- **Serial console bridge**: `idf.py monitor` acts as interactive shell endpoint over UART/USB-Serial-JTAG
- **Worker-task execution**: Heavy commands run off LVGL event stack to prevent overflow
- **Guarded SD access**: All SD operations use shared mount/unmount with validation and bounded output
- **DOS-style file commands**: `cd`, `dir`, `copy`, `move`, `del`, `ren`, `mkdir`, `rmdir`, `type`, `write`, `append`, `touch`
- **Full `dir` option set**: `/W` wide, `/P` paged, `/S` recursive, `/B` bare, `/L` lowercase, `/A` attribute filter, `/O` sort order
- **Volume management**: `chkdsk`/`scandisk` capacity and integrity report, `format` behind an exact confirmation word
- **diskpart-style disk tools**: `disk list`/`detail`/`clean`/`create partition primary`/`delete partition`/`format` for MBR partition-table management and FORMAT.COM-style formatting with `/FS:` `/A:` `/V:` `/Q`
- **DOS-style boot scripting**: `CONFIG.SYS` directives (SET, PATH, PROMPT, ECHO, ROTATE, BRIGHTNESS, VOLUME, WIFI_*, BLUETOOTH, USB_*, GPIO) and `AUTOEXEC.BAT` execution at boot, with default-file generation
- **Storage guardrails**: free-space prechecks, self-copy protection, partial-destination cleanup, copy progress
- **RAM-only shell state**: Environment variables, PATH, current working directory, batch arguments
- **Batch file engine**: `.bat` execution with `%0`/`%1`..`%9`/`%*` expansion, `:label` targets, `goto` (including the implicit `:eof` label), `call` (with argument forwarding and errorlevel propagation), `for %%var in (set) do ...` loops (literal tokens or a wildcard pattern), `rem` comments, `echo on/off`
- **Real batch control flow**: `pause` and `choice` block on an actual keypress, `setlocal`/`endlocal` scope the environment, `exit /b` leaves one batch file, `if` with `errorlevel N` (≥), `exist <path>`, case-insensitive `/i` string tests, and `not`
- **Batch expressions**: `set /a` integer arithmetic with the full DOS operator set plus comparison (`== != < > <= >=`) and logical (`&& ||`) operators that yield 1/0, `set /p` prompted input, trailing `^` line continuation
- **DOS prompt engine**: `prompt` template with `$p $g $t $d $v $n` and more, driving both the UART console and the on-screen input line
- **Text utilities**: `find` (`/I /N /C /V`), `more` (keypress paging), `tree` (recursive, `/F /A`), `fc`, `sort` (`/R /I /U`)
- **Redirection**: `>`, `>>`, and `<` in any order on one line
- **Multi-stage pipes**: `cmd1 | cmd2 | cmd3` with quote-aware splitting
- **Command chaining**: `a & b` (both), `a && b` (on success), `a || b` (on failure)
- **DOS quoting and escaping**: `"text"` groups with expansion, `'text'` groups literally, `^c` escapes any character
- **Hosted Wi-Fi**: ESP-Hosted + esp_wifi_remote on C6 with version compatibility gate. Station-only, enforced in code and by compiling SoftAP out. Every Hosted and wifi_remote call is confined to `components/networking/`. `wifi status` reports SSID/BSSID/channel/RSSI/PHY/IP/DNS/uptime, `wifi scan` is RSSI-sorted with a bare `/b` form, and classic `ping` + `dns`/`nslookup` connectivity commands set ERRORLEVEL for batch use.
- **Known Wi-Fi networks**: A persistent list of previously-used networks lives on the SD card (`sd:/WIFI.KNOWN`, hand-editable plain text). On boot with `WIFI_AUTOCONNECT=ON`, the firmware scans and connects to the best known network in range (preferred / highest priority / strongest RSSI), falling back to the classic single-credential path when the SD card is absent. Manage it with `wifi known`, `wifi save`, `wifi forget`, and `wifi preferred` — passwords are never printed.
- **Basic HTTPS**: `httpget <url> [localfile]` (alias `wget`) performs a simple HTTPS/HTTP GET over the same `esp_http_client` stack c6ota uses, printing the body or saving it to SD with free-space guardrails, setting ERRORLEVEL, and supporting redirection/pipes. All HTTP/TLS code lives in `components/networking/`.
- **Hosted Bluetooth**: NimBLE VHCI on C6 for BLE scan and advertising, with a sorted bounded scan report and session-scoped `advertise on [name]`
- **USB Host**: MSC mass storage at `/usb0`, HID keyboard/mouse with opt-in echo
- **USB Keyboard Auto-Detect**: Plug in a USB keyboard to type commands; on-screen keyboard hides automatically. Full US keyboard layout supported including symbols, keypad, navigation keys, and function keys.
- **C6 OTA updates**: Validated firmware updates from SD or HTTP/S over ESP-Hosted SDIO
- **Hardware controls**: Brightness, rotation, battery telemetry, volume, GPIO inspection
- **Time / SNTP control**: `date`, `time`, `timezone`, and `sntp`/`ntpsync` commands (all owned by `components/clock/`). `sntp sync` synchronizes the clock over Wi-Fi, `timezone <TZ>` sets a POSIX timezone string, and `date`/`time` show a fuller clock panel (local/UTC/unix/timezone/uptime/sync) while still supporting the DOS-style set forms.
- **Fixed header bar**: Wi-Fi, battery, Bluetooth, USB, SD status with transient notifications
- **Real-time system panel**: Memory (MEM), CPU usage (CPU bar + %), and Battery (BAT) all dynamically linked to FreeRTOS runtime statistics on the far right of the header
- **FreeRTOS task introspection**: `ps` / `tasks` / `top` list every task (name, state, priority, core, stack high-water mark) with per-task CPU% since the last sample; `/b` emits machine-parsable rows for pipes. Read-only.
- **Debug history**: 5-entry error/warning buffer surfaced via `debug` command

## Command Set

See [command.md](command.md) for the complete command reference. Quick overview:

| Category | Commands |
|----------|----------|
| **System** | `help`, `sysinfo`, `clear`/`cls`, `reboot`, `version`/`ver`, `about`, `debug`, `mem`, `ps`/`tasks`/`top`, `screenshot`/`scr`/`capture` |
| **Hardware** | `brightness`, `rotate`, `battery`, `volume`, `gpio list|status|read|set` |
| **Storage** | `cd`/`chdir`, `dir`, `copy`, `move`, `del`/`erase`, `ren`/`rename`, `md`/`mkdir`, `rd`/`rmdir`, `type`, `write`, `append`, `touch` |
| **Volume** | `chkdsk`/`scandisk`, `format`, `label`, `attrib`, `xcopy` |
| **Disk / partitions** | `disk list`, `disk detail`, `disk clean`, `disk create partition primary [size=N]`, `disk delete partition N`, `disk format` |
| **SD Tools** | `sd info`, `sd ls`, `sd stat`, `sd cat` |
| **Wi-Fi** | `wifi status|scan [/b]|diag|connect|disconnect`, `wifi known|save|forget|clear known|preferred` |
| **Connectivity** | `ping <host-or-ip> [count]`, `dns <hostname>` (alias `nslookup`), `httpget <url> [localfile]` (alias `wget`) |
| **Bluetooth** | `bluetooth status|scan [limit]|advertise <on [name]|off>`, `bt` (alias) |
| **USB** | `usb status|ls|keyboard on|off|mouse on|off` |
| **Batch** | `set`, `set /a`, `set /p`, `path`, `echo on|off`, `call`, `if`, `goto`, `shift`, `pause`, `choice`, `setlocal`, `endlocal`, `exit [/b]` |
| **Text tools** | `find`, `more`, `tree`, `fc`, `sort`, `prompt` |
| **Time / SNTP** | `date` `[MM-DD-YYYY]`, `time` `[HH:MM[:SS]]`, `timezone` `[TZ]`, `sntp`/`ntpsync` `[sync]` |
| **Redirection** | `>`, `>>`, and `<` to and from SD files |
| **Pipes** | `cmd1 | cmd2 | cmd3` (up to 4 stages) |
| **Chaining** | `a & b`, `a && b`, `a || b` (up to 8 commands) |
| **Quoting** | `"grouped"`, `'literal'`, `^` escapes |
| **OTA** | `c6ota sd:/path|http[s]://url|default` |

## Build and Flash

```sh
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
| [roadmap.md](roadmap.md) | Future parity goals and delivery phases |
| [licence.md](licence.md) | Proprietary notice + third-party license summary |
| [API.md](API.md) | Public module API reference |
| [SDK.md](SDK.md) | Module integration guide with examples |
| [board_config.yaml](board_config.yaml) | Hardware configuration single source of truth |

## License

Project-authored code: Copyright (c) 2026 Stoian Alexandru. All rights reserved.

Third-party components remain under their original licenses (Apache 2.0, MIT, BSD). See [licence.md](licence.md) for details.
