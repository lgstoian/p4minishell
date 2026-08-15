/**
 * @file test_calc.c
 * @brief Unit tests for the `calc` float evaluator and the `for /f` helpers.
 *
 * Covers calc_evaluate() and calc_format_number() from components/batch/calc.c
 * (arithmetic and precedence, every math/string function, hex literals, PI,
 * RAN#, angle modes, POL/REC X/Y side effects, assignment through the env
 * table, and error paths), plus the pure `for /f` option parser and line
 * splitter from components/batch/batch.c.
 *
 * Pure logic with no hardware dependency; the environment table is cleared by
 * batch_init(), which the runner calls before any suite.
 */

#include "unity.h"
#include "calc.h"
#include "batch.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define TOL 1e-9

/** Evaluate and assert a numeric result within @p tolerance. */
static void expect_num(const char *expression, double expected)
{
    calc_value_t result;
    const char *error = NULL;

    TEST_ASSERT_TRUE_MESSAGE(calc_evaluate(expression, &result, &error), expression);
    TEST_ASSERT_FALSE_MESSAGE(result.is_string, expression);
    TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(TOL, expected, result.num, expression);
}

/** Evaluate and assert a string result. */
static void expect_str(const char *expression, const char *expected)
{
    calc_value_t result;
    const char *error = NULL;

    TEST_ASSERT_TRUE_MESSAGE(calc_evaluate(expression, &result, &error), expression);
    TEST_ASSERT_TRUE_MESSAGE(result.is_string, expression);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(expected, result.str, expression);
}

/** Assert that an expression is rejected with a reason. */
static void expect_error(const char *expression)
{
    calc_value_t result;
    const char *error = NULL;

    TEST_ASSERT_FALSE_MESSAGE(calc_evaluate(expression, &result, &error), expression);
    TEST_ASSERT_NOT_NULL_MESSAGE(error, expression);
}

/* ========================================================================
 * ARITHMETIC AND PRECEDENCE
 * ======================================================================== */

void test_calc_arithmetic(void)
{
    expect_num("2+3", 5);
    expect_num("10-4", 6);
    expect_num("6*7", 42);
    expect_num("20/4", 5);
    expect_num("7.0/2", 3.5);
    expect_num("-7/2", -3.5);
    expect_num("2+3*4", 14);
    expect_num("(2+3)*4", 20);
    expect_num("2*3+4", 10);
    expect_num("-5", -5);
    expect_num("--5", 5);
    expect_num("+7", 7);
    expect_num("1.5", 1.5);
    expect_num("1e3", 1000);
    expect_num(".5", 0.5);
}

void test_calc_power_and_mod(void)
{
    expect_num("2^10", 1024);
    expect_num("2^0.5", sqrt(2.0));
    expect_num("2^3^2", 512);           /* right-associative */
    expect_num("-2^2", -4);             /* unary minus binds looser than ^ */
    expect_num("2^-1", 0.5);            /* negative exponent */
    expect_num("7 MOD 3", 1);
    expect_num("-7 MOD 3", 2);          /* result sign follows the divisor */
    expect_num("7 MOD -3", -2);
    expect_num("10 MOD 0.5", 0);
    expect_num("2*3^2", 18);
}

void test_calc_hex_literals(void)
{
    expect_num("&HFF", 255);
    expect_num("&h10", 16);
    expect_num("0xFF", 255);
    expect_num("&HFF + 1", 256);
}

/* ========================================================================
 * MATH FUNCTIONS
 * ======================================================================== */

void test_calc_math_functions(void)
{
    expect_num("abs(-5)", 5);
    expect_num("abs(5)", 5);
    expect_num("sgn(-3)", -1);
    expect_num("sgn(0)", 0);
    expect_num("sgn(3)", 1);
    expect_num("int(-3.5)", -4);
    expect_num("fix(-3.5)", -3);
    expect_num("frac(3.5)", 0.5);
    expect_num("round(2.5)", 3);
    expect_num("round(-2.5)", -3);
    expect_num("round(3.14159, 2)", 3.14);
    expect_num("sqr(16)", 4);
    expect_num("exp(1)", M_E);
    expect_num("ln(exp(1))", 1);
    expect_num("log(100)", 2);
    expect_num("fact(5)", 120);
    expect_num("fact(0)", 1);
    expect_num("ncr(5,2)", 10);
    expect_num("npr(5,2)", 20);
    expect_num("mod(10,3)", 1);
    expect_num("pi", M_PI);
}

void test_calc_trig_degrees(void)
{
    calc_set_angle_mode(true);
    expect_num("sin(30)", 0.5);
    expect_num("cos(60)", 0.5);
    expect_num("tan(45)", 1);
    expect_num("asin(1)", 90);
    expect_num("acos(1)", 0);
    expect_num("atan(1)", 45);
}

