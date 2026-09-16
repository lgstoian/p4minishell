/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
#ifndef P4MINISHELL_C6OTA_H
#define P4MINISHELL_C6OTA_H

/**
 * @file c6ota.h
 * @brief ESP32-C6 OTA update module via ESP-Hosted SDIO.
 *
 * Owns the full C6 firmware update workflow:
 *   - Source parsing (sd:/path, http[s]://url, or "default")
 *   - Confirmation flow with exact YES prompt
 *   - HTTP/HTTPS download with Wi-Fi readiness wait
 *   - ESP-IDF image header validation (magic 0xE9 + chip ID 0x000D)
 *   - Wi-Fi stop/restore around the OTA transfer
 *   - Hosted OTA RPC sequence (begin/write/end/activate)
 *   - Progress reporting every 5%
 *   - Success/failure transcript output
 *
 * Depends on components/networking for Wi-Fi wait and restore hooks.
 */

#include <stdbool.h>

/** Progress callback type. percent is 0-100 for live progress; negative values are reserved for non-progress messages. */
typedef void (*c6ota_progress_callback_t)(int percent, const char *msg);

/** Reset module state. Call once at boot after transcript path is ready. */
void c6ota_init(void);

/** Start the OTA workflow for the given source. NULL or unsupported source emits usage text. */
void c6ota_perform(const char *source);

/** Register an optional callback for transcript-formatted progress and status messages. */
void c6ota_register_progress_callback(c6ota_progress_callback_t cb);

/** Try to handle input as a YES/NO confirmation reply. Returns true if consumed. */
bool c6ota_try_handle_input(const char *input);

/** Returns true while an OTA transfer is in progress. */
bool c6ota_is_busy(void);

/** Returns true when the module is waiting for YES/NO confirmation. */
bool c6ota_is_confirmation_pending(void);

#endif