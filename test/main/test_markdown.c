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

    /* NULL-safety. A NULL destination is a measure-only request, so it
     * returns the required size rather than 0. */
    TEST_ASSERT_EQUAL_size_t(0, markdown_render_html(NULL, out, sizeof(out)));
    TEST_ASSERT_GREATER_THAN_size_t(0, markdown_render_html("x", NULL, sizeof(out)));
}

void test_markdown_html_lists(void)
{
    char out[2048];

    (void)markdown_render_html("1. one\n2. two\n", out, sizeof(out));
    TEST_ASSERT_NOT_NULL(strstr(out, "<ol>"));
    TEST_ASSERT_NOT_NULL(strstr(out, "<li>one</li>"));
    TEST_ASSERT_NOT_NULL(strstr(out, "</ol>"));

    /* Nested unordered list sits inside the parent item. */
    (void)markdown_render_html("- a\n  - b\n", out, sizeof(out));
    TEST_ASSERT_NOT_NULL(strstr(out, "<li>a<ul>"));

    /* Task list: checkbox states. */
    (void)markdown_render_html("- [ ] todo\n- [x] done\n", out, sizeof(out));
    TEST_ASSERT_NOT_NULL(strstr(out, "<li class=\"task\">"));
    TEST_ASSERT_NOT_NULL(strstr(out, "<input type=\"checkbox\" disabled> todo</li>"));
    TEST_ASSERT_NOT_NULL(strstr(out, "disabled checked> done</li>"));
}

void test_markdown_html_table(void)
{
    char out[2048];

    (void)markdown_render_html("| a | b |\n|---|:--:|\n| 1 | 2 |\n", out, sizeof(out));
    TEST_ASSERT_NOT_NULL(strstr(out, "<table>"));
    TEST_ASSERT_NOT_NULL(strstr(out, "<thead>"));
    TEST_ASSERT_NOT_NULL(strstr(out, "<th align=\"left\">a</th>"));
    TEST_ASSERT_NOT_NULL(strstr(out, "<th align=\"center\">b</th>"));
    TEST_ASSERT_NOT_NULL(strstr(out, "<td align=\"left\">1</td>"));
    TEST_ASSERT_NOT_NULL(strstr(out, "</table>"));
}

void test_markdown_html_inline(void)
{
    char out[2048];

    /* Nested inline markup recurses. */
    (void)markdown_render_html("**a *b* c**\n", out, sizeof(out));
    TEST_ASSERT_NOT_NULL(strstr(out, "<strong>a <em>b</em> c</strong>"));

    /* Flanking: these must NOT emphasize (passthrough guarantee). */
    (void)markdown_render_html("2 * 3 = 6\n", out, sizeof(out));
    TEST_ASSERT_NULL(strstr(out, "<em>"));
    (void)markdown_render_html("foo_bar baz\n", out, sizeof(out));
    TEST_ASSERT_NULL(strstr(out, "<em>"));
    (void)markdown_render_html("a * b * c\n", out, sizeof(out));
    TEST_ASSERT_NULL(strstr(out, "<em>"));

    /* Escapes stay literal; multi-backtick code spans are opaque. */
    (void)markdown_render_html("\\*x\\*\n", out, sizeof(out));
    TEST_ASSERT_NOT_NULL(strstr(out, "*x*"));
    TEST_ASSERT_NULL(strstr(out, "<em>"));
    (void)markdown_render_html("``a `b` c``\n", out, sizeof(out));
    TEST_ASSERT_NOT_NULL(strstr(out, "<code>a `b` c</code>"));
}

void test_markdown_html_links_images(void)
{
    char out[2048];

    (void)markdown_render_html("![alt](pic.png)\n", out, sizeof(out));
    TEST_ASSERT_NOT_NULL(strstr(out, "<img src=\"pic.png\" alt=\"alt\">"));

    (void)markdown_render_html("<http://x>\n", out, sizeof(out));
    TEST_ASSERT_NOT_NULL(strstr(out, "<a href=\"http://x\">http://x</a>"));

    /* Unsafe scheme is refused (text stays inert). */
    (void)markdown_render_html("[x](javascript:alert(1))\n", out, sizeof(out));
    TEST_ASSERT_NULL(strstr(out, "<a "));
    TEST_ASSERT_NOT_NULL(strstr(out, "javascript"));
}

void test_markdown_html_heading_fence(void)
{
    char out[2048];

    (void)markdown_render_html("### x ###\n", out, sizeof(out));
    TEST_ASSERT_NOT_NULL(strstr(out, "<h3>x</h3>"));
    /* 7 hashes is not a heading. */
    (void)markdown_render_html("####### x\n", out, sizeof(out));
    TEST_ASSERT_NULL(strstr(out, "<h"));

    (void)markdown_render_html("```c\nint x;\n```\n", out, sizeof(out));
    TEST_ASSERT_NOT_NULL(strstr(out, "<pre><code class=\"language-c\">"));
}

void test_markdown_html_measure(void)
{
    const char *doc = "1. one\n2. two\n- a\n- b\n";
    char small[16];
    size_t need = markdown_render_html(doc, NULL, 0); /* measure-only */
    size_t got = markdown_render_html(doc, small, sizeof(small));

    /* A small destination still reports the full required size so the caller
     * can detect and report truncation (never just the bytes that fit). */
    TEST_ASSERT_GREATER_THAN_size_t(0, need);
    TEST_ASSERT_EQUAL_size_t(need, got);
    TEST_ASSERT_GREATER_OR_EQUAL_size_t(sizeof(small), got);
    TEST_ASSERT_EQUAL_CHAR('\0', small[sizeof(small) - 1]);
}

