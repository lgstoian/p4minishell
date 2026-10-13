/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file chat.h
 * @brief Board-to-board chat transport (leaf module, mirrors portal pattern).
 *
 * A minimal CLI chat between two P4MiniShell boards. One board runs a
 * hosted SoftAP (APSTA mode) with a bounded HTTP server; the other joins
 * via `wifi connect`. Both boards use the `chat` verb family. The transport
 * mirrors the captive portal's APSTA/DHCP/TX-cap pattern to avoid USB brownout
 * on self-powered boards. The chat is read-only (no remote command execution);
 * messages are text only.
 */

#ifndef CHAT_H
#define CHAT_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Opaque handle for the chat instance. Created by chat_start() and destroyed
 * by chat_stop(). The handle is not copyable.
 */
typedef struct chat_instance_s *chat_handle_t;

/**
 * Configuration for chat_start(). All fields are validated; on failure the
 * function returns an error and does not start the AP or HTTP server.
 */
typedef struct {
    /** SSID for the SoftAP (must be non-empty). */
    const char *ap_ssid;
    /** Wi-Fi channel for the SoftAP (1..13). */
    uint8_t ap_channel;
    /** Maximum simultaneous SoftAP client connections. */
    uint8_t ap_max_conn;
    /** RF TX power cap in quarter-dBm (e.g., 52 = 13 dBm). */
    int8_t ap_tx_power_qdbm;
    /** HTTP port for the chat server (typically 80). */
    uint16_t http_port;
    /** Maximum message bytes per send. */
    size_t msg_max_bytes;
    /** Maximum messages retained in the local inbox (per peer). */
    size_t inbox_max;
    /** Maximum messages retained in the local log (all peers). */
    size_t log_max;
    /** Maximum inactivity seconds before auto-stop (0 = no timeout). */
    uint32_t timeout_secs;
} chat_config_t;

/**
 * Initialize default chat_config_t with values from p4minishell_config.h.
 * The caller may then override specific fields before passing to chat_start().
 */
void chat_config_init_default(chat_config_t *config);

/**
 * Start the chat host: creates the SoftAP (APSTA mode), applies the TX power
 * cap, starts the DHCP server, and launches the HTTP server on the AP netif.
 *
 * This function must be called with the Wi-Fi init-task claim held (see
 * wifi_try_claim_init_task()/wifi_release_init_task() in networking.c). The
 * portal precedent does this in networking_handle_wifi_command().
 *
 * @param config  Validated configuration (may be a modified copy of the
 *                defaults from chat_config_init_default()).
 * @param out_handle  On success, receives the opaque chat instance handle.
 * @return ESP_OK on success; ESP_ERR_INVALID_ARG if config is invalid;
 *         ESP_ERR_INVALID_STATE if the AP is already running; ESP_FAIL on
 *         lower-layer failure (netif creation, wifi mode switch, httpd start).
 */
esp_err_t chat_start(const chat_config_t *config, chat_handle_t *out_handle);

/**
 * Stop the chat host: stops the HTTP server, destroys the SoftAP netif,
 * restores the previous TX power, and returns to STA-only mode.
 *
 * This function must be called with the Wi-Fi init-task claim held.
 *
 * @param handle  Opaque handle returned by chat_start().
 * @return ESP_OK on success; ESP_ERR_INVALID_STATE if the handle is invalid
 *         or the chat is not running.
 */
esp_err_t chat_stop(chat_handle_t handle);

/**
 * Check if the chat host is currently active.
 *
 * @param handle  Opaque handle returned by chat_start().
 * @return true if the chat AP and HTTP server are running.
 */
bool chat_is_active(chat_handle_t handle);

/**
 * Send a chat message to the peer. The message is delivered via HTTP POST
 * to the peer's `/chat` endpoint. The function blocks until the request
 * completes or times out.
 *
 * @param handle  Opaque handle returned by chat_start().
 * @param peer_ip  IPv4 address of the peer (string form, e.g. "192.168.4.2").
 * @param text  Message text (must not exceed config.msg_max_bytes).
 * @return ESP_OK on success; ESP_ERR_INVALID_ARG if text is too long;
 *         ESP_ERR_INVALID_STATE if chat is not active; ESP_ERR_TIMEOUT if
 *         the HTTP request times out; ESP_FAIL on lower-layer error.
 */
esp_err_t chat_send(chat_handle_t handle, const char *peer_ip, const char *text);

/**
 * Poll the chat inbox for new messages since a given sequence number.
 * This is the peer-side GET `/chat?since=<seq>` operation.
 *
 * @param handle  Opaque handle returned by chat_start().
 * @param since_seq  Return messages with sequence > since_seq.
 * @param out_messages  On success, receives a heap-allocated string containing
 *                      the new messages in the format "seq\tfrom_ip\ttext\n"
 *                      for each message. The caller MUST free this string.
 * @return ESP_OK on success (out_messages may be empty string if no new
 *         messages); ESP_ERR_INVALID_STATE if chat is not active; ESP_FAIL
 *         on lower-layer error.
 */
