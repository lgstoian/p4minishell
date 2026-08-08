/**
 * @file test_shell_quoting.c
 * @brief Unit tests for shell quoting, escaping, and command chaining.
 *
 * Covers the shared quote/escape scanner in components/shell/shell.c:
 * shell_find_unquoted_char(), shell_has_unquoted_char(),
 * shell_unescape_in_place(), the quoting-aware shell_split_args(), and
 * shell_split_chain().
 *
 * These are pure text transforms and run without hardware.
 */

#include "unity.h"
#include "shell.h"
#include <string.h>

/* ========================================================================
 * UNQUOTED OPERATOR SCANNING
 * ======================================================================== */

void test_shell_quoting_find_unquoted(void)
{
    /* A bare operator is found. */
    TEST_ASSERT_NOT_NULL(shell_find_unquoted_char("a | b", '|'));
    TEST_ASSERT_TRUE(shell_has_unquoted_char("dir > out.txt", '>'));

    /* Double quotes hide it. */
    TEST_ASSERT_NULL(shell_find_unquoted_char("echo \"a | b\"", '|'));
    TEST_ASSERT_FALSE(shell_has_unquoted_char("echo \"a > b\"", '>'));

    /* Single quotes hide it. */
    TEST_ASSERT_NULL(shell_find_unquoted_char("echo 'a | b'", '|'));
    TEST_ASSERT_FALSE(shell_has_unquoted_char("echo 'a & b'", '&'));

    /* A caret escape hides it. */
    TEST_ASSERT_NULL(shell_find_unquoted_char("echo a^|b", '|'));
    TEST_ASSERT_FALSE(shell_has_unquoted_char("echo a^&b", '&'));
    TEST_ASSERT_FALSE(shell_has_unquoted_char("echo a^>b", '>'));

    /* An operator after a closed quote is still syntax. */
    TEST_ASSERT_NOT_NULL(shell_find_unquoted_char("echo \"a\" | sort", '|'));

    /* An escaped caret does not escape the character after it, so the
     * operator that follows ^^ is real syntax. */
    TEST_ASSERT_NOT_NULL(shell_find_unquoted_char("echo a^^|b", '|'));

    /* Multi-character target sets. */
    TEST_ASSERT_NOT_NULL(shell_find_unquoted_any("a < b", "><"));
    TEST_ASSERT_NULL(shell_find_unquoted_any("echo \"a < b\"", "><"));

    /* NULL safety. */
    TEST_ASSERT_NULL(shell_find_unquoted_char(NULL, '|'));
    TEST_ASSERT_NULL(shell_find_unquoted_any("abc", NULL));
    TEST_ASSERT_FALSE(shell_has_unquoted_char(NULL, '|'));
}

/* ========================================================================
 * MARKUP REMOVAL
 * ======================================================================== */

void test_shell_quoting_unescape(void)
{
    char buf[128];

    /* Double quotes are removed, contents kept. */
    strcpy(buf, "\"hello world\"");
    TEST_ASSERT_EQUAL_STRING("hello world", shell_unescape_in_place(buf));

    /* Single quotes are removed, contents kept. */
    strcpy(buf, "'hello world'");
    TEST_ASSERT_EQUAL_STRING("hello world", shell_unescape_in_place(buf));

    /* A caret escape yields the literal payload. */
    strcpy(buf, "a^|b");
    TEST_ASSERT_EQUAL_STRING("a|b", shell_unescape_in_place(buf));

    strcpy(buf, "a^ b");
    TEST_ASSERT_EQUAL_STRING("a b", shell_unescape_in_place(buf));

    /* ^^ collapses to a single caret. */
    strcpy(buf, "a^^b");
    TEST_ASSERT_EQUAL_STRING("a^b", shell_unescape_in_place(buf));

    /* A caret can escape a quote so it stays data. */
    strcpy(buf, "say ^\"hi^\"");
    TEST_ASSERT_EQUAL_STRING("say \"hi\"", shell_unescape_in_place(buf));

    /* Inside single quotes a caret is literal, not an escape. */
    strcpy(buf, "'a^b'");
    TEST_ASSERT_EQUAL_STRING("a^b", shell_unescape_in_place(buf));

    /* Quotes of the other kind survive inside a quoted run. */
    strcpy(buf, "\"it's here\"");
    TEST_ASSERT_EQUAL_STRING("it's here", shell_unescape_in_place(buf));

    /* Text without markup is unchanged. */
    strcpy(buf, "plain");
    TEST_ASSERT_EQUAL_STRING("plain", shell_unescape_in_place(buf));

    TEST_ASSERT_NULL(shell_unescape_in_place(NULL));
}

