/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file tui_frame.h
 * @module tui
 * @layer leaf
 * @domain ui
 * @provides double-buffered cell frame, dirty-rect diff, ONE cell renderer,
 *           ONE image sampler, 24-bit/256-colour mapping through the DOS palette
 * @status experimental
 * @test test/main/test_tui.c
 * @hard-rule ai-context.md#module-layering-rules
 * @brief Pure, hardware-free half of the TUI cell renderer.
 *
 * The live grid is double buffered: `presented` is the copy LVGL last showed,
 * `working` is what the draw primitives mutate. A frame is diffed cell-by-cell
 * into a bounding dirty rect; the renderer turns the grid into the one recolor
 * markup string the LVGL label consumes, and `tui_frame_commit()` publishes the
 * working copy. Nothing here touches LVGL, a window manager or a display, so
 * the firmware (through tui.c) and the host render gate compile and link the
 * SAME code. Header-only by design: the tui component's source list stays
 * untouched and the headless lane needs no second renderer.
 *
 * Colour: every 24-bit/256-colour input is quantised through tui_rgb_to_dos()
 * (the ONE ansi_cga_nearest mapping). Default DOS cells (fg/bg 16) are never
 * rewritten, so the classic 80x25 look is preserved.
 */

#ifndef P4MINISHELL_TUI_FRAME_H
#define P4MINISHELL_TUI_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "tui.h"           /* tui_cell_t, gfx_surface_t, tui_rgb_to_dos */
#include "ansi.h"          /* ansi_image_glyph, ANSI_IMAGE_RAMP_LAST: the ONE ramp */
#include "ansi_palette.h"  /* SH_BOX_* CP437 glyphs: the ONE box vocabulary */

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * DIRTY RECT
 * ======================================================================== */

/** Inclusive 1-based bounding box of every cell that changed. */
typedef struct {
    int min_col;
    int min_row;
    int max_col;
    int max_row;
    bool any;
} tui_dirty_t;

/** Empty rect (the "nothing changed" result). Pure. */
static inline tui_dirty_t tui_dirty_none(void)
{
    tui_dirty_t d;

    d.min_col = 0;
    d.min_row = 0;
    d.max_col = 0;
    d.max_row = 0;
    d.any = false;
    return d;
}

/** Grow @p d to contain cell (@p col, @p row). Pure. */
static inline void tui_dirty_add(tui_dirty_t *d, int col, int row)
{
    if (d == NULL || col < 1 || row < 1) return;
    if (!d->any) {
        d->min_col = d->max_col = col;
        d->min_row = d->max_row = row;
        d->any = true;
        return;
    }
    if (col < d->min_col) d->min_col = col;
    if (col > d->max_col) d->max_col = col;
    if (row < d->min_row) d->min_row = row;
    if (row > d->max_row) d->max_row = row;
}

/** Union two rects; a rect with any=false contributes nothing. Pure. */
static inline tui_dirty_t tui_dirty_union(tui_dirty_t a, const tui_dirty_t *b)
{
    if (b == NULL || !b->any) return a;
    if (!a.any) return *b;
    if (b->min_col < a.min_col) a.min_col = b->min_col;
    if (b->min_row < a.min_row) a.min_row = b->min_row;
    if (b->max_col > a.max_col) a.max_col = b->max_col;
    if (b->max_row > a.max_row) a.max_row = b->max_row;
    return a;
}

/* ========================================================================
 * DOUBLE-BUFFERED FRAME
 * ======================================================================== */

/**
 * Two equal-sized cell grids. `presented` is the last committed frame (what
 * LVGL shows); `working` is the next frame. Storage is owned by the caller
 * (PSRAM in the firmware, a stack/static array on the host) and never
 * allocated here, which keeps this layer headless-safe.
 */
typedef struct {
    int cols;
    int rows;
    tui_cell_t *presented;
    tui_cell_t *working;
} tui_frame_t;

/** Bind caller-owned storage to a frame. No copy, no allocation. Pure. */
static inline void tui_frame_bind(tui_frame_t *f, int cols, int rows,
                                  tui_cell_t *presented, tui_cell_t *working)
{
    if (f == NULL) return;
    f->cols = cols > 0 ? cols : 0;
    f->rows = rows > 0 ? rows : 0;
    f->presented = presented;
    f->working = working;
}

