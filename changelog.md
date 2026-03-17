## [0.1.13] - 2026-03-17
- Confirmed the working ESP32-P4 host and ESP32-C6 co-processor baseline end to end: shell UI, BSP-managed display and touch init, hosted Wi-Fi startup, Wi-Fi shell commands, SD tools, and `c6ota` now operate together on the checked-in project configuration
- Finalized the host-side ESP-Hosted profile around `espressif/esp_hosted 2.12.1` plus `espressif/esp_wifi_remote 1.4.1`, 1-bit SDIO at 10 MHz on CLK=18 CMD=19 D0=14 D1=15 D2=16 D3=17, forced ESP32-C6 reset on every host boot through GPIO54, and 1500-byte `c6ota` transfer chunks
- Updated the project docs and board metadata to describe the stable working configuration directly instead of the earlier rollback and investigation state

## [0.1.12] - 2026-03-17
- Reverted the earlier hosted startup cleanup batch after Wi-Fi still failed on the ESP32-P4 to ESP32-C6 SDIO path even after the later transport-wait experiment was removed
- Restored the last known-good hosted reset behavior by switching the ESP32-C6 back to `CONFIG_ESP_HOSTED_SLAVE_RESET_ON_EVERY_HOST_BOOTUP`, because this board baseline had proven Wi-Fi startup only with a forced co-processor reset during host boot
- Restored the original boot-time and post-`c6ota` Wi-Fi diagnostic pass and removed the app-side hosted log suppression so the serial monitor and shell transcript again match the earlier working baseline before any new root-cause investigation

## [0.1.11] - 2026-03-17
- Reverted the app-side `ESP_HOSTED_EVENT_TRANSPORT_UP` wait experiment after it regressed the previously working Wi-Fi startup path on this ESP32-P4 to ESP32-C6 SDIO baseline
- Restored the prior hosted startup order that had Wi-Fi working: connect to the ESP32-C6, validate the hosted firmware version, then continue into the normal sdkconfig-driven Wi-Fi runtime path

## [0.1.10] - 2026-03-17
- Removed the automatic `wifi diag` scan from boot-time Wi-Fi startup and post-`c6ota` restore, keeping those paths limited to the proven connect flow while leaving `wifi diag` available on demand for explicit diagnostics
- Restored the hosted reset policy to `CONFIG_ESP_HOSTED_SLAVE_RESET_ONLY_IF_NECESSARY`, which removes the clean-boot `Reset slave using GPIO[54]` warning without changing the working OTA or Wi-Fi path
- Suppressed non-actionable `H_SDIO_DRV` and `rpc_rsp` warning noise in the app so serial output stays focused on real hosted transport failures while the shell transcript remains the user-facing Wi-Fi status surface

## [0.1.9] - 2026-03-17
- Added an explicit ESP-Hosted firmware compatibility gate to the normal Wi-Fi startup path: after `esp_hosted_connect_to_slave()` the shell now reads the ESP32-C6 hosted version and refuses to continue into `esp_wifi_init()` unless the co-processor matches the host `2.12.x` release line
- Fixed the reported SDIO/RPC fallout from mismatched host and co-processor firmware by failing Wi-Fi startup early with transcript-visible recovery guidance instead of continuing into incompatible `esp_wifi_remote` traffic
- Corrected the host component lock back to the ESP32-C6 `2.12.x` line by restoring `espressif/esp_hosted 2.12.1` and `espressif/esp_wifi_remote 1.4.1`, which matches the checked-in C6 project history instead of forcing the co-processor back to `2.9.x`

## [0.1.8] - 2026-03-17
- Re-enabled the original hosted Wi-Fi runtime automatically after normal boot and after successful `c6ota`, keeping the shell UI startup path intact by running the restore flow in a background task instead of the LVGL input path
- Added transcript-facing Wi-Fi diagnostics on boot, after successful `c6ota`, and through the new `wifi diag` command, including connection state, IP status, and a nearby-network scan with SSID, RSSI, channel, and auth mode
- Hardened Wi-Fi runtime retries by cleaning up partial init state before retrying the original startup routine, so boot-time or post-OTA restore failures report cleanly and can be retried without tearing up the shell

