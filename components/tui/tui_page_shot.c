/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file tui_page_shot.c
 * @brief Hardware-free page-shot model: capture/restore/release of a rendered
 *        page as an opaque PSRAM blob.
 *
 * A snapshot is a plain byte image of one rendered page: a fixed header, the
 * cell grid (cols*rows tui_cell_t, 4-byte aligned), then the link regions and
 * the address bar. Restoring it is a memcpy into the view's live buffers, which
 * is why the browser's back/forward cache costs no network, parse or render
 * work. The blob is versioned by its dimensions: a snapshot taken under a
 * different grid is rejected rather than blitted into the wrong shape, and a
 * truncated blob whose link array would overlap the cells is rejected too.
 *
 * This file is the MODEL half. It includes only tui.h, p4heap and the standard
 * library - no LVGL, no window manager, no display, no board BSP - so the host
 * unit-test lane (test/host) compiles and links the same source the firmware
 * runs. The live page (cell buffer, link table, address bar) stays in tui.c and
 * is reached solely through the optional `tui_page_shot_view_ops_t` seam. No
 * view installed (host lane, or before init) means no page to capture: capture
 * returns NULL and restore returns false, exactly like a page-less board.
 *
 * @module tui
 * @layer leaf
 * @domain ui
 * @provides rendered-page snapshots (capture, restore, release) as PSRAM blobs
 * @status stable
 * @test test/main/test_browser_cache.c
 * @hard-rule ai-context.md#module-layering-rules
 */

#include "tui_page_shot_internal.h"

#include <stdio.h>
#include <string.h>

#include "p4heap.h"

/* The live page, when one is up. NULL on the host lane and before tui_init(). */
static const tui_page_shot_view_ops_t *s_view = NULL;
static void *s_view_ctx = NULL;

void tui_page_shot_set_view(const tui_page_shot_view_ops_t *ops, void *ctx)
{
    s_view = ops;
    s_view_ctx = ctx;
}

/** Ask the view for the live page. False when no page is up. */
static bool page_shot_acquire(tui_page_shot_view_t *out)
{
    if (s_view == NULL || s_view->acquire == NULL) {
        return false;
    }
    return s_view->acquire(s_view_ctx, out);
}

/** A descriptor is only usable when every buffer it names is present. */
static bool page_shot_view_valid(const tui_page_shot_view_t *view)
{
    return view->cols > 0 && view->rows > 0 &&
           view->cells != NULL && view->links != NULL &&
           view->link_count != NULL &&
           view->address != NULL && view->address_size > 0;
}

static size_t page_shot_cell_bytes(const tui_page_shot_view_t *view)
{
    return (size_t)view->cols * (size_t)view->rows * sizeof(tui_cell_t);
}

/** Clamp a link count to the table the view actually has. */
static int page_shot_link_count(int count)
{
    if (count < 0) {
        return 0;
    }
    if (count > TUI_MAX_LINKS) {
        return TUI_MAX_LINKS;
    }
    return count;
}

/** Byte offset of the link array for a header plus @p cell_bytes of cells. */
static uint32_t page_shot_link_offset(size_t cell_bytes)
{
    return (uint32_t)((sizeof(tui_page_shot_t) + cell_bytes + 3u) & ~(size_t)3u);
}

void *tui_page_shot_capture(void)
{
    tui_page_shot_view_t view;
    tui_page_shot_t *shot;
    size_t cell_bytes;
    size_t total;
    int link_count;

    if (!page_shot_acquire(&view) || !page_shot_view_valid(&view)) {
        return NULL;
    }

    cell_bytes = page_shot_cell_bytes(&view);
    link_count = page_shot_link_count(*view.link_count);
    total = (size_t)page_shot_link_offset(cell_bytes);
    total += (size_t)link_count * sizeof(tui_link_t);

    shot = p4heap_alloc_psram(total);
    if (shot == NULL) {
        return NULL;
    }
    shot->cols = view.cols;
    shot->rows = view.rows;
    shot->link_count = link_count;
    shot->link_offset = page_shot_link_offset(cell_bytes);
    shot->total_bytes = (uint32_t)total;   /* bounded by the TUI grid */
    memset(shot->address, 0, sizeof(shot->address));
    snprintf(shot->address, sizeof(shot->address), "%s", view.address);

    memcpy(shot + 1, view.cells, cell_bytes);
    if (link_count > 0) {
        memcpy((char *)shot + shot->link_offset, view.links,
               (size_t)link_count * sizeof(tui_link_t));
    }
    return shot;
}

bool tui_page_shot_restore(const void *blob)
{
    const tui_page_shot_t *shot = (const tui_page_shot_t *)blob;
    tui_page_shot_view_t view;
    size_t cell_bytes;
    int count;

    if (shot == NULL) {
        return false;
    }
    if (!page_shot_acquire(&view) || !page_shot_view_valid(&view)) {
        return false;
    }
    /* A snapshot from another grid (or a truncated blob) is refused, not
     * blitted: the cell stride would be wrong and the page would be garbage.
     * The recorded total size also makes every subsequent read bounded. */
    if (shot->cols != view.cols || shot->rows != view.rows) {
        return false;
    }
    if (shot->total_bytes < sizeof(tui_page_shot_t)) {
        return false;
    }
    cell_bytes = page_shot_cell_bytes(&view);
    {
        /* Upper bound: header + full grid (with the 4-byte alignment pad) +
         * every link the page can hold. Rejecting anything larger keeps a
         * corrupted blob from driving a huge memcpy. */
        size_t max_total = sizeof(tui_page_shot_t) + cell_bytes + 4u +
                           (size_t)TUI_MAX_LINKS * sizeof(tui_link_t);

        if ((size_t)shot->total_bytes > max_total) {
            return false;
        }
    }
    if (shot->link_offset < sizeof(tui_page_shot_t) + cell_bytes ||
        shot->link_offset > shot->total_bytes) {
        return false;
    }

    count = shot->link_count;
    if (count < 0 || count > TUI_MAX_LINKS) {
        return false;
    }
    if ((size_t)shot->link_offset + (size_t)count * sizeof(tui_link_t) >
        (size_t)shot->total_bytes) {
        return false;
    }

    memcpy(view.cells, shot + 1, cell_bytes);

    if (count > 0) {
        memcpy(view.links, (const char *)shot + shot->link_offset,
               (size_t)count * sizeof(tui_link_t));
    }
    *view.link_count = count;
    snprintf(view.address, view.address_size, "%s", shot->address);
    return true;
}

void tui_page_shot_release(void *blob)
{
    p4heap_free(blob);
}
