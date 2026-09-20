/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
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
 *   [Status Panel: WiFi | BT | USB | SD | Activity]  [Notification/Clock]  [Sys: MEM | CPU | BAT]
 *
 * Icons (pure ASCII — no LVGL symbols). Two styles, selected by
 * P4_CONFIG_HEADER_STATUS_STYLE:
 *   words: WiFi HI/MID/LOW/WEAK/OFF, BT ON/IDLE/OFF, USB ON/OFF,
 *          SD NO/INS/ON/ERR, ACT, MEM + free heap, CPU bar + %, BAT bar + %.
 *   glyph: one compact letter per indicator ("W BT U S A" / "M C B") colored by
 *          state; the reclaimed width goes to the notification area, and a
 *          number still follows MEM/CPU/BAT. Tap shows a one-line detail.
 * Color is state-driven in both styles (header_status.c is the single source):
 *   green = healthy, amber = degraded, red = off/failed, muted = absent (SD).
 *   WiFi:  signal quality (strong/good = green, weak = amber, off = red)
 *   BT:    connected = green, enabled/idle = amber, off = red
 *   USB:   connected = green, off = red
 *   SD:    mounted = green, inserted = amber, error = red, none = muted
 *   ACT:   amber while a C6 OTA or a background job runs, else hidden
 *   MEM/CPU/BAT: healthy = green, low/hot = amber, critical = red
 *
 * Center area:
 *   - Shows the local clock ("HH:MM") when idle, and the active notification
 *     otherwise. Notifications are queued (FIFO) with a severity that colors
 *     them: info = normal, warn = amber, error = red.
 *
 * Features:
 *   - Grouped panels with subtle background tint for visual separation
 *   - Notification queue in center with "!" prefix + scrolling text + severity
 *   - Idle center shows the local time; tap any indicator for one-line detail
 *   - System panel shows memory, CPU usage, and battery — all dynamically
 *     linked to FreeRTOS runtime statistics and updated in real time
 *   - All updates are thread-safe via lv_async_call dispatch
 *   - ALL text uses plain ASCII — guaranteed to render with any font
 *
 * The adaptive poll cadence (header_refresh.c) is chosen by the shell and
 * applied by main's single header timer; the queue mechanics live in the pure
 * header_notify_queue.c. Neither is duplicated here.
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

/**
 * Indicator identity for the tap/long-press action callback.
 * WiFi..SD are the left status panel; MEM/CPU/BATTERY are the right system
 * panel. The two panels keep separate letter spaces, so "BT" and "B" (battery)
 * never appear side by side.
 */
typedef enum {
    HEADER_STATUS_WIFI = 0,
    HEADER_STATUS_BLUETOOTH,
    HEADER_STATUS_USB,
    HEADER_STATUS_SD,
    HEADER_STATUS_ACTIVITY,   /**< Conditional: OTA/bg-job activity indicator. */
    HEADER_STATUS_MEM,
    HEADER_STATUS_CPU,
    HEADER_STATUS_BATTERY,
    HEADER_STATUS_KIND_COUNT,
} header_status_kind_t;

/** Number of indicators in the left status panel (WiFi, BT, USB, SD, activity). */
#define HEADER_STATUS_PANEL_COUNT 5

/**
 * Invoked when a status indicator is long-pressed. The shell layer registers
 * one handler that runs the matching full status command (for example
 * `wifi status`). Tapping an indicator shows a one-line detail in the header's
 * own notification area without calling this hook.
 */
typedef void (*header_status_action_cb_t)(header_status_kind_t kind);

/** Register the long-press handler (NULL clears it). Safe before header_init. */
void header_register_status_action(header_status_action_cb_t cb);

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
    bool glyph_style;   /**< true when the compact colored-glyph style is active. */
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

/** Severity of a header notification; drives the message color and icon. */
typedef enum {
    HEADER_NOTIFY_INFO = 0, /**< Normal message (default text color). */
    HEADER_NOTIFY_WARN,     /**< Warning (amber). */
    HEADER_NOTIFY_ERR,      /**< Error (red). */
} header_notify_level_t;

/**
 * Queue a notification for the center area. Notifications are shown FIFO; a
 * busy center does not drop a new alert, it queues it (up to
 * P4_CONFIG_HEADER_NOTIFY_QUEUE). An empty @p text flushes the queue and
 * clears the center (the `notify -` clear). A timeout of 0 shows the message
 * until the next notification/clear.
 */
void header_notify(header_notify_level_t level, const char *text, uint32_t timeout_ms);

/**
 * Queue an INFO notification. Thin wrapper over header_notify() kept for the
 * original callers.
 */
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

/** One atomic header state snapshot for header_update_batch(). */
typedef struct {
    bool wifi_connected;
    int wifi_rssi;              /**< Wi-Fi signal strength in dBm. */
    int battery_percent;        /**< 0-100 (only meaningful when adc_ready). */
    bool battery_adc_ready;
    bool battery_charging;      /**< true when the pack is on external charge power. */
    bool bt_enabled;
    bool bt_connected;
    bool usb_connected;
    header_sd_state_t sd_state;
    uint32_t free_heap;         /**< Free 8-bit capable heap bytes. */
    uint32_t total_heap;        /**< Total 8-bit capable heap bytes. */
    int cpu_percent;            /**< 0-100 CPU utilization. */
    uint32_t task_count;
    uint32_t uptime_seconds;
    const char *clock_text;     /**< Local "HH:MM" (or "--:--"); NULL hides it. */
    bool c6ota_busy;            /**< C6 OTA update in progress. */
    bool bg_jobs_running;       /**< At least one background job is running. */
} header_batch_t;

/**
 * Batch-update all header state at once and schedule a single async render.
 * This is more efficient than calling individual header_update_*() functions
 * because it avoids multiple lv_async_call dispatches and redundant renders.
 * Safe to call from any task context. @p in is copied, so the caller's
 * clock_text buffer may be transient.
 */
void header_update_batch(const header_batch_t *in);

/** Force a synchronous header re-render. Call only from LVGL task context. */
void header_force_render(void);

/**
 * Deinitialize the header bar, releasing all widgets and resetting state.
 * Call before rebuilding the UI after display rotation or resolution change.
 * Must be called from LVGL task context only.
 */
void header_deinit(void);

#endif