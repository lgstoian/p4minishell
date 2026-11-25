/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_imgdec.c
 * @brief Unit tests for the PNG/JPEG/BMP format probe (components/imgdec).
 *
 * `imgdec_probe` is a pure magic-byte sniff (no decode, no LVGL), so these
 * tests construct the smallest valid headers and assert the detected format
 * and native dimensions.
 */

#include "unity.h"
#include "imgdec.h"
#include "imagefile.h"
#include "imagefile_cells.h" /* the ONE sampler adapter, header-only */
#include "p4minishell_config.h"
#include <string.h>

static void imgdec_case_fit_decode(void);
static void imgdec_case_cell_sampler(void);

/** Minimal PNG: 8-byte signature + IHDR with width at [16..19], height at [20..23]. */
static void imgdec_case_png(void)
{
    uint8_t png[24] = {0};
    int w = 0;
    int h = 0;

    static const uint8_t sig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

    memcpy(png, sig, sizeof(sig));
    /* width = 0x00000100 (256), height = 0x00000080 (128), big-endian */
    png[16] = 0x00; png[17] = 0x00; png[18] = 0x01; png[19] = 0x00;
    png[20] = 0x00; png[21] = 0x00; png[22] = 0x00; png[23] = 0x80;

    TEST_ASSERT_EQUAL_INT(IMGDEC_FMT_PNG, imgdec_probe(png, sizeof(png), &w, &h));
    TEST_ASSERT_EQUAL_INT(256, w);
    TEST_ASSERT_EQUAL_INT(128, h);

    /* A truncated header is not a PNG. */
    TEST_ASSERT_EQUAL_INT(IMGDEC_FMT_UNKNOWN, imgdec_probe(png, 16, NULL, NULL));
    /* A wrong signature is not a PNG. */
    png[0] = 0x00;
    TEST_ASSERT_EQUAL_INT(IMGDEC_FMT_UNKNOWN, imgdec_probe(png, sizeof(png), NULL, NULL));
}

/** Minimal JPEG: SOI + SOF0 frame header carrying the native dimensions. */
static void imgdec_case_jpeg(void)
{
    uint8_t jpg[11] = {0};
    int w = 0;
    int h = 0;

    jpg[0] = 0xFF; jpg[1] = 0xD8;         /* SOI */
    jpg[2] = 0xFF; jpg[3] = 0xC0;         /* SOF0 */
    jpg[4] = 0x00; jpg[5] = 0x11;         /* segment length */
    jpg[6] = 0x08;                        /* precision */
    jpg[7] = 0x00; jpg[8] = 0x40;         /* height = 64 */
    jpg[9] = 0x00; jpg[10] = 0x20;        /* width  = 32 */

    TEST_ASSERT_EQUAL_INT(IMGDEC_FMT_JPEG, imgdec_probe(jpg, sizeof(jpg), &w, &h));
    TEST_ASSERT_EQUAL_INT(32, w);
    TEST_ASSERT_EQUAL_INT(64, h);

    /* SOI alone (no SOF) is not a JPEG. */
    TEST_ASSERT_EQUAL_INT(IMGDEC_FMT_UNKNOWN,
                          imgdec_probe(jpg, 2, NULL, NULL));
    /* A wrong first marker is not a JPEG. */
    jpg[1] = 0xE0;
    TEST_ASSERT_EQUAL_INT(IMGDEC_FMT_UNKNOWN, imgdec_probe(jpg, sizeof(jpg), NULL, NULL));
}

