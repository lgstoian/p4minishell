/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file imagefile_cells.h
 * @brief Thin adapter from the ONE image-to-cell sampler to DOS cell grids.
 *
 * `tui_frame.h` owns the only RGB565 -> (ramp glyph, DOS fg) sampler in the
 * firmware. Callers that need DOM image cells (dom_image_t packs
 * `(dos_fg << 4) | ramp_level` in one byte) must not write a second sampler:
 * include this header, call imagefile_sample_rgb565_cells() with a sink, or
 * use the ready-made grid sink. The adapter is header-only, so no component
 * dependency or extra translation unit is added; the caller links the same
 * tui/ansi code it already uses for every other cell surface.
 *
 * @module imagefile
 * @layer leaf
 * @domain ui
 * @provides one-sampler RGB565 -> DOS cell grid adapter
 * @status stable
 * @test test/main/test_imgdec.c
 * @hard-rule ai-context.md#module-layering-rules
 */
#ifndef P4MINISHELL_IMAGEFILE_CELLS_H
#define P4MINISHELL_IMAGEFILE_CELLS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "imagefile.h"
#include "tui_frame.h" /* the ONE sampler: tui_sample_image_cells */
#include "ansi.h"      /* the ONE ramp: ANSI_IMAGE_RAMP / _LAST */

#ifdef __cplusplus
extern "C" {
#endif

/** One sampled DOS cell: 1-based col/row, DOS fg 0..15, ramp level 0..9. */
typedef void (*imagefile_cell_sink_t)(void *ctx, int col, int row,
                                      uint8_t dos_fg, uint8_t level);

/** Invert the one glyph ramp without reimplementing the colour mapping. */
static inline uint8_t imagefile_ramp_level(char glyph)
{
    const char *hit = strchr(ANSI_IMAGE_RAMP, glyph);
    int level = (hit != NULL) ? (int)(hit - ANSI_IMAGE_RAMP) : 0;

    if (level < 0) level = 0;
    if (level > ANSI_IMAGE_RAMP_LAST) level = ANSI_IMAGE_RAMP_LAST;
    return (uint8_t)level;
}

/** Internal: translate the sampler's glyph sink into a (dos, level) sink. */
typedef struct {
    imagefile_cell_sink_t sink;
    void *ctx;
} imagefile_cell_adapter_t;

static inline void imagefile_cell_forward(void *ctx, int col, int row,
                                          char glyph, uint8_t fg)
{
    imagefile_cell_adapter_t *a = (imagefile_cell_adapter_t *)ctx;

    a->sink(a->ctx, col, row, fg, imagefile_ramp_level(glyph));
}

/**
 * Sample an RGB565 surface into an w x h cell region through the ONE
 * tui_sample_image_cells() sampler. Pure: no allocation, no display. Col/row
 * follow the sampler's 1-based convention; clipping belongs to the sink.
 */
static inline void imagefile_sample_rgb565_cells(int x, int y, int w, int h,
                                                 const gfx_surface_t *img,
                                                 imagefile_cell_sink_t sink,
                                                 void *ctx)
{
    imagefile_cell_adapter_t adapter;

    if (sink == NULL) return;
    adapter.sink = sink;
    adapter.ctx = ctx;
    tui_sample_image_cells(x, y, w, h, img, imagefile_cell_forward, &adapter);
}

/** Caller-owned grid for the ready-made dom_image_t-compatible sink. */
typedef struct {
    uint8_t *pixels; /**< Row-major bytes, (dos_fg << 4) | level. */
    int grid_w;      /**< Grid width in cells. */
    int grid_h;      /**< Grid height in cells. */
    int origin_col;  /**< Sampler col mapped to grid column 0 (usually 1). */
    int origin_row;  /**< Sampler row mapped to grid row 0 (usually 1). */
} imagefile_cell_grid_t;

/** Sink writing dom_image_t's exact packed byte; out-of-grid cells are clipped. */
static inline void imagefile_grid_cell_sink(void *ctx, int col, int row,
                                            uint8_t dos_fg, uint8_t level)
{
    imagefile_cell_grid_t *g = (imagefile_cell_grid_t *)ctx;
    int cx;
    int cy;

    if (g == NULL || g->pixels == NULL) return;
    cx = col - g->origin_col;
    cy = row - g->origin_row;
    if (cx < 0 || cy < 0 || cx >= g->grid_w || cy >= g->grid_h) return;
    g->pixels[(size_t)cy * (size_t)g->grid_w + (size_t)cx] =
        (uint8_t)(((unsigned)dos_fg << 4) | ((unsigned)level & 0x0Fu));
}

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_IMAGEFILE_CELLS_H */
