# Changelog

All notable changes to P4MiniShell are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [0.6.0] - 2026-04-29

### Fixed
- **SD card status icon in header**: Fixed `header_update_sd()` to immediately update state and fall back to direct render when LVGL async dispatch fails
- **Header update robustness**: All `header_update_*()` functions now update internal state immediately (safe from any task context) and fall back to synchronous render on allocation/async failure
- **SD icon rendering**: SD indicator now uses consistent `HEADER_SD_SYMBOL` in both mounted and unmounted states instead of `LV_SYMBOL_WARNING` when unmounted

### Changed
- **header.c**: Refactored all public update functions (`header_update_wifi`, `header_update_battery`, `header_update_bluetooth`, `header_update_usb`, `header_update_sd`) to set state immediately before scheduling async render
- **header.c**: Async callbacks simplified to render-only (state already set by caller)
- **header.c**: Battery percent clamping moved from async callback to public API entry point

### Verified
- **Clean build**: Zero errors, zero warnings
- **Binary**: p4minishell.bin 1,490,688 bytes (82% free)
- **No regressions**: Boot, screen rendering, Wi-Fi, all commands preserved

---

## [0.5.0] - 2026-04-29

### Fixed
- **Stale AI comments**: Replaced all 25+ `// AI:` prefixed comments across main.c, usb.c, c6ota.c, and managed BSP with proper descriptive comments
- **Comment consistency**: All section markers now use descriptive text instead of AI-prefixed tags

### Verified
- **Clean build**: Zero errors, zero warnings on ESP-IDF 5.5.3 / esp32p4 target
- **Binary integrity**: p4minishell.bin 1,490,608 bytes (82% free in 8MB partition)
- **No regressions**: Boot, screen rendering, Wi-Fi, all 40+ commands preserved

### Documentation
- Updated ai-context.md with audit rules and verification checklist
- Updated changelog.md with v0.5.0 hardening entry

---

## [0.4.0] - 2026-04-29

### Added
- **p4minishell.h**: Public API header declaring host bridge callbacks and shared shell utilities
- **p4minishell.c**: Extracted `shell_networking_*` bridge functions from main.c into dedicated module
- **Modular bridge layer**: Networking bridge functions now live in `main/p4minishell.c`, callable from `components/networking`

### Changed
- **main/main.c**: Added `#include "p4minishell.h"` and `#include "p4minishell_config.h"`; networking bridge functions changed from `static` to public linkage
- **main/CMakeLists.txt**: Added `p4minishell.c` to SRCS
- **main/p4minishell.c**: Rewritten as thin bridge layer with extern forward declarations to main.c shell functions

### Architecture Note
- c6ota and usb host bridge functions remain in main.c due to ESP-IDF component model requirements (cross-component linking needs app-component residency)
- networking bridge functions extracted to p4minishell.c since networking component links against main transitively

---

## [0.3.0] - 2026-04-29

### Added
- **Centralized configuration system**: All hardcoded values moved to `p4minishell_config.h` with companion YAML documentation in `p4minishell_config.yaml`
- **Config header**: Single C header with all tunable values organized by subsystem (shell identity, buffers, UI layout, Wi-Fi, Bluetooth, SD card, batch engine, GPIO, header visuals, USB host, C6 OTA, task stacks)
- **Config YAML**: Machine-readable documentation with value, type, description, and valid range for every configurable parameter
- **Backward-compatible aliases**: All existing `SHELL_*`, `NETWORKING_*`, `BLUETOOTH_*`, `HEADER_*`, `C6OTA_*`, and `USB_*` macros preserved as aliases to `P4_CONFIG_*` equivalents

### Changed
- **main/main.c**: Replaced 40+ inline `#define` macros with `#include "p4minishell_config.h"` plus backward-compatibility aliases
- **components/networking/networking.c**: Moved all `#define` values to config header
- **components/networking/bluetooth.c**: Moved all `#define` values to config header
- **components/header/header.c**: Moved color and sizing defines to config header
- **components/c6ota/c6ota.c**: Moved OTA parameters to config header
- **components/usb/usb.c**: Moved USB host parameters to config header; HID key codes kept local (standard USB HID usage table)
- **All component CMakeLists.txt**: Added `${CMAKE_SOURCE_DIR}` to `INCLUDE_DIRS` for config header access

