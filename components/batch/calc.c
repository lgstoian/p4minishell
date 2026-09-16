/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
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
#include <time.h>

#define CALC_STR_BYTES          P4_CONFIG_CALC_STR_BYTES
#define CALC_MAX_DEPTH          P4_CONFIG_CALC_MAX_DEPTH
#define CALC_PRINT_PRECISION    P4_CONFIG_CALC_PRINT_PRECISION
#define CALC_PI                 3.14159265358979323846264338327950288

#ifndef P4_CONFIG_CALC_ARG_MAX
#define P4_CONFIG_CALC_ARG_MAX  8
#endif
#define CALC_ARG_MAX            P4_CONFIG_CALC_ARG_MAX

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

/* ========================================================================
 * FINANCIAL CORE (HP-12C sign conventions: cash out negative, in positive)
 * ========================================================================
 * All five TVM functions share one annuity equation. With rate r, periods n,
 * payment pmt, present pv, future fv and type t (0 = end, 1 = beginning):
 *
 *   pv*(1+r)^n + pmt*(1+r*t)*(((1+r)^n - 1)/r) + fv = 0      (r != 0)
 *   pv + pmt*n + fv = 0                                       (r == 0)
 */

/** Annuity residual: 0 when (rate, nper, pmt, pv, fv, type) is consistent. */
static double calc_annuity_residual(double rate, double nper, double pmt,
                                    double pv, double fv, double type)
{
    if (fabs(rate) < 1e-12) {
        return pv + pmt * nper + fv;
    }
    double f = pow(1.0 + rate, nper);
    return pv * f + pmt * (1.0 + rate * type) * (f - 1.0) / rate + fv;
}

static bool calc_fin_type_ok(double type)
{
    return type == 0.0 || type == 1.0;
}

static bool calc_fin_pv(double rate, double nper, double pmt, double fv,
                        double type, double *out)
{
    if (fabs(rate) < 1e-12) {
        *out = -(fv + pmt * nper);
        return true;
    }
    if (rate <= -1.0) {
        return false;
    }
    double f = pow(1.0 + rate, nper);
    *out = -(fv + pmt * (1.0 + rate * type) * (f - 1.0) / rate) / f;
    return true;
}

static bool calc_fin_fv(double rate, double nper, double pmt, double pv,
                        double type, double *out)
{
    if (fabs(rate) < 1e-12) {
        *out = -(pv + pmt * nper);
        return true;
    }
    if (rate <= -1.0) {
        return false;
    }
    double f = pow(1.0 + rate, nper);
    *out = -(pv * f + pmt * (1.0 + rate * type) * (f - 1.0) / rate);
    return true;
}

static bool calc_fin_pmt(double rate, double nper, double pv, double fv,
                         double type, double *out)
{
    if (nper == 0.0) {
        return false;
    }
    if (fabs(rate) < 1e-12) {
        *out = -(fv + pv) / nper;
        return true;
    }
    if (rate <= -1.0) {
        return false;
    }
    double f = pow(1.0 + rate, nper);
    double denom = (1.0 + rate * type) * (f - 1.0);
    if (denom == 0.0) {
        return false;
    }
    *out = -(fv + pv * f) * rate / denom;
    return true;
}

static bool calc_fin_nper(double rate, double pmt, double pv, double fv,
                          double type, double *out)
{
    if (fabs(rate) < 1e-12) {
        if (pmt == 0.0) {
            return false;
        }
        *out = -(fv + pv) / pmt;
        return true;
    }
    if (rate <= -1.0) {
        return false;
    }
    double a = pmt * (1.0 + rate * type);
    double num = a - fv * rate;
    double den = a + pv * rate;
    if (den == 0.0 || num / den <= 0.0) {
        return false;
    }
    *out = log(num / den) / log(1.0 + rate);
    return true;
}

/** Solve the annuity equation for rate with Newton's method (numeric slope).
 *  Returns false when the iteration leaves the valid domain or stalls. */
