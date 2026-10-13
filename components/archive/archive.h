/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file archive.h
 * @brief Archive management for P4MiniShell: USTAR (.p4a) and ZIP (.zip).
 *
 * Unified archive interface supporting two formats:
 *  - USTAR (.p4a): store-only POSIX tar with CRC manifest trailer.
 *    Host tar extracts the container fine; `archive verify` checks every
 *    member against the manifest.
 *  - ZIP (.zip): standard ZIP with stored (0) and deflate (8) compression.
 *    No ZIP64, no encryption, no spanning; CRC-32 per member.
 *
 * Both formats share the same member model and unified verbs:
 *  - `archive create <file> <path> [paths...]`
 *  - `archive extract <file> [dest]`
 *  - `archive list <file> [/b]`
 *  - `archive verify <file>`
 *  - `backup <file> [paths...]` (USTAR only, for system backups)
 *
 * Format is detected by file extension (.p4a = USTAR, .zip = ZIP) or
 * by magic bytes when extension is ambiguous.
 *
 * Layout rules: member paths are relative with `/` separators (a leading
 * source directory keeps its basename, e.g. `sd:/DOCS` -> `DOCS/...`);
 * names beyond format limits are skipped and counted, never truncated;
 * absolute or `..`-escaping members are refused on extract.
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

/** Archive format. */
typedef enum {
    ARCHIVE_FORMAT_USTAR = 0,  /**< USTAR (.p4a) with CRC manifest. */
    ARCHIVE_FORMAT_ZIP   = 1,  /**< ZIP (.zip) with CRC-32 per member. */
    ARCHIVE_FORMAT_AUTO  = 2,  /**< Detect by extension or magic bytes. */
} archive_format_t;

/** Manifest member name (last entry of USTAR archives we write). */
#define ARCHIVE_MANIFEST_NAME "P4CRC.MANIFEST"

/** Longest member path (ZIP 256, USTAR 256 via prefix+name). */
#define ARCHIVE_PATH_MAX 256

/** Archive format bitmask for create-time selection. */
#define ARCHIVE_FORMAT_USTAR_BIT (1u << ARCHIVE_FORMAT_USTAR)
#define ARCHIVE_FORMAT_ZIP_BIT   (1u << ARCHIVE_FORMAT_ZIP)

/** One archive member (list callback). */
typedef struct {
    char path[ARCHIVE_PATH_MAX + 1]; /**< Relative path (dirs end '/'). */
    uint64_t size;       /**< Payload bytes (0 for directories). */
    int64_t mtime;       /**< Modification time (unix seconds). */
    bool is_dir;         /**< True for directory entries. */
    uint32_t crc;        /**< CRC-32 (from manifest for USTAR, from ZIP local header). */
    bool crc_known;      /**< True when the format carried this member's CRC. */
    archive_format_t format; /**< Format of this archive (for context). */
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
 * Format is selected by @p format (or auto-detected by extension if AUTO).
 * Writes `<archive>.tmp` then renames over any existing file.
 * For USTAR, appends `P4CRC.MANIFEST` trailer.
 */
esp_err_t archive_create(const char *archive_path, const char *const *sources,
                         int source_count, archive_stats_t *stats,
                         archive_format_t format);

/**
 * Extract an archive under @p dest_dir (created when missing).
 * Format is auto-detected by extension or magic bytes.
 * The USTAR manifest member is consumed, never materialized.
 * Unsafe paths are skipped.
 */
esp_err_t archive_extract(const char *archive_path, const char *dest_dir,
                          archive_stats_t *stats);

/** Walk members without writing (`list`; USTAR manifest is skipped). */
esp_err_t archive_list(const char *archive_path, archive_list_cb_t cb, void *ctx);

/** Re-read every member and check it against the manifest (USTAR) or CRC (ZIP). */
esp_err_t archive_verify(const char *archive_path, archive_stats_t *stats);

/* ------------------------------------------------------------------------
 * Pure format cores (no SD, no commands — unit-tested)
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

/* ---------------------------------------------------------------------
 * ZIP support
 * ---------------------------------------------------------------------
 * The ZIP reader (open/close/get_entry/find_entry/extract/list plus the
 * pure header parsers) lives in components/zip and is declared in <zip.h>.
 * It used to be duplicated here; archive now delegates to it, so there is
 * exactly one ZIP implementation and one CRC-32 primitive
 * (strutil_crc32_update()).
 */

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_ARCHIVE_H */
