/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file portal.h
 * @brief Captive-portal Wi-Fi setup for first-run without an SD card.
 *
 * When the device boots without a configured Wi-Fi network the portal
 * starts a SoftAP + DNS server + HTTP captive-portal page.  A client
 * that joins the AP is redirected to the portal page where they enter
 * their home SSID and password.  The credentials are saved to the
 * known-network list and the portal shuts down.
 *
 * Public surface:
 *   portal_start()      — start the SoftAP + DNS + HTTP portal
 *   portal_stop()       — stop everything and release resources
 *   portal_is_active()  — true while the portal is running
 *   portal_should_autostart() — true when first-run conditions are met
 */

#ifndef P4MINISHELL_PORTAL_H
#define P4MINISHELL_PORTAL_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start the captive portal (SoftAP + DNS + HTTP).
 *  Safe to call when already active (no-op). */
esp_err_t portal_start(void);

/** Stop the captive portal and release all resources.
 *  Safe to call when not active (no-op). */
esp_err_t portal_stop(void);

/** True while the portal is running. */
bool portal_is_active(void);

/** True when the portal has been requested to stop (credential submission). */
bool portal_should_stop(void);

/** True when the device should auto-start the portal:
 *  SD card is not mounted, no known networks, and no CONFIG.SYS
 *  credentials.  Intended to be called from the boot hook. */
bool portal_should_autostart(void);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_PORTAL_H */