---

## [0.2.0] - 2026-04-29

### Changed
- **Documentation overhaul**: Rewrote all project documentation files (`readme.md`, `documentation.md`, `ai-context.md`, `command.md`, `roadmap.md`, `licence.md`, `API.md`, `SDK.md`) with consistent structure, comprehensive detail, and proper Markdown formatting
- **Source code comments**: Replaced all `// AI:` prefixed comments with proper Doxygen-style documentation throughout `main/main.c`, all component headers, and `board_config.h`
- **Header files**: Added full `@file` Doxygen blocks with architecture descriptions, parameter documentation, and behavioral contracts to `header.h`, `networking.h`, `bluetooth.h`, `c6ota.h`, and `usb.h`
- **board_config.yaml**: Restructured with clear section headers, detailed pin descriptions, and validated hardware metadata

---

## [0.1.24] - 2026-03-18

### Added
- New `components/header` module: fixed LVGL top bar for notifications plus passive Wi-Fi, battery, Bluetooth, USB, and SD status indicators
- Live header notifications from Wi-Fi, USB, Bluetooth, and `c6ota` event paths
- Resolution-scaled header height with clamped minimum/maximum

### Fixed
- SD card status icon now appears consistently with same size, color, and alignment as other status icons
- Header notification area stays blank when idle, shows only during active module events
- SD mount/unmount state changes now trigger immediate header icon updates

### Changed
- Header status indicators use guaranteed-visible retro ASCII labels instead of LVGL symbol glyphs
- Header is non-scrollable with left-to-right status icons and notification area on far right

---

## [0.1.23] - 2026-03-18

### Added
- New `components/usb` module: ESP-IDF USB Host bring-up for MSC external storage and HID keyboard/mouse
- Shell-facing `usb` command family: `usb status`, `usb ls [path]`, `usb keyboard <on|off>`, `usb mouse <on|off>`
- USB MSC storage mounted at `/usb0` via VFS/FATFS with transcript-friendly output
- Managed component dependencies: `espressif/usb_host_msc`, `espressif/usb_host_hid`

---

## [0.1.22] - 2026-03-18

### Changed
- **c6ota refactored** into `components/c6ota` with identical public API and behavior
- Full ESP32-C6 OTA flow moved out of `main/main.c` while preserving confirmation flow, transcript output, source handling, hosted OTA RPC sequence, and Wi-Fi stop/restore
- Added `API.md` and `SDK.md` documenting the stable `c6ota_init`, `c6ota_perform`, and `c6ota_register_progress_callback` integration surface

---

## [0.1.21] - 2026-03-18

### Added
- Hosted NimBLE Bluetooth on ESP32-C6 over ESP-Hosted VHCI in `components/networking/bluetooth.c`
- Shell commands: `bluetooth status`, `bluetooth scan`, `bluetooth advertise <on|off>`, with `bt` alias
- Stateful Bluetooth lifecycle: scan/advertise reuse active hosted controller session

### Changed
- **Networking refactored** into `components/networking`: Wi-Fi runtime state, hosted startup, OTA restore hooks, and Bluetooth handling no longer live in `main/main.c`
- Preserved all existing hosted Wi-Fi behavior and command flow through new module APIs
- Replaced disabled hosted Bluedroid stub with hosted NimBLE

---

## [0.1.20] - 2026-03-18

### Added
- **Serial console bridge**: ESP-IDF UART/USB-Serial-JTAG monitor now accepts shell commands
- stdin lines routed through existing shell worker, transcript, masking, and history flow
- stdout mirrors transcript output

### Fixed
- Monitor write-timeout caused by firmware not consuming interactive serial input
- Serial prompt loop no longer floods `P4Shell>` during idle stdin polling

---

## [0.1.19] - 2026-03-18

