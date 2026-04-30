# Hosted Module API

This document describes the public integration surface exposed by the hosted runtime modules under `components/`.

## Shared integration pattern
- `main/main.c` remains the shell UI, transcript, parser, and orchestration layer.
- `components/ansi` owns the ANSI/VT escape sequence processing: SGR color palette, format string builder, text processing.
- `components/display` owns all display hardware state: rotation, resolution, refresh rate, brightness, power management, and touch handle.
- `components/windows` owns the LVGL screen layout: named regions, dynamic scaling, rotation-aware layout, and consistent styling.
- `components/header` owns the fixed top-bar UI for notifications and passive status display.
- `components/networking` owns hosted Wi-Fi runtime state and also bootstraps the hosted Bluetooth module.
- `components/usb` owns USB Host Library state, USB MSC storage, and USB HID keyboard or mouse debug behavior.
- `components/c6ota` owns the shell-visible ESP32-C6 OTA workflow and depends on `components/networking` for Wi-Fi wait and restore hooks.
- All modules keep user-visible behavior in the shell transcript or fixed status header instead of returning rich status objects to the caller.

## ANSI/VT Module API

The ANSI module (`components/ansi/`) provides SGR (Select Graphic Rendition) escape sequence processing for colored terminal output. It owns the 16-color palette, format string builder, and ANSI text processing state machine.

### Lifecycle
- `void ansi_init(void)` — Initialize the color palette from `p4minishell_config.h`. Called once during `shell_init()`.
- `bool ansi_is_initialized(void)` — Check if the ANSI module is initialized.

### Color Palette
- `uint32_t ansi_get_palette_color(ansi_color_index_t index)` — Get a palette color by index.
- `uint32_t ansi_get_default_fg(void)` / `uint32_t ansi_get_default_bg(void)` — Get default foreground/background colors.
- `void ansi_set_palette_color(ansi_color_index_t index, uint32_t color)` — Modify a palette entry at runtime.

### Format String Builder
- `int ansi_format(char *dst, size_t dst_size, const char *format, ...)` — Build an ANSI-formatted string with `@`-prefixed color/attribute specifiers.
- `int ansi_vformat(char *dst, size_t dst_size, const char *format, va_list args)` — Variadic version.

### Text Processing
- `void ansi_process_text(const char *text, ansi_segment_fn_t segment_fn, void *user_data)` — Parse ANSI escape sequences and emit plain-text segments with style state.
- `int ansi_strip_to_plain(char *dst, size_t dst_size, const char *src)` — Strip all ANSI escape sequences, returning plain text only.
- `bool ansi_contains_escapes(const char *text)` — Check if text contains ANSI escape sequences.

### Quick Formatters
- `int ansi_fg_text(char *dst, size_t dst_size, int fg_code, const char *text)` — Wrap text in foreground color SGR codes.
- `int ansi_fg_bg_text(char *dst, size_t dst_size, int fg_code, int bg_code, const char *text)` — Wrap text in foreground + background SGR codes.
- `int ansi_attr_text(char *dst, size_t dst_size, int attr_code, const char *text)` — Wrap text in attribute SGR codes.

### Shell Integration
- `void shell_transcript_append_ansi(const char *text)` — Append ANSI-formatted text to transcript (strips ANSI for LVGL, passes through to UART).
- `void shell_transcript_appendf_ansi(const char *format, ...)` — Append printf-style ANSI-formatted text to transcript.

### ANSI Format Specifiers
| Specifier | SGR Code | Meaning |
|-----------|----------|---------|
| `@R` | 0 | Reset all attributes |
| `@B` | 1 | Bold on |
| `@D` | 2 | Dim on |
| `@I` | 3 | Italic on |
| `@U` | 4 | Underline on |
| `@k` | 30 | Foreground black |
| `@r` | 31 | Foreground red |
| `@g` | 32 | Foreground green |
| `@y` | 33 | Foreground yellow |
| `@b` | 34 | Foreground blue |
| `@m` | 35 | Foreground magenta |
| `@c` | 36 | Foreground cyan |
| `@w` | 37 | Foreground white |
| `@K` | 90 | Foreground bright black |
| `@Rr` | 91 | Foreground bright red |
| `@G` | 92 | Foreground bright green |
| `@Y` | 93 | Foreground bright yellow |
| `@L` | 94 | Foreground bright blue |
| `@M` | 95 | Foreground bright magenta |
| `@C` | 96 | Foreground bright cyan |
| `@W` | 97 | Foreground bright white |