esp_err_t chat_poll(chat_handle_t handle, uint32_t since_seq, char **out_messages);

/**
 * Get the local message log (all messages sent and received).
 *
 * @param handle  Opaque handle returned by chat_start().
 * @param out_log  On success, receives a heap-allocated string containing
 *                 the log entries in the format "seq\tfrom_ip\ttext\n".
 *                 The caller MUST free this string.
 * @return ESP_OK on success; ESP_ERR_INVALID_STATE if chat is not active.
 */
esp_err_t chat_get_log(chat_handle_t handle, char **out_log);

/**
 * Clear the local inbox and log.
 *
 * @param handle  Opaque handle returned by chat_start().
 * @return ESP_OK on success; ESP_ERR_INVALID_STATE if chat is not active.
 */
esp_err_t chat_clear(chat_handle_t handle);

/**
 * Get chat status string (heap-allocated, caller frees). Contains active
 * state, AP IP, connected peer count, inbox/log sizes, and sequence number.
 *
 * @param handle  Opaque handle returned by chat_start().
 * @param out_status  On success, receives a heap-allocated status string.
 * @return ESP_OK on success; ESP_ERR_INVALID_STATE if chat is not active.
 */
esp_err_t chat_get_status(chat_handle_t handle, char **out_status);

/**
 * Internal: the onmsg hook runner. Called by the HTTP server when a new
 * message arrives. The callback is registered by the command layer and must
 * be NULL-checked.
 */
typedef void (*chat_onmsg_cb_t)(const char *from_ip, const char *text, uint32_t seq);

/**
 * Register the onmsg callback. The command layer calls this from `chat onmsg`
 * to install its hook. Passing NULL disables the hook.
 */
void chat_set_onmsg_cb(chat_handle_t handle, chat_onmsg_cb_t cb);

/**
 * Get the current sequence number (monotonic, per chat instance).
 */
uint32_t chat_get_seq(chat_handle_t handle);

/* ========================================================================
 * COMMAND-FACING HOST ENTRY POINTS
 * ========================================================================
 * The chat component owns its own singleton host instance, so the command
 * layer never holds a handle and never talks to chat_start()/chat_stop()
 * directly.
 *
 * These wrappers live here rather than in components/networking because chat
 * is a consumer of networking, not the other way round: an earlier set of
 * networking_chat_* shims called straight into chat_* from networking.c,
 * which inverted the layering (chat already REQUIRES networking) and could
 * never have compiled. Starting the host also takes networking's Wi-Fi
 * init-task claim, which is exactly what chat_start() requires and what the
 * captive portal does for its own SoftAP.
 *
 * Transcript output is deliberately NOT done here: presentation belongs to
 * the command layer, so these return results and let the caller print.
 */

/**
 * Start the chat host SoftAP + HTTP server.
 *
 * @param ssid         SSID override, or NULL/"" for P4_CONFIG_CHAT_AP_SSID.
 * @param out_ssid     Optional buffer for the SSID actually used (may be NULL).
 * @param out_ssid_size Size of @p out_ssid.
 * @param out_url      Optional buffer receiving "http://<ip>:<port>" so the
 *                     caller can show the reachable address (may be NULL).
 * @param out_url_size Size of @p out_url.
 * @return ESP_OK on success; ESP_ERR_INVALID_STATE when already active or
 *         when another owner holds the Wi-Fi init-task claim.
 */
esp_err_t chat_host_on(const char *ssid, char *out_ssid, size_t out_ssid_size,
                       char *out_url, size_t out_url_size);

/** Stop the host and release the Wi-Fi init-task claim. */
esp_err_t chat_host_off(void);

/** @return true while the host SoftAP + server are running. */
bool chat_host_is_active(void);

/** @return the singleton host handle, or NULL when no host is running. */
chat_handle_t chat_host_handle(void);

/** Send @p text to @p peer_ip through the running host. */
esp_err_t chat_host_send(const char *peer_ip, const char *text);

/** Fetch messages newer than @p since_seq. */
esp_err_t chat_host_poll(uint32_t since_seq, char **out_messages);

/** Fetch the full local log. */
esp_err_t chat_host_log(char **out_log);

/** Clear the inbox and log. */
esp_err_t chat_host_clear(void);

/** Fetch the host/peer status string. */
esp_err_t chat_host_status(char **out_status);

/**
 * Install the on-message hook.
 *
 * @param line  Batch line to run on message receive, or NULL/"" to clear.
 * @param cb    Callback invoked by the HTTP server on a new message, or NULL.
 */
esp_err_t chat_host_onmsg(const char *line, chat_onmsg_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif /* CHAT_H */