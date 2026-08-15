/**
 * @file calc.c
 * @brief `calc` command and floating-point expression evaluator.
 *
 * A batch-native calculator that brings the FX-870P/VX-4 BASIC math and
 * string surface into the DOS-style shell. `calc` evaluates a floating-point
 * expression and either prints the result or stores it into an environment
 * variable (`calc NAME=<expr>`), so batch files get real math:
 *
 *   calc 2^10            ->  1024
 *   calc x = sin(30)     ->  x = 0.5        (DEG mode is the default)
 *   calc len('hello')    ->  5
 *   calc &HFF + 1        ->  256
 *
 * Grammar (recursive descent over double / fixed-string values):
 *
 *   expr        := additive
 *   additive    := multiplicative (('+'|'-') multiplicative)*    ('+' concats strings)
 *   multiplicative := power (('*'|'/'|'MOD') power)*
 *   power       := unary ('^' power)?                           (right-assoc exponent)
 *   unary       := ('-'|'+') unary | primary
 *   primary     := number | '&H' hex | '0x' hex | string | '(' expr ')'
 *                  | FUNC '(' args ')' | 'PI' | 'RAN#' ['(' seed ')'] | variable
 *
 * POL/REC store their two results in the X and Y environment variables, the
 * same documented side effect the calculator's BASIC has. RAN# uses a seeded
 * xorshift generator so unit tests are deterministic.
 *
 * This file lives in components/batch (it is a batch language verb) and is
 * dispatched from components/command/command.c with the raw, unsplit line so
 * quoted string arguments survive the shell tokenizer. The evaluator itself
 * is pure: no transcript output, so test/main/test_calc.c can drive it.
 */

#include "calc.h"
#include "batch.h"
#include "shell.h"
#include "ansi_palette.h"
#include "esp_random.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CALC_STR_BYTES          P4_CONFIG_CALC_STR_BYTES
#define CALC_MAX_DEPTH          P4_CONFIG_CALC_MAX_DEPTH
#define CALC_PRINT_PRECISION    P4_CONFIG_CALC_PRINT_PRECISION
#define CALC_PI                 3.14159265358979323846264338327950288

/* ========================================================================
 * ANGLE MODE
 * ======================================================================== */

static bool s_calc_degrees = (P4_CONFIG_CALC_ANGLE_DEFAULT_DEG != 0);

bool calc_angle_is_degrees(void)
{
    return s_calc_degrees;
}

void calc_set_angle_mode(bool degrees)
{
    s_calc_degrees = degrees;
}

/* ========================================================================
 * RANDOM NUMBER GENERATOR (RAN#)
 * ========================================================================
 * A deterministic xorshift64 so `calc ran#(seed)` sequences are reproducible
 * in the unit tests; the first unseeded call seeds from the ESP hardware RNG.
 */

static uint64_t s_ran_state = 0;
static bool s_ran_seeded = false;

static void calc_ran_seed(uint64_t seed)
{
    if (seed == 0) {
        seed = 1;
    }
    s_ran_state = seed;
    s_ran_seeded = true;
}

/** Return a uniform double in [0, 1). */
static double calc_ran_next(void)
{
    if (!s_ran_seeded) {
        calc_ran_seed(((uint64_t)esp_random() << 32) | esp_random());
    }

    uint64_t x = s_ran_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    s_ran_state = x;
    return (double)(x >> 11) * (1.0 / 9007199254740992.0);
}

/* ========================================================================
 * NUMBER / STRING VALUE HELPERS
 * ======================================================================== */

void calc_format_number(double value, char *out, size_t size)
{
    char fmt[16];

    if (size == 0) {
        return;
    }
    out[0] = '\0';

    if (isnan(value)) {
        snprintf(out, size, "NaN");
        return;
    }
    if (isinf(value)) {
        snprintf(out, size, "%s", value > 0 ? "Inf" : "-Inf");
        return;
    }

    /* Integral values print without a decimal point (calculator style). */
    if (value == floor(value) && fabs(value) < 1e15) {
        snprintf(out, size, "%lld", (long long)value);
        return;
    }

    snprintf(fmt, sizeof(fmt), "%%.%dg", CALC_PRINT_PRECISION);
    snprintf(out, size, fmt, value);
}

/** Convert a value to its string form (numbers are formatted). */
static void calc_value_to_string(const calc_value_t *value, char *out, size_t size)
{
    if (value->is_string) {
        snprintf(out, size, "%s", value->str);
    } else {
        calc_format_number(value->num, out, size);
    }
}

/**
 * Convert a value to a number. Strings are accepted when the whole text
 * parses as a number (matching DOS's tolerant variable coercion).
 */
static bool calc_value_to_number(const calc_value_t *value, double *out)
{
    if (!value->is_string) {
        *out = value->num;
        return true;
    }

    const char *p = value->str;
    char *end = NULL;

    while (*p != '\0' && isspace((unsigned char)*p)) {
        p++;
    }
    errno = 0;
    *out = strtod(p, &end);
    if (end == p || errno == ERANGE) {
        return false;
    }
    while (*end != '\0' && isspace((unsigned char)*end)) {
        end++;
    }
    return *end == '\0';
}

static void calc_set_number(calc_value_t *value, double number)
{
    value->is_string = false;
    value->num = number;
    value->str[0] = '\0';
}

static void calc_set_string(calc_value_t *value, const char *text)
{
    value->is_string = true;
    value->num = 0.0;
    snprintf(value->str, sizeof(value->str), "%s", text);
}

/* ========================================================================
 * EXPRESSION PARSER
 * ======================================================================== */

/** Parser state threaded through the expression grammar. */
typedef struct {
    const char *cursor;   /**< Current read position. */
    int depth;            /**< Parenthesis / function nesting depth. */
    bool failed;          /**< Set on the first syntax or domain error. */
    const char *message;  /**< Human-readable reason for the failure. */
} calc_parser_t;

