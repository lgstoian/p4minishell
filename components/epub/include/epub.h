/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file epub.h
 * @brief EPUB reader for P4MiniShell.
 *
 * EPUB is a ZIP-based archive format for digital books containing:
 *  - OEBPS/ or similar directory with content
 *  - META-INF/container.xml pointing to the package document (OPF)
 *  - Package document (OPF) with metadata, manifest, spine, and guide
 *  - Navigation document (NCX or EPUB3 nav.xhtml) for TOC
 *  - HTML content documents (chapters)
 *  - CSS, images, fonts
 *
 * This component provides:
 *  - EPUB archive opening and validation
 *  - OPF parsing for metadata, manifest, and spine
 *  - NCX/nav.xhtml parsing for table of contents
 *  - Chapter extraction and rendering via html_render_ansi()
 *  - Spine-based navigation (prev/next chapter)
 *  - TOC-based navigation (jump to chapter by title)
 */

#ifndef EPUB_H
#define EPUB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Constants                                                                 */
/* ------------------------------------------------------------------------- */

#define EPUB_PATH_MAX         256
#define EPUB_TITLE_MAX        128
#define EPUB_AUTHOR_MAX       128
#define EPUB_CHAPTER_MAX      256
#define EPUB_TOC_ENTRY_MAX    64

/* ------------------------------------------------------------------------- */
/* Types                                                                     */
/* ------------------------------------------------------------------------- */

typedef struct epub_t epub_handle_t;

/** EPUB metadata from OPF. */
typedef struct {
    char title[EPUB_TITLE_MAX];
    char author[EPUB_AUTHOR_MAX];
    char language[16];
    char identifier[64];
    char publisher[64];
    char date[32];
    char description[256];
} epub_metadata_t;

/** A single chapter/spine item. */
typedef struct {
    char id[64];
    char href[EPUB_PATH_MAX];
    char title[EPUB_TITLE_MAX];
    int spine_index;          /**< Position in spine (0-based). */
    bool linear;              /**< Part of linear reading order. */
} epub_chapter_t;

/** Table of contents entry (from NCX or nav.xhtml). */
typedef struct {
    char label[EPUB_TITLE_MAX];
    char href[EPUB_PATH_MAX];
    int chapter_index;        /**< Index into epub_chapter_t array, or -1. */
    int depth;                /**< Nesting depth (0 = top level). */
} epub_toc_entry_t;

/** Complete EPUB navigation state. */
typedef struct {
    const epub_chapter_t *current;
    int current_index;        /**< 0-based index in spine. */
    int total_chapters;
} epub_nav_t;

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

/**
 * Open an EPUB archive for reading.
 * @param archive_path  Path to .epub file (VFS path, e.g. "sd:/book.epub").
 * @param out_handle    Opaque handle for subsequent operations.
 * @return ESP_OK on success; ESP_ERR_NOT_FOUND, ESP_ERR_INVALID_ARG,
 *         ESP_ERR_NO_MEM, ESP_FAIL on corruption/invalid format.
 */
esp_err_t epub_open(const char *archive_path, epub_handle_t **out_handle);

/**
 * Close an EPUB handle.
 * @param handle  Handle returned by epub_open().
 */
void epub_close(epub_handle_t *handle);

/**
 * Get EPUB metadata (from OPF).
 * @param handle  Handle returned by epub_open().
 * @param out_meta  Filled on success.
 * @return ESP_OK on success; ESP_ERR_INVALID_STATE if not parsed.
 */
esp_err_t epub_get_metadata(const epub_handle_t *handle, epub_metadata_t *out_meta);

/**
 * Get the spine (linear reading order chapters).
 * @param handle  Handle returned by epub_open().
 * @param out_chapters  Caller-allocated array of epub_chapter_t.
 * @param max_chapters  Size of out_chapters array.
 * @param out_count  Number of chapters written.
 * @return ESP_OK on success.
 */
esp_err_t epub_get_spine(const epub_handle_t *handle,
                         epub_chapter_t *out_chapters,
                         int max_chapters,
                         int *out_count);

/**
 * Get the table of contents (from NCX or nav.xhtml).
 * @param handle  Handle returned by epub_open().
 * @param out_toc  Caller-allocated array of epub_toc_entry_t.
 * @param max_entries  Size of out_toc array.
 * @param out_count  Number of entries written.
 * @return ESP_OK on success.
 */
esp_err_t epub_get_toc(const epub_handle_t *handle,
                       epub_toc_entry_t *out_toc,
                       int max_entries,
                       int *out_count);

/**
 * Get current navigation state.
 * @param handle  Handle returned by epub_open().
 * @param out_nav  Filled on success.
 * @return ESP_OK on success.
 */
esp_err_t epub_get_nav(const epub_handle_t *handle, epub_nav_t *out_nav);

/**
 * Set current chapter by spine index.
 * @param handle  Handle returned by epub_open().
 * @param index  Spine index (0-based).
 * @return ESP_OK on success; ESP_ERR_INVALID_ARG if out of range.
 */
esp_err_t epub_set_chapter(epub_handle_t *handle, int index);