/** Bound storage usable? Pure. */
static inline bool tui_frame_valid(const tui_frame_t *f)
{
    return f != NULL && f->cols > 0 && f->rows > 0 &&
           f->presented != NULL && f->working != NULL;
}

/** Working cell at 1-based (col,row), clamped; NULL when unbound. Pure. */
static inline tui_cell_t *tui_frame_cell(tui_frame_t *f, int col, int row)
{
    if (!tui_frame_valid(f)) return NULL;
    if (col < 1) col = 1;
    if (col > f->cols) col = f->cols;
    if (row < 1) row = 1;
    if (row > f->rows) row = f->rows;
    return &f->working[(row - 1) * f->cols + (col - 1)];
}

/** Exact cell comparison (bytes of UTF-8 plus fg/bg/attr). Pure. */
static inline bool tui_frame_cell_eq(const tui_cell_t *a, const tui_cell_t *b)
{
    if (a == b) return true;
    if (a == NULL || b == NULL) return false;
    return a->fg == b->fg && a->bg == b->bg && a->attr == b->attr &&
           strncmp(a->utf8, b->utf8, sizeof(a->utf8)) == 0;
}

/** Write one working cell. Pure (no LVGL). */
static inline void tui_frame_set(tui_frame_t *f, int col, int row,
                                 const char *utf8, uint8_t fg, uint8_t bg,
                                 uint8_t attr)
{
    tui_cell_t *cell = tui_frame_cell(f, col, row);

    if (cell == NULL) return;
    strncpy(cell->utf8, utf8 != NULL ? utf8 : "", sizeof(cell->utf8) - 1);
    cell->utf8[sizeof(cell->utf8) - 1] = '\0';
    cell->fg = fg;
    cell->bg = bg;
    cell->attr = attr;
}

/** Clear the working grid to one space/colour pair. Pure. */
static inline void tui_frame_clear(tui_frame_t *f, uint8_t fg, uint8_t bg)
{
    if (!tui_frame_valid(f)) return;
    for (int row = 1; row <= f->rows; row++) {
        for (int col = 1; col <= f->cols; col++) {
            tui_frame_set(f, col, row, " ", fg, bg, 0);
        }
    }
}

static inline tui_dirty_t tui_frame_diff_rect(const tui_frame_t *f,
                                              int col1, int row1,
                                              int col2, int row2);

/** Diff presented vs working over the whole grid. Pure. */
static inline tui_dirty_t tui_frame_diff(const tui_frame_t *f)
{
    return tui_frame_diff_rect(f, 1, 1,
                               tui_frame_valid(f) ? f->cols : 0,
                               tui_frame_valid(f) ? f->rows : 0);
}

/**
 * Diff presented vs working over [col1..col2] x [row1..row2] (clamped, 1-based).
 * This is the dirty-rect core: callers with a known edit region pay only for
 * that region, and the host gate can assert the rect a scene produces. Pure.
 */
static inline tui_dirty_t tui_frame_diff_rect(const tui_frame_t *f,
                                              int col1, int row1,
                                              int col2, int row2)
{
    tui_dirty_t d = tui_dirty_none();

    if (!tui_frame_valid(f)) return d;
    if (col1 < 1) col1 = 1;
    if (row1 < 1) row1 = 1;
    if (col2 > f->cols) col2 = f->cols;
    if (row2 > f->rows) row2 = f->rows;
    for (int row = row1; row <= row2; row++) {
        for (int col = col1; col <= col2; col++) {
            const tui_cell_t *a = &f->presented[(row - 1) * f->cols + (col - 1)];
            const tui_cell_t *b = &f->working[(row - 1) * f->cols + (col - 1)];

            if (!tui_frame_cell_eq(a, b)) tui_dirty_add(&d, col, row);
        }
    }
    return d;
}

/** Publish the working frame to `presented` (memcpy). Call AFTER diffing and
 *  after the single LVGL update. Pure with respect to LVGL. */
static inline void tui_frame_commit(tui_frame_t *f)
{
    size_t bytes;

    if (!tui_frame_valid(f)) return;
    bytes = (size_t)f->cols * (size_t)f->rows * sizeof(tui_cell_t);
    memcpy(f->presented, f->working, bytes);
}

/* ========================================================================
 * THE ONE CELL RENDERER (markup for the LVGL recolor label)
 * ======================================================================== */

