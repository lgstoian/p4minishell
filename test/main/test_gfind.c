/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_gfind.c
 * @brief Unit tests for the pure `gfind /files` filter (gfind_commands.c).
 *
 * Covers gfind_name_allowed(): hidden-entry skipping, the store-directory
 * skip list, explicit `/ext:` matching, and the default text-bearing
 * filetype rule. The tree walk itself is hardware-verified through the data
 * suite.
 */

#include "unity.h"
#include "command.h"
#include "p4minishell_config.h"

#include <stdbool.h>
#include <string.h>

void test_gfind_hidden(void)
{
    /* Dot entries are skipped by default, kept with include_hidden. */
    TEST_ASSERT_FALSE(gfind_name_allowed(".trash", true, false, NULL));
    TEST_ASSERT_FALSE(gfind_name_allowed(".hidden.txt", false, false, ".txt"));
    TEST_ASSERT_TRUE(gfind_name_allowed(".hidden.txt", false, true, ".txt"));
    TEST_ASSERT_TRUE(gfind_name_allowed("visible.txt", false, false, ".txt"));
}

void test_gfind_store_dirs(void)
{
    /* The structured stores are never descended (their own scopes cover them). */
    TEST_ASSERT_FALSE(gfind_name_allowed("DBS", true, false, NULL));
    TEST_ASSERT_FALSE(gfind_name_allowed("dbs", true, false, NULL));
    TEST_ASSERT_FALSE(gfind_name_allowed("ALARMS", true, false, NULL));
    /* Other directories pass; a directory named like a file kind still passes
     * (the file rules apply to files only). */
    TEST_ASSERT_TRUE(gfind_name_allowed("docs", true, false, NULL));
    TEST_ASSERT_TRUE(gfind_name_allowed("APPS", true, false, NULL));
    TEST_ASSERT_TRUE(gfind_name_allowed("notes.txt", true, false, NULL));

    /* A file named DBS is NOT skipped by the directory rule. */
    TEST_ASSERT_FALSE(gfind_name_allowed("DBS", false, false, NULL)); /* unknown kind */
}

void test_gfind_ext_list(void)
{
    /* Explicit list: dots optional, separators , ; space. */
    TEST_ASSERT_TRUE(gfind_name_allowed("readme.txt", false, false, ".txt,.md"));
    TEST_ASSERT_TRUE(gfind_name_allowed("readme.TXT", false, false, "txt;md"));
    TEST_ASSERT_TRUE(gfind_name_allowed("notes.md", false, false, ".txt .md"));
    TEST_ASSERT_FALSE(gfind_name_allowed("image.bmp", false, false, ".txt,.md"));
    TEST_ASSERT_FALSE(gfind_name_allowed("noext", false, false, ".txt"));
    /* A list entry without a leading dot still matches a dotted file, and the
     * comparison is extension-anchored (not a suffix of a longer name). */
    TEST_ASSERT_FALSE(gfind_name_allowed("notxt", false, false, ".txt"));
    TEST_ASSERT_TRUE(gfind_name_allowed("a.json", false, false, "json"));
}

void test_gfind_default_kinds(void)
{
    /* No explicit list: text-bearing kinds kept, images/unknown skipped. */
    TEST_ASSERT_TRUE(gfind_name_allowed("a.txt", false, false, NULL));
    TEST_ASSERT_TRUE(gfind_name_allowed("a.log", false, false, NULL));
    TEST_ASSERT_TRUE(gfind_name_allowed("a.ini", false, false, NULL));
    TEST_ASSERT_TRUE(gfind_name_allowed("a.md", false, false, NULL));
    TEST_ASSERT_TRUE(gfind_name_allowed("a.json", false, false, NULL));
    TEST_ASSERT_TRUE(gfind_name_allowed("a.bat", false, false, NULL));
    TEST_ASSERT_FALSE(gfind_name_allowed("a.bmp", false, false, NULL));
    TEST_ASSERT_FALSE(gfind_name_allowed("a.bin", false, false, NULL));
    TEST_ASSERT_FALSE(gfind_name_allowed("noext", false, false, NULL));
}

void test_gfind_null_safe(void)
{
    TEST_ASSERT_FALSE(gfind_name_allowed(NULL, false, false, NULL));
    TEST_ASSERT_FALSE(gfind_name_allowed("", false, false, NULL));
    TEST_ASSERT_FALSE(gfind_name_allowed(NULL, true, true, ".txt"));
}
