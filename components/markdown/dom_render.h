/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file dom_render.h
 * @brief One DOM layout renderer for every surface (TUI page and ANSI).
 *
 * The browser, the HTML reader, and any future DOM consumer share this
 * renderer: it walks the DOM (components/markdown/dom.c), honours the computed
 * CSS (components/markdown/css.c), and emits through a caller-supplied sink.
 * There is no second layout engine.
 *
 * A TUI caller provides put_cell/put_link (the page buffer + tap regions); an
 * ANSI caller provides put_ansi. Images are optional: the caller supplies
 * load_image (the browser resolves/fetches/decodes; the markdown leaf never
 * touches networking/storage, preserving the one-way layering).
 */
#ifndef P4MINISHELL_DOM_RENDER_H
#define P4MINISHELL_DOM_RENDER_H

#include "dom.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** A decoded image as DOS 16-color cells (one byte per pixel).
 *
 * The caller's load_image owns the decode (BMP via components/gfx) and the
 * RGB565 -> DOS mapping (tui_rgb_to_dos); the renderer only samples the grid.
 * Each byte packs `(dos_fg << 4) | ramp_level`: the high nibble is the DOS
 * foreground colour and the low nibble is the luminance-ramp level (0..9)
 * that picks the shade glyph. Packing keeps one byte per pixel while letting
 * the renderer choose a glyph that is never the LVGL recolor delimiter '#'
 * (see ansi_image_glyph in components/ansi/ansi.h - the one shared ramp).
 * @p pixels is a plain malloc() buffer the renderer frees. */
typedef struct {
    int width;
    int height;
    uint8_t *pixels;
    /* Native size is tracked by the caller's report sink, not in this struct:
     * the browser cache copies exactly width*height bytes and must not see a
     * layout change. */
} dom_image_t;

/** Sample an RGB565 pixel grid into packed DOS cells: the pure, hardware-free
 *  equivalent of tui_sample_image_cells() for the DOM image sink. It uses the
 *  ONE shared luminance ramp (ansi_image_glyph) and the ONE DOS palette
 *  (ansi_cga_nearest) - the same mapping the live TUI sampler applies - so an
 *  image, a canvas bitmap and a CSS backdrop never get a second sampler or a
 *  second colour model. Nearest-neighbour sampling preserves the source
 *  aspect when dst_w/dst_h are derived from it. Pure; no allocation. */
bool dom_image_sample_rgb565(const uint16_t *src, int src_w, int src_h,
                             uint8_t *dst, int dst_w, int dst_h);

/** Outcome of one image on a rendered page (report sink below). */
typedef enum {
    DOM_IMAGE_OK,        /**< decoded and drawn at native cell size */
    DOM_IMAGE_RESIZED,   /**< decoded, fitted to the page budget */
    DOM_IMAGE_MISSING,   /**< decode/load failed; alt text is shown */
    DOM_IMAGE_FALLBACK   /**< no bitmap; element fallback content is shown */
} dom_image_state_t;

/** Structured report for one image sink call. No formatted string is baked
 *  into the leaf: each surface renders these fields in its own idiom. */
typedef struct {
    const char *kind;       /**< "img", "background" or "canvas" */
    const char *src;        /**< resolved source (basename is fine) */
    dom_image_state_t state;
    int native_w;
    int native_h;
    int fit_w;
    int fit_h;
    const char *alt;        /**< NULL or "" when the element has no alt text */
} dom_image_report_t;

/** Output sinks. Set exactly one of the cell/ANSI paths. */
typedef struct {
    /** TUI cell sink (1-based col/row). NULL for ANSI. */
    void (*put_cell)(void *ctx, int col, int row, const char *utf8,
                     uint8_t fg, uint8_t bg, uint8_t attr);
    /** TUI link region (1-based x/y, w/h). NULL to skip link registration. */
    void (*put_link)(void *ctx, int x, int y, int w, int h, const char *url);
    /** ANSI sink. NULL for TUI. */
    void (*put_ansi)(void *ctx, const char *text);
    /** Optional image loader. Return false to fall back to alt text. */
    bool (*load_image)(void *ctx, const char *src, dom_image_t *out);
    /** Optional <canvas> bitmap loader (the browser runs the scripts first).
     *  Return false to render the element's fallback content, which is what a
     *  no-JS surface (editor preview, print-to-HTML, EPUB) does. */
    bool (*load_canvas)(void *ctx, dom_node_t *node, dom_image_t *out);
    /** CSS background: fill a rectangle's background colour, keeping any text
     *  already drawn in those cells. NULL on surfaces without cell colours.
     *  @p alpha is 0-255: 0 means fully transparent and is never called, so
     *  the surface behind a `transparent`/`rgba(...,0)` block shows through. */
    void (*fill_bg)(void *ctx, int col, int row, int w, int h, uint8_t bg,
                    uint8_t alpha);
    /** Optional CSS background-image loader (page backdrop). */
    bool (*load_background)(void *ctx, const char *src, dom_image_t *out);
    /** Optional emoji fallback. Given one UTF-8 sequence, return a replacement
     *  string (written into @p buf, NUL-terminated) or NULL to draw it as-is.
     *  The caller owns the emoji core; the renderer stays table-free. */
    const char *(*emoji_fallback)(void *ctx, const char *seq, size_t len,
                                  char buf[5]);
    /** Optional emoji blit for cell surfaces (pack images). Return true when
     *  the sequence was drawn and store the cells consumed in @p cells_out
     *  (0 for a zero-width modifier). Called before emoji_fallback. */
    bool (*put_emoji)(void *ctx, int col, int row, const char *seq, size_t len,
                      int *cells_out);
    /** Optional image outcome sink: one call per <img>, <canvas> or CSS
     *  background-image. A missing decode is reported before the alt text is
     *  drawn and an oversized image reports its fitted size, so a host gate or
     *  a batch caller can prove nothing was dropped silently. */
    void (*report_image)(void *ctx, const dom_image_report_t *report);
    void *ctx;
    int cols;             /**< TUI columns / ANSI wrap width. */
    int rows;             /**< TUI rows. */
    bool tui;             /**< true: cells; false: ANSI. */
    bool ansi_links;      /**< Append " (url)" after link text (reader). */
    uint8_t base_bg;      /**< Surface background (DOS index) used to composite
                               translucent text on a cell surface. */
    const char *base_url; /**< Page URL for relative images (may be NULL). */
} dom_render_ops_t;

/** Render @p tree through @p ops. Safe on NULL (no-op). */
void dom_render_tree(dom_tree_t *tree, const dom_render_ops_t *ops);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_DOM_RENDER_H */
