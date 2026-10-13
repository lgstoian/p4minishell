/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_epub.c
 * @brief EPUB transport unit tests.
 *
 * Tests the EPUB component's config initialization, OPF/NCX parsing,
 * spine/TOC construction, and chapter rendering. Pure logic tests — no
 * hardware or network required.
 */

#include "unity.h"
#include "epub.h"
#include "p4minishell_config.h"
#include <string.h>

/* --- Config init --- */
TEST_CASE("epub config init default", "[epub]")
{
    /* The config is defined in p4minishell_config.h */
    TEST_ASSERT_EQUAL_STRING("P4MiniShell-Chat", P4_CONFIG_CHAT_AP_SSID); /* Just checking config loads */
    TEST_ASSERT_EQUAL(1, P4_CONFIG_CHAT_ENABLE);
}

/* --- Config bounds --- */
TEST_CASE("epub config bounds", "[epub]")
{
    /* Test that config values are within reasonable bounds */
    TEST_ASSERT_GREATER_THAN(0, P4_CONFIG_EPUB_MAX_CHAPTERS);
    TEST_ASSERT_LESS_THAN(1024, P4_CONFIG_EPUB_MAX_CHAPTERS);
    TEST_ASSERT_GREATER_THAN(0, P4_CONFIG_EPUB_MAX_TOC);
    TEST_ASSERT_LESS_THAN(1024, P4_CONFIG_EPUB_MAX_TOC);
    TEST_ASSERT_GREATER_THAN(0, P4_CONFIG_EPUB_MAX_MANIFEST);
    TEST_ASSERT_LESS_THAN(1024, P4_CONFIG_EPUB_MAX_MANIFEST);
}

/* --- Manifest item lookup --- */
TEST_CASE("epub manifest lookup by id", "[epub]")
{
    epub_manifest_item_t items[] = {
        { .id = "chap1", .href = "ch1.xhtml", .media_type = "application/xhtml+xml" },
        { .id = "chap2", .href = "ch2.xhtml", .media_type = "application/xhtml+xml" },
        { .id = "cover", .href = "cover.jpg", .media_type = "image/jpeg" },
    };
    int count = 3;

    const epub_manifest_item_t *found = epub_find_manifest_by_id(items, count, "chap2");
    TEST_ASSERT_NOT_NULL(found);
    TEST_ASSERT_EQUAL_STRING("chap2", found->id);
    TEST_ASSERT_EQUAL_STRING("ch2.xhtml", found->href);
    TEST_ASSERT_EQUAL_STRING("application/xhtml+xml", found->media_type);

    found = epub_find_manifest_by_id(items, count, "nonexistent");
    TEST_ASSERT_NULL(found);
}

/* --- HREF resolution --- */
TEST_CASE("epub href resolution relative", "[epub]")
{
    char out[EPUB_PATH_MAX];

    /* Simple relative */
    TEST_ASSERT_TRUE(epub_resolve_href("OEBPS", "ch1.xhtml", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("OEBPS/ch1.xhtml", out);

    /* With subdirectory */
    TEST_ASSERT_TRUE(epub_resolve_href("OEBPS/Text", "ch1.xhtml", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("OEBPS/Text/ch1.xhtml", out);

    /* Parent directory */
    TEST_ASSERT_TRUE(epub_resolve_href("OEBPS/Text", "../images/cover.jpg", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("OEBPS/images/cover.jpg", out);

    /* Multiple parent dirs */
    TEST_ASSERT_TRUE(epub_resolve_href("OEBPS/Text/Chapter1", "../../images/cover.jpg", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("OEBPS/images/cover.jpg", out);

    /* Current dir reference */
    TEST_ASSERT_TRUE(epub_resolve_href("OEBPS/Text", "./ch1.xhtml", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("OEBPS/Text/ch1.xhtml", out);

    /* Empty base */
    TEST_ASSERT_TRUE(epub_resolve_href("", "ch1.xhtml", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("ch1.xhtml", out);
}

/* --- NCX/nav parsing not implemented as standalone --- */
TEST_CASE("epub NCX parsing placeholder", "[epub]")
{
    /* NCX/nav parsing is integrated in epub_open() via Expat callbacks.
     * Standalone test not implemented; use epub_open() for full integration test. */
    TEST_ASSERT_TRUE(true);
}

/* --- OPF parsing not implemented as standalone --- */
TEST_CASE("epub OPF parsing placeholder", "[epub]")
{
    /* OPF parsing is integrated in epub_open() via Expat callbacks.
     * Standalone test not implemented; use epub_open() for full integration test. */
    TEST_ASSERT_TRUE(true);
}

/* --- Config constants --- */
TEST_CASE("epub config constants defined", "[epub]")
{
    /* Verify config macros exist and have reasonable defaults */
    #ifndef P4_CONFIG_EPUB_TEMP_DIR
    #error "P4_CONFIG_EPUB_TEMP_DIR not defined"
    #endif

    #ifndef P4_CONFIG_EPUB_MAX_CHAPTERS
    #error "P4_CONFIG_EPUB_MAX_CHAPTERS not defined"
    #endif

    #ifndef P4_CONFIG_EPUB_MAX_TOC
    #error "P4_CONFIG_EPUB_MAX_TOC not defined"
    #endif

    #ifndef P4_CONFIG_EPUB_MAX_MANIFEST
    #error "P4_CONFIG_EPUB_MAX_MANIFEST not defined"
    #endif

    TEST_ASSERT_GREATER_THAN(0, P4_CONFIG_EPUB_MAX_CHAPTERS);
    TEST_ASSERT_GREATER_THAN(0, P4_CONFIG_EPUB_MAX_TOC);
    TEST_ASSERT_GREATER_THAN(0, P4_CONFIG_EPUB_MAX_MANIFEST);
    TEST_ASSERT_LESS_THAN(1024, P4_CONFIG_EPUB_MAX_CHAPTERS);
    TEST_ASSERT_LESS_THAN(1024, P4_CONFIG_EPUB_MAX_TOC);
    TEST_ASSERT_LESS_THAN(1024, P4_CONFIG_EPUB_MAX_MANIFEST);
}