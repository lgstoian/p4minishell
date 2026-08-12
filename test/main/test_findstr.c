/**
 * @file test_findstr.c
 * @brief Unit tests for the pure findstr matcher helpers.
 *
 * Covers shell_fsre_search() and shell_findstr_match_line() from
 * components/storage/storage_commands.c. Both are pure text functions with no
 * hardware or filesystem dependency, and both are load-bearing for the
 * classic DOS `findstr` text-search command added in v0.24.27.
 */

#include "unity.h"
#include "storage_commands.h"
#include <stdbool.h>
#include <string.h>

/* ========================================================================
 * REGEX ENGINE (shell_fsre_search)
 * ======================================================================== */

void test_findstr_regex_literal(void)
{
    int end = 0;

    TEST_ASSERT_EQUAL_INT(2, shell_fsre_search("abc", "xxabcxx", false, &end));
    TEST_ASSERT_EQUAL_INT(5, end);

    TEST_ASSERT_EQUAL_INT(0, shell_fsre_search("abc", "abc", false, &end));
    TEST_ASSERT_EQUAL_INT(3, end);

    TEST_ASSERT_EQUAL_INT(-1, shell_fsre_search("abc", "abx", false, &end));
    TEST_ASSERT_EQUAL_INT(-1, shell_fsre_search("xyz", "abc", false, &end));

    /* Empty pattern matches at position 0. */
    TEST_ASSERT_EQUAL_INT(0, shell_fsre_search("", "anything", false, &end));
}

void test_findstr_regex_case(void)
{
    int end = 0;

    /* Case-sensitive by default: no match on a different case. */
    TEST_ASSERT_EQUAL_INT(-1, shell_fsre_search("ABC", "xxabcxx", false, &end));
    /* With /I the comparison is case-insensitive. */
    TEST_ASSERT_EQUAL_INT(2, shell_fsre_search("ABC", "xxabcxx", true, &end));
    TEST_ASSERT_EQUAL_INT(0, shell_fsre_search("aBc", "AbC", true, &end));
}

void test_findstr_regex_anchors(void)
{
    int end = 0;

    /* ^ only matches at the start of the line. */
    TEST_ASSERT_EQUAL_INT(0, shell_fsre_search("^abc", "abcd", false, &end));
    TEST_ASSERT_EQUAL_INT(4, end);
    TEST_ASSERT_EQUAL_INT(-1, shell_fsre_search("^abc", "xabcd", false, &end));

    /* $ only matches at the end of the line. */
    TEST_ASSERT_EQUAL_INT(2, shell_fsre_search("abc$", "xxabc", false, &end));
    TEST_ASSERT_EQUAL_INT(5, end);
    TEST_ASSERT_EQUAL_INT(-1, shell_fsre_search("abc$", "xxabcd", false, &end));

    /* Both ends. */
    TEST_ASSERT_EQUAL_INT(0, shell_fsre_search("^abc$", "abc", false, &end));
    TEST_ASSERT_EQUAL_INT(3, end);
    TEST_ASSERT_EQUAL_INT(-1, shell_fsre_search("^abc$", "xabc", false, &end));
}

void test_findstr_regex_dot_and_star(void)
{
    int end = 0;

    /* . matches any single character. */
    TEST_ASSERT_EQUAL_INT(0, shell_fsre_search("a.c", "axc", false, &end));
    TEST_ASSERT_EQUAL_INT(0, shell_fsre_search("a.c", "a.c", false, &end));
    TEST_ASSERT_EQUAL_INT(-1, shell_fsre_search("a.c", "ac", false, &end));

    /* * matches zero or more of the preceding atom. */
    TEST_ASSERT_EQUAL_INT(0, shell_fsre_search("ab*c", "ac", false, &end));
    TEST_ASSERT_EQUAL_INT(0, shell_fsre_search("ab*c", "abc", false, &end));
    TEST_ASSERT_EQUAL_INT(0, shell_fsre_search("ab*c", "abbbc", false, &end));
    TEST_ASSERT_EQUAL_INT(-1, shell_fsre_search("ab*c", "axc", false, &end));

    /* .* spans any run. */
    TEST_ASSERT_EQUAL_INT(0, shell_fsre_search("a.*c", "aXYZc", false, &end));
}

