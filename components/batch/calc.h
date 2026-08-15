/**
 * @file calc.h
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
 * Supported functions: ABS SGN INT FIX FRAC ROUND MOD SQR EXP LN LOG SIN COS
 * TAN ASN ACS ATN SINH COSH TANH FACT NCR NPR PI RAN# POL REC DMS DMS$ VAL
 * VALF STR$ HEX$ ASC CHR$ LEN LEFT$ MID$ RIGHT$. Numbers accept `&H` and `0x`
 * hex literals; strings are delimited by `'` or `"` and concatenate with `+`.
 * POL/REC store their two results in the X and Y environment variables, the
 * same documented side effect the calculator's BASIC has.
 *
 * The evaluator is a pure recursive-descent parser over double / fixed-string
 * values (no transcript I/O), so `test/main/test_calc.c` can exercise it
 * without hardware.
 */

#ifndef P4MINISHELL_CALC_H
#define P4MINISHELL_CALC_H

#include <stdbool.h>
#include <stddef.h>
#include "p4minishell_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/** One calculator value: either a double or a fixed-size string. */
typedef struct {
    bool is_string;                             /**< true when @p str is valid. */
    double num;                                 /**< Numeric payload. */
    char str[P4_CONFIG_CALC_STR_BYTES];         /**< String payload. */
} calc_value_t;

/**
 * Evaluate a calculator expression (pure; reads environment variables through
 * shell_env_get for bare identifiers, and POL/REC store their results into
 * the X/Y variables via shell_env_set).
 *
 * @param expression   Expression text (without any `NAME=` assignment).
 * @param result_out   Receives the value. May be NULL.
 * @param error_out    Receives a static reason string on failure. May be NULL.
 * @return true on success, false on a syntax or domain error.
 */
bool calc_evaluate(const char *expression, calc_value_t *result_out,
                   const char **error_out);

/** @return true when trig functions operate in degrees (default). */
bool calc_angle_is_degrees(void);

/** Set the trig angle mode: true = degrees, false = radians. */
void calc_set_angle_mode(bool degrees);

/**
 * Format a double as a calculator-style result: integral values print without
 * a decimal point, fractional values use %g precision trimmed to the config
 * precision, and NaN/Inf print as text.
 *
 * @param value  Number to format.
 * @param out    Destination buffer.
 * @param size   Destination capacity (must be >= 2).
 */
void calc_format_number(double value, char *out, size_t size);

/**
 * `calc` command entry point (batch language verb, dispatched from
 * components/command/command.c). Receives the raw, unsplit command line so
 * quoted string arguments inside the expression survive the shell tokenizer.
 *
 *   calc [NAME=] <expr>      print the result or store it in NAME
 *   calc /hex <expr>         print an integral result as &H hex
 *   calc /deg | /rad         set the trig angle mode
 *   calc /angle              show the current angle mode
 *
 * @return ERRORLEVEL: 0 ok, 1 evaluation/domain/store error, 2 usage.
 */
int shell_command_calc_line(const char *line);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_CALC_H */
