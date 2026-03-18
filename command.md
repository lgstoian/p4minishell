# Command Reference

P4MiniShell currently exposes the following shell commands.

Project planning and license notes live in `roadmap.md` and `licence.md`.

## Core commands
- `help`: Show the built-in command list.
- `sysinfo`: Show board, display, storage, heap, and Wi-Fi runtime state.
- `brightness <0-100>`: Set the LCD backlight brightness through the BSP PWM brightness path.
- `rotate <0|90|180|270>`: Rotate the display and remap GT911 touch orientation to match.
- `battery`: Read the configured battery ADC pin, show scaled voltage, estimated percentage, raw ADC data, and current light-sleep request state.
- `battery sleep <on|off|status>`: Request or inspect light sleep only when `CONFIG_PM_ENABLE` is enabled.
- `volume <0-100>`: Set the speaker volume through the existing ES8311 codec path.
- `version`: Show the app banner string and ESP-IDF version.
- `ver`: Alias of `version`.
- `about`: Show the shell and board summary.
- `clear`: Clear transcript history.
- `cls`: Alias of `clear`.
- `reboot`: Restart the board.

## Console access
- On-screen shell: the LVGL prompt remains the primary touch-driven shell entry point.
- Serial shell: the configured ESP-IDF console exposed through `idf.py monitor` now accepts the same commands and prints the same prompt and transcript output.
- Shared behavior: commands entered through the serial console reuse the same shell submit path, masking rules, history policy, and transcript output model as touch-entered commands, and the prompt is only reprinted when a fresh serial command line is expected.

## DOS-style shell commands
- `cd` or `chdir`: Show the current SD working directory.
- `cd <path>`: Change the RAM-only current working directory. Supports `sd:/...`, `/sdcard/...`, relative paths, `.`, and `..`.
- `dir [path]`: List directory entries from the current directory or an explicit target path with bounded transcript output.
- `copy <src> <dst>`: Copy a file on SD using the guarded shell worker path.
- `move <src> <dst>`: Move or rename a file or directory on SD.
- `del <path>` or `erase <path>`: Delete a file from SD.
- `ren <src> <dst>` or `rename <src> <dst>`: Rename a file or directory on SD.
- `md <path>` or `mkdir <path>`: Create a directory on SD.
- `rd <path>` or `rmdir <path>`: Remove an empty directory from SD.
- `type <path>`: Print a text-safe file preview without dumping raw binary bytes into the transcript.
- `write <path> <text>`: Create or overwrite a text file with the provided text.
- `append <path> <text>`: Append text to a file, creating it if needed.
- `touch <path>`: Create an empty file if missing or refresh its timestamp if it already exists.
- `set`: List RAM-only environment variables.
- `set NAME=VALUE`: Create or update a RAM-only environment variable.
- `path`: Show the current RAM-only batch PATH.
- `path <dir1>;<dir2>;...`: Replace the PATH used for `.bat` lookup.
- `echo <text>`: Print text after variable expansion.
- `echo on` or `echo off`: Enable or disable batch command echoing.
- `call <file.bat> [args]`: Execute a batch file from SD with `%1` through `%9` argument expansion.

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
- `gpio list`: Show the exposed board GPIO table, including current level, shell write policy, and a short role description for each board pin.
- `gpio status`: Show the current levels plus role text for the full exposed board pin table.
- `gpio read <pin>`: Read the current logic level from an arbitrary GPIO number.
- `gpio set <pin> <0|1>`: Drive a GPIO only when the shell pin table marks that pin safe for writes.
- `bluetooth status`: Show hosted Bluetooth readiness, NimBLE sync state, and current advertising state.
- `bluetooth scan`: Run a BLE scan through the hosted NimBLE path on the ESP32-C6 and print discovered devices to the transcript.
- `bluetooth advertise on`: Start non-connectable BLE advertising through hosted NimBLE on the ESP32-C6.
- `bluetooth advertise off`: Stop hosted BLE advertising.
- `bt ...`: Alias for the `bluetooth` command family.
- Lifecycle note: `bluetooth enable` is the one-time host bring-up path for the current boot; later scan and advertising commands reuse that running hosted controller session instead of restarting it.
- `rgb led <color>`: Reserved color-name command surface for a future board-declared RGB LED implementation.
- `rgb <r> <g> <b>`: Reserved numeric RGB command surface for a future board-declared RGB LED implementation.
- `camera init`: Reserved camera initialization command surface for a future board-declared camera implementation.
- `camera snap <filename>`: Reserved snapshot command surface for a future board-declared camera implementation.
- `debug`: Show recent shell/runtime status entries, free heap, runtime warning count, and Wi-Fi state. Healthy shell UI startup is recorded here instead of as a boot warning.

