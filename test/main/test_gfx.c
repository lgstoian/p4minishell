/**
 * @file test_gfx.c
 * @brief Unit tests for the headless raster core (components/gfx/gfx.c).
 *
 * Covers RGB565 conversion, alloc bounds, clipping, and line/rect/circle
 * geometry on small heap surfaces. Display glue (lv_canvas in
 * gfx_commands.c) stays hardware-verified via screenshots.
 */

#include "unity.h"
#include "gfx.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

void test_gfx_rgb_to_565(void)
{
    TEST_ASSERT_EQUAL_UINT16(0x0000, gfx_rgb_to_565(0x000000));
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_rgb_to_565(0xFFFFFF));
    TEST_ASSERT_EQUAL_UINT16(0xF800, gfx_rgb_to_565(0xFF0000));
    TEST_ASSERT_EQUAL_UINT16(0x07E0, gfx_rgb_to_565(0x00FF00));
    TEST_ASSERT_EQUAL_UINT16(0x001F, gfx_rgb_to_565(0x0000FF));
    /* Low bits truncate (565 precision). */
    TEST_ASSERT_EQUAL_UINT16(0x0000, gfx_rgb_to_565(0x070307));
}

void test_gfx_alloc_bounds(void)
{
    gfx_surface_t s = {NULL, 0, 0};

    TEST_ASSERT_FALSE(gfx_surface_alloc(NULL, 10, 10));
    TEST_ASSERT_FALSE(gfx_surface_alloc(&s, 0, 10));
    TEST_ASSERT_FALSE(gfx_surface_alloc(&s, 10, 0));
    TEST_ASSERT_FALSE(gfx_surface_alloc(&s, GFX_MAX_W + 1, 10));
    TEST_ASSERT_FALSE(gfx_surface_alloc(&s, 10, GFX_MAX_H + 1));
    TEST_ASSERT_TRUE(gfx_surface_alloc(&s, 10, 8));
    TEST_ASSERT_EQUAL_INT(10, s.w);
    TEST_ASSERT_EQUAL_INT(8, s.h);
    TEST_ASSERT_NOT_NULL(s.px);
    gfx_surface_free(&s);
    TEST_ASSERT_NULL(s.px);
    TEST_ASSERT_EQUAL_INT(0, s.w);
    gfx_surface_free(NULL); /* NULL-safe */
}

void test_gfx_pixel_clip(void)
{
    gfx_surface_t s = {NULL, 0, 0};

    TEST_ASSERT_TRUE(gfx_surface_alloc(&s, 4, 4));
    gfx_surface_clear(&s, 0x0000);
    gfx_surface_pixel(&s, 1, 2, 0xFFFF);
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 1, 2));
    TEST_ASSERT_EQUAL_UINT16(0x0000, gfx_surface_get(&s, 0, 0));
    /* Out-of-bounds writes are ignored, reads return 0. */
    gfx_surface_pixel(&s, -1, 0, 0xFFFF);
    gfx_surface_pixel(&s, 4, 0, 0xFFFF);
    gfx_surface_pixel(&s, 0, -1, 0xFFFF);
    gfx_surface_pixel(&s, 0, 4, 0xFFFF);
    TEST_ASSERT_EQUAL_UINT16(0, gfx_surface_get(&s, -1, 0));
    TEST_ASSERT_EQUAL_UINT16(0, gfx_surface_get(&s, 4, 4));
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 1, 2));
    gfx_surface_pixel(NULL, 0, 0, 0xFFFF); /* NULL-safe */
    TEST_ASSERT_EQUAL_UINT16(0, gfx_surface_get(NULL, 0, 0));
    gfx_surface_free(&s);
}

void test_gfx_line_endpoints(void)
{
    gfx_surface_t s = {NULL, 0, 0};

    TEST_ASSERT_TRUE(gfx_surface_alloc(&s, 8, 8));
    gfx_surface_clear(&s, 0x0000);
    gfx_surface_line(&s, 0, 0, 7, 7, 0xFFFF);
    /* Diagonal endpoints land; a midpoint lands (Bresenham). */
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 0, 0));
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 7, 7));
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 3, 3));
    /* Horizontal + vertical. */
    gfx_surface_clear(&s, 0x0000);
    gfx_surface_line(&s, 1, 5, 6, 5, 0xFFFF);
    gfx_surface_line(&s, 2, 1, 2, 6, 0xFFFF);
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 1, 5));
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 6, 5));
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 2, 1));
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 2, 6));
    TEST_ASSERT_EQUAL_UINT16(0x0000, gfx_surface_get(&s, 0, 0));
    gfx_surface_free(&s);
}

