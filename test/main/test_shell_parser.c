/**
 * @file test_shell_parser.c
 * @brief Unit tests for the shell parser utilities.
 *
 * Tests shell_trim(), shell_split_args(), shell_text_equals_ignore_case(),
 * and shell_parse_percentage_arg() from components/shell/shell.c.
 */

#include "unity.h"
#include "shell.h"
#include <string.h>

/* ========================================================================
 * SHELL TRIM TESTS
 * ======================================================================== */

void test_shell_parser_trim(void)
{
    char buf[64];

    /* Leading whitespace */
    strcpy(buf, "  hello");
    TEST_ASSERT_EQUAL_STRING("hello", shell_trim(buf));

    /* Trailing whitespace */
    strcpy(buf, "hello  ");
    TEST_ASSERT_EQUAL_STRING("hello", shell_trim(buf));

    /* Both */
    strcpy(buf, "  hello  ");
    TEST_ASSERT_EQUAL_STRING("hello", shell_trim(buf));

    /* Only whitespace */
    strcpy(buf, "   ");
    TEST_ASSERT_EQUAL_STRING("", shell_trim(buf));

    /* Empty string */
    strcpy(buf, "");
    TEST_ASSERT_EQUAL_STRING("", shell_trim(buf));

    /* No whitespace */
    strcpy(buf, "hello");
    TEST_ASSERT_EQUAL_STRING("hello", shell_trim(buf));

    /* NULL safety */
    TEST_ASSERT_NULL(shell_trim(NULL));
}

/* ========================================================================
 * SHELL SPLIT ARGS TESTS
 * ======================================================================== */

void test_shell_parser_split_args(void)
{
    char buf[128];
    char *argv[8];
    int argc;

    /* Simple command */
    strcpy(buf, "help");
    argc = shell_split_args(buf, argv, 8);
    TEST_ASSERT_EQUAL(1, argc);
    TEST_ASSERT_EQUAL_STRING("help", argv[0]);

    /* Multiple args */
    strcpy(buf, "wifi connect myssid mypass");
    argc = shell_split_args(buf, argv, 8);
    TEST_ASSERT_EQUAL(4, argc);
    TEST_ASSERT_EQUAL_STRING("wifi", argv[0]);
    TEST_ASSERT_EQUAL_STRING("connect", argv[1]);
    TEST_ASSERT_EQUAL_STRING("myssid", argv[2]);
    TEST_ASSERT_EQUAL_STRING("mypass", argv[3]);

    /* Quoted argument */
    strcpy(buf, "echo \"hello world\"");
    argc = shell_split_args(buf, argv, 8);
    TEST_ASSERT_EQUAL(2, argc);
    TEST_ASSERT_EQUAL_STRING("echo", argv[0]);
    TEST_ASSERT_EQUAL_STRING("hello world", argv[1]);

    /* Leading whitespace */
    strcpy(buf, "   help");
    argc = shell_split_args(buf, argv, 8);
    TEST_ASSERT_EQUAL(1, argc);
    TEST_ASSERT_EQUAL_STRING("help", argv[0]);

    /* Empty string */
    strcpy(buf, "");
    argc = shell_split_args(buf, argv, 8);
    TEST_ASSERT_EQUAL(0, argc);

    /* NULL safety */
    argc = shell_split_args(NULL, argv, 8);
    TEST_ASSERT_EQUAL(0, argc);

    /* Max args limit */
    strcpy(buf, "a b c d e f g h i j");
    argc = shell_split_args(buf, argv, 5);
    TEST_ASSERT_EQUAL(5, argc);
}

/* ========================================================================
 * SHELL COUNT ARGS TESTS (argument-truncation detection)
 * ======================================================================== */

void test_shell_parser_count_args(void)
{
    /* Matches shell_split_args() counting without mutating the input. */
    TEST_ASSERT_EQUAL(0, shell_count_args(""));
    TEST_ASSERT_EQUAL(0, shell_count_args("   "));
    TEST_ASSERT_EQUAL(0, shell_count_args(NULL));

    TEST_ASSERT_EQUAL(1, shell_count_args("help"));
    TEST_ASSERT_EQUAL(1, shell_count_args("   help   "));
    TEST_ASSERT_EQUAL(4, shell_count_args("wifi connect myssid mypass"));
    TEST_ASSERT_EQUAL(2, shell_count_args("echo \"hello world\""));
    TEST_ASSERT_EQUAL(4, shell_count_args("echo a 'b c' d"));

    /* Far beyond the dispatcher argv capacity (a full 4096-byte echo). */
    TEST_ASSERT(shell_count_args("echo a b c d e f g h i j k l m n o p q r s t u v w x y z 1 2 3 4 5 6 7 8 9") > 32);

    /* The input is never mutated by counting. */
    {
        char buf[64];
        strcpy(buf, "echo a   b");
        TEST_ASSERT_EQUAL(3, shell_count_args(buf));
        TEST_ASSERT_EQUAL_STRING("echo a   b", buf);
    }
}

/* ========================================================================
 * SHELL TEXT EQUALS TESTS
 * ======================================================================== */

void test_shell_parser_text_equals(void)
{
    /* Case insensitive match */
    TEST_ASSERT_TRUE(shell_text_equals_ignore_case("help", "HELP"));
    TEST_ASSERT_TRUE(shell_text_equals_ignore_case("HELP", "help"));
    TEST_ASSERT_TRUE(shell_text_equals_ignore_case("Help", "hElP"));
    TEST_ASSERT_TRUE(shell_text_equals_ignore_case("wifi", "WIFI"));

    /* Exact match */
    TEST_ASSERT_TRUE(shell_text_equals_ignore_case("hello", "hello"));

    /* Mismatch */
    TEST_ASSERT_FALSE(shell_text_equals_ignore_case("help", "halp"));
    TEST_ASSERT_FALSE(shell_text_equals_ignore_case("wifi", "wifi "));
    TEST_ASSERT_FALSE(shell_text_equals_ignore_case("ab", "abc"));

    /* NULL safety */
    TEST_ASSERT_FALSE(shell_text_equals_ignore_case(NULL, "help"));
    TEST_ASSERT_FALSE(shell_text_equals_ignore_case("help", NULL));
    TEST_ASSERT_FALSE(shell_text_equals_ignore_case(NULL, NULL));
}

/* ========================================================================
 * SHELL PARSE PERCENTAGE TESTS
 * ======================================================================== */

void test_shell_parser_percentage_parse(void)
{
    int result;

    /* Valid values */
    TEST_ASSERT_TRUE(shell_parse_percentage_arg("0", &result));
    TEST_ASSERT_EQUAL(0, result);

    TEST_ASSERT_TRUE(shell_parse_percentage_arg("50", &result));
    TEST_ASSERT_EQUAL(50, result);

    TEST_ASSERT_TRUE(shell_parse_percentage_arg("100", &result));
    TEST_ASSERT_EQUAL(100, result);

    /* Invalid values */
    TEST_ASSERT_FALSE(shell_parse_percentage_arg("-1", &result));
    TEST_ASSERT_FALSE(shell_parse_percentage_arg("101", &result));
    TEST_ASSERT_FALSE(shell_parse_percentage_arg("abc", &result));
    TEST_ASSERT_FALSE(shell_parse_percentage_arg("", &result));

    /* NULL safety */
    TEST_ASSERT_FALSE(shell_parse_percentage_arg(NULL, &result));
    TEST_ASSERT_FALSE(shell_parse_percentage_arg("50", NULL));
}
