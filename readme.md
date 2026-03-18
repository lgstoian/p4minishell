# P4MiniShell
P4MiniShell is an embedded, touch-driven command shell for the ESP32-P4 host and ESP32-C6 co-processor baseline used in this workspace. It replaces the demo UI with a persistent DOS-style shell surface built on LVGL while preserving the checked-in BSP display, touch, SD, and hosted Wi-Fi bring-up path.

The current firmware is not a desktop DOS clone and it is not yet an MS-DOS-compatible runtime. What it already provides is the embedded foundation for that direction: a locked transcript UI, RAM-only shell state, SD-backed file workflows, batch-file execution, ESP-Hosted Wi-Fi on the C6, and a real OTA maintenance path for the co-processor.

## What this project is
- A single-binary ESP32-P4 shell application with the shell UI in [main/main.c](main/main.c) and hosted connectivity extracted into `components/networking`
- A DOS-inspired command environment for SD-card workflows on an embedded touchscreen device
- A host-side control surface for an ESP32-C6 connected over ESP-Hosted SDIO
- A base for future native application loading, shell SDK work, and broader MS-DOS-style command compatibility

## What this project is not yet
- A full MS-DOS-compatible command interpreter
- A real process-based operating system with executable loading and isolation
- A literal x86 `.exe` runtime
- A complete application SDK for third-party shell apps

## Architecture summary
- UI: LVGL transcript textarea, prompt-bearing input line, on-screen keyboard, and touch recall controls
- Serial console: the configured ESP-IDF console stdin and stdout now mirror the same shell transcript and command path used by the touch UI, so `idf.py monitor` can act as an interactive shell endpoint
- Storage model: guarded SD-card access with DOS-style relative paths rooted at `/sdcard`
- Shell model: worker-task command execution, RAM-only environment variables, PATH, batch frames, and transcript-backed redirection
- Connectivity: ESP32-P4 host Wi-Fi and BLE routed through a dedicated networking component using ESP-Hosted plus `esp_wifi_remote` and hosted NimBLE on the ESP32-C6 over SDIO
- Maintenance path: `c6ota` for validated ESP32-C6 application updates from SD or HTTP/S sources

## Planning and licensing
- Implementation roadmap: see [roadmap.md](roadmap.md)
- Project and third-party license summary: see [licence.md](licence.md)

