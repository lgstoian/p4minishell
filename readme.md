# P4MiniShell

Embedded DOS-style command shell for the ESP32-P4 host with ESP32-C6 co-processor over ESP-Hosted SDIO.

**Version:** 0.15.0 | **Target:** ESP32-P4 + ESP32-C6 | **Display:** JD9165 1024x600 MIPI-DSI | **ESP-IDF:** v6.0.1

## Overview

P4MiniShell replaces the default LVGL demo UI with a persistent DOS-style shell surface built on LVGL 9.2.2. It provides a locked transcript UI, RAM-only shell state, SD-backed file workflows, batch-file execution, ESP-Hosted Wi-Fi and Bluetooth on the C6, USB host support, and a real OTA maintenance path for the co-processor.

## Architecture

```
main/main.c                 Shell UI, parser, transcript, command history, orchestration
p4minishell_config.h        Centralized configuration (all tunable values)
p4minishell_config.yaml     Configuration documentation (YAML)
components/ansi/            ANSI/VT escape sequence module (SGR colors, attributes, formatting)
components/display/         Display manager (rotation, resolution, refresh, brightness, power)
components/windows/         Window manager (LVGL screen layout, dynamic scaling, styling)
components/clock/           Clock manager (SNTP time sync, timezone, local/UTC formatting)
components/keyboard/        Keyboard manager (LVGL keyboard, visibility, modes)
components/shell/           Shell core (transcript, history, debug log, UART, sysinfo)
components/command/         Command dispatcher (parser, execution task, all built-ins)
components/header/          Fixed top status bar (Wi-Fi, battery, Bluetooth, USB, SD)
components/networking/      ESP-Hosted Wi-Fi + hosted NimBLE Bluetooth on C6
components/usb/             USB Host MSC storage (/usb0) + HID keyboard/mouse
components/c6ota/           ESP32-C6 firmware OTA via ESP-Hosted SDIO
coprocessor/esp32c6_slave/  ESP32-C6 hosted slave firmware project
```

### Module Responsibilities

- **Shell Layer (main/main.c)**: LVGL display/touch, transcript system, async buffer, command parser, command history, serial console bridge, worker task, SD access, file system, batch engine, output redirection, hardware controls, GPIO management, debug history
- **Display Module (components/display)**: Rotation control, resolution queries, refresh rate, backlight brightness, power management, diagnostics, thread safety
- **Window Manager (components/windows)**: Named regions (HEADER, TRANSCRIPT, INPUT_ROW, KEYBOARD), resolution-aware scaling, rotation-aware, styling
- **Header Module (components/header)**: Fixed top status bar with Wi-Fi, Bluetooth, USB, SD icons; MEM/CPU/BAT system panel; notification area
- **ANSI/VT Module (components/ansi)**: 16-color PowerShell-inspired palette, SGR parsing, format string builder, ANSI-to-plain stripping
- **Command Module (components/command)**: Command parser/dispatcher with worker task; routes to shell, hardware, module, SD/FS, batch, extended commands
- **Shell Core (components/shell)**: Transcript management, command history, debug log, UART console, system info commands
- **Networking (components/networking)**: ESP-Hosted Wi-Fi runtime, version gate, event handling, command dispatch, OTA hooks
- **Bluetooth (components/networking/bluetooth.c)**: NimBLE VHCI on C6, BLE scan, advertising, status reporting
- **USB (components/usb)**: MSC storage at /usb0, HID keyboard/mouse with auto-detect, US key map
- **C6 OTA (components/c6ota)**: Firmware update from SD or HTTP/S, image validation, progress reporting

### Key Features

- Touch-first shell UI with transcript textarea, prompt input line, on-screen keyboard, 10-command recall
- ANSI/VT color support with PowerShell-inspired 16-color palette
- Serial console bridge over UART/USB-Serial-JTAG
- Worker-task execution off LVGL event stack
- Guarded SD access with shared mount/unmount
- DOS-style file commands: cd, dir, copy, move, del, ren, mkdir, rmdir, type, write, append, touch
- RAM-only shell state: environment variables, PATH, CWD, batch arguments
- Batch file engine with %1..%9 expansion, rem comments, echo on/off
- Output redirection with > and >> to SD files
- Hosted Wi-Fi with version compatibility gate
- Hosted Bluetooth (NimBLE VHCI) for BLE scan and advertising
- USB Host MSC storage, HID keyboard/mouse
- USB keyboard auto-detect with on-screen keyboard auto-hide
- C6 OTA updates from SD or HTTP/S
- Hardware controls: brightness, rotation, battery, volume, GPIO
- Fixed header bar with real-time system panel (MEM, CPU, BAT)
- Debug history (5-entry circular buffer)

