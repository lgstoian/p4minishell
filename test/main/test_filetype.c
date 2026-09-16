/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_filetype.c
 * @brief Unit tests for components/filetype (central extension registry).
 */

#include "unity.h"
#include "filetype.h"
#include <string.h>

void test_filetype_batch(void)
{
    TEST_ASSERT_EQUAL(FILETYPE_BATCH, filetype_of("FOO.BAT"));
    TEST_ASSERT_EQUAL(FILETYPE_BATCH, filetype_of("sd:/APPS/x.Cmd"));
    TEST_ASSERT_TRUE(filetype_is_executable(FILETYPE_BATCH));
    TEST_ASSERT_EQUAL_STRING("batch", filetype_name(FILETYPE_BATCH));
}

void test_filetype_markdown(void)
{
    TEST_ASSERT_EQUAL(FILETYPE_MARKDOWN, filetype_of("a.md"));
    TEST_ASSERT_EQUAL(FILETYPE_MARKDOWN, filetype_of("a.MARKDOWN"));
    TEST_ASSERT_EQUAL(FILETYPE_MARKDOWN, filetype_of("a.mkd"));
    TEST_ASSERT_TRUE(filetype_is_markdown(FILETYPE_MARKDOWN));
    TEST_ASSERT_FALSE(filetype_is_markdown(FILETYPE_BATCH));
}

void test_filetype_json_text(void)
{
    TEST_ASSERT_EQUAL(FILETYPE_JSON, filetype_of("c.json"));
    TEST_ASSERT_EQUAL(FILETYPE_TEXT, filetype_of("n.txt"));
    TEST_ASSERT_EQUAL(FILETYPE_TEXT, filetype_of("n.LOG"));
    TEST_ASSERT_EQUAL(FILETYPE_TEXT, filetype_of("n.sys"));
    TEST_ASSERT_EQUAL(FILETYPE_TEXT, filetype_of("n.ini"));
    TEST_ASSERT_FALSE(filetype_is_executable(FILETYPE_JSON));
}

void test_filetype_image(void)
{
    TEST_ASSERT_EQUAL(FILETYPE_IMAGE, filetype_of("a.bmp"));
    TEST_ASSERT_EQUAL(FILETYPE_IMAGE, filetype_of("PIC.BMP"));
    TEST_ASSERT_EQUAL(FILETYPE_IMAGE, filetype_of("sd:/DIR.X/pic.dib"));
    TEST_ASSERT_TRUE(filetype_is_image(FILETYPE_IMAGE));
    TEST_ASSERT_FALSE(filetype_is_image(FILETYPE_TEXT));
    TEST_ASSERT_EQUAL_STRING("image", filetype_name(FILETYPE_IMAGE));
}

void test_filetype_unknown(void)
{
    TEST_ASSERT_EQUAL(FILETYPE_UNKNOWN, filetype_of(NULL));
    TEST_ASSERT_EQUAL(FILETYPE_UNKNOWN, filetype_of(""));
    TEST_ASSERT_EQUAL(FILETYPE_UNKNOWN, filetype_of("noext"));
    TEST_ASSERT_EQUAL(FILETYPE_UNKNOWN, filetype_of(".profile"));
    TEST_ASSERT_EQUAL(FILETYPE_UNKNOWN, filetype_of("dir.name/file"));
    TEST_ASSERT_EQUAL(FILETYPE_UNKNOWN, filetype_of("x.xyz"));
    /* Dots in directories must not leak into the basename extension. */
    TEST_ASSERT_EQUAL(FILETYPE_UNKNOWN, filetype_of("sd:/DIR.X/file"));
    TEST_ASSERT_EQUAL(FILETYPE_BATCH, filetype_of("sd:/DIR.X/file.bat"));
    TEST_ASSERT_EQUAL_STRING("unknown", filetype_name(FILETYPE_UNKNOWN));
}

void test_filetype_has_extension(void)
{
    TEST_ASSERT_TRUE(filetype_has_extension("a.BAT", ".bat"));
    TEST_ASSERT_FALSE(filetype_has_extension("a.bat", ".cmd"));
    TEST_ASSERT_FALSE(filetype_has_extension(NULL, ".bat"));
    TEST_ASSERT_FALSE(filetype_has_extension("a", ".bat"));
}
