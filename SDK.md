# Hosted Module SDK Guide

This guide describes how `main/main.c` integrates the hosted runtime modules in this workspace: `components/header`, `components/networking`, `components/bluetooth`, `components/usb`, and `components/c6ota`.

## Architecture
- `main/main.c` owns the transcript, parser, command history rules, and boot banner.
- `components/header` owns the fixed top-bar LVGL widgets for notifications plus Wi-Fi, battery, Bluetooth, USB, and SD status.
- `components/networking` owns hosted Wi-Fi runtime state, boot restore, diagnostics, and OTA restore hooks.
- `components/networking/bluetooth.c` owns the hosted NimBLE control path for Bluetooth commands.
- `components/usb` owns ESP-IDF USB Host Library bring-up, MSC VFS registration at `/usb0`, and HID keyboard or mouse debug echo.
- `components/c6ota` owns the ESP32-C6 OTA workflow and uses `components/networking` when it needs Wi-Fi readiness or restore behavior.

## Required boot-time integration
1. Include `header.h`, `networking.h`, `bluetooth.h`, `usb.h`, and `c6ota.h` where those modules are orchestrated.
2. Build a single `networking_host_ops_t` callback table backed by the shell transcript and debug-history functions.
3. Call `header_init()` once after LVGL is ready and before the transcript widgets are created so the fixed bar is the first child on the screen.
4. Call `c6ota_init()` once after the transcript path is ready.
5. Register the OTA transcript callback with `c6ota_register_progress_callback(...)`.
6. Call `networking_init(&host_ops)` once so the Wi-Fi module can capture the host hooks and the Bluetooth module can inherit the same callback surface.
7. Call `usb_init()` once after `networking_init(&host_ops)` so the USB host stack starts after the existing networking bootstrap without regressing boot orchestration.
8. Start a small periodic status refresh, for example an LVGL timer every 5 seconds, that feeds `header_update_wifi`, `header_update_battery`, `header_update_bluetooth`, `header_update_usb`, `header_update_sd`, and then `header_update_status()`.

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
- `c6ota` currently uses dedicated host bridge functions implemented in `main/main.c` plus a progress callback registration step, rather than consuming `networking_host_ops_t` directly.
- `components/usb` also uses dedicated host bridge functions implemented in `main/main.c`, mirroring the transcript and debug behavior already used by `c6ota`.
- `components/header` is display-only and is updated through its public `header_update_*` calls rather than consuming the transcript callback surface directly.
- Wi-Fi now also uses the `notify_header` host callback for immediate header notices on key connection lifecycle events, while USB and `c6ota` use dedicated host bridge functions in `main/main.c` for the same purpose.

## Shell parser integration
1. Route `wifi ...` commands to `networking_handle_wifi_command(...)`.
2. Route `bluetooth ...` commands to `bluetooth_handle_command(...)`.
3. Route `usb ...` commands to `usb_handle_command(command_copy)`.
4. Route `c6ota ...` requests to `c6ota_perform(source)`.
5. Before normal command parsing completes, pass raw user input through `c6ota_try_handle_input(...)` so pending `YES` or `NO` responses stay in the OTA confirmation path.
6. Use `c6ota_is_confirmation_pending()` to keep confirmation replies out of normal command history.
7. Use `c6ota_is_busy()` when the shell wants to report OTA state, such as `sysinfo` output.

## Example boot integration
```c
static void shell_header_status_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    header_update_wifi(networking_wifi_is_connected(), current_rssi);
    header_update_battery(current_battery_percent);
    header_update_bluetooth(bluetooth_is_enabled(), bluetooth_is_connected());
    header_update_usb(usb_is_connected());
    /* Update the header SD icon immediately after mount/unmount so the SD indicator matches the actual card state. */
    header_update_sd(sd_is_mounted);
    header_update_status();
}

static void shell_c6ota_progress(int percent, const char *msg)
{
    (void)percent;
    if (msg != NULL) {
        shell_transcript_append_text(msg);
    }
}

void shell_boot_init(void)
{
    header_init();
    c6ota_init();
    c6ota_register_progress_callback(shell_c6ota_progress);

    networking_init(&(networking_host_ops_t){
        .transcript_append_text = shell_transcript_append_text,
        .schedule_transcript_append_text = shell_networking_schedule_text,
        .record_error = shell_networking_record_error,
        .record_warning = shell_networking_record_warning,
        .record_info = shell_networking_record_info,
    });

    usb_init();
    lv_timer_create(shell_header_status_timer_cb, 5000, NULL);
}
```

