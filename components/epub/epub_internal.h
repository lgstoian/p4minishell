/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file epub_internal.h
 * @brief EPUB instance layout shared by the component's translation units.
 *
 * epub.h deliberately exposes only the opaque `epub_handle_t`, but
 * epub_render.c (and epub.c) must read the parsed spine/toc/manifest and the
 * current navigation state. Keeping the layout here means ONE definition of
 * `struct epub_t` instead of a forward-declared, partially-duplicated copy in
 * every file that needs it.
 *
 * This header is internal to components/epub and is not part of the public
 * surface (include <epub.h> from outside).
 */

#ifndef P4MINISHELL_EPUB_INTERNAL_H
#define P4MINISHELL_EPUB_INTERNAL_H

#include "epub.h"

/** Maximum manifest items parsed from the OPF package document. */
#define EPUB_MAX_MANIFEST    256
/** Maximum spine entries (reading order) held per instance. */
#define EPUB_MAX_SPINE       256

/** Maximum table-of-contents entries built from the NCX/nav document. */
#define EPUB_MAX_TOC         256

/** One OPF manifest item, kept private to the component. */
typedef struct epub_manifest_item_internal {
    char id[64];
    char href[EPUB_PATH_MAX];
    char media_type[64];
} epub_manifest_item_internal_t;

/** The opaque handle from epub.h, completed.
 *
 *  epub.h names this type `epub_handle_t`; the component's own sources also
 *  use the shorter `epub_t`, which used to have no typedef at all (so every
 *  `p->epub->toc` access failed to compile). Both names now refer to the same
 *  single struct. */
typedef struct epub_t epub_t;

struct epub_t {
    void *zip_handle;                 /**< ZIP archive handle (components/zip). */
    char archive_path[256];
    char temp_dir[64];                /**< Extraction temp directory. */
    char opf_path[EPUB_PATH_MAX];     /**< Path to the OPF file within the archive. */
    char opf_dir[EPUB_PATH_MAX];      /**< Directory containing the OPF. */

    /* Parsed data */
    epub_metadata_t metadata;
    epub_chapter_t spine[EPUB_MAX_SPINE];
    int spine_count;
    epub_toc_entry_t toc[EPUB_MAX_TOC];
    int toc_count;
    epub_manifest_item_internal_t manifest[EPUB_MAX_MANIFEST];
    int manifest_count;

    /* Navigation state */
    int current_spine_index;          /**< -1 = none selected. */
};

#endif /* P4MINISHELL_EPUB_INTERNAL_H */