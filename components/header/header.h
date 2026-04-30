#ifndef P4MINISHELL_HEADER_H
#define P4MINISHELL_HEADER_H

/**
 * @file header.h
 * @brief Fixed top status bar for P4MiniShell.
 *
 * Passive, display-only module owning the LVGL fixed top bar.
 * All public functions are safe to call from any task context.
 *
 * Layout:
 *   [Status Panel: WiFi | BT | USB | SD]  [! Notification]  [Sys Panel: MEM | CPU | BAT]
 *
 * Icons (always visible, show state text, pure ASCII — no LVGL symbols):
 *   WiFi:  WiFi HI/MID/LOW/WEAK/OFF  (signal quality)
 *   BT:    BT ON/BT IDLE/BT OFF
 *   USB:   USB ON/USB OFF
 *   SD:    SD NO/SD INS/SD ON/SD ERR  (4 persistent states)
 *   MEM:   MEM/MEM LOW + free heap (always visible, real-time FreeRTOS)
 *   CPU:   CPU bar + percentage (real-time FreeRTOS runtime stats)
 *   BAT:   [####]/[### ]/[##  ]/[#   ]/[    ] bar + percentage (always visible)
 *          Shows "BAT N/C" when ADC is not connected / unavailable
 *
 * Features:
 *   - Grouped panels with subtle background tint for visual separation
 *   - Notification area in center with "!" prefix + scrolling text
 *   - System panel shows memory, CPU usage, and battery — all dynamically
 *     linked to FreeRTOS runtime statistics and updated in real time
 *   - All updates are thread-safe via lv_async_call dispatch
 *   - ALL text uses plain ASCII — guaranteed to render with any font
 */

#include <stdbool.h>
#include <stdint.h>

/** Base header height in pixels. Scaled dynamically from display resolution. */
#define HEADER_HEIGHT 40

/** SD card status enumeration for persistent display. */
typedef enum {
    HEADER_SD_NONE = 0,       /**< No SD card detected */
    HEADER_SD_INSERTED,       /**< Card detected but not mounted */
    HEADER_SD_MOUNTED,        /**< Card mounted and ready */
    HEADER_SD_ERROR,          /**< Card error or mount failure */
} header_sd_state_t;

/** Initialize the header bar. Call once after LVGL is ready, before transcript. */
void header_init(void);

/** Request a header re-render from cached state via async dispatch. */
void header_update_status(void);

/** Show a transient notification (center area with bell icon). Clears after timeout_ms. */
void header_set_notification(const char *text, uint32_t timeout_ms);

/** Update Wi-Fi indicator. connected drives styling, rssi for HI/MID/LOW/WEAK label. */
void header_update_wifi(bool connected, int rssi);

/**
 * Update battery icon, bar, and percentage label.
 * Always visible — shows "BAT N/C" with dim styling when adc_ready is false.
 * @param percent  0-100 battery percentage (only meaningful when adc_ready is true)
 * @param adc_ready  true when ADC is connected and reading valid data
 */
void header_update_battery(int percent, bool adc_ready);

/** Update Bluetooth indicator. enabled for controller ready, connected for sync. */
void header_update_bluetooth(bool enabled, bool connected);

/** Update USB indicator for attached MSC or HID devices. */
void header_update_usb(bool connected);

/**
 * Update SD card indicator with persistent state.
 * @param state  HEADER_SD_NONE (no card), HEADER_SD_INSERTED (card present),
 *               HEADER_SD_MOUNTED (filesystem ready), HEADER_SD_ERROR (failure)
 */
void header_update_sd(header_sd_state_t state);

/**
 * Update memory display with real-time FreeRTOS heap statistics.
 * @param free_heap_bytes   current free 8-bit capable heap
 * @param total_heap_bytes  total 8-bit capable heap size
 */
void header_update_mem(uint32_t free_heap_bytes, uint32_t total_heap_bytes);

/**
 * Update CPU usage display with real-time FreeRTOS runtime stats.
 * @param cpu_percent  0-100 CPU utilization percentage
 * @param task_count   current number of FreeRTOS tasks
 */
void header_update_cpu(int cpu_percent, uint32_t task_count);

/**
 * Update uptime display.
 * @param uptime_seconds  system uptime in seconds
 */
void header_update_uptime(uint32_t uptime_seconds);

/** Force a synchronous header re-render. Call only from LVGL task context. */
void header_force_render(void);

/**
 * Deinitialize the header bar, releasing all widgets and resetting state.
 * Call before rebuilding the UI after display rotation or resolution change.
 * Must be called from LVGL task context only.
 */
void header_deinit(void);

#endif