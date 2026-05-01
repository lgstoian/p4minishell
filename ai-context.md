# P4MiniShell AI Context Rules

## Project Identity
- **Name**: P4MiniShell
- **Type**: Embedded DOS-style command shell
- **Target**: ESP32-P4 (host) + ESP32-C6 (co-processor over ESP-Hosted SDIO)
- **Framework**: ESP-IDF v6.0.1
- **UI**: LVGL 9.2.2 with JD9165 1024x600 display + GT911 touch

## Mandatory Reading Before Any Change
1. changelog.md - version history and recent changes
2. readme.md - project overview and current behavior
3. ai-context.md - this file (project rules)
4. board_config.yaml - hardware configuration
5. p4minishell_config.h - centralized config values
6. p4minishell_config.yaml - config documentation
7. command.md - command reference
8. sdkconfig - current build configuration
9. main/idf_component.yml - component dependencies
10. For roadmap work: also read roadmap.md and licence.md

## Configuration Rules
- ALL tunable values MUST live in p4minishell_config.h, never hardcoded in source
- p4minishell_config.yaml MUST be updated whenever a config value changes
- Use P4_CONFIG_* macros for new code; SHELL_* aliases exist for backward compatibility
- USB HID key codes are standard USB HID usage table values and stay local to usb.c
- board_config.h remains the hardware-pin source of truth (GPIO assignments, display timing)
- sdkconfig remains the ESP-IDF build-configuration source of truth

## Source Code Rules

### Comments
- All major sections need descriptive comments explaining purpose and behavior
- Use Doxygen-style @file, @brief, @param for public APIs
- Section markers should describe what follows, not just repeat function names
- NEVER use `// AI:` prefix in comments; use plain descriptive text
- Existing `// AI:` comments must be replaced with equivalent descriptive text

### Audit and Hardening Rules
- Before every release: scan all source files for `// AI:` comments and replace them
- Build must produce ZERO errors and ZERO warnings on target toolchain
- No build artifacts in repository; .gitignore must cover build/, sdkconfig, and generated files
- All hardcoded strings must reference p4minishell_config.h macros
- Public API functions must have Doxygen documentation in their header files
- Cross-component calls must go through documented bridge functions (p4minishell.h)
- Semaphore/mutex acquisition must always have a corresponding release path
- Buffer operations must bounds-check before memcpy/snprintf
- NULL pointer checks required before dereference in all public API functions
- All `lv_async_call` return values must be checked; free payload on failure
- All `lv_textarea_get_text()` return values must be NULL-checked before use
- Forward declarations without implementations are a critical bug; never commit them

### Header Bar Rules
- All `header_update_*()` functions MUST update internal state immediately before scheduling async render
- Header state writes (bool/int) are atomic on this platform and safe from any task context
- If LVGL async dispatch fails (lv_async_call returns error), fall back to synchronous header_render()
- If allocation fails for the async payload, fall back to synchronous header_render()
- SD indicator uses consistent HEADER_SD_SYMBOL in both mounted and unmounted states
- SD indicator shows persistent state (NO/INS/ON/ERR)
- Battery is ALWAYS visible — shows "BAT N/C" with muted styling when ADC is not connected
- System panel (MEM | CPU | BAT) is on the far right, all dynamically linked to FreeRTOS runtime stats
- header_update_battery(int percent, bool adc_ready) — pass adc_ready=false for N/C display
- header_update_mem(uint32_t free_heap, uint32_t total_heap) — real-time from heap_caps
- header_update_cpu(int percent, uint32_t task_count) — real-time from FreeRTOS runtime stats
- header_update_uptime(uint32_t seconds) — system uptime from esp_timer_get_time()
- Touch init failure must not prevent header rendering (BSP touch is optional)

