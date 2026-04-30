# P4MiniShell

Embedded DOS-style command shell for the ESP32-P4 host with ESP32-C6 co-processor over ESP-Hosted SDIO.

## Overview

P4MiniShell replaces the default LVGL demo UI with a persistent DOS-style shell surface built on LVGL 9.2.2. It provides a locked transcript UI, RAM-only shell state, SD-backed file workflows, batch-file execution, ESP-Hosted Wi-Fi and Bluetooth on the C6, USB host support, and a real OTA maintenance path for the co-processor.

The current firmware is not a desktop DOS clone and is not yet an MS-DOS-compatible runtime. It provides the embedded foundation for that direction.

## Architecture

```
main/main.c                 Shell UI, parser, transcript, command history, orchestration
p4minishell_config.h        Centralized configuration (all tunable values)
p4minishell_config.yaml     Configuration documentation (YAML source of truth)
components/display/         Display manager (rotation, resolution, refresh, brightness, power)
components/windows/         Window manager (LVGL screen layout, dynamic scaling, styling)
components/header/          Fixed top status bar (Wi-Fi, battery, Bluetooth, USB, SD)
components/networking/      ESP-Hosted Wi-Fi + hosted NimBLE Bluetooth on C6
components/usb/             USB Host MSC storage (/usb0) + HID keyboard/mouse
components/c6ota/           ESP32-C6 firmware OTA via ESP-Hosted SDIO
coprocessor/esp32c6_slave/  ESP32-C6 hosted slave firmware project
```

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
| **Display** | JD9165 1024x600 MIPI-DSI via LVGL 9.2.2 |
| **Touch** | GT911 via I2C |
| **Storage** | FATFS on SD with LFN support (255 chars) |
| **Audio** | ES8311 codec via I2S |
| **Battery** | ADC on GPIO53 with 2:1 divider (3.3V-4.2V range) |
| **Hosted SDIO** | CLK=18 CMD=19 D0=14 D1=15 D2=16 D3=17, reset GPIO54 |
| **USB Host** | MSC storage at /usb0 + HID keyboard/mouse |
| **ESP-IDF** | v5.5.3 |
| **Build target** | esp32p4 |

## Key Features

- **Touch-first shell UI**: Transcript textarea, prompt input line, on-screen keyboard, 10-command recall
- **Serial console bridge**: `idf.py monitor` acts as interactive shell endpoint over UART/USB-Serial-JTAG
- **Worker-task execution**: Heavy commands run off LVGL event stack to prevent overflow
- **Guarded SD access**: All SD operations use shared mount/unmount with validation and bounded output
- **DOS-style file commands**: `cd`, `dir`, `copy`, `move`, `del`, `ren`, `mkdir`, `rmdir`, `type`, `write`, `append`, `touch`
- **RAM-only shell state**: Environment variables, PATH, current working directory, batch arguments
- **Batch file engine**: `.bat` execution with `%1`..`%9` expansion, `rem` comments, `echo on/off`
- **Output redirection**: `>` and `>>` to SD files
- **Hosted Wi-Fi**: ESP-Hosted + esp_wifi_remote on C6 with version compatibility gate
- **Hosted Bluetooth**: NimBLE VHCI on C6 for BLE scan and advertising
- **USB Host**: MSC mass storage at `/usb0`, HID keyboard/mouse with opt-in echo
- **C6 OTA updates**: Validated firmware updates from SD or HTTP/S over ESP-Hosted SDIO
- **Hardware controls**: Brightness, rotation, battery telemetry, volume, GPIO inspection
- **Fixed header bar**: Wi-Fi, battery, Bluetooth, USB, SD status with transient notifications
- **Real-time system panel**: Memory (MEM), CPU usage (CPU bar + %), and Battery (BAT) all dynamically linked to FreeRTOS runtime statistics on the far right of the header
- **Debug history**: 5-entry error/warning buffer surfaced via `debug` command

## Command Set

See [command.md](command.md) for the complete command reference. Quick overview:

| Category | Commands |
|----------|----------|
| **System** | `help`, `sysinfo`, `clear`/`cls`, `reboot`, `version`/`ver`, `about`, `debug`, `mem` |
| **Hardware** | `brightness`, `rotate`, `battery`, `volume`, `gpio list|status|read|set` |
| **Storage** | `cd`/`chdir`, `dir`, `copy`, `move`, `del`/`erase`, `ren`/`rename`, `md`/`mkdir`, `rd`/`rmdir`, `type`, `write`, `append`, `touch` |
| **SD Tools** | `sd info`, `sd ls`, `sd stat`, `sd cat` |
| **Wi-Fi** | `wifi status|scan|diag|connect|disconnect` |
| **Bluetooth** | `bluetooth status|scan|advertise on|off`, `bt` (alias) |
| **USB** | `usb status|ls|keyboard on|off|mouse on|off` |
| **Batch** | `set`, `path`, `echo on|off`, `call <file.bat>` |
| **Redirection** | `>` and `>>` to SD files |
| **OTA** | `c6ota sd:/path|http[s]://url|default` |

## Build and Flash

```sh
# Source ESP-IDF environment
$env:IDF_PATH = "C:\esp\v5.5.3\esp-idf"
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
