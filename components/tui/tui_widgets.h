/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file tui_widgets.h
 * @module tui
 * @layer leaf
 * @domain ui
 * @provides focusable widgets, row/column/grid layouts, scroll regions,
 *           key/tap routing through the existing modal + pane/event layers
 * @status experimental
 * @test test/main/test_tui.c
 * @hard-rule ai-context.md#module-layering-rules
 * @brief Compact component layer on top of pane/wig/hud/modal.
 *
 * No second window manager and no second modal runtime: focus state, scroll
 * regions and routing live in the existing tui_pane.c TU; key chords are
 * resolved by modal_chord_push() and taps by tui_tap_hit(), with every input
 * recorded through tui_event_post(). The geometry helpers here are pure and
 * independent of LVGL, so they are unit-testable headlessly.
 */

#ifndef P4MINISHELL_TUI_WIDGETS_H
#define P4MINISHELL_TUI_WIDGETS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tui_pane.h"
#include "tui_modal.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum named scroll regions (fixed capacity, no allocation). */
#define TUI_WIDGET_SCROLL_MAX 16

/* ========================================================================
 * PURE GEOMETRY
 * ======================================================================== */

typedef enum {
    TUI_LAYOUT_ROW = 0,
    TUI_LAYOUT_COLUMN,
    TUI_LAYOUT_GRID
} tui_layout_t;

typedef struct {
    int x, y, w, h;
} tui_rect_t;

/**
 * Geometry of item @p index (0-based) in a layout of @p count items inside
 * @p bounds. row: count columns; column: count rows; grid: @p cols columns and
 * ceil(count/cols) rows. @p gap is the inter-cell space in cells. Returns
 * false on an out-of-range index, non-positive count or row/column overflow.
 * Pure.
 */
static inline bool tui_layout_rect(tui_layout_t kind, tui_rect_t bounds,
                                   int index, int count, int cols, int gap,
                                   tui_rect_t *out)
{
    int nx, ny, cw, ch;

    if (out == NULL || index < 0 || count <= 0 || index >= count) return false;
    if (bounds.w < 1 || bounds.h < 1 || gap < 0) return false;
    if (gap >= bounds.w || gap >= bounds.h) return false;
    switch (kind) {
    case TUI_LAYOUT_ROW:
        nx = count;
        ny = 1;
        break;
    case TUI_LAYOUT_COLUMN:
        nx = 1;
        ny = count;
        break;
    case TUI_LAYOUT_GRID:
        nx = cols > 0 ? cols : count;
        ny = (count + nx - 1) / nx;
        break;
    default:
        return false;
    }
    if (index / nx >= ny) return false;
    cw = (bounds.w - gap * (nx - 1)) / nx;
    ch = (bounds.h - gap * (ny - 1)) / ny;
    if (cw < 1 || ch < 1) return false;
    out->x = bounds.x + (index % nx) * (cw + gap);
    out->y = bounds.y + (index / nx) * (ch + gap);
    out->w = cw;
    out->h = ch;
    return true;
}

/** Clamp a scroll offset to [0, content-viewport]. Pure. */
static inline int tui_scroll_clamp(int offset, int viewport, int content)
{
    int max = content - viewport;

    if (max < 0) max = 0;
    if (offset < 0) return 0;
    if (offset > max) return max;
    return offset;
}

/** Move a clamped scroll offset by @p delta rows. Pure. */
static inline int tui_scroll_step(int offset, int viewport, int content,
                                  int delta)
{
    return tui_scroll_clamp(offset + delta, viewport, content);
}

/** Wrap a focus index in [0,count) by @p dir (-1/+1); -1 when empty. Pure. */
static inline int tui_focus_wrap(int current, int count, int dir)
{
    if (count <= 0) return -1;
    if (dir == 0) dir = 1;
    current += dir;
    current %= count;
    if (current < 0) current += count;
    return current;
}

/* ========================================================================
 * SHARED FOCUS / SCROLL / ROUTING STATE (implemented in tui_pane.c)
 * ======================================================================== */

/** Register an existing widget as focusable (idempotent). */
bool tui_widget_focus_add(const char *name);

/** Focus a registered widget by name. */
bool tui_widget_focus_set(const char *name);

/** Move focus by dir (-1 previous, +1 next). @return new index or -1. */
int tui_widget_focus_step(int dir);

/** Focused widget name, or NULL when none. */
const char *tui_widget_focus_current(void);

/** Number of focusable widgets. */
int tui_widget_focus_count(void);

/** Clear the focus ring (called by tui_workspace_reset()). */
void tui_widget_focus_clear(void);

/** Bound label of a widget (NULL when missing/unbound). */
const char *tui_widget_get_label(const char *name);

/** Create/replace a named scroll region (viewport/content in rows). */
bool tui_scroll_region_add(const char *name, int viewport, int content);

/** Scroll a region by delta rows; @return the clamped offset or -1. */
int tui_scroll_region_step(const char *name, int delta);

/** Current offset of a region, or -1 when missing. */
int tui_scroll_region_get(const char *name);

/** Remove a scroll region. */
bool tui_scroll_region_remove(const char *name);

/**
 * Route a key stroke through the EXISTING modal chord resolver and record it
 * in the workspace event queue. No new mode stack is created.
 */
bool tui_widgets_route_key(const char *stroke, char *label_out, size_t size);

/** Route a cell tap through the EXISTING hotspot table and event queue. */
bool tui_widgets_route_tap(int col, int row, char *label_out, size_t size);

/** Activate the focused widget's bound label. @return the label or NULL. */
const char *tui_widgets_activate_focused(void);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_TUI_WIDGETS_H */
