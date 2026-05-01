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

## Required boot-time integration

1. Include `ansi.h`, `display.h`, `windows.h`, `header.h`, `networking.h`, `bluetooth.h`, `usb.h`, and `c6ota.h` where those modules are orchestrated.
2. Call `ansi_init()` during shell module initialization (done automatically by `shell_init()`).
3. Call `display_init()` first to initialize the display hardware and LVGL port.
4. Register the UI rebuild callback with `display_register_ui_rebuild_callback(shell_build_ui)` so rotation changes trigger full UI rebuilds.
5. Call `windows_init()` to build the LVGL shell surface (header, transcript, input row, keyboard).
6. Build a single `networking_host_ops_t` callback table backed by the shell transcript and debug-history functions.
7. Call `c6ota_init()` once after the transcript path is ready.
8. Register the OTA transcript callback with `c6ota_register_progress_callback(...)`.
9. Call `networking_init(&host_ops)` once so the Wi-Fi module can capture the host hooks.
10. Call `usb_init()` once after `networking_init(&host_ops)`.
11. Start a periodic status refresh timer that feeds `header_update_*` functions.

## Shared host callback model

`networking_host_ops_t` is the common host-to-module bridge for Wi-Fi and Bluetooth:

```c
static const networking_host_ops_t host_ops = {
    .transcript_append_text = shell_transcript_append_text,
    .schedule_transcript_append_text = shell_networking_schedule_text,
    .record_error = shell_networking_record_error,
    .record_warning = shell_networking_record_warning,
    .record_info = shell_networking_record_info,
};
```

- `networking_init(...)` stores this table.
- `networking_init(...)` also calls `bluetooth_init(&s_host_ops)`, so Bluetooth uses the same transcript and debug hooks automatically.
- `c6ota` uses dedicated host bridge functions implemented in `main/main.c` plus a progress callback registration.
- `components/usb` also uses dedicated host bridge functions implemented in `main/main.c`.
- `components/header` is display-only and is updated through its public `header_update_*` calls.
- Wi-Fi uses the `notify_header` host callback for immediate header notices on key connection lifecycle events.

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
| `@B` | 1 | Bold |
| `@D` | 2 | Dim |
| `@I` | 3 | Italic |
| `@U` | 4 | Underline |
| `@b` | 5 | Slow blink |
| `@r` | 7 | Reverse |
| `@h` | 8 | Hidden |
| `@S` | 9 | Strikethrough |
| `@k` | 30 | Black foreground |
| `@r` | 31 | Red foreground |
| `@g` | 32 | Green foreground |
| `@y` | 33 | Yellow foreground |
| `@B` | 34 | Blue foreground |
| `@M` | 35 | Magenta foreground |
| `@C` | 36 | Cyan foreground |
| `@W` | 37 | White foreground |
| `@K` | 90 | Bright black (gray) |
| `@R` | 91 | Bright red foreground |
| `@G` | 92 | Bright green foreground |
| `@Y` | 93 | Bright yellow foreground |
| `@b` | 94 | Bright blue foreground |
| `@m` | 95 | Bright magenta foreground |
| `@c` | 96 | Bright cyan foreground |
| `@w` | 97 | Bright white foreground |

Note: When the same specifier is used for both an attribute and a color (e.g., `@R` = reset vs `@R` = bright red), the parser resolves the conflict by context. If the specifier appears where an attribute is expected, it resolves to the attribute; if where a color is expected, it resolves to the color. In practice, always use attributes first in the format string, then colors.

## Display Module API

### Lifecycle
- `void display_init(void)` — Initialize the display hardware and LVGL port.
- `bool display_is_initialized(void)` — Check if the display module is initialized.

### Rotation
- `esp_err_t display_set_rotation(display_rotation_t rotation)` — Set display rotation (0/90/180/270), including GT911 touch remapping.
- `display_rotation_t display_get_rotation(void)` — Get current display rotation.
- `esp_err_t display_rotation_parse(const char *text, display_rotation_t *rotation_out)` — Parse rotation from string.

