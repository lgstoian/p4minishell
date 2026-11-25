/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file imagefile.h
 * @brief One in-memory image decode: any supported format, any target size.
 *
 * BMP is decoded by the gfx core, PNG/JPEG by imgdec. Keeping this glue in its
 * own leaf matters: gfx must stay LVGL-free (the host unit lane links it), so
 * it cannot require imgdec, while emoji packs and the image/modal path can.
 *
 * @module imagefile
 * @layer leaf
 * @domain ui
 * @provides decode BMP/PNG/JPEG bytes to a scaled RGB565 gfx surface
 * @status stable
 * @test test/main/test_emoji.c
 * @hard-rule ai-context.md#module-layering-rules
 */
#ifndef P4MINISHELL_IMAGEFILE_H
#define P4MINISHELL_IMAGEFILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gfx.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Decode @p data (BMP, PNG or JPEG) to a @p w x @p h RGB565 surface.
 *  The caller owns @p out (gfx_surface_free).
 *  @return false on an unsupported/corrupt image or allocation failure. */
bool imagefile_decode_mem(const uint8_t *data, size_t len, int w, int h,
                          gfx_surface_t *out);

/** Native pixel size of an in-memory image (BMP header or PNG/JPEG probe).
 *  Used by callers that fit an image into a cell grid before decoding. */
bool imagefile_probe_mem(const uint8_t *data, size_t len, int *w, int *h);

/**
 * Decode an image through the ONE decoder at the largest size that fits
 * @p max_w x @p max_h while preserving aspect (gfx_bmp_fit contract, never
 * upscales). The native image is never materialised; @p out holds the fitted
 * RGB565 surface and its dimensions on success. This is the web/markdown
 * image path: callers that then need DOS cells use imagefile_cells.h, which
 * wraps the ONE tui_sample_image_cells() sampler - no second decoder, no
 * second sampler, no second palette.
 */
bool imagefile_decode_fit(const uint8_t *data, size_t len,
                          int max_w, int max_h, gfx_surface_t *out);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_IMAGEFILE_H */