## Window Manager API

The window manager (`components/windows/`) is the central layout controller for the LVGL shell UI. It owns the screen region partitioning, dynamic scaling, and consistent styling. All LVGL screen-level widgets are created and owned by this module.

### Lifecycle
- `esp_err_t windows_init(void)` — Build all UI windows on the LVGL screen. Must be called after `display_init()`.
- `void windows_deinit(void)` — Tear down all windows. Calls `header_deinit()` internally.
- `bool windows_is_initialized(void)` — Check if the window manager is initialized.

### Window Object Accessors
- `lv_obj_t *windows_get_transcript(void)` — Scrollable command output textarea
- `lv_obj_t *windows_get_input_line(void)` — Single-line command entry textarea
- `lv_obj_t *windows_get_keyboard(void)` — On-screen LVGL keyboard
- `lv_obj_t *windows_get_prev_button(void)` — Previous history button
- `lv_obj_t *windows_get_next_button(void)` — Next history button
- `lv_obj_t *windows_get_input_row(void)` — Input row container
- `lv_obj_t *windows_get_screen(void)` — Active LVGL screen

### Dimension & Scaling
- `lv_coord_t windows_get_display_width(void)` — Current display width from display.c
- `lv_coord_t windows_get_display_height(void)` — Current display height from display.c
- `lv_coord_t windows_scale_height_percent(int pct, lv_coord_t min, lv_coord_t max)` — Scale height as percentage of display height
- `lv_coord_t windows_scale_width_percent(int pct, lv_coord_t min, lv_coord_t max)` — Scale width as percentage of display width
- `window_rect_t windows_get_rect(window_region_t region)` — Get bounding rectangle for a named region

### Styling
- `lv_color_t windows_get_color(const char *name)` — Get color by semantic name (`bg_screen`, `bg_transcript`, `bg_input_row`, `bg_keyboard`, `text`, `text_muted`)
- `const lv_font_t *windows_get_terminal_font(void)` — Get the terminal font

### Helpers
- `void windows_show_boot_banner(const char *message)` — Show boot message in transcript
- `void windows_reset_input_line(const char *prompt)` — Reset input line to prompt

### Thread Safety
All LVGL object creation/destruction must happen on the LVGL task. Public accessors return raw LVGL object pointers — callers must use from LVGL task context or via `lv_async_call`.

## Display API

The display manager (`components/display/`) is the central controller for all display hardware. It owns rotation, resolution, refresh rate, brightness, power state, and touch handle management. All display-related shell commands route through this module.

### Initialization & Lifecycle

- `esp_err_t display_init(void)`
  - Initialize the display manager and physical display hardware.
  - Wraps `bsp_display_start_with_config()` with `BOARD_CFG_*` values.
  - Turns on backlight by default. Returns `ESP_FAIL` if display init fails.

- `void display_deinit(void)`
  - Deinitialize the display manager (resets internal state tracking).
  - Does NOT power off the display hardware.

- `bool display_is_initialized(void)`
  - Returns true if `display_init()` completed successfully.

- `lv_display_t *display_get_lvgl_handle(void)`
  - Get the LVGL display handle for direct LVGL operations.

- `void *display_get_touch_handle(void)`
  - Get the touch handle for direct touch operations.

- `void display_register_ui_rebuild_callback(void (*rebuild_fn)(void))`
  - Register a callback invoked via `lv_async_call` after rotation changes.
  - The shell registers `shell_build_ui()` here so the display manager can trigger full UI rebuilds.

### Rotation Control

- `display_rotation_t display_get_rotation(void)`
  - Get the current display rotation (0, 90, 180, or 270).

- `esp_err_t display_set_rotation(display_rotation_t rotation)`
  - Apply a new display rotation. Triggers LVGL software rotation, touch controller remapping, and schedules UI rebuild via registered callback.

- `esp_err_t display_rotation_parse(const char *str, display_rotation_t *rotation_out)`
  - Parse a rotation string ("0", "90", "180", "270") into `display_rotation_t`.

- `const char *display_rotation_to_string(display_rotation_t rotation)`
  - Get the rotation as a human-readable string.

- `lv_display_rotation_t display_rotation_to_lvgl(display_rotation_t rotation)`
- `display_rotation_t display_rotation_from_lvgl(lv_display_rotation_t lvgl_rotation)`
  - Convert between display manager and LVGL rotation enums.

### Resolution