void test_calc_trig_radians(void)
{
    calc_set_angle_mode(false);
    expect_num("sin(0)", 0);
    expect_num("cos(0)", 1);
    expect_num("sin(pi/2)", 1);
    expect_num("atan(1)", M_PI / 4);
    expect_num("sinh(0)", 0);
    expect_num("cosh(0)", 1);
    expect_num("tanh(1)", tanh(1));
    calc_set_angle_mode(true);          /* restore the default */
}

void test_calc_math_errors(void)
{
    expect_error("sqr(-1)");
    expect_error("ln(0)");
    expect_error("log(-5)");
    expect_error("asin(2)");
    expect_error("acos(-2)");
    expect_error("fact(-1)");
    expect_error("fact(3.5)");
    expect_error("ncr(2,3)");
    expect_error("1/0");
    expect_error("7 MOD 0");
    expect_error("0^-1");
    expect_error("abs()");
    expect_error("abs(1,2)");
    expect_error("ncr(5)");
}

/* ========================================================================
 * STRING FUNCTIONS
 * ======================================================================== */

void test_calc_string_functions(void)
{
    expect_str("chr$(65)", "A");
    expect_str("str$(42)", "42");
    expect_str("str$(1.5)", "1.5");
    expect_str("hex$(255)", "FF");
    expect_str("hex$(&H1F)", "1F");
    expect_str("dms$(12.3456)", "12d20'44.16\"");
    expect_str("'hello'", "hello");
    expect_str("'a' + 'b'", "ab");
    expect_str("'ab' + 1", "ab1");
    expect_str("left$('hello',2)", "he");
    expect_str("right$('hello',2)", "lo");
    expect_str("mid$('hello',2,3)", "ell");
    expect_str("mid$('hello',2)", "ello");
    expect_str("mid$('hello',9)", "");
    expect_str("left$('hello',99)", "hello");
}

void test_calc_string_numbers(void)
{
    expect_num("len('hello')", 5);
    expect_num("len('')", 0);
    expect_num("asc('A')", 65);
    expect_num("val('42')", 42);
    expect_num("val('12abc')", 12);
    expect_num("val('abc')", 0);
    expect_num("valf('3.25')", 3.25);
}

void test_calc_string_errors(void)
{
    expect_error("asc('')");
    expect_error("chr$(300)");
    expect_error("chr$(-1)");
    expect_error("left$('abc',-1)");
    expect_error("mid$('abc',0)");
    expect_error("mid$('abc',1,-1)");
    expect_error("'unterminated");
    expect_error("lbound('abc')");
}

/* ========================================================================
 * VARIABLES, ASSIGNMENT, AND POL/REC SIDE EFFECTS
 * ======================================================================== */

void test_calc_env_variables(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, shell_env_set("CALCTST", "12"));
    expect_num("calctst * 2", 24);
    expect_num("calctst + calctst", 24);
    expect_num("undefined_var", 0);       /* DOS: undefined reads as 0 */
    (void)shell_env_set("CALCTST", "");
}

void test_calc_pol_rec_side_effects(void)
{
    const char *xval;
    const char *yval;

    calc_set_angle_mode(true);
    expect_num("pol(3,4)", 5);
    xval = shell_env_get("X");
    yval = shell_env_get("Y");
    TEST_ASSERT_NOT_NULL_MESSAGE(xval, "pol sets X");
    TEST_ASSERT_NOT_NULL_MESSAGE(yval, "pol sets Y");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("5", xval, "pol r");
    /* The stored angle round-trips through calc_format_number's 12-digit
     * precision, so compare with a matching tolerance. */
    TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(1e-6, 53.130102, strtod(yval, NULL), "pol theta");

    expect_num("rec(10,60)", 5);
    xval = shell_env_get("X");
    yval = shell_env_get("Y");
    TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(1e-6, 5.0, strtod(xval, NULL), "rec x");
    TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(1e-6, 8.660254, strtod(yval, NULL), "rec y");
    calc_set_angle_mode(true);
}

void test_calc_random(void)
{
    calc_value_t a;
    calc_value_t b;
    const char *error = NULL;

    /* Seeded RAN# is deterministic. */
    TEST_ASSERT_TRUE(calc_evaluate("ran#(42)", &a, &error));
    TEST_ASSERT_TRUE(calc_evaluate("ran#(42)", &b, &error));
    TEST_ASSERT_EQUAL_DOUBLE(a.num, b.num);
    TEST_ASSERT_TRUE(a.num >= 0.0 && a.num < 1.0);
}

