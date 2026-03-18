## Overview
P4MiniShell now boots into a simple shell UI instead of the LVGL widgets demo.

## Project documents
- `readme.md`: user-facing introduction, current behavior, and build notes
- `roadmap.md`: parity plan for the DOS-style shell, native app runtime, and future SDK work
- `licence.md`: proprietary notice for project-authored code plus third-party license summary

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
- Heavy shell commands no longer run on the LVGL input-event stack directly; the callback now hands work to a dedicated command task before command parsing and SD/FATFS traversal begin
- Command-family handlers now receive a preserved copy of the original command line before the generic parser tokenizes it, which keeps `wifi`, `sd`, and `c6ota` subcommands functional when they perform their own second-stage parsing
- The visual style intentionally stays close to a compact DOS/MS-DOS terminal: dense text, immutable history pane, prompt line, and scan-friendly output
- Healthy shell startup is treated as normal status, not a warning; the boot milestone is retained in the `debug` command history instead of the serial warning stream
- See `command.md` for the current command reference

## Built-in commands
- help: list available commands
- cls: alias of `clear`
- c6ota <sd:/file.bin|http[s]://url|default>: perform the real ESP-Hosted SDIO OTA update against the ESP32-C6, using a staged HTTP download or SD/default image source before a Wi-Fi-off transport-only transfer after the exact YES confirmation prompt
- cd / chdir [path]: show or change the RAM-only current SD working directory used by DOS-style file commands
- dir [path]: list files and directories from the current SD working directory using the guarded SD access path
- copy, move, del / erase, ren / rename, md / mkdir, rd / rmdir: COMMAND.COM-style SD file management commands using long filenames and UTF-8 paths
- type <path>: print a text-safe file dump from SD without raw binary output
- write <path> <text>, append <path> <text>, touch <path>: create or modify SD text files through the shell worker task only
- call <file.bat> [args]: execute a batch file from SD with `%1`..`%9`, `rem`, and `echo on/off`
- set, path, echo: RAM-only environment and batch control commands, including PATH-based `.bat` lookup
- brightness <0-100>: set the LCD backlight level through the existing BSP brightness API
- rotate <0|90|180|270>: rotate the active display and remap the GT911 touch transform so pointer coordinates stay aligned with the panel
- battery: report scaled battery voltage, estimated percentage, raw ADC reading, and configured light-sleep status
- battery sleep <on|off|status>: request or query light sleep only when power management is enabled in sdkconfig
- volume <0-100>: set speaker output volume through the ES8311 codec device already used by the BSP audio path
- gpio list | status | read <pin> | set <pin> <0|1>: expose the board pin table, allow reads, and restrict writes to shell-safe GPIOs only
- bt status | bt enable | bt scan: shell-facing Bluetooth command surface kept parser-visible, but intentionally disabled on the current ESP32-C6 hosted baseline because the attempted Bluedroid bring-up path proved unstable during controller startup
- rgb led <color> or rgb <r> <g> <b>: reserved command surface for a future board-declared RGB LED implementation; the current workspace still reports unsupported because the JC1060 reference repo does not expose authoritative RGB LED wiring or a declared RGB driver here
- camera init | camera snap <filename>: reserved command surface for a future board-declared camera path; the current workspace still reports unsupported because the JC1060 reference repo shows a camera add-on path, but this workspace does not ship the declared sensor, CSI map, or local camera stack needed to use it
- Wi-Fi/ESP-Hosted startup is asynchronous: the UI boots first, then the original hosted Wi-Fi routine runs in a worker task during normal boot so a dead or blank C6 does not block the shell surface
- Wi-Fi/ESP-Hosted startup now includes a firm compatibility gate: once the SDIO link is up, the shell reads the ESP32-C6 hosted firmware version and aborts Wi-Fi startup unless the co-processor matches the host `2.12.x` ESP-Hosted release line
- The same background worker now restores Wi-Fi after successful `c6ota`, and the working project configuration keeps the transcript status + scan diagnostic pass on boot and post-OTA restore while still leaving `wifi diag` available on demand
- sysinfo: report board_config-backed display/touch/storage values, IDF version, heap, PSRAM, and current Wi-Fi runtime state
- wifi status: show Wi-Fi runtime state, target SSID, AP info, and IP info when available
- wifi scan: only runs after Wi-Fi startup succeeds, then lists SSIDs, RSSI, auth mode, and channel
- wifi diag: run the transcript-facing Wi-Fi status and scan diagnostic path manually
- wifi connect: connect using sdkconfig default credentials
- wifi connect <ssid> <pass>: connect using runtime credentials without writing the password into transcript history or recall history
- wifi disconnect: disconnect the current station session
- sd: show SD status and available SD subcommands
- sd info: mount the SD card on demand and report card metadata plus root availability
- sd ls [path]: mount the SD card on demand, enumerate directory entries through the FatFs LFN path, list full long filenames with entry type and file sizes, and report a friendly insert-and-retry message when no card is present
- sd stat <path>: show resolved path, entry type, size, and mode for a file or directory
- sd cat <path> [max_bytes]: show a bounded text-safe preview of a regular file without dumping arbitrary binary data into the transcript
- mem: report current free heap, minimum heap, internal heap, and PSRAM usage
- gpio status: report key GPIO levels for display and C6-related pins
- debug: show the last 5 stored error/warning entries plus Wi-Fi state, heap, and warning count
- clear: clear terminal history and redraw a fresh prompt
- reboot: print a reboot message and restart the board
- version/about: report app, board, and ESP-IDF identity details