- `display_resolution_t display_get_resolution(void)`
  - Get current effective resolution accounting for rotation.

- `display_resolution_t display_get_native_resolution(void)`
  - Get native panel resolution (1024x600).

### Refresh Rate

- `display_refresh_config_t display_get_refresh_config(void)`
  - Get current refresh rate configuration (estimated ~60 Hz from panel timing).

- `esp_err_t display_set_refresh_rate(uint32_t target_hz)`
  - Set target refresh rate. Returns `ESP_ERR_NOT_SUPPORTED` on JD9165 panel (fixed timing).

### Brightness

- `int display_get_brightness(void)`
  - Get current backlight brightness percentage (0-100).

- `esp_err_t display_set_brightness(int percent)`
  - Set backlight brightness via BSP PWM path. Returns `ESP_ERR_INVALID_ARG` if out of range.

### Power Management

- `display_power_state_t display_get_power_state(void)`
  - Get current display power state (on/sleep/off).

- `esp_err_t display_set_power_state(display_power_state_t state)`
  - Set display power state. Controls backlight on/off.

- `esp_err_t display_sleep(void)` / `esp_err_t display_wake(void)`
  - Convenience functions for sleep/wake transitions.

### Display Info & Diagnostics

- `display_info_t display_get_info(void)`
  - Get comprehensive display information (resolution, rotation, refresh, brightness, timing, buffer config, panel/touch driver names).

- `void display_print_info(void (*print_fn)(const char *format, ...))`
  - Print formatted display info through a caller-provided print function (e.g., `shell_transcript_appendf`).

### Thread Safety

All display manager state is protected by a `portMUX_TYPE` spinlock. Public API functions use `portENTER_CRITICAL`/`portEXIT_CRITICAL` for atomic access. LVGL operations are dispatched via `lv_async_call` when called from non-LVGL task contexts.

## Header API

All `header_update_*()` functions are **safe to call from any task context** (LVGL task, shell worker, timer callback, interrupt handler). They:
1. Update internal state immediately (atomic bool/int writes)
2. Schedule an LVGL async render callback
3. Fall back to synchronous `header_render()` if async dispatch fails or allocation fails

- `void header_init(void)`
  - Call once after LVGL is ready and before the transcript widgets are created.
  - Builds the fixed non-scrollable top bar, scales its height from the active display resolution, and places status icons left-to-right with the notification area in the center.
  - System panel (MEM | CPU | BAT) is on the far right, all dynamically linked to FreeRTOS runtime stats.

- `void header_update_status(void)`
  - Request a header re-render from the currently cached state.
  - Useful after a batch of `header_update_*` calls when the caller wants one final refresh point.

- `void header_set_notification(const char *text, uint32_t timeout_ms)`
  - Show a short notification in the center of the fixed header with "!" icon prefix.
  - Uses LVGL async dispatch so callers can invoke it from shell worker tasks or other non-LVGL contexts.

- `void header_update_wifi(bool connected, int rssi)`
  - Update the Wi-Fi status indicator (WiFi HI/MID/LOW/WEAK/OFF).
  - State set immediately; render happens via async dispatch or direct fallback.

- `void header_update_battery(int percent, bool adc_ready)`
  - Update the battery icon, bar, and percentage label. Clamped to 0-100.
  - When `adc_ready` is false, shows "BAT N/C" with muted styling (battery always visible).
  - State set immediately; render happens via async dispatch or direct fallback.

- `void header_update_bluetooth(bool enabled, bool connected)`
  - Update the Bluetooth indicator (BT ON/BT IDLE/BT OFF).
  - State set immediately; render happens via async dispatch or direct fallback.

- `void header_update_usb(bool connected)`
  - Update the USB indicator (USB ON/USB OFF).
  - State set immediately; render happens via async dispatch or direct fallback.

- `void header_update_sd(header_sd_state_t state)`
  - Update the SD indicator with persistent state (SD NO/SD INS/SD ON/SD ERR).
  - State set immediately; render happens via async dispatch or direct fallback.

- `void header_update_mem(uint32_t free_heap_bytes, uint32_t total_heap_bytes)`
  - Update the memory display (MEM/MEM LOW + formatted size) from real-time FreeRTOS heap stats.
  - State set immediately; render happens via async dispatch or direct fallback.

- `void header_update_cpu(int cpu_percent, uint32_t task_count)`
  - Update the CPU usage bar and percentage label from real-time FreeRTOS runtime stats.
  - Clamped 0-100; warning color above P4_CONFIG_HEADER_CPU_WARN_PCT (85%).
  - State set immediately; render happens via async dispatch or direct fallback.

