/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file batch_expr.c
 * @brief Integer expression evaluator behind `set /a` (+ set helpers).
 *
 * Moved verbatim out of batch.c in v0.35.6, except that the two set
 * helpers now report through batch_set_errorlevel() (same state, same
 * values) instead of writing s_errorlevel directly, and are exported
 * for shell_command_set(). The recursive-descent grammar, precedence,
 * and error semantics are unchanged.
 */

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "batch.h"
#include "shell.h"
#include "storage.h"
#include "ansi.h"
#include "ansi_palette.h"
#include "p4minishell_config.h"
#include "esp_err.h"

/* Backward-compatibility aliases (same block as batch.c). */
#define BATCH_TAG                       P4_CONFIG_SHELL_TAG
#define SHELL_PROMPT                    P4_CONFIG_SHELL_PROMPT
#define SHELL_COMMAND_BYTES             P4_CONFIG_COMMAND_BYTES
#define SHELL_SD_PATH_BYTES             P4_CONFIG_SD_PATH_BYTES
#define SHELL_ENV_VAR_MAX               P4_CONFIG_ENV_VAR_MAX
#define SHELL_ENV_NAME_BYTES            P4_CONFIG_ENV_NAME_BYTES
#define SHELL_ENV_VALUE_BYTES           P4_CONFIG_ENV_VALUE_BYTES
#define SHELL_BATCH_LINE_BYTES          P4_CONFIG_BATCH_LINE_BYTES
#define SHELL_KEY_WAIT_TIMEOUT_MS       P4_CONFIG_KEY_WAIT_TIMEOUT_MS

/* ========================================================================
 * ARITHMETIC EXPRESSIONS (set /a)
 * ========================================================================
 * A recursive-descent evaluator over 32-bit signed integers, following the
 * COMMAND.COM operator set and precedence:
 *
 *   |        bitwise or            (lowest)
 *   ^        bitwise xor
 *   &        bitwise and
 *   << >>    shifts
 *   + -      additive
 *   * / %    multiplicative
 *   - ~ !    unary
 *   ( )      grouping              (highest)
 *
 * A bare identifier reads an environment variable, and an undefined variable
 * evaluates to 0 exactly as DOS does. Numbers accept decimal, `0x` hex, and
 * leading-zero octal.
 */

/** Parser state threaded through the expression grammar. */
typedef struct {
    const char *cursor;   /**< Current read position. */
    int depth;            /**< Parenthesis nesting depth. */
    bool failed;          /**< Set on the first syntax or maths error. */
    const char *message;  /**< Human-readable reason for the failure. */
} expr_parser_t;

static int32_t shell_expr_parse_or(expr_parser_t *parser);
static int32_t shell_expr_parse_compare(expr_parser_t *parser);
static int32_t shell_expr_parse_logical_and(expr_parser_t *parser);
static int32_t shell_expr_parse_logical(expr_parser_t *parser);

/** Record the first error and stop evaluating. */
static void shell_expr_fail(expr_parser_t *parser, const char *message)
{
    if (!parser->failed) {
        parser->failed = true;
        parser->message = message;
    }
}

static void shell_expr_skip_space(expr_parser_t *parser)
{
    while (isspace((unsigned char)*parser->cursor)) {
        parser->cursor++;
    }
}

/** Consume @p text when it is next, after whitespace. */
static bool shell_expr_accept(expr_parser_t *parser, const char *text)
{
    size_t length = strlen(text);

    shell_expr_skip_space(parser);
    if (strncmp(parser->cursor, text, length) != 0) {
        return false;
    }

    parser->cursor += length;
    return true;
}

/**
 * Parse a number, a variable reference, or a parenthesised subexpression.
 */
