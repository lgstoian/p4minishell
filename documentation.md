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
- sysinfo: report board_config-backed display/touch/storage values, IDF version, heap, PSRAM, and current Wi-Fi runtime state
- wifi status: show Wi-Fi runtime state, target SSID, AP info, and IP info when available
- wifi connect: connect using sdkconfig default credentials
- wifi connect <ssid> <pass>: connect using runtime credentials without writing the password into transcript history or recall history
- wifi disconnect: disconnect the current station session
- clear: clear terminal history and redraw a fresh prompt
- reboot: print a reboot message and restart the board

## Runtime constraints
- Active target is esp32p4
- Wi-Fi startup now follows sdkconfig at runtime and the checked-in workspace enables the host Wi-Fi path for the esp32p4 board baseline
- Wi-Fi runtime now initializes NVS first and falls back to erase-and-retry when the stored NVS layout is incompatible, because esp_wifi_init() depends on NVS being ready on this configuration
- On the esp32p4 host Wi-Fi path, ESP-Hosted now connects to an ESP32-C6 co-processor over SDIO before esp_wifi_init() runs so the standard `esp_wifi_*` shell code can stay unchanged via `esp_wifi_remote`
- The checked-in hosted transport uses CLK=18 CMD=19 D0=14 D1=15 D2=16 D3=17 and reset GPIO54; if the ESP32-C6 path is absent or miswired, the shell reports the exact hosted-link failure instead of the old extconn hardware note
- ESP-Hosted reset policy is now `SLAVE_RESET_ONLY_IF_NECESSARY`, which avoids the boot-time `Reset slave using GPIO[54]` warning during healthy starts while still allowing recovery if transport init fails
- `coprocessor/esp32c6_slave` is the repo-local co-processor project for building a version-matched ESP32-C6 ESP-Hosted image from the same `2.12.1` source line locked into the host build
- Default Wi-Fi credentials are stored in sdkconfig for local validation, while runtime `wifi connect <ssid> <pass>` masks the password in transcript output and keeps it out of command recall
- LVGL example compilation is disabled in sdkconfig because the shell app no longer uses those sources and the Wi-Fi-enabled image otherwise overruns the esp32p4 link region
- The checked-in Wi-Fi profile is intentionally station-only for the shell use case, with WPA3, enterprise auth, SoftAP, and Wi-Fi IRAM optimizations disabled to reduce linker pressure on esp32p4
- Additional size controls now active in sdkconfig are warn-level compile-time logging, newlib nano formatting, and disabled AMPDU aggregation because the shell does not need peak Wi-Fi throughput
- PSRAM XIP instruction and rodata mapping are also disabled in sdkconfig because the host Wi-Fi image exceeded the shared flash/PSRAM mapping window during link with those options enabled
- The repository still carries a requested JC1060P470C name while the checked-in BSP baseline is ESP32-P4-Function-EV-Board
- Command submission is handled by LV_EVENT_READY on the input line, not by editing the transcript directly

## Build and flash
```sh
idf.py build flash monitor
```