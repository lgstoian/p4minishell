#ifndef P4MINISHELL_BLUETOOTH_H
#define P4MINISHELL_BLUETOOTH_H

/**
 * @file bluetooth.h
 * @brief Hosted NimBLE Bluetooth module for P4MiniShell.
 *
 * Owns Bluetooth bring-up through NimBLE VHCI on the ESP32-C6 over ESP-Hosted SDIO.
 * Supports BLE scanning and non-connectable advertising.
 * Keeps hosted controller state across shell commands so scan/advertise
 * requests reuse the active host stack instead of reinitializing.
 *
 * Uses the same networking_host_ops_t callback surface as the Wi-Fi module.
 */

#include <stdbool.h>

#include "networking.h"

/** Initialize Bluetooth with the shared host callback table. Called by networking_init(). */
void bluetooth_init(const networking_host_ops_t *ops);

/** Entry point for shell-level `bluetooth ...` and `bt ...` command dispatch. */
void bluetooth_handle_command(char *command);

/** Print transcript-visible Bluetooth readiness, NimBLE state, and advertising state. */
void bluetooth_status(void);

/** Run a BLE scan through hosted NimBLE and print discovered devices to transcript. */
void bluetooth_scan(void);

/** Start (true) or stop (false) non-connectable BLE advertising. */
void bluetooth_advertise(bool enable);

/** Returns true when hosted controller and NimBLE host are initialized. */
bool bluetooth_is_enabled(void);

/** Returns true when NimBLE host synchronization is complete. */
bool bluetooth_is_connected(void);

#endif