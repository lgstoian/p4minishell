/**
 * @file test_history_search.c
 * @brief Unit tests for the reverse-history search matching helper.
 *
 * Covers shell_history_search_matches() from components/shell/shell.c, the
 * case-insensitive substring filter shared by the Ctrl+R reverse search and
 * the batch-testable `history /search <text>` verb. The interactive state
 * machine around it (query editing, cycling matches) runs only with an LVGL
 * input line and is exercised by tools/completion_test.py on hardware.
 */

#include "unity.h"
#include "shell.h"

void test_history_search_matches_basic(void)
{
    TEST_ASSERT_TRUE(shell_history_search_matches("git commit -m x", "commit"));
    TEST_ASSERT_TRUE(shell_history_search_matches("GIT COMMIT", "commit"));
    TEST_ASSERT_TRUE(shell_history_search_matches("git log", "git"));
    TEST_ASSERT_FALSE(shell_history_search_matches("git log", "push"));

    /* An empty query matches every entry. */
    TEST_ASSERT_TRUE(shell_history_search_matches("anything", ""));
}

void test_history_search_matches_null(void)
{
    TEST_ASSERT_FALSE(shell_history_search_matches(NULL, "x"));
    TEST_ASSERT_FALSE(shell_history_search_matches("x", NULL));
}

void test_history_generation_advances(void)
{
    size_t before = shell_history_generation();

    shell_history_clear();
    shell_store_command_history("HISTGEN_ALPHA");
    TEST_ASSERT_TRUE(shell_history_generation() > before);
    shell_history_clear();
}