### Resolution
- `display_resolution_t display_get_resolution(void)` — Get current and native resolution.
- `lv_coord_t display_get_width(void)` — Get current display width (accounts for rotation).
- `lv_coord_t display_get_height(void)` — Get current display height (accounts for rotation).

### Brightness
- `esp_err_t display_set_brightness(int percent)` — Set backlight brightness (0-100).
- `int display_get_brightness(void)` — Get current backlight brightness.

### Power Management
- `esp_err_t display_set_power_state(display_power_state_t state)` — Set display power state (on/sleep/off).
- `display_power_state_t display_get_power_state(void)` — Get current display power state.
- `esp_err_t display_sleep(void)` — Put display to sleep.
- `esp_err_t display_wake(void)` — Wake display from sleep.

### Refresh
- `display_refresh_config_t display_get_refresh_config(void)` — Get current refresh configuration.
- `esp_err_t display_set_refresh_rate(uint32_t target_hz)` — Set target refresh rate (dynamic, not supported on JD9165).

### Diagnostics
- `void display_get_info(display_info_t *info)` — Get comprehensive display diagnostics.
- `void display_print_info(void (*print_fn)(const char *format, ...))` — Print formatted display diagnostics.

### Touch
- `esp_err_t display_get_touch_handle(esp_lcd_touch_handle_t *touch_handle)` — Get GT911 touch handle.

### UI Rebuild Callback
- `void display_register_ui_rebuild_callback(void (*callback)(void))` — Register a callback invoked after display rotation changes.

## Window Manager API

### Lifecycle
- `void windows_init(void)` — Build all LVGL UI regions (header, transcript, input, keyboard).
- `void windows_deinit(void)` — Tear down all UI regions before rotation rebuild.

### Region Access
- `window_rect_t windows_get_rect(window_region_t region)` — Get bounding rectangle for a named region.
- `lv_obj_t *windows_get_header(void)` — Get the header container object.
- `lv_obj_t *windows_get_transcript(void)` — Get the transcript textarea object.
- `lv_obj_t *windows_get_input_area(void)` — Get the input area container.
- `lv_obj_t *windows_get_input_line(void)` — Get the input line textarea object.
- `lv_obj_t *windows_get_keyboard(void)` — Get the keyboard object.
- `lv_obj_t *windows_get_prev_button(void)` — Get the history previous button.
- `lv_obj_t *windows_get_next_button(void)` — Get the history next button.

### Scaling
- `lv_coord_t windows_get_display_width(void)` — Get current display width.
- `lv_coord_t windows_get_display_height(void)` — Get current display height.
- `lv_coord_t windows_scale(lv_coord_t value, lv_coord_t reference_width)` — Scale a value relative to reference.

### Styling
- `lv_color_t windows_get_color(window_color_t color_id)` — Get a color by semantic ID.
- `const lv_font_t *windows_get_terminal_font(void)` — Get the terminal font.

### Helpers
- `int window_region_count(void)` — Get the number of defined regions.

## Header Module API

### Lifecycle
- `void header_init(lv_obj_t *parent)` — Create the header UI within the given parent container.
- `void header_deinit(void)` — Tear down header widgets.

### Status Updates (all use lv_async_call for thread safety)
- `void header_update_wifi(bool connected, const char *ssid)` — Update Wi-Fi icon and SSID.
- `void header_update_bluetooth(bool enabled, bool connected)` — Update Bluetooth icon.
- `void header_update_usb(bool connected)` — Update USB icon.
- `void header_update_sd(sd_status_t status, const char *label)` — Update SD card icon.
- `void header_update_mem(void)` — Refresh memory gauge.
- `void header_update_cpu(void)` — Refresh CPU gauge.
- `void header_update_battery(int percent, bool charging)` — Refresh battery gauge.
- `void header_update_uptime(void)` — Refresh uptime display.