/** Worst-case markup bytes: per cell `#rrggbb ` + 3 UTF-8 bytes + `#` (16),
 *  plus one newline per row and the terminator. Pure. */
static inline size_t tui_frame_markup_cap(const tui_frame_t *f)
{
    if (!tui_frame_valid(f)) return 1;
    return (size_t)f->rows * (size_t)((size_t)f->cols * 16 + 1) + 1;
}

/**
 * Render rows [row1..row2] (inclusive, clamped) into @p out as the recolor
 * markup the TUI label consumes. The entire firmware path and the host gate
 * call THIS function; there is no second formatter. Run coalescing, bold-on-
 * stock-colour and `#` escaping match tui_flush's historical contract exactly.
 *
 * @return bytes written (excluding the terminator). Pure, LVGL-free.
 */
static inline size_t tui_frame_render_span(const tui_frame_t *f, int row1,
                                           int row2, char *out, size_t cap)
{
    size_t pos = 0;

    if (!tui_frame_valid(f) || out == NULL || cap == 0) return 0;
    if (row1 < 1) row1 = 1;
    if (row2 > f->rows) row2 = f->rows;
    for (int r = row1; r <= row2; r++) {
        int c = 1;

        while (c <= f->cols) {
            const tui_cell_t *cell = &f->working[(r - 1) * f->cols + (c - 1)];
            uint8_t fg = cell->fg;

            if ((cell->attr & 1u) != 0 && fg < 8) fg += 8;
            {
                int run = 1;

                while (c + run <= f->cols) {
                    const tui_cell_t *n =
                        &f->working[(r - 1) * f->cols + (c + run - 1)];
                    uint8_t nfg = n->fg;

                    if ((n->attr & 1u) != 0 && nfg < 8) nfg += 8;
                    if (nfg != fg) break;
                    run++;
                }
                {
                    bool tagged = (fg < 16);
                    uint32_t col = 0;

                    if (tagged) {
                        int n;

                        col = tui_dos_color_rgb(fg);
                        n = snprintf(out + pos, cap - pos, "#%06x ",
                                     (unsigned)(col & 0xFFFFFFu));
                        if (n < 0) break;
                        if ((size_t)n >= cap - pos) { pos = cap - 1; break; }
                        pos += (size_t)n;
                    }
                    for (int k = 0; k < run; k++) {
                        const tui_cell_t *cc =
                            &f->working[(r - 1) * f->cols + (c + k - 1)];
                        const char *utf8 = cc->utf8[0] ? cc->utf8 : " ";

                        if (tagged && strchr(utf8, '#') != NULL) {
                            int n;

                            if (pos + 1 < cap) out[pos++] = '#';
                            for (const char *q = utf8; *q != '\0' && pos + 2 < cap; q++) {
                                out[pos++] = *q;
                                if (*q == '#') out[pos++] = '#';
                            }
                            n = snprintf(out + pos, cap - pos, "#%06x ",
                                         (unsigned)(col & 0xFFFFFFu));
                            if (n < 0) break;
                            if ((size_t)n >= cap - pos) { pos = cap - 1; break; }
                            pos += (size_t)n;
                            continue;
                        }
                        for (const char *q = utf8; *q != '\0' && pos + 2 < cap; q++) {
                            out[pos++] = *q;
                            if (*q == '#') out[pos++] = '#';
                        }
                    }
                    if (tagged && pos + 1 < cap) out[pos++] = '#';
                    c += run;
                }
            }
        }
        if (r < row2 && pos + 1 < cap) out[pos++] = '\n';
    }
    if (pos >= cap) pos = cap - 1;
    out[pos] = '\0';
    return pos;
}

/** Render the whole grid (the LVGL label payload). Pure. */
static inline size_t tui_frame_render_full(const tui_frame_t *f,
                                           char *out, size_t cap)
{
    return tui_frame_render_span(f, 1, tui_frame_valid(f) ? f->rows : 0,
                                 out, cap);
}

/**
 * Append `NNN@start-end:fg ...` colour-run lines for rows [row1..row2], the
 * same format `tui dump /a` emits. Used by the host gate to render-check scenes
 * without pixels and without a second colour model. Pure.
 */
