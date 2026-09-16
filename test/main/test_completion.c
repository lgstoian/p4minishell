/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_completion.c
 * @brief Unit tests for the shell tab-completion and inline ghost providers.
 *
 * Covers command_complete_line() and command_ghost_line() from
 * components/command/command.c. Both derive their candidate set from the
 * shell help table (single source of truth), so these tests are fixed to
 * that table instead of a hard-coded command list: the first help entry's
 * name is used to exercise the first-token path. Path/app completion needs
 * an SD card and simply yields nothing here.
 */

#include "unity.h"
#include "command.h"
#include "shell.h"
#include <stdio.h>
#include <string.h>

void test_completion_empty_line(void)
{
    char out[128];

    out[0] = '\0';
    TEST_ASSERT_EQUAL_INT(0, command_complete_line("", 0, out, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(0, command_complete_line(NULL, 0, out, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(0, command_ghost_line("", out, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(0, command_ghost_line("   ", out, sizeof(out)));
}

void test_completion_first_token_includes_help_names(void)
{
    const char *name = NULL;
    char prefix[8];
    char out[128];
    int total;
    int found = 0;

    TEST_ASSERT_TRUE(shell_help_entry_count() > 0);
    TEST_ASSERT_TRUE(shell_help_entry_get(0, &name, NULL));
    TEST_ASSERT_NOT_NULL(name);

    snprintf(prefix, sizeof(prefix), "%.2s", name);
    total = command_complete_line(prefix, 0, out, sizeof(out));
    TEST_ASSERT_TRUE(total >= 1);

    for (int i = 0; i < total; i++) {
        command_complete_line(prefix, i, out, sizeof(out));
        if (strcasecmp(out, name) == 0) {
            found = 1;
            break;
        }
    }
    TEST_ASSERT_TRUE(found);
}

void test_completion_match_index_bounds(void)
{
    char out[128] = "keep";

    /* An exact match still counts; an out-of-range index leaves out intact. */
    TEST_ASSERT_TRUE(command_complete_line("echo", 999, out, sizeof(out)) >= 1);
    TEST_ASSERT_EQUAL_STRING("keep", out);
}

void test_ghost_first_token_prefix(void)
{
    const char *name = NULL;
    char prefix[8];
    char out[128];

    TEST_ASSERT_TRUE(shell_help_entry_get(0, &name, NULL));
    snprintf(prefix, sizeof(prefix), "%.2s", name);

    TEST_ASSERT_TRUE(command_ghost_line(prefix, out, sizeof(out)) >= 1);
    TEST_ASSERT_EQUAL_INT(0, strncasecmp(out, prefix, 2));
}

void test_ghost_usage_flag(void)
{
    char out[128];

    /* The `history` usage string exposes /save, /load, /search, /clear. */
    TEST_ASSERT_TRUE(command_ghost_line("history /s", out, sizeof(out)) >= 1);
    TEST_ASSERT_EQUAL_INT(0, strncasecmp(out, "/s", 2));
}
