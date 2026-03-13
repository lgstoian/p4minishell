# P4MiniShell
P4MiniShell is a minimal LVGL shell for the current ESP32-P4 workspace baseline.

The app keeps the existing BSP display and touch bring-up intact and replaces the old demo screen with a shell surface made from a scrollable textarea and an on-screen keyboard.

## Current behavior
- Boot banner: P4MiniShell v0.1 ready | JC1060P470C | type help
- UI: transcript area, prompt-bearing input line, recall buttons, and attached lv_keyboard
- Commands: help, sysinfo, wifi status, wifi connect, wifi disconnect, clear, reboot
- Command recall: last 10 commands via Prev/Next buttons, with the input line kept separate from transcript history
- Display/touch init: still owned by the managed BSP and board_config-generated constants
- Wi-Fi startup: attempted at runtime from sdkconfig, with default credentials available for `wifi connect` and password masking for `wifi connect <ssid> <pass>` in transcript/history
- Wi-Fi runtime prerequisite: NVS is initialized before esp_wifi_init(), with an automatic erase-and-retry path if stored NVS metadata is incompatible
- Host Wi-Fi prerequisite: ESP-Hosted is enabled and the shell connects to an ESP32-C6 co-processor over SDIO before esp_wifi_init() runs on esp32p4
- Host Wi-Fi hardware requirement: the checked-in hosted path expects ESP32-C6 over SDIO on CLK=18 CMD=19 D0=14 D1=15 D2=16 D3=17 with reset GPIO54; if the co-processor does not answer there, the shell reports the hosted-link failure explicitly
- Hosted reset policy: the host no longer resets the ESP32-C6 on every boot; it only asserts reset if the SDIO transport cannot initialize cleanly
- Co-processor firmware upgrade path: `coprocessor/esp32c6_slave` builds the matching ESP-Hosted `2.12.1` ESP32-C6 image so the host/co-processor version check can be resolved without downgrading host features
- Build footprint: unused LVGL examples are disabled in sdkconfig so the Wi-Fi-enabled shell still links on the esp32p4 baseline
- Wi-Fi profile: sdkconfig is trimmed for station use only, with WPA2-style credential flow and Wi-Fi IRAM optimizations disabled to stay within the esp32p4 image budget
- Toolchain profile: sdkconfig now uses newlib nano formatting and warn-level compile-time logging to keep the shell build inside the esp32p4 image window
- Memory mapping: PSRAM XIP instruction/rodata mapping is disabled so host Wi-Fi does not overflow the shared flash/PSRAM mapping window during link

## Hardware and software baseline
- Target: esp32p4
- Display: JD9165 1024x600 via the existing BSP and esp_lcd_jd9165
- Touch: GT911 via the existing BSP and esp_lcd_touch_gt911
- LVGL port: esp_lvgl_port from managed_components
- Config source: board_config.yaml, sdkconfig, and the managed component manifests
- Current Wi-Fi config: ESP-Hosted + esp_wifi_remote are enabled in sdkconfig for an ESP32-C6 SDIO co-processor and the shell can connect using either sdkconfig defaults or runtime credentials
- Current board caveat: this workspace now assumes the ESP32-C6 is wired to the dedicated hosted SDIO pins 18/19/14/15/16/17 plus reset 54, not the BSP SD-card bus pins 39-44

## Build and flash
```sh
idf.py build flash monitor
```