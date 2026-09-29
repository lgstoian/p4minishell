/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
#ifndef P4MINISHELL_BLUETOOTH_H
#define P4MINISHELL_BLUETOOTH_H

/**
 * @file bluetooth.h
 * @brief Hosted NimBLE Bluetooth module for P4MiniShell.
 *
 * Owns Bluetooth bring-up through NimBLE VHCI on the ESP32-C6 over ESP-Hosted SDIO.
 * Supports BLE scanning, non-connectable advertising, connectable connections,
 * and BLE HID host (keyboard/mouse) functionality.
 *
 * BLE HID host: connects to external BLE keyboards and mice, subscribes to
 * HID report characteristics, and routes incoming reports to the shell input
 * path (the same path USB HID keyboards use).
 *
 * Uses the same networking_host_ops_t callback surface as the Wi-Fi module.
 */

#include <stdbool.h>
#include <stdint.h>

#include "networking.h"

/** Initialize Bluetooth with the shared host callback table. Called by networking_init(). */
void bluetooth_init(const networking_host_ops_t *ops);

/** Entry point for shell-level `bluetooth ...` and `bt ...` command dispatch. */
void bluetooth_handle_command(char *command);

/** Print transcript-visible Bluetooth readiness, NimBLE state, and advertising state. */
void bluetooth_status(void);

/**
 * Run a bounded BLE scan through hosted NimBLE and print the discovered
 * devices (name + address + RSSI) sorted by signal strength.
 *
 * @param limit  Maximum results to print. <= 0 selects the configured default
 *               (P4_CONFIG_BT_SCAN_LIMIT). The scan always terminates after
 *               P4_CONFIG_BT_SCAN_DURATION_MS so the command never hangs.
 */
void bluetooth_scan(int limit);

/**
 * Start (true) or stop (false) non-connectable BLE advertising.
 *
 * @param enable  true to advertise, false to stop.
 * @param name    Session-only advertising name. When non-NULL and non-empty,
 *                it overrides the configured default device name for the rest
 *                of this session; it is never persisted across boots.
 */
void bluetooth_advertise(bool enable, const char *name);

/**
 * Connect to a BLE device by MAC address for HID input.
 *
 * Parses the address string (XX:XX:XX:XX:XX:XX, case-insensitive), initiates
 * a BLE connection, discovers the HID service (0x1812), and subscribes to
 * HID report characteristics. Incoming reports are routed to the shell input
 * path. Fails if already connected or if the address is invalid.
 *
 * @param addr_str  MAC address in "XX:XX:XX:XX:XX:XX" format.
 * @return ESP_OK on successful connection initiation, or an error.
 */
esp_err_t bluetooth_connect(const char *addr_str);

/**
 * Disconnect from the currently connected BLE HID device.
 * No-op when no device is connected. Stops HID report notifications and
 * releases the GATT client resources.
 */
void bluetooth_disconnect(void);

/**
 * Returns true when a BLE HID device is connected and able to send reports.
 * Distinct from bluetooth_is_connected() which only indicates NimBLE host sync.
 */
bool bluetooth_is_device_connected(void);

/** Returns true when hosted controller and NimBLE host are initialized. */
bool bluetooth_is_enabled(void);

/** Returns true when NimBLE host synchronization is complete. */
bool bluetooth_is_connected(void);

#endif