## [0.1.7] - 2026-03-17
- Enabled FATFS long filename support for the shell build using heap-backed LFN buffers with a 255-character limit, which fixes truncated SD root names, `sd ls` long-name failures, and `c6ota default` lookup against `esp32c6_hosted_slave.bin` or `network_adapter.bin`
- Switched `sd ls` to direct FatFs directory enumeration so the shell shows full long filenames reliably and no longer trips over the old invalid-name path during long-entry reads on the mounted SD card
- Kept the proven ESP-Hosted `c6ota` flow pinned to `espressif/esp_hosted` `2.9.7`, with Wi-Fi stopped before transfer, 1536-byte OTA chunks, header validation for magic `0xE9` plus ESP32-C6 chip ID, exact YES confirmation text, 5% progress lines, and the factory `v2.3.0` first-upgrade warning pointing to the standalone CrowPanel tool URL
- Fixed the `sd ls` stack-protection panic by moving shell command execution off the LVGL input-event callback stack and onto a dedicated command worker task with its own stack budget and LVGL mutex handoff
- Fixed the `c6ota default` hosted teardown crash by keeping the existing ESP-Hosted SDIO transport alive for Wi-Fi-off OTA mode instead of calling `esp_hosted_deinit()` before reconnecting the C6 link

## [0.1.6] - 2026-03-17
- Fixed the repeated SD-related `ldo` warning spam by replacing the BSP SD-card on-chip LDO helper with a repo-local power-control path that acquires SD VO4 explicitly at 3300 mV on esp32p4
- Kept the earlier plain-SDMMC fallback for invalid or unsupported LDO-control cases, while tightening mount-failure and unmount cleanup so repeated `sd` commands do not leak the SD power handle

## [0.1.5] - 2026-03-17
- Hardened the shell SD path so all SD commands use a shared guarded mount or unmount flow, validated path resolution, bounded transcript output, and safe cleanup on missing cards, bad paths, and open failures
- Expanded the SD command family with `sd info`, `sd stat <path>`, and `sd cat <path> [max_bytes]`, while keeping `sd ls [path]` compatible and improving it with entry type and file size reporting
- Added transcript-safe limits for SD diagnostics: directory listings stop after 128 entries and `sd cat` previews at most 8192 bytes with non-printable bytes sanitized instead of dumping raw binary into the shell

## [0.1.4] - 2026-03-17
- Repaired `c6ota` to match the proven CrowPanel SDIO OTA method: HTTP images are downloaded first, then the shell stops Wi-Fi completely, reinitializes ESP-Hosted, and performs the OTA transfer over a clean SDIO-only link
- Changed `c6ota` transfer chunks to 1536 bytes, added `c6ota default`, enforced ESP32-C6 image validation with image magic plus chip ID, and updated progress output to `C6 OTA: XX% (YYYY KB / ZZZZ KB)` every 5%
- Replaced the old yes or no prompt with the exact confirmation `WARNING: This will reboot the C6. Type YES to continue`, restored Wi-Fi automatically after OTA failures, and updated the success text to `C6 OTA completed successfully! Type reboot to activate new firmware.`
- Added the factory first-upgrade warning for ESP32-C6 firmware `v2.3.0` and pinned the host manifest to `espressif/esp_hosted` `2.9.7`

## [0.1.3] - 2026-03-17
- Removed the temporary boot-time `W (p4minishell)` shell UI initialization log so healthy boots no longer emit a warning just to advertise that the display transcript is live
- Preserved the same startup milestone through the existing `debug` command history instead of the serial warning path, keeping shell boot, LCD render, and on-screen transcript behavior unchanged

