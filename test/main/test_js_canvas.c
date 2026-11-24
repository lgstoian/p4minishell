/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_js_canvas.c
 * @brief Canvas 2D core: compositing, transforms, paths, text, snapshots.
 *
 * The browser's `<canvas>` path is a software rasterizer. These cases pin the
 * spec behaviours that are easy to get subtly wrong (source-over alpha,
 * clipping, save/restore, the affine transform, width/height reset) without a
 * board, a JS engine or LVGL.
 */

#include "unity.h"
#include "js_canvas.h"
#include <string.h>

static js_canvas_rgba_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    js_canvas_rgba_t c = { r, g, b, a };

    return c;
}

static void px(const js_canvas_t *canvas, int x, int y, uint8_t *out)
{
    const uint8_t *p = js_canvas_pixels(canvas) +
        ((size_t)y * (size_t)js_canvas_width(canvas) + (size_t)x) * 4u;

    memcpy(out, p, 4);
}

void test_js_canvas_fill_rect_and_clear(void)
{
    js_canvas_t *canvas = js_canvas_new(8, 8);
    uint8_t p[4];

    TEST_ASSERT_NOT_NULL(canvas);
    js_canvas_set_fill(canvas, rgba(255, 0, 0, 255));
    js_canvas_fill_rect(canvas, 1, 1, 3, 3);
    px(canvas, 1, 1, p);
    TEST_ASSERT_EQUAL_UINT8(255, p[0]);
    TEST_ASSERT_EQUAL_UINT8(0, p[1]);
    TEST_ASSERT_EQUAL_UINT8(255, p[3]);
    px(canvas, 0, 0, p);
    TEST_ASSERT_EQUAL_UINT8(0, p[3]);

    js_canvas_clear_rect(canvas, 2, 2, 1, 1);
    px(canvas, 2, 2, p);
    TEST_ASSERT_EQUAL_UINT8(0, p[3]);
    js_canvas_free(canvas);
}

/** Source-over: 50% alpha over opaque black is a half-intensity red. */
void test_js_canvas_global_alpha(void)
{
    js_canvas_t *canvas = js_canvas_new(2, 2);
    uint8_t p[4];

    TEST_ASSERT_NOT_NULL(canvas);
    js_canvas_set_fill(canvas, rgba(0, 0, 0, 255));
    js_canvas_fill_rect(canvas, 0, 0, 2, 2);
    js_canvas_set_global_alpha(canvas, 128);
    js_canvas_set_fill(canvas, rgba(255, 0, 0, 255));
    js_canvas_fill_rect(canvas, 0, 0, 2, 2);
    px(canvas, 0, 0, p);
    TEST_ASSERT_UINT8_WITHIN(2, 128, p[0]);
    TEST_ASSERT_EQUAL_UINT8(0, p[1]);
    TEST_ASSERT_EQUAL_UINT8(255, p[3]);
    js_canvas_free(canvas);
}

void test_js_canvas_transform_translate_scale(void)
{
    js_canvas_t *canvas = js_canvas_new(16, 16);
    uint8_t p[4];

    TEST_ASSERT_NOT_NULL(canvas);
    js_canvas_set_fill(canvas, rgba(0, 255, 0, 255));
    js_canvas_translate(canvas, 4, 4);
    js_canvas_scale(canvas, 2, 2);
    js_canvas_fill_rect(canvas, 0, 0, 2, 2);
    px(canvas, 4, 4, p);
    TEST_ASSERT_EQUAL_UINT8(255, p[1]);
    px(canvas, 7, 7, p);
    TEST_ASSERT_EQUAL_UINT8(255, p[1]);
    px(canvas, 8, 8, p);
    TEST_ASSERT_EQUAL_UINT8(0, p[3]);

    js_canvas_reset_transform(canvas);
    js_canvas_save(canvas);
    js_canvas_translate(canvas, 10, 0);
    js_canvas_restore(canvas);
    js_canvas_set_fill(canvas, rgba(0, 0, 255, 255));
    js_canvas_fill_rect(canvas, 0, 0, 1, 1);
    px(canvas, 0, 0, p);
    TEST_ASSERT_EQUAL_UINT8(255, p[2]);
    js_canvas_free(canvas);
}