## ESP32-C6 OTA
- Implementation: the shell parser now delegates `c6ota` to `components/c6ota/c6ota.c`, with the stable module entry points documented in `API.md` and `SDK.md`.
- `c6ota sd:/path/to/firmware.bin`: Stream a valid ESP-IDF ESP32-C6 application image from SD over the existing ESP-Hosted SDIO link.
- `c6ota http://host/path/to/firmware.bin`: Download a valid ESP-IDF ESP32-C6 application image over HTTP or HTTPS, then stop Wi-Fi and stream it over a Wi-Fi-off ESP-Hosted SDIO OTA session without tearing down the hosted transport first.
- `c6ota default`: Load `esp32c6_hosted_slave.bin` or `network_adapter.bin` from the SD card root automatically.
- Prerequisites: Wi-Fi must be connected or default sdkconfig credentials must be available for HTTP or HTTPS sources. Factory `v2.3.0` requires the one-time standalone tool from `lboshuizen/crowpanel-p4-c6-sdio-ota` first.
- Behavior: the shell validates image magic `0xE9` plus ESP32-C6 chip ID `0x000D`, asks for the exact confirmation `WARNING: This will reboot the C6. Type YES to continue`, transfers in 1500-byte chunks, reports progress every 5% as `C6 OTA: XX% (YYYY KB / ZZZZ KB)`, keeps the existing hosted SDIO link alive in Wi-Fi-off mode during the transfer, restores Wi-Fi on failures, restores the original Wi-Fi routine on success, and prints `C6 OTA completed successfully! Type reboot to activate new firmware.` on success.

## Runtime notes
- `sd ls` shows full long filenames because FATFS LFN support is enabled with heap-backed buffers and a 255-character limit.
- `c6ota default` resolves `esp32c6_hosted_slave.bin` or `network_adapter.bin` from the SD root correctly.
- Shell commands entered from the prompt run on a dedicated command worker task rather than directly on the LVGL input-event callback stack, which avoids stack-protection panics during heavier commands such as `sd ls`.
- DOS-style file commands reuse the same guarded SD mount path as the `sd` command family, but maintain their own RAM-only current working directory and PATH state.
- `>` and `>>` redirection write the transcript delta for a command to an SD file while still leaving the command output visible in the shell transcript.
- Batch files support `.bat` lookup through the current working directory and PATH, `%1` through `%9` argument expansion, `rem` and `::` comments, and `echo on` or `echo off` flow control.
- `c6ota` avoids `esp_hosted_deinit()` before transfer because the current ESP-Hosted SDIO teardown path can assert on this esp32p4 host configuration.
- Wi-Fi starts in a background task on normal boot, runs the same restore path after successful `c6ota`, and keeps the transcript diagnostic pass during those restore flows while still exposing `wifi diag` for an extra on-demand report.
- Hosted Wi-Fi and hosted Bluetooth now live under `components/networking`, so the shell parser delegates those command families instead of owning the runtime transport logic directly in `main/main.c`.
- Hosted OTA now lives under `components/c6ota`, so `main/main.c` only provides transcript and parser orchestration while the OTA module keeps the proven update flow intact.
- The `wifi`, `sd`, and `c6ota` command families now preserve the full unsplit command line before subcommand parsing, which fixes the regression where family commands could lose their subcommand text after the generic parser tokenized the first word in place.
- `idf.py monitor` is now interactive on the configured console path because the firmware consumes stdin and mirrors shell transcript output to stdout instead of leaving the serial path as logs only.
- `rgb` and `camera` stay intentionally explicit about unsupported states: the JC1060 reference repo does not expose authoritative RGB LED wiring, and this workspace still lacks the local camera stack needed by the JC1060 camera examples.