- `void header_update_uptime(uint32_t uptime_seconds)`
  - Update the uptime counter (used internally for formatting).
  - State set immediately; render happens via async dispatch or direct fallback.

- `void header_force_render(void)`
  - Force a synchronous header re-render. Call only from LVGL task context.

- `void header_deinit(void)`
  - Deinitialize the header bar, releasing all widgets and resetting state.
  - Call before rebuilding the UI after display rotation or resolution change.
  - Must be called from LVGL task context only.

- `void (*notify_header)(const char *text, uint32_t timeout_ms)` inside `networking_host_ops_t`
  - Optional host callback used by the networking module to surface live Wi-Fi notices directly in the fixed header without moving header ownership into `components/networking`.

## Host callback surface
`components/networking` and `components/bluetooth` share the same host callback table:

- `typedef struct networking_host_ops_t`
  - `void (*transcript_append_text)(const char *text)`
  - `void (*schedule_transcript_append_text)(const char *text)`
  - `void (*record_error)(const char *tag, esp_err_t error, const char *message)`
  - `void (*record_warning)(const char *tag, const char *message)`
  - `void (*record_info)(const char *tag, const char *message)`

`main/main.c` fills this table once and passes it to `networking_init(...)`. `components/networking` stores it and forwards the same callbacks into `bluetooth_init(...)`.

## Networking API
- `void networking_init(const networking_host_ops_t *ops)`
  - Registers the shared shell callback surface.
  - Initializes hosted Bluetooth through `bluetooth_init(...)`.
  - Starts the normal boot-time Wi-Fi restore flow once.

- `void networking_handle_wifi_command(char *command)`
  - Entry point for shell-level `wifi ...` command dispatch.

- `void networking_wifi_status(void)`
- `void networking_wifi_scan(void)`
- `void networking_wifi_diag(void)`
- `void networking_wifi_disconnect(void)`
  - Shell-facing Wi-Fi helpers used when the parser wants explicit subcommand entry points.

- `const char *networking_wifi_state_string(void)`
- `networking_wifi_state_t networking_wifi_state(void)`
- `esp_err_t networking_wifi_last_error(void)`
- `bool networking_wifi_is_connected(void)`
- `void networking_append_sysinfo_summary(void)`
  - Status helpers used by the shell and `sysinfo` output.

- `esp_err_t networking_wifi_wait_for_ota(void)`
- `bool networking_wifi_is_starting(void)`
- `void networking_wifi_capture_restore_state(networking_wifi_restore_state_t *restore_state)`
- `esp_err_t networking_wifi_shutdown(void)`
- `esp_err_t networking_wifi_restore_after_ota_failure(const networking_wifi_restore_state_t *restore_state)`
- `void networking_wifi_request_post_ota_restore(const networking_wifi_restore_state_t *restore_state)`
  - OTA-support helpers consumed by `components/c6ota`.

## Bluetooth API
- `void bluetooth_init(const networking_host_ops_t *ops)`
  - Receives the same host callback surface used by networking.
  - Keeps Bluetooth transcript and debug behavior aligned with the Wi-Fi module.

- `void bluetooth_handle_command(char *command)`
  - Entry point for shell-level `bluetooth ...` command dispatch.

- `void bluetooth_status(void)`
- `void bluetooth_scan(void)`
- `void bluetooth_advertise(bool enable)`
  - Focused Bluetooth helpers for shell subcommands.

- `bool bluetooth_is_enabled(void)`
- `bool bluetooth_is_connected(void)`
  - Read-only state helpers used by the header integration to show hosted BLE readiness without moving Bluetooth ownership back into `main/main.c`.

## USB API
- `void usb_init(void)`
  - Call once during boot after `networking_init(...)` so the USB module can install the shared host library and both class drivers.

- `void usb_handle_command(char *command)`
  - Entry point for shell-level `usb ...` command dispatch.
  - Preserves the same transcript-first behavior used by the other modular shell families.

- `void usb_status(void)`
  - Prints transcript-visible host, MSC, and HID state.

- `void usb_msc_mount(void)`
  - Mounts a connected MSC device at `/usb0` with `msc_host_vfs_register(...)`.

- `void usb_msc_ls(const char *path)`
  - Lists files from `usb:/...`, `/usb0/...`, or a relative USB-root path.
  - Keeps directory output bounded and user-facing so it matches the existing SD command feel.