static void calc_parse_additive(calc_parser_t *parser, calc_value_t *out);
static void calc_parse_power(calc_parser_t *parser, calc_value_t *out);

/** Record the first error and stop evaluating. */
static void calc_expr_fail(calc_parser_t *parser, const char *message)
{
    if (!parser->failed) {
        parser->failed = true;
        parser->message = message;
    }
}

static void calc_parse_skip_space(calc_parser_t *parser)
{
    while (isspace((unsigned char)*parser->cursor)) {
        parser->cursor++;
    }
}

/** Convert a value argument to a number, failing the parse on a bad operand. */
static bool calc_arg_number(calc_parser_t *parser, const calc_value_t *value, double *out)
{
    if (calc_value_to_number(value, out)) {
        return true;
    }
    calc_expr_fail(parser, "numeric argument expected");
    return false;
}

/** Convert a value argument to a string, failing the parse on overflow. */
static bool calc_arg_string(calc_parser_t *parser, const calc_value_t *value,
                            char *out, size_t size)
{
    char text[CALC_STR_BYTES];

    calc_value_to_string(value, text, sizeof(text));
    if (strlen(text) >= size) {
        calc_expr_fail(parser, "string argument too long");
        return false;
    }
    snprintf(out, size, "%s", text);
    return true;
}

/* ------------------------------------------------------------------------
 * NUMERIC LITERALS
 * ------------------------------------------------------------------------ */

/** Numeric value of one hex digit. */
static unsigned calc_hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return (unsigned)(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return (unsigned)(c - 'a' + 10);
    }
    return (unsigned)(c - 'A' + 10);
}

/**
 * Parse a number literal: decimal/float (`strtod`), BASIC `&H` hex, or C `0x`
 * hex. Out-of-range and malformed literals are reported rather than clamped.
 */
static void calc_parse_number_literal(calc_parser_t *parser, calc_value_t *out)
{
    const char *c = parser->cursor;
    unsigned long long hex = 0;
    int digits = 0;

    if (c[0] == '&' && (c[1] == 'h' || c[1] == 'H')) {
        c += 2;
        while (isxdigit((unsigned char)*c)) {
            hex = hex * 16 + calc_hex_digit(*c);
            c++;
            digits++;
        }
        if (digits == 0) {
            calc_expr_fail(parser, "malformed hex literal");
            return;
        }
        parser->cursor = c;
        calc_set_number(out, (double)hex);
        return;
    }

    if (c[0] == '0' && (c[1] == 'x' || c[1] == 'X')) {
        c += 2;
        while (isxdigit((unsigned char)*c)) {
            hex = hex * 16 + calc_hex_digit(*c);
            c++;
            digits++;
        }
        if (digits == 0) {
            calc_expr_fail(parser, "malformed hex literal");
            return;
        }
        parser->cursor = c;
        calc_set_number(out, (double)hex);
        return;
    }

    {
        char *end = NULL;
        double value;

        errno = 0;
        value = strtod(parser->cursor, &end);
        if (end == parser->cursor || errno == ERANGE) {
            calc_expr_fail(parser, "malformed number");
            return;
        }
        parser->cursor = end;
        calc_set_number(out, value);
    }
}

/* ------------------------------------------------------------------------
 * FUNCTIONS
 * ------------------------------------------------------------------------ */

/** A numeric unary helper for trig-degree conversion on input. */
static double calc_trig_input(double x)
{
    return s_calc_degrees ? x * (CALC_PI / 180.0) : x;
}

/** A numeric helper for inverse-trig output in the current angle mode. */
static double calc_trig_output(double radians)
{
    return s_calc_degrees ? radians * (180.0 / CALC_PI) : radians;
}

/** Floor-modulo, so the result sign follows the divisor (BASIC MOD). */
static bool calc_floor_mod(double a, double b, double *out)
{
    if (b == 0.0) {
        return false;
    }
    *out = a - b * floor(a / b);
    return true;
}

/** Factorial: non-negative integers only, capped before overflow. */
static bool calc_factorial(double x, double *out)
{
    double result = 1.0;

    if (x < 0.0 || x != floor(x) || x > 170.0) {
        return false;
    }
    for (int i = 2; i <= (int)x; i++) {
        result *= i;
    }
    *out = result;
    return true;
}

/** Combinations / permutations over integers, guarded against overflow. */
static bool calc_ncx_npx(double n, double r, bool permutation, double *out)
{
    double result = 1.0;
    double k;

    if (n < 0.0 || n != floor(n) || r < 0.0 || r != floor(r)) {
        return false;
    }
    if (r > n) {
        return false;
    }
    k = permutation ? r : (r < n - r ? r : n - r);
    for (int i = 0; i < (int)k; i++) {
        result *= (n - i) / (permutation ? 1.0 : (double)(i + 1));
    }
    if (isinf(result)) {
        return false;
    }
    *out = result;
    return true;
}

/** Convert decimal degrees to the numeric D.MMSS form the FX-870P uses. */
static double calc_dms_value(double x)
{
    int sign = (x < 0.0) ? -1 : 1;
    double a = fabs(x);
    double d = floor(a);
    double mf = (a - d) * 60.0;
    double m = floor(mf);
    double s = (mf - m) * 60.0;

    return sign * (d + m / 100.0 + s / 10000.0);
}

/** Format decimal degrees as a `Dd MM' SS.ss"` string. */
static void calc_dms_string(double x, char *out, size_t size)
{
    int sign = (x < 0.0) ? -1 : 1;
    double a = fabs(x);
    double d = floor(a);
    double mf = (a - d) * 60.0;
    double m = floor(mf);
    double s = (mf - m) * 60.0;

    snprintf(out, size, "%s%lldd%02lld'%.2f\"", sign < 0 ? "-" : "",
             (long long)d, (long long)m, s);
}

/** BASIC VAL: parse the leading number, ignoring a non-numeric tail. */
static double calc_val_number(const char *text)
{
    const char *p = text;
    char *end = NULL;

    while (*p != '\0' && isspace((unsigned char)*p)) {
        p++;
    }
    errno = 0;
    double value = strtod(p, &end);
    if (end == p) {
        return 0.0;
    }
    return value;
}

