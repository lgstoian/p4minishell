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

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_IMAGEFILE_H */
