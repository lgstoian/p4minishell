/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_shell_pipeline.c
 * @brief Unit tests for the shell command pipeline: pipe detection,
 *        command chaining, and the agreement between the tokenizer and
 *        the pipe/chain scanners.
 *
 * These are pure text transforms and run without hardware. The functions
 * under test live in components/shell/shell.c and are exercised through
 * their public API.
 */

#include "unity.h"
#include "shell.h"
#include "p4minishell_config.h"
#include <string.h>

/* ========================================================================
 * PIPE DETECTION AGREEMENT
 * ========================================================================
 * shell_command_has_pipe() and shell_split_args() share the same quote/escape
 * scanner, so they can never disagree about whether a `|` is syntax or data.
 * Verify that agreement across every quoting form.
 */

void test_pipe_detection_agreement(void)
{
    char buf[192];
    char *argv[8];
    int argc;

    /* A bare pipe is detected as a pipeline. */
    TEST_ASSERT_TRUE(shell_has_unquoted_char("type f.txt | sort", '|'));

    /* A pipe inside double quotes is data, not a pipeline. */
    strcpy(buf, "echo \"a | b\"");
    TEST_ASSERT_FALSE(shell_has_unquoted_char(buf, '|'));
    argc = shell_split_args(buf, argv, 8);
    TEST_ASSERT_EQUAL(2, argc);
    TEST_ASSERT_EQUAL_STRING("echo", argv[0]);
    TEST_ASSERT_EQUAL_STRING("a | b", argv[1]);

    /* A pipe inside single quotes is data. */
    strcpy(buf, "echo 'a | b'");
    TEST_ASSERT_FALSE(shell_has_unquoted_char(buf, '|'));
    argc = shell_split_args(buf, argv, 8);
    TEST_ASSERT_EQUAL(2, argc);
    TEST_ASSERT_EQUAL_STRING("a | b", argv[1]);

    /* A single caret escapes the pipe; a doubled caret is a literal caret,
     * so the pipe that follows it is real syntax. */
    strcpy(buf, "echo a^|b");
    TEST_ASSERT_FALSE(shell_has_unquoted_char(buf, '|'));
    argc = shell_split_args(buf, argv, 8);
    TEST_ASSERT_EQUAL(2, argc);
    TEST_ASSERT_EQUAL_STRING("a|b", argv[1]);

    /* A doubled caret yields a literal caret; the pipe after it is real. */
    strcpy(buf, "echo a^^|b");
    TEST_ASSERT_TRUE(shell_has_unquoted_char(buf, '|'));

    /* A pipe after a closed quote is real syntax. */
    strcpy(buf, "echo \"hello\" | sort");
    TEST_ASSERT_TRUE(shell_has_unquoted_char(buf, '|'));

    /* Multiple pipe characters, all quoted: still data. */
    strcpy(buf, "echo \"a | b | c\"");
    TEST_ASSERT_FALSE(shell_has_unquoted_char(buf, '|'));

    /* Mixed: one quoted pipe and one real pipe. The real one is detected. */
    strcpy(buf, "echo \"a | b\" | sort");
    TEST_ASSERT_TRUE(shell_has_unquoted_char(buf, '|'));

    /* NULL safety. */
    TEST_ASSERT_FALSE(shell_has_unquoted_char(NULL, '|'));
}

/* ========================================================================
 * PIPE DETECTION VS REDIRECTION
 * ========================================================================
 * The redirection scanner looks for `>` and `<`; the pipe scanner looks for
 * `|`. They share the quote/escape scanner, so quoting that hides one hides
 * all. Verify that redirection operators are also quote-aware.
 */

void test_redirection_quote_awareness(void)
{
    char buf[192];
    char *argv[8];
    int argc;

    /* A `>` inside double quotes is data, not redirection. */
    strcpy(buf, "echo \"a > b\"");
    argc = shell_split_args(buf, argv, 8);
    TEST_ASSERT_EQUAL(2, argc);
    TEST_ASSERT_EQUAL_STRING("a > b", argv[1]);

    /* A `>` inside single quotes is data. */
    strcpy(buf, "echo 'a > b'");
    argc = shell_split_args(buf, argv, 8);
    TEST_ASSERT_EQUAL(2, argc);
    TEST_ASSERT_EQUAL_STRING("a > b", argv[1]);

    /* A caret-escaped `>` is data. */
    strcpy(buf, "echo a^>b");
    argc = shell_split_args(buf, argv, 8);
    TEST_ASSERT_EQUAL(2, argc);
    TEST_ASSERT_EQUAL_STRING("a>b", argv[1]);

    /* A `<` inside quotes is data. */
    strcpy(buf, "echo \"a < b\"");
    argc = shell_split_args(buf, argv, 8);
    TEST_ASSERT_EQUAL(2, argc);
    TEST_ASSERT_EQUAL_STRING("a < b", argv[1]);

    /* A `>>` inside quotes is data. */
    strcpy(buf, "echo \"a >> b\"");
    argc = shell_split_args(buf, argv, 8);
    TEST_ASSERT_EQUAL(2, argc);
    TEST_ASSERT_EQUAL_STRING("a >> b", argv[1]);
}

/* ========================================================================
 * CHAIN SPLIT: EDGE CASES BEYOND THE BASIC SUITE
 * ========================================================================
 * test_shell_quoting.c covers the basic chain-split behavior. Here we cover
 * the cases that matter most for security and correctness: the pipe-versus-
 * chain distinction under every quoting form, and single-quote protection.
 */

