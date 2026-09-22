/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_calc.c
 * @brief Unit tests for the `calc` float evaluator and the `for /f` helpers.
 *
 * Covers calc_evaluate() and calc_format_number() from components/batch/calc.c
 * (arithmetic and precedence, every math/string function, financial TVM/NPV/
 * IRR/depreciation, epoch-day date functions, hex literals, PI,
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
 * NEW MATH FUNCTIONS (CUR, DEG, ASINH, ACOSH, ATANH)
 * ======================================================================== */

void test_calc_new_math_functions(void)
{
    /* CUR - cube root */
    expect_num("cur(8)", 2);
    expect_num("cur(27)", 3);
    expect_num("cur(-27)", -3);
    expect_num("cur(-8)", -2);
    expect_num("cur(0)", 0);
    expect_num("cur(1)", 1);
    expect_num("cur(1000)", 10);

    /* DEG - sexagesimal to decimal */
    /* 30.1530 = 30° 15' 30" = 30 + 15/60 + 30/3600 = 30.258333... */
    expect_num("deg(30.1530)", 30.0 + 15.0/60.0 + 30.0/3600.0);
    /* 45.30 = 45° 30' 00" = 45 + 30/60 = 45.5 */
    expect_num("deg(45.30)", 45.5);
    /* 0.0001 = 0° 00' 01" = 1/3600 */
    expect_num("deg(0.0001)", 1.0/3600.0);
    /* Negative */
    expect_num("deg(-30.1530)", -(30.0 + 15.0/60.0 + 30.0/3600.0));
    /* Degrees only */
    expect_num("deg(90)", 90);

    /* ASINH - inverse hyperbolic sine */
    expect_num("asinh(0)", 0);
    expect_num("asinh(1)", asinh(1));
    expect_num("asinh(-1)", asinh(-1));
    expect_num("asinh(10)", asinh(10));
    expect_num("asinh(-10)", asinh(-10));

    /* ACOSH - inverse hyperbolic cosine */
    expect_num("acosh(1)", 0);
    expect_num("acosh(2)", acosh(2));
    expect_num("acosh(10)", acosh(10));

    /* ATANH - inverse hyperbolic tangent */
    expect_num("atanh(0)", 0);
    expect_num("atanh(0.5)", atanh(0.5));
    expect_num("atanh(-0.5)", atanh(-0.5));
    expect_num("atanh(0.999)", atanh(0.999));
    expect_num("atanh(-0.999)", atanh(-0.999));
}

void test_calc_new_math_errors(void)
{
    /* ACOSH domain: x >= 1 */
    expect_error("acosh(0.5)");
    expect_error("acosh(0)");
    expect_error("acosh(-1)");

    /* ATANH domain: -1 < x < 1 */
    expect_error("atanh(1)");
    expect_error("atanh(-1)");
    expect_error("atanh(1.5)");
    expect_error("atanh(-1.5)");
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
    expect_str("upper$('Hello')", "HELLO");
    expect_str("lower$('HeLLo')", "hello");
    expect_str("trim$('  hi  ')", "hi");
    expect_str("trim$('hi')", "hi");
    expect_str("replace$('aaa','a','b')", "bbb");
    expect_str("replace$('hello','l','')", "heo");
    expect_str("replace$('hello','','x')", "hello");
}

void test_calc_instr(void)
{
    expect_num("instr('hello','l')", 3);
    expect_num("instr('hello','z')", 0);
    expect_num("instr('hello','')", 0);
    expect_num("instr(2,'hello','l')", 3);
    expect_num("instr(4,'hello','l')", 4);
    expect_num("instr(9,'hello','l')", 0);
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

void test_calc_base_and_units(void)
{
    expect_str("bin$(0)", "0");
    expect_str("bin$(5)", "101");
    expect_str("bin$(255)", "11111111");
    expect_str("oct$(0)", "0");
    expect_str("oct$(8)", "10");
    expect_str("oct$(255)", "377");
    expect_num("valb('FF',16)", 255);
    expect_num("valb('101',2)", 5);
    expect_num("valb('17',8)", 15);
    expect_num("valb('Z',36)", 35);
    expect_num("c2f(0)", 32);
    expect_num("c2f(100)", 212);
    expect_num("f2c(32)", 0);
    expect_num("f2c(212)", 100);
    expect_num("in2mm(1)", 25.4);
    expect_num("mm2in(25.4)", 1);
    expect_num("lb2kg(1)", 0.45359237);
    expect_num("kg2lb(1)", 2.20462262185);
    expect_error("bin$(3.5)");
    expect_error("bin$(-1)");
    expect_error("bin$(99999999999)");
    expect_error("oct$(-1)");
    expect_error("valb('FF',1)");
    expect_error("valb('FF',37)");
    expect_error("valb('GG',16)");
    expect_error("valb('10')");
    /* The VAL prefix guard must not swallow VALB. */
    expect_num("val('42')", 42);
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

void test_calc_command_assignment(void)
{
    const char *value;
    int level;

    /* `calc NAME = <expr>` must store only the part before '=' in NAME (the
     * expression contains spaces, which shell_env_set rejects). Regression
     * for the bug where the whole "NAME = expr" text was passed as the name. */
    (void)shell_env_set("CALCCMD", "");
    level = shell_command_calc_line("calc calccmd = 2^3");
    TEST_ASSERT_EQUAL(0, level);
    value = shell_env_get("CALCCMD");
    TEST_ASSERT_NOT_NULL(value);
    TEST_ASSERT_EQUAL_STRING("8", value);

    /* A malformed name is still rejected. */
    level = shell_command_calc_line("calc 3bad name = 1");
    TEST_ASSERT_EQUAL(1, level);
    (void)shell_env_set("CALCCMD", "");
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

void test_forf_command_set(void)
{
    char inner[256];

    /* Single-quoted sets are the command form, whitespace-tolerant. */
    TEST_ASSERT_TRUE(shell_forf_is_command_set("'dir /b'", false, inner, sizeof(inner)));
    TEST_ASSERT_EQUAL_STRING("dir /b", inner);
    TEST_ASSERT_TRUE(shell_forf_is_command_set("  'echo hi'  ", false, inner, sizeof(inner)));
    TEST_ASSERT_EQUAL_STRING("echo hi", inner);

    /* Backquotes select the command form only with usebackq. */
    TEST_ASSERT_FALSE(shell_forf_is_command_set("`dir`", false, inner, sizeof(inner)));
    TEST_ASSERT_TRUE(shell_forf_is_command_set("`dir`", true, inner, sizeof(inner)));
    TEST_ASSERT_EQUAL_STRING("dir", inner);

    /* Plain files, wildcards, and empty sets stay the file form. */
    TEST_ASSERT_FALSE(shell_forf_is_command_set("data.txt", false, inner, sizeof(inner)));
    TEST_ASSERT_FALSE(shell_forf_is_command_set("*.txt", true, inner, sizeof(inner)));
    TEST_ASSERT_FALSE(shell_forf_is_command_set("", false, inner, sizeof(inner)));
    TEST_ASSERT_FALSE(shell_forf_is_command_set("'unbalanced", false, inner, sizeof(inner)));
    TEST_ASSERT_FALSE(shell_forf_is_command_set(NULL, false, inner, sizeof(inner)));

    /* An empty quoted command still detects (warns when run). */
    TEST_ASSERT_TRUE(shell_forf_is_command_set("''", false, inner, sizeof(inner)));
    TEST_ASSERT_EQUAL_STRING("", inner);
}

void test_arg_apply_modifiers(void)
{
    char out[128];

    /* Empty mods only strip one pair of double quotes. */
    shell_arg_apply_modifiers("\"hello\"", "", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("hello", out);
    shell_arg_apply_modifiers("plain", "", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("plain", out);
    shell_arg_apply_modifiers("\"unbalanced", "", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("\"unbalanced", out);

    /* Name / extension / directory surgery on FATFS paths. */
    shell_arg_apply_modifiers("sd:/DIR/FILE.TXT", "n", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("FILE", out);
    shell_arg_apply_modifiers("sd:/DIR/FILE.TXT", "x", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING(".TXT", out);
    shell_arg_apply_modifiers("sd:/DIR/FILE.TXT", "nx", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("FILE.TXT", out);
    shell_arg_apply_modifiers("sd:/DIR/FILE.TXT", "dp", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("sd:/DIR/", out);
    shell_arg_apply_modifiers("sd:/DIR/FILE.TXT", "dpnx", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("sd:/DIR/FILE.TXT", out);

    /* No drive letters on FATFS: `d` contributes nothing. */
    shell_arg_apply_modifiers("sd:/DIR/FILE.TXT", "d", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("", out);

    /* A leading dot is not an extension. */
    shell_arg_apply_modifiers(".profile", "x", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("", out);
    shell_arg_apply_modifiers(".profile", "n", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING(".profile", out);

    /* `f` resolves against the shell cwd; the name always survives. */
    shell_arg_apply_modifiers("sd:/DIR/FILE.TXT", "f", out, sizeof(out));
    TEST_ASSERT_TRUE(strstr(out, "FILE.TXT") != NULL);

    /* NULL-safe: NULL value reads empty, NULL mods only dequote. */
    shell_arg_apply_modifiers(NULL, "nx", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("", out);
    shell_arg_apply_modifiers("\"q\"", NULL, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("q", out);
}

/* ========================================================================
 * FINANCIAL FUNCTIONS (HP-12C conventions)
 * ======================================================================== */

/** Evaluate and assert a numeric result within an explicit tolerance. */
static void expect_near(const char *expression, double expected, double tol)
{
    calc_value_t result;
    const char *error = NULL;

    TEST_ASSERT_TRUE_MESSAGE(calc_evaluate(expression, &result, &error), expression);
    TEST_ASSERT_FALSE_MESSAGE(result.is_string, expression);
    TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(tol, expected, result.num, expression);
}

void test_calc_financial_tvm(void)
{
    /* 30-year mortgage: $200k at 5% nominal -> $1073.64/mo. */
    expect_near("PMT(0.05/12,360,200000)", -1073.6432, 0.01);
    /* Closed-form round trip: PV of that payment stream is the principal. */
    expect_near("PV(0.05/12,360,PMT(0.05/12,360,200000))", 200000.0, 1e-6);
    /* $100/mo saved 30 years at 5% -> ~$83k. */
    expect_near("FV(0.05/12,360,-100)", 83225.86, 0.1);
    /* Term recovery from the rounded payment. */
    expect_near("NPER(0.05/12,-1073.6432,200000)", 360.0, 0.05);
    /* Rate recovery is exact on the unrounded round trip. */
    expect_num("RATE(360,PMT(0.05/12,360,200000),200000)", 0.05 / 12);
    /* Zero-rate (linear) paths. */
    expect_num("FV(0,12,-100,-1000)", 2200.0);
    expect_num("PV(0,12,-100)", 1200.0);
    expect_num("PMT(0,12,1200)", -100.0);
    expect_num("NPER(0,-100,1200)", 12.0);
    /* Beginning-of-period annuity-due. */
    expect_near("PV(0.1,2,-100,0,1)", 190.909, 0.001);
    expect_near("FV(0.1,2,-100,0,1)", 231.0, 0.001);
}

void test_calc_financial_npv_irr(void)
{
    expect_near("NPV(0.1,-1000,300,400,500)", -21.0368, 0.01);
    expect_num("NPV(0,-1000,300,400,500)", 200.0);
    expect_near("IRR(-1000,300,400,500)", 0.08896, 0.001);
    /* Simple two-flow case: double your money in one period -> 100%. */
    expect_near("IRR(-100,200)", 1.0, 1e-9);
}

void test_calc_financial_depreciation(void)
{
    expect_num("SLN(10000,1000,5)", 1800.0);
    expect_num("SYD(10000,1000,5,1)", 3000.0);
    expect_num("SYD(10000,1000,5,5)", 600.0);
    expect_near("DB(10000,1000,5,1)", 3690.43, 0.05);
    expect_near("DB(10000,1000,5,5)", 584.89, 0.5);
    /* Partial first year halves the first charge. */
    expect_near("DB(10000,1000,5,1,6)", 1845.21, 0.05);
}

void test_calc_financial_errors(void)
{
    expect_error("PV(0.05)");
    expect_error("PV(0.05,12)");
    expect_error("FV(0.05,12,100,0,2)");
    expect_error("PMT(0.05,0,1000)");
    expect_error("NPER(0.05,0,0)");
    expect_error("NPER(0,0,100)");
    expect_error("RATE(12,0,100)");
    expect_error("RATE(0,100,1000)");
    expect_error("NPV(0.1)");
    expect_error("NPV(-1,100)");
    expect_error("IRR(100)");
    expect_error("IRR(100,200)");
    expect_error("SLN(1,2,0)");
    expect_error("SYD(1,2,0,1)");
    expect_error("SYD(1,2,5,6)");
    expect_error("DB(1000,1000,5,1)");
    expect_error("DB(10000,1000,5,6)");
    expect_error("DB(10000,1000,5,1,13)");
    expect_error("PV()");
    expect_error("IRR()");
}

/* ========================================================================
 * DATE FUNCTIONS (epoch-day serials)
 * ======================================================================== */

void test_calc_dates(void)
{
    /* 2026-09-15 is a Tuesday (DOW 2); 1970-01-01 was a Thursday (4). */
    expect_num("DOW(DATE(2026,9,15))", 2.0);
    expect_num("DOW(DATE(1970,1,1))", 4.0);
    expect_num("DATE(1970,1,1)", 0.0);
    expect_num("YEAR(DATE(2026,9,15))", 2026.0);
    expect_num("MONTH(DATE(2026,9,15))", 9.0);
    expect_num("DAY(DATE(2026,9,15))", 15.0);
    /* Jan 1 -> Sep 15 in a non-leap year is 257 days. */
    expect_num("DAYS(DATE(2026,1,1),DATE(2026,9,15))", 257.0);
    expect_num("DATEADD(DATE(2026,1,1),30)-DATE(2026,1,31)", 0.0);
    expect_str("DATESTR(DATEADD(DATE(2026,1,1),30))", "2026-01-31");
    /* Month ends, including leap February. */
    expect_str("DATESTR(EOMONTH(DATE(2026,2,10),0))", "2026-02-28");
    expect_str("DATESTR(EOMONTH(DATE(2026,1,15),1))", "2026-02-28");
    expect_str("DATESTR(EOMONTH(DATE(2026,3,15),-1))", "2026-02-28");
    expect_num("DAY(EOMONTH(DATE(2024,1,1),1))", 29.0);
    expect_num("DAY(DATE(2024,2,29))", 29.0);
    /* ISO round trip, and pre-1970 serials. */
    expect_str("DATESTR(DATEVALUE('2026-09-15'))", "2026-09-15");
    expect_num("DATEVALUE('2026-09-15')", 20711.0);
    expect_num("DATEVALUE('2026-09-15')-DATE(2026,9,15)", 0.0);
    expect_num("DATE(1969,12,31)", -1.0);
    expect_str("DATESTR(DATE(1969,12,31))", "1969-12-31");
}

void test_calc_date_today_roundtrip(void)
{
    calc_value_t result;
    const char *error = NULL;

    /* TODAY() rebuilds from its own parts whatever the clock says. */
    TEST_ASSERT_TRUE(calc_evaluate("TODAY()", &result, &error));
    TEST_ASSERT_FALSE(result.is_string);
    TEST_ASSERT_TRUE_MESSAGE(
        calc_evaluate("TODAY()-DATE(YEAR(TODAY()),MONTH(TODAY()),DAY(TODAY()))",
                      &result, &error),
        "TODAY round trip");
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.0, result.num);
}

void test_calc_date_errors(void)
{
    expect_error("DATE(2026,13,1)");
    expect_error("DATE(2026,0,1)");
    expect_error("DATE(2026,2,29)");
    expect_error("DATE(2026,1,0)");
    expect_error("DATE(2026,1,1.5)");
    expect_error("DATE(2026,1)");
    expect_error("DATEVALUE('15/09/2026')");
    expect_error("DATEVALUE('2026-09-15!')");
    expect_error("DATEVALUE('2026-13-01')");
    expect_error("DATEVALUE('nope')");
    expect_error("DATESTR()");
    expect_error("EOMONTH(DATE(2026,1,1),1.5)");
    expect_error("DOW()");
    expect_error("TODAY(1)");
    expect_error("DAYS(DATE(2026,1,1))");
}