/**
 * Dispatch a function call. The opening `(` has been consumed and the closing
 * `)` is next. Arguments are parsed into @p args, with @p arg_count holding
 * how many were parsed (each function validates its own arity).
 */
static void calc_parse_function_body(calc_parser_t *parser, const char *name,
                                     calc_value_t *out)
{
    calc_value_t args[3];
    int arg_count = 0;
    double x, y, z;

    if (*parser->cursor != '(') {
        calc_expr_fail(parser, "function call needs '('");
        return;
    }
    parser->cursor++;

    if (*parser->cursor != ')') {
        for (;;) {
            if (arg_count >= (int)(sizeof(args) / sizeof(args[0]))) {
                calc_expr_fail(parser, "too many function arguments");
                return;
            }
            calc_parse_additive(parser, &args[arg_count]);
            if (parser->failed) {
                return;
            }
            arg_count++;
            calc_parse_skip_space(parser);
            if (*parser->cursor == ',') {
                parser->cursor++;
                continue;
            }
            break;
        }
    }
    if (*parser->cursor != ')') {
        calc_expr_fail(parser, "missing ')'");
        return;
    }
    parser->cursor++;

    if (strncasecmp(name, "ABS", 3) == 0) {
        if (arg_count != 1) { calc_expr_fail(parser, "ABS takes 1 argument"); return; }
        if (!calc_arg_number(parser, &args[0], &x)) { return; }
        calc_set_number(out, fabs(x));
        return;
    }
    if (strncasecmp(name, "SGN", 3) == 0) {
        if (arg_count != 1) { calc_expr_fail(parser, "SGN takes 1 argument"); return; }
        if (!calc_arg_number(parser, &args[0], &x)) { return; }
        calc_set_number(out, x > 0.0 ? 1.0 : (x < 0.0 ? -1.0 : 0.0));
        return;
    }
    if (strncasecmp(name, "INT", 3) == 0) {
        if (arg_count != 1) { calc_expr_fail(parser, "INT takes 1 argument"); return; }
        if (!calc_arg_number(parser, &args[0], &x)) { return; }
        calc_set_number(out, floor(x));
        return;
    }
    if (strncasecmp(name, "FIX", 3) == 0) {
        if (arg_count != 1) { calc_expr_fail(parser, "FIX takes 1 argument"); return; }
        if (!calc_arg_number(parser, &args[0], &x)) { return; }
        calc_set_number(out, trunc(x));
        return;
    }
    if (strncasecmp(name, "FRAC", 4) == 0) {
        if (arg_count != 1) { calc_expr_fail(parser, "FRAC takes 1 argument"); return; }
        if (!calc_arg_number(parser, &args[0], &x)) { return; }
        calc_set_number(out, x - floor(x));
        return;
    }
    if (strncasecmp(name, "ROUND", 5) == 0) {
        double scale;
        if (arg_count < 1 || arg_count > 2) { calc_expr_fail(parser, "ROUND takes 1 or 2 arguments"); return; }
        if (!calc_arg_number(parser, &args[0], &x)) { return; }
        if (arg_count == 2) {
            if (!calc_arg_number(parser, &args[1], &y)) { return; }
            scale = pow(10.0, floor(y));
        } else {
            scale = 1.0;
        }
        if (isinf(scale) || scale == 0.0) {
            calc_expr_fail(parser, "ROUND precision out of range");
            return;
        }
        calc_set_number(out, (x >= 0.0) ? floor(x * scale + 0.5) / scale
                                        : ceil(x * scale - 0.5) / scale);
        return;
    }
    if (strncasecmp(name, "SQR", 3) == 0) {
        if (arg_count != 1) { calc_expr_fail(parser, "SQR takes 1 argument"); return; }
        if (!calc_arg_number(parser, &args[0], &x)) { return; }
        if (x < 0.0) { calc_expr_fail(parser, "SQR domain error"); return; }
        calc_set_number(out, sqrt(x));
        return;
    }
    if (strncasecmp(name, "EXP", 3) == 0) {
        if (arg_count != 1) { calc_expr_fail(parser, "EXP takes 1 argument"); return; }
        if (!calc_arg_number(parser, &args[0], &x)) { return; }
        calc_set_number(out, exp(x));
        return;
    }
    if (strncasecmp(name, "LN", 2) == 0) {
        if (arg_count != 1) { calc_expr_fail(parser, "LN takes 1 argument"); return; }
        if (!calc_arg_number(parser, &args[0], &x)) { return; }
        if (x <= 0.0) { calc_expr_fail(parser, "LN domain error"); return; }
        calc_set_number(out, log(x));
        return;
    }
    if (strncasecmp(name, "LOG", 3) == 0) {
        if (arg_count != 1) { calc_expr_fail(parser, "LOG takes 1 argument"); return; }
        if (!calc_arg_number(parser, &args[0], &x)) { return; }
        if (x <= 0.0) { calc_expr_fail(parser, "LOG domain error"); return; }
        calc_set_number(out, log10(x));
        return;
    }
    /* SINH/COSH/TANH must be checked before SIN/COS/TAN: the shorter names
     * are prefixes of the longer ones. */
    if (strncasecmp(name, "SINH", 4) == 0) {
        if (arg_count != 1) { calc_expr_fail(parser, "SINH takes 1 argument"); return; }
        if (!calc_arg_number(parser, &args[0], &x)) { return; }
        calc_set_number(out, sinh(x));
        return;
    }
    if (strncasecmp(name, "COSH", 4) == 0) {
        if (arg_count != 1) { calc_expr_fail(parser, "COSH takes 1 argument"); return; }
        if (!calc_arg_number(parser, &args[0], &x)) { return; }
        calc_set_number(out, cosh(x));
        return;
    }
    if (strncasecmp(name, "TANH", 4) == 0) {
        if (arg_count != 1) { calc_expr_fail(parser, "TANH takes 1 argument"); return; }
        if (!calc_arg_number(parser, &args[0], &x)) { return; }
        calc_set_number(out, tanh(x));
        return;
    }
    if (strncasecmp(name, "SIN", 3) == 0) {
        if (arg_count != 1) { calc_expr_fail(parser, "SIN takes 1 argument"); return; }
        if (!calc_arg_number(parser, &args[0], &x)) { return; }
        calc_set_number(out, sin(calc_trig_input(x)));
        return;
    }
    if (strncasecmp(name, "COS", 3) == 0) {
        if (arg_count != 1) { calc_expr_fail(parser, "COS takes 1 argument"); return; }
        if (!calc_arg_number(parser, &args[0], &x)) { return; }
        calc_set_number(out, cos(calc_trig_input(x)));
        return;
    }
    if (strncasecmp(name, "TAN", 3) == 0) {
        if (arg_count != 1) { calc_expr_fail(parser, "TAN takes 1 argument"); return; }
        if (!calc_arg_number(parser, &args[0], &x)) { return; }
        calc_set_number(out, tan(calc_trig_input(x)));
        return;
    }
    if (strncasecmp(name, "ASN", 3) == 0 || strncasecmp(name, "ASIN", 4) == 0) {
        if (arg_count != 1) { calc_expr_fail(parser, "ASN takes 1 argument"); return; }
        if (!calc_arg_number(parser, &args[0], &x)) { return; }
        if (x < -1.0 || x > 1.0) { calc_expr_fail(parser, "ASN domain error"); return; }
        calc_set_number(out, calc_trig_output(asin(x)));
        return;
    }
    if (strncasecmp(name, "ACS", 3) == 0 || strncasecmp(name, "ACOS", 4) == 0) {
        if (arg_count != 1) { calc_expr_fail(parser, "ACS takes 1 argument"); return; }
        if (!calc_arg_number(parser, &args[0], &x)) { return; }
        if (x < -1.0 || x > 1.0) { calc_expr_fail(parser, "ACS domain error"); return; }
        calc_set_number(out, calc_trig_output(acos(x)));
        return;
    }
    if (strncasecmp(name, "ATN", 3) == 0 || strncasecmp(name, "ATAN", 4) == 0) {
        if (arg_count != 1) { calc_expr_fail(parser, "ATN takes 1 argument"); return; }
        if (!calc_arg_number(parser, &args[0], &x)) { return; }
        calc_set_number(out, calc_trig_output(atan(x)));
        return;
    }
    if (strncasecmp(name, "FACT", 4) == 0) {
        if (arg_count != 1) { calc_expr_fail(parser, "FACT takes 1 argument"); return; }
        if (!calc_arg_number(parser, &args[0], &x)) { return; }
        if (!calc_factorial(x, &y)) { calc_expr_fail(parser, "FACT domain error"); return; }
        calc_set_number(out, y);
        return;
    }
    if (strncasecmp(name, "NCR", 3) == 0) {
        if (arg_count != 2) { calc_expr_fail(parser, "NCR takes 2 arguments"); return; }
        if (!calc_arg_number(parser, &args[0], &x) || !calc_arg_number(parser, &args[1], &y)) { return; }
        if (!calc_ncx_npx(x, y, false, &z)) { calc_expr_fail(parser, "NCR domain error"); return; }
        calc_set_number(out, z);
        return;
    }
    if (strncasecmp(name, "NPR", 3) == 0) {
        if (arg_count != 2) { calc_expr_fail(parser, "NPR takes 2 arguments"); return; }
        if (!calc_arg_number(parser, &args[0], &x) || !calc_arg_number(parser, &args[1], &y)) { return; }
        if (!calc_ncx_npx(x, y, true, &z)) { calc_expr_fail(parser, "NPR domain error"); return; }
        calc_set_number(out, z);
        return;
    }
    if (strncasecmp(name, "MOD", 3) == 0) {
        if (arg_count != 2) { calc_expr_fail(parser, "MOD takes 2 arguments"); return; }
        if (!calc_arg_number(parser, &args[0], &x) || !calc_arg_number(parser, &args[1], &y)) { return; }
        if (!calc_floor_mod(x, y, &z)) { calc_expr_fail(parser, "divide by zero"); return; }
        calc_set_number(out, z);
        return;
    }
    if (strncasecmp(name, "DMS", 3) == 0 && name[3] != '$') {
        if (arg_count != 1) { calc_expr_fail(parser, "DMS takes 1 argument"); return; }
        if (!calc_arg_number(parser, &args[0], &x)) { return; }
        calc_set_number(out, calc_dms_value(x));
        return;
    }
    if (strncasecmp(name, "DMS$", 4) == 0) {
        char text[CALC_STR_BYTES];
        if (arg_count != 1) { calc_expr_fail(parser, "DMS$ takes 1 argument"); return; }
        if (!calc_arg_number(parser, &args[0], &x)) { return; }
        calc_dms_string(x, text, sizeof(text));
        calc_set_string(out, text);
        return;
    }
    if (strncasecmp(name, "VAL", 3) == 0 && name[3] != 'F') {
        char text[CALC_STR_BYTES];
        if (arg_count != 1) { calc_expr_fail(parser, "VAL takes 1 argument"); return; }
        if (!calc_arg_string(parser, &args[0], text, sizeof(text))) { return; }
        calc_set_number(out, calc_val_number(text));
        return;
    }
    if (strncasecmp(name, "VALF", 4) == 0) {
        char text[CALC_STR_BYTES];
        if (arg_count != 1) { calc_expr_fail(parser, "VALF takes 1 argument"); return; }
        if (!calc_arg_string(parser, &args[0], text, sizeof(text))) { return; }
        calc_set_number(out, calc_val_number(text));
        return;
    }
    if (strncasecmp(name, "STR$", 4) == 0) {
        char text[CALC_STR_BYTES];
        if (arg_count != 1) { calc_expr_fail(parser, "STR$ takes 1 argument"); return; }
        if (!calc_arg_number(parser, &args[0], &x)) { return; }
        calc_value_to_string(&args[0], text, sizeof(text));
        calc_set_string(out, text);
        return;
    }
    if (strncasecmp(name, "HEX$", 4) == 0) {
        char text[24];
        if (arg_count != 1) { calc_expr_fail(parser, "HEX$ takes 1 argument"); return; }
        if (!calc_arg_number(parser, &args[0], &x)) { return; }
        if (x != floor(x) || fabs(x) > 9.2233720368547758e18) {
            calc_expr_fail(parser, "HEX$ needs an integer");
            return;
        }
        snprintf(text, sizeof(text), "%llX", (unsigned long long)(long long)x);
        calc_set_string(out, text);
        return;
    }
    if (strncasecmp(name, "ASC", 3) == 0) {
        char text[CALC_STR_BYTES];
        if (arg_count != 1) { calc_expr_fail(parser, "ASC takes 1 argument"); return; }
        if (!calc_arg_string(parser, &args[0], text, sizeof(text))) { return; }
        if (text[0] == '\0') { calc_expr_fail(parser, "ASC needs a non-empty string"); return; }
        calc_set_number(out, (unsigned char)text[0]);
        return;
    }
    if (strncasecmp(name, "CHR$", 4) == 0) {
        char text[2];
        if (arg_count != 1) { calc_expr_fail(parser, "CHR$ takes 1 argument"); return; }
        if (!calc_arg_number(parser, &args[0], &x)) { return; }
        if (x < 0.0 || x > 255.0 || x != floor(x)) {
            calc_expr_fail(parser, "CHR$ needs an integer 0..255");
            return;
        }
        text[0] = (char)(int)x;
        text[1] = '\0';
        calc_set_string(out, text);
        return;
    }
    if (strncasecmp(name, "LEN", 3) == 0) {
        char text[CALC_STR_BYTES];
        if (arg_count != 1) { calc_expr_fail(parser, "LEN takes 1 argument"); return; }
        if (!calc_arg_string(parser, &args[0], text, sizeof(text))) { return; }
        calc_set_number(out, (double)strlen(text));
        return;
    }
    if (strncasecmp(name, "LEFT$", 5) == 0) {
        char text[CALC_STR_BYTES];
        char result[CALC_STR_BYTES];
        int take;
        if (arg_count != 2) { calc_expr_fail(parser, "LEFT$ takes 2 arguments"); return; }
        if (!calc_arg_string(parser, &args[0], text, sizeof(text))) { return; }
        if (!calc_arg_number(parser, &args[1], &x)) { return; }
        take = (int)x;
        if (take < 0) { calc_expr_fail(parser, "LEFT$ length must be >= 0"); return; }
        if (take > (int)strlen(text)) {
            take = (int)strlen(text);
        }
        snprintf(result, sizeof(result), "%.*s", take, text);
        calc_set_string(out, result);
        return;
    }
    if (strncasecmp(name, "RIGHT$", 6) == 0) {
        char text[CALC_STR_BYTES];
        char result[CALC_STR_BYTES];
        size_t len, take;
        if (arg_count != 2) { calc_expr_fail(parser, "RIGHT$ takes 2 arguments"); return; }
        if (!calc_arg_string(parser, &args[0], text, sizeof(text))) { return; }
        if (!calc_arg_number(parser, &args[1], &x)) { return; }
        if (x < 0.0) { calc_expr_fail(parser, "RIGHT$ length must be >= 0"); return; }
        len = strlen(text);
        take = ((double)len > x) ? (size_t)x : len;
        snprintf(result, sizeof(result), "%s", text + len - take);
        calc_set_string(out, result);
        return;
    }
    if (strncasecmp(name, "MID$", 4) == 0) {
        char text[CALC_STR_BYTES];
        char result[CALC_STR_BYTES];
        size_t len, start, take;
        if (arg_count < 2 || arg_count > 3) {
            calc_expr_fail(parser, "MID$ takes 2 or 3 arguments");
            return;
        }
        if (!calc_arg_string(parser, &args[0], text, sizeof(text))) { return; }
        if (!calc_arg_number(parser, &args[1], &x)) { return; }
        if (x < 1.0) { calc_expr_fail(parser, "MID$ start must be >= 1"); return; }
        len = strlen(text);
        start = (size_t)x - 1;
        if (start >= len) {
            result[0] = '\0';
        } else {
            if (arg_count == 3) {
                if (!calc_arg_number(parser, &args[2], &y)) { return; }
                if (y < 0.0) { calc_expr_fail(parser, "MID$ length must be >= 0"); return; }
                take = (size_t)y;
            } else {
                take = len;
            }
            if (take > len - start) {
                take = len - start;
            }
            snprintf(result, sizeof(result), "%.*s", (int)take, text + start);
        }
        calc_set_string(out, result);
        return;
    }
    if (strncasecmp(name, "POL", 3) == 0) {
        double r, t;
        char rbuf[32];
        char tbuf[32];
        if (arg_count != 2) { calc_expr_fail(parser, "POL takes 2 arguments"); return; }
        if (!calc_arg_number(parser, &args[0], &x) || !calc_arg_number(parser, &args[1], &y)) { return; }
        r = sqrt(x * x + y * y);
        t = calc_trig_output(atan2(y, x));
        calc_format_number(r, rbuf, sizeof(rbuf));
        calc_format_number(t, tbuf, sizeof(tbuf));
        (void)shell_env_set("X", rbuf);
        (void)shell_env_set("Y", tbuf);
        calc_set_number(out, r);
        return;
    }
    if (strncasecmp(name, "REC", 3) == 0) {
        double xx, yy;
        char xbuf[32];
        char ybuf[32];
        if (arg_count != 2) { calc_expr_fail(parser, "REC takes 2 arguments"); return; }
        if (!calc_arg_number(parser, &args[0], &x) || !calc_arg_number(parser, &args[1], &y)) { return; }
        xx = x * cos(calc_trig_input(y));
        yy = x * sin(calc_trig_input(y));
        calc_format_number(xx, xbuf, sizeof(xbuf));
        calc_format_number(yy, ybuf, sizeof(ybuf));
        (void)shell_env_set("X", xbuf);
        (void)shell_env_set("Y", ybuf);
        calc_set_number(out, xx);
        return;
    }

    calc_expr_fail(parser, "unknown function");
}

