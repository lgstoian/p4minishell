/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file selection.h
 * @module selection
 * @layer leaf
 * @domain ui
 * @provides one drag-range model, cell/row text assembly, the single
 *           clipboard commit sink and the highlight paints every text
 *           surface shares
 * @status stable
 * @test test/main/test_selection.c
 * @hard-rule ai-context.md#module-layering-rules
 * @brief Shared text-selection core: drag range, copy-from-provider, highlight.
 *
 * Every text surface in P4MiniShell selects the same way: a long press arms a
 * selection, dragging extends it, and a tap inside copies it. This file is the
 * one implementation of that behaviour's data model and of the text it
 * produces:
 *
 *   - range model: a 1-based anchor cell and focus cell, normalised into an
 *     inclusive rectangle (row/column - a *cell*, not a byte);
 *   - copy-from-provider: the surface supplies one cell of UTF-8 text on
 *     demand, so no surface keeps a second copy of its own text just for
 *     copying. The TUI page answers from its page grid, the transcript from
 *     its rendered row table, the modal pagers from their label text;
 *   - one clipboard: the commit sink is installed by the shell
 *     (`shell_clipboard_set` + `header_notify`), so every surface writes to
 *     the same `clip`/`paste` buffer - there is no second clipboard;
 *   - highlight paints: `sel_paint_rect` is the one rectangle primitive;
 *     `sel_paint_cells` tiles it over a uniform cell grid (the TUI page) and
 *     the transcript calls it per selected row (its rows have measured,
 *     variable heights because they wrap); `sel_paint_label` drives a
 *     label-owned text selection for wrap- and font-accurate highlighting in
 *     the modal pagers.
 *
 * The range/assembly half is plain C (`selection.c`) and unit-tested on
 * target by test/main/test_selection.c; the LVGL paints live in
 * selection_ui.c. Callers hold the LVGL port lock when they paint.
 */

#ifndef P4MINISHELL_SELECTION_H
#define P4MINISHELL_SELECTION_H

#include <stdbool.h>
#include <stddef.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * RANGE MODEL
 * ======================================================================== */

/** A drag selection: the cell the gesture started on and the current cell.
 *  Cells are 1-based (row, column); the column is a glyph column, never a
 *  byte offset. */
typedef struct {
    int ar;   /**< Anchor row (the long press). */
    int ac;   /**< Anchor column. */
    int fr;   /**< Focus row (the live drag point). */
    int fc;   /**< Focus column. */
} sel_range_t;

/** Normalised inclusive bounds of a selection: r0/c0 <= r1/c1. */
typedef struct {
    int r0;
    int c0;
    int r1;
    int c1;
} sel_bounds_t;

/** Arm a selection at one cell. */
void sel_range_init(sel_range_t *range, int row, int col);

/** Move the focus (the dragged end) of a selection. */
void sel_range_focus(sel_range_t *range, int row, int col);

/** Normalise @p range into @p out. */
void sel_range_bounds(const sel_range_t *range, sel_bounds_t *out);

/** True when @p row/@p col falls inside the normalised @p bounds. */
bool sel_bounds_contains(const sel_bounds_t *bounds, int row, int col);

/** True when the gesture has not left its anchor cell (an empty selection). */
bool sel_bounds_is_anchor(const sel_bounds_t *bounds);

/** Compare two 1-based cells in reading order (row first, then column).
 *  @return negative when (r1,c1) precedes (r2,c2), 0 when equal, positive
 *          when it follows. */
int sel_cell_order(int r0, int c0, int r1, int c1);

/* ========================================================================
 * COPY-FROM-PROVIDER
 * ======================================================================== */

/**
 * One cell of text, as UTF-8 (1..4 bytes), supplied by the surface.
 *
 * @return false when the cell holds no glyph (blank, or past the end of the
 *         row). The copy path then contributes nothing for that cell, so a
 *         grid with variable-length rows may pass a column bound that is
 *         larger than any row.
 */
typedef bool (*sel_cell_text_fn)(void *ctx, int row, int col,
                                 char *out, size_t out_size);

/**
 * Assemble a cell range into @p out as text: cells left-to-right per row,
 * trailing blanks trimmed from every row, rows joined with '\n' and no
 * trailing newline.
 *
 * @return Bytes written (excluding the NUL). 0 when nothing was selected.
 */
size_t sel_assembly_from_cells(sel_cell_text_fn get, void *ctx,
                               const sel_bounds_t *bounds,
                               char *out, size_t out_size);

