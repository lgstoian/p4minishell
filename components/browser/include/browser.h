/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file browser.h
 * @module browser
 * @layer leaf
 * @domain ui
 * @provides PSRAM image cache and rendered-page back/forward cache for the browser
 * @status stable
 * @hard-rule ai-context.md#module-layering-rules
 * @test test/main/test_browser_cache.c
 * @brief Browser memory caches — public API.
 *
 * Two bounded LRU caches, both living in the PSRAM pool (the P4 has 32 MB and
 * internal DRAM is the scarce resource - see ai-context.md):
 *
 *   1. Image cache: the decoded DOS-cell grid for a URL, so a re-render or a
 *      back/forward skips BOTH the network fetch and the PNG/JPEG decode.
 *   2. Page cache: a whole rendered page (cells + link regions + address bar,
 *      captured by the tui snapshot API), so back/forward to a visited page is a
 *      memcpy instead of a fetch, a parse and a render.
 *
 * Both are pure RAM caches; nothing here touches the SD card or the network.
 * The browser command layer (components/command/browser_commands.c) is the only
 * consumer, and it runs on the command worker, so the caches are single-owner.
 *
 * Budget: IMAGE_CACHE_MAX x (IMAGE_MAX_WIDTH * IMAGE_MAX_HEIGHT) bytes of pixel
 * data plus PAGE_CACHE_MAX whole pages, all in PSRAM.
 */

#ifndef P4MINISHELL_BROWSER_CACHE_H
#define P4MINISHELL_BROWSER_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dom_render.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Create the cache locks. Safe to call once from browser_init(). */
void browser_cache_init(void);

/* ========================================================================
 * IMAGE CACHE
 * ======================================================================== */

/** Look up a decoded image by its source URL.
 *  @return true when @p out received an owned pixel buffer (release it with
 *          free(), exactly as the renderer expects). false on a miss. */
bool browser_image_cache_get(const char *url, dom_image_t *out);

/** Store a decoded image under its source URL. Takes its own copy of the pixels.
 *  @return false when the entry does not fit (too large) or on OOM. */
bool browser_image_cache_put(const char *url, const dom_image_t *img);

void browser_image_cache_clear(void);
int  browser_image_cache_count(void);
size_t browser_image_cache_bytes(void);

/* ========================================================================
 * PAGE CACHE (back/forward)
 * ======================================================================== */

/** Restore a cached rendered page by URL. On a hit the TUI page, link regions and
 *  address bar are replaced by memcpy; the caller then flushes and renders the
 *  chrome. @return false on a miss or a shape mismatch. */
bool browser_page_cache_get(const char *url);

/** Capture the currently rendered page and store it under @p url.
 *  @return false on OOM or when no page is up. */
bool browser_page_cache_put(const char *url);

void browser_page_cache_clear(void);
int  browser_page_cache_count(void);
size_t browser_page_cache_bytes(void);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_BROWSER_CACHE_H */