static inline size_t tui_frame_render_runs(const tui_frame_t *f, int row1,
                                           int row2, char *out, size_t cap)
{
    size_t pos = 0;

    if (!tui_frame_valid(f) || out == NULL || cap == 0) return 0;
    out[0] = '\0';
    if (row1 < 1) row1 = 1;
    if (row2 > f->rows) row2 = f->rows;
    for (int r = row1; r <= row2; r++) {
        int run_start = 1;
        int run_fg = -1;
        int run_end = 0;
        int n;

        n = snprintf(out + pos, cap - pos, "%03d@", r);
        if (n < 0 || (size_t)n >= cap - pos) return pos;
        pos += (size_t)n;
        for (int c = 1; c <= f->cols; c++) {
            uint8_t fg = f->working[(r - 1) * f->cols + (c - 1)].fg;

            if (run_fg < 0) {
                run_start = c;
                run_fg = fg;
                run_end = c;
            } else if ((int)fg == run_fg) {
                run_end = c;
            } else {
                n = snprintf(out + pos, cap - pos, "%s%d-%d:%d",
                             pos && out[pos - 1] != '@' ? " " : "",
                             run_start, run_end, run_fg);
                if (n < 0 || (size_t)n >= cap - pos) return pos;
                pos += (size_t)n;
                run_start = c;
                run_fg = fg;
                run_end = c;
            }
        }
        if (run_fg >= 0) {
            n = snprintf(out + pos, cap - pos, "%s%d-%d:%d",
                         pos && out[pos - 1] != '@' ? " " : "",
                         run_start, run_end, run_fg);
            if (n < 0 || (size_t)n >= cap - pos) return pos;
            pos += (size_t)n;
        }
        if (pos + 2 >= cap) return pos;
        out[pos++] = '\n';
        out[pos] = '\0';
    }
    return pos;
}

/* ========================================================================
 * MULTITASKING: edge-swipe predicate + DOS app switcher frame
 * ========================================================================
 * The TUI touch callback already owns press/move/release. It asks THIS pure
 * predicate whether a stroke was a bottom-left edge swipe, then calls the one
 * registered session-switch handler. No second gesture engine, and the host
 * modal gate drives the identical function with mock pointer vectors.
 */

/**
 * @brief True for a bottom-left edge swipe: press in the bottom-left quarter
 * of the panel, then travel right and up past a quarter of each axis.
 *
 * Pure: no LVGL, display or allocation. @p w/@p h are logical display pixels.
 */
static inline bool tui_edge_swipe_bottom_left(int x0, int y0, int x1, int y1,
                                               int w, int h)
{
    int dx;
    int dy;

    if (w < 40 || h < 24) {
        return false;
    }
    if (x0 < 0 || x0 > w / 4 || y0 < h * 3 / 4 || y0 >= h) {
        return false;
    }
    dx = x1 - x0;
    dy = y1 - y0;
    return dx >= w / 4 && dy <= -h / 4;
}

/** Bounded ASCII text writer for the switcher; every cell goes through the
 *  ONE tui_frame_set() cell writer. Pure. */
static inline void tui_frame_switcher_text(tui_frame_t *f, int col, int row,
                                            const char *text, int max_chars,
                                            uint8_t fg, uint8_t bg, uint8_t attr)
{
    char ch[2] = { '\0', '\0' };
    int i = 0;

    if (!tui_frame_valid(f) || text == NULL || max_chars <= 0) return;
    for (; *text != '\0' && i < max_chars; text++, i++) {
        ch[0] = *text;
        tui_frame_set(f, col + i, row, ch, fg, bg, attr);
    }
}

/**
 * @brief Draw the DOS-style app switcher into a frame (1-based, 80x25 box).
 *
 * The box is clipped to the bound grid, so a narrower live panel cannot clip
 * a cell. @p names are in from-top order; @p focus is the from-top index of
 * the highlighted session. Uses only the one cell writer / one CP437 palette.
 * Pure; the firmware wrapper in tui.c flushes once through tui_flush().
 */
