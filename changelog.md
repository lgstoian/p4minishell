## [0.1.0] - 2026-03-13
- Replaced the LVGL widgets demo in main/main.c with a shell UI built on the existing BSP startup path
- Preserved the original BSP-managed JD9165 display and GT911 touch initialization by keeping bsp_display_start_with_config() and BOARD_CFG_* settings unchanged
- Added a scrollable LVGL textarea, attached lv_keyboard, boot banner, and built-in commands: help, sysinfo, clear, reboot
- Added sysinfo reporting for board_config-backed values, ESP-IDF version, heap usage, PSRAM totals, and Wi-Fi unsupported status on esp32p4
- Refreshed project metadata to match the current shell implementation and verified esp32p4 baseline
- Split the shell UI into a read-only transcript area plus a dedicated prompt-bearing input line bound to the on-screen keyboard
- Added LV_EVENT_READY command submission on the input line and a 10-command recall buffer with Prev/Next touch controls