## Header integration notes
- The header is passive and display-only. It must not own Wi-Fi, Bluetooth, USB, SD, or battery runtime behavior.
- All `header_update_*()` functions are safe to call from any task context.
- State is set immediately (atomic bool/int writes); render is scheduled via LVGL async dispatch.
- If async dispatch fails, a synchronous `header_render()` fallback ensures the widget updates.
- Poll Wi-Fi RSSI through `esp_wifi_sta_get_ap_info()`, battery through shell ADC helper.
- `header_set_notification(...)` is async-safe (uses LVGL async dispatch internally).
- Header is non-scrollable, resolution-scaled, left-to-right status icons, notification on far right.
- SD indicator is hidden when no card mounted, visible with consistent `HEADER_SD_SYMBOL` when mounted.

## Example command dispatch
```c
if (strncmp(argv[0], "wifi", 4) == 0 && argv[0][4] == '\0') {
    networking_handle_wifi_command(command_copy);
    return true;
}

if (strncmp(argv[0], "bluetooth", 9) == 0 && argv[0][9] == '\0') {
    bluetooth_handle_command(command_copy);
    return true;
}

if (strncmp(argv[0], "usb", 3) == 0 && argv[0][3] == '\0') {
    usb_handle_command(command_copy);
    return true;
}

if (c6ota_try_handle_input(trimmed)) {
    return true;
}

if (strncmp(argv[0], "c6ota", 5) == 0 && argv[0][5] == '\0') {
    c6ota_perform(argc >= 2 ? argv[1] : NULL);
    return true;
}
```

## OTA-specific runtime flow
1. Validate the requested source and queue the exact YES confirmation prompt.
2. For HTTP or HTTPS, wait for Wi-Fi readiness through `networking_wifi_wait_for_ota()` and download the full image first.
3. Validate the ESP-IDF app header and ESP32-C6 chip ID.
4. Capture the current Wi-Fi restore state through `networking_wifi_capture_restore_state(...)`.
5. Stop Wi-Fi completely for OTA, keep the hosted transport alive, and switch into Wi-Fi-off OTA mode.
6. Transfer the image over `esp_hosted_slave_ota_begin/write/end/activate` with the existing fixed chunk size and progress formatting.
7. Request post-OTA Wi-Fi restore on success, or restore Wi-Fi immediately after failures.

## Error reporting expectations
- `networking`, `bluetooth`, and `c6ota` are shell-oriented modules. They report meaningful state through transcript text and debug-history hooks rather than through a large return-value API.
- `usb` follows the same shell-oriented rule: MSC mount or listing and HID enable or disable state stay transcript-visible instead of introducing a separate structured shell protocol.
- OTA failures may include ESP-IDF names such as `ESP_ERR_INVALID_ARG`, `ESP_ERR_INVALID_STATE`, `ESP_ERR_INVALID_SIZE`, `ESP_ERR_NOT_FOUND`, `ESP_ERR_NOT_SUPPORTED`, `ESP_ERR_NO_MEM`, and `ESP_FAIL`.
- Caller code should not rewrite module-owned shell messages if behavior compatibility matters.

## Compatibility rules
- Keep the ESP-Hosted dependency aligned with the current project baseline in `main/idf_component.yml`.
- Preserve the existing shell-visible command surfaces for `wifi`, `bluetooth`, `usb`, and `c6ota`.
- Preserve the exact OTA confirmation, progress, success, and failure strings.
- Do not reintroduce pre-OTA `esp_hosted_deinit()` on this esp32p4 baseline.
- Keep `c6ota default` aligned with the long-filename-safe SD lookup for `esp32c6_hosted_slave.bin` and `network_adapter.bin`.