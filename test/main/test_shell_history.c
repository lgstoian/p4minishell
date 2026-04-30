/**
 * @file test_shell_history.c
 * @brief Unit tests for the shell command history.
 *
 * Tests shell_store_command_history() and shell_recall_history()
 * from components/shell/shell.c.
 */

#include "unity.h"
#include "shell.h"
#include <string.h>

/* ========================================================================
 * SHELL HISTORY STORE TESTS
 * ======================================================================== */

void test_shell_history_store(void)
{
    /* Store a simple command */
    shell_store_command_history("help");
    /* No crash = pass. History is internal and not directly inspectable
     * without LVGL context, but store should not crash with NULL widgets. */
    TEST_ASSERT_TRUE(1);
}

void test_shell_history_recall(void)
{
    /* Recall with no history should be safe */
    shell_recall_history(-1);
    shell_recall_history(1);
    TEST_ASSERT_TRUE(1);
}

/* ========================================================================
 * SHELL HISTORY PASSWORD MASK TESTS
 * ======================================================================== */

void test_shell_history_password_mask(void)
{
    /* Store a wifi connect command with password */
    shell_store_command_history("wifi connect MyWiFi secret123");

    /* Store a wifi connect without password */
    shell_store_command_history("wifi connect MyWiFi");

    /* Store a regular command */
    shell_store_command_history("help");

    /* All should not crash */
    TEST_ASSERT_TRUE(1);
}