static inline void tui_frame_draw_app_switcher(tui_frame_t *f,
                                               const char *const *names,
                                               int count, int focus)
{
    const int bx = 8;
    const int by = 3;
    const int bw = 64;
    const int bh = 19;
    int c;
    int r;
    int i;
    char line[64];

    if (!tui_frame_valid(f)) return;

    /* Box frame: top/bottom rails and both sides, single CP437 line glyphs. */
    tui_frame_set(f, bx, by, SH_BOX_TL, 7, 1, 0);
    tui_frame_set(f, bx + bw - 1, by, SH_BOX_TR, 7, 1, 0);
    tui_frame_set(f, bx, by + bh - 1, SH_BOX_BL, 7, 1, 0);
    tui_frame_set(f, bx + bw - 1, by + bh - 1, SH_BOX_BR, 7, 1, 0);
    for (c = bx + 1; c < bx + bw - 1; c++) {
        tui_frame_set(f, c, by, SH_BOX_H, 7, 1, 0);
        tui_frame_set(f, c, by + bh - 1, SH_BOX_H, 7, 1, 0);
    }
    for (r = by + 1; r < by + bh - 1; r++) {
        tui_frame_set(f, bx, r, SH_BOX_V, 7, 1, 0);
        tui_frame_set(f, bx + bw - 1, r, SH_BOX_V, 7, 1, 0);
    }

    {
        const char *title = " P4MiniShell APP SWITCHER ";
        int title_len = (int)strlen(title);

        tui_frame_set(f, bx + 2, by, SH_BOX_L, 7, 1, 0);
        tui_frame_switcher_text(f, bx + 3, by, title, title_len, 15, 1, 1);
        tui_frame_set(f, bx + 3 + title_len, by, SH_BOX_R, 7, 1, 0);
    }

    /* Up to eight sessions: number, name and a bold focus arrow. */
    for (i = 0; i < 8; i++) {
        bool focused = (i == focus);
        uint8_t fg = focused ? 14 : 7;
        uint8_t attr = focused ? 1 : 0;
        const char *name = (names != NULL && i < count && names[i] != NULL)
                           ? names[i] : "";

        r = by + 2 + i;
        tui_frame_set(f, bx + 1, r, focused ? ">" : " ", fg, 1, attr);
        snprintf(line, sizeof(line), "%2d  %-32s", i + 1, name);
        tui_frame_switcher_text(f, bx + 3, r, line, bw - 5, fg, 1, attr);
    }
    if (count <= 0) {
        tui_frame_switcher_text(f, bx + 3, by + 2, "(no sessions)",
                                bw - 5, 8, 1, 0);
    }

    tui_frame_switcher_text(f, bx + 2, by + bh - 2,
                            " Alt+Tab / bottom-left swipe: next app   Esc: close ",
                            bw - 4, 8, 1, 0);
}

/* ========================================================================
 * COLOUR: 24-bit / 256 mapped through the ONE DOS palette
 * ======================================================================== */

/** xterm-256 index 16..255 -> 24-bit RGB; 0..15 keep the DOS RGB values.
 *  This is an index decoder, not a colour parser: the only quantiser remains
 *  tui_rgb_to_dos(). Pure. */
