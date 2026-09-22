/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_batch_expr.c
 * @brief Unit tests for the `set /a` arithmetic expression evaluator.
 *
 * Covers shell_expr_evaluate() from components/batch/batch.c: operator
 * precedence, associativity, every supported operator, number bases, error
 * detection, and the DOS rule that an undefined variable reads as 0.
 *
 * Pure integer maths with no hardware dependency. Variable lookups go through
 * the batch module's environment table, which batch_init() clears.
 */

#include "unity.h"
#include "batch.h"
#include <stdint.h>

/** Evaluate and assert the expected value, failing on a parse error. */
static void expect_value(const char *expression, int32_t expected)
{
    int32_t result = 0;
    const char *error = NULL;

    TEST_ASSERT_TRUE_MESSAGE(shell_expr_evaluate(expression, &result, &error), expression);
    TEST_ASSERT_EQUAL_INT32_MESSAGE(expected, result, expression);
}

/** Assert that an expression is rejected. */
static void expect_error(const char *expression)
{
    int32_t result = 0;
    const char *error = NULL;

    TEST_ASSERT_FALSE_MESSAGE(shell_expr_evaluate(expression, &result, &error), expression);
    TEST_ASSERT_NOT_NULL_MESSAGE(error, expression);
}

/* ========================================================================
 * LITERALS AND NUMBER BASES
 * ======================================================================== */

void test_batch_expr_literals(void)
{
    expect_value("0", 0);
    expect_value("42", 42);
    expect_value("  7  ", 7);

    /* Hexadecimal and octal, as DOS accepts. */
    expect_value("0x10", 16);
    expect_value("0xFF", 255);
    expect_value("010", 8);

    /* Unary operators. */
    expect_value("-5", -5);
    expect_value("+5", 5);
    expect_value("--5", 5);
    expect_value("~0", -1);
    expect_value("!0", 1);
    expect_value("!5", 0);
}

/* ========================================================================
 * ARITHMETIC AND PRECEDENCE
 * ======================================================================== */

void test_batch_expr_arithmetic(void)
{
    expect_value("2+3", 5);
    expect_value("10-4", 6);
    expect_value("6*7", 42);
    expect_value("20/4", 5);
    expect_value("17%5", 2);

    /* Integer division truncates toward zero. */
    expect_value("7/2", 3);
    expect_value("-7/2", -3);

    /* Multiplicative binds tighter than additive. */
    expect_value("2+3*4", 14);
    expect_value("2*3+4", 10);
    expect_value("10-2*3", 4);

    /* Left associativity. */
    expect_value("10-3-2", 5);
    expect_value("100/5/2", 10);

    /* Parentheses override precedence. */
    expect_value("(2+3)*4", 20);
    expect_value("2*(3+4)", 14);
    expect_value("((1+2)*(3+4))", 21);

    /* Unary minus against a parenthesised group. */
    expect_value("-(2+3)", -5);
}

/* ========================================================================
 * BITWISE AND SHIFT OPERATORS
 * ======================================================================== */

void test_batch_expr_bitwise(void)
{
    expect_value("6&3", 2);
    expect_value("6|3", 7);
    expect_value("6^3", 5);

    expect_value("1<<4", 16);
    expect_value("256>>4", 16);

    /* An arithmetic right shift preserves the sign. */
    expect_value("-16>>2", -4);

    /* Precedence: shifts bind tighter than &, which binds tighter than
     * ^, which binds tighter than |. */
    expect_value("1|2&3", 3);
    expect_value("1^1|4", 4);
    expect_value("1+1<<2", 8);

    /* Parentheses still win. */
    expect_value("(1|2)&3", 3);
    expect_value("1|(2&3)", 3);

    /* An over-wide shift is clamped rather than invoking undefined behavior. */
    expect_value("1<<64", 0);
    expect_value("1<<32", 0);
}

/* ========================================================================
 * VARIABLES
 * ======================================================================== */

void test_batch_expr_variables(void)
{
    int32_t result = 0;

    /* An undefined variable reads as 0, exactly as DOS does. This is the
     * behavior that lets `set /a total=total+1` work on first use. */
    expect_value("UNDEFINED_TEST_VAR", 0);
    expect_value("UNDEFINED_TEST_VAR+5", 5);

    /* A defined variable is read back and used in the expression. */
    TEST_ASSERT_EQUAL(ESP_OK, shell_env_set("EXPRTEST", "12"));
    expect_value("EXPRTEST", 12);
    expect_value("EXPRTEST*2", 24);
    expect_value("EXPRTEST+EXPRTEST", 24);

    /* Hex stored in a variable is parsed on read. */
    TEST_ASSERT_EQUAL(ESP_OK, shell_env_set("EXPRHEX", "0x10"));
    expect_value("EXPRHEX", 16);

    /* Clean up so later suites see a predictable environment. */
    TEST_ASSERT_EQUAL(ESP_OK, shell_env_set("EXPRTEST", ""));
    TEST_ASSERT_EQUAL(ESP_OK, shell_env_set("EXPRHEX", ""));

    /* Cleared variables read as 0 again. */
    TEST_ASSERT_TRUE(shell_expr_evaluate("EXPRTEST", &result, NULL));
    TEST_ASSERT_EQUAL_INT32(0, result);
}

