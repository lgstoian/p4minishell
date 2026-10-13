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
#include <stddef.h>
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

/** Stash credentials for a STA connect attempt after the portal stops.
 *  The connect runs once on the event-loop task right after teardown, so the
 *  APSTA->STA mode switch cannot race it. Cleared by portal_stop(). */
void portal_request_connect_after_stop(const char *ssid, const char *password);

/** Run the stashed post-stop connect if one is pending (exactly once).
 *  Called from the event-loop task after portal_stop(). */
void portal_connect_pending_after_stop(void);

/** True when the device should auto-start the portal:
 *  SD card is not mounted, no known networks, and no CONFIG.SYS
 *  credentials.  Intended to be called from the boot hook. */
bool portal_should_autostart(void);

/** HTML-escape for SSIDs interpolated into the portal page. Pure,
 *  unit-tested. */
size_t portal_html_escape(const char *in, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_PORTAL_H */
