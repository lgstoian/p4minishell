/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file zip.h
 * @brief Minimal ZIP reader for EPUB extraction.
 *
 * A bounded, streaming ZIP reader tailored for EPUB extraction:
 *  - Reads central directory from the end of the file
 *  - Supports stored (0) and deflate (8) compression methods
 *  - Extracts individual entries by name or index
 *  - Bounded memory usage (streaming extraction, no full archive in RAM)
 *  - No ZIP64 support (EPUBs are small); no encryption; no spanning
 */

#ifndef ZIP_H
#define ZIP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum ZIP entry path length (matches USTAR/EPUB conventions). */
#define ZIP_PATH_MAX 256

/** ZIP entry metadata. */
typedef struct {
    char path[ZIP_PATH_MAX + 1]; /**< Entry name (relative, '/' separators). */
    uint32_t compressed_size;    /**< Compressed size in bytes. */
    uint32_t uncompressed_size;  /**< Uncompressed size in bytes. */
    uint16_t compression_method; /**< 0 = stored, 8 = deflate. */
    uint32_t crc32;              /**< CRC-32 of uncompressed data. */
    uint32_t local_header_offset; /**< Offset of local file header. */
    bool is_dir;                 /**< True for directory entries. */
} zip_entry_t;

/** Running totals for extract/list operations. */
typedef struct {
    int entries;          /**< Total entries processed. */
    uint64_t bytes;       /**< Payload bytes moved. */
    int skipped;          /**< Entries skipped (unsupported, unsafe). */
} zip_stats_t;

/** List callback: receives one entry. Return false to stop early. */
typedef bool (*zip_list_cb_t)(const zip_entry_t *entry, void *ctx);

/**
 * Open a ZIP archive for reading.
 * @param archive_path  Path to the ZIP file (VFS path, e.g. "sd:/book.epub").
 * @param out_handle    Opaque handle for subsequent operations.
 * @return ESP_OK on success; ESP_ERR_NOT_FOUND, ESP_ERR_INVALID_ARG,
 *         ESP_ERR_NO_MEM, ESP_FAIL on corruption.
 */
esp_err_t zip_open(const char *archive_path, void **out_handle);

/**
 * Close a ZIP archive handle.
 * @param handle  Handle returned by zip_open().
 */
void zip_close(void *handle);

/**
 * Get the number of entries in the ZIP.
 * @param handle  Handle returned by zip_open().
 * @return Number of entries, or -1 on error.
 */
int zip_get_count(void *handle);

/**
 * Get an entry by index (0-based, central directory order).
 * @param handle  Handle returned by zip_open().
 * @param index   Entry index.
 * @param out_entry  Filled on success.
 * @return ESP_OK on success; ESP_ERR_INVALID_ARG if index out of range.
 */
esp_err_t zip_get_entry(void *handle, int index, zip_entry_t *out_entry);

/**
 * Find an entry by exact path name.
 * @param handle  Handle returned by zip_open().
 * @param path    Entry path (case-sensitive, '/' separators).
 * @param out_entry  Filled on success.
 * @return ESP_OK on success; ESP_ERR_NOT_FOUND if not found.
 */
esp_err_t zip_find_entry(void *handle, const char *path, zip_entry_t *out_entry);

/**
 * Extract a single entry to a destination file.
 * @param handle        Handle returned by zip_open().
 * @param entry         Entry to extract (from zip_get_entry/zip_find_entry).
 * @param dest_path     Destination file path (VFS path).
 * @param stats         Optional running stats.
 * @return ESP_OK on success; ESP_ERR_INVALID_ARG, ESP_ERR_NO_MEM,
 *         ESP_FAIL on decompression/corruption.
 */
esp_err_t zip_extract_entry(void *handle, const zip_entry_t *entry,
                            const char *dest_path);

/**
 * Extract all entries under a destination directory.
 * Creates directories as needed. Unsafe paths are skipped.
 * @param handle        Handle returned by zip_open().
 * @param dest_dir      Destination directory (created if missing).
 * @param stats         Optional running stats.
 * @return ESP_OK on success; ESP_ERR_INVALID_ARG, ESP_ERR_NO_MEM,
 *         ESP_FAIL on I/O or decompression errors.
 */
esp_err_t zip_extract_all(void *handle, const char *dest_dir, zip_stats_t *stats);

/**
 * Walk all entries and invoke callback for each.
 * The manifest/CRC entries are included; return false from callback to stop.
 * @param handle  Handle returned by zip_open().
 * @param cb      Callback per entry.
 * @param ctx     User context passed to callback.
 * @return ESP_OK on success; ESP_ERR_INVALID_ARG if handle invalid.
 */
esp_err_t zip_list(void *handle, zip_list_cb_t cb, void *ctx);

/* ------------------------------------------------------------------------- */
/* Pure format core (unit-testable, no VFS)                                 */
/* ------------------------------------------------------------------------- */

#define ZIP_LOCAL_HEADER_SIG   0x04034b50
#define ZIP_CENTRAL_DIR_SIG    0x02014b50
#define ZIP_END_CENTRAL_DIR_SIG 0x06054b50

#define ZIP_METHOD_STORED   0
#define ZIP_METHOD_DEFLATE  8

/** Parse a ZIP local file header (unit-testable, no I/O). */
bool zip_parse_local_header(const uint8_t *buf, size_t len,
                            uint16_t *method_out, uint32_t *crc32_out,
                            uint32_t *compressed_size_out, uint32_t *uncompressed_size_out,
                            uint16_t *name_len_out, uint16_t *extra_len_out);

/** Parse a central directory entry (unit-testable, no I/O). */
bool zip_parse_central_dir(const uint8_t *buf, size_t len,
                           uint16_t *method_out, uint32_t *crc32_out,
                           uint32_t *compressed_size_out, uint32_t *uncompressed_size_out,
                           uint16_t *name_len_out, uint16_t *extra_len_out, uint16_t *comment_len_out,
                           uint32_t *local_offset_out, bool *is_dir_out, char *name_out, size_t name_cap);

/** Parse end-of-central-directory record (unit-testable, no I/O). */
bool zip_parse_eocd(const uint8_t *buf, size_t len,
                    uint16_t *disk_num, uint16_t *disk_cd_num,
                    uint16_t *cd_entries_this_disk, uint16_t *cd_entries_total,
                    uint32_t *cd_size, uint32_t *cd_offset, uint16_t *comment_len);

/** True when @p path is safe to materialize (relative, no `..` escape). */
bool zip_path_safe(const char *path);

/** Incremental CRC-32 (IEEE 802.3, init 0xFFFFFFFF, invert at end). */
uint32_t zip_crc32_update(uint32_t crc, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* ZIP_H */