static void imgdec_case_bmp_unknown(void)
{
    /* A real BMP is >= 54 bytes; the probe needs at least the 4-byte guard. */
    uint8_t bmp[4] = { 'B', 'M', 0, 0 };

    TEST_ASSERT_EQUAL_INT(IMGDEC_FMT_BMP, imgdec_probe(bmp, sizeof(bmp), NULL, NULL));

    TEST_ASSERT_EQUAL_INT(IMGDEC_FMT_UNKNOWN,
                          imgdec_probe((const uint8_t *)"XXXX", 4, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(IMGDEC_FMT_UNKNOWN, imgdec_probe(NULL, 0, NULL, NULL));
}

void test_imgdec_probe_all(void)
{
    imgdec_case_png();
    imgdec_case_jpeg();
    imgdec_case_bmp_unknown();
    imgdec_case_fit_decode();
    imgdec_case_cell_sampler();
}

/** Probe rejections: the documented native-size cap and malformed headers. */
void test_imgdec_probe_rejects_out_of_range(void)
{
    uint8_t png[24] = {0};
    uint8_t jpg[11] = {0};
    static const uint8_t sig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    unsigned too_wide = (unsigned)P4_CONFIG_IMGDEC_MAX_W + 1u;

    memcpy(png, sig, sizeof(sig));
    png[16] = (uint8_t)(too_wide >> 24);
    png[17] = (uint8_t)(too_wide >> 16);
    png[18] = (uint8_t)(too_wide >> 8);
    png[19] = (uint8_t)too_wide;
    png[20] = 0x00; png[21] = 0x00; png[22] = 0x00; png[23] = 0x40;
    /* A PNG past the native decode cap is refused, not decoded into a huge
     * LVGL allocation. */
    TEST_ASSERT_EQUAL_INT(IMGDEC_FMT_UNKNOWN,
                          imgdec_probe(png, sizeof(png), NULL, NULL));

    /* Same for a JPEG whose SOF exceeds the cap. */
    jpg[0] = 0xFF; jpg[1] = 0xD8;
    jpg[2] = 0xFF; jpg[3] = 0xC0;
    jpg[4] = 0x00; jpg[5] = 0x11;
    jpg[6] = 0x08;
    jpg[7] = 0x00; jpg[8] = 0x40;
    jpg[9] = (uint8_t)((P4_CONFIG_IMGDEC_MAX_W + 1) >> 8);
    jpg[10] = (uint8_t)(P4_CONFIG_IMGDEC_MAX_W + 1);
    TEST_ASSERT_EQUAL_INT(IMGDEC_FMT_UNKNOWN,
                          imgdec_probe(jpg, sizeof(jpg), NULL, NULL));

    /* A segment length below the 2-byte length field is malformed: the walker
     * must stop instead of stepping through the buffer two bytes at a time. */
    jpg[4] = 0x00; jpg[5] = 0x00;
    TEST_ASSERT_EQUAL_INT(IMGDEC_FMT_UNKNOWN,
                          imgdec_probe(jpg, sizeof(jpg), NULL, NULL));

    /* A PNG needs the full 24-byte signature+IHDR prefix. */
    TEST_ASSERT_EQUAL_INT(IMGDEC_FMT_UNKNOWN, imgdec_probe(png, 23, NULL, NULL));
}

/** The RGB888 -> RGB565 mapping is shared by PNG, JPEG and the browser cell
 *  ramp; a wrong channel shift silently tints every decoded image. */
void test_imgdec_rgb565_conversion(void)
{
    TEST_ASSERT_EQUAL_HEX16(0x0000, imgdec_rgb888_to_565(0x000000));
    TEST_ASSERT_EQUAL_HEX16(0xFFFF, imgdec_rgb888_to_565(0xFFFFFF));
    TEST_ASSERT_EQUAL_HEX16(0xF800, imgdec_rgb888_to_565(0xFF0000)); /* red   */
    TEST_ASSERT_EQUAL_HEX16(0x07E0, imgdec_rgb888_to_565(0x00FF00)); /* green */
    TEST_ASSERT_EQUAL_HEX16(0x001F, imgdec_rgb888_to_565(0x0000FF)); /* blue  */
    TEST_ASSERT_EQUAL_HEX16(0x3333, imgdec_rgb888_to_565(0x336699));
}

/** A BMP probe must report its dimensions: the browser sizes its cell grid
 *  from them, and a 0x0 result collapses the image to a single pixel. */
void test_imgdec_probe_bmp_dims(void)
{
    uint8_t hdr[54] = {0};
    int w = 0;
    int h = 0;

    hdr[0] = 'B';
    hdr[1] = 'M';
    hdr[18] = 0x80;   /* width 128 */
    hdr[22] = 0x30;   /* height 48 */
    TEST_ASSERT_EQUAL_INT(IMGDEC_FMT_BMP, imgdec_probe(hdr, sizeof(hdr), &w, &h));
    TEST_ASSERT_EQUAL_INT(128, w);
    TEST_ASSERT_EQUAL_INT(48, h);

    /* A negative height is a top-down bitmap; the magnitude is the height. */
    hdr[22] = 0xCE;
    hdr[23] = 0xFF;
    hdr[24] = 0xFF;
    hdr[25] = 0xFF;   /* -50 (0xFFFFFFCE) */
    TEST_ASSERT_EQUAL_INT(IMGDEC_FMT_BMP, imgdec_probe(hdr, sizeof(hdr), &w, &h));
    TEST_ASSERT_EQUAL_INT(50, h);
}

/** Minimal 2x2 24-bit BMP, bottom-up rows, 4-byte padding (the sprite format). */
static size_t imgdec_bmp_2x2(uint8_t *buf, const uint8_t bgr[12])
{
    static const int stride = 8;

    memset(buf, 0, 70);
    buf[0] = 'B'; buf[1] = 'M';
    buf[2] = 70;
    buf[10] = 54;
    buf[14] = 40;
    buf[18] = 2; buf[22] = 2;
    buf[26] = 1; buf[28] = 24;
    buf[34] = 16;
    /* File row 0 is the caller's bottom row. */
    memcpy(buf + 54, bgr + 6, 6);
    memcpy(buf + 54 + stride, bgr, 6);
    return 70;
}

/** Fit-to-target decode goes through the ONE imagefile decoder; the native
 *  2x2 source is never materialised and the fitted surface is RGB565. */
static void imgdec_case_fit_decode(void)
{
    uint8_t buf[70];
    static const uint8_t red[12] = {
        0,0,255, 0,0,255, 0,0,255, 0,0,255,
    };
    gfx_surface_t out = {NULL, 0, 0};

    imgdec_bmp_2x2(buf, red);
    TEST_ASSERT_TRUE(imagefile_decode_fit(buf, sizeof(buf), 1, 1, &out));
    TEST_ASSERT_EQUAL_INT(1, out.w);
    TEST_ASSERT_EQUAL_INT(1, out.h);
    TEST_ASSERT_NOT_NULL(out.px);
    TEST_ASSERT_EQUAL_HEX16(0xF800, gfx_surface_get(&out, 0, 0));
    gfx_surface_free(&out);

    /* A source that already fits keeps native size; invalid bytes fail. */
    TEST_ASSERT_TRUE(imagefile_decode_fit(buf, sizeof(buf), 8, 8, &out));
    TEST_ASSERT_EQUAL_INT(2, out.w);
    TEST_ASSERT_EQUAL_INT(2, out.h);
    gfx_surface_free(&out);
    TEST_ASSERT_FALSE(imagefile_decode_fit((const uint8_t *)"XX", 2, 4, 4, &out));
}

/** The DOM cell adapter must reuse the ONE tui sampler and ramp, never a
 *  second nearest-neighbour loop or palette. Cell packing is
 *  (dos_fg << 4) | ramp_level, exactly dom_image_t's byte format. */
static void imgdec_case_cell_sampler(void)
{
    gfx_surface_t img = {NULL, 0, 0};
    imagefile_cell_grid_t grid;
    uint8_t cells[4] = {0, 0, 0, 0};

    TEST_ASSERT_TRUE(gfx_surface_alloc(&img, 2, 2));
    gfx_surface_clear(&img, 0x0000);
    gfx_surface_pixel(&img, 1, 0, 0xFFFF);
    gfx_surface_pixel(&img, 0, 1, 0x001F);
    grid.pixels = cells;
    grid.grid_w = 2;
    grid.grid_h = 2;
    grid.origin_col = 1;
    grid.origin_row = 1;
    imagefile_sample_rgb565_cells(1, 1, 2, 2, &img,
                                  imagefile_grid_cell_sink, &grid);
    TEST_ASSERT_EQUAL_INT(0, cells[0] & 0x0F);   /* black -> ramp level 0 */
    TEST_ASSERT_EQUAL_INT(ANSI_IMAGE_RAMP_LAST, cells[1] & 0x0F); /* white */
    TEST_ASSERT_EQUAL_INT(ANSI_IMAGE_RAMP_LAST, imagefile_ramp_level(
                              ANSI_IMAGE_RAMP[ANSI_IMAGE_RAMP_LAST]));
    /* Out-of-grid writes are clipped and a NULL sink is a no-op. */
    imagefile_sample_rgb565_cells(5, 5, 2, 2, &img,
                                  imagefile_grid_cell_sink, &grid);
    TEST_ASSERT_EQUAL_INT(0, cells[0] & 0x0F);
    imagefile_sample_rgb565_cells(1, 1, 2, 2, &img, NULL, &grid);
    TEST_ASSERT_EQUAL_INT(ANSI_IMAGE_RAMP_LAST, cells[1] & 0x0F);
    gfx_surface_free(&img);
}