/* ========================================================================
 * TOKENIZER WITH QUOTES AND ESCAPES
 * ======================================================================== */

void test_shell_quoting_split_args(void)
{
    char buf[160];
    char *argv[8];
    int argc;

    /* Double-quoted argument keeps its spaces and loses its quotes. */
    strcpy(buf, "echo \"hello world\"");
    argc = shell_split_args(buf, argv, 8);
    TEST_ASSERT_EQUAL(2, argc);
    TEST_ASSERT_EQUAL_STRING("echo", argv[0]);
    TEST_ASSERT_EQUAL_STRING("hello world", argv[1]);

    /* Single-quoted argument behaves the same for grouping. */
    strcpy(buf, "echo 'hello world'");
    argc = shell_split_args(buf, argv, 8);
    TEST_ASSERT_EQUAL(2, argc);
    TEST_ASSERT_EQUAL_STRING("hello world", argv[1]);

    /* A caret-escaped space joins two words into one argument. */
    strcpy(buf, "echo hello^ world");
    argc = shell_split_args(buf, argv, 8);
    TEST_ASSERT_EQUAL(2, argc);
    TEST_ASSERT_EQUAL_STRING("hello world", argv[1]);

    /* Quotes in the middle of a word still group. */
    strcpy(buf, "copy pre\"fix mid\"post dest");
    argc = shell_split_args(buf, argv, 8);
    TEST_ASSERT_EQUAL(3, argc);
    TEST_ASSERT_EQUAL_STRING("prefix midpost", argv[1]);
    TEST_ASSERT_EQUAL_STRING("dest", argv[2]);

    /* An escaped quote is data, not a delimiter. */
    strcpy(buf, "echo ^\"quoted^\"");
    argc = shell_split_args(buf, argv, 8);
    TEST_ASSERT_EQUAL(2, argc);
    TEST_ASSERT_EQUAL_STRING("\"quoted\"", argv[1]);

    /* Single quotes protect a double quote and vice versa. */
    strcpy(buf, "echo 'say \"hi\"'");
    argc = shell_split_args(buf, argv, 8);
    TEST_ASSERT_EQUAL(2, argc);
    TEST_ASSERT_EQUAL_STRING("say \"hi\"", argv[1]);

    /* Plain arguments still behave exactly as before. */
    strcpy(buf, "wifi connect myssid mypass");
    argc = shell_split_args(buf, argv, 8);
    TEST_ASSERT_EQUAL(4, argc);
    TEST_ASSERT_EQUAL_STRING("wifi", argv[0]);
    TEST_ASSERT_EQUAL_STRING("mypass", argv[3]);
}

/* ========================================================================
 * COMMAND CHAINING
 * ======================================================================== */