/* ------------------------------------------------------------------------
 * PRIMARY: literals, variables, PI, RAN#, and function calls
 * ------------------------------------------------------------------------ */

static void calc_parse_primary(calc_parser_t *parser, calc_value_t *out)
{
    char name[32];
    size_t name_len = 0;

    calc_parse_skip_space(parser);
    if (parser->failed) {
        return;
    }

    if (*parser->cursor == '(') {
        if (parser->depth >= CALC_MAX_DEPTH) {
            calc_expr_fail(parser, "expression nests too deeply");
            return;
        }
        parser->cursor++;
        parser->depth++;
        calc_parse_additive(parser, out);
        parser->depth--;
        calc_parse_skip_space(parser);
        if (*parser->cursor != ')') {
            calc_expr_fail(parser, "missing ')'");
        } else {
            parser->cursor++;
        }
        return;
    }

    /* String literal: the shell strips the outer quoting layer from the raw
     * line, so accept both `'` and `"` as delimiters inside the expression. */
    if (*parser->cursor == '\'' || *parser->cursor == '"') {
        char quote = *parser->cursor;
        const char *start = ++parser->cursor;
        const char *end = strchr(parser->cursor, quote);

        if (end == NULL) {
            calc_expr_fail(parser, "unterminated string");
            return;
        }
        {
            size_t length = (size_t)(end - start);
            char text[CALC_STR_BYTES];

            if (length >= sizeof(text)) {
                calc_expr_fail(parser, "string literal too long");
                return;
            }
            memcpy(text, start, length);
            text[length] = '\0';
            parser->cursor = end + 1;
            calc_set_string(out, text);
        }
        return;
    }

    /* Number literal: digit, '.', '&' (hex), or 0x. */
    if (isdigit((unsigned char)*parser->cursor) || *parser->cursor == '.' ||
        *parser->cursor == '&') {
        calc_parse_number_literal(parser, out);
        return;
    }

    /* Identifier: a function call, PI / RAN#, or an environment variable. */
    if (isalpha((unsigned char)*parser->cursor) || *parser->cursor == '_') {
        while ((isalnum((unsigned char)*parser->cursor) ||
                *parser->cursor == '_' || *parser->cursor == '$' ||
                *parser->cursor == '#') && name_len + 1 < sizeof(name)) {
            name[name_len++] = *parser->cursor++;
        }
        name[name_len] = '\0';

        /* PI constant. */
        if (strcasecmp(name, "PI") == 0) {
            calc_set_number(out, CALC_PI);
            return;
        }

        /* Random number, optionally with an explicit seed. Checked before the
         * generic function dispatch so `ran#(42)` parses as a seed, not as an
         * unknown function call. */
        if (strcasecmp(name, "RAN#") == 0) {
            calc_parse_skip_space(parser);
            if (*parser->cursor == '(') {
                calc_value_t seed;
                double seed_num;

                parser->cursor++;
                calc_parse_additive(parser, &seed);
                calc_parse_skip_space(parser);
                if (*parser->cursor != ')') {
                    calc_expr_fail(parser, "missing ')'");
                    return;
                }
                parser->cursor++;
                if (!calc_value_to_number(&seed, &seed_num)) {
                    calc_expr_fail(parser, "RAN# seed must be numeric");
                    return;
                }
                calc_ran_seed((uint64_t)(int64_t)seed_num);
            }
            calc_set_number(out, calc_ran_next());
            return;
        }

        /* Function call. */
        calc_parse_skip_space(parser);
        if (*parser->cursor == '(') {
            calc_parse_function_body(parser, name, out);
            return;
        }

        /* Environment variable reference. Names are alphanumeric + underscore;
         * a `$`/`#` here means an unknown function rather than a variable. */
        if (strchr(name, '$') == NULL && strchr(name, '#') == NULL) {
            const char *value = shell_env_get(name);

            if (value != NULL && value[0] != '\0') {
                double number;

                /* A numeric value coerces to a number, anything else is text. */
                calc_set_string(out, value);
                if (calc_value_to_number(out, &number)) {
                    calc_set_number(out, number);
                }
                return;
            }
            /* Undefined variables are 0, matching DOS `set /a`. */
            calc_set_number(out, 0.0);
            return;
        }

        calc_expr_fail(parser, "unknown identifier");
        return;
    }

    calc_expr_fail(parser, "unexpected character");
}