static int32_t shell_expr_parse_primary(expr_parser_t *parser)
{
    shell_expr_skip_space(parser);

    if (parser->failed) {
        return 0;
    }

    if (*parser->cursor == '(') {
        int32_t value;

        if (parser->depth >= P4_CONFIG_SET_EXPR_DEPTH_MAX) {
            shell_expr_fail(parser, "expression nests too deeply");
            return 0;
        }

        parser->cursor++;
        parser->depth++;
        value = shell_expr_parse_logical(parser);
        parser->depth--;

        if (!shell_expr_accept(parser, ")")) {
            shell_expr_fail(parser, "missing ')'");
        }
        return value;
    }

    /* Numeric literal: strtol handles decimal, 0x hex, and octal via base 0.
     * Out-of-range literals (e.g. 2147483648) are rejected rather than
     * silently clamped, so `-2147483648/-1` is reported as an error instead
     * of evaluating to a wrapped value. */
    if (isdigit((unsigned char)*parser->cursor)) {
        char *end = NULL;
        long value;

        errno = 0;
        value = strtol(parser->cursor, &end, 0);

        if (end == parser->cursor || errno == ERANGE) {
            shell_expr_fail(parser, "malformed number");
            return 0;
        }
        parser->cursor = end;
        return (int32_t)value;
    }

    /* Variable reference. An undefined name is 0, matching DOS. */
    if (isalpha((unsigned char)*parser->cursor) || *parser->cursor == '_') {
        char name[SHELL_ENV_NAME_BYTES];
        size_t length = 0;
        const char *value;

        while ((isalnum((unsigned char)*parser->cursor) || *parser->cursor == '_') &&
               length + 1 < sizeof(name)) {
            name[length++] = *parser->cursor++;
        }
        name[length] = '\0';

        value = shell_env_get(name);
        if (value == NULL) {
            return 0;
        }

        return (int32_t)strtol(value, NULL, 0);
    }

    shell_expr_fail(parser, "unexpected character");
    return 0;
}

/** Unary minus, plus, bitwise not, and logical not. */
static int32_t shell_expr_parse_unary(expr_parser_t *parser)
{
    shell_expr_skip_space(parser);

    if (parser->failed) {
        return 0;
    }

    if (*parser->cursor == '-') {
        parser->cursor++;
        return -shell_expr_parse_unary(parser);
    }

    if (*parser->cursor == '+') {
        parser->cursor++;
        return shell_expr_parse_unary(parser);
    }

    if (*parser->cursor == '~') {
        parser->cursor++;
        return ~shell_expr_parse_unary(parser);
    }

    if (*parser->cursor == '!') {
        parser->cursor++;
        return (shell_expr_parse_unary(parser) == 0) ? 1 : 0;
    }

    return shell_expr_parse_primary(parser);
}

static int32_t shell_expr_parse_multiplicative(expr_parser_t *parser)
{
    int32_t left = shell_expr_parse_unary(parser);

    while (!parser->failed) {
        shell_expr_skip_space(parser);

        if (*parser->cursor == '*') {
            parser->cursor++;
            left = left * shell_expr_parse_unary(parser);
        } else if (*parser->cursor == '/') {
            int32_t right;

            parser->cursor++;
            right = shell_expr_parse_unary(parser);
            if (right == 0) {
                shell_expr_fail(parser, "divide by zero");
                return 0;
            }
            /* INT32_MIN / -1 overflows on two's-complement hardware. */
            if (left == INT32_MIN && right == -1) {
                shell_expr_fail(parser, "arithmetic overflow");
                return 0;
            }
            left = left / right;
        } else if (*parser->cursor == '%') {
            int32_t right;

            parser->cursor++;
            right = shell_expr_parse_unary(parser);
            if (right == 0) {
                shell_expr_fail(parser, "divide by zero");
                return 0;
            }
            if (left == INT32_MIN && right == -1) {
                shell_expr_fail(parser, "arithmetic overflow");
                return 0;
            }
            left = left % right;
        } else {
            break;
        }
    }

    return left;
}

static int32_t shell_expr_parse_additive(expr_parser_t *parser)
{
    int32_t left = shell_expr_parse_multiplicative(parser);

    while (!parser->failed) {
        shell_expr_skip_space(parser);

        if (*parser->cursor == '+') {
            parser->cursor++;
            left = left + shell_expr_parse_multiplicative(parser);
        } else if (*parser->cursor == '-') {
            parser->cursor++;
            left = left - shell_expr_parse_multiplicative(parser);
        } else {
            break;
        }
    }

    return left;
}

static int32_t shell_expr_parse_shift(expr_parser_t *parser)
{
    int32_t left = shell_expr_parse_additive(parser);

    while (!parser->failed) {
        int32_t right;

        if (shell_expr_accept(parser, "<<")) {
            right = shell_expr_parse_additive(parser);
            /* A shift of 32 or more is undefined in C; clamp to 0. */
            left = (right >= 0 && right < 32) ? (int32_t)((uint32_t)left << right) : 0;
        } else if (shell_expr_accept(parser, ">>")) {
            right = shell_expr_parse_additive(parser);
            left = (right >= 0 && right < 32) ? (left >> right) : (left < 0 ? -1 : 0);
        } else {
            break;
        }
    }

    return left;
}