static bool calc_fin_rate(double nper, double pmt, double pv, double fv,
                          double type, double guess, double *out)
{
    double r = guess;

    if (nper <= 0.0) {
        return false;
    }

    /* pmt == 0 reduces the annuity equation to pv*(1+r)^n + fv = 0, which has
     * a closed-form root only when pv and fv are both non-zero and opposite in
     * sign. Newton would otherwise walk toward r = -1 and accept a spurious
     * root (the residual becomes denormal-small before the domain edge trips),
     * e.g. RATE(12,0,100) must be a domain error. */
    if (fabs(pmt) < 1e-12) {
        double ratio;

        if (pv == 0.0 || fv == 0.0) {
            return false;
        }
        ratio = -fv / pv;
        if (!(ratio > 0.0)) {
            return false;
        }
        r = pow(ratio, 1.0 / nper) - 1.0;
        if (!isfinite(r) || r <= -0.99999999) {
            return false;
        }
        *out = r;
        return true;
    }

    for (int i = 0; i < 100; i++) {
        if (r <= -0.99999999) {
            return false;
        }
        double fr = calc_annuity_residual(r, nper, pmt, pv, fv, type);
        if (fabs(fr) < 1e-12) {
            *out = r;
            return true;
        }
        double h = 1e-7 * (1.0 + fabs(r));
        double slope = (calc_annuity_residual(r + h, nper, pmt, pv, fv, type) -
                        calc_annuity_residual(r - h, nper, pmt, pv, fv, type)) / (2.0 * h);
        if (slope == 0.0 || !isfinite(slope)) {
            return false;
        }
        double step = fr / slope;
        r -= step;
        if (fabs(step) < 1e-12) {
            if (r <= -0.99999999) {
                return false;
            }
            *out = r;
            return true;
        }
    }
    return false;
}

/** Net present value of values[0..count) discounted at rate. */
static bool calc_fin_npv(double rate, const double *values, int count, double *out)
{
    double total = 0.0;
    double df = 1.0;

    if (rate <= -1.0) {
        return false;
    }
    for (int i = 0; i < count; i++) {
        total += values[i] / df;
        df *= (1.0 + rate);
    }
    *out = total;
    return true;
}

/** Internal rate of return: the rate with NPV == 0. Needs a sign change in
 *  the cash flows, otherwise no solution exists. */
static bool calc_fin_irr(const double *values, int count, double *out)
{
    bool positive = false;
    bool negative = false;
    double r = 0.1;

    for (int i = 0; i < count; i++) {
        if (values[i] > 0.0) {
            positive = true;
        } else if (values[i] < 0.0) {
            negative = true;
        }
    }
    if (!positive || !negative) {
        return false;
    }
    for (int i = 0; i < 100; i++) {
        double fr;
        double h;
        double slope;

        if (r <= -0.99999999) {
            return false;
        }
        if (!calc_fin_npv(r, values, count, &fr)) {
            return false;
        }
        if (fabs(fr) < 1e-9) {
            *out = r;
            return true;
        }
        h = 1e-7 * (1.0 + fabs(r));
        double up;
        double down;
        if (!calc_fin_npv(r + h, values, count, &up) ||
            !calc_fin_npv(r - h, values, count, &down)) {
            return false;
        }
        slope = (up - down) / (2.0 * h);
        if (slope == 0.0 || !isfinite(slope)) {
            return false;
        }
        double step = fr / slope;
        r -= step;
        if (fabs(step) < 1e-12) {
            if (r <= -0.99999999) {
                return false;
            }
            *out = r;
            return true;
        }
    }
    return false;
}

/* ========================================================================
 * DATE CORE (epoch-day serials: days since 1970-01-01, proleptic Gregorian)
 * ========================================================================
 * Civil algorithms after Howard Hinnant (public domain). Serials are whole
 * days; pre-1970 dates are negative and fully supported.
 */

/** Days since 1970-01-01 for a civil date (pre-validated). */
static int64_t calc_days_from_civil(int y, int m, int d)
{
    y -= m <= 2 ? 1 : 0;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned mp = (unsigned)(m + (m > 2 ? -3 : 9));
    unsigned doy = (153 * mp + 2) / 5 + (unsigned)(d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

/** Civil date for an epoch-day serial. */
static void calc_civil_from_days(int64_t z, int *yp, int *mp, int *dp)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int y = (int)yoe + (int)(era * 400);
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp2 = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp2 + 2) / 5 + 1;
    unsigned m = mp2 + (mp2 < 10 ? 3 : -9);
    *yp = y + (m <= 2 ? 1 : 0);
    *mp = (int)m;
    *dp = (int)d;
}