/* ------------------------------------------------------------------------
 * OPERATOR LEVELS
 * ------------------------------------------------------------------------ */

/** Unary minus and plus; binds looser than power, so `-2^2` is -(2^2). */
static void calc_parse_unary(calc_parser_t *parser, calc_value_t *out)
{
    calc_parse_skip_space(parser);
    if (parser->failed) {
        return;
    }

    if (*parser->cursor == '-') {
        double x;

        parser->cursor++;
        calc_parse_unary(parser, out);
        if (!calc_arg_number(parser, out, &x)) {
            return;
        }
        calc_set_number(out, -x);
        return;
    }

    if (*parser->cursor == '+') {
        parser->cursor++;
        calc_parse_unary(parser, out);
        return;
    }

    calc_parse_power(parser, out);
}

/** Power `^`, right-associative (2^3^2 = 512), exponent parsed as a unary. */
static void calc_parse_power(calc_parser_t *parser, calc_value_t *out)
{
    calc_value_t base;
    double base_num;
    double exponent;

    calc_parse_primary(parser, &base);
    if (parser->failed) {
        return;
    }

    calc_parse_skip_space(parser);
    if (*parser->cursor != '^') {
        *out = base;
        return;
    }

    parser->cursor++;
    calc_parse_unary(parser, out);
    if (!calc_arg_number(parser, out, &exponent)) {
        return;
    }
    if (!calc_value_to_number(&base, &base_num)) {
        calc_expr_fail(parser, "numeric base expected for '^'");
        return;
    }
    if (base_num == 0.0 && exponent < 0.0) {
        calc_expr_fail(parser, "divide by zero");
        return;
    }
    calc_set_number(out, pow(base_num, exponent));
}

