/**
 * @file db.h
 * @brief Palm-OS-style SD-backed record store for P4MiniShell.
 *
 * Named databases live on the SD card under `sd:/DBS/<name>.DB/`:
 *
 *   HEADER.INI        name, creator, type, version, mod, next_id, record_count
 *   CATEGORIES.INI    0=Unfiled .. 15=...
 *   INDEX.TXT         one "id cat flags key size" line per record
 *   RECORDS/          R<8-hex-id>.DAT per record payload
 *
 * Every record carries a monotonic 32-bit id (from HEADER.INI:next_id, never
 * reused), a 0..15 category, a flags byte, an optional key, and a text/binary
 * payload. Records are soft-deleted by setting the deleted flag; `db purge`
 * physically removes them and rewrites the index.
 *
 * Every operation:
 *   - opens its own guarded SD session (`shell_sd_begin`/`shell_sd_end`)
 *   - pre-checks free space before any write
 *   - writes atomically (temp file + rename)
 *   - heap-allocates every command/record-sized buffer
 * This is a leaf component (depends only on `storage` + FreeRTOS/heap). It
 * never prints and never parses commands; the `db` command surface lives in
 * components/command/db_commands.c.
 */

#ifndef P4MINISHELL_DB_H
#define P4MINISHELL_DB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "p4minishell_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bit 6 of the creator field is the "dirty" flag in classic PalmOS; here we
 * store plain strings, so the creator/type are simple up-to-8-char ids. */

/** Public flags used by the shell; private bits are managed internally. */
#define DB_FLAG_SECRET      P4_CONFIG_DB_FLAG_SECRET
#define DB_FLAG_DELETED     P4_CONFIG_DB_FLAG_DELETED

/** A record's index-metadata (id, category, flags, key, payload size). */
typedef struct {
    uint32_t id;                 /**< Monotonic record id, never reused. */
    uint8_t  category;           /**< 0..P4_CONFIG_DB_CATEGORY_COUNT-1. */
    uint8_t  flags;              /**< DB_FLAG_SECRET / DB_FLAG_DELETED. */
    char     key[P4_CONFIG_DB_KEY_BYTES]; /**< Optional key (may be empty). */
    uint32_t size;               /**< Payload bytes in the .DAT file. */
} db_record_info_t;

/** Database identity/statistics (HEADER.INI contents + category counts). */
typedef struct {
    char name[P4_CONFIG_DB_NAME_BYTES];   /**< Database name (no .DB). */
    char creator[P4_CONFIG_DB_ID_BYTES];  /**< Creator id (8 chars max). */
    char type[P4_CONFIG_DB_ID_BYTES];     /**< Type id (8 chars max). */
    uint32_t version;                     /**< Creator-defined version. */
    uint32_t next_id;                     /**< Next id to hand out. */
    uint32_t record_count;                /**< Live (non-deleted) records. */
    uint32_t deleted_count;               /**< Soft-deleted records. */
    uint32_t created_sec;                 /**< Creation timestamp (unix). */
    uint32_t modified_sec;                /**< Last modification timestamp. */
} db_info_t;

/** Iteration callback for `db list`. Return false to stop early. */
typedef bool (*db_list_cb_t)(const char *name, void *ctx);

/** Find callback: receives one matching live record. Return false to stop. */
typedef bool (*db_find_cb_t)(const db_record_info_t *info, void *ctx);

/* ------------------------------------------------------------------------
 * Database lifecycle
 * ---------------------------------------------------------------------- */

/** Create a new database directory + HEADER/CATEGORIES. Fails if it exists. */
esp_err_t db_create(const char *name, const char *creator, const char *type,
                    uint32_t version);

/** Read a database's HEADER/CATEGORIES summary. */
esp_err_t db_info(const char *name, db_info_t *out);

/** Iterate every `*.DB` directory under sd:/DBS. */
esp_err_t db_list(db_list_cb_t cb, void *ctx);

/** Delete an entire database directory tree (and its records). */
esp_err_t db_drop(const char *name);

