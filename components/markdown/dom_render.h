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
} dom_image_t;

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
    void *ctx;
    int cols;             /**< TUI columns / ANSI wrap width. */
    int rows;             /**< TUI rows. */
    bool tui;             /**< true: cells; false: ANSI. */
    bool ansi_links;      /**< Append " (url)" after link text (reader). */
    const char *base_url; /**< Page URL for relative images (may be NULL). */
} dom_render_ops_t;

/** Render @p tree through @p ops. Safe on NULL (no-op). */
void dom_render_tree(dom_tree_t *tree, const dom_render_ops_t *ops);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_DOM_RENDER_H */