/** Multiplicative level: `*`, `/`, and the BASIC `MOD` keyword. */
static void calc_parse_multiplicative(calc_parser_t *parser, calc_value_t *out)
{
    calc_value_t value;
    double left;
    double right;

    calc_parse_unary(parser, &value);

    while (!parser->failed) {
        calc_parse_skip_space(parser);

        if (*parser->cursor == '*' || *parser->cursor == '/') {
            char op = *parser->cursor;

            parser->cursor++;
            if (!calc_value_to_number(&value, &left)) {
                calc_expr_fail(parser, "numeric operand expected");
                return;
            }
            calc_parse_unary(parser, &value);
            if (!calc_value_to_number(&value, &right)) {
                calc_expr_fail(parser, "numeric operand expected");
                return;
            }
            if (op == '*') {
                left = left * right;
            } else {
                if (right == 0.0) {
                    calc_expr_fail(parser, "divide by zero");
                    return;
                }
                left = left / right;
            }
            calc_set_number(&value, left);
        } else if (strncasecmp(parser->cursor, "MOD", 3) == 0 &&
                   !(isalnum((unsigned char)parser->cursor[3]) ||
                     parser->cursor[3] == '_' || parser->cursor[3] == '$')) {
            parser->cursor += 3;
            if (!calc_value_to_number(&value, &left)) {
                calc_expr_fail(parser, "numeric operand expected");
                return;
            }
            calc_parse_unary(parser, &value);
            if (!calc_value_to_number(&value, &right)) {
                calc_expr_fail(parser, "numeric operand expected");
                return;
            }
            if (!calc_floor_mod(left, right, &left)) {
                calc_expr_fail(parser, "divide by zero");
                return;
            }
            calc_set_number(&value, left);
        } else {
            break;
        }
    }

    *out = value;
}

