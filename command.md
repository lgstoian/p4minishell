# P4MiniShell Command Reference

Complete reference for all shell commands available in P4MiniShell.

## ANSI/VT Color Support

All command output uses ANSI SGR escape sequences (ESC[...m) for colored text rendering.
The color palette is PowerShell-inspired with green base text on black background.

### Color Scheme
| Context | Color | ANSI Code |
|---------|-------|-----------|
| Headers/titles | Bright Green | `\e[92m` |
| Field labels | Cyan | `\e[36m` |
| Success values | Green | `\e[32m` |
| Errors | Red | `\e[31m` |
| Warnings | Yellow | `\e[33m` |
| Muted/secondary | Bright Black (Gray) | `\e[90m` |
| Default text | Bright Green (base) | `\e[92m` |
| Reset | Default | `\e[0m` |

### ANSI Format Specifiers (for shell_transcript_appendf_ansi)
| Specifier | Meaning |
|-----------|---------|
| `@R` | Reset all attributes |
| `@B` | Bold on |
| `@g` | Foreground green |
| `@G` | Foreground bright green |
| `@r` | Foreground red |
| `@y` | Foreground yellow |
| `@c` | Foreground cyan |
| `@C` | Foreground bright cyan |
| `@k` | Foreground black (gray) |
| `@w` | Foreground white |
| `@W` | Foreground bright white |
| `@m` | Foreground magenta |
| `@M` | Foreground bright magenta |
| `@b` | Foreground blue |
| `@Y` | Foreground bright yellow |

## UI Model

- Fixed top header bar with status icons (Wi-Fi, Bluetooth, USB, SD) and system panel (MEM, CPU, BAT) dynamically linked to FreeRTOS
- Scrollable transcript textarea for command output (read-only)
- Single-line input textarea with prompt for command entry
- On-screen LVGL keyboard attached to input line
- Prev/Next buttons for 10-command recall history
- Serial console: idf.py monitor accepts same commands via UART/USB-Serial-JTAG
- Command submission: LV_EVENT_READY on input line (touch) or Enter (serial)
- Heavy commands run on dedicated worker task (not LVGL input callback stack)

## System Commands

| Command | Description |
|---------|-------------|
| help | Show built-in command list |
| sysinfo | Show board, display, storage, heap, FreeRTOS tasks, uptime, Wi-Fi, and OTA state |
| clear / cls | Clear transcript history and redraw prompt |
| reboot | Restart the board |
| version / ver | Show app banner, ESP-IDF version, chip info, uptime, heap, and task count |
| about | Show shell and board summary with header description, uptime, and task count |
| debug | Show last 5 error/warning entries, Wi-Fi state, heap, warning count |
| mem | Show free heap, total heap, minimum heap, internal heap, task count, PSRAM state |

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
visible and hidden. `status` reports current visibility, mode, and height.

### battery
Read battery ADC pin (GPIO53, 2:1 divider), show scaled voltage, estimated percentage (3.3V-4.2V range), raw ADC data, and light-sleep state.

### battery sleep <on|off|status>
Request or inspect light sleep. Only available when CONFIG_PM_ENABLE is enabled in sdkconfig.

### volume <0-100>
Set speaker volume through ES8311 codec path.

### gpio list
Show exposed board GPIO table with pin numbers, current levels, write policy, and role descriptions.

### gpio status
Show current levels and role text for all exposed board pins.

### gpio read <pin>
Read current logic level from any GPIO number.

### gpio set <pin> <0|1>
Drive a GPIO output. Only allowed for pins marked safe for writes in the shell pin table.

## DOS-Style File Commands

All file commands operate on SD card through guarded mount/unmount. Working directory and environment are RAM-only (not persisted across boots).

| Command | Description |
|---------|-------------|
| cd / chdir | Show current working directory |
| cd <path> | Change working directory (sd:/, /sdcard/, relative, ., ..) |
| dir [path] | List directory entries with bounded output |
| copy <src> <dst> | Copy file on SD |
| move <src> <dst> | Move or rename file/directory |
| del / erase <path> | Delete file from SD |
| ren / rename <src> <dst> | Rename file or directory |
| md / mkdir <path> | Create directory |
| rd / rmdir <path> | Remove empty directory |
| type <path> | Print text-safe file preview (no raw binary) |
| write <path> <text> | Create or overwrite text file |
| append <path> <text> | Append text to file |
| touch <path> | Create empty file or refresh timestamp |

## Environment and Batch Commands

| Command | Description |
|---------|-------------|
| set | List all RAM-only environment variables |
| set NAME=VALUE | Create or update environment variable |
| path | Show current batch PATH |
| path <dir1>;<dir2>;... | Replace PATH for .bat lookup |
| echo <text> | Print text after variable expansion |
| echo on / echo off | Enable/disable batch command echoing |
| call <file.bat> [args] | Execute batch file with %1..%9 expansion |

### Batch File Features
- %1 through %9 argument expansion
- rem and :: comment lines
- echo on/off flow control
- PATH-based .bat lookup
- Nested calls up to 4 levels deep

### Output Redirection
- > file: Write command transcript output to SD file (overwrite)
- >> file: Append command transcript output to SD file

## Wi-Fi Commands

| Command | Description |
|---------|-------------|
| wifi status | Show Wi-Fi runtime state, target SSID, AP info, IP info |
| wifi scan | Scan for nearby access points (SSID, RSSI, channel, auth mode) |
| wifi diag | Run full diagnostic: connection state, IP status, network scan |
| wifi connect | Connect using sdkconfig default credentials |
| wifi connect <ssid> <pass> | Connect with runtime credentials (password masked) |
| wifi disconnect | Disconnect current station session |

### Wi-Fi Behavior
- Boot-time startup in background task (does not block shell UI)
- ESP-Hosted version compatibility gate: refuses init if C6 firmware != host 2.12.x
- Recovery guidance points to coprocessor/esp32c6_slave or c6ota default
- Restores automatically after successful c6ota
- Transcript-facing diagnostics on boot and post-OTA restore

## Bluetooth Commands

| Command | Description |
|---------|-------------|
| bluetooth status | Show hosted Bluetooth readiness, NimBLE state, advertising state |
| bluetooth scan | BLE scan through hosted NimBLE on C6 (up to 8 devices) |
| bluetooth advertise on | Start non-connectable BLE advertising |
| bluetooth advertise off | Stop BLE advertising |
| bt ... | Alias for bluetooth command family |

### Bluetooth Lifecycle
- bluetooth enable initializes hosted controller and NimBLE host once
- Subsequent scan/advertise commands reuse active session
- Hosted NimBLE VHCI on ESP32-C6 over ESP-Hosted SDIO

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
| sd info | Mount SD, report card metadata and root availability |
| sd ls [path] | List directory with full long filenames, entry types, sizes |
| sd stat <path> | Show resolved path, type, size, mode for file/directory |
| sd cat <path> [max_bytes] | Text-safe file preview (1-8192 bytes, non-printable sanitized) |

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

## Unsupported Commands

| Command | Reason |
|---------|--------|
| rgb led <color> | No RGB LED wiring declared in board metadata |
| rgb <r> <g> <b> | No RGB LED wiring declared in board metadata |
| camera init | No camera stack in current workspace |
| camera snap <filename> | No camera stack in current workspace |

## Runtime Notes

- sd ls shows full long filenames (FATFS LFN, 255-char limit, UTF-8)
- Directory listings bounded to 128 entries
- sd cat preview bounded to 8192 bytes
- SD VO4 LDO acquired at 3300 mV before mounts (no ldo warning spam)
- Commands run on worker task (prevents LVGL stack overflow)
- Family commands (wifi, sd, c6ota) receive full unsplit command text
- Serial prompt stateful: no P4Shell> spam during idle polling
- Password masking in transcript and command recall history
