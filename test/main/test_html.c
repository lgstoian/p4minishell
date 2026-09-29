/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_html.c
 * @brief Unit tests for the HTML reader (components/markdown/html_read.c).
 *
 * Covers the block/inline/entity mapping to ANSI by checking the ANSI-stripped
 * reading text plus a few SGR-presence assertions. Pure: no SD, no LVGL.
 */

#include "unity.h"
#include "html.h"
#include "markdown.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Render + strip SGR into a caller buffer for readable assertions. */
static void render_plain(const char *html, char *dst, size_t dst_size)
{
    char *ansi = malloc(4096);

    TEST_ASSERT_NOT_NULL(ansi);
    html_render_ansi(html, ansi, 4096);
    markdown_strip_ansi(ansi, dst, dst_size);
    free(ansi);
}

void test_html_render_basic(void)
{
    char ansi[512];
    char plain[256];

    (void)html_render_ansi("<h1>Title</h1><p>Hello <b>bold</b> and <code>code</code>.</p>",
                           ansi, sizeof(ansi));
    TEST_ASSERT_NOT_NULL(strstr(ansi, "\x1b[1m"));   /* bold heading/strong */
    render_plain("<h1>Title</h1><p>Hello <b>bold</b> and <code>code</code>.</p>",
                 plain, sizeof(plain));
    TEST_ASSERT_NOT_NULL(strstr(plain, "Title"));
    TEST_ASSERT_NOT_NULL(strstr(plain, "Hello"));
    TEST_ASSERT_NOT_NULL(strstr(plain, "bold"));
    TEST_ASSERT_NOT_NULL(strstr(plain, "code"));
}

void test_html_render_entities(void)
{
    char plain[256];

    render_plain("A&amp;B &lt;x&gt; &quot;q&quot; &#39; &mdash; &#x41; end", plain, sizeof(plain));
    TEST_ASSERT_NOT_NULL(strstr(plain, "A&B"));
    TEST_ASSERT_NOT_NULL(strstr(plain, "<x>"));
    TEST_ASSERT_NOT_NULL(strstr(plain, "\"q\""));
    TEST_ASSERT_NOT_NULL(strstr(plain, "'"));
    TEST_ASSERT_NOT_NULL(strstr(plain, "A end")); /* &#x41; -> A */
    /* No stray ampersands left from decoded entities. */
    TEST_ASSERT_NULL(strstr(plain, "&amp;"));
    TEST_ASSERT_NULL(strstr(plain, "&#"));
}

void test_html_render_lists(void)
{
    char plain[256];

    render_plain("<ul><li>one</li><li>two</li></ul>", plain, sizeof(plain));
    TEST_ASSERT_NOT_NULL(strstr(plain, "- one"));
    TEST_ASSERT_NOT_NULL(strstr(plain, "- two"));

    render_plain("<ol><li>alpha</li><li>beta</li></ol>", plain, sizeof(plain));
    TEST_ASSERT_NOT_NULL(strstr(plain, "1. alpha"));
    TEST_ASSERT_NOT_NULL(strstr(plain, "2. beta"));
}

void test_html_render_nested_list(void)
{
    char plain[256];

    render_plain("<ul><li>a<ul><li>b</li></ul></li></ul>", plain, sizeof(plain));
    TEST_ASSERT_NOT_NULL(strstr(plain, "- a"));
    TEST_ASSERT_NOT_NULL(strstr(plain, "  - b")); /* nested indent */
}

void test_html_render_table(void)
{
    char plain[256];

    render_plain("<table><tr><th>A</th><th>B</th></tr>"
                 "<tr><td>1</td><td>2</td></tr></table>", plain, sizeof(plain));
    TEST_ASSERT_NOT_NULL(strstr(plain, "| A"));
    TEST_ASSERT_NOT_NULL(strstr(plain, "| 1"));
}

void test_html_render_links_images(void)
{
    char plain[256];

    render_plain("<a href=\"http://example/x\">text</a>", plain, sizeof(plain));
    TEST_ASSERT_NOT_NULL(strstr(plain, "text (http://example/x)"));

    render_plain("<img alt=\"a picture\">", plain, sizeof(plain));
    TEST_ASSERT_NOT_NULL(strstr(plain, "[a picture]"));

    render_plain("<img>", plain, sizeof(plain));
    TEST_ASSERT_NOT_NULL(strstr(plain, "[image]"));
}

void test_html_render_skips_head_script_style(void)
{
    char plain[256];

    render_plain("<head><title>Hidden</title></head>"
                 "<p>body</p><script>var x=1;</script><style>.a{}</style>",
                 plain, sizeof(plain));
    TEST_ASSERT_NOT_NULL(strstr(plain, "body"));
    TEST_ASSERT_NULL(strstr(plain, "Hidden"));
    TEST_ASSERT_NULL(strstr(plain, "var x"));
    TEST_ASSERT_NULL(strstr(plain, ".a{}"));
}

void test_html_render_pre_preserves(void)
{
    char plain[256];

    render_plain("<pre>a\n  b</pre>", plain, sizeof(plain));
    TEST_ASSERT_NOT_NULL(strstr(plain, "a\n  b"));
}

void test_html_render_comment_and_doctype(void)
{
    char plain[256];

    render_plain("<!DOCTYPE html><!-- hidden --><p>x</p>", plain, sizeof(plain));
    TEST_ASSERT_NOT_NULL(strstr(plain, "x"));
    TEST_ASSERT_NULL(strstr(plain, "DOCTYPE"));
    TEST_ASSERT_NULL(strstr(plain, "hidden"));
}

void test_html_render_blockquote(void)
{
    char plain[256];

    render_plain("<blockquote><p>quoted</p></blockquote>", plain, sizeof(plain));
    TEST_ASSERT_NOT_NULL(strstr(plain, "quoted"));
    TEST_ASSERT_NOT_NULL(strstr(plain, "\xE2\x94\x82")); /* U+2502 bar */
}

void test_html_render_measure_and_truncate(void)
{
    char small[8];
    size_t need;

    /* NULL source -> 0. */
    TEST_ASSERT_EQUAL_size_t(0, html_render_ansi(NULL, small, sizeof(small)));

    /* Measure-only returns the required length. */
    need = html_render_ansi("<p>hello</p>", NULL, 0);
    TEST_ASSERT_GREATER_THAN_size_t(0, need);

    /* A too-small destination truncates but stays NUL-terminated. */
    need = html_render_ansi("<p>hello world, this is long</p>", small, sizeof(small));
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32((uint32_t)sizeof(small), (uint32_t)need);
    TEST_ASSERT_TRUE(strlen(small) < sizeof(small));
}

void test_html_render_malformed_is_tolerated(void)
{
    char plain[256];

    /* Unclosed tag, stray '<', attributes with no value: must not crash and
     * should still surface the text. */
    render_plain("<p>ok <notclosed and < 3 >end", plain, sizeof(plain));
    TEST_ASSERT_NOT_NULL(strstr(plain, "ok"));
    TEST_ASSERT_NOT_NULL(strstr(plain, "end"));
}