/** Additive level: `+` / `-`; a string on either side makes `+` concatenate. */
static void calc_parse_additive(calc_parser_t *parser, calc_value_t *out)
{
    calc_value_t right;
    double ln, rn;

    calc_parse_multiplicative(parser, out);

    while (!parser->failed) {
        calc_parse_skip_space(parser);

        if (*parser->cursor == '+') {
            parser->cursor++;
            calc_parse_multiplicative(parser, &right);
            if (out->is_string || right.is_string) {
                char a[CALC_STR_BYTES];
                char b[CALC_STR_BYTES];

                calc_value_to_string(out, a, sizeof(a));
                calc_value_to_string(&right, b, sizeof(b));
                if (snprintf(out->str, sizeof(out->str), "%s%s", a, b) >= (int)sizeof(out->str)) {
                    calc_expr_fail(parser, "string result too long");
                    return;
                }
                out->is_string = true;
            } else {
                if (!calc_value_to_number(out, &ln) || !calc_value_to_number(&right, &rn)) {
                    calc_expr_fail(parser, "numeric operand expected");
                    return;
                }
                calc_set_number(out, ln + rn);
            }
        } else if (*parser->cursor == '-') {
            parser->cursor++;
            calc_parse_multiplicative(parser, &right);
            if (!calc_value_to_number(out, &ln) || !calc_value_to_number(&right, &rn)) {
                calc_expr_fail(parser, "numeric operand expected");
                return;
            }
            calc_set_number(out, ln - rn);
        } else {
            break;
        }
    }
}

/* ========================================================================
 * PUBLIC EVALUATOR
 * ======================================================================== */

bool calc_evaluate(const char *expression, calc_value_t *result_out, const char **error_out)
{
    calc_parser_t parser;
    calc_value_t value;

    if (error_out != NULL) {
        *error_out = NULL;
    }
    if (result_out != NULL) {
        result_out->is_string = false;
        result_out->num = 0.0;
        result_out->str[0] = '\0';
    }

    if (expression == NULL) {
        if (error_out != NULL) {
            *error_out = "no expression";
        }
        return false;
    }

    parser.cursor = expression;
    parser.depth = 0;
    parser.failed = false;
    parser.message = NULL;

    calc_parse_additive(&parser, &value);

    if (!parser.failed) {
        calc_parse_skip_space(&parser);
        if (*parser.cursor != '\0') {
            calc_expr_fail(&parser, "trailing characters after the expression");
        }
    }

    if (parser.failed) {
        if (error_out != NULL) {
            *error_out = parser.message;
        }
        return false;
    }

    if (result_out != NULL) {
        *result_out = value;
    }
    return true;
}

/* ========================================================================
 * `calc` COMMAND
 * ======================================================================== */

/**
 * Locate the assignment `=` at top level in an expression (outside any quoted
 * string and at parenthesis depth 0), so `calc x = mid$('a=b', 1, 1)` assigns
 * to x rather than splitting on the `=` inside the string.
 */
static const char *calc_find_assignment(const char *expression)
{
    int depth = 0;
    char quote = 0;

    for (const char *p = expression; *p != '\0'; p++) {
        if (quote != 0) {
            if (*p == quote) {
                quote = 0;
            }
            continue;
        }
        if (*p == '\'' || *p == '"') {
            quote = *p;
            continue;
        }
        if (*p == '(') {
            depth++;
            continue;
        }
        if (*p == ')' && depth > 0) {
            depth--;
            continue;
        }
        if (*p == '=' && depth == 0) {
            return p;
        }
    }
    return NULL;
}