void test_findstr_regex_class(void)
{
    int end = 0;

    TEST_ASSERT_EQUAL_INT(0, shell_fsre_search("[ab]c", "bc", false, &end));
    TEST_ASSERT_EQUAL_INT(-1, shell_fsre_search("[ab]c", "xc", false, &end));

    /* Ranges. */
    TEST_ASSERT_EQUAL_INT(0, shell_fsre_search("[a-c]x", "bx", false, &end));
    TEST_ASSERT_EQUAL_INT(-1, shell_fsre_search("[a-c]x", "dx", false, &end));

    /* Negated class. */
    TEST_ASSERT_EQUAL_INT(0, shell_fsre_search("[^0-9]", "a", false, &end));
    TEST_ASSERT_EQUAL_INT(-1, shell_fsre_search("[^0-9]", "5", false, &end));
}

void test_findstr_regex_escapes(void)
{
    int end = 0;

    /* \c makes a metacharacter literal. */
    TEST_ASSERT_EQUAL_INT(0, shell_fsre_search("a\\*b", "a*b", false, &end));
    TEST_ASSERT_EQUAL_INT(-1, shell_fsre_search("a\\*b", "axb", false, &end));
    TEST_ASSERT_EQUAL_INT(0, shell_fsre_search("a\\.b", "a.b", false, &end));

    /* \< and \> are word boundaries. */
    TEST_ASSERT_EQUAL_INT(4, shell_fsre_search("\\<cat\\>", "the cat sat", false, &end));
    TEST_ASSERT_EQUAL_INT(0, shell_fsre_search("\\<cat\\>", "cat", false, &end));
    TEST_ASSERT_EQUAL_INT(-1, shell_fsre_search("\\<cat\\>", "concatenate", false, &end));
    TEST_ASSERT_EQUAL_INT(0, shell_fsre_search("\\<cat", "catalog", false, &end));
}

/* ========================================================================
 * LINE MATCHER (shell_findstr_match_line)
 * ======================================================================== */

void test_findstr_match_literal(void)
{
    TEST_ASSERT_TRUE(shell_findstr_match_line("abc", false, "xxabcxx", false, false, false, false));
    TEST_ASSERT_FALSE(shell_findstr_match_line("abc", false, "xxabx", false, false, false, false));

    /* Case-sensitive by default, insensitive with /I. */
    TEST_ASSERT_FALSE(shell_findstr_match_line("ABC", false, "xxabcxx", false, false, false, false));
    TEST_ASSERT_TRUE(shell_findstr_match_line("ABC", false, "xxabcxx", true, false, false, false));
}

void test_findstr_match_switches(void)
{
    /* /X whole-line exact. */
    TEST_ASSERT_TRUE(shell_findstr_match_line("abc", false, "abc", false, false, false, true));
    TEST_ASSERT_FALSE(shell_findstr_match_line("abc", false, "xabc", false, false, false, true));
    TEST_ASSERT_FALSE(shell_findstr_match_line("abc", false, "abcx", false, false, false, true));

    /* /B line begins with. */
    TEST_ASSERT_TRUE(shell_findstr_match_line("abc", false, "abcdef", false, true, false, false));
    TEST_ASSERT_FALSE(shell_findstr_match_line("abc", false, "xabc", false, true, false, false));

    /* /E line ends with. */
    TEST_ASSERT_TRUE(shell_findstr_match_line("def", false, "abcdef", false, false, true, false));
    TEST_ASSERT_FALSE(shell_findstr_match_line("def", false, "defx", false, false, true, false));
}

void test_findstr_match_regex(void)
{
    TEST_ASSERT_TRUE(shell_findstr_match_line("a.c", true, "axc", false, false, false, false));
    TEST_ASSERT_FALSE(shell_findstr_match_line("a.c", true, "ac", false, false, false, false));

    TEST_ASSERT_TRUE(shell_findstr_match_line("^start", true, "starting", false, false, false, false));
    TEST_ASSERT_FALSE(shell_findstr_match_line("^start", true, "not starting", false, false, false, false));

    TEST_ASSERT_TRUE(shell_findstr_match_line("end$", true, "the end", false, false, false, false));
    TEST_ASSERT_FALSE(shell_findstr_match_line("end$", true, "the ending", false, false, false, false));

    /* Regex + /X whole line. */
    TEST_ASSERT_TRUE(shell_findstr_match_line("^abc$", true, "abc", false, false, false, true));
    TEST_ASSERT_FALSE(shell_findstr_match_line("^abc$", true, "xabc", false, false, false, true));
}