void test_js_canvas_stroke_rect_border(void)
{
    js_canvas_t *canvas = js_canvas_new(10, 10);
    uint8_t p[4];

    TEST_ASSERT_NOT_NULL(canvas);
    js_canvas_set_stroke(canvas, rgba(255, 255, 255, 255));
    js_canvas_stroke_rect(canvas, 0, 0, 6, 6);
    px(canvas, 0, 0, p);
    TEST_ASSERT_EQUAL_UINT8(255, p[3]);
    px(canvas, 5, 0, p);
    TEST_ASSERT_EQUAL_UINT8(255, p[3]);
    px(canvas, 2, 3, p);
    TEST_ASSERT_EQUAL_UINT8(0, p[3]);
    js_canvas_free(canvas);
}

void test_js_canvas_path_fill_and_arc(void)
{
    js_canvas_t *canvas = js_canvas_new(16, 16);
    uint8_t p[4];

    TEST_ASSERT_NOT_NULL(canvas);
    js_canvas_set_fill(canvas, rgba(0, 0, 255, 255));
    js_canvas_begin_path(canvas);
    js_canvas_move_to(canvas, 2, 2);
    js_canvas_line_to(canvas, 12, 2);
    js_canvas_line_to(canvas, 2, 12);
    js_canvas_close_path(canvas);
    js_canvas_fill(canvas);
    px(canvas, 4, 4, p);
    TEST_ASSERT_EQUAL_UINT8(255, p[2]);
    px(canvas, 11, 11, p);
    TEST_ASSERT_EQUAL_UINT8(0, p[3]);

    js_canvas_clear(canvas);
    js_canvas_set_fill(canvas, rgba(255, 255, 0, 255));
    js_canvas_begin_path(canvas);
    js_canvas_arc(canvas, 8, 8, 5, 0, 6.2831853f, false);
    js_canvas_close_path(canvas);
    js_canvas_fill(canvas);
    px(canvas, 8, 8, p);
    TEST_ASSERT_EQUAL_UINT8(255, p[0]);
    TEST_ASSERT_EQUAL_UINT8(255, p[1]);
    js_canvas_free(canvas);
}

void test_js_canvas_text_and_measure(void)
{
    js_canvas_t *canvas = js_canvas_new(40, 20);
    int lit = 0;

    TEST_ASSERT_NOT_NULL(canvas);
    TEST_ASSERT_EQUAL_INT(8, js_canvas_measure_text(canvas, "A"));
    TEST_ASSERT_EQUAL_INT(16, js_canvas_measure_text(canvas, "AB"));
    js_canvas_set_fill(canvas, rgba(255, 255, 255, 255));
    js_canvas_fill_text(canvas, "AB", 0, 8, false);
    for (int y = 0; y < 20; y++) {
        for (int x = 0; x < 40; x++) {
            uint8_t p[4];

            px(canvas, x, y, p);
            if (p[3] != 0) lit++;
        }
    }
    TEST_ASSERT_GREATER_THAN_INT(10, lit);
    TEST_ASSERT_LESS_THAN_INT(16 * 8 * 2, lit);
    js_canvas_free(canvas);
}

void test_js_canvas_resize_resets_bitmap(void)
{
    js_canvas_t *canvas = js_canvas_new(8, 8);
    uint8_t p[4];

    TEST_ASSERT_NOT_NULL(canvas);
    js_canvas_set_fill(canvas, rgba(255, 0, 0, 255));
    js_canvas_fill_rect(canvas, 0, 0, 8, 8);
    TEST_ASSERT_TRUE(js_canvas_resize(canvas, 4, 3));
    TEST_ASSERT_EQUAL_INT(4, js_canvas_width(canvas));
    TEST_ASSERT_EQUAL_INT(3, js_canvas_height(canvas));
    px(canvas, 0, 0, p);
    TEST_ASSERT_EQUAL_UINT8(0, p[3]);
    js_canvas_free(canvas);
}

void test_js_canvas_snapshot_downsample(void)
{
    js_canvas_t *canvas = js_canvas_new(8, 8);
    uint8_t out[4 * 4 * 4];
    int w = 0;
    int h = 0;

    TEST_ASSERT_NOT_NULL(canvas);
    js_canvas_set_fill(canvas, rgba(255, 0, 0, 255));
    js_canvas_fill_rect(canvas, 0, 0, 4, 8);
    TEST_ASSERT_TRUE(js_canvas_snapshot_rgba(canvas, 4, 4, out, &w, &h));
    TEST_ASSERT_EQUAL_INT(4, w);
    TEST_ASSERT_EQUAL_INT(4, h);
    TEST_ASSERT_EQUAL_UINT8(255, out[0]);
    TEST_ASSERT_EQUAL_UINT8(255, out[3]);
    TEST_ASSERT_EQUAL_UINT8(0, out[(size_t)3 * 4 + 3]); /* right half empty */
    js_canvas_free(canvas);
}