void test_markdown_render_html_page(void)
{
    char out[2048];
    size_t n;

    n = markdown_render_html_page("# Title\n\nHello **bold**.\n", "Notes",
                                  out, sizeof(out));
    TEST_ASSERT_GREATER_THAN_size_t(0, n);
    /* Full-page shell with reader CSS and escaped title. */
    TEST_ASSERT_NOT_NULL(strstr(out, "<!DOCTYPE html>"));
    TEST_ASSERT_NOT_NULL(strstr(out, "<html"));
    TEST_ASSERT_NOT_NULL(strstr(out, "<title>Notes</title>"));
    TEST_ASSERT_NOT_NULL(strstr(out, "<style>"));
    TEST_ASSERT_NOT_NULL(strstr(out, "max-width:42em"));
    TEST_ASSERT_NOT_NULL(strstr(out, "<main>"));
    /* Body reuses the fragment renderer (no duplication). */
    TEST_ASSERT_NOT_NULL(strstr(out, "<h1>Title</h1>"));
    TEST_ASSERT_NOT_NULL(strstr(out, "<strong>bold</strong>"));
    TEST_ASSERT_NOT_NULL(strstr(out, "</html>"));

    /* Title is escaped; NULL/empty falls back without crashing. */
    n = markdown_render_html_page("hi\n", "<a>&\"b\"</a>", out, sizeof(out));
    TEST_ASSERT_GREATER_THAN_size_t(0, n);
    TEST_ASSERT_NOT_NULL(strstr(out, "&lt;a&gt;&amp;&quot;b&quot;&lt;/a&gt;"));
    n = markdown_render_html_page("hi\n", NULL, out, sizeof(out));
    TEST_ASSERT_GREATER_THAN_size_t(0, n);
    TEST_ASSERT_NOT_NULL(strstr(out, "<title>P4MiniShell</title>"));

    /* The fragment entry point is unchanged (still a fragment, no wrapper). */
    n = markdown_render_html("# T\n", out, sizeof(out));
    TEST_ASSERT_GREATER_THAN_size_t(0, n);
    TEST_ASSERT_NULL(strstr(out, "<!DOCTYPE html>"));
    TEST_ASSERT_NOT_NULL(strstr(out, "<h1>T</h1>"));

    /* NULL-safety and truncation contract mirrors the fragment form. A NULL
     * destination is a measure-only request (returns the required size). */
    TEST_ASSERT_EQUAL_size_t(0, markdown_render_html_page(NULL, "t", out, sizeof(out)));
    TEST_ASSERT_GREATER_THAN_size_t(0, markdown_render_html_page("x", "t", NULL, sizeof(out)));
    TEST_ASSERT_GREATER_OR_EQUAL_size_t(0, markdown_render_html_page("x", "t", out, 0));
}

void test_markdown_render_print(void)
{
    char out[4096];
    char small[16];
    char body[512];
    size_t small_got;
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

    /* NULL / zero-size safety: no source or no destination is an empty
     * document, and a small destination still reports the required size. */
    TEST_ASSERT_EQUAL_size_t(0, markdown_render_print(NULL, NULL, 40, 20, NULL, 0));
    small_got = markdown_render_print("x", "t", 40, 20, small, sizeof(small));
    TEST_ASSERT_GREATER_THAN_size_t(0, small_got);
    TEST_ASSERT_GREATER_OR_EQUAL_size_t(sizeof(small), small_got);
    TEST_ASSERT_EQUAL_CHAR('\0', small[sizeof(small) - 1]);
}

void test_markdown_print_widths(void)
{
    char out[4096];

    /* Tab expands to the next 4-cell stop. */
    (void)markdown_render_print("a\tb\n", "t", 8, 100, out, sizeof(out));
    TEST_ASSERT_NOT_NULL(strstr(out, "a   b"));

    /* CR is not a printable column. */
    (void)markdown_render_print("abc\r\n", "t", 8, 100, out, sizeof(out));
    TEST_ASSERT_NULL(strchr(out, '\r'));

    /* CJK: a wide line wraps earlier than the same cell count of ASCII. */
    {
        const char *five = "\xe4\xb8\xad\xe4\xb8\xad\xe4\xb8\xad\xe4\xb8\xad\xe4\xb8\xad";
        const char *six = "\xe4\xb8\xad\xe4\xb8\xad\xe4\xb8\xad\xe4\xb8\xad\xe4\xb8\xad\xe4\xb8\xad";
        int n5 = 0;
        int n6 = 0;
        size_t i;
        (void)markdown_render_print(five, "t", 10, 100, out, sizeof(out));
        for (i = 0; out[i] != '\0'; i++) {
            if (out[i] == '\n') {
                n5++;
            }
        }
        (void)markdown_render_print(six, "t", 10, 100, out, sizeof(out));
        for (i = 0; out[i] != '\0'; i++) {
            if (out[i] == '\n') {
                n6++;
            }
        }
        TEST_ASSERT_EQUAL_INT(n5 + 1, n6);
        /* The wide characters survive (no mid-sequence split). */
        TEST_ASSERT_NOT_NULL(strstr(out, "\xe4\xb8\xad"));
    }
}