/* ------------------------------------------------------------------------
 * Categories (Palm-OS 16 categories)
 * ---------------------------------------------------------------------- */

/** Set the label of category @p cat (0..15). An empty label clears it. */
esp_err_t db_category_set(const char *name, uint8_t cat, const char *label);

/** Read the label of category @p cat into @p label (NUL-terminated). */
esp_err_t db_category_get(const char *name, uint8_t cat,
                          char *label, size_t label_size);

/** Iterate every non-empty category label. cb returns false to stop. */
esp_err_t db_categories_foreach(const char *name,
                                bool (*cb)(uint8_t cat, const char *label, void *ctx),
                                void *ctx);

/* ------------------------------------------------------------------------
 * Records
 * ---------------------------------------------------------------------- */

/** Add a record; returns the new id in @p out_id. Payload may be NULL for an
 *  empty record. */
esp_err_t db_add(const char *name, uint8_t cat, const char *key, bool secret,
                 const void *payload, size_t len, uint32_t *out_id);

/** Read a record's metadata + payload. @p buf may be NULL to fetch metadata
 *  only. On success @p inout_len holds the payload bytes copied (or the full
 *  payload size when @p buf was too small / NULL). */
esp_err_t db_get(const char *name, uint32_t id, void *buf, size_t *inout_len,
                 bool reveal_secret, db_record_info_t *out);

/** Update a record's metadata + payload (id never changes). */
esp_err_t db_set(const char *name, uint32_t id, uint8_t cat, const char *key,
                 bool secret, const void *payload, size_t len);

/** Soft-delete a record (@p permanent true physically removes it). */
esp_err_t db_del(const char *name, uint32_t id, bool permanent);

/** Physically remove every soft-deleted record and rewrite the index. */
esp_err_t db_purge(const char *name);

/* ------------------------------------------------------------------------
 * Find / count
 * ---------------------------------------------------------------------- */

/** Count live records, optionally restricted to a category (0xFF = any). */
esp_err_t db_count(const char *name, uint8_t cat_filter, int *out_count);

/** Linear scan of the index for live records, optional category/key/text
 *  filters (NULL = any). @p ignore_case makes key and payload text matches
 *  case-insensitive. Callback receives each match; stop with return false.
 *  @p out_count receives the number of matches (capped by P4_CONFIG_DB_FIND_MAX). */
esp_err_t db_find(const char *name, uint8_t cat_filter, const char *key_filter,
                  const char *text_filter, bool ignore_case, bool reveal_secret,
                  db_find_cb_t cb, void *ctx, int *out_count);

/* ------------------------------------------------------------------------
 * Export / import (portable text interchange)
 * ---------------------------------------------------------------------- */

/**
 * Export a database to a text file. Each live record becomes one line:
 *   id|cat|flags|key|payload
 * Payloads are written raw (a `|` inside the payload is preserved only when
 * the payload has none; otherwise import uses a length-prefixed form). For the
 * shell we use the simple form and reject `|` in payloads at add time is NOT
 * enforced; instead export writes the payload hex-encoded for binary safety.
 * @p file is a path (relative to cwd or an `sd:` path). NULL defaults to
 * sd:/DBS/<name>.EXPORT.
 */
esp_err_t db_export(const char *name, const char *file);

/**
 * Import records from a text file produced by db_export. Appends records with
 * fresh ids (never reuses an existing id). @p file NULL defaults to
 * sd:/DBS/<name>.EXPORT.
 */
esp_err_t db_import(const char *name, const char *file);

/* ------------------------------------------------------------------------
 * Current-database pointer (RAM-only, resets on reboot)
 * ---------------------------------------------------------------------- */

/** Set the current database name (RAM string; empty clears it). */
esp_err_t db_current_set(const char *name);

/** Copy the current database name into @p buf. Returns false when none set. */
bool db_current_get(char *buf, size_t size);

/** Validate a database name (length, no path separators / dots). */
bool db_name_valid(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_DB_H */
