/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file applib_db.h
 * @brief Database group of the applib (Palm-OS-style SD record store).
 *
 * Native apps can create and query named, SD-backed databases (under
 * sd:/DBS/<name>.DB/) the same way the `db` shell command does. Every wrapper
 * NULL-checks and returns false on failure, matching the other applib groups.
 * All database data lives on the SD card.
 */

#ifndef P4MINISHELL_APPLIB_DB_H
#define P4MINISHELL_APPLIB_DB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "db.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Create a new database (fails if it already exists). */
bool app_db_create(const char *name, const char *creator, const char *type,
                   uint32_t version);

/** Read a database's summary (name/creator/type/version/counts). */
bool app_db_info(const char *name, db_info_t *out);

/** Delete an entire database tree. */
bool app_db_drop(const char *name);

/** Set a category label (empty clears it). */
bool app_db_category_set(const char *name, uint8_t cat, const char *label);

/** Read a category label. */
bool app_db_category_get(const char *name, uint8_t cat, char *label, size_t size);

/** Add a record; returns the new id in @p out_id. */
bool app_db_add(const char *name, uint8_t cat, const char *key, bool secret,
                const void *data, size_t len, uint32_t *out_id);

/** Read a record's payload; @p inout_len is capacity in / payload out. */
bool app_db_get(const char *name, uint32_t id, void *buf, size_t *inout_len,
                bool reveal_secret, db_record_info_t *info);

/** Update a record (id unchanged). */
bool app_db_set(const char *name, uint32_t id, uint8_t cat, const char *key,
                bool secret, const void *data, size_t len);

/** Delete a record (soft by default, permanent when @p permanent is true). */
bool app_db_del(const char *name, uint32_t id, bool permanent);

/** Physically remove every soft-deleted record. */
bool app_db_purge(const char *name);

/** Count live records (cat_filter 0xFF = any). */
bool app_db_count(const char *name, uint8_t cat_filter, int *out_count);

/** Linear find over live records; callback receives each match. */
typedef bool (*app_db_find_cb)(const db_record_info_t *info, void *ctx);
bool app_db_find(const char *name, uint8_t cat_filter, const char *key_filter,
                 const char *text_filter, bool ignore_case, bool reveal_secret,
                 app_db_find_cb cb, void *ctx, int *out_count);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_APPLIB_DB_H */