static int32_t shell_expr_parse_and(expr_parser_t *parser)
{
    int32_t left = shell_expr_parse_shift(parser);

    while (!parser->failed) {
        shell_expr_skip_space(parser);

        /* A single '&' is bitwise and; '&&' is a command separator and must
         * never be consumed here. */
        if (*parser->cursor != '&' || parser->cursor[1] == '&') {
            break;
        }

        parser->cursor++;
        left = left & shell_expr_parse_shift(parser);
    }

    return left;
}

static int32_t shell_expr_parse_xor(expr_parser_t *parser)
{
    int32_t left = shell_expr_parse_and(parser);

    while (!parser->failed) {
        shell_expr_skip_space(parser);

        if (*parser->cursor != '^') {
            break;
        }

        parser->cursor++;
        left = left ^ shell_expr_parse_and(parser);
    }

    return left;
}

static int32_t shell_expr_parse_or(expr_parser_t *parser)
{
    int32_t left = shell_expr_parse_xor(parser);

    while (!parser->failed) {
        shell_expr_skip_space(parser);

        /* '||' is a command separator, so only a lone '|' is bitwise or. */
        if (*parser->cursor != '|' || parser->cursor[1] == '|') {
            break;
        }

        parser->cursor++;
        left = left | shell_expr_parse_xor(parser);
    }

    return left;
}

/**
 * Relational / equality operators: `==`, `!=`, `<`, `>`, `<=`, `>=`.
 *
 * Lower precedence than the bitwise operators, matching cmd.exe: each operand
 * is a full `|`/`^`/`&`/shift expression and the comparison yields 1 when the
 * relation holds, 0 otherwise. A single `<`/`>` is never confused with the
 * shift operators `<<`/`>>`, which a tighter precedence level consumes first.
 */
static int32_t shell_expr_parse_compare(expr_parser_t *parser)
{
    int32_t left = shell_expr_parse_or(parser);

    while (!parser->failed) {
        shell_expr_skip_space(parser);

        if (shell_expr_accept(parser, "==")) {
            left = (left == shell_expr_parse_or(parser)) ? 1 : 0;
        } else if (shell_expr_accept(parser, "!=")) {
            left = (left != shell_expr_parse_or(parser)) ? 1 : 0;
        } else if (shell_expr_accept(parser, "<=")) {
            left = (left <= shell_expr_parse_or(parser)) ? 1 : 0;
        } else if (shell_expr_accept(parser, ">=")) {
            left = (left >= shell_expr_parse_or(parser)) ? 1 : 0;
        } else if (shell_expr_accept(parser, "<")) {
            left = (left < shell_expr_parse_or(parser)) ? 1 : 0;
        } else if (shell_expr_accept(parser, ">")) {
            left = (left > shell_expr_parse_or(parser)) ? 1 : 0;
        } else {
            break;
        }
    }

    return left;
}

/**
 * Logical `&&` — binds tighter than `||`, matching cmd.exe.
 *
 * Returns 1/0. Both sides are always evaluated (no short-circuiting), so
 * `0 && 1/0` still reports a divide-by-zero error. The right operand is
 * parsed into a local before combining: a C `&&` would short-circuit and skip
 * consuming it, leaving a dangling tail that fails the expression.
 */
static int32_t shell_expr_parse_logical_and(expr_parser_t *parser)
{
    int32_t left = shell_expr_parse_compare(parser);

    while (!parser->failed) {
        shell_expr_skip_space(parser);

        if (shell_expr_accept(parser, "&&")) {
            int32_t right = shell_expr_parse_compare(parser);
            left = ((left != 0) && (right != 0)) ? 1 : 0;
        } else {
            break;
        }
    }

    return left;
}

/**
 * Logical `||` — the lowest-precedence operator in the grammar.
 *
 * Returns 1/0, evaluating both sides (no short-circuiting), matching
 * cmd.exe. From the shell an expression using `&&`/`||` must be quoted: an
 * unquoted `&&`/`||` is a command-chain separator and is split before the
 * command ever runs.
 */