/** Detect a top-level POL(...)/REC(...) call so the command can print the pair. */
static bool calc_expr_is_polrec(const char *expression, bool *is_pol)
{
    const char *p = expression;

    while (*p != '\0' && isspace((unsigned char)*p)) {
        p++;
    }
    if (strncasecmp(p, "pol(", 4) == 0) {
        *is_pol = true;
        return true;
    }
    if (strncasecmp(p, "rec(", 4) == 0) {
        *is_pol = false;
        return true;
    }
    return false;
}

/** Render a result as its stored text (number, string, or &H hex). */
static void calc_result_to_text(const calc_value_t *value, bool hex_mode,
                                char *out, size_t size)
{
    if (value->is_string) {
        snprintf(out, size, "%s", value->str);
    } else if (hex_mode) {
        double x = value->num;

        if (x == floor(x) && fabs(x) < 9.2233720368547758e18) {
            snprintf(out, size, "&H%llX", (unsigned long long)(long long)x);
        } else {
            calc_format_number(x, out, size);
        }
    } else {
        calc_format_number(value->num, out, size);
    }
}

int shell_command_calc_line(const char *line)
{
    char *buffer;
    char *p;
    int hex_mode = 0;
    const char *expression;
    const char *equals;
    calc_value_t result;
    const char *error = NULL;

    if (line == NULL || line[0] == '\0') {
        shell_print_usage("Usage: calc [NAME=] <expr> | calc /deg | calc /rad | calc /angle | calc /hex <expr>");
        return 2;
    }

    buffer = strdup(line);
    if (buffer == NULL) {
        shell_print_error("calc: out of memory");
        return 1;
    }
    p = shell_trim(buffer);

    if (strncasecmp(p, "calc", 4) != 0) {
        shell_print_error("calc: malformed command line");
        free(buffer);
        return 2;
    }
    p += 4;

    /* Angle-mode and hex switches. */
    for (;;) {
        while (*p != '\0' && isspace((unsigned char)*p)) {
            p++;
        }
        if (strncasecmp(p, "/deg", 4) == 0 && (p[4] == '\0' || isspace((unsigned char)p[4]))) {
            calc_set_angle_mode(true);
            shell_transcript_appendf_ansi(SH_LBL "calc.angle" SH_RST "=" SH_VAL "degrees" SH_RST "\n");
            free(buffer);
            return 0;
        }
        if (strncasecmp(p, "/rad", 4) == 0 && (p[4] == '\0' || isspace((unsigned char)p[4]))) {
            calc_set_angle_mode(false);
            shell_transcript_appendf_ansi(SH_LBL "calc.angle" SH_RST "=" SH_VAL "radians" SH_RST "\n");
            free(buffer);
            return 0;
        }
        if (strncasecmp(p, "/angle", 6) == 0 && (p[6] == '\0' || isspace((unsigned char)p[6]))) {
            shell_transcript_appendf_ansi(SH_LBL "calc.angle" SH_RST "=" SH_VAL "%s" SH_RST "\n",
                                          calc_angle_is_degrees() ? "degrees" : "radians");
            free(buffer);
            return 0;
        }
        if (strncasecmp(p, "/hex", 4) == 0 && (p[4] == '\0' || isspace((unsigned char)p[4]))) {
            hex_mode = 1;
            p += 4;
            continue;
        }
        break;
    }

    while (*p != '\0' && isspace((unsigned char)*p)) {
        p++;
    }
    if (*p == '\0') {
        shell_print_usage("Usage: calc [NAME=] <expr> | calc /deg | calc /rad | calc /angle | calc /hex <expr>");
        free(buffer);
        return 2;
    }

    expression = p;
    equals = calc_find_assignment(expression);

    if (!calc_evaluate(equals != NULL ? equals + 1 : expression, &result, &error)) {
        shell_print_error("calc: %s", error != NULL ? error : "invalid expression");
        free(buffer);
        return 1;
    }

    if (equals != NULL) {
        char name[CALC_STR_BYTES];
        char text[P4_CONFIG_ENV_VALUE_BYTES];
        size_t name_len = (size_t)(equals - expression);

        /* Extract only the variable name before the '=': the expression itself
         * (`y = 6 * 7`) must not be passed to shell_env_set, which rejects
         * names containing spaces. */
        if (name_len >= sizeof(name)) {
            name_len = sizeof(name) - 1;
        }
        memcpy(name, expression, name_len);
        name[name_len] = '\0';
        shell_trim(name);
        if (name[0] == '\0') {
            shell_print_error("calc: no variable name before '='");
            free(buffer);
            return 1;
        }

        calc_result_to_text(&result, hex_mode, text, sizeof(text));
        if (shell_env_set(name, text) != ESP_OK) {
            shell_print_error("calc: invalid variable name or environment is full");
            free(buffer);
            return 1;
        }
        shell_transcript_appendf_ansi(SH_LBL "%s" SH_RST "=" SH_VAL "%s" SH_RST "\n", name, text);
    } else {
        bool is_pol = false;

        /* A bare top-level POL/REC call shows both results (stored in X/Y). */
        if (calc_expr_is_polrec(expression, &is_pol)) {
            const char *xval = shell_env_get("X");
            const char *yval = shell_env_get("Y");

            shell_transcript_appendf_ansi(SH_LBL "%s" SH_RST "=" SH_NUM "%s" SH_RST "  "
                                          SH_LBL "%s" SH_RST "=" SH_NUM "%s" SH_RST "\n",
                                          is_pol ? "r" : "x",
                                          xval != NULL ? xval : "?",
                                          is_pol ? "t" : "y",
                                          yval != NULL ? yval : "?");
        } else if (result.is_string) {
            shell_transcript_appendf_ansi(SH_STR "%s" SH_RST "\n", result.str);
        } else {
            char text[48];

            calc_result_to_text(&result, hex_mode, text, sizeof(text));
            shell_transcript_appendf_ansi(SH_NUM "%s" SH_RST "\n", text);
        }
    }

    free(buffer);
    return 0;
}