static inline uint32_t tui_256_rgb(uint8_t index)
{
    static const int levels[6] = {0, 95, 135, 175, 215, 255};
    int r, g, b;

    if (index < 16) return tui_dos_color_rgb(index);
    if (index < 232) {
        int n = index - 16;

        r = levels[n / 36];
        g = levels[(n / 6) % 6];
        b = levels[n % 6];
    } else {
        int v = 8 + (index - 232) * 10;

        if (v > 255) v = 255;
        r = g = b = v;
    }
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/** Nearest DOS index for a 24-bit RGB value (the ONE palette). Pure. */
static inline uint8_t tui_frame_rgb_to_dos(uint32_t rgb)
{
    return tui_rgb_to_dos(rgb);
}

/** Nearest DOS index for an xterm-256 index. Pure. */
static inline uint8_t tui_frame_256_to_dos(uint8_t index)
{
    return tui_rgb_to_dos(tui_256_rgb(index));
}

/** Write a cell whose colours arrive as 24-bit RGB (e.g. CSS `rgba()`),
 *  quantised through the ONE DOS palette. `has_bg=false` leaves bg at 16.
 *  Pure. */
static inline void tui_frame_set_rgb(tui_frame_t *f, int col, int row,
                                     const char *utf8, uint32_t fg_rgb,
                                     uint32_t bg_rgb, bool has_bg,
                                     uint8_t attr)
{
    tui_frame_set(f, col, row, utf8, tui_frame_rgb_to_dos(fg_rgb),
                  has_bg ? tui_frame_rgb_to_dos(bg_rgb) : 16, attr);
}

/* ========================================================================
 * THE ONE IMAGE-TO-CELL SAMPLER (shared by grid, page and sprite cells)
 * ======================================================================== */

/** Sink for one sampled cell: 1-based col/row, ramp glyph, DOS fg index. */
typedef void (*tui_pixel_sink_t)(void *ctx, int col, int row, char glyph,
                                 uint8_t fg);

/**
 * Sample an RGB565 surface into an w x h cell region. One nearest-neighbour
 * sample per cell, luminance -> the ONE shared ramp (ansi_image_glyph), colour
 * -> the ONE DOS palette (tui_rgb_to_dos). No second sampler or colour parser.
 * Pure; clipping is the sink's job (as in tui.c's grid/page sinks).
 */
static inline void tui_sample_image_cells(int x, int y, int w, int h,
                                          const gfx_surface_t *img,
                                          tui_pixel_sink_t sink, void *ctx)
{
    if (img == NULL || img->px == NULL || w < 1 || h < 1 || sink == NULL) return;
    for (int cy = 0; cy < h; cy++) {
        int sy = (int)((int64_t)cy * img->h / h);

        if (sy >= img->h) sy = img->h - 1;
        for (int cx = 0; cx < w; cx++) {
            int sx = (int)((int64_t)cx * img->w / w);
            uint16_t px;
            uint32_t rgb;
            int r, g, b, lum, level;

            if (sx >= img->w) sx = img->w - 1;
            px = img->px[(size_t)sy * (size_t)img->w + (size_t)sx];
            r = ((px >> 11) & 0x1F) << 3;
            g = ((px >> 5) & 0x3F) << 2;
            b = (px & 0x1F) << 3;
            rgb = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
            /* RGB565 white is 248/252/248, not 255/255/255. Normalise by the
             * weighted maximum a 5/6/5 pixel can hold so white reaches the top
             * ramp glyph instead of stopping one level short. */
            lum = (r * 30 + g * 59 + b * 11) * 255 / 25036;
            level = lum * ANSI_IMAGE_RAMP_LAST / 255;
            if (level < 0) level = 0;
            if (level > ANSI_IMAGE_RAMP_LAST) level = ANSI_IMAGE_RAMP_LAST;
            sink(ctx, x + cx, y + cy, ansi_image_glyph((char)level),
                 tui_rgb_to_dos(rgb));
        }
    }
}

/** Sprite-cell sink for tui_sample_image_cells(): writes the sampled glyph
 *  and DOS fg into a frame's working grid. Pure (no LVGL). */
static inline void tui_frame_sprite_sink(void *ctx, int col, int row,
                                         char glyph, uint8_t fg)
{
    tui_frame_t *f = (tui_frame_t *)ctx;
    char ch[2];

    ch[0] = glyph;
    ch[1] = '\0';
    tui_frame_set(f, col, row, ch, fg, 16, 0);
}

static inline void tui_frame_draw_sprite(tui_frame_t *f, int x, int y,
                                         int w, int h, const gfx_surface_t *img)
{
    if (!tui_frame_valid(f)) return;
    tui_sample_image_cells(x, y, w, h, img, tui_frame_sprite_sink, f);
}

/* ========================================================================
 * LIVE FRAME (implemented in tui.c, declared here to avoid a tui.h cycle)
 * ======================================================================== */

/** Read-only view of the live double-buffered frame. NULL storage when TUI
 *  mode is inactive; the descriptor itself is always valid. */
const tui_frame_t *tui_live_frame(void);

/** Diff the live frame (presented vs working). Pure read; inactive = empty. */
tui_dirty_t tui_live_frame_dirty(void);

/** Draw an RGB565 sprite through the ONE sampler into the live working grid.
 *  The caller flushes once (tui_flush is still the only LVGL path). */
void tui_draw_sprite(int x, int y, int w, int h, const gfx_surface_t *img);

/** Print text at 1-based (col,row) with 24-bit RGB colours quantised through
 *  the ONE DOS palette. has_bg=false keeps the default background (16). */
void tui_print_rgb(int col, int row, const char *text,
                   uint32_t fg_rgb, uint32_t bg_rgb, bool has_bg);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_TUI_FRAME_H */
