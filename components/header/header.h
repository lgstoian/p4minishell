#ifndef P4MINISHELL_HEADER_H
#define P4MINISHELL_HEADER_H

/**
 * @file header.h
 * @brief Fixed top status bar for P4MiniShell.
 *
 * Passive, display-only module owning the LVGL fixed top bar.
 * All public functions are safe to call from any task context.
 *
 * Layout (config-driven):
 *   [status_row: 4 icons left-to-right] [notification_area right]
 *
 * Icons (always visible, show state text):
 *   WiFi:  symbol + HI/MID/LOW/OFF
 *   BT:    symbol + ON/PAIR/OFF
 *   USB:   symbol + ON/OFF
 *   SD:    ASCII "SD:" + NO/INS/ON/ERR  (4 persistent states)
 *   BAT:   symbol + bar + percentage
 *
 * The SD icon uses plain ASCII prefix for guaranteed visibility across all fonts.
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

/** Show a transient notification (right side). Clears after timeout_ms. */
void header_set_notification(const char *text, uint32_t timeout_ms);

/** Update Wi-Fi indicator. connected drives styling, rssi for HI/MID/LOW label. */
void header_update_wifi(bool connected, int rssi);

/** Update battery icon, bar, and percentage label. Clamped 0-100. */
void header_update_battery(int percent);

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

/** Force a synchronous header re-render. Call only from LVGL task context. */
void header_force_render(void);

#endif