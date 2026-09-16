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
