/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_batch_control.c
 * @brief Unit tests for the pure batch control-flow helpers.
 *
 * Covers the label scanner (batch_label_is_line / batch_label_extract) and the
 * BASIC `on <expr> goto|gosub <targets>` parser (batch_on_parse /
 * batch_on_select) that back `goto`/`gosub`/`return`/`on`. These are the exact
 * helpers the executor uses, exposed for testing (no duplicated logic).
 *
 * Pure string parsing with no hardware dependency.
 */

#include "unity.h"
#include "batch.h"

#include <string.h>

/* ========================================================================
 * LABEL SCANNING
 * ======================================================================== */

void test_batch_label_is_line(void)
{
    TEST_ASSERT_TRUE(batch_label_is_line(":loop"));
    TEST_ASSERT_TRUE(batch_label_is_line("  :loop"));
    TEST_ASSERT_TRUE(batch_label_is_line("\t:loop echo x"));
    TEST_ASSERT_TRUE(batch_label_is_line("::comment"));
    TEST_ASSERT_TRUE(batch_label_is_line(":"));
    TEST_ASSERT_FALSE(batch_label_is_line("echo x"));
    TEST_ASSERT_TRUE(batch_label_is_line(": "));       /* still a label line */
    TEST_ASSERT_FALSE(batch_label_is_line(""));
    TEST_ASSERT_FALSE(batch_label_is_line(NULL));
}

void test_batch_label_extract(void)
{
    char name[64];

    batch_label_extract(":loop", name, sizeof(name));
    TEST_ASSERT_EQUAL_STRING("loop", name);

    batch_label_extract("   :loop arg1 arg2", name, sizeof(name));
    TEST_ASSERT_EQUAL_STRING("loop", name);

    batch_label_extract(":100", name, sizeof(name));
    TEST_ASSERT_EQUAL_STRING("100", name);

    batch_label_extract(":Mixed.Case-2", name, sizeof(name));
    TEST_ASSERT_EQUAL_STRING("Mixed.Case-2", name);

    batch_label_extract("::comment text", name, sizeof(name));
    TEST_ASSERT_EQUAL_STRING(":comment", name);

    /* A label line with only the colon yields an empty name. */
    batch_label_extract(":", name, sizeof(name));
    TEST_ASSERT_EQUAL_STRING("", name);

    /* Over-long names are truncated to the buffer width. */
    batch_label_extract(":abcdefghij", name, 5);
    TEST_ASSERT_EQUAL_STRING("abcd", name);
    TEST_ASSERT_EQUAL_INT(0, name[4]);

    /* NULL inputs never dereference. */
    batch_label_extract(NULL, name, sizeof(name));
    batch_label_extract(":x", NULL, sizeof(name));
    batch_label_extract(":x", name, 0);
}

/* ========================================================================
 * `on <expr> goto|gosub|call <targets>` PARSER
 * ======================================================================== */

static void expect_on(const char *want_expr, bool want_gosub, const char *want_targets,
                      int argc, char *argv[])
{
    char expr[256];
    char targets[256];
    bool is_gosub = false;

    TEST_ASSERT_TRUE(batch_on_parse(argc, argv, expr, sizeof(expr), &is_gosub,
                                    targets, sizeof(targets)));
    TEST_ASSERT_EQUAL_STRING(want_expr, expr);
    TEST_ASSERT_EQUAL_STRING(want_targets, targets);
    TEST_ASSERT_EQUAL_INT(want_gosub ? 1 : 0, is_gosub ? 1 : 0);
}