void test_shell_chain_split(void)
{
    shell_chain_segment_t segments[8];
    char buf[192];
    bool truncated = false;
    int count;

    /* No separator yields exactly one FIRST segment. */
    strcpy(buf, "dir");
    count = shell_split_chain(buf, segments, 8, &truncated);
    TEST_ASSERT_EQUAL(1, count);
    TEST_ASSERT_EQUAL_STRING("dir", segments[0].command);
    TEST_ASSERT_EQUAL(SHELL_CHAIN_FIRST, segments[0].op);
    TEST_ASSERT_FALSE(truncated);

    /* Unconditional chaining with '&'. */
    strcpy(buf, "echo one & echo two");
    count = shell_split_chain(buf, segments, 8, &truncated);
    TEST_ASSERT_EQUAL(2, count);
    TEST_ASSERT_EQUAL_STRING("echo one", segments[0].command);
    TEST_ASSERT_EQUAL_STRING("echo two", segments[1].command);
    TEST_ASSERT_EQUAL(SHELL_CHAIN_ALWAYS, segments[1].op);

    /* Conditional on success with '&&'. */
    strcpy(buf, "cd logs && dir");
    count = shell_split_chain(buf, segments, 8, &truncated);
    TEST_ASSERT_EQUAL(2, count);
    TEST_ASSERT_EQUAL_STRING("cd logs", segments[0].command);
    TEST_ASSERT_EQUAL(SHELL_CHAIN_ON_SUCCESS, segments[1].op);

    /* Conditional on failure with '||'. */
    strcpy(buf, "type missing.txt || echo not found");
    count = shell_split_chain(buf, segments, 8, &truncated);
    TEST_ASSERT_EQUAL(2, count);
    TEST_ASSERT_EQUAL(SHELL_CHAIN_ON_FAILURE, segments[1].op);
    TEST_ASSERT_EQUAL_STRING("echo not found", segments[1].command);

    /* Mixed operators in one line. */
    strcpy(buf, "a && b || c & d");
    count = shell_split_chain(buf, segments, 8, &truncated);
    TEST_ASSERT_EQUAL(4, count);
    TEST_ASSERT_EQUAL(SHELL_CHAIN_FIRST, segments[0].op);
    TEST_ASSERT_EQUAL(SHELL_CHAIN_ON_SUCCESS, segments[1].op);
    TEST_ASSERT_EQUAL(SHELL_CHAIN_ON_FAILURE, segments[2].op);
    TEST_ASSERT_EQUAL(SHELL_CHAIN_ALWAYS, segments[3].op);

    /* A single '|' is the pipe operator and must NOT split the chain, so the
     * pipeline reaches the pipe executor intact. */
    strcpy(buf, "type f.txt | sort");
    count = shell_split_chain(buf, segments, 8, &truncated);
    TEST_ASSERT_EQUAL(1, count);
    TEST_ASSERT_EQUAL_STRING("type f.txt | sort", segments[0].command);

    /* A pipeline on one side of a chain separator stays intact. */
    strcpy(buf, "type f.txt | sort && echo done");
    count = shell_split_chain(buf, segments, 8, &truncated);
    TEST_ASSERT_EQUAL(2, count);
    TEST_ASSERT_EQUAL_STRING("type f.txt | sort", segments[0].command);
    TEST_ASSERT_EQUAL(SHELL_CHAIN_ON_SUCCESS, segments[1].op);

    /* Quoted and escaped separators are data. */
    strcpy(buf, "echo \"a && b\"");
    count = shell_split_chain(buf, segments, 8, &truncated);
    TEST_ASSERT_EQUAL(1, count);
    TEST_ASSERT_EQUAL_STRING("echo \"a && b\"", segments[0].command);

    strcpy(buf, "echo a^&b");
    count = shell_split_chain(buf, segments, 8, &truncated);
    TEST_ASSERT_EQUAL(1, count);
    TEST_ASSERT_EQUAL_STRING("echo a^&b", segments[0].command);

    /* Truncation is reported rather than silently dropping commands. */
    strcpy(buf, "a & b & c & d");
    count = shell_split_chain(buf, segments, 2, &truncated);
    TEST_ASSERT_EQUAL(2, count);
    TEST_ASSERT_TRUE(truncated);

    /* NULL safety. */
    TEST_ASSERT_EQUAL(0, shell_split_chain(NULL, segments, 8, NULL));
    strcpy(buf, "dir");
    TEST_ASSERT_EQUAL(0, shell_split_chain(buf, NULL, 8, NULL));
    TEST_ASSERT_EQUAL(0, shell_split_chain(buf, segments, 0, NULL));
}
