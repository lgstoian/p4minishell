/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_markdown.c
 * @brief Unit tests for components/markdown (line + document rendering).
 */

#include "unity.h"
#include "markdown.h"
#include <string.h>

void test_markdown_plain_passthrough(void)
{
    char out[512];
    TEST_ASSERT_FALSE(markdown_render_line("hello world", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("hello world\n", out);
    /* Asterisk math without pairs is untouched. */
    TEST_ASSERT_FALSE(markdown_render_line("2 * 3 = 6", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("2 * 3 = 6\n", out);
    /* Intra-word underscore untouched. */
    TEST_ASSERT_FALSE(markdown_render_line("foo_bar baz", out, sizeof(out)));
}

void test_markdown_emphasis(void)
{
    char out[512];
    TEST_ASSERT_TRUE(markdown_render_line("**bold**", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("\x1b[1mbold\x1b[22m\n\x1b[0m", out);
    TEST_ASSERT_TRUE(markdown_render_line("*it*", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("\x1b[3mit\x1b[23m\n\x1b[0m", out);
    TEST_ASSERT_TRUE(markdown_render_line("~~gone~~", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("\x1b[9mgone\x1b[29m\n\x1b[0m", out);
    TEST_ASSERT_TRUE(markdown_render_line("`code`", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("\x1b[2mcode\x1b[22m\n\x1b[0m", out);
    /* Nested: bold containing italic. */
    TEST_ASSERT_TRUE(markdown_render_line("**a *b* c**", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("\x1b[1ma \x1b[3mb\x1b[23m c\x1b[22m\n\x1b[0m", out);
    /* Unpaired opener stays literal. */
    TEST_ASSERT_FALSE(markdown_render_line("a * b * c", out, sizeof(out)));
}

void test_markdown_links(void)
{
    char out[512];
    TEST_ASSERT_TRUE(markdown_render_line("[t](http://x)", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("\x1b[4m\x1b[36mt\x1b[39m\x1b[24m (http://x)\n\x1b[0m", out);
    /* Missing paren: literal. */
    TEST_ASSERT_FALSE(markdown_render_line("[t]http://x", out, sizeof(out)));
}

void test_markdown_blocks(void)
{
    char out[512];
    TEST_ASSERT_TRUE(markdown_render_line("# Hi", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("\x1b[1m\x1b[4mHi\x1b[24m\x1b[22m\n\x1b[0m", out);
    TEST_ASSERT_TRUE(markdown_render_line("## Hi ##", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("\x1b[1m\x1b[4mHi\x1b[24m\x1b[22m\n\x1b[0m", out);
    TEST_ASSERT_TRUE(markdown_render_line("> quoted", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("\xe2\x94\x82 quoted\n", out);
    TEST_ASSERT_TRUE(markdown_render_line("- item", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("- item\n", out);
    TEST_ASSERT_TRUE(markdown_render_line("---", out, sizeof(out)));
    /* 40 box-drawing dashes + newline. */
    TEST_ASSERT_EQUAL(40 * 3 + 1, (int)strlen(out));
}

void test_markdown_doc_tables(void)
{
    char out[1024];
    const char *doc = "| a | bb |\n|---|---|\n| ccc | d |\n";
    size_t n = markdown_render_doc(doc, out, sizeof(out));
    TEST_ASSERT_GREATER_THAN(0, (int)n);
    /* Aligned columns: header + separator + body. */
    TEST_ASSERT_NOT_NULL(strstr(out, "| a   | bb |"));
    TEST_ASSERT_NOT_NULL(strstr(out, "| --- | -- |"));
    TEST_ASSERT_NOT_NULL(strstr(out, "| ccc | d  |"));
}

void test_markdown_doc_fences(void)
{
    char out[1024];
    const char *doc = "text\n```\ncode here\n```\nmore\n";
    markdown_render_doc(doc, out, sizeof(out));
    /* Fence markers dropped, content dimmed. */
    TEST_ASSERT_NULL(strstr(out, "```"));
    TEST_ASSERT_NOT_NULL(strstr(out, "\x1b[2mcode here\x1b[22m"));
    TEST_ASSERT_NOT_NULL(strstr(out, "more"));
}

void test_markdown_display_width(void)
{
    TEST_ASSERT_EQUAL(3, (int)markdown_display_width("abc"));
    TEST_ASSERT_EQUAL(2, (int)markdown_display_width("\xe4\xb8\xad"));
    TEST_ASSERT_EQUAL(3, (int)markdown_display_width("a\xe4\xb8\xad"));
}

void test_markdown_render_html(void)
{
    char out[1024];
    size_t n;

    n = markdown_render_html("# Title\n\nHello **bold** and `code`.\n", out, sizeof(out));
    TEST_ASSERT_GREATER_THAN_size_t(0, n);
    TEST_ASSERT_NOT_NULL(strstr(out, "<h1>Title</h1>"));
    TEST_ASSERT_NOT_NULL(strstr(out, "<strong>bold</strong>"));
    TEST_ASSERT_NOT_NULL(strstr(out, "<code>code</code>"));

    n = markdown_render_html("- one\n- two\n", out, sizeof(out));
    TEST_ASSERT_GREATER_THAN_size_t(0, n);
    TEST_ASSERT_NOT_NULL(strstr(out, "<ul>"));
    TEST_ASSERT_NOT_NULL(strstr(out, "<li>one</li>"));

    n = markdown_render_html("> quoted\n", out, sizeof(out));
    TEST_ASSERT_NOT_NULL(strstr(out, "<blockquote>"));

    n = markdown_render_html("```\n<b>raw</b>\n```\n", out, sizeof(out));
    TEST_ASSERT_NOT_NULL(strstr(out, "<pre><code>"));
    TEST_ASSERT_NOT_NULL(strstr(out, "&lt;b&gt;raw&lt;/b&gt;"));

    /* Escaping keeps angle brackets inert in a paragraph. */
    (void)markdown_render_html("a < b & c\n", out, sizeof(out));
    TEST_ASSERT_NOT_NULL(strstr(out, "&lt;"));
    TEST_ASSERT_NOT_NULL(strstr(out, "&amp;"));

    /* NULL-safety and truncation contract. */
    TEST_ASSERT_EQUAL_size_t(0, markdown_render_html(NULL, out, sizeof(out)));
    TEST_ASSERT_EQUAL_size_t(0, markdown_render_html("x", NULL, sizeof(out)));
}

void test_markdown_render_print(void)
{
    char out[4096];
    char body[512];
    size_t i;
    size_t n;

    n = markdown_render_print("hello print", "Doc", 40, 20, out, sizeof(out));
    TEST_ASSERT_GREATER_THAN_size_t(0, n);
    TEST_ASSERT_NOT_NULL(strstr(out, "Doc"));
    TEST_ASSERT_NOT_NULL(strstr(out, "Page 1"));
    TEST_ASSERT_NOT_NULL(strstr(out, "hello print"));

    /* A body taller than the page must paginate with a form feed + Page 2. */
    for (i = 0; i < sizeof(body) - 2; i++) {
        body[i] = (i % 2 == 0) ? 'a' : ' ';
    }
    body[sizeof(body) - 1] = '\0';
    n = markdown_render_print(body, "Long", 40, 6, out, sizeof(out));
    TEST_ASSERT_GREATER_THAN_size_t(0, n);
    TEST_ASSERT_NOT_NULL(strstr(out, "Page 1"));
    TEST_ASSERT_NOT_NULL(strstr(out, "Page 2"));
    TEST_ASSERT_NOT_NULL(strchr(out, '\f'));

    /* NULL / zero-size safety. */
    TEST_ASSERT_EQUAL_size_t(0, markdown_render_print(NULL, NULL, 40, 20, NULL, 0));
    TEST_ASSERT_EQUAL_size_t(0, markdown_render_print("x", "t", 40, 20, NULL, 10));
}