### Fixed
- **Command-family dispatch regression**: `wifi status|scan|diag|connect|disconnect` now work correctly after boot
- Root cause: parser now preserves original unsplit command text before tokenization so family handlers receive full command line
- Same fix applied to `sd` and `c6ota` family handlers

---

## [0.1.18] - 2026-03-17

### Changed
- Disabled hosted Bluedroid Bluetooth path after `bt enable` caused board crashes in HCI parser
- `bt` command surface kept visible but returns explicit unsupported state
- Removed direct host BT build dependency

---

## [0.1.17] - 2026-03-17

### Changed
- `gpio list` and `gpio status` now report pins with clearer board-role text
- `rgb` and `camera` messages updated to honestly explain current hardware gaps

### Added
- Hosted Bluedroid Bluetooth path enabled in host build with `bt status|enable|scan`

---

## [0.1.16] - 2026-03-17

### Added
- Hardware control commands: `brightness <0-100>`, `rotate <0|90|180|270>`, `battery`, `volume <0-100>`
- `gpio list|status|read <pin>|set <pin> <0|1>` with write restrictions
- Runtime display rotation with GT911 touch remapping
- ADC-backed battery reporting using board-configured divider values
- ES8311 speaker volume control through BSP codec path
- Parser-visible `bt`, `rgb`, and `camera` families with sdkconfig/metadata gates

---

## [0.1.15] - 2026-03-17

### Added
- `roadmap.md`: parity plan for COMMAND.COM features, native app loading, shell SDK
- `licence.md`: proprietary notice for project-authored code plus third-party license summary

### Changed
- README rewritten to describe P4MiniShell as an embedded DOS-style shell platform

---

## [0.1.14] - 2026-03-17

### Added
- COMMAND.COM-style SD workflow: `cd`/`chdir`, `dir`, `copy`, `move`, `del`/`erase`, `ren`/`rename`, `md`/`mkdir`, `rd`/`rmdir`, `type`, `write`, `append`, `touch`
- RAM-only environment variables: `set`, `path`, `echo`
- Batch file engine: `.bat` execution with `%1`..`%9` expansion, `rem` comments, `echo on/off`, PATH-based lookup
- SD-backed output redirection: `>` and `>>` for text-producing commands

---

## [0.1.13] - 2026-03-17

### Changed
- Finalized ESP-Hosted profile: `espressif/esp_hosted 2.12.1` + `espressif/esp_wifi_remote 1.4.1`
- 1-bit SDIO at 10 MHz on CLK=18 CMD=19 D0=14 D1=15 D2=16 D3=17
- Forced ESP32-C6 reset on every host boot through GPIO54
- 1500-byte `c6ota` transfer chunks

### Fixed
- End-to-end validation: shell UI, BSP display/touch, hosted Wi-Fi, SD tools, and `c6ota` all working together

---

## [0.1.12] - 2026-03-17

### Fixed
- Restored `CONFIG_ESP_HOSTED_SLAVE_RESET_ON_EVERY_HOST_BOOTUP` after Wi-Fi failures
- Restored boot-time and post-`c6ota` Wi-Fi diagnostic pass
- Removed app-side hosted log suppression

---

## [0.1.11] - 2026-03-17

### Fixed
- Reverted `ESP_HOSTED_EVENT_TRANSPORT_UP` wait experiment that regressed Wi-Fi startup
- Restored prior hosted startup order: connect C6, validate firmware version, continue Wi-Fi path

---

## [0.1.10] - 2026-03-17

### Changed
- Removed automatic `wifi diag` scan from boot-time startup and post-`c6ota` restore
- Restored hosted reset policy to `CONFIG_ESP_HOSTED_SLAVE_RESET_ONLY_IF_NECESSARY`
- Suppressed non-actionable `H_SDIO_DRV` and `rpc_rsp` warning noise

---

## [0.1.9] - 2026-03-17

### Added
- **ESP-Hosted firmware compatibility gate**: after `esp_hosted_connect_to_slave()`, shell reads C6 hosted version and refuses Wi-Fi init unless co-processor matches host `2.12.x` line
- Transcript-visible recovery guidance on version mismatch