### Shell Core Rules
- ALL transcript output MUST go through `shell_transcript_append_text()` / `shell_transcript_appendf()` from `components/shell/`
- ANSI-colored transcript output MUST go through `shell_transcript_append_ansi()` / `shell_transcript_appendf_ansi()` using `@`-prefixed format specifiers
- ANSI color palette is defined in `p4minishell_config.h` via `P4_CONFIG_ANSI_*` macros; never hardcode ANSI color values
- ALL debug logging MUST use `shell_record_errorf()` / `shell_record_warningf()` / `shell_record_infof()`
- Command history MUST use `shell_store_command_history()` / `shell_recall_history()`
- UART console MUST use `shell_uart_console_start()` / `shell_uart_console_write_text()`
- System info commands (help, sysinfo, version, about, mem, debug) live in `components/shell/`

### Command Rules
- ALL command dispatch MUST go through `shell_execute_command()` / `shell_execute_command_core()` from `components/command/`
- Command execution runs on a dedicated worker task to protect the LVGL stack
- Module-routed commands (wifi, bluetooth, usb, c6ota) are dispatched from command.c

### Keyboard Manager Rules
- ALL keyboard operations MUST go through `components/keyboard/` — never call LVGL keyboard APIs directly from main.c
- `keyboard_init()` MUST be called after `display_init()` and from the LVGL task context
- Keyboard visibility changes trigger automatic UI reflow via the window manager callback
- When keyboard is hidden, the transcript area expands to fill the freed space
- Keyboard height is configurable via `P4_CONFIG_KEYBOARD_*` macros
- Keyboard modes (text_lower, text_upper, number, symbols) are managed by the keyboard component

### Window Manager Rules
- ALL LVGL screen layout MUST go through `components/windows/` — never create screen-level widgets directly in main.c
- `windows_init()` MUST be called after `display_init()` and from the LVGL task context (inside `bsp_display_lock`)
- `windows_deinit()` MUST be called before rebuilding the UI after rotation changes
- Window objects MUST be accessed via `windows_get_*()` accessors, never stored as static variables in main.c
- All window region dimensions MUST use config macros (`P4_CONFIG_WINDOW_*`), never hardcoded
- Colors MUST be accessed through `windows_get_color()` with semantic names, never raw hex values
- The window manager delegates header rendering to `components/header/` via `header_init()`/`header_deinit()`
- The window manager queries display resolution from `components/display/` via `display_get_width()`/`display_get_height()`

### Display and Touch
- Display init MUST use `display_init()` (which wraps `bsp_display_start_with_config()` with BOARD_CFG_* values)
- ALL display operations MUST go through `components/display/` public API — never call BSP display functions directly from main.c
- Touch init MUST use the managed GT911 driver through BSP (handled internally by display manager)
- Never replace the BSP display/touch path with raw driver calls
- After `display_set_rotation()`, the display manager schedules a UI rebuild via the registered callback
- `shell_build_ui()` MUST call `header_deinit()` before `lv_obj_clean()` for clean header re-init
- Touch rotation mapping is handled automatically by the display manager on rotation change
- Keyboard height scales dynamically (~35% of vertical resolution, clamped 180-280px)
- Input row height scales dynamically (~8% of vertical resolution, clamped 40-56px)
- Header height is rotation-aware (uses correct resolution axis for 90/270)
- Display state variables (rotation, brightness, power) are owned by the display manager and accessed through its public API
- `display_register_ui_rebuild_callback()` MUST be called after `display_init()` to register the shell's UI rebuild function
- The display manager's `display_info_t` struct is the canonical source for sysinfo display diagnostics