## Configuration

All tunable values live in `p4minishell_config.h`, organized by subsystem: shell identity, buffer sizes, UI layout, Wi-Fi, Bluetooth, SD card, batch engine, GPIO, header visuals, USB host, C6 OTA, and task stacks.

Three config sources:
- `p4minishell_config.h` — C-level tunable values (buffer sizes, limits, colors, stack sizes)
- `board_config.h` — Hardware pin assignments and display timing
- `sdkconfig` — ESP-IDF build configuration (Kconfig-driven)

## Hardware Baseline

| Component | Detail |
|-----------|--------|
| **Host MCU** | ESP32-P4 |
| **Co-processor** | ESP32-C6 over ESP-Hosted SDIO |
| **Display** | JD9165 1024x600 MIPI-DSI via LVGL 9.2.2 |
| **Touch** | GT911 via I2C |
| **Storage** | FATFS on SD with LFN support (255 chars) |
| **Audio** | ES8311 codec via I2S |
| **Battery** | ADC on GPIO53 with 2:1 divider (3.3V-4.2V range) |
| **Hosted SDIO** | CLK=18 CMD=19 D0=14 D1=15 D2=16 D3=17, reset GPIO54 |
| **USB Host** | MSC storage at /usb0 + HID keyboard/mouse |
| **ESP-IDF** | v6.0.1 |
| **Build target** | esp32p4 |

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

## Command Set

See [command.md](command.md) for the complete command reference. Quick overview:

| Category | Commands |
|----------|----------|
| **System** | `help`, `sysinfo`, `clear`/`cls`, `reboot`, `version`/`ver`, `about`, `debug`, `mem` |
| **Hardware** | `brightness`, `rotate`, `battery`, `volume`, `gpio list\|status\|read\|set` |
| **Storage** | `cd`/`chdir`, `dir`, `copy`, `move`, `del`/`erase`, `ren`/`rename`, `md`/`mkdir`, `rd`/`rmdir`, `type`, `write`, `append`, `touch` |
| **SD Tools** | `sd info`, `sd ls`, `sd stat`, `sd cat` |
| **Wi-Fi** | `wifi status\|scan\|diag\|connect\|disconnect` |
| **Bluetooth** | `bluetooth status\|scan\|advertise on\|off`, `bt` (alias) |
| **USB** | `usb status\|ls\|keyboard on\|off\|mouse on\|off` |
| **Batch** | `set`, `path`, `echo on\|off`, `call <file.bat>` |
| **Redirection** | `>` and `>>` to SD files |
| **OTA** | `c6ota sd:/path\|http[s]://url\|default` |

## Build and Flash

```sh
# Source ESP-IDF environment (v6.0.1)
$env:IDF_PATH = "D:\esp-idf"
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

### Runtime Constraints

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

## Build System

### CMake Structure

- Root `CMakeLists.txt`: Sets `EXTRA_COMPONENT_DIRS` for managed components, configures `board_config.h`
- `main/CMakeLists.txt`: Registers `main.c` with component dependencies
- Component `CMakeLists.txt` files: Standard `idf_component_register()` for each module

### sdkconfig Profile

- Target: `esp32p4`
- Flash: 16 MB, QIO mode
- Optimization: Performance (`-O3`)
- PSRAM: Enabled, 200 MHz, XIP from PSRAM disabled
- ESP-Hosted: SDIO host interface, 1-bit bus, reset GPIO 54
- Wi-Fi: Station-only, WPA2, nano newlib, warn-level logging
- FATFS: LFN heap-backed, 255 chars, UTF-8 encoding
- Power: Light sleep enabled (`CONFIG_PM_ENABLE`)
- Console: USB-Serial-JTAG

## Documentation

| File | Purpose |
|------|---------|
| [readme.md](readme.md) | Project overview, architecture, build instructions |
| [changelog.md](changelog.md) | Version history with categorized changes |
| [api.md](API.md) | Public module API reference |
| [command.md](command.md) | Complete command reference |
| [roadmap.md](roadmap.md) | Future parity goals and delivery phases |
| [ai-context.md](ai-context.md) | Project rules and constraints for AI-assisted development |
| [licence.md](licence.md) | Proprietary notice + third-party license summary |
| [board_config.yaml](board_config.yaml) | Hardware configuration single source of truth |
| [p4minishell_config.yaml](p4minishell_config.yaml) | Configuration documentation for all tunable values |

## License

Project-authored code: Copyright (c) 2026 Stoian Alexandru. All rights reserved.

Third-party components remain under their original licenses (Apache 2.0, MIT, BSD). See [licence.md](licence.md) for details.
