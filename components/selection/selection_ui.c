/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file selection_ui.c
 * @brief Shared selection highlight paints (LVGL).
 *
 * Two paints cover every text surface:
 *
 *   - sel_paint_cells(): translucent rectangles over a uniform cell grid, for
 *     the TUI page (character cells) and the shell transcript (whole rows).
 *     The rectangle set is the one the TUI page used inline; it now lives here
 *     so both grids render a selection identically.
 *   - sel_paint_label(): the label-owned text selection, for the modal pagers.
 *     LVGL draws it per glyph with the real wrap/font metrics, so a char range
 *     in a proportional reading font highlights exactly.
 *
 * The caller owns @p layer / @p label and holds the LVGL port lock.
 */

#include "selection.h"

/* Highlight colours: the same accent the TUI page always used, so a selection
 * looks the same on every surface. */
#define SEL_FILL_COLOR     0x2D7FF9
#define SEL_BORDER_COLOR   0x7FB2FF

void sel_paint_rect(lv_obj_t *layer, lv_coord_t x, lv_coord_t y,
                    lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *box;

    if (layer == NULL || w <= 0 || h <= 0) {
        return;
    }
    box = lv_obj_create(layer);
    if (box == NULL) {
        return;
    }
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, w, h);
    lv_obj_set_style_bg_color(box, lv_color_hex(SEL_FILL_COLOR), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_50, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(SEL_BORDER_COLOR), 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_scrollable(box, false);
    lv_obj_set_clickable(box, false);
}

void sel_paint_cells(lv_obj_t *layer, lv_coord_t ox, lv_coord_t oy,
                     lv_coord_t cell_w, lv_coord_t cell_h,
                     int cols, const sel_bounds_t *bounds)
{
    if (layer == NULL) {
        return;
    }
    lv_obj_clean(layer);
    if (bounds == NULL || cell_w <= 0 || cell_h <= 0 || cols <= 0) {
        return;
    }
    for (int r = bounds->r0; r <= bounds->r1; r++) {
        int row_start = (r == bounds->r0) ? bounds->c0 : 1;
        int row_end = (r == bounds->r1) ? bounds->c1 : cols;
        int x;
        int w;

        if (row_start < 1) {
            row_start = 1;
        }
        if (row_end > cols) {
            row_end = cols;
        }
        x = (row_start - 1) * (int)cell_w;
        w = (row_end - row_start + 1) * (int)cell_w;
        if (w <= 0) {
            continue;
        }
        sel_paint_rect(layer, ox + (lv_coord_t)x,
                       oy + (lv_coord_t)((r - 1) * (int)cell_h),
                       (lv_coord_t)w, cell_h);
    }
}

void sel_paint_label(lv_obj_t *label, int start_char, int end_char)
{
    if (label == NULL) {
        return;
    }
    if (start_char < 0) {
        start_char = 0;
    }
    if (end_char < start_char) {
        end_char = start_char;
    }
    /* The label draws its own selection: exact for wrapped and proportional
     * text, and no second highlight layer of ours to keep in sync. LVGL takes
     * the highlight colour from LV_PART_SELECTED, which the default theme
     * paints its primary blue (#2196F3): a whole-document selection then reads
     * as the LVGL "blue screen". Pin it to the shared selection colours. */
    lv_obj_set_style_bg_color(label, lv_color_hex(SEL_FILL_COLOR),
                              LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(label, LV_OPA_50, LV_PART_SELECTED);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF),
                                LV_PART_SELECTED);
    lv_label_set_text_selection_start(label, (uint32_t)start_char);
    lv_label_set_text_selection_end(label, (uint32_t)end_char);
    lv_obj_invalidate(label);
}

void sel_paint_label_clear(lv_obj_t *label)
{
    if (label == NULL) {
        return;
    }
    lv_label_set_text_selection_start(label, LV_LABEL_TEXT_SELECTION_OFF);
    lv_label_set_text_selection_end(label, LV_LABEL_TEXT_SELECTION_OFF);
    lv_obj_invalidate(label);
}
