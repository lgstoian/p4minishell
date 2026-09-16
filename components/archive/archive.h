/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file archive.h
 * @brief USTAR (.p4a) backup archives for P4MiniShell.
 *
 * Store-only POSIX tar (no compression: zero new dependencies, streamable,
 * host-compatible) with a trailing `P4CRC.MANIFEST` member carrying one
 * `crc32 size path` line per file. Host tar extracts the container fine
 * (the manifest lands as a regular file); `archive verify` checks every
 * member against it.
 *
 * Layout rules: member paths are relative with `/` separators (a leading
 * source directory keeps its basename, e.g. `sd:/DOCS` -> `DOCS/...`);
 * names beyond the USTAR 100+155 split are skipped and counted, never
 * truncated; absolute or `..`-escaping members are refused on extract.
 * Directory mtimes are stored for host fidelity but not restored on the
 * device (documented).
 *
 * Every operation opens its own guarded SD session, pre-checks free space,
 * streams in chunk-sized pieces through heap buffers, and writes atomically
 * (temp + rename). This is a leaf component (storage + shell only); the
 * `archive`/`backup`/`restore` verbs live in components/command/.
 */

#ifndef P4MINISHELL_ARCHIVE_H
#define P4MINISHELL_ARCHIVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Manifest member name (last entry of every archive we write). */
#define ARCHIVE_MANIFEST_NAME "P4CRC.MANIFEST"

/** Longest USTAR member path (155 prefix + 1 slash + 100 name). */
#define ARCHIVE_PATH_MAX 256

/** One archive member (list callback). */
typedef struct {
    char path[ARCHIVE_PATH_MAX + 1]; /**< Relative path (dirs end '/'). */
    uint64_t size;       /**< Payload bytes (0 for directories). */
    int64_t mtime;       /**< Modification time (unix seconds). */
    bool is_dir;         /**< True for directory entries. */
    uint32_t crc;        /**< CRC-32 from the manifest (0 when absent). */
    bool crc_known;      /**< True when the manifest carried this member. */
} archive_entry_t;

/** Running totals for create/extract/verify. */
typedef struct {
    int files;           /**< File members written/read. */
    int dirs;            /**< Directory members written/read. */
    int skipped;         /**< Entries skipped (long names, I/O, unsafe). */
    uint64_t bytes;      /**< Payload bytes moved. */
} archive_stats_t;

/** List callback: receives one member. Return false to stop early. */
typedef bool (*archive_list_cb_t)(const archive_entry_t *entry, void *ctx);

/* ------------------------------------------------------------------------
 * Store operations (resolved VFS paths; each opens its own SD session)
 * ---------------------------------------------------------------------- */

/**
 * Create an archive from files/directories (each kept under its basename).
 * Writes `<archive>.tmp` then renames over any existing file.
 */
esp_err_t archive_create(const char *archive_path, const char *const *sources,
                         int source_count, archive_stats_t *stats);

/**
 * Extract an archive under @p dest_dir (created when missing). The manifest
 * member is consumed, never materialized. Unsafe paths are skipped.
 */
esp_err_t archive_extract(const char *archive_path, const char *dest_dir,
                          archive_stats_t *stats);

/** Walk members without writing (`list`; the manifest is skipped). */
esp_err_t archive_list(const char *archive_path, archive_list_cb_t cb, void *ctx);

/** Re-read every member and check it against the manifest. */
esp_err_t archive_verify(const char *archive_path, archive_stats_t *stats);

/* ------------------------------------------------------------------------
 * Pure format core (no SD, no commands — unit-tested)
 * ---------------------------------------------------------------------- */

/** Incremental CRC-32 (IEEE 802.3, init 0xFFFFFFFF, invert at the end). */
uint32_t archive_crc32_update(uint32_t crc, const uint8_t *data, size_t len);

/** Write an unsigned value as a NUL-terminated octal field. */
void archive_octal(uint64_t value, char *out, size_t out_size);

/** Read a NUL/space-terminated octal field. False on malformed input. */
bool archive_unoctal(const char *in, size_t in_size, uint64_t *out);

/** Format one 512-byte USTAR header for @p e (typeflag/mode/magic set).
 *  The caller must ensure archive_entry_fits(e->path): over-long paths
 *  are clamped, never rejected, here. */
void archive_format_header(const archive_entry_t *e, uint8_t block[512]);

/**
 * Parse + checksum-verify one 512-byte header.
 * @return 1 member, 0 zero block (end marker), -1 corrupt.
 */
int archive_parse_header(const uint8_t block[512], archive_entry_t *out);

/** True when @p path is safe to materialize (relative, no `..` escape). */
bool archive_path_safe(const char *path);

/** True when @p entry fits the USTAR 100+155 name/prefix split. */
bool archive_entry_fits(const char *entry);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_ARCHIVE_H */
