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

/** Header layout mode. */
typedef enum {
    HEADER_MODE_AUTO = 0, /**< Fit to the live resolution (compact as needed). */
    HEADER_MODE_FULL,     /**< Always full labels/graph; may clip if tiny. */
    HEADER_MODE_COMPACT,  /**< Always abbreviated labels. */
    HEADER_MODE_COUNT
} header_mode_t;

/** Current header height in pixels for the live resolution/rotation. Matches
 * the window manager's reserved header region exactly. 0 when hidden by the
 * caller's visibility state is handled by the caller. */
int header_get_height(void);

/** Select the layout mode (AUTO by default; re-lays out on next render). */
void header_set_mode(header_mode_t mode);
header_mode_t header_get_mode(void);
const char *header_mode_name(header_mode_t mode);
/** Parse "auto"/"full"/"compact" (case-insensitive); NULL when unknown. */
bool header_mode_parse(const char *text, header_mode_t *out);

/** Schedule a re-layout/re-render (safe from any task). */
void header_relayout(void);

/** Snapshot of the last resolved layout + actual widget geometry (debug). */
typedef struct {
    int status_level;   /**< 0 full, 1 short, 2 min. */
    int sys_level;
    bool show_center;
    int font_step;      /**< 0 primary UI font, 1 compact fallback. */
    int left_w;         /**< Requested left-panel width. */
    int center_w;       /**< Requested center width. */
    int right_w;        /**< Requested right-panel width. */
    int actual_left;    /**< Live widget widths (0 when hidden/absent). */
    int actual_center;
    int actual_right;
    int screen_w;
} header_metrics_t;

/** Fill @p out with the last layout snapshot (zeroed when not initialised). */
void header_get_metrics(header_metrics_t *out);

/** Initialize the header bar. Call once after LVGL is ready, before transcript. */
void header_init(void);

/** Re-resolve label fonts after a font switch (same stale-pointer reason as
 * keyboard_refresh_fonts). No-op before header_init. */
void header_refresh_fonts(void);
void header_refresh_theme(void);

/**
 * Show or hide the entire header bar. When hidden, the window manager reports
 * a zero-height header region so the transcript expands to fill the space.
 * The caller is responsible for scheduling a UI rebuild via
 * display_schedule_ui_rebuild() after the first toggle so the layout updates.
 *
 * @param visible  true to show, false to hide.
 */
void header_set_visible(bool visible);

/** @return true when the header bar is visible, false when hidden. */
bool header_get_visible(void);

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

/**
 * Batch-update all header state at once and schedule a single async render.
 * This is more efficient than calling individual header_update_*() functions
 * because it avoids multiple lv_async_call dispatches and redundant renders.
 * Safe to call from any task context.
 *
 * @param wifi_connected    Wi-Fi link status
 * @param wifi_rssi         Wi-Fi signal strength in dBm
 * @param battery_percent   Battery charge 0-100 (only meaningful if adc_ready)
 * @param battery_adc_ready ADC readiness flag
 * @param bt_enabled        Bluetooth controller enabled
 * @param bt_connected      Bluetooth connected
 * @param usb_connected     Any USB device attached
 * @param sd_state          SD card state (NONE/INSERTED/MOUNTED/ERROR)
 * @param free_heap         Free 8-bit capable heap bytes
 * @param total_heap        Total 8-bit capable heap bytes
 * @param cpu_percent       CPU utilization 0-100
 * @param task_count        Number of FreeRTOS tasks
 * @param uptime_seconds    System uptime in seconds
 */
void header_update_batch(
    bool wifi_connected, int wifi_rssi,
    int battery_percent, bool battery_adc_ready,
    bool bt_enabled, bool bt_connected,
    bool usb_connected, header_sd_state_t sd_state,
    uint32_t free_heap, uint32_t total_heap,
    int cpu_percent, uint32_t task_count,
    uint32_t uptime_seconds
);

/** Force a synchronous header re-render. Call only from LVGL task context. */
void header_force_render(void);

/**
 * Deinitialize the header bar, releasing all widgets and resetting state.
 * Call before rebuilding the UI after display rotation or resolution change.
 * Must be called from LVGL task context only.
 */
void header_deinit(void);

#endif