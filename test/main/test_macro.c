/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_macro.c
 * @brief Unit tests for the macro recorder (components/batch/batch.c).
 *
 * Covers batch_macro_record_line/status/text/discard plus the
 * `macro record/stop/play/status` verb state machine: line capture,
 * `macro` control-line and blank exclusion, buffer overflow auto-stop,
 * and idle error paths. File writing (`macro stop` onto SD) and replay
 * (`macro play`) need the card and stay hardware-verified.
 *
 * RAM-only paths; each test discards the recorder state it uses. The
 * `macro` verb's print helpers are headless-safe (same as the `calc`
 * command-assignment test).
 */

#include "unity.h"
#include "batch.h"

#include <stdio.h>
#include <string.h>

static void macro_cmd(int argc, char **argv)
{
    shell_command_macro(argc, argv);
}

void test_macro_idle_paths(void)
{
    char *status_argv[] = {"macro", "status"};
    char *stop_argv[] = {"macro", "stop"};
    char *play_argv[] = {"macro", "play", "nope.bat"};
    char *bogus_argv[] = {"macro", "bogus"};
    batch_macro_status_t st;

    batch_macro_discard();
    /* Idle verbs print and change nothing (no SD, no crash). */
    macro_cmd(2, status_argv);
    macro_cmd(2, stop_argv);
    macro_cmd(3, play_argv);
    macro_cmd(2, bogus_argv);
    macro_cmd(1, status_argv);
    batch_macro_status(&st);
    TEST_ASSERT_FALSE(st.active);
    TEST_ASSERT_FALSE(st.overflowed);
    TEST_ASSERT_EQUAL_STRING("", batch_macro_text());
    batch_macro_discard();
}

void test_macro_record_and_exclude(void)
{
    char *record_argv[] = {"macro", "record"};
    batch_macro_status_t st;

    batch_macro_discard();
    macro_cmd(2, record_argv);
    batch_macro_status(&st);
    TEST_ASSERT_TRUE(st.active);
    TEST_ASSERT_FALSE(st.overflowed);
    TEST_ASSERT_TRUE(st.capacity > 0);
    TEST_ASSERT_TRUE(strstr(st.file, "MACRO.BAT") != NULL);

    batch_macro_record_line("dir /b");
    batch_macro_record_line("   ");
    batch_macro_record_line("");
    batch_macro_record_line("macro stop");
    batch_macro_record_line("  macro play x.bat  ");
    batch_macro_record_line("echo macro stop");
    batch_macro_record_line("calc 1+1");
    TEST_ASSERT_EQUAL_STRING("dir /b\necho macro stop\ncalc 1+1\n",
                             batch_macro_text());
    batch_macro_status(&st);
    TEST_ASSERT_TRUE(st.active);
    TEST_ASSERT_EQUAL_UINT(strlen("dir /b\necho macro stop\ncalc 1+1\n"),
                           (unsigned)st.used);
    batch_macro_discard();
    TEST_ASSERT_EQUAL_STRING("", batch_macro_text());
}

void test_macro_record_overflow(void)
{
    char *record_argv[] = {"macro", "record"};
    batch_macro_status_t st;
    char line[32];
    int i;

    batch_macro_discard();
    macro_cmd(2, record_argv);
    batch_macro_status(&st);
    /* Each line is at least 12 bytes, so looping up to the buffer capacity
     * guarantees the recorder auto-stops on overflow; the loop is bounded by
     * the capacity so it can never run away. */
    for (i = 0; i < (int)st.capacity && st.active; i++) {
        snprintf(line, sizeof(line), "echo line %d", i);
        batch_macro_record_line(line);
        batch_macro_status(&st);
    }
    batch_macro_status(&st);
    TEST_ASSERT_FALSE(st.active);
    TEST_ASSERT_TRUE(st.overflowed);
    TEST_ASSERT_TRUE(st.used > 0);
    TEST_ASSERT_TRUE(st.used < st.capacity);
    TEST_ASSERT_TRUE(strlen(batch_macro_text()) == st.used);
    /* Further lines are ignored once auto-stopped. */
    batch_macro_record_line("echo after");
    TEST_ASSERT_TRUE(strlen(batch_macro_text()) == st.used);
    batch_macro_discard();
}

void test_macro_double_record(void)
{
    char *record_argv[] = {"macro", "record", "a.bat"};
    batch_macro_status_t st;

    batch_macro_discard();
    macro_cmd(3, record_argv);
    macro_cmd(3, record_argv); /* second start refused, first survives */
    batch_macro_status(&st);
    TEST_ASSERT_TRUE(st.active);
    TEST_ASSERT_TRUE(strstr(st.file, "a.bat") != NULL);
    batch_macro_discard();
}
