/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file certs.h
 * @brief TLS trust store management for P4MiniShell.
 *
 * Manages a certificate store on the SD card (sd:/CERTS/) and loads the
 * certificates into the mbedTLS global CA store at boot.  httpget and c6ota
 * use the combined trust set (user certificates + compiled-in Mozilla bundle)
 * automatically.
 *
 * The `certs` command (components/command/certs_commands.c) drives the store
 * from the shell; this module provides the pure core:
 *   - certs_load_sd_store()   — scan sd:/CERTS, build PEM buffer, set global CA
 *   - certs_reload()          — re-load after add/remove/rebuild
 *   - certs_get_store_info()  — trust store status (cert count, buffer size)
 *   - certs_clear()           — wipe the loaded store (reboot or rebuild)
 */

#ifndef P4MINISHELL_CERTS_H
#define P4MINISHELL_CERTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Snapshot of the current trust store for `certs info`. */
typedef struct {
    uint32_t pem_bytes;       /**< Bytes in the concatenated PEM buffer (0 = none). */
    int pem_files;            /**< Number of PEM files loaded. */
    int der_files;            /**< Number of DER files loaded. */
    bool global_store_active; /**< true when the mbedTLS global CA store is set. */
    bool sd_available;        /**< true when the SD CERTS directory was accessible. */
} certs_store_info_t;

/**
 * Scan sd:/CERTS for PEM and DER certificate files, concatenate them, and
 * register the result as the mbedTLS global CA store.  Idempotent; calling
 * this when the store is already loaded is a lightweight no-op.
 *
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND when the CERTS directory is
 *         empty or absent, or an error from file I/O / mbedTLS.
 */
esp_err_t certs_load_sd_store(void);

/** Re-load the SD certificate store (used after add/remove/rebuild). */
esp_err_t certs_reload(void);

/** Fill @p out with the current trust store status. */
void certs_get_store_info(certs_store_info_t *out);

/** Clear the loaded trust store from memory.  The SD files are untouched. */
void certs_clear(void);

/** True when the global CA store has been loaded and is active. */
bool certs_is_loaded(void);

/** Scan sd:/CERTS for *.pem files and write their concatenation to
 *  sd:/CERTS/BUNDLE.PEM.  Used by `certs rebuild`. */
esp_err_t certs_rebuild_bundle(void);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_CERTS_H */
