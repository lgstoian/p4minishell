## Overview
P4MiniShell now boots into a simple shell UI instead of the LVGL widgets demo.

## Hardware reuse policy
- Display init remains on the existing BSP path through bsp_display_start_with_config()
- Touch init remains on the existing BSP path through the managed GT911 driver
- All active LCD and touch settings still come from board_config-generated BOARD_CFG_* macros and sdkconfig-backed BSP behavior

## UI model
- A scrollable textarea acts as the visible shell history and current input surface
- A bottom lv_keyboard is attached directly to that textarea
- The shell seeds the display with the boot message and then appends a prompt for command entry

## Built-in commands
- help: list available commands
- sysinfo: report board_config-backed display/touch/storage values, IDF version, heap, PSRAM, and Wi-Fi unsupported status
- clear: clear terminal history and redraw a fresh prompt
- reboot: print a reboot message and restart the board

## Runtime constraints
- Active target is esp32p4
- Wi-Fi is reported as unsupported on the current target/config because this workspace does not expose an enabled Wi-Fi stack
- The repository still carries a requested JC1060P470C name while the checked-in BSP baseline is ESP32-P4-Function-EV-Board

## Build and flash
```sh
idf.py build flash monitor
```