/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file wifi_known.h
 * @brief Persistent known Wi-Fi network list (sd:/WIFI.KNOWN).
 *
 * Part of components/networking — the single owner of Wi-Fi behaviour. This
 * module keeps an in-memory cache of previously-used networks (SSID +
 * password + metadata) and persists it to the SD card as a plain, hand-editable
 * text file.
 *
 * Safety contract (non-negotiable):
 *   - All SD I/O goes through the guarded storage session API
 *     (shell_sd_begin / shell_sd_end) and tolerates failure at every step.
 *   - No SD card, mount failure, missing/corrupt file, read-only or full disk
 *     → empty list, no crash, no freeze, no infinite retry. Callers fall back
 *     to the single-credential path.
 *   - Passwords are stored in the file but NEVER printed to the transcript,
 *     history, debug log, or UART. `wifi known` shows SSIDs only.
 *   - The file is only ever rewritten atomically (temp file + rename) with a
 *     free-space pre-check and partial-file cleanup on failure.
 */

#ifndef P4MINISHELL_WIFI_KNOWN_H
#define P4MINISHELL_WIFI_KNOWN_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi.h"

#include "networking.h"
#include "p4minishell_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/** One known-network entry (in-memory + file line). */
typedef struct {
    char ssid[NETWORKING_WIFI_SSID_BYTES];           /**< Network SSID. */
    char password[NETWORKING_WIFI_PASSWORD_BYTES];   /**< Password (may be empty = open). */
    int authmode;                                    /**< wifi_auth_mode_t as int. */
    int priority;                                    /**< Higher = preferred order (0-100). */
    bool preferred;                                  /**< Explicit preferred flag. */
    int64_t last_connected;                          /**< Unix seconds of last success, 0 = never. */
    int connect_count;                               /**< Number of successful connections. */
} networking_wifi_known_entry_t;

/**
 * Bind the host render/log callbacks (from networking.c) and (re)create the
 * module mutex. Safe to call once from networking_init().
 */
void networking_wifi_known_init(const networking_host_ops_t *ops);

/**
 * Load the known-network list from the SD card into the in-memory cache.
 * Non-blocking and fully safe: any failure (no card, no mount, missing or
 * corrupt file) leaves an empty list and returns a non-OK error without
 * printing anything. The cache is retained until the next load.
 *
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND when the file is absent or the
 *         card is unavailable, or an I/O error. Callers treat any non-OK as
 *         "no known networks".
 */
esp_err_t networking_wifi_known_load(void);

/**
 * Persist the in-memory known-network cache to the SD card. Atomic rewrite
 * (temp file + rename), free-space pre-checked, partial-file cleanup on
 * failure. Safe to call when the card is absent (returns an error, no crash).
 *
 * @return ESP_OK on success, or a non-OK error (mount, space, I/O).
 */
esp_err_t networking_wifi_known_save(void);

/** Number of entries currently cached. */
int networking_wifi_known_count(void);

/**
 * Copy an entry by index.
 *
 * @param index  0-based index into the cache.
 * @param out    Receives the entry. Must not be NULL.
 * @return true when the index is valid.
 */
bool networking_wifi_known_get(int index, networking_wifi_known_entry_t *out);

/**
 * Update or insert an entry (deduplicated by SSID, case-insensitive).
 *
 * @param ssid           Network SSID (must not be NULL/empty).
 * @param password       Password; an empty string keeps the existing password
 *                       when the entry already exists.
 * @param authmode       wifi_auth_mode_t, or -1 to keep the existing value.
 * @param mark_connected When true, bump connect_count and stamp last_connected.
 * @return ESP_OK (persisted), ESP_ERR_NO_MEM if the list is full and nothing
 *         could be evicted, or the save error (still cached in memory).
 */
esp_err_t networking_wifi_known_upsert(const char *ssid, const char *password,
                                       int authmode, bool mark_connected);

/** Remove an entry by SSID and persist. Returns ESP_OK even when not found. */
esp_err_t networking_wifi_known_remove(const char *ssid);

/** Remove every entry and persist (empty file). */
esp_err_t networking_wifi_known_clear(void);

/** Mark or unmark an entry as preferred and persist. */
esp_err_t networking_wifi_known_set_preferred(const char *ssid, bool preferred);

/**
 * Pick the best visible known network from a scan result set.
 *
 * Selection order: any entry marked preferred first (highest priority then
 * strongest RSSI among them), otherwise highest priority then strongest RSSI.
 * An SSID hidden from scan results (empty in the record) never matches.
 *
 * @param records  Scan results (must not be NULL).
 * @param count    Number of records.
 * @return Index into the cache of the best visible entry, or -1 when none of
 *         the known networks is visible.
 */
int networking_wifi_known_pick_visible(const wifi_ap_record_t *records, uint16_t count);

/**
 * Print the known networks (SSIDs only, with preferred / priority / last-used
 * markers). Never prints passwords. When the SD card is unavailable, prints a
 * clear message and returns non-OK so the caller can set ERRORLEVEL.
 *
 * @return ESP_OK when the list was shown (even if empty), or
 *         ESP_ERR_NOT_FOUND when the SD card is unavailable.
 */
esp_err_t networking_wifi_known_list(void);

/**
 * Record a successful connection to the given network: upsert the entry
 * (marking it connected) and, when P4_CONFIG_WIFI_KNOWN_AUTOSAVE is enabled,
 * persist to SD. Fully safe when the card is absent. Called from the GOT_IP
 * event handler.
 */
void networking_wifi_known_record_connect(const char *ssid, const char *password);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_WIFI_KNOWN_H */