/**
 * The one clipboard commit path. The shell installs a sink that calls
 * `shell_clipboard_set` and posts the "selection copied" header notice, so
 * every surface ends up in the same `clip`/`paste` buffer.
 */
typedef void (*sel_commit_fn)(const char *text);

/** Install the commit sink (called once, from shell init). */
void sel_set_commit(sel_commit_fn commit);

/** Hand @p text to the commit sink. @return false when there is no sink or
 *  the text is empty. */
bool sel_commit(const char *text);

/**
 * Copy a cell range through @p get into the shell clipboard.
 *
 * @return true when text was assembled and committed.
 */
bool sel_copy_cells(sel_cell_text_fn get, void *ctx, const sel_bounds_t *bounds);

/** Copy the character span [first, last) of @p text to the clipboard (the
 *  modal pagers' exact char range; the same half-open span the label
 *  highlight paints). Trailing blanks per line are trimmed.
 *  @return true when something was copied. */
bool sel_copy_text_span(const char *text, int first, int last);

/* ========================================================================
 * CHARACTER / ROW HELPERS (for the label-backed pagers)
 * ======================================================================== */

/** Number of '\n'-separated lines in @p text (always >= 1 for ""). */
int sel_line_count(const char *text);

/** Number of glyphs (characters, not bytes) in @p text. */
int sel_char_count(const char *text);

/** Character index (not bytes) of the first character of 1-based @p row. */
int sel_row_start_index(const char *text, int row);

/** Map a 1-based row/column cell to a character index in @p text. Returns the
 *  character index of the start of @p row when the column is past the line
 *  end, and -1 when the row does not exist. */
int sel_row_col_to_index(const char *text, int row, int col);

/** Map a character index in @p text back to a 1-based row/column cell.
 *  @return false when @p index is out of range. */
bool sel_index_to_row_col(const char *text, int index, int *row_out,
                          int *col_out);

/** Byte offset of character @p index in @p text (clamped to the NUL). */
size_t sel_char_byte_offset(const char *text, int index);

/**
 * Linear character span of a drag range over a '\n'-separated text model: from
 * the anchor to the focus in reading order, independent of the drag direction.
 * This is the pager's selection: text has no columns that must line up, so a
 * drag from the middle of line 2 to the end of line 5 covers the whole tail of
 * line 2, every line between, and the head of line 5 - the same span the label
 * highlight paints, and the exact span the copy takes.
 *
 * @param first_out  First character of the span.
 * @param last_out   One past its last character.
 */
bool sel_text_span_from_range(const char *text, const sel_range_t *range,
                              int *first_out, int *last_out);

/** True when character @p index falls inside the linear span of @p range. */
bool sel_text_range_contains(const char *text, const sel_range_t *range,
                             int index);

/** Copy the UTF-8 glyph starting at @p s into @p out (NUL-terminated).
 *  @return bytes consumed (0 at the end of the string). */
size_t sel_utf8_glyph(const char *s, char *out, size_t out_size);

/* ========================================================================
 * HIGHLIGHT PAINTS (LVGL; caller holds the port lock)
 * ======================================================================== */

/**
 * Paint one highlight rectangle in @p layer. This is the single primitive the
 * grid and row paints below use; a surface with variable row heights (the
 * transcript, whose rows wrap) calls it directly per selected row.
 */
void sel_paint_rect(lv_obj_t *layer, lv_coord_t x, lv_coord_t y,
                    lv_coord_t w, lv_coord_t h);

/**
 * Draw @p bounds as translucent rectangles on @p layer, one per row of a
 * uniform @p cell_w x @p cell_h grid whose origin is (@p ox, @p oy) inside
 * @p layer. The first and last row are clipped to the column bounds; middle
 * rows span @p cols columns. @p layer is cleared first (it must be owned by
 * the surface and hold nothing else).
 *
 * Used by the TUI page (a character grid) and any other uniform-pitch grid.
 */
void sel_paint_cells(lv_obj_t *layer, lv_coord_t ox, lv_coord_t oy,
                     lv_coord_t cell_w, lv_coord_t cell_h,
                     int cols, const sel_bounds_t *bounds);

/**
 * Highlight the character span [start_char, end_char) of a label using the
 * widget's own text-selection rendering: wrap, alignment and font metrics are
 * LVGL's, so the highlight is exact even for the proportional reading font.
 */
void sel_paint_label(lv_obj_t *label, int start_char, int end_char);

/** Remove the label highlight. */
void sel_paint_label_clear(lv_obj_t *label);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_SELECTION_H */
