/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "tui.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file tui_page_shot_internal.h
 * @brief Private seam between the page-shot MODEL (tui_page_shot.c) and its
 *        VIEW (tui.c).
 *
 * The model owns everything that is pure bytes: the blob layout (header, cell
 * grid, link regions, address bar), the size arithmetic, the alignment, the
 * shape validation and release. It includes only tui.h and p4heap, so the host
 * unit-test lane (test/host) links it natively - the same source the firmware
 * runs, and the same source `components/browser/src/browser_cache.c` calls
 * through the public `tui_page_shot_*` API.
 *
 * The view stays in tui.c: the live page buffer, the link table, the address
 * bar, the scroll offset and the render/flush plumbing all keep living there.
 * tui.c publishes exactly one descriptor - where the live page's bytes are
 * right now, or "no page up" - through the optional ops table below, which it
 * installs in `tui_init()`/`tui_viewport_init()`. On the host nothing installs
 * it and the pointer stays NULL, so capture reports NULL and restore reports
 * false. That is dependency inversion, not a stub: no page data is fabricated
 * and no no-op path pretends a page exists.
 *
 * @module tui
 * @layer leaf
 * @domain ui
 * @provides page-shot blob model sink seam (live page descriptor)
 * @status stable
 * @test test/main/test_browser_cache.c
 * @hard-rule ai-context.md#module-layering-rules
 */

/**
 * Where the live page's bytes are, as the view sees them.
 *
 * Pointers are into the view's own storage (the PSRAM page buffer, the static
 * link table and the address-bar array in tui.c); the model copies to/from
 * them and never retains a pointer after the callback returns.
 */
typedef struct {
    int cols;              /**< grid width of the live page */
    int rows;              /**< page rows (not the visible row count) */
    tui_cell_t *cells;     /**< cols*rows cells, in place */
    tui_link_t *links;     /**< TUI_MAX_LINKS entries, in place */
    int *link_count;       /**< in: links to capture; out: links restored */
    char *address;         /**< live address bar, in place */
    size_t address_size;   /**< capacity of @p address */
} tui_page_shot_view_t;

/** View-side hooks the model calls when the view is installed. */
typedef struct {
    /**
     * Fill @p out with the live page and return true.
     *
     * Returns false when no page is up (the viewport was never initialised,
     * or a deinit tore it down), which the model reports as "no shot" rather
     * than inventing one.
     */
    bool (*acquire)(void *ctx, tui_page_shot_view_t *out);
} tui_page_shot_view_ops_t;

/**
 * Install (or clear, with NULL) the live-page descriptor.
 *
 * Not thread-safe by design: tui.c installs it once during init, before any
 * capture can run, and the host lane never installs one.
 */
void tui_page_shot_set_view(const tui_page_shot_view_ops_t *ops, void *ctx);

#ifdef __cplusplus
}
#endif
