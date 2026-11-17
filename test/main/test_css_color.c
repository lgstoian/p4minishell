/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_css_color.c
 * @brief The one CSS colour parser: syntax, alpha and DOS quantisation.
 *
 * HTML, the Canvas 2D context and the TUI all resolve colour strings through
 * `css_color_parse`. These cases pin the shared contract so a page and a
 * modal cannot disagree, and cover the alpha rules transparency relies on.
 */

#include "unity.h"
#include "css_color.h"
#include "js_canvas.h"

void test_css_color_hex_forms(void)
{
    css_rgba_t c = { 0 };

    TEST_ASSERT_TRUE(css_color_parse("#f00", &c));
    TEST_ASSERT_EQUAL_UINT8(255, c.r);
    TEST_ASSERT_EQUAL_UINT8(0, c.g);
    TEST_ASSERT_EQUAL_UINT8(0, c.b);
    TEST_ASSERT_EQUAL_UINT8(255, c.a);

    TEST_ASSERT_TRUE(css_color_parse("#00ff0080", &c));
    TEST_ASSERT_EQUAL_UINT8(0, c.r);
    TEST_ASSERT_EQUAL_UINT8(255, c.g);
    TEST_ASSERT_EQUAL_UINT8(128, c.a);

    TEST_ASSERT_TRUE(css_color_parse("#1234", &c));
    TEST_ASSERT_EQUAL_UINT8(0x11, c.r);
    TEST_ASSERT_EQUAL_UINT8(0x22, c.g);
    TEST_ASSERT_EQUAL_UINT8(0x33, c.b);
    TEST_ASSERT_EQUAL_UINT8(0x44, c.a);

    TEST_ASSERT_FALSE(css_color_parse("#12345", &c));
    TEST_ASSERT_FALSE(css_color_parse("#gggggg", &c));
}

void test_css_color_rgb_and_alpha(void)
{
    css_rgba_t c = { 0 };

    TEST_ASSERT_TRUE(css_color_parse("rgb(1, 2, 3)", &c));
    TEST_ASSERT_EQUAL_UINT8(1, c.r);
    TEST_ASSERT_EQUAL_UINT8(2, c.g);
    TEST_ASSERT_EQUAL_UINT8(3, c.b);
    TEST_ASSERT_EQUAL_UINT8(255, c.a);

    TEST_ASSERT_TRUE(css_color_parse("RGBA(255, 0, 0, 0.5)", &c));
    TEST_ASSERT_EQUAL_UINT8(255, c.r);
    TEST_ASSERT_UINT8_WITHIN(2, 128, c.a);

    TEST_ASSERT_TRUE(css_color_parse("rgba(0,0,0,0)", &c));
    TEST_ASSERT_EQUAL_UINT8(0, c.a);

    TEST_ASSERT_TRUE(css_color_parse("rgba(0,0,0,100%)", &c));
    TEST_ASSERT_EQUAL_UINT8(255, c.a);

    TEST_ASSERT_TRUE(css_color_parse("rgb(100%, 0%, 50%)", &c));
    TEST_ASSERT_EQUAL_UINT8(255, c.r);
    TEST_ASSERT_EQUAL_UINT8(0, c.g);
    TEST_ASSERT_UINT8_WITHIN(2, 128, c.b);
}

void test_css_color_transparent_and_named(void)
{
    css_rgba_t c = { 0 };

    TEST_ASSERT_TRUE(css_color_parse("transparent", &c));
    TEST_ASSERT_EQUAL_UINT8(0, c.a);

    TEST_ASSERT_TRUE(css_color_parse("White", &c));
    TEST_ASSERT_EQUAL_UINT8(255, c.r);
    TEST_ASSERT_EQUAL_UINT8(255, c.g);
    TEST_ASSERT_EQUAL_UINT8(255, c.b);
    TEST_ASSERT_EQUAL_UINT8(255, c.a);

    TEST_ASSERT_TRUE(css_color_parse("navy", &c));
    TEST_ASSERT_EQUAL_UINT8(0, c.r);
    TEST_ASSERT_EQUAL_UINT8(0, c.g);
    TEST_ASSERT_EQUAL_UINT8(128, c.b);

    TEST_ASSERT_FALSE(css_color_parse("not-a-colour", &c));
    TEST_ASSERT_FALSE(css_color_parse("", &c));
    TEST_ASSERT_FALSE(css_color_parse(NULL, &c));
}