void test_gfx_rect_fill_and_border(void)
{
    gfx_surface_t s = {NULL, 0, 0};

    TEST_ASSERT_TRUE(gfx_surface_alloc(&s, 8, 8));
    /* Outline: corners set, interior untouched. */
    gfx_surface_clear(&s, 0x0000);
    gfx_surface_rect(&s, 1, 1, 4, 3, 0xFFFF, false);
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 1, 1));
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 4, 3));
    TEST_ASSERT_EQUAL_UINT16(0x0000, gfx_surface_get(&s, 2, 2));
    /* Fill: interior set. */
    gfx_surface_clear(&s, 0x0000);
    gfx_surface_rect(&s, 1, 1, 4, 3, 0xFFFF, true);
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 2, 2));
    TEST_ASSERT_EQUAL_UINT16(0x0000, gfx_surface_get(&s, 0, 0));
    /* Degenerate sizes are ignored. */
    gfx_surface_rect(&s, 0, 0, 0, 5, 0xFFFF, true);
    gfx_surface_rect(&s, 0, 0, 5, 0, 0xFFFF, true);
    TEST_ASSERT_EQUAL_UINT16(0x0000, gfx_surface_get(&s, 0, 0));
    gfx_surface_free(&s);
}

void test_gfx_circle_outline_and_fill(void)
{
    gfx_surface_t s = {NULL, 0, 0};

    TEST_ASSERT_TRUE(gfx_surface_alloc(&s, 16, 16));
    /* Outline r=3: cardinal points land, center stays clear. */
    gfx_surface_clear(&s, 0x0000);
    gfx_surface_circle(&s, 8, 8, 3, 0xFFFF, false);
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 11, 8));
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 5, 8));
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 8, 11));
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 8, 5));
    TEST_ASSERT_EQUAL_UINT16(0x0000, gfx_surface_get(&s, 8, 8));
    /* Fill: center painted. */
    gfx_surface_clear(&s, 0x0000);
    gfx_surface_circle(&s, 8, 8, 3, 0xFFFF, true);
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 8, 8));
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 8, 9));
    /* r=0 plots one pixel; negative is ignored. */
    gfx_surface_clear(&s, 0x0000);
    gfx_surface_circle(&s, 3, 3, 0, 0xFFFF, false);
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 3, 3));
    gfx_surface_circle(&s, 3, 3, -2, 0xFFFF, true);
    TEST_ASSERT_EQUAL_UINT16(0x0000, gfx_surface_get(&s, 4, 4));
    gfx_surface_free(&s);
}

/** Build a minimal 24-bit BI_RGB BMP in a stack buffer (header + rows).
 * w*h<=64 cells; rows padded to 4 bytes. Returns total bytes. */
static size_t test_bmp_build(uint8_t *buf, int w, int h, const uint8_t *bgr_top_down)
{
    uint32_t stride = ((uint32_t)w * 3u + 3u) & ~3u;
    uint32_t img = stride * (uint32_t)h;
    uint32_t total = 54 + img;
    int y;

    memset(buf, 0, total);
    buf[0] = 'B';
    buf[1] = 'M';
    buf[2] = (uint8_t)total;
    buf[3] = (uint8_t)(total >> 8);
    buf[10] = 54;
    buf[14] = 40;
    buf[18] = (uint8_t)w;
    buf[22] = (uint8_t)h;
    buf[26] = 1;
    buf[28] = 24;
    buf[34] = (uint8_t)img;
    buf[35] = (uint8_t)(img >> 8);
    /* Pixels bottom-up: file row 0 = caller's last row. */
    for (y = 0; y < h; y++) {
        memcpy(buf + 54 + (uint32_t)y * stride,
               bgr_top_down + (uint32_t)(h - 1 - y) * (uint32_t)w * 3u,
               (uint32_t)w * 3u);
    }
    return total;
}

