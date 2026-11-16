/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file imgdec.c
 * @brief PNG (LodePNG) + JPEG (TJpgDec) decode to a scaled RGB565 buffer.
 *
 * Both decoders are the ones LVGL already compiles into its component
 * (`src/libs/lodepng`, `src/libs/tjpgd`), so there is exactly one PNG decoder
 * and one JPEG decoder in the firmware. This file only wraps them and does the
 * RGB8888/RGB888 -> RGB565 nearest-neighbour scale that the TUI/browser path
 * needs.
 *
 * The native decode uses LVGL's allocator (LodePNG's `lodepng_malloc` maps to
 * `lv_malloc`), so the source is capped by `P4_CONFIG_IMGDEC_MAX_W/H`; the
 * scaled output is PSRAM (`p4heap`), like every other bulk surface.
 */

#include "imgdec.h"

#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "libs/lodepng/lodepng.h"
#include "libs/tjpgd/tjpgd.h"

#include "p4minishell_config.h"
#include "esp_heap_caps.h"
#include "p4heap.h"

/** TJpgDec work pool (the library's recommended size). */
#define IMGDEC_JPEG_POOL_BYTES 4096

/* ------------------------------------------------------------------------- */
/* Magic-byte probing                                                        */
/* ------------------------------------------------------------------------- */

static const uint8_t PNG_SIG[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

/** PNG IHDR: width at [16..19], height at [20..23], both big-endian. */
static bool imgdec_png_dims(const uint8_t *d, size_t n, int *w, int *h)
{
    if (d == NULL || n < 24 || memcmp(d, PNG_SIG, sizeof(PNG_SIG)) != 0) {
        return false;
    }
    *w = ((int)d[16] << 24) | ((int)d[17] << 16) | ((int)d[18] << 8) | (int)d[19];
    *h = ((int)d[20] << 24) | ((int)d[21] << 16) | ((int)d[22] << 8) | (int)d[23];
    return (*w > 0 && *h > 0 && *w <= P4_CONFIG_IMGDEC_MAX_W &&
            *h <= P4_CONFIG_IMGDEC_MAX_H);
}

/** Walk JPEG markers to the SOF frame header for the native dimensions. */
static bool imgdec_jpeg_dims(const uint8_t *d, size_t n, int *w, int *h)
{
    size_t i = 2;

    if (d == NULL || n < 4 || d[0] != 0xFF || d[1] != 0xD8) {
        return false;
    }
    while (i + 3 < n) {
        uint8_t marker;

        if (d[i] != 0xFF) {
            i++;
            continue;
        }
        marker = d[i + 1];
        if (marker == 0xD8 || marker == 0x01 ||
            (marker >= 0xD0 && marker <= 0xD7)) {
            i += 2;
            continue;
        }
        if (marker == 0xD9 || marker == 0xDA) {
            return false;   /* EOI / SOS before an SOF: no dimensions */
        }
        {
            int seglen = ((int)d[i + 2] << 8) | (int)d[i + 3];

            /* A length shorter than the length field itself cannot advance
             * past the marker; treat it as a malformed stream instead of
             * walking the buffer in 2-byte steps. */
            if (seglen < 2) {
                return false;
            }
            if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 &&
                marker != 0xC8 && marker != 0xCC) {
                if (i + 9 > n) {
                    return false;
                }
                *h = ((int)d[i + 5] << 8) | (int)d[i + 6];
                *w = ((int)d[i + 7] << 8) | (int)d[i + 8];
                return (*w > 0 && *h > 0 && *w <= P4_CONFIG_IMGDEC_MAX_W &&
                        *h <= P4_CONFIG_IMGDEC_MAX_H);
            }
            i += 2 + (size_t)seglen;
        }
    }
    return false;
}

imgdec_format_t imgdec_probe(const uint8_t *data, size_t len,
                             int *native_w, int *native_h)
{
    int w = 0;
    int h = 0;

    if (data == NULL || len < 4) {
        return IMGDEC_FMT_UNKNOWN;
    }
    if (imgdec_png_dims(data, len, &w, &h)) {
        if (native_w != NULL) *native_w = w;
        if (native_h != NULL) *native_h = h;
        return IMGDEC_FMT_PNG;
    }
    if (imgdec_jpeg_dims(data, len, &w, &h)) {
        if (native_w != NULL) *native_w = w;
        if (native_h != NULL) *native_h = h;
        return IMGDEC_FMT_JPEG;
    }
    if (data[0] == 'B' && data[1] == 'M') {
        return IMGDEC_FMT_BMP;
    }
    return IMGDEC_FMT_UNKNOWN;
}

/* ------------------------------------------------------------------------- */
/* Scaling: native pixels -> target RGB565 (nearest neighbour)               */
/* ------------------------------------------------------------------------- */

