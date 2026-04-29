#ifndef P4MINISHELL_HEADER_H
#define P4MINISHELL_HEADER_H

/**
 * @file header.h
 * @brief Fixed top status bar for P4MiniShell.
 *
 * This is a passive, display-only module that owns the LVGL widgets for the
 * fixed top bar. It displays:
 *   - Wi-Fi status (connected/disconnected + signal quality)
 *   - Battery percentage (icon + bar + numeric)
 *   - Bluetooth readiness (enabled/connected/off)
 *   - USB attachment state
 *   - SD card mount state
 *   - Transient notifications from module events
 *
 * The header is non-scrollable, scales its height from the display resolution,
 * and places status icons left-to-right with the notification area on the far right.
 * It must not own Wi-Fi, Bluetooth, USB, SD, or battery runtime behavior.
 */

#include <stdbool.h>
#include <stdint.h>

#define HEADER_HEIGHT 40

// Public API - all functions use LVGL async dispatch internally so they are
// safe to call from shell worker tasks or module callbacks.

/** Initialize the header bar. Call once after LVGL is ready and before transcript widgets. */
void header_init(void);

/** Request a header re-render from currently cached state. */
void header_update_status(void);

/** Show a short notification text for the given timeout (milliseconds). */
void header_set_notification(const char *text, uint32_t timeout_ms);

/** Update Wi-Fi indicator: connected state and RSSI for signal quality label. */
void header_update_wifi(bool connected, int rssi);

/** Update battery icon, bar, and percentage label. */
void header_update_battery(int percent);

/** Update Bluetooth indicator for hosted BLE lifecycle state. */
void header_update_bluetooth(bool enabled, bool connected);

/** Update USB indicator for attached MSC or HID devices. */
void header_update_usb(bool connected);

/** Update SD indicator. When mounted, the icon is visible; when false, it is hidden. */
void header_update_sd(bool mounted);

#endif