- `void usb_hid_keyboard_enable(void)`
- `void usb_hid_keyboard_disable(void)`
- `void usb_hid_mouse_enable(void)`
- `void usb_hid_mouse_disable(void)`
  - Toggle transcript echo for attached HID boot devices without changing the rest of the shell input path.

- `bool usb_is_connected(void)`
- `bool usb_is_mounted(void)`
- `bool usb_is_keyboard_attached(void)`
- `bool usb_is_mouse_attached(void)`
  - Read-only state helpers used by the header component integration in `main/main.c`.

- `void usb_register_keyboard_input_callback(usb_keyboard_input_cb_t cb)`
  - Register a callback to receive USB keyboard input events (press/release with key code and modifiers).
  - The shell registers `shell_usb_keyboard_input()` here for CLI injection.

- `bool usb_key_to_ascii_full(uint8_t key_code, uint8_t modifiers, char *out)`
  - Convert a USB HID key code and modifiers to an ASCII character.
  - Supports full US keyboard layout: letters, numbers, symbols, keypad, with modifier-aware shifted characters.

- `const char *usb_key_name_full(uint8_t key_code)`
  - Get a human-readable name for any USB HID key code (F1-F12, arrows, navigation, etc.).

## Keyboard API (External Input Mode)

- `void keyboard_set_external_input(bool enabled)`
  - Enable/disable external input mode. When enabled, on-screen keyboard auto-hides.
  - Used by the shell when a USB keyboard is detected/removed.

- `bool keyboard_is_external_input_enabled(void)`
  - Query whether external input mode is active.

- `void keyboard_force_visible(void)`
  - Force on-screen keyboard to stay visible even when external input is enabled.

- `void keyboard_clear_force_visible(void)`
  - Clear force-visible override; re-evaluate auto-hide based on external input state.

## Shell USB Keyboard Bridge

- `void shell_usb_keyboard_input(uint8_t key_code, uint8_t modifiers, bool pressed)`
  - Handle a USB keyboard input event for CLI injection.
  - Dispatches to LVGL task via `lv_async_call` for safe input line manipulation.
  - Supports: printable characters, Enter (submit), Backspace, ESC (clear), Tab, arrows (cursor/history), Delete, Home, End.

## C6 OTA API
- `void c6ota_init(void)`
  - Call once during boot after the shell transcript path is ready.
  - Resets module-owned OTA state, including pending confirmation and in-progress tracking.

- `void c6ota_perform(const char *source)`
  - Starts the shell-visible `c6ota` flow for `sd:/...`, `/sdcard/...`, `http://...`, `https://...`, or `default`.
  - Preserves the established behavior: source validation, factory warning, exact YES confirmation text, background OTA execution, Wi-Fi stop or restore handling, live progress output, and final success or failure reporting.
  - Passing `NULL` or an unsupported source emits the existing usage text instead of changing shell behavior.

- `void c6ota_register_progress_callback(void (*cb)(int percent, const char *msg))`
  - Registers an optional callback that receives transcript-formatted OTA messages from the module.
  - `percent` is `0..100` for live progress updates.
  - Negative `percent` values are reserved for non-progress shell integration messages so the shell can preserve the same synchronous or asynchronous transcript behavior after the refactor.

- `bool c6ota_try_handle_input(const char *input)`
- `bool c6ota_is_busy(void)`
- `bool c6ota_is_confirmation_pending(void)`
  - Shell-integration helpers used by `main/main.c` to keep the confirmation flow and `sysinfo` state outside the OTA implementation details.

## Behavioral contract
- Public shell command surfaces remain `wifi ...`, `bluetooth ...`, `usb ...`, and `c6ota <sd:/path/to/firmware.bin|http[s]://host/path.bin|default>`.
- Wi-Fi and Bluetooth continue to report user-facing state through the shared transcript and debug hooks.
- USB continues that same transcript-first contract and mounts MSC storage at `/usb0` instead of changing the existing SD path.
- OTA confirmation text remains `WARNING: This will reboot the C6. Type YES to continue`.
- OTA success text remains `C6 OTA completed successfully! Type reboot to activate new firmware.`.
- OTA progress text remains `C6 OTA: XX% (YYYY KB / ZZZZ KB)`.
- OTA continues to validate ESP-IDF app-image magic `0xE9` and ESP32-C6 chip ID `0x000D` before transfer.