## Current behavior
- Boot banner: P4MiniShell v0.1 ready | JC1060P470C | type help
- UI: DOS-style transcript area, prompt-bearing input line, recall buttons, and attached lv_keyboard
- Healthy boot behavior: shell UI startup no longer emits a warning-level serial log; the same milestone is stored in the existing `debug` history instead
- Enter behavior: command execution is confirmed on the input line through LV_EVENT_READY, while transcript history remains locked above it
- Command execution safety: the input callback now queues shell work onto a dedicated command task, so heavier commands such as `sd ls` do not overflow the small LVGL event stack
- Command-family dispatch safety: the shell now preserves the original unsplit command line before tokenization, so `wifi`, `sd`, and `c6ota` subcommands continue to work after the worker-task parser hands control to their family-specific handlers
- UART and monitor interaction: the firmware now consumes stdin on the configured ESP-IDF console, prints the same prompt on the serial side, mirrors transcript output to stdout, and routes typed monitor commands back through the same shell parser, password masking, history, and transcript flow used on-screen
- Serial prompt behavior: the monitor prompt is now emitted only when a fresh command entry is needed, which prevents idle stdin polling from flooding repeated `P4Shell>` prompts
- Commands: help, cls, c6ota, sysinfo, brightness, rotate, battery, volume, cd, chdir, dir, copy, move, del, erase, ren, rename, md, mkdir, rd, rmdir, type, write, append, touch, call, set, path, echo, wifi status, wifi scan, wifi diag, wifi connect, wifi disconnect, sd info, sd ls, sd stat, sd cat, mem, gpio list, gpio status, gpio read, gpio set, bluetooth status, bluetooth scan, bluetooth advertise on, bluetooth advertise off, bt (alias), rgb, camera, debug, clear, reboot, version, ver, about
- Command recall: last 10 commands via Prev/Next buttons, with the input line kept separate from transcript history
- Display/touch init: still owned by the managed BSP and board_config-generated constants
- Display controls: `brightness <0-100>` drives the BSP backlight PWM path and `rotate <0|90|180|270>` rotates the active LVGL display while remapping GT911 touch coordinates to match
- Battery and power status: `battery` reads the configured ADC pin and divider values from board metadata, reports scaled voltage plus an estimated percentage, and `battery sleep <on|off|status>` reports or requests light sleep only when `CONFIG_PM_ENABLE` is enabled in sdkconfig
- Audio control: `volume <0-100>` uses the existing ES8311 speaker path exposed by the BSP codec device
- GPIO controls: `gpio list` and `gpio status` now report the exposed board pins with clearer board-role text for the shared I2C bus, audio path, ESP32-C6 hosted SDIO link, battery monitor, and MicroSD bus, while `gpio set <pin> <0|1>` stays limited to pins marked safe for shell-side writes
- Bluetooth command surface: `bluetooth status`, `bluetooth scan`, and `bluetooth advertise <on|off>` now route through hosted NimBLE on the ESP32-C6 over ESP-Hosted VHCI, with `bt` kept as a parser alias for the same command family
- Bluetooth lifecycle: after `bluetooth enable`, later `bluetooth scan` or `bluetooth advertise <on|off>` calls reuse the existing hosted controller and NimBLE host state instead of reinitializing the controller path
- RGB and camera command surface: `rgb led <color>`, `rgb <r> <g> <b>`, `camera init`, and `camera snap <filename>` stay intentionally blocked for now because the JC1060 reference repo still does not expose authoritative RGB LED wiring and this workspace still lacks the declared camera stack used by the JC1060 camera examples
- Wi-Fi startup: attempted automatically in a background task after normal boot through the `components/networking` module using the original sdkconfig-driven hosted routine, with default credentials available for `wifi connect` and password masking for `wifi connect <ssid> <pass>` in transcript/history
- Hosted compatibility guard: normal Wi-Fi startup now reads the ESP32-C6 hosted firmware version right after the SDIO link comes up and refuses to continue unless the co-processor matches the host `2.12.x` ESP-Hosted line, which prevents the earlier SDIO queue drops and RPC response errors seen with mismatched firmware
- Wi-Fi status: enabled in the checked-in sdkconfig through ESP-Hosted plus `esp_wifi_remote`, with `wifi scan` available once the runtime has started
- C6 firmware update path: `c6ota <sd:/file.bin|http[s]://url|default>` downloads or opens a valid ESP32-C6 app image, then performs the Wi-Fi-off ESP-Hosted SDIO OTA flow over the existing hosted link without tearing the SDIO transport down first
- Hosted Wi-Fi restores automatically after normal boot and after successful `c6ota`, while the shell stays usable because the hosted probe and reconnect run in a background task with the normal transcript-facing diagnostic pass
- `wifi diag` emits transcript-facing status plus scan output, and `wifi connect` still probes ESP-Hosted in a background task so a missing C6 fails visibly without blocking the shell
- Wi-Fi command reliability: `wifi status | scan | diag | connect | disconnect` now share the fixed family-dispatch path, so post-boot shell control works again after the parser regression that had been dropping subcommands at runtime
- C6 firmware update image requirement: the OTA path expects a valid ESP-IDF application image with a readable app header
- Wi-Fi runtime prerequisite: NVS is initialized before esp_wifi_init(), with an automatic erase-and-retry path if stored NVS metadata is incompatible
- Host Wi-Fi prerequisite: ESP-Hosted is enabled and the shell connects to an ESP32-C6 co-processor over SDIO before esp_wifi_init() runs on esp32p4
- Host Wi-Fi compatibility prerequisite: the P4 host is now restored to the ESP32-C6 `2.12.x` hosted line through `espressif/esp_hosted 2.12.1`; if the running C6 reports a different major or minor version, the shell keeps Wi-Fi off and points recovery at `coprocessor/esp32c6_slave` or `c6ota default`
- Host Wi-Fi hardware requirement: the checked-in hosted path expects ESP32-C6 over SDIO on CLK=18 CMD=19 D0=14 D1=15 D2=16 D3=17 with reset GPIO54; if the co-processor does not answer there, the shell reports the hosted-link failure explicitly
- Hosted reset policy: this project resets the ESP32-C6 on every host boot before the SDIO Wi-Fi bring-up path continues
- Co-processor firmware upgrade path: `coprocessor/esp32c6_slave` remains the repo-local ESP32-C6 firmware project, and the shell host path stays on the same `2.12.x` ESP-Hosted release line used by that project
- Networking module layout: `components/networking/networking.c` owns hosted Wi-Fi startup, state, events, scans, diagnostics, and OTA restore hooks, while `components/networking/bluetooth.c` owns hosted NimBLE controller bring-up, scan, and advertising control
- Monitor usage: after flashing, `idf.py monitor` can now be used as a real shell endpoint over the configured console path, not only as a log viewer
- Command reference: see `command.md` for the current shell command surface and usage notes
- Build footprint: unused LVGL examples are disabled in sdkconfig so the Wi-Fi-enabled shell still links on the esp32p4 baseline
- Wi-Fi profile: sdkconfig is trimmed for station use only, with WPA2-style credential flow and Wi-Fi IRAM optimizations disabled to stay within the esp32p4 image budget
- Toolchain profile: sdkconfig now uses newlib nano formatting and warn-level compile-time logging to keep the shell build inside the esp32p4 image window
- Memory mapping: PSRAM XIP instruction/rodata mapping is disabled so host Wi-Fi does not overflow the shared flash/PSRAM mapping window during link
- Error handling: command failures now produce friendly transcript messages and are stored in a 5-entry debug history visible through `debug`
- Debugging: `debug` reports the last 5 stored shell/runtime status entries, current Wi-Fi state, free heap, and runtime warning count; normal shell UI startup is recorded there without raising a boot warning
- SD command hardening: every shell-side SD operation now uses a shared guarded mount or unmount path, bounded directory output, validated path resolution, and safe cleanup so missing cards or bad paths do not destabilize the shell
- SD long filename support: FATFS LFN is now enabled with heap-backed buffers and a 255-character limit, so `sd ls` shows full names from the SD root and `c6ota default` can resolve `esp32c6_hosted_slave.bin` or `network_adapter.bin` without truncation-related misses
- SD BSP power control: the managed board layer now acquires SD VO4 explicitly at 3300 mV for SD-card IO power, which removes the repeated `ldo` voltage-0 warning spam seen during SD mounts on the esp32p4 path
- SD shell tools: `sd info` reports card metadata, `sd ls` now shows entry types and file sizes, `sd stat` reports file or directory details, and `sd cat` provides a bounded text-safe file preview for quick inspection on-device
- DOS-style SD shell tools: the shell now keeps a RAM-only current SD working directory, RAM-only environment variables and PATH, text-file creation and editing helpers, COMMAND.COM-style file operations, batch-file execution from SD, and transcript-safe `>` / `>>` output redirection back onto SD files