/** Convert one RGB888 pixel to RGB565. */
uint16_t imgdec_rgb888_to_565(uint32_t rgb)
{
    return (uint16_t)(((rgb & 0xF80000u) >> 19) |
                      ((rgb & 0x00FC00u) >> 5) |
                      ((rgb & 0x0000F8u) >> 3));
}

static uint16_t *imgdec_scale_rgb888(const uint8_t *src, int src_w, int src_h,
                                     int dst_w, int dst_h)
{
    uint16_t *out;
    int y;
    int x;

    if (src == NULL || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) {
        return NULL;
    }
    out = p4heap_alloc_psram((size_t)dst_w * (size_t)dst_h * 2u);
    if (out == NULL) {
        return NULL;
    }
    for (y = 0; y < dst_h; y++) {
        int sy = y * src_h / dst_h;
        const uint8_t *row = src + (size_t)sy * (size_t)src_w * 3u;

        for (x = 0; x < dst_w; x++) {
            int sx = x * src_w / dst_w;
            const uint8_t *px = row + (size_t)sx * 3u;

            out[(size_t)y * (size_t)dst_w + (size_t)x] =
                imgdec_rgb888_to_565(((uint32_t)px[0] << 16) |
                                     ((uint32_t)px[1] << 8) | px[2]);
        }
    }
    return out;
}

static uint16_t *imgdec_scale_rgba(const uint8_t *src, int src_w, int src_h,
                                   int dst_w, int dst_h)
{
    uint16_t *out;
    int y;
    int x;

    if (src == NULL || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) {
        return NULL;
    }
    out = p4heap_alloc_psram((size_t)dst_w * (size_t)dst_h * 2u);
    if (out == NULL) {
        return NULL;
    }
    for (y = 0; y < dst_h; y++) {
        int sy = y * src_h / dst_h;
        const uint8_t *row = src + (size_t)sy * (size_t)src_w * 4u;

        for (x = 0; x < dst_w; x++) {
            int sx = x * src_w / dst_w;
            const uint8_t *px = row + (size_t)sx * 4u;
            /* Composite onto white (the TUI ramp has no alpha). */
            uint32_t a = px[3];
            uint32_t r = (px[0] * a + 255u * (255u - a)) / 255u;
            uint32_t g = (px[1] * a + 255u * (255u - a)) / 255u;
            uint32_t b = (px[2] * a + 255u * (255u - a)) / 255u;

            out[(size_t)y * (size_t)dst_w + (size_t)x] =
                imgdec_rgb888_to_565((r << 16) | (g << 8) | b);
        }
    }
    return out;
}

/* ------------------------------------------------------------------------- */
/* PNG (LodePNG)                                                             */
/* ------------------------------------------------------------------------- */

static bool imgdec_png_decode(const uint8_t *data, size_t len,
                              int dst_w, int dst_h, imgdec_image_t *out)
{
    unsigned char *rgba = NULL;
    unsigned w = 0;
    unsigned h = 0;

    memset(out, 0, sizeof(*out));
    if (lodepng_decode32(&rgba, &w, &h, data, len) != 0 || rgba == NULL) {
        if (rgba != NULL) {
            lv_free(rgba);
        }
        return false;
    }
    out->pixels = imgdec_scale_rgba(rgba, (int)w, (int)h, dst_w, dst_h);
    lv_free(rgba);
    if (out->pixels == NULL) {
        return false;
    }
    out->width = dst_w;
    out->height = dst_h;
    return true;
}

/* ------------------------------------------------------------------------- */
/* JPEG (TJpgDec)                                                            */
/* ------------------------------------------------------------------------- */

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
} imgdec_jpeg_src_t;

typedef struct {
    uint8_t *pixels;    /* RGB888, native size */
    int width;
    int height;
} imgdec_jpeg_dst_t;

typedef struct {
    imgdec_jpeg_src_t src;
    imgdec_jpeg_dst_t dst;
} imgdec_jpeg_ctx_t;

/** TJpgDec input: read from the in-memory buffer (or seek when @p buff is NULL). */
static size_t imgdec_jpeg_infunc(JDEC *jd, uint8_t *buff, size_t ndata)
{
    imgdec_jpeg_ctx_t *ctx = (imgdec_jpeg_ctx_t *)jd->device;

    if (ctx == NULL) {
        return 0;
    }
    if (buff != NULL) {
        size_t remaining = (ctx->src.pos < ctx->src.len) ? ctx->src.len - ctx->src.pos : 0;
        size_t n = (ndata < remaining) ? ndata : remaining;

        if (n > 0) {
            memcpy(buff, ctx->src.data + ctx->src.pos, n);
            ctx->src.pos += n;
        }
        return n;
    }
    {
        size_t newpos = ctx->src.pos + ndata;

        if (newpos > ctx->src.len) {
            return 0;
        }
        ctx->src.pos = newpos;
        return ndata;
    }
}

