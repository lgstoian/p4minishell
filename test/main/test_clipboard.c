/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_clipboard.c
 * @brief Unit tests for the shell-core RAM clipboard.
 *
 * Covers shell_clipboard_set/get/is_file/copy_transcript() from
 * components/shell/shell.c (previously only incidentally exercised via
 * the editor selection tests). Transcript writes land in the RAM buffer
 * headless (NULL widget), so copy_transcript() is testable without LVGL.
 */

#include "unity.h"
#include "shell.h"
#include <string.h>
#include <stdbool.h>

void test_clipboard_set_get(void)
{
    shell_clipboard_set("CLIPBOARD_PROBE_123");
    TEST_ASSERT_EQUAL_STRING("CLIPBOARD_PROBE_123", shell_clipboard_get());
    TEST_ASSERT_FALSE(shell_clipboard_is_file());
}

void test_clipboard_empty_and_null(void)
{
    shell_clipboard_set("");
    TEST_ASSERT_EQUAL_STRING("", shell_clipboard_get());

    shell_clipboard_set(NULL);
    TEST_ASSERT_EQUAL_STRING("", shell_clipboard_get());
}

void test_clipboard_file_flag(void)
{
    shell_clipboard_set("plain text");
    TEST_ASSERT_FALSE(shell_clipboard_is_file());

    shell_clipboard_set_file("sd:/NOTE.TXT");
    TEST_ASSERT_TRUE(shell_clipboard_is_file());
    TEST_ASSERT_EQUAL_STRING("sd:/NOTE.TXT", shell_clipboard_get());

    /* Text stores clear the file flag again. */
    shell_clipboard_set("back to text");
    TEST_ASSERT_FALSE(shell_clipboard_is_file());
}

void test_clipboard_copy_transcript(void)
{
    shell_transcript_append_text("CLIP_MARK_A_1\n");
    shell_transcript_append_text("CLIP_MARK_B_2\n");
    shell_transcript_append_text("CLIP_MARK_C_3\n");

    TEST_ASSERT_TRUE(shell_clipboard_copy_transcript(2));
    TEST_ASSERT_NOT_NULL(strstr(shell_clipboard_get(), "CLIP_MARK_B_2"));
    TEST_ASSERT_NOT_NULL(strstr(shell_clipboard_get(), "CLIP_MARK_C_3"));
    TEST_ASSERT_NULL(strstr(shell_clipboard_get(), "CLIP_MARK_A_1"));

    /* Requesting more lines than exist copies everything available. */
    TEST_ASSERT_TRUE(shell_clipboard_copy_transcript(100000));
    TEST_ASSERT_NOT_NULL(strstr(shell_clipboard_get(), "CLIP_MARK_A_1"));
}