### Fixed
- SDIO/RPC fallout from mismatched host/co-processor firmware
- Host component lock restored to `espressif/esp_hosted 2.12.1` and `espressif/esp_wifi_remote 1.4.1`

---

## [0.1.8] - 2026-03-17

### Added
- `wifi diag` command: connection state, IP status, nearby-network scan
- Transcript-facing Wi-Fi diagnostics on boot and post-`c6ota`

### Fixed
- Wi-Fi runtime retries: partial init state cleaned up before retrying
- Hosted Wi-Fi restores automatically after normal boot and successful `c6ota` in background task

---

## [0.1.7] - 2026-03-17

### Added
- FATFS long filename support: heap-backed LFN buffers, 255-character limit
- `sd ls` uses direct FatFs directory enumeration for reliable long filenames

### Fixed
- `sd ls` stack-protection panic: command execution moved to dedicated worker task
- `c6ota default` hosted teardown crash: ESP-Hosted SDIO transport kept alive for Wi-Fi-off OTA
- Truncated SD root names and `c6ota default` lookup failures

---

## [0.1.6] - 2026-03-17

### Fixed
- Repeated SD `ldo` warning spam: BSP SD-card power control acquires SD VO4 explicitly at 3300 mV on esp32p4
- Mount-failure and unmount cleanup so repeated `sd` commands don't leak SD power handle

---

## [0.1.5] - 2026-03-17

### Added
- `sd info`: card metadata and root availability
- `sd stat <path>`: resolved path, entry type, size, and mode
- `sd cat <path> [max_bytes]`: bounded text-safe file preview (max 8192 bytes)

### Changed
- All SD commands use shared guarded mount/unmount flow with validated path resolution
- `sd ls` shows entry types and file sizes
- Directory listings bounded to 128 entries

---

## [0.1.4] - 2026-03-17

### Added
- `c6ota default`: auto-load `esp32c6_hosted_slave.bin` or `network_adapter.bin` from SD root
- ESP32-C6 image validation: magic `0xE9` + chip ID `0x000D`
- Factory first-upgrade warning for C6 firmware `v2.3.0`

### Changed
- `c6ota` transfer chunks: 1536 bytes
- Progress output: `C6 OTA: XX% (YYYY KB / ZZZZ KB)` every 5%
- Confirmation prompt: `WARNING: This will reboot the C6. Type YES to continue`
- Success text: `C6 OTA completed successfully! Type reboot to activate new firmware.`
- HTTP images downloaded first, then Wi-Fi stopped for clean SDIO-only OTA transfer

---

## [0.1.3] - 2026-03-17

### Changed
- Healthy shell UI startup no longer emits warning-level log
- Boot milestone preserved in `debug` command history instead

---

## [0.1.2] - 2026-03-17

### Removed
- `c6update` utility fully removed and archived (previously used esp-serial-flasher + GPIO54)

---

## [0.1.1] - 2026-03-14

### Added
- `c6ota <source>` shell command: ESP-Hosted SDIO OTA for ESP32-C6
- Input-driven safety gate with `This will reboot the C6. Continue? (yes/no)` prompt
- ESP-IDF app header validation before transfer
- Live percentage progress in locked transcript UI
- Support for `sd:/firmware.bin` and `http[s]://host/path/to/firmware.bin` sources

---

## [0.1.0] - 2026-03-13

### Added
- Initial shell UI replacing LVGL widgets demo
- BSP-managed JD9165 display and GT911 touch initialization
- Scrollable LVGL textarea transcript, on-screen keyboard, boot banner
- Built-in commands: `help`, `sysinfo`, `clear`, `reboot`
- 10-command recall buffer with Prev/Next touch controls
- Runtime Wi-Fi initialization path following sdkconfig
- Shell Wi-Fi commands: `wifi status`, `wifi connect`, `wifi disconnect`
- Password masking for `wifi connect <ssid> <pass>`
- `wifi scan`, `sd ls`, `mem`, `gpio status`, `debug`, `version`, `about`
- 5-entry debug/error history buffer
- ESP-Hosted + esp_wifi_remote targeting ESP32-C6 over SDIO
- `coprocessor/esp32c6_slave`: repo-local ESP32-C6 hosted slave firmware project
- Station-only Wi-Fi profile, nano newlib, warn-level logging for image size
- PSRAM XIP mapping disabled to prevent flash/PSRAM overflow at link

