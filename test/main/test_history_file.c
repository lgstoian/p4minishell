/**
 * @file test_history_file.c
 * @brief Unit tests for the history file format round-trip.
 *
 * Covers shell_history_save_lines() / shell_history_load_lines() from
 * components/shell/shell.c (extracted from the `history /save|/load`
 * command in v0.35.5; previously inline and SD-only). The helpers take a
 * plain FILE*, so tmpfile() exercises the format headless: one command
 * per line, blanks skipped, NULL entries skipped.
 */

#include "unity.h"
#include "shell.h"
#include <stdio.h>
#include <string.h>

void test_history_file_roundtrip(void)
{
    FILE *fp = tmpfile();

    if (fp == NULL) {
        TEST_IGNORE_MESSAGE("tmpfile unavailable on this target");
    }
    shell_history_clear();
    shell_store_command_history("HISTF_ALPHA");
    shell_store_command_history("HISTF_BETA");
    shell_store_command_history("HISTF_GAMMA");

    TEST_ASSERT_TRUE(shell_history_save_lines(fp));
    shell_history_clear();
    TEST_ASSERT_EQUAL_size_t(0, shell_history_get_count());

    rewind(fp);
    TEST_ASSERT_EQUAL_size_t(3, shell_history_load_lines(fp));
    TEST_ASSERT_EQUAL_size_t(3, shell_history_get_count());
    TEST_ASSERT_EQUAL_STRING("HISTF_ALPHA", shell_history_get(0));
    TEST_ASSERT_EQUAL_STRING("HISTF_GAMMA", shell_history_get(2));
    shell_history_clear();
    fclose(fp);
}

void test_history_file_skips_blanks(void)
{
    FILE *fp = tmpfile();

    if (fp == NULL) {
        TEST_IGNORE_MESSAGE("tmpfile unavailable on this target");
    }
    fputs("HISTF_ONE\n\n   \nHISTF_TWO\n", fp);

    shell_history_clear();
    rewind(fp);
    TEST_ASSERT_EQUAL_size_t(2, shell_history_load_lines(fp));
    TEST_ASSERT_EQUAL_STRING("HISTF_ONE", shell_history_get(0));
    TEST_ASSERT_EQUAL_STRING("HISTF_TWO", shell_history_get(1));
    shell_history_clear();
    fclose(fp);
}

void test_history_file_null_stream(void)
{
    TEST_ASSERT_FALSE(shell_history_save_lines(NULL));
    TEST_ASSERT_EQUAL_size_t(0, shell_history_load_lines(NULL));
}
