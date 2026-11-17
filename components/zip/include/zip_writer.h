/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file zip_writer.h
 * @brief The one ZIP writer: stored entries, zip64-free, EPUB-safe ordering.
 *
 * Read support lives in `zip.c` (the single ZIP/`.PKG` reader). This is its
 * write side and exists because EPUB is a ZIP: the `mimetype` entry must be
 * first, STORED and without an extra field, which no generic tool guarantees.
 * Entries are stored (method 0) so the writer needs no zlib and the output is
 * trivially valid; the documents it packages are small.
 *
 * @module zip
 * @layer leaf
 * @domain fs
 * @provides stored-entry ZIP writing (EPUB packaging)
 * @status stable
 * @test test/main/test_epub.c
 * @hard-rule ai-context.md#module-layering-rules
 */
#ifndef P4MINISHELL_ZIP_WRITER_H
#define P4MINISHELL_ZIP_WRITER_H

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Upper bound on entries in one archive (EPUB needs six). */
#define ZIP_WRITER_MAX_ENTRIES 24

typedef struct zip_writer zip_writer_t;

/**
 * Create (truncate) @p path and start an archive.
 * @return ESP_OK, or ESP_ERR_INVALID_ARG / ESP_FAIL when the file cannot be
 *         written (the card may be missing or full).
 */
esp_err_t zip_writer_create(const char *path, zip_writer_t **out_writer);

/**
 * Append one entry. Call this with the EPUB `mimetype` first: the writer emits
 * entries in call order, which is the order the EPUB spec requires for it.
 *
 * @param writer  Archive from zip_writer_create().
 * @param name    Entry path with '/' separators (no leading slash).
 * @param data    Bytes to store (may be NULL only when @p len is 0).
 * @param len     Byte count.
 */
esp_err_t zip_writer_add(zip_writer_t *writer, const char *name,
                         const void *data, size_t len);

/**
 * Write the central directory, close the file, and free the writer.
 * On failure the partial file is removed, so a broken archive can never be
 * mistaken for a good one.
 */
esp_err_t zip_writer_close(zip_writer_t *writer);

/** Free the writer and delete a partially written file. */
void zip_writer_abort(zip_writer_t *writer);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_ZIP_WRITER_H */