## [0.1.19] - 2026-03-18
- Fixed the shell command-family dispatch regression that left `wifi status`, `wifi scan`, `wifi diag`, `wifi connect`, and `wifi disconnect` effectively inert even though boot-time hosted Wi-Fi still initialized and connected correctly
- Fixed the root cause in the shell parser by preserving the original unsplit command text before tokenization, so family handlers that re-parse subcommands now receive the full command line instead of only the first token
- Applied the same command-routing fix to the `sd` and `c6ota` family handlers so their subcommand parsing stays reliable without changing the proven boot, display, hosted Wi-Fi, or OTA runtime paths

## [0.1.18] - 2026-03-17
- Disabled the earlier hosted Bluedroid Bluetooth bring-up path on the ESP32-C6 baseline after `bt enable` proved able to crash the board inside the Bluedroid HCI parser during controller startup
- Kept the `bt` command surface visible, but changed it back to an explicit unsupported state on this current ESP32-C6 hosted configuration so boot, display, SD, and Wi-Fi remain stable
- Removed the direct host BT build dependency and hard-gated the shell's Bluetooth runtime path so `bt enable` and `bt scan` now fail safely instead of entering the unstable controller startup path

## [0.1.17] - 2026-03-17
- Tightened `gpio list` and `gpio status` so the shell now reports the exposed board pins with clearer JC1060 and ESP32-P4 role text instead of terse raw labels
- Enabled the hosted Bluedroid Bluetooth path in the host build, added the required BT component dependency, and completed the shell-side `bt status | enable | scan` runtime helpers against the local ESP-Hosted example flow
- Kept `rgb` and `camera` intentionally blocked, but updated those shell messages to explain the current evidence more honestly: the JC1060 reference repo does not expose authoritative RGB LED wiring, and this workspace still lacks the local camera stack needed by the JC1060 camera examples

## [0.1.16] - 2026-03-17
- Expanded the shell with hardware control commands for `brightness`, `rotate`, `battery`, `volume`, and the safer `gpio list | status | read | set` flow while preserving the existing BSP boot path, locked transcript UI, SD tools, Wi-Fi restore flow, and `c6ota` behavior
- Added runtime display rotation with GT911 touch remapping, ADC-backed battery reporting using board-configured divider values, and ES8311 speaker volume control through the existing BSP codec path
- Surfaced `bt status | enable | scan`, `rgb`, and `camera` in the parser and help output with explicit sdkconfig or board-metadata gates so unsupported hardware paths fail clearly instead of pretending support on the current workspace baseline

## [0.1.15] - 2026-03-17
- Rewrote the README introduction to describe P4MiniShell as an embedded ESP32-P4 and ESP32-C6 DOS-style shell platform instead of a minimal demo replacement
- Added `roadmap.md` to capture the missing work for COMMAND.COM parity, native app loading, a future shell SDK and API, and the separate design decision needed for literal DOS `.exe` compatibility
- Added `licence.md` to mark the project-authored code as proprietary to Stoian Alexandru while preserving the verified third-party Apache, MIT, and protobuf-c license obligations already present in the workspace

## [0.1.14] - 2026-03-17
- Expanded the shell toward a COMMAND.COM-style SD workflow with RAM-only `cd`/`chdir`, `dir`, `copy`, `move`, `del`/`erase`, `ren`/`rename`, `md`/`mkdir`, `rd`/`rmdir`, `type`, `write`, `append`, `touch`, `set`, `path`, `echo`, and `call`, while keeping all execution on the existing shell worker task
- Added SD-backed redirection for transcript-safe text commands using `>` and `>>`, with writes confined to the guarded SD mount path and no filesystem writes outside the SD card
- Added a lightweight batch engine for `.bat` files on SD, including `%1`..`%9` argument expansion, `rem` comments, `echo on/off`, PATH-based batch lookup, and direct `.bat` invocation through the normal shell dispatcher

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