# P4MiniShell
P4MiniShell is a minimal LVGL shell for the current ESP32-P4 workspace baseline.

The app keeps the existing BSP display and touch bring-up intact and replaces the old demo screen with a shell surface made from a scrollable textarea and an on-screen keyboard.

## Current behavior
- Boot banner: P4MiniShell v0.1 ready | JC1060P470C | type help
- UI: DOS-style transcript area, prompt-bearing input line, recall buttons, and attached lv_keyboard
- Enter behavior: command execution is confirmed on the input line through LV_EVENT_READY, while transcript history remains locked above it
- Commands: help, c6update, sysinfo, wifi status, wifi scan, wifi connect, wifi disconnect, sd ls, mem, gpio status, debug, clear, reboot, version, about
- Command recall: last 10 commands via Prev/Next buttons, with the input line kept separate from transcript history
- Display/touch init: still owned by the managed BSP and board_config-generated constants
- Wi-Fi startup: attempted at runtime from sdkconfig, with default credentials available for `wifi connect` and password masking for `wifi connect <ssid> <pass>` in transcript/history
- Wi-Fi status: enabled in the checked-in sdkconfig through ESP-Hosted plus `esp_wifi_remote`, with `wifi scan` available once the runtime has started
- C6 firmware update path: `c6update <path>` mounts the SD card, opens a merged ESP32-C6 image, drives BOOT/reset through sdkconfig-backed GPIOs, and streams flash progress into the transcript while the shell stays on the same screen
- Hosted Wi-Fi now initializes on the first `wifi connect` command instead of during boot, so the shell stays usable even when the ESP32-C6 firmware is missing or unhealthy
- `wifi connect` now probes ESP-Hosted in a background task and fails faster if the C6 never brings transport up
- C6 firmware update image requirement: the updater expects a merged flash image for offset `0x0`, for example one produced by `idf.py merge-bin` or `esptool.py merge_bin`
- Wi-Fi runtime prerequisite: NVS is initialized before esp_wifi_init(), with an automatic erase-and-retry path if stored NVS metadata is incompatible
- Host Wi-Fi prerequisite: ESP-Hosted is enabled and the shell connects to an ESP32-C6 co-processor over SDIO before esp_wifi_init() runs on esp32p4
- Host Wi-Fi hardware requirement: the checked-in hosted path expects ESP32-C6 over SDIO on CLK=18 CMD=19 D0=14 D1=15 D2=16 D3=17 with reset GPIO54; if the co-processor does not answer there, the shell reports the hosted-link failure explicitly
- Hosted reset policy: the host no longer resets the ESP32-C6 on every boot; it only asserts reset if the SDIO transport cannot initialize cleanly
- Co-processor firmware upgrade path: `coprocessor/esp32c6_slave` builds the matching ESP-Hosted `2.12.1` ESP32-C6 image so the host/co-processor version check can be resolved without downgrading host features
- Build footprint: unused LVGL examples are disabled in sdkconfig so the Wi-Fi-enabled shell still links on the esp32p4 baseline
- Wi-Fi profile: sdkconfig is trimmed for station use only, with WPA2-style credential flow and Wi-Fi IRAM optimizations disabled to stay within the esp32p4 image budget
- Toolchain profile: sdkconfig now uses newlib nano formatting and warn-level compile-time logging to keep the shell build inside the esp32p4 image window
- Memory mapping: PSRAM XIP instruction/rodata mapping is disabled so host Wi-Fi does not overflow the shared flash/PSRAM mapping window during link
- Error handling: command failures now produce friendly transcript messages and are stored in a 5-entry debug history visible through `debug`
- Debugging: `debug` reports the last 5 stored errors/warnings, current Wi-Fi state, free heap, and runtime warning count

## Hardware and software baseline
- Target: esp32p4
- Display: JD9165 1024x600 via the existing BSP and esp_lcd_jd9165
- Touch: GT911 via the existing BSP and esp_lcd_touch_gt911
- LVGL port: esp_lvgl_port from managed_components
- Config source: board_config.yaml, sdkconfig, and the managed component manifests
- Current Wi-Fi config: ESP-Hosted + esp_wifi_remote are enabled in sdkconfig for an ESP32-C6 SDIO co-processor and the shell can connect using either sdkconfig defaults or runtime credentials
- Current C6 updater config source: `sdkconfig` now owns `CONFIG_P4MINISHELL_C6_FLASH_UART_PORT`, `CONFIG_P4MINISHELL_C6_FLASH_UART_TX_GPIO`, `CONFIG_P4MINISHELL_C6_FLASH_UART_RX_GPIO`, `CONFIG_P4MINISHELL_C6_EN_GPIO`, `CONFIG_P4MINISHELL_C6_RESET_GPIO`, and `CONFIG_P4MINISHELL_C6_BOOT_GPIO`
- On the checked-in ESP32-P4-Function-EV-Board baseline, bundled ESP-Hosted docs indicate initial C6 serial flashing uses the external `PROG_C6` header plus ESP-Prog; `c6update` only becomes usable when the board actually provides host-driven C6 UART/BOOT wiring or when the flow is replaced with hosted OTA
- Current board caveat: this workspace now assumes the ESP32-C6 is wired to the dedicated hosted SDIO pins 18/19/14/15/16/17 plus reset 54, not the BSP SD-card bus pins 39-44

## Build and flash
```sh
idf.py build flash monitor
```

## C6 update usage
1. Copy a merged ESP32-C6 flash image to the SD card.
2. Run `c6update`, `c6update default`, or `c6update sd:/path/to/merged-image.bin` from the shell.
3. If the board reports that no verified P4-to-C6 flash UART is wired, use the external `PROG_C6` header or a future OTA flow instead.