void test_css_color_dos_quantisation(void)
{
    css_rgba_t c = { 0 };

    /* Pure white and pure black land on the exact DOS extremes. */
    TEST_ASSERT_TRUE(css_color_parse("#ffffff", &c));
    TEST_ASSERT_EQUAL_UINT8(15, css_color_to_dos(&c));
    TEST_ASSERT_TRUE(css_color_parse("#000000", &c));
    TEST_ASSERT_EQUAL_UINT8(0, css_color_to_dos(&c));

    /* A fully transparent colour has no DOS index. */
    TEST_ASSERT_TRUE(css_color_parse("rgba(255,0,0,0)", &c));
    TEST_ASSERT_EQUAL_UINT8(16, css_color_to_dos(&c));

    /* DOS 16 means "unset"; every real index round-trips to itself. */
    for (uint8_t i = 0; i < 16; i++) {
        css_rgba_t pal = css_color_from_dos(i);

        TEST_ASSERT_EQUAL_UINT8(i, css_color_to_dos(&pal));
        TEST_ASSERT_EQUAL_UINT8(255, pal.a);
    }
    TEST_ASSERT_EQUAL_UINT8(255, css_color_from_dos(16).a);
}

void test_css_color_blend(void)
{
    css_rgba_t fg = { 255, 0, 0, 255 };
    css_rgba_t bg = { 0, 0, 0, 255 };
    css_rgba_t out;

    out = css_color_blend(fg, bg, 255);
    TEST_ASSERT_EQUAL_UINT8(255, out.r);
    TEST_ASSERT_EQUAL_UINT8(0, out.g);
    TEST_ASSERT_EQUAL_UINT8(255, out.a);

    /* Half red over black is the dark red DOS cell. */
    out = css_color_blend(fg, bg, 128);
    TEST_ASSERT_UINT8_WITHIN(2, 128, out.r);
    TEST_ASSERT_EQUAL_UINT8(255, out.a);

    out = css_color_blend(fg, bg, 0);
    TEST_ASSERT_EQUAL_UINT8(0, out.r);
    TEST_ASSERT_EQUAL_UINT8(0, out.g);
    TEST_ASSERT_EQUAL_UINT8(0, out.b);
}

void test_css_color_is_the_canvas_parser(void)
{
    css_rgba_t a = { 0 };
    js_canvas_rgba_t b = { 0 };
    static const char *const samples[] = {
        "#abc", "#11223344", "rgb(10,20,30)", "rgba(1,2,3,0.25)",
        "transparent", "olive",
    };

    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
        TEST_ASSERT_TRUE(css_color_parse(samples[i], &a));
        TEST_ASSERT_TRUE(js_canvas_parse_color(samples[i], &b));
        TEST_ASSERT_EQUAL_UINT8(a.r, b.r);
        TEST_ASSERT_EQUAL_UINT8(a.g, b.g);
        TEST_ASSERT_EQUAL_UINT8(a.b, b.b);
        TEST_ASSERT_EQUAL_UINT8(a.a, b.a);
    }
    /* The canvas accepts the same strings, including the ones only the shared
     * parser knows (aqua/fuchsia/teal/...): one implementation, no drift. */
    TEST_ASSERT_TRUE(js_canvas_parse_color("aqua", &b));
    TEST_ASSERT_TRUE(js_canvas_parse_color("teal", &b));
    TEST_ASSERT_FALSE(js_canvas_parse_color("nope", &b));
}
