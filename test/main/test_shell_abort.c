/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_shell_abort.c
 * @brief Unit tests for the foreground-break flag and worker-busy state.
 *
 * Covers shell_request_abort() / shell_abort_requested() /
 * shell_clear_abort() and shell_set_command_busy() /
 * shell_is_command_busy() from components/shell/shell.c. The batch
 * checkpoints that consume the flag unwind on the worker and stay
 * hardware-verified.
 *
 * Pure state logic with no hardware dependency. Leaves both flags cleared
 * so later suites observe the boot default.
 */

#include "unity.h"
#include "shell.h"

void test_shell_abort_initially_clear(void)
{
    shell_clear_abort();
    TEST_ASSERT_FALSE(shell_abort_requested());
}

void test_shell_abort_request_and_clear(void)
{
    shell_clear_abort();
    shell_request_abort();
    TEST_ASSERT_TRUE(shell_abort_requested());
    /* Requests are idempotent: repeated taps change nothing. */
    shell_request_abort();
    TEST_ASSERT_TRUE(shell_abort_requested());
    shell_clear_abort();
    TEST_ASSERT_FALSE(shell_abort_requested());
    /* Clearing twice is safe. */
    shell_clear_abort();
    TEST_ASSERT_FALSE(shell_abort_requested());
}

void test_shell_command_busy_tracks(void)
{
    shell_set_command_busy(false);
    TEST_ASSERT_FALSE(shell_is_command_busy());
    shell_set_command_busy(true);
    TEST_ASSERT_TRUE(shell_is_command_busy());
    shell_set_command_busy(false);
    TEST_ASSERT_FALSE(shell_is_command_busy());
}