void test_gfx_bmp_parse_ok(void)
{
    /* 4x2 red/green checker: BGR triples top-down. */
    uint8_t px[2 * 4 * 3] = {
        0,0,255, 0,255,0, 0,0,255, 0,255,0,
        0,255,0, 0,0,255, 0,255,0, 0,0,255,
    };
    uint8_t buf[54 + 2 * 12];
    gfx_bmp_info_t info;

    test_bmp_build(buf, 4, 2, px);
    TEST_ASSERT_TRUE(gfx_bmp_parse_header(buf, sizeof(buf), &info));
    TEST_ASSERT_EQUAL_INT(4, info.w);
    TEST_ASSERT_EQUAL_INT(2, info.h);
    TEST_ASSERT_EQUAL_UINT32(54, info.data_offset);
    TEST_ASSERT_EQUAL_UINT32(12, info.row_stride);
}

void test_gfx_bmp_parse_rejects(void)
{
    uint8_t px[4 * 3] = {0};
    uint8_t buf[54 + 4];
    uint8_t tmp[54 + 4];
    gfx_bmp_info_t info;

    test_bmp_build(buf, 1, 1, px);
    /* Truncated header. */
    TEST_ASSERT_FALSE(gfx_bmp_parse_header(buf, 20, &info));
    TEST_ASSERT_FALSE(gfx_bmp_parse_header(NULL, sizeof(buf), &info));
    TEST_ASSERT_FALSE(gfx_bmp_parse_header(buf, sizeof(buf), NULL));
    /* Bad signature. */
    memcpy(tmp, buf, sizeof(tmp));
    tmp[0] = 'X';
    TEST_ASSERT_FALSE(gfx_bmp_parse_header(tmp, sizeof(tmp), &info));
    /* 32 bpp. */
    memcpy(tmp, buf, sizeof(tmp));
    tmp[28] = 32;
    TEST_ASSERT_FALSE(gfx_bmp_parse_header(tmp, sizeof(tmp), &info));
    /* RLE compression. */
    memcpy(tmp, buf, sizeof(tmp));
    tmp[30] = 1;
    TEST_ASSERT_FALSE(gfx_bmp_parse_header(tmp, sizeof(tmp), &info));
    /* Negative height (top-down). */
    memcpy(tmp, buf, sizeof(tmp));
    tmp[22] = 0xFF;
    tmp[23] = 0xFF;
    tmp[24] = 0xFF;
    tmp[25] = 0xFF;
    TEST_ASSERT_FALSE(gfx_bmp_parse_header(tmp, sizeof(tmp), &info));
    /* Zero width. */
    memcpy(tmp, buf, sizeof(tmp));
    tmp[18] = 0;
    TEST_ASSERT_FALSE(gfx_bmp_parse_header(tmp, sizeof(tmp), &info));
    /* Width over the sprite cap. */
    memcpy(tmp, buf, sizeof(tmp));
    tmp[18] = (uint8_t)(GFX_SPR_MAX + 1);
    TEST_ASSERT_FALSE(gfx_bmp_parse_header(tmp, sizeof(tmp), &info));
    /* Pixel data overruns the buffer. */
    TEST_ASSERT_FALSE(gfx_bmp_parse_header(buf, sizeof(buf) - 1, &info));
}

void test_gfx_bmp_decode_565(void)
{
    /* 2x1: red then blue (BGR triples). */
    uint8_t px[2 * 3] = {0, 0, 255, 255, 0, 0};
    uint8_t buf[54 + 8];
    gfx_bmp_info_t info;
    gfx_surface_t s = {NULL, 0, 0};
    size_t n;

    n = test_bmp_build(buf, 2, 1, px);
    TEST_ASSERT_TRUE(gfx_bmp_parse_header(buf, n, &info));
    TEST_ASSERT_TRUE(gfx_bmp_decode_565(buf, n, &info, &s));
    TEST_ASSERT_EQUAL_INT(2, s.w);
    TEST_ASSERT_EQUAL_INT(1, s.h);
    TEST_ASSERT_EQUAL_UINT16(0xF800, gfx_surface_get(&s, 0, 0));
    TEST_ASSERT_EQUAL_UINT16(0x001F, gfx_surface_get(&s, 1, 0));
    gfx_surface_free(&s);
    /* NULL-safe. */
    TEST_ASSERT_FALSE(gfx_bmp_decode_565(NULL, n, &info, &s));
    TEST_ASSERT_FALSE(gfx_bmp_decode_565(buf, n, NULL, &s));
    TEST_ASSERT_FALSE(gfx_bmp_decode_565(buf, n, &info, NULL));
}

