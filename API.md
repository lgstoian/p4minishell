# Hosted Module API

This document describes the public integration surface exposed by the hosted runtime modules under `components/`.

## Shared integration pattern
- `main/main.c` remains the shell UI, transcript, parser, and orchestration layer.
- `components/networking` owns hosted Wi-Fi runtime state and also bootstraps the hosted Bluetooth module.
- `components/usb` owns USB Host Library state, USB MSC storage, and USB HID keyboard or mouse debug behavior.
- `components/c6ota` owns the shell-visible ESP32-C6 OTA workflow and depends on `components/networking` for Wi-Fi wait and restore hooks.
- All three areas keep user-visible behavior in the shell transcript instead of returning rich status objects to the caller.

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