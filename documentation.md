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
- Enter/OK on the input line is handled by `LV_EVENT_READY`, which is the confirmed command execution path in the current shell
- The visual style intentionally stays close to a compact DOS/MS-DOS terminal: dense text, immutable history pane, prompt line, and scan-friendly output

## Built-in commands
- help: list available commands
- c6update <path>: mount the SD card, open a merged ESP32-C6 flash image, enter ROM download mode through the configured BOOT/reset GPIOs, and flash the image over the configured UART while reporting progress to the transcript
- Wi-Fi/ESP-Hosted startup is lazy: the UI boots first, and hosted transport bring-up happens on demand from `wifi connect`, which prevents a dead or blank C6 from forcing a host reboot loop
- On-demand Wi-Fi startup now runs in a worker task and uses a shorter hosted transport retry budget, so a missing C6 fails visibly without tying up the shell for a long SDIO retry sequence
- sysinfo: report board_config-backed display/touch/storage values, IDF version, heap, PSRAM, and current Wi-Fi runtime state
- wifi status: show Wi-Fi runtime state, target SSID, AP info, and IP info when available
- wifi scan: only runs after Wi-Fi startup succeeds, then lists SSIDs, RSSI, auth mode, and channel
- wifi connect: connect using sdkconfig default credentials
- wifi connect <ssid> <pass>: connect using runtime credentials without writing the password into transcript history or recall history
- wifi disconnect: disconnect the current station session
- sd ls [path]: mount the SD card on demand, list directory entries, and report a friendly insert-and-retry message when no card is present
- mem: report current free heap, minimum heap, internal heap, and PSRAM usage
- gpio status: report key GPIO levels for display and C6-related pins
- debug: show the last 5 stored error/warning entries plus Wi-Fi state, heap, and warning count
- clear: clear terminal history and redraw a fresh prompt
- reboot: print a reboot message and restart the board
- version/about: report app, board, and ESP-IDF identity details

## Runtime constraints
- Active target is esp32p4
- The C6 updater uses `esp-serial-flasher` on a dedicated UART and expects a merged ESP32-C6 image whose flash base is `0x0`
- The C6 updater reads its wiring from sdkconfig: UART port, UART TX/RX, EN, reset, and BOOT GPIOs all live under the `P4MiniShell` Kconfig menu
- On the current ESP32-P4-Function-EV-Board baseline, upstream ESP-Hosted documentation routes initial C6 serial flashing through the external `PROG_C6` header with an ESP-Prog; there is no verified on-board P4-controlled flash UART in this workspace baseline, so `c6update` will report that constraint until custom host-to-C6 UART/BOOT wiring is provided
- If `CONFIG_P4MINISHELL_C6_RESET_GPIO` is unset, the updater falls back to toggling `CONFIG_P4MINISHELL_C6_EN_GPIO` as the reset line for ROM download mode
- The updater mounts the BSP SD card path on demand, accepts `sd:/...` or `/sdcard/...` paths, and unmounts the card after a successful mount it initiated itself
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
- Every command path now wraps failure-prone ESP-IDF calls with friendly transcript output and pushes summary entries into a small in-memory debug history buffer for later inspection

## Build and flash
```sh
idf.py build flash monitor
```