void test_gfx_blit_clip_transparent(void)
{
    gfx_surface_t dst = {NULL, 0, 0};
    gfx_surface_t src = {NULL, 0, 0};

    TEST_ASSERT_TRUE(gfx_surface_alloc(&dst, 4, 4));
    TEST_ASSERT_TRUE(gfx_surface_alloc(&src, 2, 2));
    gfx_surface_clear(&dst, 0x0000);
    gfx_surface_pixel(&src, 0, 0, 0xF800);
    gfx_surface_pixel(&src, 1, 0, 0x07E0);
    gfx_surface_pixel(&src, 0, 1, 0x001F);
    gfx_surface_pixel(&src, 1, 1, 0xFFFF);
    /* Plain blit at (1,1). */
    gfx_surface_blit(&dst, &src, 1, 1, false, 0x0000);
    TEST_ASSERT_EQUAL_UINT16(0xF800, gfx_surface_get(&dst, 1, 1));
    TEST_ASSERT_EQUAL_UINT16(0x07E0, gfx_surface_get(&dst, 2, 1));
    TEST_ASSERT_EQUAL_UINT16(0x001F, gfx_surface_get(&dst, 1, 2));
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&dst, 2, 2));
    TEST_ASSERT_EQUAL_UINT16(0x0000, gfx_surface_get(&dst, 0, 0));
    /* Clipped blit at (-1,-1): only src(1,1) lands on dst(0,0). */
    gfx_surface_clear(&dst, 0x0000);
    gfx_surface_blit(&dst, &src, -1, -1, false, 0x0000);
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&dst, 0, 0));
    TEST_ASSERT_EQUAL_UINT16(0x0000, gfx_surface_get(&dst, 1, 0));
    /* Transparent: white skipped, rest lands. */
    gfx_surface_clear(&dst, 0x1234);
    gfx_surface_blit(&dst, &src, 0, 0, true, 0xFFFF);
    TEST_ASSERT_EQUAL_UINT16(0xF800, gfx_surface_get(&dst, 0, 0));
    TEST_ASSERT_EQUAL_UINT16(0x1234, gfx_surface_get(&dst, 1, 1));
    /* NULL-safe. */
    gfx_surface_blit(NULL, &src, 0, 0, false, 0);
    gfx_surface_blit(&dst, NULL, 0, 0, false, 0);
    gfx_surface_free(&dst);
    gfx_surface_free(&src);
}

void test_gfx_565_to_888_row(void)
{
    uint16_t src[3] = {0xF800, 0x07E0, 0x001F};
    uint8_t dst[9] = {0};

    gfx_565_to_888_row(dst, src, 3);
    /* BGR triples: red -> {0,0,248}, green -> {0,252,0}, blue -> {248,0,0}. */
    TEST_ASSERT_EQUAL_UINT8(0, dst[0]);
    TEST_ASSERT_EQUAL_UINT8(0, dst[1]);
    TEST_ASSERT_EQUAL_UINT8(248, dst[2]);
    TEST_ASSERT_EQUAL_UINT8(0, dst[3]);
    TEST_ASSERT_EQUAL_UINT8(252, dst[4]);
    TEST_ASSERT_EQUAL_UINT8(0, dst[5]);
    TEST_ASSERT_EQUAL_UINT8(248, dst[6]);
    TEST_ASSERT_EQUAL_UINT8(0, dst[7]);
    TEST_ASSERT_EQUAL_UINT8(0, dst[8]);
    gfx_565_to_888_row(NULL, src, 3); /* NULL-safe */
    gfx_565_to_888_row(dst, NULL, 3);
}
