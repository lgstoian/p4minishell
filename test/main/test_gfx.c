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
#include <stdlib.h>
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

void test_gfx_hline_vline_clip(void)
{
    gfx_surface_t s = {NULL, 0, 0};

    TEST_ASSERT_TRUE(gfx_surface_alloc(&s, 8, 4));
    gfx_surface_clear(&s, 0x0000);
    gfx_surface_hline(&s, 2, 1, 4, 0xFFFF);
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 2, 1));
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 5, 1));
    TEST_ASSERT_EQUAL_UINT16(0x0000, gfx_surface_get(&s, 1, 1));
    TEST_ASSERT_EQUAL_UINT16(0x0000, gfx_surface_get(&s, 6, 1));
    /* Left/right clipping. */
    gfx_surface_clear(&s, 0x0000);
    gfx_surface_hline(&s, -2, 2, 4, 0xFFFF);   /* covers x=0,1 */
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 0, 2));
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 1, 2));
    TEST_ASSERT_EQUAL_UINT16(0x0000, gfx_surface_get(&s, 2, 2));
    gfx_surface_hline(&s, 6, 3, 5, 0xFFFF);    /* covers x=6,7 */
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 6, 3));
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 7, 3));
    /* Vertical span + no-ops. */
    gfx_surface_clear(&s, 0x0000);
    gfx_surface_vline(&s, 3, 0, 2, 0xFFFF);
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 3, 0));
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 3, 1));
    TEST_ASSERT_EQUAL_UINT16(0x0000, gfx_surface_get(&s, 3, 2));
    gfx_surface_hline(&s, 0, 0, 0, 0xFFFF);
    gfx_surface_hline(&s, 0, 99, 4, 0xFFFF);
    gfx_surface_vline(&s, 0, 0, -1, 0xFFFF);
    TEST_ASSERT_EQUAL_UINT16(0x0000, gfx_surface_get(&s, 0, 0));
    gfx_surface_hline(NULL, 0, 0, 4, 0xFFFF); /* NULL-safe */
    gfx_surface_free(&s);
}

void test_gfx_triangle_fill_outline(void)
{
    gfx_surface_t s = {NULL, 0, 0};

    TEST_ASSERT_TRUE(gfx_surface_alloc(&s, 16, 16));
    /* Right triangle (0,0)-(15,0)-(0,15): hypotenuse is x+y=15. */
    gfx_surface_clear(&s, 0x0000);
    gfx_surface_triangle(&s, 0, 0, 15, 0, 0, 15, 0xFFFF, false);
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 0, 0));
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 15, 0));
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 0, 15));
    TEST_ASSERT_EQUAL_UINT16(0x0000, gfx_surface_get(&s, 3, 3));   /* interior */
    TEST_ASSERT_EQUAL_UINT16(0x0000, gfx_surface_get(&s, 12, 12)); /* outside */
    /* Filled: interior paints, outside stays clear. */
    gfx_surface_clear(&s, 0x0000);
    gfx_surface_triangle(&s, 0, 0, 15, 0, 0, 15, 0xFFFF, true);
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 0, 0));
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 3, 3));
    TEST_ASSERT_EQUAL_UINT16(0x0000, gfx_surface_get(&s, 12, 12));
    /* Degenerate (collinear) falls back to the outline. */
    gfx_surface_clear(&s, 0x0000);
    gfx_surface_triangle(&s, 0, 0, 10, 10, 20, 20, 0xFFFF, true);
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 5, 5));
    gfx_surface_free(&s);
}

void test_gfx_polygon_fill_outline(void)
{
    gfx_surface_t s = {NULL, 0, 0};
    /* Concave "U": the notch center (5,6) must stay empty. */
    const int u[16] = {0, 0, 10, 0, 10, 10, 7, 10, 7, 3, 3, 3, 3, 10, 0, 10};

    TEST_ASSERT_TRUE(gfx_surface_alloc(&s, 12, 12));
    gfx_surface_clear(&s, 0x0000);
    gfx_surface_polygon(&s, u, 8, 0xFFFF, true);
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 1, 1));  /* top bar */
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 5, 1));  /* top bar */
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 1, 6));  /* left arm */
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 8, 6));  /* right arm */
    TEST_ASSERT_EQUAL_UINT16(0x0000, gfx_surface_get(&s, 5, 6));  /* notch */
    /* Outline mode: interior empty, edge vertex set. */
    gfx_surface_clear(&s, 0x0000);
    gfx_surface_polygon(&s, u, 8, 0xFFFF, false);
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 0, 0));
    TEST_ASSERT_EQUAL_UINT16(0x0000, gfx_surface_get(&s, 1, 1));
    /* NULL-safe. */
    gfx_surface_polygon(&s, NULL, 8, 0xFFFF, true);
    gfx_surface_polygon(NULL, u, 8, 0xFFFF, true);
    gfx_surface_free(&s);
}