static int32_t shell_expr_parse_logical(expr_parser_t *parser)
{
    int32_t left = shell_expr_parse_logical_and(parser);

    while (!parser->failed) {
        shell_expr_skip_space(parser);

        if (shell_expr_accept(parser, "||")) {
            int32_t right = shell_expr_parse_logical_and(parser);
            left = ((left != 0) || (right != 0)) ? 1 : 0;
        } else {
            break;
        }
    }

    return left;
}

bool shell_expr_evaluate(const char *expression, int32_t *result_out, const char **error_out)
{
    expr_parser_t parser;
    int32_t value;

    if (error_out != NULL) {
        *error_out = NULL;
    }
    if (result_out != NULL) {
        *result_out = 0;
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

    value = shell_expr_parse_logical(&parser);

    if (!parser.failed) {
        shell_expr_skip_space(&parser);
        if (*parser.cursor != '\0') {
            shell_expr_fail(&parser, "trailing characters after the expression");
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

/**
 * Locate the assignment `=` in a `set /a` statement.
 *
 * The statement is `NAME[OP]=<expression>`; the expression may itself contain
 * comparison operators (`==`, `!=`, `<=`, `>=`), so a plain strchr would split
 * on the first `=` of a comparison. This scans left to right and treats an `=`
 * as the assignment only when it is not part of a comparison operator:
 *   - `==`, `!=`, `<=`, `>=` are skipped (their `=` is comparison syntax);
 *   - `<<=` / `>>=` are compound shift assignments (their `=` IS the one);
 *   - anything else is the assignment (`=`, or compound `+= -= *= /= %= &= |= ^=`).
 *
 * @return Pointer to the assignment `=`, or NULL when the statement is a pure
 *         expression with no assignment (which `set /a` then evaluates and
 *         prints rather than storing).
 */
static char *shell_expr_find_assignment(char *statement)
{
    char *p;

    for (p = statement; *p != '\0'; p++) {
        if (*p != '=') {
            continue;
        }

        {
            char prev = (p > statement) ? p[-1] : '\0';
            char next = p[1];

            if (prev == '=' || prev == '!') {
                continue;   /* second of `==`, or the `=` of `!=` */
            }

            if (prev == '<' || prev == '>') {
                if (p > statement + 1 && p[-2] == prev) {
                    return p;   /* `<<=` or `>>=` : compound shift assignment */
                }
                continue;   /* `<=` or `>=` : comparison, not the assignment */
            }

            if (next == '=') {
                p++;        /* first of `==` : skip the pair */
                continue;
            }

            return p;       /* plain `=` or compound `+= -= *= /= %= &= |= ^=` */
        }
    }

    return NULL;
}

/**
 * `set /a` — evaluate an arithmetic expression and store the result.
 *
 * Usage: set /a NAME=<expression>
 *        set /a <expression>          (prints the result without storing)
 *
 * Compound assignment operators (`+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`,
 * `^=`, `<<=`, `>>=`) are supported, as in DOS. Expressions may also use the
 * comparison operators `==`, `!=`, `<`, `>`, `<=`, `>=` (1 when true, else 0)
 * and the logical operators `&&`, `||` (1/0), following cmd.exe precedence.
 */
void shell_command_set_arithmetic(int argc, char **argv)
{
    char statement[SHELL_ENV_NAME_BYTES + SHELL_ENV_VALUE_BYTES];
    char name[SHELL_ENV_NAME_BYTES];
    char value_text[32];
    const char *error = NULL;
    char *equals;
    char *expression;
    int32_t result = 0;

    if (argc < 3) {
        shell_print_usage("Usage: set /a NAME=<expression>");
        shell_transcript_append_text("  Operators: + - * / % & | ^ ~ ! << >> == != < > <= >= && || ( )\n");
        batch_set_errorlevel(1);
        return;
    }

    /* Rejoin so an unquoted expression with spaces still parses. */
    shell_join_args(argv, 2, argc, statement, sizeof(statement));

    /* Find the assignment '=' that is not part of a comparison or a
     * compound operator's own character. */
    equals = shell_expr_find_assignment(statement);

    if (equals == NULL) {
        /* No assignment: evaluate and report, leaving the environment alone. */
        if (!shell_expr_evaluate(statement, &result, &error)) {
            shell_transcript_appendf("set: %s\n", error != NULL ? error : "invalid expression");
            batch_set_errorlevel(1);
            return;
        }

        shell_transcript_appendf_ansi(SH_NUM "%ld" SH_RST "\n", (long)result);
        batch_set_errorlevel(0);
        return;
    }

    /* Detect a compound assignment: the character before '=' is an operator. */
    {
        char op = '\0';
        char *name_end = equals;
        bool shift_op = false;

        if (name_end > statement) {
            char previous = name_end[-1];

            if (previous == '+' || previous == '-' || previous == '*' || previous == '/' ||
                previous == '%' || previous == '&' || previous == '|' || previous == '^') {
                op = previous;
                name_end--;
            } else if (previous == '<' || previous == '>') {
                /* '<<=' and '>>=' need both characters removed. */
                if (name_end - 1 > statement && name_end[-2] == previous) {
                    op = previous;
                    shift_op = true;
                    name_end -= 2;
                }
            }
        }

        *name_end = '\0';
        expression = equals + 1;

        snprintf(name, sizeof(name), "%s", shell_trim(statement));
        if (name[0] == '\0') {
            shell_transcript_append_text("set: /a needs a variable name before '='\n");
            batch_set_errorlevel(1);
            return;
        }

        if (!shell_expr_evaluate(expression, &result, &error)) {
            shell_transcript_appendf("set: %s\n", error != NULL ? error : "invalid expression");
            batch_set_errorlevel(1);
            return;
        }

        /* Apply the compound operator against the variable's current value. */
        if (op != '\0') {
            const char *current_text = shell_env_get(name);
            int32_t current = (current_text != NULL) ? (int32_t)strtol(current_text, NULL, 0) : 0;

            switch (op) {
            case '+': result = current + result; break;
            case '-': result = current - result; break;
            case '*': result = current * result; break;
            case '/':
                if (result == 0) {
                    shell_transcript_append_text("set: divide by zero\n");
                    batch_set_errorlevel(1);
                    return;
                }
                result = current / result;
                break;
            case '%':
                if (result == 0) {
                    shell_transcript_append_text("set: divide by zero\n");
                    batch_set_errorlevel(1);
                    return;
                }
                result = current % result;
                break;
            case '&': result = current & result; break;
            case '|': result = current | result; break;
            case '^': result = current ^ result; break;
            case '<':
                result = (shift_op && result >= 0 && result < 32)
                             ? (int32_t)((uint32_t)current << result) : 0;
                break;
            case '>':
                result = (shift_op && result >= 0 && result < 32)
                             ? (current >> result) : (current < 0 ? -1 : 0);
                break;
            default:
                break;
            }
        }
    }

    snprintf(value_text, sizeof(value_text), "%ld", (long)result);
    if (shell_env_set(name, value_text) != ESP_OK) {
        shell_print_error("set: invalid variable name or environment is full");
        batch_set_errorlevel(1);
        return;
    }

    shell_transcript_appendf_ansi(SH_LBL "%s" SH_RST "=" SH_NUM "%s" SH_RST "\n", name, value_text);
    batch_set_errorlevel(0);
}

/**
 * `set /p` — prompt the user and store the typed line in a variable.
 *
 * Usage: set /p NAME=<prompt text>
 *
 * Matches DOS: if the user submits an empty line the variable keeps its
 * previous value rather than being cleared.
 */
void shell_command_set_prompt(int argc, char **argv)
{
    char statement[SHELL_ENV_NAME_BYTES + SHELL_ENV_VALUE_BYTES];
    char name[SHELL_ENV_NAME_BYTES];
    char input[P4_CONFIG_SET_PROMPT_INPUT_BYTES];
    char *equals;
    char *prompt;
    uint32_t timeout_ms = SHELL_KEY_WAIT_TIMEOUT_MS;
    bool hidden = false;

    if (argc < 3) {
        shell_print_usage("Usage: set /p NAME=<prompt text>");
        batch_set_errorlevel(1);
        return;
    }

    shell_join_args(argv, 2, argc, statement, sizeof(statement));

    equals = strchr(statement, '=');
    if (equals == NULL) {
        shell_transcript_append_text("set: /p needs NAME=<prompt text>\n");
        batch_set_errorlevel(1);
        return;
    }

    *equals = '\0';
    prompt = equals + 1;

    snprintf(name, sizeof(name), "%s", shell_trim(statement));
    if (name[0] == '\0') {
        shell_transcript_append_text("set: /p needs a variable name before '='\n");
        batch_set_errorlevel(1);
        return;
    }

    /* `set /p NAME=< file` and pipe stages (`echo x | set /p var=`) read one
     * line from the active input-redirection source instead of prompting,
     * matching cmd.exe. The slot belongs to exactly one command and is cleared
     * after dispatch, so the line is consumed exactly once. */
    {
        char *resolved = malloc(SHELL_SD_PATH_BYTES);
        shell_sd_session_t session;

        if (resolved == NULL) {
            shell_print_error("set: out of memory reading the input source");
            batch_set_errorlevel(1);
            return;
        }

        if (storage_resolve_input_source(NULL, resolved, SHELL_SD_PATH_BYTES) == ESP_OK) {
            FILE *file = NULL;
            bool stored = false;

            if (shell_sd_begin(&session) == ESP_OK) {
                file = fopen(resolved, "r");
                if (file != NULL) {
                    if (fgets(input, sizeof(input), file) != NULL) {
                        size_t len = strlen(input);

                        while (len > 0 && (input[len - 1] == '\n' || input[len - 1] == '\r')) {
                            input[--len] = '\0';
                        }
                        if (len > 0 && shell_env_set(name, input) == ESP_OK) {
                            stored = true;
                        }
                    }
                    fclose(file);
                }
                shell_sd_end(&session, "set");
            }

            if (!stored) {
                /* An empty line leaves the variable unchanged (DOS parity); a
                 * missing/unreadable source is a real error. */
                shell_print_error("set: no data read from %s, the variable is unchanged", resolved);
                free(resolved);
                batch_set_errorlevel(1);
                return;
            }
            free(resolved);
            batch_set_errorlevel(0);
            return;
        }
        free(resolved);
    }

    if (prompt[0] != '\0') {
        /* Optional switches stripped from the displayed prompt:
         *   /T:secs  wait timeout (like `choice /T`)
         *   /P       password mode — the typed line is not echoed */
        char *slash_t = strstr(prompt, "/T:");

        if (slash_t != NULL && (slash_t == prompt || isspace((unsigned char)slash_t[-1]))) {
            long seconds = strtol(slash_t + 3, NULL, 10);
            char *token_end = slash_t;

            while (*token_end != '\0' && !isspace((unsigned char)*token_end)) {
                token_end++;
            }
            if (slash_t > prompt && isspace((unsigned char)slash_t[-1])) {
                slash_t[-1] = '\0';
                /* Collapse the gap left between the prompt and the tail. */
                memmove(slash_t - 1, token_end, strlen(token_end) + 1);
            } else {
                memmove(slash_t, token_end, strlen(token_end) + 1);
            }
            if (seconds > 0) {
                timeout_ms = (uint32_t)seconds * 1000U;
            }
        }

        {
            char *slash_p = strstr(prompt, "/P");

            if (slash_p != NULL && (slash_p == prompt || isspace((unsigned char)slash_p[-1]))) {
                char *token_end = slash_p;

                while (*token_end != '\0' && !isspace((unsigned char)*token_end)) {
                    token_end++;
                }
                if (slash_p > prompt && isspace((unsigned char)slash_p[-1])) {
                    slash_p[-1] = '\0';
                    memmove(slash_p - 1, token_end, strlen(token_end) + 1);
                } else {
                    memmove(slash_p, token_end, strlen(token_end) + 1);
                }
                hidden = true;
            }
        }

        shell_transcript_appendf("%s", prompt);
    }

    {
        bool got = hidden ? shell_read_line_hidden(input, sizeof(input), timeout_ms)
                          : shell_read_line(input, sizeof(input), timeout_ms);

        if (!got) {
            /* Cancelled, timed out, or no key source. DOS leaves the variable
             * untouched in this case, so errorlevel reports the failure
             * without destroying an existing value. */
            shell_transcript_append_text("set: no input received, the variable is unchanged\n");
            batch_set_errorlevel(1);
            return;
        }
    }

    if (input[0] == '\0') {
        /* An empty line leaves the variable as it was, matching DOS. */
        batch_set_errorlevel(1);
        return;
    }

    if (shell_env_set(name, input) != ESP_OK) {
        shell_print_error("set: invalid variable name or environment is full");
        batch_set_errorlevel(1);
        return;
    }

    batch_set_errorlevel(0);
}
