/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file imgdec.h
 * @module imgdec
 * @layer leaf
 * @domain media
 * @provides the ONE PNG + JPEG decoder (LodePNG / TJpgDec, the libs LVGL already compiles)
 * @status stable
 * @test test/main/test_imgdec.c
 * @hard-rule ai-context.md#image-support-rules
 * @brief PNG + JPEG decode to a scaled RGB565 buffer.
 *
 * `components/gfx/` owns BMP (the strict sprite path and the `gfx load` contract);
 * this leaf owns the two formats the web actually uses. Both decoders are the
 * ones LVGL already compiles into its component (`src/libs/lodepng`,
 * `src/libs/tjpgd`), so there is exactly one PNG decoder and one JPEG decoder in
 * the firmware — this file is a thin wrapper, not a second implementation.
 *
 * Decode goes straight to the target size (nearest neighbour, same contract as
 * `gfx_bmp_decode_scaled_565`): the native image is never materialised as a
 * second surface. Output is RGB565 so the TUI/browser path can map it through
 * `tui_rgb_to_dos()` exactly like a decoded BMP.
 */

#ifndef P4MINISHELL_IMGDEC_H
#define P4MINISHELL_IMGDEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Detected image format (magic-byte sniff, no decode). */
typedef enum {
    IMGDEC_FMT_UNKNOWN = 0,
    IMGDEC_FMT_BMP,     /**< 24/32-bit BI_RGB (owned by components/gfx) */
    IMGDEC_FMT_PNG,     /**< PNG (LodePNG) */
    IMGDEC_FMT_JPEG,    /**< JPEG (TJpgDec) */
} imgdec_format_t;

/** One decoded, scaled image. `pixels` is a PSRAM RGB565 buffer (w*h*2 bytes)
 *  owned by the caller; release with imgdec_free(). */
typedef struct {
    uint16_t *pixels;   /**< RGB565, row-major, w*h*2 bytes. */
    int width;          /**< Decoded width in pixels. */
    int height;         /**< Decoded height in pixels. */
} imgdec_image_t;

/**
 * Sniff the format and (for PNG/JPEG) read the native dimensions without
 * decoding. @p native_w/@p native_h are optional.
 * @return the format; IMGDEC_FMT_UNKNOWN when the data is not PNG/JPEG/BMP.
 */
imgdec_format_t imgdec_probe(const uint8_t *data, size_t len,
                             int *native_w, int *native_h);

/**
 * Decode a PNG or JPEG to a dst_w x dst_h RGB565 buffer (nearest neighbour).
 *
 * @param data      Encoded image bytes.
 * @param len       Byte length.
 * @param dst_w     Target width (>= 1).
 * @param dst_h     Target height (>= 1).
 * @param out       Receives the allocated pixels (PSRAM); free with
 *                  imgdec_free(). Zeroed on failure.
 * @return true on success.
 */
bool imgdec_decode_scaled(const uint8_t *data, size_t len,
                          int dst_w, int dst_h, imgdec_image_t *out);

/** Free the pixels returned by imgdec_decode_scaled(). NULL-safe. */
void imgdec_free(imgdec_image_t *img);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_IMGDEC_H */