### Wi-Fi Rules
- Wi-Fi startup MUST follow sdkconfig only
- Auto-start in background task on normal boot
- Restore after successful c6ota
- ESP-Hosted + esp_wifi_remote for C6 SDIO path
- Version compatibility gate: refuse init if C6 firmware major/minor != host 2.12.x
- NVS initialized before esp_wifi_init() with erase-and-retry recovery
- Station-only profile; no SoftAP, WPA3, or enterprise
- Password masking in transcript and command history
- ALL shared Wi-Fi state (s_wifi_state, s_wifi_connected, s_wifi_target_ssid, etc.) MUST be accessed through wifi_lock()/wifi_unlock() mutex
- Wi-Fi init task claiming MUST use wifi_try_claim_init_task()/wifi_release_init_task() (TOCTOU-safe atomic)
- Persistent watchdog (networking_wifi_watchdog_task) retries disconnected Wi-Fi with exponential backoff (1s→30s cap, 120s total timeout)
- Watchdog starts automatically on WIFI_EVENT_STA_DISCONNECTED; stops on successful connection or timeout

### Bluetooth Rules
- Hosted NimBLE on C6 over ESP-Hosted VHCI (not Bluedroid)
- Stateful lifecycle: enable once, reuse for scan/advertise
- bt is alias for bluetooth
- Supported: status, scan, advertise on|off

### SD Card Rules
- All SD commands use shared guarded mount/unmount path
- FATFS LFN enabled with heap-backed buffers, MAX_LFN=255, UTF-8 encoding
- Bounded output: 128 entries max for listings, 8192 bytes max for sd cat
- SD VO4 LDO explicitly acquired at 3300 mV before mounts
- Path resolution handles sd:/, /sdcard/, and relative paths

### USB Rules
- USB Host Library with MSC and HID class drivers
- MSC mounts at /usb0 via VFS/FATFS
- HID echo is opt-in (keyboard/mouse on/off)
- Follows same bounded transcript style as SD commands
- USB keyboard auto-detect: automatically hides on-screen keyboard when USB keyboard attached
- USB keystrokes injected into shell CLI input line via shell_usb_keyboard_input() bridge
- Full US keyboard layout supported (60+ HID key codes with modifier-aware mapping)
- Auto-detect runs in periodic header refresh timer; state transitions trigger notifications
- keyboard_set_external_input() / keyboard_clear_force_visible() for manual control

### OTA Rules (c6ota)
- Lives in components/c6ota with stable public API
- Exact confirmation: WARNING: This will reboot the C6. Type YES to continue
- Validates ESP-IDF app magic 0xE9 + ESP32-C6 chip ID 0x000D
- 1500-byte chunks, progress every 5%
- Wi-Fi stopped before transfer, kept alive during (no esp_hosted_deinit())
- Factory v2.3.0 needs one-time standalone tool first

### Command Execution
- Heavy commands run on dedicated worker task (not LVGL input callback stack)
- Preserve original unsplit command text for family handlers (wifi, sd, c6ota)
- LV_EVENT_READY on input line is the confirmed submission path
- Serial console reuses same shell path (stdin to submit, stdout from transcript)

### Shell State
- RAM-only: current working directory, environment variables, PATH, batch args
- No persistence across boots
- Environment variables: max 24, names alphanumeric + underscore
- Batch depth: max 4 nested calls

### Header Bar
- Passive, display-only module in components/header
- Non-scrollable, resolution-scaled height
- Status icons: Wi-Fi, battery, Bluetooth, USB, SD
- SD icon hidden when no card mounted
- Notification area transient (blank when idle)

### Build Constraints
- LVGL examples MUST stay disabled (image budget)
- Station-only Wi-Fi profile
- Newlib nano formatting
- Warn-level compile-time logging
- PSRAM XIP instruction/rodata mapping MUST stay disabled
- ESP-Hosted reset: SLAVE_RESET_ON_EVERY_HOST_BOOTUP

### Documentation Updates
After every task, update: changelog.md, readme.md, ai-context.md, board_config.yaml, command.md

### Hardware Gaps (Do NOT implement)
- RGB LED: no authoritative wiring in JC1060 reference
- Camera: no local camera stack in workspace
- Both should fail explicitly with honest messages

### Error Handling
- Friendly transcript messages for all failures
- 5-entry debug history buffer via debug command
- Healthy boot recorded in debug history, not as warning
- ESP-IDF error names in messages: esp_err_to_name()
