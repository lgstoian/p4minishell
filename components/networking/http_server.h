/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
#ifndef P4MINISHELL_HTTP_SERVER_H
#define P4MINISHELL_HTTP_SERVER_H

/**
 * @file http_server.h
 * @brief Lightweight read-only HTTP file server for P4MiniShell.
 *
 * Serves the SD card over the Wi-Fi link through the esp_http_server driver.
 * The URL path maps onto files under the SD mount point; directories get an
 * HTML listing. Optional HTTP Basic authentication is controlled by
 * P4_CONFIG_HTTPD_AUTH_USERNAME / _PASSWORD (empty username = off).
 *
 * The server is the sole owner of the esp_http_server surface in the
 * firmware, mirroring how networking.c owns esp_http_client. Its lifecycle is
 * tied to Wi-Fi events: it auto-starts on IP_EVENT_STA_GOT_IP and stops on
 * WIFI_EVENT_STA_DISCONNECTED (both gated by P4_CONFIG_HTTPD_AUTOSTART for
 * the start side), and it can also be driven manually with `httpd start` /
 * `httpd stop`.
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/** HTTP server lifecycle state. */
typedef enum {
    NETWORKING_HTTPD_STATE_STOPPED = 0,
    NETWORKING_HTTPD_STATE_STARTING,
    NETWORKING_HTTPD_STATE_RUNNING,
    NETWORKING_HTTPD_STATE_FAILED,
} networking_httpd_state_t;

/** Snapshot of the server for `httpd status`. */
typedef struct {
    networking_httpd_state_t state;
    bool auth_enabled;
    bool sd_mounted;
    uint16_t port;
    uint16_t open_sockets;
    uint16_t max_sockets;
    uint32_t request_count;
    esp_err_t last_error;
} networking_httpd_status_t;

/** Start the HTTP file server. Requires an active Wi-Fi connection. */
esp_err_t networking_httpd_start(void);

/** Stop the HTTP file server if it is running. */
esp_err_t networking_httpd_stop(void);

/** Print a `httpd status` report to the transcript. */
void networking_httpd_status(void);

/** Report whether the server task is currently running. */
bool networking_httpd_is_running(void);

/** Lifecycle hook: auto-start the server after a successful DHCP lease.
 *  No-op when P4_CONFIG_HTTPD_AUTOSTART is 0 or the server already runs. */
void networking_httpd_maybe_autostart(void);

/** Lifecycle hook: stop the server on Wi-Fi disconnect. */
void networking_httpd_maybe_stop(void);

#endif /* P4MINISHELL_HTTP_SERVER_H */
