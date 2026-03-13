## Overview
P4MiniShell now boots into a simple shell UI instead of the LVGL widgets demo.

## Hardware reuse policy
- Display init remains on the existing BSP path through bsp_display_start_with_config()
- Touch init remains on the existing BSP path through the managed GT911 driver
- All active LCD and touch settings still come from board_config-generated BOARD_CFG_* macros and sdkconfig-backed BSP behavior

## UI model
- A scrollable transcript textarea shows shell history and command output
- A dedicated one-line input textarea holds the prompt and current command entry
- A bottom lv_keyboard is attached only to the input line
- Prev and Next buttons provide basic recall of the last 10 commands for touch-only use

## Built-in commands
- help: list available commands
- sysinfo: report board_config-backed display/touch/storage values, IDF version, heap, PSRAM, and Wi-Fi unsupported status
- clear: clear terminal history and redraw a fresh prompt
- reboot: print a reboot message and restart the board

## Runtime constraints
- Active target is esp32p4
- Wi-Fi is reported as unsupported on the current target/config because this workspace does not expose an enabled Wi-Fi stack
- The repository still carries a requested JC1060P470C name while the checked-in BSP baseline is ESP32-P4-Function-EV-Board
- Command submission is handled by LV_EVENT_READY on the input line, not by editing the transcript directly

## Build and flash
```sh
idf.py build flash monitor
```