## Hardware and software baseline
- Target: esp32p4
- Display: JD9165 1024x600 via the existing BSP and esp_lcd_jd9165
- Touch: GT911 via the existing BSP and esp_lcd_touch_gt911
- Battery monitor: ADC on GPIO53 with a 2:1 divider model and 3.3 V to 4.2 V scaling window for the current shell estimate
- LVGL port: esp_lvgl_port from managed_components
- Config source: board_config.yaml, sdkconfig, and the managed component manifests
- RGB LED declaration: not present in the current board metadata, so RGB shell commands stay blocked intentionally
- Camera declaration: not present in the current board metadata, so camera shell commands stay blocked intentionally
- Current Wi-Fi config: ESP-Hosted + esp_wifi_remote are enabled in sdkconfig for an ESP32-C6 SDIO co-processor and the shell can connect using either sdkconfig defaults or runtime credentials
- GPIO54 reference: GPIO54 remains the documented ESP32-C6 reset line used by the hosted SDIO path
- C6 OTA progress and errors: the locked transcript emits `C6 OTA: XX% (YYYY KB / ZZZZ KB)` every 5% for `c6ota <sd:/file.bin|http[s]://url|default>`, restores Wi-Fi after success, restores Wi-Fi after failures, and stores clear SD, HTTP, Wi-Fi, and hosted-link failure hints in the debug history buffer
- Current board caveat: this workspace assumes the ESP32-C6 is wired to the dedicated hosted SDIO pins 18/19/14/15/16/17 plus reset 54, not the BSP SD-card bus pins 39-44

## Build and flash
```sh
idf.py build flash monitor
```

## C6 update usage
1. Copy a valid ESP-IDF ESP32-C6 application image to the SD card if you plan to use the SD-backed OTA path.
2. Run `c6ota sd:/c6_new.bin` to stream an OTA image from the SD card after the shell stops Wi-Fi and switches the existing ESP-Hosted link into a transport-only OTA session.
3. Run `c6ota http://host/path/to/esp32c6.bin` or `c6ota https://host/path/to/esp32c6.bin` to download the image first, then transfer it over the same Wi-Fi-off SDIO OTA path.
4. Run `c6ota default` to load `esp32c6_hosted_slave.bin` or `network_adapter.bin` from the SD card root automatically.
5. `c6ota` validates the image magic byte and ESP32-C6 chip ID, transfers the payload in 1500-byte chunks, reports progress every 5%, and asks for the exact confirmation `WARNING: This will reboot the C6. Type YES to continue` before any OTA transfer begins.
6. Factory `v2.3.0` needs the one-time standalone tool from `https://github.com/lboshuizen/crowpanel-p4-c6-sdio-ota` first. On success the shell prints `C6 OTA completed successfully! Type reboot to activate new firmware.` and immediately re-runs the normal Wi-Fi routine in the background.
7. If boot or `wifi connect` reports a hosted version mismatch, the host and C6 need to land on the same `2.12.x` line. The checked-in host manifest now follows that line again, and the repo-local recovery path remains flashing the matching image from `coprocessor/esp32c6_slave` or placing `esp32c6_hosted_slave.bin` on SD and running `c6ota default`.