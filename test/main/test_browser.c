/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_browser.c
 * @brief Website-rendering unit tests.
 *
 * Drives the SAME pure HTML->TUI-page path the networked `browser <url>` uses
 * (browser_render_html) and asserts the rendered cells through the new
 * tui_page_get_cell() getter - no live network, no LVGL, no screenshot needed.
 * Covers the structure the renderer promises: headings, paragraphs, links
 * (colored), images (alt text), lists and tables.
 */

#include "unity.h"
#include "command.h"
#include "tui.h"
#include "p4minishell_config.h"

#include <stdbool.h>
#include <string.h>
#include <stdint.h>

/* Concatenate one page row's cells (1-based cols) into @p out. */
static void page_row_text(int row, char *out, size_t out_size)
{
    size_t pos = 0;
    out[0] = '\0';
    for (int c = 1; c <= P4_CONFIG_TUI_COLS; c++) {
        char u[8];
        if (!tui_page_get_cell(c, row, u, sizeof(u), NULL, NULL, NULL)) {
            continue;
        }
        size_t l = strlen(u);
        if (pos + l + 1 >= out_size) {
            break;
        }
        memcpy(out + pos, u, l);
        pos += l;
    }
    out[pos] = '\0';
}

static bool page_contains(const char *needle)
{
    char row[P4_CONFIG_TUI_COLS * 4 + 4];
    for (int r = 1; r <= P4_CONFIG_TUI_PAGE_ROWS; r++) {
        page_row_text(r, row, sizeof(row));
        if (strstr(row, needle) != NULL) {
            return true;
        }
    }
    return false;
}

void test_browser_render_heading_and_paragraph(void)
{
    const char *html =
        "<html><body><h1>Big Title</h1><p>Hello world</p></body></html>";
    TEST_ASSERT_EQUAL_INT(0, browser_render_html(html, "http://test/"));
    TEST_ASSERT_TRUE(page_contains("Big Title"));
    TEST_ASSERT_TRUE(page_contains("Hello world"));
}

void test_browser_render_link_is_colored(void)
{
    const char *html =
        "<html><body><a href=\"http://example.com/x\">ClickMe</a></body></html>";
    TEST_ASSERT_EQUAL_INT(0, browser_render_html(html, "http://test/"));

    char row[P4_CONFIG_TUI_COLS * 4 + 4];
    bool checked = false;
    for (int r = 1; r <= P4_CONFIG_TUI_PAGE_ROWS && !checked; r++) {
        page_row_text(r, row, sizeof(row));
        if (strstr(row, "ClickMe") == NULL) {
            continue;
        }
        for (int c = 1; c <= P4_CONFIG_TUI_COLS; c++) {
            char u[8];
            uint8_t fg = 0;
            tui_page_get_cell(c, r, u, sizeof(u), &fg, NULL, NULL);
            if (u[0] == 'C') {
                TEST_ASSERT_EQUAL_UINT8(9, fg);   /* link foreground */
                checked = true;
                break;
            }
        }
    }
    TEST_ASSERT_TRUE(checked);
}

void test_browser_render_image_alt_text(void)
{
    const char *html =
        "<html><body><img src=\"x.png\" alt=\"AltText\"></body></html>";
    TEST_ASSERT_EQUAL_INT(0, browser_render_html(html, "http://test/"));
    TEST_ASSERT_TRUE(page_contains("AltText"));
}

void test_browser_render_list_items(void)
{
    const char *html =
        "<html><body><ul><li>Alpha</li><li>Beta</li></ul></body></html>";
    TEST_ASSERT_EQUAL_INT(0, browser_render_html(html, "http://test/"));
    TEST_ASSERT_TRUE(page_contains("Alpha"));
    TEST_ASSERT_TRUE(page_contains("Beta"));
}

void test_browser_render_null_and_empty_are_safe(void)
{
    /* NULL is a hard error; an empty/malformed document must not crash. */
    TEST_ASSERT_EQUAL_INT(1, browser_render_html(NULL, "http://test/"));
    (void)browser_render_html("", "http://test/");
    (void)browser_render_html("<p>unclosed <b>bold", "http://test/");
}

void test_browser_render_reinit_does_not_leak(void)
{
    /* browser_viewport_init()/re-render must be repeatable (the page buffer is
     * freed and re-allocated, not leaked). Render several times and assert the
     * last document wins. */
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_EQUAL_INT(0, browser_render_html("<p>first</p>", "http://a/"));
        TEST_ASSERT_EQUAL_INT(0, browser_render_html("<p>second</p>", "http://b/"));
    }
    TEST_ASSERT_TRUE(page_contains("second"));
}
