# P4MiniShell Technical Documentation

## Architecture Overview

P4MiniShell is a modular embedded shell application for ESP32-P4 with an ESP32-C6 co-processor. The codebase is organized into a shell orchestration layer and dedicated component modules.

### Module Layout

```
main/main.c                     Shell UI, parser, transcript, orchestration
p4minishell_config.h            Centralized configuration header
p4minishell_config.yaml         Configuration documentation (YAML)
components/display/display.c    Display manager (rotation, resolution, refresh, brightness, power)
components/windows/windows.c    Window manager (LVGL screen layout, dynamic scaling, styling)
components/header/header.c      Fixed top status bar (LVGL widgets)
components/networking/networking.c  Hosted Wi-Fi runtime (ESP-Hosted + esp_wifi_remote)
components/networking/bluetooth.c   Hosted NimBLE Bluetooth (VHCI on C6)
components/usb/usb.c            USB Host (MSC storage + HID keyboard/mouse)
components/c6ota/c6ota.c        ESP32-C6 OTA updates (ESP-Hosted SDIO)
coprocessor/esp32c6_slave/      ESP32-C6 hosted slave firmware project
```

### Configuration System

All tunable values are centralized in `p4minishell_config.h`. The header is organized
by subsystem with `P4_CONFIG_` prefixed macros. The companion `p4minishell_config.yaml`
documents every value with type, description, and valid range.

Backward-compatible `SHELL_*`, `NETWORKING_*`, `BLUETOOTH_*`, `HEADER_*`, `C6OTA_*`,
and `USB_*` aliases are defined in each source file that needs them.

Three config sources exist, each with a distinct role:
- `p4minishell_config.h` — C-level tunable values (buffer sizes, limits, colors, stack sizes)
- `board_config.h` — Hardware pin assignments and display timing (from board_config.yaml)
- `sdkconfig` — ESP-IDF build configuration (Kconfig-driven)

### Shell Layer (main/main.c)

The shell is the UI and orchestration layer. It owns:

- **LVGL display and touch**: BSP-managed JD9165 1024x600 MIPI-DSI display and GT911 touch via `display_init()` (delegated to `components/display/`)
- **Transcript system**: Scrollable textarea for command output with overflow protection and truncation markers
- **Async transcript buffer**: Thread-safe buffer for background task output, flushed via LVGL async callback
- **Command parser**: Tokenization with quote support, family dispatch, and preserved original command text
- **Command history**: 10-entry recall buffer with password masking for `wifi connect`
- **Serial console bridge**: stdin/stdout routed through same shell path as touch UI
- **Worker task**: Dedicated FreeRTOS task for command execution (prevents LVGL stack overflow)
- **SD access layer**: Guarded mount/unmount with path resolution, FATFS LFN support (255 chars)
- **File system**: RAM-only current working directory, environment variables (24 max), PATH
- **Batch engine**: `.bat` file execution with `%1`..`%9` expansion, `rem` comments, `echo on/off`
- **Output redirection**: `>` and `>>` to SD files via transcript delta copy
- **Hardware controls**: Backlight PWM, display rotation with touch remapping, battery ADC, audio codec volume — all routed through `components/display/` for display operations
- **GPIO management**: Pin table with board roles, read access for all pins, write restricted to safe pins
- **Debug history**: 5-entry circular buffer surfaced via `debug` command

### Display Module (components/display)

Central display controller owning all display hardware state and operations:

- **Rotation control**: 0/90/180/270 degree rotation with automatic GT911 touch remapping
- **Resolution queries**: Native panel resolution (1024x600) and current effective resolution (accounting for rotation)
- **Refresh rate**: Query current refresh rate (~60 Hz from panel timing); dynamic rate change API exists but is noted as not supported on JD9165 panel
- **Backlight brightness**: 0-100% PWM brightness control through BSP LEDC path
- **Power management**: Display on/sleep/off power state transitions with backlight control
- **Display diagnostics**: Comprehensive `display_info_t` struct with all timing, buffer, and config data
- **Thread safety**: State variables protected by critical sections; LVGL operations dispatched via `lv_async_call`
- **UI rebuild callback**: Registered callback invoked after rotation changes to trigger full UI rebuild
- **Touch handle**: Lazy acquisition of GT911 touch handle from BSP; cached for rotation remapping
- **Public API**: `display_init()`, `display_set_rotation()`, `display_get_rotation()`, `display_set_brightness()`, `display_get_brightness()`, `display_get_resolution()`, `display_get_info()`, `display_set_power_state()`, `display_sleep()`, `display_wake()`, `display_set_refresh_rate()`, `display_register_ui_rebuild_callback()`

### Window Manager (components/windows)

Central layout manager owning the LVGL screen region partitioning and dynamic scaling:

