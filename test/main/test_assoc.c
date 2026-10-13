/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_assoc.c
 * @brief Unit tests for the DOS-shaped file-association helpers.
 *
 * Covers the pure pieces behind `assoc`/`ftype`:
 * `command_assoc_normalize_ext()` and `command_assoc_split_arg()` from
 * components/command/command.c (declared in command.h). ASSOC.INI I/O and
 * `open` dispatch need the SD card and are hardware-verified through s06.
 */

#include "unity.h"
#include "command.h"

#include <string.h>

void test_assoc_normalize_ext(void)
{
    char out[24];

    TEST_ASSERT_TRUE(command_assoc_normalize_ext(".txt", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING(".txt", out);
    TEST_ASSERT_TRUE(command_assoc_normalize_ext("TXT", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING(".txt", out);
    TEST_ASSERT_TRUE(command_assoc_normalize_ext(".MD", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING(".md", out);
    TEST_ASSERT_TRUE(command_assoc_normalize_ext("c++", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING(".c++", out);
    TEST_ASSERT_TRUE(command_assoc_normalize_ext("tar.gz", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING(".tar.gz", out);
    /* Rejects: empty, bare dot, bad charset, NULLs. */
    TEST_ASSERT_FALSE(command_assoc_normalize_ext("", out, sizeof(out)));
    TEST_ASSERT_FALSE(command_assoc_normalize_ext(".", out, sizeof(out)));
    TEST_ASSERT_FALSE(command_assoc_normalize_ext(".t xt", out, sizeof(out)));
    TEST_ASSERT_FALSE(command_assoc_normalize_ext(".t;t", out, sizeof(out)));
    TEST_ASSERT_FALSE(command_assoc_normalize_ext(NULL, out, sizeof(out)));
    TEST_ASSERT_FALSE(command_assoc_normalize_ext(".txt", NULL, 0));
}

void test_assoc_normalize_ext_bounds(void)
{
    char tiny[4];
    char out[24];

    /* ".ab" (3 chars + NUL) fits tiny[4]; ".abc" does not. */
    TEST_ASSERT_TRUE(command_assoc_normalize_ext(".ab", tiny, sizeof(tiny)));
    TEST_ASSERT_EQUAL_STRING(".ab", tiny);
    TEST_ASSERT_FALSE(command_assoc_normalize_ext(".abc", tiny, sizeof(tiny)));
    /* Always NUL-terminated on success. */
    memset(out, 'Z', sizeof(out));
    TEST_ASSERT_TRUE(command_assoc_normalize_ext("md", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING(".md", out);
}

void test_assoc_split_arg(void)
{
    char ext[24];
    char app[64];

    memset(app, 0, sizeof(app));
    TEST_ASSERT_EQUAL_INT(ASSOC_ARG_SHOW,
                          command_assoc_split_arg(".txt", ext, sizeof(ext),
                                                  app, sizeof(app)));
    TEST_ASSERT_EQUAL_STRING(".txt", ext);
    TEST_ASSERT_EQUAL_INT(ASSOC_ARG_CLEAR,
                          command_assoc_split_arg(".txt=", ext, sizeof(ext),
                                                  app, sizeof(app)));
    TEST_ASSERT_EQUAL_STRING(".txt", ext);
    TEST_ASSERT_EQUAL_INT(ASSOC_ARG_SET,
                          command_assoc_split_arg(".txt=NOTES", ext, sizeof(ext),
                                                  app, sizeof(app)));
    TEST_ASSERT_EQUAL_STRING(".txt", ext);
    TEST_ASSERT_EQUAL_STRING("NOTES", app);
    /* Case folds the extension, keeps the app name verbatim. */
    TEST_ASSERT_EQUAL_INT(ASSOC_ARG_SET,
                          command_assoc_split_arg("MD=Writer", ext, sizeof(ext),
                                                  app, sizeof(app)));
    TEST_ASSERT_EQUAL_STRING(".md", ext);
    TEST_ASSERT_EQUAL_STRING("Writer", app);
    /* Rejects. */
    TEST_ASSERT_EQUAL_INT(ASSOC_ARG_BAD,
                          command_assoc_split_arg("", ext, sizeof(ext),
                                                  app, sizeof(app)));
    TEST_ASSERT_EQUAL_INT(ASSOC_ARG_BAD,
                          command_assoc_split_arg("no-dot-here!", ext, sizeof(ext),
                                                  app, sizeof(app)));
    TEST_ASSERT_EQUAL_INT(ASSOC_ARG_BAD,
                          command_assoc_split_arg("=NOTES", ext, sizeof(ext),
                                                  app, sizeof(app)));
    TEST_ASSERT_EQUAL_INT(ASSOC_ARG_BAD,
                          command_assoc_split_arg(NULL, ext, sizeof(ext),
                                                  app, sizeof(app)));
}