/** Number of chapters in reading order (spine entries). */
int epub_get_spine_count(const epub_handle_t *handle);

/**
 * Go to next chapter in spine.
 * @param handle  Handle returned by epub_open().
 * @return ESP_OK on success; ESP_ERR_INVALID_STATE if at end.
 */
esp_err_t epub_next_chapter(epub_handle_t *handle);

/**
 * Go to previous chapter in spine.
 * @param handle  Handle returned by epub_open().
 * @return ESP_OK on success; ESP_ERR_INVALID_STATE if at beginning.
 */
esp_err_t epub_prev_chapter(epub_handle_t *handle);

/**
 * Go to chapter by TOC entry index.
 * @param handle  Handle returned by epub_open().
 * @param toc_index  Index into TOC array from epub_get_toc().
 * @return ESP_OK on success; ESP_ERR_INVALID_ARG if invalid.
 */
esp_err_t epub_goto_toc_entry(epub_handle_t *handle, int toc_index);

/**
 * Render current chapter to ANSI buffer.
 * @param handle  Handle returned by epub_open().
 * @param out_buf  Destination buffer (or NULL for measure-only).
 * @param buf_size  Size of out_buf.
 * @param out_len  Actual/required length (excl. NUL).
 * @return ESP_OK on success; ESP_ERR_INVALID_STATE if no chapter selected;
 *         ESP_ERR_NO_MEM if buffer too small (check *out_len).
 */
esp_err_t epub_render_chapter(const epub_handle_t *handle,
                              char *out_buf,
                              size_t buf_size,
                              size_t *out_len);

/**
 * Extract a specific resource (image, CSS, font) by href.
 * @param handle  Handle returned by epub_open().
 * @param href  Resource href from manifest (relative to OPF directory).
 * @param dest_path  Destination file path on SD card.
 * @return ESP_OK on success; ESP_ERR_NOT_FOUND if not in manifest.
 */
esp_err_t epub_extract_resource(const epub_handle_t *handle,
                                const char *href,
                                const char *dest_path);

/* ------------------------------------------------------------------------- */
/* Pure format core (unit-testable, no VFS)                                 */
/* ------------------------------------------------------------------------- */

/**
 * Parse OPF (package document) XML for metadata, manifest, spine.
 * @param xml  OPF XML text.
 * @param len  Length of xml.
 * @param out_meta  Output metadata.
 * @param out_chapters  Caller-allocated chapter array.
 * @param max_chapters  Size of out_chapters.
 * @param out_count  Number of chapters found.
 * @return true on success.
 */
bool epub_parse_opf(const char *xml, size_t len,
                    epub_metadata_t *out_meta,
                    epub_chapter_t *out_chapters,
                    int max_chapters,
                    int *out_count);

/**
 * Parse NCX (Navigation Control XML) for table of contents.
 * @param xml  NCX XML text.
 * @param len  Length of xml.
 * @param out_toc  Caller-allocated TOC entry array.
 * @param max_entries  Size of out_toc.
 * @param out_count  Number of entries found.
 * @return true on success.
 */
bool epub_parse_ncx(const char *xml, size_t len,
                    epub_toc_entry_t *out_toc,
                    int max_entries,
                    int *out_count);

/**
 * Parse EPUB3 nav.xhtml for table of contents.
 * @param xml  nav.xhtml XML text.
 * @param len  Length of xml.
 * @param out_toc  Caller-allocated TOC entry array.
 * @param max_entries  Size of out_toc.
 * @param out_count  Number of entries found.
 * @return true on success.
 */
bool epub_parse_nav(const char *xml, size_t len,
                    epub_toc_entry_t *out_toc,
                    int max_entries,
                    int *out_count);

/* ------------------------------------------------------------------------- */
/* Manifest entry (internal)                                                 */
/* ------------------------------------------------------------------------- */

typedef struct {
    char id[64];
    char href[EPUB_PATH_MAX];
    char media_type[64];
} epub_manifest_item_t;

/**
 * Parse OPF manifest into an array.
 * @param xml  OPF XML text.
 * @param len  Length of xml.
 * @param out_items  Caller-allocated array.
 * @param max_items  Size of out_items.
 * @param out_count  Number of items found.
 * @return true on success.
 */
bool epub_parse_manifest(const char *xml, size_t len,
                         epub_manifest_item_t *out_items,
                         int max_items,
                         int *out_count);

/**
 * Find manifest item by id.
 * @param items  Manifest array.
 * @param count  Number of items.
 * @param id  Manifest item id to find.
 * @return Pointer to item or NULL.
 */
const epub_manifest_item_t *epub_find_manifest_by_id(const epub_manifest_item_t *items,
                                                      int count,
                                                      const char *id);

/**
 * Resolve a relative href to absolute path within EPUB.
 * @param opf_dir  Directory containing the OPF file.
 * @param href  Relative href from manifest or spine.
 * @param out_path  Output buffer.
 * @param out_size  Size of output buffer.
 * @return true on success.
 */
bool epub_resolve_href(const char *opf_dir, const char *href,
                       char *out_path, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* EPUB_H */