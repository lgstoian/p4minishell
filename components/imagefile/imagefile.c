/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file imagefile.c
 * @brief The one in-memory image decoder: any supported format, any target
 *        size, RGB565 out.
 *
 * A BMP is decoded by gfx's own BMP core; PNG/JPEG go through imgdec (LodePNG
 * / TJpgDec). Callers that used to branch on the format inline (emoji packs,
 * and the image/modal path) call this instead, so "what formats exist" and
 * "which decoder owns them" are answered in exactly one place.
 *
 * @module imagefile
 * @layer leaf
 * @domain ui
 * @provides in-memory image decode (BMP/PNG/JPEG) to a scaled RGB565 surface
 * @status stable
 * @test test/main/test_emoji.c, test/main/test_gfx.c
 * @hard-rule ai-context.md#module-layering-rules
 */

#include "imagefile.h"
#include "imgdec.h"

#include <string.h>

bool imagefile_decode_mem(const uint8_t *data, size_t len, int w, int h,
                          gfx_surface_t *out)
{
    imgdec_format_t fmt;
    int native_w = 0;
    int native_h = 0;

    if (data == NULL || len == 0 || out == NULL || w < 1 || h < 1) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    fmt = imgdec_probe(data, len, &native_w, &native_h);

    if (fmt == IMGDEC_FMT_PNG || fmt == IMGDEC_FMT_JPEG) {
        imgdec_image_t imd;

        if (!imgdec_decode_scaled(data, len, w, h, &imd)) {
            return false;
        }
        out->px = imd.pixels;
        out->w = imd.width;
        out->h = imd.height;
        return true;
    }

    {
        gfx_bmp_info_t info;

        if (!gfx_bmp_parse_header_ex(data, len, &info, GFX_IMAGE_MAX_W,
                                     GFX_IMAGE_MAX_H)) {
            return false;
        }
        return gfx_bmp_decode_scaled_565(data, len, &info, w, h, out);
    }
}
