# P4MiniShell
P4MiniShell is a minimal LVGL shell for the current ESP32-P4 workspace baseline.

The app keeps the existing BSP display and touch bring-up intact and replaces the old demo screen with a shell surface made from a scrollable textarea and an on-screen keyboard.

## Current behavior
- Boot banner: P4MiniShell v0.1 ready | JC1060P470C | type help
- UI: transcript area, prompt-bearing input line, recall buttons, and attached lv_keyboard
- Commands: help, sysinfo, clear, reboot
- Command recall: last 10 commands via Prev/Next buttons, with the input line kept separate from transcript history
- Display/touch init: still owned by the managed BSP and board_config-generated constants

## Hardware and software baseline
- Target: esp32p4
- Display: JD9165 1024x600 via the existing BSP and esp_lcd_jd9165
- Touch: GT911 via the existing BSP and esp_lcd_touch_gt911
- LVGL port: esp_lvgl_port from managed_components
- Config source: board_config.yaml, sdkconfig, and the managed component manifests

## Build and flash
```sh
idf.py build flash monitor
```