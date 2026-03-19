#ifndef P4MINISHELL_HEADER_H
#define P4MINISHELL_HEADER_H

#include <stdbool.h>
#include <stdint.h>

#define HEADER_HEIGHT 40

// AI: Header module added as a separate component with a fixed top bar for notifications plus WiFi, battery, Bluetooth, USB, and SD status.
// AI: SD status now uses a consistent symbol/text and only shows when a card is mounted, matching the style of the other status icons.
// AI: Public API matches the simple update-call style used by c6ota, networking, and usb so main stays an orchestration layer only.
void header_init(void);
void header_update_status(void);
void header_set_notification(const char *text, uint32_t timeout_ms);
void header_update_wifi(bool connected, int rssi);
void header_update_battery(int percent);
void header_update_bluetooth(bool enabled, bool connected);
void header_update_usb(bool connected);
void header_update_sd(bool mounted);

#endif