void test_gfx_ellipse(void)
{
    gfx_surface_t s = {NULL, 0, 0};

    TEST_ASSERT_TRUE(gfx_surface_alloc(&s, 21, 21));
    gfx_surface_clear(&s, 0x0000);
    gfx_surface_ellipse(&s, 10, 10, 8, 4, 0xFFFF, true);
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 10, 10)); /* center */
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 18, 10)); /* +rx */
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 10, 14)); /* +ry */
    TEST_ASSERT_EQUAL_UINT16(0x0000, gfx_surface_get(&s, 18, 14)); /* corner */
    /* Outline: center empty, cardinal set. */
    gfx_surface_clear(&s, 0x0000);
    gfx_surface_ellipse(&s, 10, 10, 8, 4, 0xFFFF, false);
    TEST_ASSERT_EQUAL_UINT16(0x0000, gfx_surface_get(&s, 10, 10));
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 18, 10));
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 10, 14));
    /* Degenerate radii. */
    gfx_surface_clear(&s, 0x0000);
    gfx_surface_ellipse(&s, 5, 5, 0, 3, 0xFFFF, false);  /* vertical line */
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 5, 2));
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 5, 8));
    gfx_surface_clear(&s, 0x0000);
    gfx_surface_ellipse(&s, 5, 5, 3, 0, 0xFFFF, false);  /* horizontal line */
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 2, 5));
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 8, 5));
    gfx_surface_clear(&s, 0x0000);
    gfx_surface_ellipse(&s, 5, 5, 0, 0, 0xFFFF, false);  /* single pixel */
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 5, 5));
    gfx_surface_free(&s);
}

void test_gfx_flood_fill(void)
{
    gfx_surface_t s = {NULL, 0, 0};
    int n;

    TEST_ASSERT_TRUE(gfx_surface_alloc(&s, 8, 8));
    gfx_surface_clear(&s, 0x0000);
    gfx_surface_rect(&s, 1, 1, 6, 6, 0xF800, false);  /* hollow box */
    /* Interior is 4x4 = 16 pixels. */
    n = gfx_surface_flood_fill(&s, 3, 3, 0xFFFF);
    TEST_ASSERT_EQUAL_INT(16, n);
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 2, 2));
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 5, 5));
    TEST_ASSERT_EQUAL_UINT16(0xF800, gfx_surface_get(&s, 1, 1));  /* border held */
    TEST_ASSERT_EQUAL_UINT16(0x0000, gfx_surface_get(&s, 0, 0));  /* outside held */
    /* Outer region is 64 - 36 = 28 pixels; the border keeps it out. */
    n = gfx_surface_flood_fill(&s, 0, 0, 0x07E0);
    TEST_ASSERT_EQUAL_INT(28, n);
    TEST_ASSERT_EQUAL_UINT16(0x07E0, gfx_surface_get(&s, 7, 7));
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 3, 3));  /* interior held */
    /* No-op on the target color and out-of-bounds / NULL. */
    TEST_ASSERT_EQUAL_INT(0, gfx_surface_flood_fill(&s, 0, 0, 0x07E0));
    TEST_ASSERT_EQUAL_INT(0, gfx_surface_flood_fill(&s, -1, 0, 0xFFFF));
    TEST_ASSERT_EQUAL_INT(0, gfx_surface_flood_fill(NULL, 0, 0, 0xFFFF));
    gfx_surface_free(&s);
}

void test_gfx_font_table(void)
{
    /* Glyph index = ch - 0x20; space is blank, 'A' matches the generator. */
    const uint8_t *space = gfx_font8x8[' ' - GFX_FONT_FIRST];
    const uint8_t *A = gfx_font8x8['A' - GFX_FONT_FIRST];

    TEST_ASSERT_EQUAL_INT(8, GFX_FONT_W);
    TEST_ASSERT_EQUAL_INT(8, GFX_FONT_H);
    TEST_ASSERT_EQUAL_INT(0x5F, GFX_FONT_GLYPHS);
    for (int i = 0; i < GFX_FONT_H; i++) {
        TEST_ASSERT_EQUAL_UINT8(0, space[i]);
    }
    TEST_ASSERT_EQUAL_UINT8(0x18, A[0]);
    TEST_ASSERT_EQUAL_UINT8(0x3C, A[1]);
    TEST_ASSERT_EQUAL_UINT8(0x7E, A[4]);
}