static bool calc_is_leap(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static int calc_month_days(int y, int m)
{
    static const int table[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    if (m < 1 || m > 12) {
        return 0;
    }
    if (m == 2 && calc_is_leap(y)) {
        return 29;
    }
    return table[m - 1];
}

/** Validate a y/m/d triple and return its serial. */
static bool calc_date_serial(int y, int m, int d, double *out)
{
    if (m < 1 || m > 12) {
        return false;
    }
    if (d < 1 || d > calc_month_days(y, m)) {
        return false;
    }
    *out = (double)calc_days_from_civil(y, m, d);
    return true;
}

/** Split a serial into y/m/d (serials round to the nearest whole day). */
static void calc_serial_parts(double serial, int *yp, int *mp, int *dp)
{
    calc_civil_from_days((int64_t)llround(serial), yp, mp, dp);
}

/** Parse an ISO 'YYYY-MM-DD' date (single-digit M/D tolerated). */
static bool calc_parse_iso_date(const char *text, double *out)
{
    int y = 0;
    int m = 0;
    int d = 0;
    int n = 0;

    if (text == NULL || sscanf(text, "%d-%d-%d%n", &y, &m, &d, &n) != 3) {
        return false;
    }
    if (text[n] != '\0') {
        return false;
    }
    return calc_date_serial(y, m, d, out);
}

/**
 * Dispatch a function call. The opening `(` has been consumed and the closing
 * `)` is next. Arguments are parsed into @p args, with @p arg_count holding
 * how many were parsed (each function validates its own arity, up to
 * CALC_ARG_MAX so financial list functions fit).
 */
static void calc_parse_function_body(calc_parser_t *parser, const char *name,
                                     calc_value_t *out)
{
    calc_value_t args[CALC_ARG_MAX];
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
    /* Exact match: `LEN` (string length) starts with `LN`, so a prefix match
     * would shadow it. */
    if (strcasecmp(name, "LN") == 0) {
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
    /* ASINH/ACOSH/ATANH must be checked before ASN/ACS/ATN: the shorter names
     * are prefixes of the longer ones. */
    if (strncasecmp(name, "ASINH", 5) == 0) {
        if (arg_count != 1) { calc_expr_fail(parser, "ASINH takes 1 argument"); return; }
        if (!calc_arg_number(parser, &args[0], &x)) { return; }
        calc_set_number(out, asinh(x));
        return;
    }
    if (strncasecmp(name, "ACOSH", 5) == 0) {
        if (arg_count != 1) { calc_expr_fail(parser, "ACOSH takes 1 argument"); return; }
        if (!calc_arg_number(parser, &args[0], &x)) { return; }
        if (x < 1.0) { calc_expr_fail(parser, "ACOSH domain error"); return; }
        calc_set_number(out, acosh(x));
        return;
    }
    if (strncasecmp(name, "ATANH", 5) == 0) {
        if (arg_count != 1) { calc_expr_fail(parser, "ATANH takes 1 argument"); return; }
        if (!calc_arg_number(parser, &args[0], &x)) { return; }
        if (x <= -1.0 || x >= 1.0) { calc_expr_fail(parser, "ATANH domain error"); return; }
        calc_set_number(out, atanh(x));
        return;
    }
    /* CUR (cube root) and DEG (sexagesimal to decimal) */
    if (strncasecmp(name, "CUR", 3) == 0) {
        if (arg_count != 1) { calc_expr_fail(parser, "CUR takes 1 argument"); return; }
        if (!calc_arg_number(parser, &args[0], &x)) { return; }
        calc_set_number(out, cbrt(x));
        return;
    }
    if (strncasecmp(name, "DEG", 3) == 0) {
        if (arg_count != 1) { calc_expr_fail(parser, "DEG takes 1 argument"); return; }
        if (!calc_arg_number(parser, &args[0], &x)) { return; }
        {
            // Integer DMMSS math: floor(frac * 100) misfires when the
            // decimal fraction sits just below an integer in binary
            // (45.30 -> 29.9999999 -> MM=29). Round to integer centi-units.
            int sign = (x < 0.0) ? -1 : 1;
            long long t = llround(fabs(x) * 10000.0);
            double d = (double)(t / 10000);
            double m = (double)((t % 10000) / 100);
            double s = (double)(t % 100);
            double result = sign * (d + m / 60.0 + s / 3600.0);
            calc_set_number(out, result);
        }
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
    /* VAL / VALF / VALB are exact names: prefix matching here mis-claimed
     * VALB (failing its 2-argument form) now that it exists. */
    if (strcasecmp(name, "VAL") == 0) {
        char text[CALC_STR_BYTES];
        if (arg_count != 1) { calc_expr_fail(parser, "VAL takes 1 argument"); return; }
        if (!calc_arg_string(parser, &args[0], text, sizeof(text))) { return; }
        calc_set_number(out, calc_val_number(text));
        return;
    }
    if (strcasecmp(name, "VALF") == 0) {
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
    /* Base conversions and unit conversions (palmtop calculator parity).
     * Exact-match compares: several of these share a first letter with an
     * older function, and the legacy arms match by prefix. */
    if (strcasecmp(name, "BIN$") == 0) {
        char text[CALC_STR_BYTES];
        char *p;
        long long v;
        if (arg_count != 1) { calc_expr_fail(parser, "BIN$ takes 1 argument"); return; }
        if (!calc_arg_number(parser, &args[0], &x)) { return; }
        /* 31 bits is the most that fits the fixed string buffer (31 digits
         * plus the terminator). */
        if (x != floor(x) || x < 0.0 || x > 2147483647.0) {
            calc_expr_fail(parser, "BIN$ needs an integer 0..2147483647");
            return;
        }
        v = (long long)x;
        p = text + sizeof(text) - 1;
        *p = '\0';
        if (v == 0) {
            *--p = '0';
        }
        while (v > 0) {
            *--p = (v & 1) ? '1' : '0';
            v >>= 1;
        }
        calc_set_string(out, p);
        return;
    }
    if (strcasecmp(name, "OCT$") == 0) {
        char text[16];
        if (arg_count != 1) { calc_expr_fail(parser, "OCT$ takes 1 argument"); return; }
        if (!calc_arg_number(parser, &args[0], &x)) { return; }
        if (x != floor(x) || x < 0.0 || x > 4294967295.0) {
            calc_expr_fail(parser, "OCT$ needs an integer 0..4294967295");
            return;
        }
        snprintf(text, sizeof(text), "%llo", (unsigned long long)(unsigned long)x);
        calc_set_string(out, text);
        return;
    }
    if (strcasecmp(name, "VALB") == 0) {
        char text[CALC_STR_BYTES];
        char *end;
        long long v;
        long base;
        if (arg_count != 2) { calc_expr_fail(parser, "VALB takes 2 arguments"); return; }
        if (!calc_arg_string(parser, &args[0], text, sizeof(text))) { return; }
        if (!calc_arg_number(parser, &args[1], &y)) { return; }
        base = (long)y;
        if (base < 2 || base > 36 || (double)base != y) {
            calc_expr_fail(parser, "VALB base must be an integer 2..36");
            return;
        }
        errno = 0;
        v = strtoll(text, &end, (int)base);
        while (*end != '\0' && isspace((unsigned char)*end)) {
            end++;
        }
        if (end == text || *end != '\0' || errno == ERANGE) {
            calc_expr_fail(parser, "VALB could not parse the string");
            return;
        }
        calc_set_number(out, (double)v);
        return;
    }
    if (strcasecmp(name, "C2F") == 0) {
        if (arg_count != 1) { calc_expr_fail(parser, "C2F takes 1 argument"); return; }
        if (!calc_arg_number(parser, &args[0], &x)) { return; }
        calc_set_number(out, x * 9.0 / 5.0 + 32.0);
        return;
    }
    if (strcasecmp(name, "F2C") == 0) {
        if (arg_count != 1) { calc_expr_fail(parser, "F2C takes 1 argument"); return; }
        if (!calc_arg_number(parser, &args[0], &x)) { return; }
        calc_set_number(out, (x - 32.0) * 5.0 / 9.0);
        return;
    }
    if (strcasecmp(name, "IN2MM") == 0) {
        if (arg_count != 1) { calc_expr_fail(parser, "IN2MM takes 1 argument"); return; }
        if (!calc_arg_number(parser, &args[0], &x)) { return; }
        calc_set_number(out, x * 25.4);
        return;
    }
    if (strcasecmp(name, "MM2IN") == 0) {
        if (arg_count != 1) { calc_expr_fail(parser, "MM2IN takes 1 argument"); return; }
        if (!calc_arg_number(parser, &args[0], &x)) { return; }
        calc_set_number(out, x / 25.4);
        return;
    }
    if (strcasecmp(name, "LB2KG") == 0) {
        if (arg_count != 1) { calc_expr_fail(parser, "LB2KG takes 1 argument"); return; }
        if (!calc_arg_number(parser, &args[0], &x)) { return; }
        calc_set_number(out, x * 0.45359237);
        return;
    }
    if (strcasecmp(name, "KG2LB") == 0) {
        if (arg_count != 1) { calc_expr_fail(parser, "KG2LB takes 1 argument"); return; }
        if (!calc_arg_number(parser, &args[0], &x)) { return; }
        calc_set_number(out, x / 0.45359237);
        return;
    }
    /* Financial functions (HP-12C conventions; exact names, no prefix
     * matching). Payments are negative when cash leaves. Optional trailing
     * args default: fv/pv 0, type 0 (end-of-period), RATE guess 0.1. */
    if (strcasecmp(name, "PV") == 0) {
        double rate, nper, pmt, fv = 0.0, type = 0.0, result;
        if (arg_count < 3 || arg_count > 5) { calc_expr_fail(parser, "PV takes 3 to 5 arguments"); return; }
        if (!calc_arg_number(parser, &args[0], &rate) ||
            !calc_arg_number(parser, &args[1], &nper) ||
            !calc_arg_number(parser, &args[2], &pmt)) { return; }
        if (arg_count >= 4 && !calc_arg_number(parser, &args[3], &fv)) { return; }
        if (arg_count >= 5 && !calc_arg_number(parser, &args[4], &type)) { return; }
        if (!calc_fin_type_ok(type)) { calc_expr_fail(parser, "PV type must be 0 or 1"); return; }
        if (!calc_fin_pv(rate, nper, pmt, fv, type, &result)) { calc_expr_fail(parser, "PV domain error"); return; }
        calc_set_number(out, result);
        return;
    }
    if (strcasecmp(name, "FV") == 0) {
        double rate, nper, pmt, pv = 0.0, type = 0.0, result;
        if (arg_count < 3 || arg_count > 5) { calc_expr_fail(parser, "FV takes 3 to 5 arguments"); return; }
        if (!calc_arg_number(parser, &args[0], &rate) ||
            !calc_arg_number(parser, &args[1], &nper) ||
            !calc_arg_number(parser, &args[2], &pmt)) { return; }
        if (arg_count >= 4 && !calc_arg_number(parser, &args[3], &pv)) { return; }
        if (arg_count >= 5 && !calc_arg_number(parser, &args[4], &type)) { return; }
        if (!calc_fin_type_ok(type)) { calc_expr_fail(parser, "FV type must be 0 or 1"); return; }
        if (!calc_fin_fv(rate, nper, pmt, pv, type, &result)) { calc_expr_fail(parser, "FV domain error"); return; }
        calc_set_number(out, result);
        return;
    }
    if (strcasecmp(name, "PMT") == 0) {
        double rate, nper, pv, fv = 0.0, type = 0.0, result;
        if (arg_count < 3 || arg_count > 5) { calc_expr_fail(parser, "PMT takes 3 to 5 arguments"); return; }
        if (!calc_arg_number(parser, &args[0], &rate) ||
            !calc_arg_number(parser, &args[1], &nper) ||
            !calc_arg_number(parser, &args[2], &pv)) { return; }
        if (arg_count >= 4 && !calc_arg_number(parser, &args[3], &fv)) { return; }
        if (arg_count >= 5 && !calc_arg_number(parser, &args[4], &type)) { return; }
        if (!calc_fin_type_ok(type)) { calc_expr_fail(parser, "PMT type must be 0 or 1"); return; }
        if (!calc_fin_pmt(rate, nper, pv, fv, type, &result)) { calc_expr_fail(parser, "PMT domain error"); return; }
        calc_set_number(out, result);
        return;
    }
    if (strcasecmp(name, "NPER") == 0) {
        double rate, pmt, pv, fv = 0.0, type = 0.0, result;
        if (arg_count < 3 || arg_count > 5) { calc_expr_fail(parser, "NPER takes 3 to 5 arguments"); return; }
        if (!calc_arg_number(parser, &args[0], &rate) ||
            !calc_arg_number(parser, &args[1], &pmt) ||
            !calc_arg_number(parser, &args[2], &pv)) { return; }
        if (arg_count >= 4 && !calc_arg_number(parser, &args[3], &fv)) { return; }
        if (arg_count >= 5 && !calc_arg_number(parser, &args[4], &type)) { return; }
        if (!calc_fin_type_ok(type)) { calc_expr_fail(parser, "NPER type must be 0 or 1"); return; }
        if (!calc_fin_nper(rate, pmt, pv, fv, type, &result)) { calc_expr_fail(parser, "NPER domain error"); return; }
        calc_set_number(out, result);
        return;
    }
    if (strcasecmp(name, "RATE") == 0) {
        double nper, pmt, pv, fv = 0.0, type = 0.0, guess = 0.1, result;
        if (arg_count < 3 || arg_count > 6) { calc_expr_fail(parser, "RATE takes 3 to 6 arguments"); return; }
        if (!calc_arg_number(parser, &args[0], &nper) ||
            !calc_arg_number(parser, &args[1], &pmt) ||
            !calc_arg_number(parser, &args[2], &pv)) { return; }
        if (arg_count >= 4 && !calc_arg_number(parser, &args[3], &fv)) { return; }
        if (arg_count >= 5 && !calc_arg_number(parser, &args[4], &type)) { return; }
        if (arg_count >= 6 && !calc_arg_number(parser, &args[5], &guess)) { return; }
        if (!calc_fin_type_ok(type)) { calc_expr_fail(parser, "RATE type must be 0 or 1"); return; }
        if (!calc_fin_rate(nper, pmt, pv, fv, type, guess, &result)) { calc_expr_fail(parser, "RATE did not converge"); return; }
        calc_set_number(out, result);
        return;
    }
    if (strcasecmp(name, "NPV") == 0) {
        double rate, result;
        double values[CALC_ARG_MAX];
        if (arg_count < 2 || arg_count > CALC_ARG_MAX) { calc_expr_fail(parser, "NPV takes 2 or more arguments"); return; }
        if (!calc_arg_number(parser, &args[0], &rate)) { return; }
        for (int i = 1; i < arg_count; i++) {
            if (!calc_arg_number(parser, &args[i], &values[i - 1])) { return; }
        }
        if (!calc_fin_npv(rate, values, arg_count - 1, &result)) { calc_expr_fail(parser, "NPV domain error"); return; }
        calc_set_number(out, result);
        return;
    }
    if (strcasecmp(name, "IRR") == 0) {
        double result;
        double values[CALC_ARG_MAX];
        if (arg_count < 2 || arg_count > CALC_ARG_MAX) { calc_expr_fail(parser, "IRR takes 2 or more arguments"); return; }
        for (int i = 0; i < arg_count; i++) {
            if (!calc_arg_number(parser, &args[i], &values[i])) { return; }
        }
        if (!calc_fin_irr(values, arg_count, &result)) { calc_expr_fail(parser, "IRR did not converge"); return; }
        calc_set_number(out, result);
        return;
    }
    if (strcasecmp(name, "SLN") == 0) {
        double cost, salvage, life;
        if (arg_count != 3) { calc_expr_fail(parser, "SLN takes 3 arguments"); return; }
        if (!calc_arg_number(parser, &args[0], &cost) ||
            !calc_arg_number(parser, &args[1], &salvage) ||
            !calc_arg_number(parser, &args[2], &life)) { return; }
        if (life <= 0.0) { calc_expr_fail(parser, "SLN life must be > 0"); return; }
        calc_set_number(out, (cost - salvage) / life);
        return;
    }
    if (strcasecmp(name, "SYD") == 0) {
        double cost, salvage, life, period;
        if (arg_count != 4) { calc_expr_fail(parser, "SYD takes 4 arguments"); return; }
        if (!calc_arg_number(parser, &args[0], &cost) ||
            !calc_arg_number(parser, &args[1], &salvage) ||
            !calc_arg_number(parser, &args[2], &life) ||
            !calc_arg_number(parser, &args[3], &period)) { return; }
        if (life <= 0.0 || period < 1.0 || period > life) { calc_expr_fail(parser, "SYD domain error"); return; }
        calc_set_number(out, (cost - salvage) * (life - period + 1.0) * 2.0 / (life * (life + 1.0)));
        return;
    }
    if (strcasecmp(name, "DB") == 0) {
        double cost, salvage, life, period, month = 12.0;
        double rate, book, dep = 0.0;
        if (arg_count < 4 || arg_count > 5) { calc_expr_fail(parser, "DB takes 4 or 5 arguments"); return; }
        if (!calc_arg_number(parser, &args[0], &cost) ||
            !calc_arg_number(parser, &args[1], &salvage) ||
            !calc_arg_number(parser, &args[2], &life) ||
            !calc_arg_number(parser, &args[3], &period)) { return; }
        if (arg_count == 5 && !calc_arg_number(parser, &args[4], &month)) { return; }
        if (cost <= 0.0 || salvage < 0.0 || salvage >= cost ||
            life <= 0.0 || period < 1.0 || period > life ||
            month < 1.0 || month > 12.0) { calc_expr_fail(parser, "DB domain error"); return; }
        rate = 1.0 - pow(salvage / cost, 1.0 / life);
        book = cost;
        for (int i = 1; (double)i <= period; i++) {
            double cap = (cost - salvage) - (cost - book);
            dep = book * rate * ((i == 1) ? month / 12.0 : 1.0);
            if (dep > cap) {
                dep = cap;
            }
            if (dep < 0.0) {
                dep = 0.0;
            }
            book -= dep;
        }
        calc_set_number(out, dep);
        return;
    }
    /* Date functions over epoch-day serials (days since 1970-01-01).
     * DOW counts 0=Sunday..6=Saturday; TODAY follows the device clock. */
    if (strcasecmp(name, "DATE") == 0) {
        double yd, md, dd, result;
        if (arg_count != 3) { calc_expr_fail(parser, "DATE takes 3 arguments"); return; }
        if (!calc_arg_number(parser, &args[0], &yd) ||
            !calc_arg_number(parser, &args[1], &md) ||
            !calc_arg_number(parser, &args[2], &dd)) { return; }
        if (yd != floor(yd) || md != floor(md) || dd != floor(dd)) { calc_expr_fail(parser, "DATE needs integers"); return; }
        if (!calc_date_serial((int)yd, (int)md, (int)dd, &result)) { calc_expr_fail(parser, "DATE domain error"); return; }
        calc_set_number(out, result);
        return;
    }
    if (strcasecmp(name, "YEAR") == 0 || strcasecmp(name, "MONTH") == 0 ||
        strcasecmp(name, "DAY") == 0 || strcasecmp(name, "DOW") == 0) {
        int y, m, d;
        if (arg_count != 1) { calc_expr_fail(parser, "YEAR/MONTH/DAY/DOW take 1 argument"); return; }
        if (!calc_arg_number(parser, &args[0], &x)) { return; }
        calc_serial_parts(x, &y, &m, &d);
        if (strcasecmp(name, "YEAR") == 0) {
            calc_set_number(out, (double)y);
        } else if (strcasecmp(name, "MONTH") == 0) {
            calc_set_number(out, (double)m);
        } else if (strcasecmp(name, "DAY") == 0) {
            calc_set_number(out, (double)d);
        } else {
            long long s = llround(x);
            calc_set_number(out, (double)(((s + 4) % 7 + 7) % 7));
        }
        return;
    }
    if (strcasecmp(name, "TODAY") == 0) {
        time_t now = time(NULL);
        struct tm tmv;
        if (arg_count != 0) { calc_expr_fail(parser, "TODAY takes no arguments"); return; }
        if (localtime_r(&now, &tmv) == NULL) { calc_expr_fail(parser, "TODAY clock error"); return; }
        calc_set_number(out, (double)calc_days_from_civil(tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday));
        return;
    }
    if (strcasecmp(name, "DATEADD") == 0) {
        if (arg_count != 2) { calc_expr_fail(parser, "DATEADD takes 2 arguments"); return; }
        if (!calc_arg_number(parser, &args[0], &x) || !calc_arg_number(parser, &args[1], &y)) { return; }
        calc_set_number(out, x + y);
        return;
    }
    if (strcasecmp(name, "DAYS") == 0) {
        if (arg_count != 2) { calc_expr_fail(parser, "DAYS takes 2 arguments"); return; }
        if (!calc_arg_number(parser, &args[0], &x) || !calc_arg_number(parser, &args[1], &y)) { return; }
        calc_set_number(out, y - x);
        return;
    }
    if (strcasecmp(name, "EOMONTH") == 0) {
        double months, result;
        int y, m, d, total;
        if (arg_count != 2) { calc_expr_fail(parser, "EOMONTH takes 2 arguments"); return; }
        if (!calc_arg_number(parser, &args[0], &x) || !calc_arg_number(parser, &args[1], &months)) { return; }
        if (months != floor(months)) { calc_expr_fail(parser, "EOMONTH months must be an integer"); return; }
        calc_serial_parts(x, &y, &m, &d);
        total = (m - 1) + (int)months;
        y += total / 12;
        m = total % 12 + 1;
        if (m <= 0) {
            m += 12;
            y -= 1;
        }
        /* C truncation (not floor) can still misplace negative totals. */
        while (m < 1) {
            m += 12;
            y -= 1;
        }
        while (m > 12) {
            m -= 12;
            y += 1;
        }
        if (!calc_date_serial(y, m, calc_month_days(y, m), &result)) { calc_expr_fail(parser, "EOMONTH domain error"); return; }
        calc_set_number(out, result);
        return;
    }
    if (strcasecmp(name, "DATEVALUE") == 0) {
        char text[CALC_STR_BYTES];
        double result;
        if (arg_count != 1) { calc_expr_fail(parser, "DATEVALUE takes 1 argument"); return; }
        if (!calc_arg_string(parser, &args[0], text, sizeof(text))) { return; }
        if (!calc_parse_iso_date(text, &result)) { calc_expr_fail(parser, "DATEVALUE needs 'YYYY-MM-DD'"); return; }
        calc_set_number(out, result);
        return;
    }
    if (strcasecmp(name, "DATESTR") == 0) {
        char text[CALC_STR_BYTES];
        int y, m, d;
        if (arg_count != 1) { calc_expr_fail(parser, "DATESTR takes 1 argument"); return; }
        if (!calc_arg_number(parser, &args[0], &x)) { return; }
        calc_serial_parts(x, &y, &m, &d);
        snprintf(text, sizeof(text), "%04d-%02d-%02d", y, m, d);
        calc_set_string(out, text);
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
        shell_print_usage("Usage: calc [NAME=] <expr> | calc /deg | calc /rad | calc /angle | calc /hex <expr> | calc /fin | calc /date");
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
        if (strncasecmp(p, "/fin", 4) == 0 && (p[4] == '\0' || isspace((unsigned char)p[4]))) {
            shell_transcript_append_text("PV(rate,nper,pmt[,fv[,type]]) FV(rate,nper,pmt[,pv[,type]])\n");
            shell_transcript_append_text("PMT(rate,nper,pv[,fv[,type]]) NPER(rate,pmt,pv[,fv[,type]])\n");
            shell_transcript_append_text("RATE(nper,pmt,pv[,fv[,type[,guess]]]) NPV(rate,v0,v1,...) IRR(v0,v1,...)\n");
            shell_transcript_append_text("SLN(cost,salvage,life) SYD(cost,salvage,life,period)\n");
            shell_transcript_append_text("DB(cost,salvage,life,period[,month])\n");
            free(buffer);
            return 0;
        }
        if (strncasecmp(p, "/date", 5) == 0 && (p[5] == '\0' || isspace((unsigned char)p[5]))) {
            shell_transcript_append_text("Serials are days since 1970-01-01. DOW: 0=Sunday..6=Saturday.\n");
            shell_transcript_append_text("DATE(y,m,d) YEAR(s) MONTH(s) DAY(s) DOW(s) TODAY()\n");
            shell_transcript_append_text("DATEADD(s,days) DAYS(a,b) EOMONTH(s,months)\n");
            shell_transcript_append_text("DATEVALUE('YYYY-MM-DD') DATESTR(s)\n");
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
        shell_print_usage("Usage: calc [NAME=] <expr> | calc /deg | calc /rad | calc /angle | calc /hex <expr> | calc /fin | calc /date");
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