void test_batch_expr_bracket_names(void)
{
    int32_t result = 0;

    /* Indexed `ARR[i]` elements are ordinary environment slots. */
    TEST_ASSERT_EQUAL(ESP_OK, shell_env_set("BRK[2]", "7"));
    TEST_ASSERT_TRUE(shell_expr_evaluate("BRK[2]*2", &result, NULL));
    TEST_ASSERT_EQUAL_INT32(14, result);
    TEST_ASSERT_TRUE(shell_expr_evaluate("BRK[9]+1", &result, NULL));
    TEST_ASSERT_EQUAL_INT32(1, result);
    TEST_ASSERT_EQUAL(ESP_OK, shell_env_set("BRK[2]", ""));
}

/* ========================================================================
 * COMPARISON OPERATORS (== != < > <= >=)
 * ======================================================================== */

void test_batch_expr_comparisons(void)
{
    /* Equality and inequality yield 1/0. */
    expect_value("5==5", 1);
    expect_value("5==6", 0);
    expect_value("5!=6", 1);
    expect_value("5!=5", 0);

    /* Ordering relations. */
    expect_value("5<6", 1);
    expect_value("6<5", 0);
    expect_value("5>6", 0);
    expect_value("6>5", 1);
    expect_value("5<=5", 1);
    expect_value("6<=5", 0);
    expect_value("5>=5", 1);
    expect_value("5>=6", 0);

    /* Comparisons bind looser than arithmetic, so arithmetic wins first. */
    expect_value("2+3==5", 1);
    expect_value("2+3==6", 0);
    expect_value("10-2*3==4", 1);
    expect_value("(2+3)==5", 1);

    /* Bitwise operators bind tighter than comparisons (cmd.exe order). */
    expect_value("1|0==0", 0);          /* (1|0) == 0 */
    expect_value("6&3==2", 1);          /* (6&3) == 2 */

    /* Shift binds tighter than comparison. */
    expect_value("1<<2==4", 1);
    expect_value("4>>1==2", 1);

    /* Hex literals compare as their numeric value. */
    expect_value("0x10==16", 1);
    expect_value("0xFF>254", 1);

    /* A chain of comparisons uses the previous result as the left side. */
    expect_value("3>2>0", 1);           /* (3>2) > 0 -> 1 > 0 */

    /* Comparisons can be used anywhere an integer is expected. */
    expect_value("(5>3)*10", 10);
    expect_value("2+(4<=4)", 3);
}

/* ========================================================================
 * LOGICAL OPERATORS (&& ||)
 * ======================================================================== */

void test_batch_expr_logical(void)
{
    expect_value("1&&1", 1);
    expect_value("1&&0", 0);
    expect_value("0&&1", 0);
    expect_value("0||1", 1);
    expect_value("0||0", 0);
    expect_value("1||0", 1);

    /* Any non-zero is truthy, exactly like the unary `!`. */
    expect_value("5&&5", 1);
    expect_value("5&&0", 0);
    expect_value("0||-1", 1);

    /* `&&` binds tighter than `||`. */
    expect_value("1||0&&0", 1);         /* 1 || (0&&0) */

    /* Both sides are evaluated (no short-circuiting, matching cmd.exe), so a
     * division by zero on the right side is still an error. */
    expect_error("0&&1/0");
    expect_error("1||1/0");

    /* Comparisons feed logical operators. */
    expect_value("(5==5)&&(6>5)", 1);
    expect_value("(5==5)&&(6<5)", 0);
}

/* ========================================================================
 * ERROR DETECTION
 * ======================================================================== */

void test_batch_expr_errors(void)
{
    /* Division and modulo by zero are caught rather than trapping. */
    expect_error("1/0");
    expect_error("1%0");
    expect_error("5/(3-3)");

    /* Malformed input. */
    expect_error("");
    expect_error("(1+2");
    expect_error("1+");
    expect_error("*5");
    expect_error("1 2");
    expect_error("@");

    /* NULL is rejected without dereferencing. */
    expect_error(NULL);

    /* INT32_MIN / -1 overflows two's-complement and is refused. */
    expect_error("-2147483648/-1");

    /* A successful evaluation must still work after errors, proving the
     * parser holds no state between calls. */
    expect_value("2+2", 4);
}