void test_gfx_text_render(void)
{
    gfx_surface_t s = {NULL, 0, 0};

    TEST_ASSERT_TRUE(gfx_surface_alloc(&s, 32, 16));
    /* Width helper. */
    TEST_ASSERT_EQUAL_INT(16, gfx_text_width("AB", 1));
    TEST_ASSERT_EQUAL_INT(32, gfx_text_width("AB", 2));
    TEST_ASSERT_EQUAL_INT(0, gfx_text_width("AB", 0) - 16); /* scale<1 -> 1 */
    TEST_ASSERT_EQUAL_INT(0, gfx_text_width(NULL, 1));
    /* Scale 1: 'A' row 0 sets glyph columns 3..4 -> x=3,4. */
    gfx_surface_clear(&s, 0x0000);
    TEST_ASSERT_EQUAL_INT(8, gfx_surface_text(&s, 0, 0, "A", 0xFFFF, 0, false, 1));
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 3, 0));
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 4, 0));
    TEST_ASSERT_EQUAL_UINT16(0x0000, gfx_surface_get(&s, 0, 0));
    /* Scale 2: each glyph pixel becomes a 2x2 block. */
    gfx_surface_clear(&s, 0x0000);
    TEST_ASSERT_EQUAL_INT(16, gfx_surface_text(&s, 0, 0, "A", 0xFFFF, 0, false, 2));
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 6, 0));
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 7, 0));
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 8, 1));
    /* Background fill covers the glyph cell; transparent leaves it. */
    gfx_surface_clear(&s, 0x0000);
    gfx_surface_text(&s, 0, 0, " ", 0xFFFF, 0x001F, true, 1);
    TEST_ASSERT_EQUAL_UINT16(0x001F, gfx_surface_get(&s, 0, 0));
    gfx_surface_clear(&s, 0x0000);
    gfx_surface_text(&s, 0, 0, " ", 0xFFFF, 0x001F, false, 1);
    TEST_ASSERT_EQUAL_UINT16(0x0000, gfx_surface_get(&s, 0, 0));
    /* Newline returns to the start column and advances a row. */
    gfx_surface_clear(&s, 0x0000);
    gfx_surface_text(&s, 2, 2, "A\nA", 0xFFFF, 0, false, 1);
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 5, 2));
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 5, 10));
    gfx_surface_text(NULL, 0, 0, "A", 0xFFFF, 0, false, 1); /* NULL-safe */
    gfx_surface_text(&s, 0, 0, NULL, 0xFFFF, 0, false, 1);
    gfx_surface_free(&s);
}

void test_gfx_view_map(void)
{
    gfx_view_t v;
    int sx;
    int sy;

    /* World -10..10 over a 0-based 100x100 raster: edges land on borders. */
    gfx_view_set(&v, -10.0, 10.0, -10.0, 10.0, 0, 0, 100, 100);
    TEST_ASSERT_TRUE(gfx_view_map(&v, 0.0, 0.0, &sx, &sy));
    TEST_ASSERT_EQUAL_INT(50, sx);
    TEST_ASSERT_EQUAL_INT(49, sy);
    TEST_ASSERT_TRUE(gfx_view_map(&v, -10.0, 10.0, &sx, &sy));
    TEST_ASSERT_EQUAL_INT(0, sx);
    TEST_ASSERT_EQUAL_INT(0, sy);
    TEST_ASSERT_TRUE(gfx_view_map(&v, 10.0, -10.0, &sx, &sy));
    TEST_ASSERT_EQUAL_INT(99, sx);
    TEST_ASSERT_EQUAL_INT(99, sy);
    /* Just outside rounds outside. */
    TEST_ASSERT_FALSE(gfx_view_map(&v, 11.0, 0.0, &sx, &sy));
    TEST_ASSERT_FALSE(gfx_view_map(&v, 0.0, -11.0, &sx, &sy));
    /* TUI 1-based rect: (0,0) lands mid-grid and in range. */
    gfx_view_set(&v, -10.0, 10.0, -10.0, 10.0, 1, 1, 80, 25);
    TEST_ASSERT_TRUE(gfx_view_map(&v, 0.0, 0.0, &sx, &sy));
    TEST_ASSERT_EQUAL_INT(41, sx);
    TEST_ASSERT_EQUAL_INT(13, sy);
    /* Degenerate windows, non-finite input, NULLs. */
    gfx_view_set(&v, 5.0, 5.0, -10.0, 10.0, 0, 0, 100, 100);
    TEST_ASSERT_FALSE(gfx_view_map(&v, 0.0, 0.0, &sx, &sy));
    gfx_view_set(&v, -10.0, 10.0, -10.0, 10.0, 0, 0, 1, 100);
    TEST_ASSERT_FALSE(gfx_view_map(&v, 0.0, 0.0, &sx, &sy));
    gfx_view_set(&v, -10.0, 10.0, -10.0, 10.0, 0, 0, 100, 100);
    TEST_ASSERT_FALSE(gfx_view_map(&v, strtod("nan", NULL), 0.0, &sx, &sy));
    TEST_ASSERT_FALSE(gfx_view_map(NULL, 0.0, 0.0, &sx, &sy));
    TEST_ASSERT_FALSE(gfx_view_map(&v, 0.0, 0.0, NULL, &sy));
    gfx_view_set(NULL, 0.0, 1.0, 0.0, 1.0, 0, 0, 10, 10); /* NULL-safe */
}