void test_chain_pipe_vs_chain_under_quotes(void)
{
    shell_chain_segment_t segments[8];
    char buf[192];
    bool truncated = false;
    int count;

    /* A pipe inside double quotes does NOT split the chain. */
    strcpy(buf, "echo \"a | b\" && echo done");
    count = shell_split_chain(buf, segments, 8, &truncated);
    TEST_ASSERT_EQUAL(2, count);
    TEST_ASSERT_EQUAL(SHELL_CHAIN_ON_SUCCESS, segments[1].op);
    TEST_ASSERT_EQUAL_STRING("echo \"a | b\"", segments[0].command);

    /* A pipe inside single quotes does NOT split the chain. */
    strcpy(buf, "echo 'a | b' || echo done");
    count = shell_split_chain(buf, segments, 8, &truncated);
    TEST_ASSERT_EQUAL(2, count);
    TEST_ASSERT_EQUAL(SHELL_CHAIN_ON_FAILURE, segments[1].op);
    TEST_ASSERT_EQUAL_STRING("echo 'a | b'", segments[0].command);

    /* A caret-escaped pipe does NOT split the chain. */
    strcpy(buf, "echo a^|b && echo done");
    count = shell_split_chain(buf, segments, 8, &truncated);
    TEST_ASSERT_EQUAL(2, count);
    TEST_ASSERT_EQUAL_STRING("echo a^|b", segments[0].command);
}

void test_chain_single_quote_protection(void)
{
    shell_chain_segment_t segments[8];
    char buf[192];
    bool truncated = false;
    int count;

    /* Separators inside single quotes are data, not chain links. */
    strcpy(buf, "echo 'a & b' && echo done");
    count = shell_split_chain(buf, segments, 8, &truncated);
    TEST_ASSERT_EQUAL(2, count);
    TEST_ASSERT_EQUAL_STRING("echo 'a & b'", segments[0].command);

    strcpy(buf, "echo 'a && b' || echo done");
    count = shell_split_chain(buf, segments, 8, &truncated);
    TEST_ASSERT_EQUAL(2, count);
    TEST_ASSERT_EQUAL_STRING("echo 'a && b'", segments[0].command);

    strcpy(buf, "echo 'a || b' & echo done");
    count = shell_split_chain(buf, segments, 8, &truncated);
    TEST_ASSERT_EQUAL(2, count);
    TEST_ASSERT_EQUAL_STRING("echo 'a || b'", segments[0].command);
}

void test_chain_truncation_with_pipes(void)
{
    shell_chain_segment_t segments[4];
    char buf[256];
    bool truncated = false;
    int count;

    /* A chain of pipelines is still subject to the segment limit. */
    strcpy(buf, "type a.txt | sort && type b.txt | sort && type c.txt | sort");
    count = shell_split_chain(buf, segments, 2, &truncated);
    TEST_ASSERT_EQUAL(2, count);
    TEST_ASSERT_TRUE(truncated);

    /* Without truncation, all three links are produced. (The chain splitter
     * modifies its input in place, so restore the buffer first.) */
    strcpy(buf, "type a.txt | sort && type b.txt | sort && type c.txt | sort");
    count = shell_split_chain(buf, segments, 8, &truncated);
    TEST_ASSERT_EQUAL(3, count);
    TEST_ASSERT_FALSE(truncated);
    TEST_ASSERT_EQUAL(SHELL_CHAIN_ON_SUCCESS, segments[1].op);
    TEST_ASSERT_EQUAL(SHELL_CHAIN_ON_SUCCESS, segments[2].op);
}

void test_chain_empty_and_whitespace(void)
{
    shell_chain_segment_t segments[8];
    char buf[128];
    bool truncated = false;
    int count;

    /* A line that is only whitespace yields no segments. */
    strcpy(buf, "   ");
    count = shell_split_chain(buf, segments, 8, &truncated);
    TEST_ASSERT_EQUAL(0, count);

    /* A leading separator still produces a FIRST segment first. */
    strcpy(buf, "& echo done");
    count = shell_split_chain(buf, segments, 8, &truncated);
    TEST_ASSERT_EQUAL(2, count);
    TEST_ASSERT_EQUAL(SHELL_CHAIN_FIRST, segments[0].op);
    TEST_ASSERT_EQUAL(SHELL_CHAIN_ALWAYS, segments[1].op);
}

/* ========================================================================
 * UNQUOTED-CHAR SCANNER: PIPE-SPECIFIC CASES
 * ========================================================================
 * The scanner backs both pipe detection and chain splitting. Verify the
 * cases that are unique to pipe handling.
 */

void test_find_unquoted_pipe(void)
{
    /* No pipe at all. */
    TEST_ASSERT_NULL(shell_find_unquoted_char("dir", '|'));

    /* A bare pipe is found. */
    char buf[64];
    strcpy(buf, "a | b");
    char *found = shell_find_unquoted_char(buf, '|');
    TEST_ASSERT_NOT_NULL(found);
    TEST_ASSERT_EQUAL('|', *found);

    /* A pipe hidden by double quotes is not found. */
    TEST_ASSERT_NULL(shell_find_unquoted_char("echo \"|\"", '|'));

    /* A pipe hidden by single quotes is not found. */
    TEST_ASSERT_NULL(shell_find_unquoted_char("echo '|'", '|'));

    /* A pipe hidden by caret escape is not found. */
    TEST_ASSERT_NULL(shell_find_unquoted_char("echo a^|b", '|'));

    /* A pipe after a closed quote run is found. */
    strcpy(buf, "echo \"a\" | sort");
    found = shell_find_unquoted_char(buf, '|');
    TEST_ASSERT_NOT_NULL(found);
    TEST_ASSERT_EQUAL('|', *found);
}
