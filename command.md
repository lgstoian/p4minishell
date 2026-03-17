# Command Reference

P4MiniShell currently exposes the following shell commands.

## Core commands
- `help`: Show the built-in command list.
- `sysinfo`: Show board, display, storage, heap, and Wi-Fi runtime state.
- `version`: Show the app banner string and ESP-IDF version.
- `about`: Show the shell and board summary.
- `clear`: Clear transcript history.
- `reboot`: Restart the board.

## Wi-Fi commands
- `wifi status`: Show Wi-Fi runtime state, target SSID, and connection state.
- `wifi scan`: Scan for nearby access points after Wi-Fi startup succeeds.
- `wifi diag`: Run the background Wi-Fi diagnostic report with connection state, IP status, and a nearby-network scan.
- `wifi connect`: Connect with sdkconfig default credentials.
- `wifi connect <ssid> <pass>`: Connect with runtime credentials; the password is masked in transcript output and skipped from command history.
- `wifi disconnect`: Disconnect the current station session.
- Hosted compatibility behavior: boot-time Wi-Fi startup and `wifi connect` now stop before `esp_wifi_init()` if the ESP32-C6 reports a hosted firmware major or minor version outside the host `2.12.x` line, and the transcript points recovery at `coprocessor/esp32c6_slave` or `c6ota default`.

## Storage and diagnostics
- `sd`: Show SD card status plus the available SD subcommands.
- `sd info`: Mount the SD card on demand and report mount point, detected card name, sector size, estimated capacity, and root availability. On the current esp32p4 BSP path this now uses the explicit SD VO4 3300 mV power-control setup that removes the old repeated `ldo` warning spam.
- `sd ls [path]`: Mount the SD card on demand and list directory entries from `sd:/...`, `/sdcard/...`, or a relative SD-root path. Output is bounded to avoid transcript floods, and file targets are summarized instead of treated as directories.
- `sd stat <path>`: Show the resolved path, entry type, size, and mode bits for a file or directory.
- `sd cat <path> [max_bytes]`: Show a bounded text-safe preview of a file. Non-printable bytes are sanitized, regular files only are accepted, and `max_bytes` is limited to `1..8192`.
- `mem`: Show free heap, minimum heap, internal heap, and PSRAM state.
- `gpio status`: Show display reset, backlight, and hosted C6 reset GPIO levels.
- `debug`: Show recent shell/runtime status entries, free heap, runtime warning count, and Wi-Fi state. Healthy shell UI startup is recorded here instead of as a boot warning.

## ESP32-C6 OTA
- `c6ota sd:/path/to/firmware.bin`: Stream a valid ESP-IDF ESP32-C6 application image from SD over the existing ESP-Hosted SDIO link.
- `c6ota http://host/path/to/firmware.bin`: Download a valid ESP-IDF ESP32-C6 application image over HTTP or HTTPS, then stop Wi-Fi and stream it over a Wi-Fi-off ESP-Hosted SDIO OTA session without tearing down the hosted transport first.
- `c6ota default`: Load `esp32c6_hosted_slave.bin` or `network_adapter.bin` from the SD card root automatically.
- Prerequisites: Wi-Fi must be connected or default sdkconfig credentials must be available for HTTP or HTTPS sources. Factory `v2.3.0` requires the one-time standalone tool from `lboshuizen/crowpanel-p4-c6-sdio-ota` first.
- Behavior: the shell validates image magic `0xE9` plus ESP32-C6 chip ID `0x000D`, asks for the exact confirmation `WARNING: This will reboot the C6. Type YES to continue`, transfers in 1500-byte chunks, reports progress every 5% as `C6 OTA: XX% (YYYY KB / ZZZZ KB)`, keeps the existing hosted SDIO link alive in Wi-Fi-off mode during the transfer, restores Wi-Fi on failures, restores the original Wi-Fi routine on success, and prints `C6 OTA completed successfully! Type reboot to activate new firmware.` on success.

## Runtime notes
- `sd ls` shows full long filenames because FATFS LFN support is enabled with heap-backed buffers and a 255-character limit.
- `c6ota default` resolves `esp32c6_hosted_slave.bin` or `network_adapter.bin` from the SD root correctly.
- Shell commands entered from the prompt run on a dedicated command worker task rather than directly on the LVGL input-event callback stack, which avoids stack-protection panics during heavier commands such as `sd ls`.
- `c6ota` avoids `esp_hosted_deinit()` before transfer because the current ESP-Hosted SDIO teardown path can assert on this esp32p4 host configuration.
- Wi-Fi starts in a background task on normal boot, runs the same restore path after successful `c6ota`, and keeps the transcript diagnostic pass during those restore flows while still exposing `wifi diag` for an extra on-demand report.