void test_batch_on_parse(void)
{
    {
        char *argv[] = { "on", "2", "goto", ":a,:b,:c" };
        expect_on("2", false, ":a,:b,:c", 4, argv);
    }
    {
        char *argv[] = { "on", "1", "gosub", ":sub" };
        expect_on("1", true, ":sub", 4, argv);
    }
    {
        char *argv[] = { "on", "1", "call", "a, b" };
        expect_on("1", true, "a, b", 4, argv);
    }
    {
        /* Uppercase keyword, multi-token expression. */
        char *argv[] = { "on", "1", "+", "1", "GOTO", "l1,l2" };
        expect_on("1 + 1", false, "l1,l2", 6, argv);
    }
    {
        char *argv[] = { "on", "%c%", "goto", ":x" };
        expect_on("%c%", false, ":x", 4, argv);
    }

    /* Malformed forms are rejected. */
    {
        char *argv[] = { "on" };
        char expr[16];
        char targets[16];
        bool g = false;
        TEST_ASSERT_FALSE(batch_on_parse(1, argv, expr, sizeof(expr), &g, targets, sizeof(targets)));
    }
    {
        char *argv[] = { "on", "1", "goto" };   /* no targets */
        char expr[16];
        char targets[16];
        bool g = false;
        TEST_ASSERT_FALSE(batch_on_parse(3, argv, expr, sizeof(expr), &g, targets, sizeof(targets)));
    }
    {
        char *argv[] = { "on", "goto", ":x" };  /* no expression */
        char expr[16];
        char targets[16];
        bool g = false;
        TEST_ASSERT_FALSE(batch_on_parse(3, argv, expr, sizeof(expr), &g, targets, sizeof(targets)));
    }
    {
        char *argv[] = { "on", "1", "jump", ":x" }; /* no keyword */
        char expr[16];
        char targets[16];
        bool g = false;
        TEST_ASSERT_FALSE(batch_on_parse(4, argv, expr, sizeof(expr), &g, targets, sizeof(targets)));
    }
}

void test_batch_on_select(void)
{
    {
        char targets[] = ":a,:b,:c";
        TEST_ASSERT_EQUAL_STRING(":a", batch_on_select(targets, 1));
    }
    {
        char targets[] = ":a,:b,:c";
        TEST_ASSERT_EQUAL_STRING(":c", batch_on_select(targets, 3));
    }
    {
        char targets[] = "a, b ,  c ";
        TEST_ASSERT_EQUAL_STRING("b", batch_on_select(targets, 2));
    }
    {
        char targets[] = "only";
        TEST_ASSERT_EQUAL_STRING("only", batch_on_select(targets, 1));
    }
    {
        /* Out of range falls through (NULL) on both ends. */
        char targets[] = "a,b";
        TEST_ASSERT_NULL(batch_on_select(targets, 0));
    }
    {
        char targets[] = "a,b";
        TEST_ASSERT_NULL(batch_on_select(targets, 3));
    }
    {
        /* A single empty entry is still a valid (empty) selection. */
        char targets[] = "";
        const char *sel = batch_on_select(targets, 1);
        TEST_ASSERT_NOT_NULL(sel);
        TEST_ASSERT_EQUAL_STRING("", sel);
    }
}

/* ========================================================================
 * `on` EXPRESSION -> TARGET SELECTION (integration of the two helpers)
 * ======================================================================== */

void test_batch_on_dispatch_selection(void)
{
    char targets[] = ":one,:two,:three";
    int32_t index = 0;
    const char *error = NULL;

    TEST_ASSERT_TRUE(shell_expr_evaluate("1 + 1", &index, &error));
    TEST_ASSERT_EQUAL_INT32(2, index);
    TEST_ASSERT_EQUAL_STRING(":two", batch_on_select(targets, (int)index));

    /* Re-split the same buffer for a different index. */
    char again[] = ":one,:two,:three";
    TEST_ASSERT_TRUE(shell_expr_evaluate("4 - 1", &index, &error));
    TEST_ASSERT_EQUAL_INT32(3, index);
    TEST_ASSERT_EQUAL_STRING(":three", batch_on_select(again, (int)index));

    /* An out-of-range result yields no selection (fall through). */
    TEST_ASSERT_TRUE(shell_expr_evaluate("9", &index, &error));
    char none[] = ":one,:two,:three";
    TEST_ASSERT_NULL(batch_on_select(none, (int)index));
}

/* ========================================================================
 * `for /L` NUMERIC SET PARSER
 * ======================================================================== */

void test_batch_forl_parse(void)
{
    int32_t start = 0;
    int32_t step = 0;
    int32_t end = 0;

    TEST_ASSERT_TRUE(shell_forl_parse("1,1,5", &start, &step, &end));
    TEST_ASSERT_EQUAL_INT32(1, start);
    TEST_ASSERT_EQUAL_INT32(1, step);
    TEST_ASSERT_EQUAL_INT32(5, end);

    TEST_ASSERT_TRUE(shell_forl_parse(" 10 , -2 , 4 ", &start, &step, &end));
    TEST_ASSERT_EQUAL_INT32(10, start);
    TEST_ASSERT_EQUAL_INT32(-2, step);
    TEST_ASSERT_EQUAL_INT32(4, end);

    /* Zero step, wrong arity, non-numeric text, and trailing garbage fail. */
    TEST_ASSERT_FALSE(shell_forl_parse("1,0,5", &start, &step, &end));
    TEST_ASSERT_FALSE(shell_forl_parse("1,1", &start, &step, &end));
    TEST_ASSERT_FALSE(shell_forl_parse("1,1,5,7", &start, &step, &end));
    TEST_ASSERT_FALSE(shell_forl_parse("a,1,5", &start, &step, &end));
    TEST_ASSERT_FALSE(shell_forl_parse("1,1,5x", &start, &step, &end));
    TEST_ASSERT_FALSE(shell_forl_parse("", &start, &step, &end));
    TEST_ASSERT_FALSE(shell_forl_parse(NULL, &start, &step, &end));
    TEST_ASSERT_FALSE(shell_forl_parse("1,1,5", NULL, &step, &end));
}