void test_gfx_view_clip_line(void)
{
    gfx_view_t v;
    int ax;
    int ay;
    int bx;
    int by;

    gfx_view_set(&v, -10.0, 10.0, -10.0, 10.0, 0, 0, 100, 100);
    /* Fully inside: exact clipped endpoints. */
    TEST_ASSERT_TRUE(gfx_view_clip_line(&v, -5.0, -5.0, 5.0, 5.0, &ax, &ay, &bx, &by));
    TEST_ASSERT_EQUAL_INT(25, ax);
    TEST_ASSERT_EQUAL_INT(74, ay);
    TEST_ASSERT_EQUAL_INT(74, bx);
    TEST_ASSERT_EQUAL_INT(25, by);
    /* Crossing the left edge clips to column 0. */
    TEST_ASSERT_TRUE(gfx_view_clip_line(&v, -20.0, 0.0, 0.0, 0.0, &ax, &ay, &bx, &by));
    TEST_ASSERT_EQUAL_INT(0, ax);
    TEST_ASSERT_EQUAL_INT(49, ay);
    TEST_ASSERT_EQUAL_INT(50, bx);
    TEST_ASSERT_EQUAL_INT(49, by);
    /* Vertical span stays in column 50, rows 0..99. */
    TEST_ASSERT_TRUE(gfx_view_clip_line(&v, 0.0, -20.0, 0.0, 20.0, &ax, &ay, &bx, &by));
    TEST_ASSERT_EQUAL_INT(50, ax);
    TEST_ASSERT_EQUAL_INT(99, ay);
    TEST_ASSERT_EQUAL_INT(50, bx);
    TEST_ASSERT_EQUAL_INT(0, by);
    /* Fully outside, degenerate, non-finite, NULL. */
    TEST_ASSERT_FALSE(gfx_view_clip_line(&v, -20.0, 0.0, -15.0, 0.0, &ax, &ay, &bx, &by));
    gfx_view_set(&v, 1.0, 1.0, 0.0, 5.0, 0, 0, 100, 100);
    TEST_ASSERT_FALSE(gfx_view_clip_line(&v, 0.0, 0.0, 2.0, 2.0, &ax, &ay, &bx, &by));
    gfx_view_set(&v, -10.0, 10.0, -10.0, 10.0, 0, 0, 100, 100);
    TEST_ASSERT_FALSE(gfx_view_clip_line(&v, strtod("inf", NULL), 0.0, 1.0, 1.0, &ax, &ay, &bx, &by));
    TEST_ASSERT_FALSE(gfx_view_clip_line(NULL, 0.0, 0.0, 1.0, 1.0, &ax, &ay, &bx, &by));
    TEST_ASSERT_FALSE(gfx_view_clip_line(&v, 0.0, 0.0, 1.0, 1.0, NULL, &ay, &bx, &by));
}

void test_gfx_view_line_draws(void)
{
    gfx_surface_t s = {NULL, 0, 0};
    gfx_view_t v;

    TEST_ASSERT_TRUE(gfx_surface_alloc(&s, 20, 20));
    gfx_surface_clear(&s, 0x0000);
    gfx_view_set(&v, -10.0, 10.0, -10.0, 10.0, 0, 0, 20, 20);
    TEST_ASSERT_TRUE(gfx_view_line(&v, &s, -10.0, 0.0, 10.0, 0.0, 0xFFFF));
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 0, 9));
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, gfx_surface_get(&s, 19, 9));
    TEST_ASSERT_EQUAL_UINT16(0x0000, gfx_surface_get(&s, 0, 0));
    /* A far-outside segment never yields giant coordinates. */
    TEST_ASSERT_FALSE(gfx_view_line(&v, &s, -1e6, -1e6, -2e6, 5.0, 0xFFFF));
    TEST_ASSERT_FALSE(gfx_view_line(&v, NULL, 0.0, 0.0, 1.0, 1.0, 0xFFFF));
    gfx_surface_free(&s);
}

void test_gfx_view_nice_step(void)
{
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 1.0, gfx_view_nice_step(10.0, 8));
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 10.0, gfx_view_nice_step(95.0, 8));
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.5, gfx_view_nice_step(3.0, 8));
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.1, gfx_view_nice_step(1.0, 8));
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 1.0, gfx_view_nice_step(0.0, 8));
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 1.0, gfx_view_nice_step(10.0, 0));
}
