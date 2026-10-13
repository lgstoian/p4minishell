/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file epub_opf.c
 * @brief OPF (Open Packaging Format) parser for EPUB.
 *
 * Parses the package document (.opf) for metadata, manifest, spine, and guide.
 * Uses Expat for streaming XML parsing.
 */

#include "epub.h"
#include "p4minishell_config.h"
#include <string.h>
#include <stdlib.h>
#include <esp_log.h>
#include <expat.h>


/* Forward declarations from epub.c */
extern void epub_xml_start(void *user_data, const char *name, const char **atts);
extern void epub_xml_end(void *user_data, const char *name);
extern void epub_xml_text(void *user_data, const char *s, int len);

/* ------------------------------------------------------------------------- */
/* Public OPF parsing function                                               */
/* ------------------------------------------------------------------------- */

bool epub_parse_opf(const char *xml, size_t len,
                    epub_metadata_t *out_meta,
                    epub_chapter_t *out_chapters,
                    int max_chapters,
                    int *out_count)
{
    if (!xml || !out_meta || !out_chapters || !out_count) return false;

    /* This function is a standalone version - for full EPUB parsing
     * use epub_open() which handles ZIP extraction and multi-file parsing. */
    return false; /* Use epub_open() for full EPUB support */
}

/* The actual OPF parsing is done in epub.c via the Expat callbacks:
 * epub_xml_start, epub_xml_end, epub_xml_text
 *
 * To parse OPF standalone, you would:
 * 1. Create XML_Parser
 * 2. Set user data to a context struct with output buffers
 * 3. Set element/text handlers to the same callbacks
 * 4. Parse the XML
 *
 * The full implementation lives in epub.c's XML callbacks which
 * populate the epub_t structure directly.
 */

bool epub_parse_manifest(const char *xml, size_t len,
                         epub_manifest_item_t *out_items,
                         int max_items,
                         int *out_count)
{
    /* Standalone manifest parsing not implemented - use epub_open() */
    return false;
}