/* ========================================================================
 * `for /A` PREFIX COLLECTION + `switch` MATCHER
 * ======================================================================== */

void test_batch_fora_collect(void)
{
    char names[64][32];
    int n;

    shell_env_set("TST[2]", "b");
    shell_env_set("TST[10]", "j");
    shell_env_set("TST[X]", "x");
    shell_env_set("OTHER[1]", "o");
    shell_env_set("TST", "plain");
    shell_env_set("TSTX[1]", "prefix-but-no-bracket");

    n = shell_fora_collect("TST", names, 64);
    TEST_ASSERT_EQUAL_INT(3, n);
    TEST_ASSERT_EQUAL_STRING("TST[2]", names[0]);
    TEST_ASSERT_EQUAL_STRING("TST[10]", names[1]);
    TEST_ASSERT_EQUAL_STRING("TST[X]", names[2]);

    /* Case-insensitive prefix; empty prefix and NULL-safe. */
    n = shell_fora_collect("tst", names, 64);
    TEST_ASSERT_EQUAL_INT(3, n);
    TEST_ASSERT_EQUAL_INT(0, shell_fora_collect("", names, 64));
    TEST_ASSERT_EQUAL_INT(0, shell_fora_collect(NULL, names, 64));
    TEST_ASSERT_EQUAL_INT(0, shell_fora_collect("TST", NULL, 64));
    TEST_ASSERT_EQUAL_INT(0, shell_fora_collect("NOPE", names, 64));

    shell_env_set("TST[2]", "");
    shell_env_set("TST[10]", "");
    shell_env_set("TST[X]", "");
    shell_env_set("OTHER[1]", "");
    shell_env_set("TST", "");
    shell_env_set("TSTX[1]", "");
}

void test_batch_switch_select(void)
{
    char *cases1[] = { "setup:cfg", "diag:dg" };
    char *cases2[] = { "C:\\x:lbl", "other:oo" };
    char *cases3[] = { "nocolon", "a:b" };

    TEST_ASSERT_EQUAL_STRING("cfg", shell_switch_select("setup", cases1, 2));
    TEST_ASSERT_EQUAL_STRING("dg", shell_switch_select("DIAG", cases1, 2));
    TEST_ASSERT_NULL(shell_switch_select("nope", cases1, 2));
    /* Split on the LAST colon so a match containing one survives. */
    TEST_ASSERT_EQUAL_STRING("lbl", shell_switch_select("C:\\x", cases2, 2));
    /* Entries without a colon never match. */
    TEST_ASSERT_NULL(shell_switch_select("nocolon", cases3, 2));
    TEST_ASSERT_EQUAL_STRING("b", shell_switch_select("a", cases3, 2));
    TEST_ASSERT_NULL(shell_switch_select(NULL, cases1, 2));
    TEST_ASSERT_NULL(shell_switch_select("setup", NULL, 2));
    TEST_ASSERT_NULL(shell_switch_select("setup", cases1, 0));
}

/* ========================================================================
 * `while` CONDITION KEYWORD TRANSLATION
 * ======================================================================== */

void test_batch_while_keywords(void)
{
    char out[64];

    shell_while_translate_keywords("%n% LSS 3", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("%n% < 3", out);
    shell_while_translate_keywords("a GEQ b", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("a >= b", out);
    shell_while_translate_keywords("x EQU 1", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("x == 1", out);

    /* Non-keywords pass through; NULL is safe. */
    shell_while_translate_keywords("1", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("1", out);
    shell_while_translate_keywords("LESSER", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("LESSER", out);
    shell_while_translate_keywords(NULL, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("", out);
    shell_while_translate_keywords("1", NULL, 0);
}