/* ========================================================================
 * FORMATTING AND PARSER ROBUSTNESS
 * ======================================================================== */

void test_calc_format_number(void)
{
    char buf[32];

    calc_format_number(5, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("5", buf);
    calc_format_number(1.5, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("1.5", buf);
    calc_format_number(-0.25, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("-0.25", buf);
    calc_format_number(M_PI, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("3.14159265359", buf);
}

void test_calc_syntax_errors(void)
{
    expect_error("");
    expect_error("2+");
    expect_error("(1");
    expect_error("1)");
    expect_error("2 2");
    expect_error("&H");
    expect_error("@");
    expect_error("1..2");
}

/* ========================================================================
 * `for /f` OPTION PARSING AND LINE SPLITTING
 * ======================================================================== */

void test_forf_options_defaults(void)
{
    shell_forf_options_t opts;

    shell_forf_parse_options("", 0, &opts);
    TEST_ASSERT_EQUAL_STRING(" \t", opts.delims);
    TEST_ASSERT_EQUAL(1, opts.token_count);
    TEST_ASSERT_EQUAL(1, opts.token_list[0]);
    TEST_ASSERT_EQUAL(0, opts.skip);
    TEST_ASSERT_EQUAL('\0', opts.eol);
    TEST_ASSERT_FALSE(opts.star);
}

void test_forf_parse_options(void)
{
    shell_forf_options_t opts;
    const char *text = "delims=,; tokens=1,3-5 skip=2 eol=;";

    TEST_ASSERT_TRUE(shell_forf_parse_options(text, strlen(text), &opts));
    TEST_ASSERT_EQUAL_STRING(",;", opts.delims);
    TEST_ASSERT_EQUAL(2, opts.skip);
    TEST_ASSERT_EQUAL(';', opts.eol);
    TEST_ASSERT_EQUAL(4, opts.token_count);
    TEST_ASSERT_EQUAL(1, opts.token_list[0]);
    TEST_ASSERT_EQUAL(3, opts.token_list[1]);
    TEST_ASSERT_EQUAL(4, opts.token_list[2]);
    TEST_ASSERT_EQUAL(5, opts.token_list[3]);
    TEST_ASSERT_FALSE(opts.star);
}

void test_forf_parse_star(void)
{
    shell_forf_options_t opts;
    const char *text = "tokens=1,* delims= ";

    TEST_ASSERT_TRUE(shell_forf_parse_options(text, strlen(text), &opts));
    TEST_ASSERT_TRUE(opts.star);
    TEST_ASSERT_EQUAL(1, opts.token_count);
    TEST_ASSERT_EQUAL(1, opts.token_list[0]);
}

void test_forf_parse_errors(void)
{
    shell_forf_options_t opts;

    TEST_ASSERT_FALSE(shell_forf_parse_options("bogus", 5, &opts));
    TEST_ASSERT_FALSE(shell_forf_parse_options("skip=x", 6, &opts));
    TEST_ASSERT_FALSE(shell_forf_parse_options("tokens=1,,2", 10, &opts));
    TEST_ASSERT_FALSE(shell_forf_parse_options("tokens=2-1", 10, &opts));
    TEST_ASSERT_FALSE(shell_forf_parse_options("tokens=0", 8, &opts));
    TEST_ASSERT_FALSE(shell_forf_parse_options("unknown=1", 9, &opts));
}

void test_forf_split_line(void)
{
    shell_forf_tok_t tokens[8];
    int count;

    count = shell_forf_split_line("a b\tc", " \t", tokens, 8);
    TEST_ASSERT_EQUAL(3, count);
    TEST_ASSERT_EQUAL_STRING_LEN("a", tokens[0].start, 1);
    TEST_ASSERT_EQUAL_STRING_LEN("b", tokens[1].start, 1);
    TEST_ASSERT_EQUAL_STRING_LEN("c", tokens[2].start, 1);

    count = shell_forf_split_line("one,two,three", ",", tokens, 8);
    TEST_ASSERT_EQUAL(3, count);
    TEST_ASSERT_EQUAL_STRING_LEN("one", tokens[0].start, 3);

    /* Empty delims means no delimiters: the whole line is one token. */
    count = shell_forf_split_line("hello world", "", tokens, 8);
    TEST_ASSERT_EQUAL(1, count);
    TEST_ASSERT_EQUAL_STRING_LEN("hello world", tokens[0].start, 11);

    /* Empty / delimiter-only lines produce no tokens. */
    count = shell_forf_split_line("", " \t", tokens, 8);
    TEST_ASSERT_EQUAL(0, count);
    count = shell_forf_split_line("   ", " \t", tokens, 8);
    TEST_ASSERT_EQUAL(0, count);
}