## [0.1.2] - 2026-03-17
- c6update utility fully removed and archived (previously used esp-serial-flasher + GPIO54). No code or build traces remain.

## [0.1.1] - 2026-03-14
- Added a new `c6ota <source>` shell command for full ESP-Hosted SDIO OTA against the ESP32-C6 using either `sd:/firmware.bin` or `http://host/path/to/firmware.bin`
- Added an input-driven safety gate for `c6ota` with the exact prompt `This will reboot the C6. Continue? (yes/no)` before any OTA transfer begins
- Extended the hosted OTA path to mount FATFS for SD sources, validate the incoming ESP-IDF app header, check the current co-processor version with `esp_hosted_get_coprocessor_fwversion()`, and require C6 firmware `v2.9.7+` for reliable SDIO OTA
- Streamed OTA payloads over the existing ESP-Hosted SDIO transport in roughly 1400-byte chunks using `esp_hosted_slave_ota_begin/write/end/activate`, with live percentage progress in the locked transcript UI and friendly fallback guidance on failures
- Kept serial `c6update <path>` for merged-image flashing and redirected legacy OTA-style `c6update ota ...` usage to the new `c6ota` command instead of maintaining two shell entry points for the same hosted update flow

## [0.1.0] - 2026-03-13
- Added a real `c6update ota <https-url>` path that uses `esp_http_client` plus `esp_hosted_slave_ota_begin/write/end` to stream an ESP32-C6 image over HTTPS, report progress every 5%, and request OTA activation when the running co-processor firmware supports it
- Kept the stock-board-safe serial guard for `c6update <path>` so unverified P4-to-C6 flash UART wiring still redirects the user to the external `PROG_C6` header instead of pretending host-side serial flashing is available
- Restored `c6update` to a stock-board-safe behavior on the checked-in JC1060P470C or ESP32-P4-Function-EV-Board baseline: the shell now reports that the external `PROG_C6` header with ESP-Prog, or ESP-Hosted OTA, is required when no verified P4-to-C6 flash UART is configured
- Clarified the stock-board GPIO54 note: GPIO54 remains the hosted reset line reference, but the repository does not claim a verified on-board P4-driven C6 serial flashing path without custom wiring
- Updated `c6update sd:/c6_new.bin` guidance to explain that the command is still used for recovery guidance on stock hardware and that a host `reboot` is only relevant after an external or OTA C6 update succeeds
- Revised `c6update` to follow a GPIO54-driven ESP32-C6 ROM download flow: open the SD image first, pulse GPIO54 low/high, optionally hold BOOT from sdkconfig, connect with `esp_serial_flasher`, flash from offset `0x0` in 4 KB chunks, then reset the target
- Updated `c6update` transcript behavior to report live flashing progress every 5% as `Flashing... XX% (YYYYY bytes)` and to emit explicit UART/SD-card oriented failure guidance on any flash step error
- Documented `c6update sd:/c6_new.bin` as the primary shell example and clarified that a successful ESP32-C6 update still requires a host `reboot` command before the new co-processor firmware is used
- Expanded the shell command set with `wifi scan`, `sd ls`, `mem`, `gpio status`, `debug`, `version`, and `about`, while keeping `help`, `sysinfo`, `clear`, and `reboot`
- Added a 5-entry debug/error history buffer and friendly transcript-facing error messages for command and runtime failures
- Confirmed Enter/OK command submission through the input line `LV_EVENT_READY` handler and documented the DOS-style locked transcript UI model
- Added an SD card driven `c6update <path>` shell command that flashes a merged ESP32-C6 image at offset `0x0` with `esp-serial-flasher`
- Changed hosted Wi-Fi startup to be on-demand from `wifi connect` and disabled host auto-restart when the C6 does not answer init, so the shell still boots for recovery/update flows
- Moved `wifi connect` hosted probing into a background task and shortened the hosted SDIO transport-up retry window so missing-C6 failures return faster with less disruption
- Clarified that the checked-in ESP32-P4-Function-EV-Board baseline does not expose a verified on-board P4-controlled C6 flash UART, so `c6update` now reports the external `PROG_C6` / OTA requirement instead of asking for impossible GPIO defaults
- Added sdkconfig-backed ESP32-C6 flasher wiring controls for UART port, UART TX/RX, EN, reset, and BOOT GPIOs so the host does not hardcode board-specific programming pins
- Reported C6 flashing progress and wiring/runtime failures directly into the locked transcript UI while keeping the normal BSP/LVGL shell model unchanged
- Added `espressif/esp-serial-flasher` to the app manifest and removed the duplicate placeholder `app_main()` source from the registered build inputs
- Replaced the LVGL widgets demo in main/main.c with a shell UI built on the existing BSP startup path
- Preserved the original BSP-managed JD9165 display and GT911 touch initialization by keeping bsp_display_start_with_config() and BOARD_CFG_* settings unchanged
- Added a scrollable LVGL textarea, attached lv_keyboard, boot banner, and built-in commands: help, sysinfo, clear, reboot
- Added sysinfo reporting for board_config-backed values, ESP-IDF version, heap usage, PSRAM totals, and Wi-Fi unsupported status on esp32p4
- Refreshed project metadata to match the current shell implementation and verified esp32p4 baseline
- Split the shell UI into a read-only transcript area plus a dedicated prompt-bearing input line bound to the on-screen keyboard
- Added LV_EVENT_READY command submission on the input line and a 10-command recall buffer with Prev/Next touch controls
- Added a runtime Wi-Fi initialization path that follows sdkconfig only, logging every init step or failure into the shell transcript during boot
- Kept the current esp32p4 baseline safe by reporting when sdkconfig does not enable native Wi-Fi or ESP32-C6 host Wi-Fi instead of changing Kconfig
- Added project Wi-Fi defaults in sdkconfig and enabled the host Wi-Fi stack path used by the current esp32p4 workspace
- Added shell Wi-Fi commands for status, connect, and disconnect, with transcript-safe password masking for `wifi connect <ssid> <pass>`
- Disabled LVGL example compilation in sdkconfig so the Wi-Fi-enabled shell still fits the esp32p4 link image budget
- Trimmed sdkconfig to a station-only Wi-Fi profile and disabled Wi-Fi IRAM-heavy optimizations so the host Wi-Fi shell can link on esp32p4
- Reduced compile-time log verbosity, enabled newlib nano format, and disabled AMPDU so the Wi-Fi-enabled image sheds more flash/rodata pressure on esp32p4
- Disabled PSRAM XIP instruction and rodata mapping in sdkconfig because the Wi-Fi-enabled image was overflowing the shared flash/PSRAM mapping window at final link
- Fixed Wi-Fi runtime startup by initializing NVS before esp_wifi_init(), including the standard erase-and-retry recovery path for incompatible stored NVS data
- Replaced the esp32p4 extconn/ESP8689 host Wi-Fi path with ESP-Hosted plus esp_wifi_remote targeting an ESP32-C6 co-processor over SDIO
- Updated the checked-in host transport configuration to CLK=18 CMD=19 D0=14 D1=15 D2=16 D3=17 with reset GPIO54, and mirrored those defaults into sdkconfig.defaults
- Updated shell Wi-Fi diagnostics so hosted-link failures now report the ESP32-C6 SDIO backend and pin map instead of the older extconn hardware note
- Changed ESP-Hosted reset policy to `CONFIG_ESP_HOSTED_SLAVE_RESET_ONLY_IF_NECESSARY` so the host no longer resets the C6 on every clean boot
- Added `coprocessor/esp32c6_slave`, a repo-local ESP32-C6 ESP-Hosted slave firmware project that tracks the same `2.12.1` source line as the host dependency lock