- **Named regions**: HEADER, TRANSCRIPT, INPUT_ROW, KEYBOARD — each with computed bounding rectangles
- **Resolution-aware scaling**: All dimensions derived from display.c's current resolution
- **Rotation-aware**: Recalculates layout on rotation change via the display manager's UI rebuild callback
- **Consistent styling**: All colors accessed through semantic names via `windows_get_color()`
- **Font management**: Centralized terminal font selection via `windows_get_terminal_font()`
- **Lifecycle**: `windows_init()` builds all UI regions; `windows_deinit()` tears down before rotation rebuild
- **Public accessors**: Individual window objects accessible via getter functions for event callback registration
- **Works with display.c**: Queries `display_get_width()`/`display_get_height()` for current resolution
- **Delegates to header.c**: Header region creation delegated to `header_init()`/`header_deinit()`

### Header Module (components/header)

Passive, display-only module that owns the fixed top bar:

- Non-scrollable LVGL flex-row container
- Resolution-scaled height (display_height / 15, clamped 32-56px)
- Status icons (left-to-right): Wi-Fi, Bluetooth, USB, SD
- System panel (far right): MEM (free heap), CPU (bar + percentage), BAT (bar + percentage)
- Battery always visible — shows "BAT N/C" when ADC is not connected
- All system panel values dynamically linked to FreeRTOS runtime statistics
- CPU usage calculated from FreeRTOS idle task runtime counter deltas
- Notification area in center for transient module events
- All public functions use LVGL async dispatch (safe from any task context)
- SD icon shows persistent state (NO/INS/ON/ERR)

### Networking Module (components/networking)

Owns ESP-Hosted Wi-Fi and bootstraps Bluetooth:

- **Wi-Fi startup**: Background task on boot, version compatibility gate against C6 firmware
- **Hosted transport**: ESP32-C6 over SDIO (CLK=18 CMD=19 D0=14 D1=15 D2=16 D3=17, reset GPIO54)
- **Version gate**: Reads C6 hosted firmware version after SDIO link up; refuses Wi-Fi init if major/minor mismatch
- **Event handling**: WIFI_EVENT and IP_EVENT handlers for connection state tracking
- **Command dispatch**: `wifi status|scan|diag|connect|disconnect` with password masking
- **OTA hooks**: `networking_wifi_wait_for_ota()`, `networking_wifi_shutdown()`, capture/restore state
- **Boot restore**: Automatic Wi-Fi restore after normal boot and after successful `c6ota`
- **Diagnostics**: Transcript-facing status + scan output via `wifi diag`

### Bluetooth Module (components/networking/bluetooth.c)

Owns hosted NimBLE Bluetooth on ESP32-C6:

- **NimBLE VHCI**: Bluetooth HCI transport over ESP-Hosted SDIO
- **Controller lifecycle**: Stateful - `bluetooth enable` initializes once, subsequent commands reuse
- **BLE scan**: Active scanning with device name resolution, bounded to 8 results
- **BLE advertising**: Non-connectable advertising with configurable on/off
- **Status reporting**: Controller readiness, NimBLE sync state, advertising state
- **Shared callbacks**: Uses same `networking_host_ops_t` as Wi-Fi module

### USB Module (components/usb)

Owns ESP-IDF USB Host Library with two class drivers:

- **MSC (Mass Storage Class)**: VFS/FATFS registration at `/usb0`, mount on demand
- **HID (Human Interface Device)**: Keyboard and mouse with opt-in transcript echo
- **Command family**: `usb status|ls|keyboard on|off|mouse on|off`
- **Bounded output**: Directory listings and file previews mirror SD command style
- **Transcript integration**: Uses dedicated host bridge functions in main.c

### C6 OTA Module (components/c6ota)

Owns the full ESP32-C6 firmware update workflow:

- **Source parsing**: `sd:/path`, `/sdcard/path`, `http[s]://url`, or `default`
- **Default resolution**: LFN-safe SD root lookup for `esp32c6_hosted_slave.bin` or `network_adapter.bin`
- **HTTP download**: Uses `esp_http_client` with Wi-Fi readiness wait
- **Image validation**: ESP-IDF app magic `0xE9` + ESP32-C6 chip ID `0x000D`
- **Factory warning**: C6 firmware `v2.3.0` requires one-time standalone tool first
- **Confirmation flow**: Exact prompt `WARNING: This will reboot the C6. Type YES to continue`
- **Transfer**: 1500-byte chunks over ESP-Hosted SDIO in Wi-Fi-off mode
- **Progress**: `C6 OTA: XX% (YYYY KB / ZZZZ KB)` every 5%
- **Wi-Fi management**: Stop before transfer, restore on failure, request post-OTA restore on success
- **Hosted transport**: Kept alive during transfer (no `esp_hosted_deinit()` to avoid assert)

## Hardware Configuration

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

## Runtime Constraints

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