/** TJpgDec output: copy each decoded RGB888 block into the native buffer. */
static int imgdec_jpeg_outfunc(JDEC *jd, void *bitmap, JRECT *rect)
{
    imgdec_jpeg_ctx_t *ctx = (imgdec_jpeg_ctx_t *)jd->device;
    const uint8_t *src = (const uint8_t *)bitmap;
    int block_w;
    int block_h;
    int y;
    int x;

    if (ctx == NULL || ctx->dst.pixels == NULL || rect == NULL) {
        return 0;
    }
    block_w = (int)rect->right - (int)rect->left + 1;
    block_h = (int)rect->bottom - (int)rect->top + 1;
    if (block_w <= 0 || block_h <= 0) {
        return 1;                       /* nothing to draw, keep decoding */
    }
    /* TJpgDec normally clips to the frame size, but its output callback is the
     * last place before a write: clamp both axes here so a malformed stream or
     * a library edge case can never run past the RGB888 buffer. */
    if ((int)rect->left >= ctx->dst.width || (int)rect->top >= ctx->dst.height) {
        return 1;
    }
    if ((int)rect->left + block_w > ctx->dst.width) {
        block_w = ctx->dst.width - (int)rect->left;
    }
    if ((int)rect->top + block_h > ctx->dst.height) {
        block_h = ctx->dst.height - (int)rect->top;
    }
    if (block_w <= 0 || block_h <= 0) {
        return 1;
    }
    for (y = 0; y < block_h; y++) {
        int row = (int)rect->top + y;
        uint8_t *dst_row;
        const uint8_t *src_row;

        dst_row = ctx->dst.pixels +
                  ((size_t)row * (size_t)ctx->dst.width + (size_t)rect->left) * 3u;
        src_row = src + (size_t)y * (size_t)block_w * 3u;
        for (x = 0; x < block_w; x++) {
            dst_row[(size_t)x * 3u + 0] = src_row[(size_t)x * 3u + 0];
            dst_row[(size_t)x * 3u + 1] = src_row[(size_t)x * 3u + 1];
            dst_row[(size_t)x * 3u + 2] = src_row[(size_t)x * 3u + 2];
        }
    }
    return 1;   /* continue decoding */
}

static bool imgdec_jpeg_decode(const uint8_t *data, size_t len,
                               int dst_w, int dst_h, imgdec_image_t *out)
{
    imgdec_jpeg_ctx_t ctx;
    uint8_t *pool = NULL;
    JDEC jd;
    JRESULT rc;

    memset(out, 0, sizeof(*out));
    memset(&ctx, 0, sizeof(ctx));
    ctx.src.data = data;
    ctx.src.len = len;
    ctx.src.pos = 0;

    pool = p4heap_alloc_psram(IMGDEC_JPEG_POOL_BYTES);
    if (pool == NULL) {
        return false;
    }
    /* jd_prepare parses the header (giving the native size) and initialises
     * the decoder state; jd_decomp then continues reading from the same
     * in-memory source. */
    rc = jd_prepare(&jd, imgdec_jpeg_infunc, pool, IMGDEC_JPEG_POOL_BYTES, &ctx);
    if (rc != JDR_OK) {
        heap_caps_free(pool);
        return false;
    }
    ctx.dst.width = (int)jd.width;
    ctx.dst.height = (int)jd.height;
    ctx.dst.pixels = p4heap_alloc_psram((size_t)ctx.dst.width * (size_t)jd.height * 3u);
    if (ctx.dst.pixels == NULL) {
        heap_caps_free(pool);
        return false;
    }
    rc = jd_decomp(&jd, imgdec_jpeg_outfunc, 0);
    if (rc == JDR_OK) {
        out->pixels = imgdec_scale_rgb888(ctx.dst.pixels, ctx.dst.width,
                                          (int)jd.height, dst_w, dst_h);
        if (out->pixels != NULL) {
            out->width = dst_w;
            out->height = dst_h;
        }
    }
    heap_caps_free(ctx.dst.pixels);
    heap_caps_free(pool);
    return (out->pixels != NULL);
}

bool imgdec_decode_scaled(const uint8_t *data, size_t len,
                          int dst_w, int dst_h, imgdec_image_t *out)
{
    imgdec_format_t fmt;

    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (data == NULL || len == 0 || dst_w <= 0 || dst_h <= 0) {
        return false;
    }
    fmt = imgdec_probe(data, len, NULL, NULL);
    if (fmt == IMGDEC_FMT_PNG) {
        return imgdec_png_decode(data, len, dst_w, dst_h, out);
    }
    if (fmt == IMGDEC_FMT_JPEG) {
        return imgdec_jpeg_decode(data, len, dst_w, dst_h, out);
    }
    return false;
}

void imgdec_free(imgdec_image_t *img)
{
    if (img == NULL) {
        return;
    }
    if (img->pixels != NULL) {
        heap_caps_free(img->pixels);
    }
    img->pixels = NULL;
    img->width = 0;
    img->height = 0;
}