### Notifications
- `void header_show_notification(const char *text, uint32_t timeout_ms)` — Show a transient notification.
- `void header_hide_notification(void)` — Hide the current notification.

### Batch Update
- `void header_batch_update(void)` — Refresh all status indicators at once.

## Shell Core Module API

### Transcript Management
- `void shell_transcript_append_text(const char *text)` — Append text to transcript.
- `void shell_transcript_appendf(const char *format, ...)` — Append formatted text.
- `void shell_transcript_append_ansi(const char *text)` — Append ANSI-formatted text.
- `void shell_transcript_appendf_ansi(const char *format, ...)` — Append formatted ANSI text.
- `void shell_schedule_transcript_appendf(const char *format, ...)` — Schedule text append from non-LVGL task.
- `void shell_transcript_reset(void)` — Clear transcript.
- `void shell_history_transcript_scroll_to_end(void)` — Scroll transcript to end.

### Command History
- `void shell_store_command_history(const char *command)` — Store command in history (with password masking).
- `void shell_recall_history(int direction)` — Recall command from history.
- `const char *shell_get_history_draft(void)` — Get current history draft.

### Debug Log
- `void shell_debug_log_push(const char *tag, const char *message)` — Push entry to debug log.
- `void shell_record_errorf(const char *tag, int error, const char *format, ...)` — Record error.
- `void shell_record_warningf(const char *tag, const char *format, ...)` — Record warning.
- `void shell_record_infof(const char *tag, const char *format, ...)` — Record info.
- `size_t shell_get_warning_count(void)` — Get warning count.
- `void shell_command_debug(void)` — Print debug log.

### UART Console
- `void shell_uart_console_start(void)` — Start UART console reader task.
- `void shell_uart_console_write_text(const char *text)` — Write to UART.
- `void shell_uart_console_print_prompt(void)` — Print prompt on UART.
- `void shell_uart_console_submit_command(const char *command)` — Submit command from UART.

### System Info
- `void shell_command_help(void)` — Print help text.
- `void shell_command_sysinfo(void)` — Print system information.
- `void shell_command_version(void)` — Print version.
- `void shell_command_about(void)` — Print about info.
- `void shell_command_mem(void)` — Print memory stats.

### Header Integration
- `void shell_header_status_refresh(void)` — Refresh header status.
- `int64_t shell_get_boot_timestamp_us(void)` — Get boot timestamp.
- `const char *shell_get_time_string(void)` — Get formatted time.
- `bool shell_time_is_synced(void)` — Check NTP sync status.

### Shell Utilities
- `bool shell_text_equals_ignore_case(const char *left, const char *right)` — Case-insensitive compare.
- `char *shell_trim(char *text)` — Trim whitespace in-place.
- `int shell_split_args(char *text, char **argv, int max_args)` — Split command line into args.
- `bool shell_parse_percentage_arg(const char *text, int *percentage_out)` — Parse 0-100 percentage.
- `bool shell_parse_size_arg(const char *text, size_t min, size_t max, size_t *value_out)` — Parse bounded size.
- `void shell_join_args(char **argv, int start, int argc, char *output, size_t output_size)` — Join args.
- `void shell_format_size(char *dst, size_t dst_size, size_t bytes)` — Format byte size with units (B, KiB, MiB, GiB).
- `const char *shell_entry_type(mode_t mode)` — Return "dir" or "file" for a mode.
- `void shell_header_notify(const char *text, uint32_t timeout_ms)` — Show header notification.

### Lifecycle
- `void shell_init(void)` — Initialize shell module.
- `void shell_set_default_state(void)` — Set default shell state.
- `bool shell_is_initialized(void)` — Check initialization.

### USB Keyboard Bridge
- `void shell_usb_keyboard_input(uint8_t key_code, uint8_t modifiers, bool pressed)` — Handle USB keyboard input.

### PowerShell Prompt Support
- `const char *shell_get_cwd_for_prompt(void)` — Get CWD formatted for prompt.

