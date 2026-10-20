/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file error_strings.h
 * @brief Centralized error code → message mapping.
 *
 * Single source of truth for every error code the firmware reports. Batch
 * scripts can read `%ERRMSG%` after a failing command; the `help` command
 * shows details for known codes; `shell_print_error()` resolves a code via
 * `error_code_string()` before formatting.
 */

#ifndef BATCH_ERROR_STRINGS_H
#define BATCH_ERROR_STRINGS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Error code range tags (kept compact; groups are contiguous) */
/* -------------------------------------------------------------------------- */

/** Generic/command-layer errors (0–99). */
#define ERR_BASE_OK             0
#define ERR_BASE_ERROR          1
#define ERR_BASE_USAGE          2
#define ERR_BASE_NOENT          3
#define ERR_BASE_IO             5
#define ERR_BASE_NOMEM          7
#define ERR_BASE_INVAL          8
#define ERR_BASE_NOTSUP         9

/** Storage/filesystem errors (100–199). */
#define ERR_STORAGE_BASE        100
#define ERR_STORAGE_FULL        101
#define ERR_STORAGE_WRITE_PROT  102
#define ERR_STORAGE_CORRUPT     103
#define ERR_STORAGE_NO_SD       104
#define ERR_STORAGE_NO_USB      105
#define ERR_STORAGE_LOOP        106

/** Network errors (200–299). */
#define ERR_NET_BASE            200
#define ERR_NET_DISCONNECTED    201
#define ERR_NET_TIMEOUT         202
#define ERR_NET_DNS             203
#define ERR_NET_TLS             204
#define ERR_NET_HTTP            205
#define ERR_NET_WIFI_START      206
#define ERR_NET_BT              207

/** App/package errors (300–399). */
#define ERR_APP_BASE            300
#define ERR_APP_NOT_FOUND       301
#define ERR_APP_CRC             302
#define ERR_APP_SIGN            303
#define ERR_APP_COMPAT          304
#define ERR_APP_LAUNCH          305

/** Hardware errors (400–499). */
#define ERR_HW_BASE             400
#define ERR_HW_AUDIO            401
#define ERR_HW_DISPLAY          402
#define ERR_HW_TOUCH            403
#define ERR_HW_BATTERY          404
#define ERR_HW_CAMERA           405
#define ERR_HW_IMU              406

/** Return a human-readable string for @p code. Returns "Unknown error" for
 * codes not in the table. Never returns NULL. */
const char *error_code_string(int code);

/** Count of enumerated error codes (for iteration / selftest bounds). */
size_t error_code_count(void);

#ifdef __cplusplus
}
#endif

#endif /* BATCH_ERROR_STRINGS_H */