void test_js_canvas_parse_color(void)
{
    js_canvas_rgba_t c;

    TEST_ASSERT_TRUE(js_canvas_parse_color("#f00", &c));
    TEST_ASSERT_EQUAL_UINT8(255, c.r);
    TEST_ASSERT_EQUAL_UINT8(0, c.g);
    TEST_ASSERT_EQUAL_UINT8(255, c.a);
    TEST_ASSERT_TRUE(js_canvas_parse_color("#00ff0080", &c));
    TEST_ASSERT_EQUAL_UINT8(0, c.r);
    TEST_ASSERT_EQUAL_UINT8(255, c.g);
    TEST_ASSERT_EQUAL_UINT8(128, c.a);
    TEST_ASSERT_TRUE(js_canvas_parse_color("rgb(1, 2, 3)", &c));
    TEST_ASSERT_EQUAL_UINT8(1, c.r);
    TEST_ASSERT_EQUAL_UINT8(3, c.b);
    TEST_ASSERT_TRUE(js_canvas_parse_color("rgba(255,0,0,0.5)", &c));
    TEST_ASSERT_UINT8_WITHIN(2, 128, c.a);
    TEST_ASSERT_TRUE(js_canvas_parse_color("transparent", &c));
    TEST_ASSERT_EQUAL_UINT8(0, c.a);
    TEST_ASSERT_FALSE(js_canvas_parse_color("not-a-colour", &c));
}

/** The Canvas gradient is the CSS gradient parser in disguise: one colour
 *  model, one DOS quantisation path, deterministic sampled pixels. */
void test_js_canvas_linear_gradient_rect(void)
{
    js_canvas_t *canvas = js_canvas_new(8, 4);
    js_canvas_gradient_t gradient;
    uint8_t p[4];

    TEST_ASSERT_NOT_NULL(canvas);
    TEST_ASSERT_TRUE(js_canvas_gradient_parse(
        "linear-gradient(90deg, #ff0000 0%, #0000ff 100%)", &gradient));
    TEST_ASSERT_EQUAL_INT(2, gradient.count);
    TEST_ASSERT_EQUAL_INT(90, gradient.angle);
    TEST_ASSERT_EQUAL_UINT8(255, js_canvas_gradient_at(&gradient, 0.0f).r);
    TEST_ASSERT_EQUAL_UINT8(255, js_canvas_gradient_at(&gradient, 1.0f).b);
    js_canvas_fill_gradient_rect(canvas, &gradient, 0, 0, 8, 4, false);
    px(canvas, 0, 1, p);
    TEST_ASSERT_EQUAL_UINT8(255, p[0]);
    TEST_ASSERT_EQUAL_UINT8(0, p[2]);
    px(canvas, 7, 1, p);
    TEST_ASSERT_EQUAL_UINT8(255, p[2]);
    TEST_ASSERT_EQUAL_UINT8(255, p[3]);
    js_canvas_free(canvas);
}

/** Rounded rect masks its corners; the shadow is a deterministic offset copy
 *  with no blur kernel or additional rasterizer. */
void test_js_canvas_rounded_rect_shadow(void)
{
    js_canvas_t *canvas = js_canvas_new(12, 12);
    uint8_t p[4];

    TEST_ASSERT_NOT_NULL(canvas);
    js_canvas_set_fill(canvas, rgba(255, 255, 255, 255));
    js_canvas_fill_rounded_rect(canvas, 2, 2, 8, 8, 3);
    px(canvas, 6, 6, p);
    TEST_ASSERT_EQUAL_UINT8(255, p[3]);
    px(canvas, 2, 2, p);
    TEST_ASSERT_EQUAL_UINT8(0, p[3]);   /* outside the quarter-circle */

    js_canvas_clear(canvas);
    js_canvas_set_shadow(canvas, rgba(255, 0, 0, 255), 2, 0, 0);
    js_canvas_set_fill(canvas, rgba(0, 0, 255, 255));
    js_canvas_fill_rect(canvas, 1, 1, 3, 3);
    px(canvas, 4, 2, p);
    TEST_ASSERT_EQUAL_UINT8(255, p[0]);   /* red shadow, outside the blue rect */
    TEST_ASSERT_EQUAL_UINT8(0, p[2]);
    TEST_ASSERT_EQUAL_INT(2, js_canvas_get_shadow_offset_x(canvas));
    js_canvas_clear_shadow(canvas);
    js_canvas_free(canvas);
}