## Runtime constraints
- Active target is esp32p4
- GPIO54 remains the documented ESP32-C6 reset reference for the hosted SDIO path
- The shell exposes only the hosted OTA path; if the C6 firmware is missing or too old for OTA, rebuild or externally refresh `coprocessor/esp32c6_slave`
- Reboot is still required after a successful OTA C6 firmware update so the P4 host reconnects against the new co-processor firmware
- Factory first-upgrade prerequisite: ESP32-C6 factory firmware `v2.3.0` still needs the one-time standalone tool from `lboshuizen/crowpanel-p4-c6-sdio-ota` before shell-driven OTA is used
- Warning: the ESP32-C6 reboots after successful OTA activation
- The updater mounts the BSP SD card path on demand, accepts `sd:/...` or `/sdcard/...` paths, and unmounts the card after a successful mount it initiated itself
- FATFS long filename support is now enabled with heap-backed buffers and `CONFIG_FATFS_MAX_LFN=255`, using the ESP-IDF 5.5.3 symbol `CONFIG_FATFS_API_ENCODING_UTF_8` for UTF-8 API paths so long SD root names resolve correctly
- The `sd` command family now shares the same guarded BSP mount path, treats `sd:/...`, `/sdcard/...`, and relative SD-root paths consistently, limits directory listings to 128 entries, bounds file previews to at most 8192 bytes, and uses direct FatFs directory reads for `sd ls` so long filenames no longer truncate or trigger invalid-name failures
- The DOS-style file commands reuse the same guarded SD mount path, but keep their own RAM-only current working directory and PATH so `cd`, `dir`, `call`, and `.bat` files behave predictably without persisting state outside runtime RAM
- Output redirection now supports `>` and `>>` for text-producing shell commands by copying the transcript delta for that command into an SD file, so transcript output remains visible on-screen while the same text is written to SD
- The managed BSP now acquires the SD IO VO4 LDO rail explicitly at 3300 mV before SD mounts on esp32p4, which fixes the earlier repeated `ldo` voltage-0 warnings without removing the plain-SD fallback for unsupported power-control cases
- The OTA worker auto-starts Wi-Fi from sdkconfig defaults when possible for `http://` or `https://` sources, otherwise it requires an existing Wi-Fi session before opening the HTTP download stage
- After the image is available locally, the OTA worker stops Wi-Fi with `esp_wifi_stop()` plus `esp_wifi_deinit()`, keeps the existing ESP-Hosted transport alive, reconnects the SDIO link in Wi-Fi-off mode, and then streams the payload through `esp_hosted_slave_ota_begin/write/end`
- The OTA worker accepts `sd:/...`, `/sdcard/...`, or `default`, validates the incoming ESP-IDF image header for magic `0xE9` and ESP32-C6 chip ID `0x000D`, resolves `default` from the SD root using the same LFN-safe path handling, streams the payload in 1500-byte chunks, reports progress every 5% as `C6 OTA: XX% (YYYY KB / ZZZZ KB)`, and requests `esp_hosted_slave_ota_activate()` when the running C6 firmware exposes that API
- Factory first-upgrade note: ESP32-C6 firmware `v2.3.0` still requires the one-time standalone tool from `https://github.com/lboshuizen/crowpanel-p4-c6-sdio-ota` before shell-driven OTA is used
- Wi-Fi startup now follows sdkconfig at runtime and the checked-in workspace enables the host Wi-Fi path for the esp32p4 board baseline
- Wi-Fi runtime now initializes NVS first and falls back to erase-and-retry when the stored NVS layout is incompatible, because esp_wifi_init() depends on NVS being ready on this configuration
- On the esp32p4 host Wi-Fi path, ESP-Hosted now connects to an ESP32-C6 co-processor over SDIO before esp_wifi_init() runs so the standard `esp_wifi_*` shell code can stay unchanged via `esp_wifi_remote`
- To avoid incompatible remote-Wi-Fi RPC traffic, the shell now stops that startup path immediately when the ESP32-C6 hosted firmware major or minor version does not match the checked-in host `2.12.x` line; recovery stays on the repo-local `coprocessor/esp32c6_slave` or `c6ota default` path instead of suppressing the warning
- The checked-in hosted transport uses CLK=18 CMD=19 D0=14 D1=15 D2=16 D3=17 and reset GPIO54; if the ESP32-C6 path is absent or miswired, the shell reports the exact hosted-link failure instead of the old extconn hardware note
- ESP-Hosted reset policy is `SLAVE_RESET_ON_EVERY_HOST_BOOTUP`, because the working ESP32-P4 to ESP32-C6 configuration in this workspace depends on a forced co-processor reset during host boot
- `coprocessor/esp32c6_slave` is the repo-local co-processor project for ESP32-C6 recovery and hosted-firmware work, and the shell host build stays on the same `2.12.x` release line
- Default Wi-Fi credentials are stored in sdkconfig for local validation, while runtime `wifi connect <ssid> <pass>` masks the password in transcript output and keeps it out of command recall
- LVGL example compilation is disabled in sdkconfig because the shell app no longer uses those sources and the Wi-Fi-enabled image otherwise overruns the esp32p4 link region
- The checked-in Wi-Fi profile is intentionally station-only for the shell use case, with WPA3, enterprise auth, SoftAP, and Wi-Fi IRAM optimizations disabled to reduce linker pressure on esp32p4
- Additional size controls now active in sdkconfig are warn-level compile-time logging, newlib nano formatting, and disabled AMPDU aggregation because the shell does not need peak Wi-Fi throughput
- PSRAM XIP instruction and rodata mapping are also disabled in sdkconfig because the host Wi-Fi image exceeded the shared flash/PSRAM mapping window during link with those options enabled
- The repository still carries a requested JC1060P470C name while the checked-in BSP baseline is ESP32-P4-Function-EV-Board
- Command submission is handled by LV_EVENT_READY on the input line, not by editing the transcript directly
- The `sd ls` panic seen after the LFN change was caused by stack pressure on the LVGL input callback, so shell command execution now runs on a separate worker task with an explicit stack budget while LVGL access is wrapped by the port mutex
- The hosted OTA path no longer calls `esp_hosted_deinit()` before `c6ota` transfers because the ESP-Hosted SDIO teardown path can assert on this esp32p4 baseline; OTA now follows the upstream example flow of stopping Wi-Fi, reusing the existing hosted transport, then restoring the original Wi-Fi routine after success
- The `wifi`, `sd`, and `c6ota` command families now preserve their full subcommand text across the worker-task parser handoff, which fixes the runtime regression where family commands could appear inert after the generic parser split the first token in place
- Every command path now wraps failure-prone ESP-IDF calls with friendly transcript output and pushes summary entries into a small in-memory debug history buffer for later inspection
- Normal shell UI initialization is also pushed into that debug history so the boot path stays observable without producing a warning on successful startup
- The new hardware command family follows the same rule: unsupported Bluetooth, RGB LED, or camera paths fail explicitly in the transcript instead of inventing board support or silently touching undeclared GPIO wiring
- Example OTA commands: `c6ota sd:/esp32c6_hosted_slave.bin`, `c6ota https://host/path/to/esp32c6.bin`, and `c6ota default`

## Build and flash
```sh
idf.py build flash monitor
```