/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file epub_nav_core.h
 * @module epub
 * @layer leaf
 * @domain app
 * @provides pure TOC/spine linking, stepping and bounds checks
 * @status experimental
 * @test test/main/test_video.c, tools/host_video_gate.py
 * @hard-rule ai-context.md#module-layering-rules
 * @brief Pure navigation rules shared by EPUB parsing, commands and host tests.
 *
 * No XML parser, ZIP reader or display lives here: the component keeps ONE
 * Expat path in epub.c/epub_nav.c/epub_ncx.c and ONE HTML-to-ANSI path in
 * epub_render.c. These helpers only reason over already-parsed arrays, so the
 * same code runs on the board and in the host gate.
 */
#ifndef P4MINISHELL_EPUB_NAV_CORE_H
#define P4MINISHELL_EPUB_NAV_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "epub.h"

#ifdef __cplusplus
extern "C" {
#endif

/** True for a linear spine item (the EPUB default is linear = yes). */
static inline bool epub_chapter_is_linear(const epub_chapter_t *chapter)
{
    return chapter != NULL && chapter->linear;
}

/** Count linear reading-order chapters. Pure; NULL/empty yields zero. */
static inline int epub_spine_linear_count(const epub_chapter_t *spine, int count)
{
    int linear = 0;

    if (spine == NULL || count < 0) {
        return 0;
    }
    for (int i = 0; i < count; i++) {
        if (epub_chapter_is_linear(&spine[i])) {
            linear++;
        }
    }
    return linear;
}

/**
 * Next/previous spine index with clamping (no wrap). @return the target index
 * or -1 when the move would leave the spine. Pure.
 */
static inline int epub_nav_step(int current, int count, int delta)
{
    int next;

    if (count <= 0 || current < 0 || current >= count || delta == 0) {
        return -1;
    }
    next = current + delta;
    if (next < 0 || next >= count) {
        return -1;
    }
    return next;
}

/** Skip leading './' and '../' components and an optional #fragment. Pure. */
static inline const char *epub_href_key(const char *href)
{
    if (href == NULL) {
        return "";
    }
    while (href[0] == '.' && (href[1] == '/' || href[1] == '\0')) {
        href += (href[1] == '/') ? 2 : 1;
    }
    while (href[0] == '.' && href[1] == '.' &&
           (href[2] == '/' || href[2] == '\0')) {
        href += (href[2] == '/') ? 3 : 2;
    }
    return href;
}

/** Case-insensitive match of two hrefs, ignoring path prefixes and fragments. Pure. */
static inline bool epub_href_match(const char *left, const char *right)
{
    const char *a = epub_href_key(left);
    const char *b = epub_href_key(right);
    size_t alen = strlen(a);
    size_t blen = strlen(b);
    const char *hash = strchr(a, '#');
    const char *other_hash = strchr(b, '#');

    if (hash != NULL) {
        alen = (size_t)(hash - a);
    }
    if (other_hash != NULL) {
        blen = (size_t)(other_hash - b);
    }
    if (alen == 0 || blen == 0) {
        return false;
    }
    /* Compare the common tail: NCX/nav hrefs are often relative to a
     * different directory than the OPF manifest paths. */
    if (alen >= blen) {
        return strncasecmp(a + alen - blen, b, blen) == 0;
    }
    return strncasecmp(b + blen - alen, a, alen) == 0;
}

/**
 * Link each TOC entry to its spine index and give an untitled chapter the TOC
 * label. Unlinked entries keep chapter_index = -1 (never a bogus 0). Pure.
 * @return the number of links established.
 */
static inline int epub_toc_link_spine(epub_toc_entry_t *toc, int toc_count,
                                      const epub_chapter_t *spine,
                                      int spine_count)
{
    int linked = 0;

    if (toc == NULL || spine == NULL || toc_count < 0 || spine_count < 0) {
        return 0;
    }
    for (int i = 0; i < toc_count; i++) {
        for (int s = 0; s < spine_count; s++) {
            if (!epub_href_match(toc[i].href, spine[s].href)) {
                continue;
            }
            toc[i].chapter_index = s;
            if (spine[s].title[0] == '\0' && toc[i].label[0] != '\0') {
                snprintf(spine[s].title, sizeof(spine[s].title), "%s",
                         toc[i].label);
            }
            linked++;
            break;
        }
    }
    return linked;
}

/** Find a TOC entry by href (case-insensitive, prefix/fragment tolerant). */
static inline int epub_toc_find_href(const epub_toc_entry_t *toc, int count,
                                     const char *href)
{
    if (toc == NULL || href == NULL || count < 0) {
        return -1;
    }
    for (int i = 0; i < count; i++) {
        if (epub_href_match(toc[i].href, href)) {
            return i;
        }
    }
    return -1;
}

/** Resolve a TOC entry index to a spine index, or -1 when not navigable. */
static inline int epub_toc_target(const epub_toc_entry_t *toc, int toc_count,
                                  const epub_chapter_t *spine, int spine_count,
                                  int toc_index)
{
    if (toc == NULL || spine == NULL || toc_index < 0 || toc_index >= toc_count) {
        return -1;
    }
    if (toc[toc_index].chapter_index >= 0 &&
        toc[toc_index].chapter_index < spine_count) {
        return toc[toc_index].chapter_index;
    }
    /* A stale link (TOC parsed before the spine, or a reordered OPF) still
     * navigates by href instead of silently refusing the entry. */
    for (int s = 0; s < spine_count; s++) {
        if (epub_href_match(toc[toc_index].href, spine[s].href)) {
            return s;
        }
    }
    return -1;
}

/** Reject impossible nesting depths from a malformed NCX/nav document. Pure. */
static inline bool epub_toc_depth_sane(int depth)
{
    return depth >= 0 && depth < 64;
}

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_EPUB_NAV_CORE_H */