## Networking Module API

### Types
- `networking_wifi_state_t` — Wi-Fi state enum (NOT_ATTEMPTED, STARTING, STARTED, ERROR).
- `networking_wifi_restore_state_t` — OTA restore state struct.
- `networking_host_ops_t` — Host callback table.

### Lifecycle
- `esp_err_t networking_init(const networking_host_ops_t *ops)` — Initialize Wi-Fi and Bluetooth.
- `bool networking_is_initialized(void)` — Check initialization.

### Wi-Fi Commands
- `void networking_handle_wifi_command(char *command)` — Route a `wifi ...` command.
- `const char *networking_wifi_state_string(void)` — Get Wi-Fi state as string.

### OTA Hooks
- `void networking_wifi_wait_for_ota(void)` — Wait for Wi-Fi readiness before OTA.
- `void networking_wifi_pre_ota_shutdown(networking_wifi_restore_state_t *state)` — Shutdown before OTA.
- `void networking_wifi_post_ota_restore(const networking_wifi_restore_state_t *state)` — Restore after OTA.

## Bluetooth Module API

- `void bluetooth_init(const networking_host_ops_t *ops)` — Initialize with host callbacks.
- `void bluetooth_handle_command(char *command)` — Route bluetooth/bt commands.
- `void bluetooth_status(void)` — Print Bluetooth status.
- `void bluetooth_scan(void)` — Start BLE scan.
- `void bluetooth_advertise(bool enable)` — Start/stop advertising.
- `bool bluetooth_is_enabled(void)` — Check if enabled.
- `bool bluetooth_is_connected(void)` — Check NimBLE sync.

## USB Module API

### Types
- `usb_key_event_t` — Key event enum (PRESS, RELEASE).
- `usb_keyboard_input_cb_t` — Keyboard input callback type.

### Lifecycle
- `esp_err_t usb_init(void)` — Initialize USB Host Library.
- `bool usb_is_initialized(void)` — Check initialization.

### Status
- `void usb_status(void)` — Print USB subsystem status.
- `bool usb_msc_is_mounted(void)` — Check if MSC is mounted.
- `bool usb_hid_keyboard_active(void)` — Check if HID keyboard is active.
- `esp_err_t usb_ls(const char *path, int max_entries)` — List USB directory.

### Keyboard
- `void usb_keyboard_set_echo(bool enabled)` — Enable/disable keyboard echo.
- `void usb_keyboard_set_enabled(bool enabled)` — Enable/disable USB keyboard.
- `void usb_mouse_set_echo(bool enabled)` — Enable/disable mouse echo.

### Input Callback
- `void usb_register_keyboard_input_callback(usb_keyboard_input_cb_t callback)` — Register keyboard input callback.

### Key Mapping (optional public utility)
- `char usb_key_to_ascii_full(uint8_t key_code, uint8_t modifiers)` — Convert USB HID key code to ASCII.
- `const char *usb_key_name_full(uint8_t key_code)` — Get human-readable key name.

### Commands
- `void usb_handle_command(char *command)` — Route a `usb ...` command.

## C6 OTA Module API

### Types
- `typedef void (*c6ota_progress_callback_t)(int percent, const char *message)` — Progress callback.

### Lifecycle
- `void c6ota_init(void)` — Initialize the OTA module.
- `bool c6ota_is_initialized(void)` — Check initialization.

### Execution
- `void c6ota_perform(const char *source)` — Perform OTA from source (sd:, http://, https://, or default).
- `bool c6ota_is_busy(void)` — Check if OTA is in progress.

### Confirmation
- `bool c6ota_is_confirmation_pending(void)` — Check if waiting for confirmation.
- `bool c6ota_try_handle_input(const char *input)` — Handle confirmation input (YES to confirm).

### Progress
- `void c6ota_register_progress_callback(c6ota_progress_callback_t callback)` — Register progress callback.
