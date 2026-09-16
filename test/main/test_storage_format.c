/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_storage_format.c
 * @brief Unit tests for storage formatting and matching helpers.
 *
 * Covers shell_sd_format_size() and shell_wildcard_match() from
 * components/storage/storage.c. Both are pure text/number transforms with no
 * hardware dependency, and both are load-bearing for the Phase 2 storage
 * features: every capacity report goes through the size formatter, and every
 * `dir` pattern filter goes through the wildcard matcher.
 */

#include "unity.h"
#include "storage.h"
#include <string.h>

/* ========================================================================
 * HUMAN-READABLE SIZE FORMATTING
 * ======================================================================== */

void test_storage_format_size(void)
{
    char buf[32];

    /* Below 1 KiB reports raw bytes with no decimal point. */
    shell_sd_format_size(0, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("0 B", buf);

    shell_sd_format_size(1, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("1 B", buf);

    shell_sd_format_size(1023, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("1023 B", buf);

    /* Exactly 1 KiB rolls over to the next unit. */
    shell_sd_format_size(1024, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("1.0 KiB", buf);

    shell_sd_format_size(1536, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("1.5 KiB", buf);

    /* MiB and GiB steps. */
    shell_sd_format_size(1024ULL * 1024ULL, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("1.0 MiB", buf);

    shell_sd_format_size(1024ULL * 1024ULL * 1024ULL, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("1.0 GiB", buf);

    /* A realistic card capacity stays in GiB rather than overflowing. */
    shell_sd_format_size(32ULL * 1024ULL * 1024ULL * 1024ULL, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("32.0 GiB", buf);

    /* GiB is the largest unit, so very large values keep scaling in GiB
     * instead of wrapping or printing an empty unit. */
    shell_sd_format_size(2048ULL * 1024ULL * 1024ULL * 1024ULL, buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "GiB"));

    /* NULL and zero-length outputs must not write anything. */
    shell_sd_format_size(1024, NULL, 0);
    buf[0] = 'x';
    shell_sd_format_size(1024, buf, 0);
    TEST_ASSERT_EQUAL('x', buf[0]);
}

/* ========================================================================
 * DOS WILDCARD MATCHING
 * ======================================================================== */

void test_storage_wildcard_match(void)
{
    /* Exact and case-insensitive matches, as FAT itself behaves. */
    TEST_ASSERT_TRUE(shell_wildcard_match("readme.txt", "readme.txt"));
    TEST_ASSERT_TRUE(shell_wildcard_match("README.TXT", "readme.txt"));
    TEST_ASSERT_TRUE(shell_wildcard_match("ReadMe.Txt", "README.txt"));
    TEST_ASSERT_FALSE(shell_wildcard_match("readme.txt", "readme.doc"));

    /* '*' spans any run, including an empty one. */
    TEST_ASSERT_TRUE(shell_wildcard_match("*", "anything.bin"));
    TEST_ASSERT_TRUE(shell_wildcard_match("*.txt", "notes.txt"));
    TEST_ASSERT_TRUE(shell_wildcard_match("*.txt", ".txt"));
    TEST_ASSERT_TRUE(shell_wildcard_match("log*", "log"));
    TEST_ASSERT_TRUE(shell_wildcard_match("log*.txt", "log2026.txt"));
    TEST_ASSERT_FALSE(shell_wildcard_match("*.txt", "notes.txtx"));
    TEST_ASSERT_FALSE(shell_wildcard_match("*.txt", "notes.doc"));

    /* '?' matches exactly one character and never zero. */
    TEST_ASSERT_TRUE(shell_wildcard_match("?.txt", "a.txt"));
    TEST_ASSERT_FALSE(shell_wildcard_match("?.txt", ".txt"));
    TEST_ASSERT_FALSE(shell_wildcard_match("?.txt", "ab.txt"));
    TEST_ASSERT_TRUE(shell_wildcard_match("log??.txt", "log01.txt"));

    /* Combined wildcards. */
    TEST_ASSERT_TRUE(shell_wildcard_match("*.?", "file.c"));
    TEST_ASSERT_TRUE(shell_wildcard_match("a*b?c", "axxxbzc"));
    TEST_ASSERT_FALSE(shell_wildcard_match("a*b?c", "axxxbzzc"));

    /* Multiple stars must not cause a false positive or infinite recursion. */
    TEST_ASSERT_TRUE(shell_wildcard_match("*a*b*", "xxaxxbxx"));
    TEST_ASSERT_FALSE(shell_wildcard_match("*a*b*", "xxbxxaxx"));

    /* Empty pattern matches only the empty name. */
    TEST_ASSERT_TRUE(shell_wildcard_match("", ""));
    TEST_ASSERT_FALSE(shell_wildcard_match("", "x"));

    /* NULL safety. */
    TEST_ASSERT_FALSE(shell_wildcard_match(NULL, "x"));
    TEST_ASSERT_FALSE(shell_wildcard_match("*", NULL));
    TEST_ASSERT_FALSE(shell_wildcard_match(NULL, NULL));
}

/* ========================================================================
 * PATH IDENTITY (self-copy guardrail)
 * ======================================================================== */

void test_storage_paths_are_same(void)
{
    /* Identical resolved paths are the same file. */
    TEST_ASSERT_TRUE(storage_paths_are_same("/sdcard/a.txt", "/sdcard/a.txt"));

    /* FAT is case-insensitive, so a case difference is still the same file.
     * This is what stops `copy A.TXT a.txt` truncating the source. */
    TEST_ASSERT_TRUE(storage_paths_are_same("/sdcard/A.TXT", "/sdcard/a.txt"));

    /* Genuinely different paths. */
    TEST_ASSERT_FALSE(storage_paths_are_same("/sdcard/a.txt", "/sdcard/b.txt"));
    TEST_ASSERT_FALSE(storage_paths_are_same("/sdcard/a.txt", "/sdcard/sub/a.txt"));

    /* NULL safety. */
    TEST_ASSERT_FALSE(storage_paths_are_same(NULL, "/sdcard/a.txt"));
    TEST_ASSERT_FALSE(storage_paths_are_same("/sdcard/a.txt", NULL));
    TEST_ASSERT_FALSE(storage_paths_are_same(NULL, NULL));
}

/* ========================================================================
 * PATH HELPERS USED BY THE NEW OPTIONS
 * ======================================================================== */

void test_storage_path_helpers(void)
{
    /* Directory-component detection accepts both separators. */
    TEST_ASSERT_TRUE(shell_path_has_directory_component("logs/today.txt"));
    TEST_ASSERT_TRUE(shell_path_has_directory_component("logs\\today.txt"));
    TEST_ASSERT_FALSE(shell_path_has_directory_component("today.txt"));
    TEST_ASSERT_FALSE(shell_path_has_directory_component(NULL));

    /* Extension matching is case-insensitive, as batch lookup relies on. */
    TEST_ASSERT_TRUE(shell_path_has_extension("run.bat", ".bat"));
    TEST_ASSERT_TRUE(shell_path_has_extension("RUN.BAT", ".bat"));
    TEST_ASSERT_FALSE(shell_path_has_extension("run.txt", ".bat"));

    /* A name shorter than the extension cannot match. */
    TEST_ASSERT_FALSE(shell_path_has_extension("a", ".bat"));
    TEST_ASSERT_FALSE(shell_path_has_extension(NULL, ".bat"));
    TEST_ASSERT_FALSE(shell_path_has_extension("run.bat", NULL));
}
