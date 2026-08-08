/**
 * @file batch.c
 * @brief Batch engine, environment variables, and batch language commands.
 *
 * Owns the `.bat` interpreter: file execution with nesting, label scanning,
 * `goto`, `for` loops, the multi-stage `|` pipe operator, the RAM-only
 * environment variable table, PATH, variable expansion, errorlevel tracking,
 * `setlocal`/`endlocal` environment scoping, and every batch language
 * command. All transcript output and debug logging go through shell.c; all SD
 * access goes through storage.c; interactive keypress waits go through the
 * shell core's key queue.
 *
 * State owned here:
 *   - Environment variables and PATH (RAM-only, 24 slots)
 *   - The setlocal snapshot stack
 *   - Active batch frame stack, errorlevel, pending goto target, stop mode
 */

#include "batch.h"
#include "storage.h"
#include "shell.h"
#include "ansi_palette.h"
#include "p4minishell_config.h"
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>

/* Backward-compatibility aliases */
#define BATCH_TAG                       P4_CONFIG_SHELL_TAG
#define SHELL_PROMPT                    P4_CONFIG_SHELL_PROMPT
#define SHELL_COMMAND_BYTES             P4_CONFIG_COMMAND_BYTES
#define SHELL_SD_PATH_BYTES             P4_CONFIG_SD_PATH_BYTES
#define SHELL_ENV_VAR_MAX               P4_CONFIG_ENV_VAR_MAX
#define SHELL_ENV_NAME_BYTES            P4_CONFIG_ENV_NAME_BYTES
#define SHELL_ENV_VALUE_BYTES           P4_CONFIG_ENV_VALUE_BYTES
#define SHELL_BATCH_LINE_BYTES          P4_CONFIG_BATCH_LINE_BYTES
#define SHELL_BATCH_ARGS_MAX            P4_CONFIG_BATCH_ARGS_MAX
#define SHELL_BATCH_DEPTH_MAX           P4_CONFIG_BATCH_DEPTH_MAX
#define SHELL_BATCH_LABEL_MAX           P4_CONFIG_BATCH_LABEL_MAX
#define SHELL_BATCH_LABEL_BYTES         P4_CONFIG_BATCH_LABEL_BYTES
#define SHELL_PAUSE_DELAY_MS            P4_CONFIG_PAUSE_DELAY_MS
#define SHELL_PIPE_SETTLE_DELAY_MS      P4_CONFIG_PIPE_SETTLE_DELAY_MS
#define SHELL_PIPE_STAGE_MAX            P4_CONFIG_PIPE_STAGE_MAX
#define SHELL_SETLOCAL_DEPTH_MAX        P4_CONFIG_SETLOCAL_DEPTH_MAX
#define SHELL_KEY_WAIT_TIMEOUT_MS       P4_CONFIG_KEY_WAIT_TIMEOUT_MS

/* ========================================================================
 * TYPES
 * ======================================================================== */

/** One RAM-only environment variable slot. */
typedef struct {
    bool used;
    char name[SHELL_ENV_NAME_BYTES];
    char value[SHELL_ENV_VALUE_BYTES];
} shell_env_var_t;

/**
 * A `:label` position recorded while scanning a batch file.
 *
 * The name is bounded by P4_CONFIG_BATCH_LABEL_BYTES rather than the full
 * command width: a label is a single identifier, and sizing it at 256 bytes
 * made the label table alone 8 KB, which overflowed the command worker task
 * stack when the frame was placed on it.
 */
typedef struct {
    char name[SHELL_BATCH_LABEL_BYTES];
    long file_pos;
} shell_batch_label_t;

/** Execution context for one nested batch file. */
typedef struct shell_batch_frame {
    bool echo_enabled;
    int argc;
    int depth;
    char args[SHELL_BATCH_ARGS_MAX][SHELL_COMMAND_BYTES];
    struct shell_batch_frame *parent;
    shell_batch_label_t labels[SHELL_BATCH_LABEL_MAX];
    int label_count;
    FILE *batch_file;
    /** setlocal scopes opened by this frame, unwound when it returns. */
    int setlocal_depth;
} shell_batch_frame_t;

/**
 * How a batch frame was asked to stop.
 *
 * `exit /b` unwinds exactly one frame; a bare `exit` inside nested batch
 * files unwinds all of them, matching COMMAND.COM.
 */
typedef enum {
    BATCH_STOP_NONE = 0,   /**< Keep executing. */
    BATCH_STOP_FRAME,      /**< Leave the current batch file only. */
    BATCH_STOP_ALL,        /**< Leave every nested batch file. */
} batch_stop_mode_t;

/* ========================================================================
 * INTERNAL STATE
 * ======================================================================== */

static bool s_initialized = false;

/* Command pipeline hooks registered by command_init() */
static batch_command_ops_t s_command_ops;

/* Environment variables (RAM-only, not persisted across boots) */
static shell_env_var_t s_shell_env_vars[SHELL_ENV_VAR_MAX];

/* Batch execution state */
static shell_batch_frame_t *s_active_batch_frame;
static int s_errorlevel;
static char s_goto_label[SHELL_COMMAND_BYTES];
static bool s_goto_pending;
static batch_stop_mode_t s_stop_mode;

/**
 * setlocal environment scope stack.
 *
 * Each entry is a full snapshot of the variable table taken when `setlocal`
 * ran. `endlocal` restores the top snapshot, and any scope a batch file
 * leaves open is unwound automatically when that frame returns — exactly the
 * COMMAND.COM contract. Snapshots are heap-allocated because the table is
 * large enough that a static stack would waste RAM on a board that mostly
 * never uses setlocal.
 */
static shell_env_var_t *s_setlocal_stack[SHELL_SETLOCAL_DEPTH_MAX];
static int s_setlocal_depth;

/* ========================================================================
 * FORWARD DECLARATIONS
 * ======================================================================== */

static void shell_env_print_all(void);
static bool shell_setlocal_push(void);
static bool shell_setlocal_pop(void);
static void shell_setlocal_unwind_to(int depth);
static bool shell_is_label(const char *line);
static void shell_extract_label_name(const char *line, char *name, size_t name_size);
static long shell_find_label_pos(shell_batch_frame_t *frame, const char *label_name);
static void shell_scan_batch_labels(shell_batch_frame_t *frame, FILE *file);
static void shell_execute_for_loop(shell_batch_frame_t *frame, char *command_line);
static void batch_run_nested(char *command);

/* ========================================================================
 * COMMAND MODULE HOOKS
 * ======================================================================== */

void batch_register_command_ops(const batch_command_ops_t *ops)
{
    if (ops == NULL) {
        memset(&s_command_ops, 0, sizeof(s_command_ops));
        return;
    }

    s_command_ops = *ops;
}

/**
 * Run one nested command line through the full command pipeline.
 *
 * Every nested context (`if` bodies, `for` bodies, pipe stages, batch lines)
 * goes through here so it inherits variable expansion and redirection. The
 * hook is NULL-checked so the batch engine degrades gracefully if it is used
 * before command_init() registers the table.
 */
static void batch_run_nested(char *command)
{
    if (command == NULL) {
        return;
    }

    if (s_command_ops.execute_command == NULL) {
        shell_transcript_append_text("batch: command pipeline is not available yet\n");
        shell_record_warningf("batch", "Nested execution attempted before command_init()");
        return;
    }

    s_command_ops.execute_command(command);
}

/* ========================================================================
 * ENVIRONMENT VARIABLES AND PATH
 * ======================================================================== */

static void shell_env_normalize_name(const char *name, char *output, size_t output_size)
{
    size_t index = 0;

    if (output == NULL || output_size == 0) {
        return;
    }

    output[0] = '\0';
    if (name == NULL) {
        return;
    }

    while (name[index] != '\0' && index + 1 < output_size) {
        output[index] = (char)toupper((unsigned char)name[index]);
        index++;
    }
    output[index] = '\0';
}

static bool shell_env_name_is_valid(const char *name)
{
    size_t index;

    if (name == NULL || name[0] == '\0') {
        return false;
    }

    for (index = 0; name[index] != '\0'; index++) {
        if (!(isalnum((unsigned char)name[index]) || name[index] == '_')) {
            return false;
        }
    }

    return true;
}

static shell_env_var_t *shell_env_find_slot(const char *name)
{
    char normalized[SHELL_ENV_NAME_BYTES];
    size_t index;

    shell_env_normalize_name(name, normalized, sizeof(normalized));
    for (index = 0; index < SHELL_ENV_VAR_MAX; index++) {
        if (s_shell_env_vars[index].used && strcmp(s_shell_env_vars[index].name, normalized) == 0) {
            return &s_shell_env_vars[index];
        }
    }

    return NULL;
}

static shell_env_var_t *shell_env_find_free_slot(void)
{
    size_t index;

    for (index = 0; index < SHELL_ENV_VAR_MAX; index++) {
        if (!s_shell_env_vars[index].used) {
            return &s_shell_env_vars[index];
        }
    }

    return NULL;
}

const char *shell_env_get(const char *name)
{
    shell_env_var_t *slot = shell_env_find_slot(name);

    return slot != NULL ? slot->value : NULL;
}

esp_err_t shell_env_set(const char *name, const char *value)
{
    char normalized[SHELL_ENV_NAME_BYTES];
    shell_env_var_t *slot;

    shell_env_normalize_name(name, normalized, sizeof(normalized));
    if (!shell_env_name_is_valid(normalized)) {
        return ESP_ERR_INVALID_ARG;
    }

    slot = shell_env_find_slot(normalized);
    if (value == NULL || value[0] == '\0') {
        if (slot != NULL) {
            memset(slot, 0, sizeof(*slot));
        }
        return ESP_OK;
    }

    if (slot == NULL) {
        slot = shell_env_find_free_slot();
        if (slot == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    slot->used = true;
    snprintf(slot->name, sizeof(slot->name), "%s", normalized);
    snprintf(slot->value, sizeof(slot->value), "%s", value);
    return ESP_OK;
}

static void shell_env_print_all(void)
{
    size_t index;
    bool any = false;

    for (index = 0; index < SHELL_ENV_VAR_MAX; index++) {
        if (!s_shell_env_vars[index].used) {
            continue;
        }
        shell_transcript_appendf_ansi(SH_LBL "%s" SH_RST "=" SH_VAL "%s" SH_RST "\n", s_shell_env_vars[index].name, s_shell_env_vars[index].value);
        any = true;
    }

    if (!any) {
        shell_print_muted("No environment variables defined");
    }
}

/* ========================================================================
 * SETLOCAL / ENDLOCAL ENVIRONMENT SCOPING
 * ========================================================================
 * `setlocal` snapshots the whole variable table; `endlocal` restores it.
 * Snapshotting the table wholesale is the honest implementation here: the
 * table is a fixed-size array, so a copy is a single memcpy and restoring it
 * correctly reverts creations, modifications, and deletions in one step.
 */

/**
 * Push a snapshot of the environment onto the setlocal stack.
 * @return true on success, false when the stack is full or out of memory.
 */
static bool shell_setlocal_push(void)
{
    shell_env_var_t *snapshot;

    if (s_setlocal_depth >= SHELL_SETLOCAL_DEPTH_MAX) {
        return false;
    }

    snapshot = malloc(sizeof(s_shell_env_vars));
    if (snapshot == NULL) {
        return false;
    }

    memcpy(snapshot, s_shell_env_vars, sizeof(s_shell_env_vars));
    s_setlocal_stack[s_setlocal_depth++] = snapshot;
    return true;
}

/**
 * Restore and release the most recent setlocal snapshot.
 * @return true when a scope was popped, false when none was open.
 */
static bool shell_setlocal_pop(void)
{
    shell_env_var_t *snapshot;

    if (s_setlocal_depth <= 0) {
        return false;
    }

    snapshot = s_setlocal_stack[--s_setlocal_depth];
    s_setlocal_stack[s_setlocal_depth] = NULL;

    if (snapshot == NULL) {
        return false;
    }

    memcpy(s_shell_env_vars, snapshot, sizeof(s_shell_env_vars));
    free(snapshot);
    return true;
}

/**
 * Unwind the setlocal stack down to @p depth.
 *
 * Called when a batch frame returns so a file that opened a scope and never
 * called `endlocal` cannot leak its changes into the caller, and so the
 * snapshot allocations are always released.
 */
static void shell_setlocal_unwind_to(int depth)
{
    if (depth < 0) {
        depth = 0;
    }

    while (s_setlocal_depth > depth) {
        (void)shell_setlocal_pop();
    }
}

/**
 * Expand `%VAR%` and batch argument references.
 *
 * Quote and escape aware, following the shell's documented rules:
 *   - Inside `'...'` nothing expands; the run is fully literal.
 *   - Inside `"..."` expansion still applies, matching COMMAND.COM.
 *   - `^%` suppresses expansion for that one percent sign.
 *
 * Quoting and escape markup is preserved here because the argument tokenizer
 * strips it later. Removing it now would let an expanded value that happens
 * to contain a space or an operator be re-parsed as syntax.
 */
void shell_expand_variables(const char *input, char *output, size_t output_size)
{
    size_t out_index = 0;
    shell_quote_state_t quote = SHELL_QUOTE_NONE;

    if (output == NULL || output_size == 0) {
        return;
    }

    output[0] = '\0';
    if (input == NULL) {
        return;
    }

    while (*input != '\0' && out_index + 1 < output_size) {
        /* A caret escape passes both characters through untouched, so `^%`
         * reaches the tokenizer as a literal percent sign. */
        if (*input == P4_CONFIG_ESCAPE_CHAR && quote != SHELL_QUOTE_SINGLE && input[1] != '\0') {
            output[out_index++] = *input++;
            if (out_index + 1 < output_size) {
                output[out_index++] = *input;
            }
            input++;
            continue;
        }

        /* Track quote runs so a single-quoted section stays literal. */
        if (*input == '"' && quote != SHELL_QUOTE_SINGLE) {
            quote = (quote == SHELL_QUOTE_DOUBLE) ? SHELL_QUOTE_NONE : SHELL_QUOTE_DOUBLE;
            output[out_index++] = *input++;
            continue;
        }

        if (*input == '\'' && quote != SHELL_QUOTE_DOUBLE) {
            quote = (quote == SHELL_QUOTE_SINGLE) ? SHELL_QUOTE_NONE : SHELL_QUOTE_SINGLE;
            output[out_index++] = *input++;
            continue;
        }

        if (quote == SHELL_QUOTE_SINGLE) {
            /* Fully literal run: copy without interpreting '%'. */
            output[out_index++] = *input++;
            continue;
        }

        if (*input == '%') {
            const char *end = strchr(input + 1, '%');
            if (end != NULL) {
                size_t token_len = (size_t)(end - (input + 1));
                char token[SHELL_ENV_NAME_BYTES];
                const char *replacement = NULL;

                if (token_len == 1 && isdigit((unsigned char)input[1]) && s_active_batch_frame != NULL) {
                    int arg_index = input[1] - '1';
                    replacement = (arg_index >= 0 && arg_index < s_active_batch_frame->argc)
                                      ? s_active_batch_frame->args[arg_index]
                                      : "";
                } else if (token_len == 1 && input[1] == '0' && s_active_batch_frame != NULL) {
                    /* %0 - script name (first argument) */
                    replacement = (s_active_batch_frame->argc > 0) ? s_active_batch_frame->args[0] : "";
                } else if (token_len == 1 && input[1] == '*' && s_active_batch_frame != NULL) {
                    /* %* - all arguments */
                    static char all_args[SHELL_BATCH_LINE_BYTES];
                    all_args[0] = '\0';
                    for (int i = 0; i < s_active_batch_frame->argc; i++) {
                        if (i > 0) {
                            strncat(all_args, " ", sizeof(all_args) - strlen(all_args) - 1);
                        }
                        strncat(all_args, s_active_batch_frame->args[i], sizeof(all_args) - strlen(all_args) - 1);
                    }
                    replacement = all_args;
                } else if (token_len == 0) {
                    replacement = "%";
                } else if (token_len < sizeof(token)) {
                    memcpy(token, input + 1, token_len);
                    token[token_len] = '\0';
                    replacement = shell_env_get(token);
                }

                if (replacement != NULL) {
                    while (*replacement != '\0' && out_index + 1 < output_size) {
                        output[out_index++] = *replacement++;
                    }
                    input = end + 1;
                    continue;
                }
            }
        }

        output[out_index++] = *input++;
    }

    output[out_index] = '\0';
}

/* ========================================================================
 * ERRORLEVEL
 * ======================================================================== */

int batch_get_errorlevel(void)
{
    return s_errorlevel;
}

void batch_set_errorlevel(int level)
{
    s_errorlevel = level;
}

/* ========================================================================
 * ENVIRONMENT COMMANDS: set, path, echo
 * ======================================================================== */

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
        value = shell_expr_parse_or(parser);
        parser->depth--;

        if (!shell_expr_accept(parser, ")")) {
            shell_expr_fail(parser, "missing ')'");
        }
        return value;
    }

    /* Numeric literal: strtol handles decimal, 0x hex, and octal via base 0. */
    if (isdigit((unsigned char)*parser->cursor)) {
        char *end = NULL;
        long value = strtol(parser->cursor, &end, 0);

        if (end == parser->cursor) {
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

    value = shell_expr_parse_or(&parser);

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
 * `set /a` — evaluate an arithmetic expression and store the result.
 *
 * Usage: set /a NAME=<expression>
 *        set /a <expression>          (prints the result without storing)
 *
 * Compound assignment operators (`+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`,
 * `^=`, `<<=`, `>>=`) are supported, as in DOS.
 */
static void shell_command_set_arithmetic(int argc, char **argv)
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
        shell_transcript_append_text("  Operators: + - * / % & | ^ ~ ! << >> ( )\n");
        s_errorlevel = 1;
        return;
    }

    /* Rejoin so an unquoted expression with spaces still parses. */
    shell_join_args(argv, 2, argc, statement, sizeof(statement));

    /* Find the assignment '=' that is not part of a comparison or a
     * compound operator's own character. */
    equals = strchr(statement, '=');

    if (equals == NULL) {
        /* No assignment: evaluate and report, leaving the environment alone. */
        if (!shell_expr_evaluate(statement, &result, &error)) {
            shell_transcript_appendf("set: %s\n", error != NULL ? error : "invalid expression");
            s_errorlevel = 1;
            return;
        }

        shell_transcript_appendf_ansi(SH_NUM "%ld" SH_RST "\n", (long)result);
        s_errorlevel = 0;
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
            s_errorlevel = 1;
            return;
        }

        if (!shell_expr_evaluate(expression, &result, &error)) {
            shell_transcript_appendf("set: %s\n", error != NULL ? error : "invalid expression");
            s_errorlevel = 1;
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
                    s_errorlevel = 1;
                    return;
                }
                result = current / result;
                break;
            case '%':
                if (result == 0) {
                    shell_transcript_append_text("set: divide by zero\n");
                    s_errorlevel = 1;
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
        s_errorlevel = 1;
        return;
    }

    shell_transcript_appendf_ansi(SH_LBL "%s" SH_RST "=" SH_NUM "%s" SH_RST "\n", name, value_text);
    s_errorlevel = 0;
}

/**
 * `set /p` — prompt the user and store the typed line in a variable.
 *
 * Usage: set /p NAME=<prompt text>
 *
 * Matches DOS: if the user submits an empty line the variable keeps its
 * previous value rather than being cleared.
 */
static void shell_command_set_prompt(int argc, char **argv)
{
    char statement[SHELL_ENV_NAME_BYTES + SHELL_ENV_VALUE_BYTES];
    char name[SHELL_ENV_NAME_BYTES];
    char input[P4_CONFIG_SET_PROMPT_INPUT_BYTES];
    char *equals;
    const char *prompt;

    if (argc < 3) {
        shell_print_usage("Usage: set /p NAME=<prompt text>");
        s_errorlevel = 1;
        return;
    }

    shell_join_args(argv, 2, argc, statement, sizeof(statement));

    equals = strchr(statement, '=');
    if (equals == NULL) {
        shell_transcript_append_text("set: /p needs NAME=<prompt text>\n");
        s_errorlevel = 1;
        return;
    }

    *equals = '\0';
    prompt = equals + 1;

    snprintf(name, sizeof(name), "%s", shell_trim(statement));
    if (name[0] == '\0') {
        shell_transcript_append_text("set: /p needs a variable name before '='\n");
        s_errorlevel = 1;
        return;
    }

    if (prompt[0] != '\0') {
        shell_transcript_appendf("%s", prompt);
    }

    if (!shell_read_line(input, sizeof(input), SHELL_KEY_WAIT_TIMEOUT_MS)) {
        /* Cancelled, timed out, or no key source. DOS leaves the variable
         * untouched in this case, so errorlevel reports the failure without
         * destroying an existing value. */
        shell_transcript_append_text("set: no input received, the variable is unchanged\n");
        s_errorlevel = 1;
        return;
    }

    if (input[0] == '\0') {
        /* An empty line leaves the variable as it was, matching DOS. */
        s_errorlevel = 1;
        return;
    }

    if (shell_env_set(name, input) != ESP_OK) {
        shell_print_error("set: invalid variable name or environment is full");
        s_errorlevel = 1;
        return;
    }

    s_errorlevel = 0;
}

void shell_command_set(int argc, char **argv)
{
    char assignment[SHELL_ENV_NAME_BYTES + SHELL_ENV_VALUE_BYTES];
    char *equals;

    if (argc == 1) {
        shell_env_print_all();
        return;
    }

    /* /a and /p change the meaning of the rest of the line, so they are
     * dispatched before the plain NAME=VALUE handling. */
    if (argv[1][0] == '/' && argv[1][2] == '\0') {
        char flag = (char)toupper((unsigned char)argv[1][1]);

        if (flag == 'A') {
            shell_command_set_arithmetic(argc, argv);
            return;
        }

        if (flag == 'P') {
            shell_command_set_prompt(argc, argv);
            return;
        }
    }

    shell_join_args(argv, 1, argc, assignment, sizeof(assignment));
    equals = strchr(assignment, '=');
    if (equals == NULL) {
        const char *value = shell_env_get(assignment);
        if (value == NULL) {
            shell_print_warning("%s is not defined", assignment);
            return;
        }
        shell_transcript_appendf_ansi(SH_LBL "%s" SH_RST "=" SH_VAL "%s" SH_RST "\n", assignment, value);
        return;
    }

    *equals = '\0';
    equals++;
    if (shell_env_set(assignment, equals) != ESP_OK) {
        shell_print_error("set: invalid variable name or environment is full");
        return;
    }

    if (equals[0] == '\0') {
        shell_print_ok("Cleared %s", assignment);
    } else {
        shell_transcript_appendf_ansi(SH_LBL "%s" SH_RST "=" SH_VAL "%s" SH_RST "\n", assignment, equals);
    }
}

void shell_command_path(int argc, char **argv)
{
    char value[SHELL_ENV_VALUE_BYTES];
    const char *current;

    if (argc == 1) {
        current = shell_env_get("PATH");
        shell_transcript_appendf_ansi(SH_LBL "PATH" SH_RST "=" SH_PATH "%s" SH_RST "\n", current != NULL ? current : "");
        return;
    }

    shell_join_args(argv, 1, argc, value, sizeof(value));
    if (shell_env_set("PATH", value) != ESP_OK) {
        shell_print_error("path: failed to update PATH");
        return;
    }

    shell_transcript_appendf_ansi(SH_LBL "PATH" SH_RST "=" SH_PATH "%s" SH_RST "\n", value);
}

void shell_command_echo(int argc, char **argv)
{
    char text[SHELL_BATCH_LINE_BYTES];

    if (argc == 1) {
        shell_transcript_appendf_ansi(SH_LBL "ECHO is" SH_RST " " SH_VAL "%s" SH_RST "\n",
                                 (s_active_batch_frame != NULL && !s_active_batch_frame->echo_enabled) ? "off" : "on");
        return;
    }

    if (s_active_batch_frame != NULL && argc == 2) {
        if (shell_text_equals_ignore_case(argv[1], "on")) {
            s_active_batch_frame->echo_enabled = true;
            return;
        }
        if (shell_text_equals_ignore_case(argv[1], "off")) {
            s_active_batch_frame->echo_enabled = false;
            return;
        }
    }

    shell_join_args(argv, 1, argc, text, sizeof(text));
    shell_transcript_appendf("%s\n", text);
}

/* ========================================================================
 * BATCH FILE RESOLUTION
 * ======================================================================== */

bool shell_resolve_batch_path(const char *command_name, char *resolved_path, size_t resolved_path_size)
{
    char candidate[SHELL_SD_PATH_BYTES];
    char path_copy[SHELL_ENV_VALUE_BYTES];
    char *entry;
    char *context = NULL;
    struct stat st;
    shell_sd_session_t session;
    esp_err_t error;
    const char *path_env = shell_env_get("PATH");

    if (command_name == NULL || command_name[0] == '\0' || resolved_path == NULL || resolved_path_size == 0) {
        return false;
    }

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        return false;
    }

    error = shell_fs_resolve_path(command_name, candidate, sizeof(candidate));
    if (error == ESP_OK && shell_sd_stat_path(candidate, &st) == ESP_OK && S_ISREG(st.st_mode)) {
        snprintf(resolved_path, resolved_path_size, "%s", candidate);
        shell_sd_end(&session, "call");
        return true;
    }

    if (!shell_path_has_extension(command_name, ".bat")) {
        char with_ext[SHELL_SD_PATH_BYTES];

        snprintf(with_ext, sizeof(with_ext), "%s.bat", command_name);
        error = shell_fs_resolve_path(with_ext, candidate, sizeof(candidate));
        if (error == ESP_OK && shell_sd_stat_path(candidate, &st) == ESP_OK && S_ISREG(st.st_mode)) {
            snprintf(resolved_path, resolved_path_size, "%s", candidate);
            shell_sd_end(&session, "call");
            return true;
        }
    }

    if (path_env != NULL && path_env[0] != '\0' && !shell_path_has_directory_component(command_name)) {
        snprintf(path_copy, sizeof(path_copy), "%s", path_env);
        entry = strtok_r(path_copy, ";", &context);
        while (entry != NULL) {
            char dir_path[SHELL_SD_PATH_BYTES];

            if (shell_fs_resolve_path(entry, dir_path, sizeof(dir_path)) == ESP_OK) {
                int written = snprintf(candidate, sizeof(candidate), "%s/%s", dir_path, command_name);
                if (written > 0 && (size_t)written < sizeof(candidate) &&
                    shell_sd_stat_path(candidate, &st) == ESP_OK && S_ISREG(st.st_mode)) {
                    snprintf(resolved_path, resolved_path_size, "%s", candidate);
                    shell_sd_end(&session, "call");
                    return true;
                }

                if (!shell_path_has_extension(command_name, ".bat")) {
                    written = snprintf(candidate, sizeof(candidate), "%s/%s.bat", dir_path, command_name);
                    if (written > 0 && (size_t)written < sizeof(candidate) &&
                        shell_sd_stat_path(candidate, &st) == ESP_OK && S_ISREG(st.st_mode)) {
                        snprintf(resolved_path, resolved_path_size, "%s", candidate);
                        shell_sd_end(&session, "call");
                        return true;
                    }
                }
            }

            entry = strtok_r(NULL, ";", &context);
        }
    }

    shell_sd_end(&session, "call");
    return false;
}

/* ========================================================================
 * BATCH CONTROL FLOW: call, goto, shift, if
 * ======================================================================== */

void shell_command_call(int argc, char **argv)
{
    char batch_path[SHELL_SD_PATH_BYTES];

    if (argc < 2) {
        shell_print_usage("Usage: call <file.bat> [args]");
        return;
    }

    if (!shell_resolve_batch_path(argv[1], batch_path, sizeof(batch_path))) {
        shell_transcript_appendf("call: batch file not found %s\n", argv[1]);
        return;
    }

    shell_execute_batch_file(batch_path, argc - 2, &argv[2]);
}

void shell_command_goto(int argc, char **argv)
{
    if (argc < 2) {
        shell_print_usage("Usage: goto <label>");
        return;
    }
    if (s_active_batch_frame == NULL) {
        shell_transcript_append_text("goto: only valid inside batch files\n");
        return;
    }
    snprintf(s_goto_label, sizeof(s_goto_label), ":%s", argv[1]);
    s_goto_pending = true;
}

void shell_command_shift(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (s_active_batch_frame == NULL) {
        shell_transcript_append_text("shift: only valid inside batch files\n");
        return;
    }
    if (s_active_batch_frame->argc <= 1) return;
    for (int i = 0; i < s_active_batch_frame->argc - 1; i++) {
        memmove(s_active_batch_frame->args[i], s_active_batch_frame->args[i + 1], SHELL_COMMAND_BYTES);
    }
    s_active_batch_frame->args[s_active_batch_frame->argc - 1][0] = '\0';
    s_active_batch_frame->argc--;
}

void shell_command_if(int argc, char **argv)
{
    /* Supports: if errorlevel N command, if exist file command, if NOT ... */
    if (argc < 3) {
        shell_print_usage("Usage: if [not] errorlevel N command | if [not] exist file command");
        return;
    }

    int arg_idx = 1;
    bool not_flag = false;

    if (shell_text_equals_ignore_case(argv[arg_idx], "not")) {
        not_flag = true;
        arg_idx++;
        if (arg_idx >= argc) {
            shell_transcript_append_text("if: expected condition after 'not'\n");
            return;
        }
    }

    bool condition = false;

    if (shell_text_equals_ignore_case(argv[arg_idx], "errorlevel")) {
        arg_idx++;
        if (arg_idx >= argc) {
            shell_transcript_append_text("if: expected number after errorlevel\n");
            return;
        }
        int level = atoi(argv[arg_idx]);
        condition = (s_errorlevel >= level);
        arg_idx++;
    } else if (shell_text_equals_ignore_case(argv[arg_idx], "exist")) {
        arg_idx++;
        if (arg_idx >= argc) {
            shell_transcript_append_text("if: expected filename after exist\n");
            return;
        }
        {
            /* Resolve against the current directory and inside a guarded SD
             * session, the same as every other filesystem command. A bare
             * stat() on the raw argument fails for any relative path. */
            char resolved[SHELL_SD_PATH_BYTES];
            shell_sd_session_t session;
            struct stat st;

            condition = false;
            if (shell_fs_resolve_path(argv[arg_idx], resolved, sizeof(resolved)) == ESP_OK &&
                shell_sd_begin(&session) == ESP_OK) {
                condition = (shell_sd_stat_path(resolved, &st) == ESP_OK);
                shell_sd_end(&session, "if exist");
            }
        }
        arg_idx++;
    } else {
        /* String comparison. The tokenizer already removed the quoting, so
         * `if "%VAR%"=="yes"` arrives as a single argument `<value>==yes`
         * and `if a == b` arrives as three. Handle both spellings.
         *
         * Quoting still matters to the user: it is what keeps an empty or
         * space-containing value from collapsing the comparison into a
         * malformed expression before it reaches here. */
        char *eq = strstr(argv[arg_idx], "==");

        if (eq != NULL) {
            /* Joined form: left==right in one argument. */
            *eq = '\0';
            const char *left = argv[arg_idx];
            const char *right = eq + 2;
            condition = (strcmp(left, right) == 0);
            arg_idx++;
        } else if (arg_idx + 2 < argc && strcmp(argv[arg_idx + 1], "==") == 0) {
            /* Spaced form: left == right as three arguments. */
            condition = (strcmp(argv[arg_idx], argv[arg_idx + 2]) == 0);
            arg_idx += 3;
        } else if (arg_idx + 1 < argc && strncmp(argv[arg_idx + 1], "==", 2) == 0) {
            /* Half-spaced form: left ==right. */
            condition = (strcmp(argv[arg_idx], argv[arg_idx + 1] + 2) == 0);
            arg_idx += 2;
        } else {
            shell_print_error("if: unsupported condition");
            return;
        }
    }

    if (not_flag) condition = !condition;

    if (condition && arg_idx < argc) {
        /* Execute the rest of the line as a command */
        char cmd[SHELL_COMMAND_BYTES];
        shell_join_args(argv, arg_idx, argc, cmd, sizeof(cmd));
        batch_run_nested(cmd);
    }
}

/* ========================================================================
 * BUILT-IN COMMANDS: pause, choice, setlocal, endlocal, exit
 * ======================================================================== */

void shell_command_pause(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* A headless board (no UART console, no USB keyboard) has no way to
     * deliver a key, so fall back to the bounded delay rather than stalling
     * a batch file until the wait times out. */
    if (!shell_key_input_available()) {
        shell_transcript_appendf_ansi(SH_MUTE "Press any key to continue . . . " SH_RST "\n");
        vTaskDelay(pdMS_TO_TICKS(SHELL_PAUSE_DELAY_MS));
        return;
    }

    shell_transcript_appendf_ansi(SH_MUTE "Press any key to continue . . . " SH_RST "\n");

    shell_key_wait_begin();
    if (!shell_wait_for_key(SHELL_KEY_WAIT_TIMEOUT_MS, NULL)) {
        shell_transcript_append_text("pause: timed out waiting for a key\n");
        shell_record_warningf("pause", "Timed out waiting for a keypress");
    }
    shell_key_wait_end();
}

void shell_command_choice(int argc, char **argv)
{
    char options[SHELL_COMMAND_BYTES];
    char message[SHELL_BATCH_LINE_BYTES];
    const char *default_key = NULL;
    bool show_list = true;
    bool case_sensitive = false;
    uint32_t timeout_ms = SHELL_KEY_WAIT_TIMEOUT_MS;
    bool has_timeout_default = false;
    size_t option_count;
    size_t index;
    int chosen = -1;

    snprintf(options, sizeof(options), "YN");
    message[0] = '\0';

    /* Parse the DOS switch set. Anything that is not a switch becomes the
     * prompt text, joined with spaces. */
    for (int arg = 1; arg < argc; arg++) {
        const char *token = argv[arg];

        if (token[0] == '/' && token[1] != '\0') {
            char flag = (char)toupper((unsigned char)token[1]);
            const char *value = token + 2;

            if (*value == ':' || *value == '=') {
                value++;
            }

            switch (flag) {
            case 'C':
                if (*value == '\0') {
                    shell_transcript_append_text("choice: /C needs a key list, for example /C:YNC\n");
                    return;
                }
                snprintf(options, sizeof(options), "%s", value);
                continue;
            case 'N':
                show_list = false;
                continue;
            case 'S':
                case_sensitive = true;
                continue;
            case 'T': {
                /* /T:c,secs — default to key c after secs seconds. */
                const char *comma = strchr(value, ',');

                if (value[0] == '\0' || comma == NULL || comma == value) {
                    shell_transcript_append_text("choice: /T needs a key and seconds, for example /T:Y,10\n");
                    return;
                }

                {
                    static char timeout_key[2];
                    long seconds = strtol(comma + 1, NULL, 10);

                    timeout_key[0] = value[0];
                    timeout_key[1] = '\0';
                    default_key = timeout_key;
                    has_timeout_default = true;

                    if (seconds > 0) {
                        timeout_ms = (uint32_t)seconds * 1000U;
                    }
                }
                continue;
            }
            default:
                shell_print_error("choice: unknown option %s", token);
                shell_print_usage("Usage: choice [/C:list] [/N] [/T:c,secs] [/S] [text]");
                return;
            }
        }

        if (message[0] != '\0') {
            strncat(message, " ", sizeof(message) - strlen(message) - 1);
        }
        strncat(message, token, sizeof(message) - strlen(message) - 1);
    }

    option_count = strlen(options);
    if (option_count == 0) {
        shell_transcript_append_text("choice: the key list is empty\n");
        return;
    }

    /* Render the prompt in DOS form: "text [Y,N]?" */
    if (message[0] != '\0') {
        shell_transcript_appendf("%s ", message);
    }

    if (show_list) {
        shell_transcript_append_text("[");
        for (index = 0; index < option_count; index++) {
            shell_transcript_appendf("%s%c", index > 0 ? "," : "", options[index]);
        }
        shell_transcript_append_text("]? ");
    }

    if (!shell_key_input_available()) {
        /* No interactive source: honor an explicit /T default, otherwise
         * take the first key, and say so instead of pretending to wait. */
        chosen = 0;
        if (has_timeout_default && default_key != NULL) {
            for (index = 0; index < option_count; index++) {
                bool match = case_sensitive
                                 ? (options[index] == default_key[0])
                                 : (toupper((unsigned char)options[index]) ==
                                    toupper((unsigned char)default_key[0]));
                if (match) {
                    chosen = (int)index;
                    break;
                }
            }
        }

        shell_transcript_appendf("%c (no interactive input, using the default)\n", options[chosen]);
        s_errorlevel = chosen + 1;
        return;
    }

    shell_key_wait_begin();
    while (chosen < 0) {
        char key = '\0';

        if (!shell_wait_for_key(timeout_ms, &key)) {
            break;
        }

        for (index = 0; index < option_count; index++) {
            bool match = case_sensitive
                             ? (options[index] == key)
                             : (toupper((unsigned char)options[index]) == toupper((unsigned char)key));
            if (match) {
                chosen = (int)index;
                break;
            }
        }

        /* An unmatched key is ignored, exactly like DOS CHOICE. */
    }
    shell_key_wait_end();

    if (chosen < 0) {
        /* Timed out: fall back to the /T key when one was given, else the
         * first key in the list. */
        chosen = 0;
        if (has_timeout_default && default_key != NULL) {
            for (index = 0; index < option_count; index++) {
                bool match = case_sensitive
                                 ? (options[index] == default_key[0])
                                 : (toupper((unsigned char)options[index]) ==
                                    toupper((unsigned char)default_key[0]));
                if (match) {
                    chosen = (int)index;
                    break;
                }
            }
        }
        shell_transcript_appendf("%c (timed out)\n", options[chosen]);
    } else {
        shell_transcript_appendf("%c\n", options[chosen]);
    }

    /* DOS reports the 1-based index of the chosen key in errorlevel. */
    s_errorlevel = chosen + 1;
}

void shell_command_setlocal(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (!shell_setlocal_push()) {
        shell_print_error("setlocal: cannot nest deeper than %d scopes",
                                 SHELL_SETLOCAL_DEPTH_MAX);
        shell_record_warningf("setlocal", "setlocal nesting limit reached");
        s_errorlevel = 1;
        return;
    }

    /* Track the scope against the running frame so an unmatched setlocal is
     * unwound automatically when the batch file returns. */
    if (s_active_batch_frame != NULL) {
        s_active_batch_frame->setlocal_depth++;
    }
}

void shell_command_endlocal(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (!shell_setlocal_pop()) {
        shell_transcript_append_text("endlocal: no matching setlocal is open\n");
        shell_record_warningf("endlocal", "endlocal without a matching setlocal");
        s_errorlevel = 1;
        return;
    }

    if (s_active_batch_frame != NULL && s_active_batch_frame->setlocal_depth > 0) {
        s_active_batch_frame->setlocal_depth--;
    }
}

void shell_command_exit(int argc, char **argv)
{
    bool frame_only = false;
    int code_index = 1;
    int code;

    /* `exit /b [code]` leaves only the current batch file. */
    if (argc >= 2 && argv[1][0] == '/' && toupper((unsigned char)argv[1][1]) == 'B' && argv[1][2] == '\0') {
        frame_only = true;
        code_index = 2;
    }

    code = (argc > code_index) ? atoi(argv[code_index]) : s_errorlevel;

    if (s_active_batch_frame == NULL) {
        if (frame_only) {
            shell_transcript_append_text("exit: /b is only valid inside batch files\n");
            return;
        }
        shell_transcript_append_text("exit: use reboot to restart the board\n");
        return;
    }

    s_errorlevel = code;
    s_stop_mode = frame_only ? BATCH_STOP_FRAME : BATCH_STOP_ALL;

    /* A pending goto would otherwise be re-evaluated after this line. */
    s_goto_pending = false;
    s_goto_label[0] = '\0';

    shell_transcript_appendf("exit: leaving %s (errorlevel %d)\n",
                             frame_only ? "this batch file" : "all batch files",
                             code);
}

/* ========================================================================
 * PIPE SUPPORT
 * ========================================================================
 * DOS-style pipeline: cmd1 | cmd2 | cmd3
 *
 * Like COMMAND.COM, each stage is spooled through a temporary file on the SD
 * card rather than a live stream — the shell runs one command at a time on a
 * single worker task, so there is no second process to stream into. Each
 * stage writes to its own spool file, and the next stage reads it through the
 * storage input-redirection slot, which is the same slot the `<` operator
 * uses. That makes `type f | find "x" | sort` behave correctly and keeps a
 * single code path in the text-processing commands.
 *
 * Every spool file is removed on every exit path, including a stage failure.
 */

/** Build the spool path for pipeline stage @p stage. */
static void shell_pipe_spool_path(int stage, char *output, size_t output_size)
{
    snprintf(output, output_size, "%s/_pipe%d.tmp", BSP_SD_MOUNT_POINT, stage);
}

void shell_execute_pipe(char *command)
{
    char *stages[SHELL_PIPE_STAGE_MAX];
    char spool[SHELL_PIPE_STAGE_MAX][64];
    int stage_count = 0;
    char *cursor;
    char *segment_start;
    int index;

    if (command == NULL) {
        return;
    }

    /* Split on '|' that is neither quoted nor caret-escaped, so a pipe
     * character inside "..." / '...' or written as ^| is treated as data.
     * The scanner lives in the shell core and is shared with redirection
     * parsing and command chaining. */
    segment_start = command;
    cursor = command;
    while (true) {
        char *pipe_pos = shell_find_unquoted_char(cursor, '|');

        if (pipe_pos == NULL) {
            break;
        }

        if (stage_count >= SHELL_PIPE_STAGE_MAX) {
            shell_transcript_appendf("pipe: at most %d stages are supported\n", SHELL_PIPE_STAGE_MAX);
            return;
        }

        *pipe_pos = '\0';
        stages[stage_count++] = shell_trim(segment_start);
        segment_start = pipe_pos + 1;
        cursor = segment_start;
    }

    if (stage_count == 0) {
        /* No unquoted separator after all: run the line as an ordinary
         * command so a quoted '|' cannot be lost. */
        batch_run_nested(command);
        return;
    }

    if (stage_count >= SHELL_PIPE_STAGE_MAX) {
        shell_transcript_appendf("pipe: at most %d stages are supported\n", SHELL_PIPE_STAGE_MAX);
        return;
    }
    stages[stage_count++] = shell_trim(segment_start);

    for (index = 0; index < stage_count; index++) {
        if (stages[index] == NULL || stages[index][0] == '\0') {
            shell_print_error("pipe: invalid pipe syntax, a stage is empty");
            return;
        }
    }

    /* Every stage but the last spools its output for the next one. */
    for (index = 0; index < stage_count; index++) {
        bool is_last = (index + 1 == stage_count);

        spool[index][0] = '\0';

        if (index > 0) {
            /* Hand the previous stage's spool file to this stage. The
             * text-processing commands pick it up when no filename argument
             * was supplied. */
            storage_set_input_redirect(spool[index - 1]);
        }

        if (is_last) {
            batch_run_nested(stages[index]);
        } else {
            char redirected[SHELL_COMMAND_BYTES * 2];

            shell_pipe_spool_path(index, spool[index], sizeof(spool[index]));
            snprintf(redirected, sizeof(redirected), "%s > %s", stages[index], spool[index]);
            batch_run_nested(redirected);

            /* Give the FATFS write cache a moment to settle before the next
             * stage opens the file for reading. */
            vTaskDelay(pdMS_TO_TICKS(SHELL_PIPE_SETTLE_DELAY_MS));
        }

        /* The redirection slot belongs to one stage only. */
        storage_clear_input_redirect();

        if (s_stop_mode != BATCH_STOP_NONE || s_goto_pending) {
            break;
        }
    }

    /* Remove every spool file that was actually created. */
    for (index = 0; index < stage_count; index++) {
        if (spool[index][0] != '\0') {
            (void)unlink(spool[index]);
        }
    }
}

/* ========================================================================
 * BATCH ENGINE: FILE EXECUTION, LABELS, FOR LOOPS
 * ======================================================================== */

esp_err_t shell_execute_batch_file(const char *path, int argc, char **argv)
{
    /* The frame carries the argument copies and the label table, which
     * together are far larger than the command worker task's stack can hold
     * across nested calls. Allocating it on the heap keeps this recursive
     * function's stack frame small and bounded regardless of nesting depth. */
    shell_batch_frame_t *frame = NULL;
    FILE *file = NULL;
    char *line = NULL;
    shell_sd_session_t session;
    esp_err_t error;
    int setlocal_depth_on_entry;
    int depth;
    int index;

    depth = (s_active_batch_frame != NULL) ? s_active_batch_frame->depth + 1 : 1;
    if (depth > SHELL_BATCH_DEPTH_MAX) {
        shell_transcript_append_text("call: maximum batch nesting depth reached\n");
        return ESP_ERR_INVALID_STATE;
    }

    /* Both the frame and the read buffer are heap-allocated. This function
     * recurses once per nesting level and each level also re-enters the
     * command pipeline, so keeping its stack frame small is what makes
     * SHELL_BATCH_DEPTH_MAX levels fit in the worker task's stack. */
    frame = calloc(1, sizeof(*frame));
    line = malloc(SHELL_BATCH_LINE_BYTES);
    if (frame == NULL || line == NULL) {
        free(frame);
        free(line);
        shell_transcript_append_text("call: out of memory starting the batch frame\n");
        shell_record_errorf("call", ESP_ERR_NO_MEM, "Out of memory allocating a batch frame");
        return ESP_ERR_NO_MEM;
    }

    frame->echo_enabled = true;
    frame->argc = MIN(argc, SHELL_BATCH_ARGS_MAX);
    frame->depth = depth;
    frame->parent = s_active_batch_frame;
    frame->label_count = 0;
    frame->batch_file = NULL;
    frame->setlocal_depth = 0;

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        shell_print_error("call: SD card not present - insert and retry");
        free(frame);
        free(line);
        return error;
    }

    file = fopen(path, "rb");
    if (file == NULL) {
        shell_sd_end(&session, "call");
        free(frame);
        free(line);
        return ESP_ERR_NOT_FOUND;
    }

    frame->batch_file = file;

    for (index = 0; index < frame->argc; index++) {
        snprintf(frame->args[index], sizeof(frame->args[index]), "%s", argv[index]);
    }

    /* Scan for labels first */
    shell_scan_batch_labels(frame, file);

    /* Remember the caller's setlocal depth so any scope this file opens and
     * forgets to close is unwound when the frame returns. */
    setlocal_depth_on_entry = s_setlocal_depth;

    s_active_batch_frame = frame;

    while (fgets(line, SHELL_BATCH_LINE_BYTES, file) != NULL) {
        char *trimmed = shell_trim(line);
        bool suppress_echo = false;
        size_t line_len;
        int continuations = 0;

        line_len = strlen(trimmed);
        while (line_len > 0 && (trimmed[line_len - 1] == '\n' || trimmed[line_len - 1] == '\r')) {
            trimmed[--line_len] = '\0';
        }

        /* Line continuation: a trailing caret joins the next physical line.
         * The caret is consumed and the lines are concatenated, so a long
         * command can be split for readability the way DOS allows.
         *
         * A doubled `^^` at the end is an escaped literal caret, not a
         * continuation, so it is left alone. The chain is bounded by
         * P4_CONFIG_LINE_CONTINUATION_MAX so a malformed file cannot loop. */
        while (line_len > 0 && trimmed[line_len - 1] == P4_CONFIG_ESCAPE_CHAR &&
               continuations < P4_CONFIG_LINE_CONTINUATION_MAX) {
            char *next_start;
            size_t next_len;
            size_t available;

            /* Count the run of trailing carets. An even count means they are
             * escaped pairs and this line does not continue. */
            {
                size_t carets = 0;
                size_t scan = line_len;

                while (scan > 0 && trimmed[scan - 1] == P4_CONFIG_ESCAPE_CHAR) {
                    carets++;
                    scan--;
                }
                if ((carets % 2) == 0) {
                    break;
                }
            }

            /* Drop the continuation caret. */
            trimmed[--line_len] = '\0';

            /* Append the next physical line into the space left in the
             * buffer, which is bounded by SHELL_BATCH_LINE_BYTES. */
            next_start = trimmed + line_len;
            available = SHELL_BATCH_LINE_BYTES - (size_t)(next_start - line) - 1;
            if (available == 0) {
                shell_transcript_append_text("call: continued line is too long\n");
                shell_record_warningf("call", "Continued batch line exceeded %d bytes",
                                      SHELL_BATCH_LINE_BYTES);
                break;
            }

            if (fgets(next_start, (int)available + 1, file) == NULL) {
                /* A trailing caret on the final line has nothing to join. */
                break;
            }

            next_len = strlen(next_start);
            while (next_len > 0 &&
                   (next_start[next_len - 1] == '\n' || next_start[next_len - 1] == '\r')) {
                next_start[--next_len] = '\0';
            }

            line_len += next_len;
            continuations++;
        }

        if (continuations >= P4_CONFIG_LINE_CONTINUATION_MAX) {
            shell_transcript_appendf("call: stopped after %d line continuations\n",
                                     P4_CONFIG_LINE_CONTINUATION_MAX);
            shell_record_warningf("call", "Line continuation limit reached");
        }

        if (trimmed[0] == '@') {
            suppress_echo = true;
            trimmed = shell_trim(trimmed + 1);
        }

        if (trimmed[0] == '\0') {
            continue;
        }

        /* Check for label - skip execution */
        if (shell_is_label(trimmed)) {
            continue;
        }

        /* Check for for loop */
        if (strncasecmp(trimmed, "for ", 4) == 0) {
            shell_execute_for_loop(frame, trimmed);
            if (s_stop_mode != BATCH_STOP_NONE || s_goto_pending) break;
            continue;
        }

        if (!suppress_echo && frame->echo_enabled) {
            shell_transcript_appendf_ansi(SH_PROMPT "%s" SH_RST SH_DESC "%s" SH_RST "\n", SHELL_PROMPT, trimmed);
        }

        batch_run_nested(trimmed);

        /* `exit` or `exit /b` asked this file to stop. */
        if (s_stop_mode != BATCH_STOP_NONE) {
            break;
        }

        /* Check for goto or call :label that may have changed execution flow */
        if (s_goto_pending) {
            long label_pos = shell_find_label_pos(frame, s_goto_label + 1);
            if (label_pos >= 0) {
                fseek(file, label_pos, SEEK_SET);
                s_goto_pending = false;
                s_goto_label[0] = '\0';
                /* Continue from the label position - the label line will be skipped */
                continue;
            } else {
                shell_transcript_appendf("goto: label not found: %s\n", s_goto_label + 1);
                s_goto_pending = false;
                s_goto_label[0] = '\0';
            }
        }
    }

    s_active_batch_frame = frame->parent;

    /* Discard any setlocal scope this file left open, restoring the caller's
     * environment and releasing the snapshots. */
    shell_setlocal_unwind_to(setlocal_depth_on_entry);

    /* `exit /b` stops only this file; the caller keeps running. A bare
     * `exit` keeps unwinding until no batch frame is left. */
    if (s_stop_mode == BATCH_STOP_FRAME) {
        s_stop_mode = BATCH_STOP_NONE;
    } else if (s_stop_mode == BATCH_STOP_ALL && frame->parent == NULL) {
        s_stop_mode = BATCH_STOP_NONE;
    }

    fclose(file);
    shell_sd_end(&session, "call");
    free(frame);
    free(line);
    return ESP_OK;
}

/* Helper: check if a line is a label (starts with :) */
static bool shell_is_label(const char *line)
{
    if (line == NULL) return false;
    while (*line && isspace((unsigned char)*line)) line++;
    return *line == ':';
}

/* Helper: extract label name from a label line */
static void shell_extract_label_name(const char *line, char *name, size_t name_size)
{
    if (line == NULL || name == NULL || name_size == 0) return;
    while (*line && isspace((unsigned char)*line)) line++;
    if (*line == ':') line++;
    size_t i = 0;
    while (*line && !isspace((unsigned char)*line) && i + 1 < name_size) {
        name[i++] = *line++;
    }
    name[i] = '\0';
}

/* Helper: find a label in the current batch frame */
static long shell_find_label_pos(shell_batch_frame_t *frame, const char *label_name)
{
    if (frame == NULL || label_name == NULL) return -1;
    for (int i = 0; i < frame->label_count; i++) {
        if (strcasecmp(frame->labels[i].name, label_name) == 0) {
            return frame->labels[i].file_pos;
        }
    }
    return -1;
}

/* Helper: scan batch file for labels and build label table */
static void shell_scan_batch_labels(shell_batch_frame_t *frame, FILE *file)
{
    if (frame == NULL || file == NULL) return;

    long current_pos = ftell(file);
    char line[SHELL_BATCH_LINE_BYTES];
    long line_pos = 0;

    rewind(file);
    frame->label_count = 0;

    /* Tracks whether the previous physical line ended with an unescaped
     * continuation caret. A line that is the tail of a continuation is part
     * of the command above it, so a ':' at its start is data, not a label. */
    bool previous_continues = false;

    while (fgets(line, sizeof(line), file) != NULL) {
        line_pos = ftell(file);
        char *trimmed = shell_trim(line);
        bool this_continues;
        size_t length;

        /* Determine whether THIS line continues, using the same odd-caret
         * rule the executor applies, so both agree on line boundaries. */
        length = strlen(trimmed);
        while (length > 0 && (trimmed[length - 1] == '\n' || trimmed[length - 1] == '\r')) {
            trimmed[--length] = '\0';
        }

        {
            size_t carets = 0;
            size_t scan = length;

            while (scan > 0 && trimmed[scan - 1] == P4_CONFIG_ESCAPE_CHAR) {
                carets++;
                scan--;
            }
            this_continues = ((carets % 2) == 1);
        }

        if (!previous_continues && shell_is_label(trimmed)) {
            if (frame->label_count < SHELL_BATCH_LABEL_MAX) {
                shell_extract_label_name(trimmed, frame->labels[frame->label_count].name,
                                         sizeof(frame->labels[frame->label_count].name));
                frame->labels[frame->label_count].file_pos = line_pos - strlen(line);
                frame->label_count++;
            }
        }

        previous_continues = this_continues;
    }

    fseek(file, current_pos, SEEK_SET);
}

/* Helper: execute a for loop command */
static void shell_execute_for_loop(shell_batch_frame_t *frame, char *command_line)
{
    (void)frame;

    /* Parse: for %%var in (set) do command */
    char *for_ptr = strstr(command_line, "for ");
    if (for_ptr == NULL) return;

    /* Skip "for " */
    for_ptr += 4;
    while (*for_ptr && isspace((unsigned char)*for_ptr)) for_ptr++;

    /* Check for %%var */
    if (*for_ptr != '%' || *(for_ptr + 1) != '%') return;
    for_ptr += 2;

    char var_name = *for_ptr;
    if (!isalpha((unsigned char)var_name)) return;
    for_ptr++;
    while (*for_ptr && isspace((unsigned char)*for_ptr)) for_ptr++;

    /* Expect "in" */
    if (strncasecmp(for_ptr, "in", 2) != 0) return;
    for_ptr += 2;
    while (*for_ptr && isspace((unsigned char)*for_ptr)) for_ptr++;

    /* Expect "(" */
    if (*for_ptr != '(') return;
    for_ptr++;

    /* Parse the set - everything until ")" */
    char set_str[SHELL_BATCH_LINE_BYTES];
    char *set_ptr = set_str;
    int paren_depth = 1;
    while (*for_ptr && paren_depth > 0 && set_ptr - set_str < (int)sizeof(set_str) - 1) {
        if (*for_ptr == '(') paren_depth++;
        else if (*for_ptr == ')') paren_depth--;
        if (paren_depth > 0) {
            *set_ptr++ = *for_ptr;
        }
        for_ptr++;
    }
    *set_ptr = '\0';

    while (*for_ptr && isspace((unsigned char)*for_ptr)) for_ptr++;

    /* Expect "do" */
    if (strncasecmp(for_ptr, "do", 2) != 0) return;
    for_ptr += 2;
    while (*for_ptr && isspace((unsigned char)*for_ptr)) for_ptr++;

    if (*for_ptr == '\0') return;

    char *do_command = shell_trim(for_ptr);
    if (*do_command == '\0') return;

    /* Now iterate over the set */
    char *save_ptr = NULL;
    char *token = strtok_r(set_str, " \t", &save_ptr);
    while (token != NULL) {
        /* Substitute every %<var> occurrence in the body with this token */
        char expanded_cmd[SHELL_BATCH_LINE_BYTES * 2];
        char *src = do_command;
        char *dst = expanded_cmd;
        size_t remaining = sizeof(expanded_cmd) - 1;

        while (*src && remaining > 0) {
            if (*src == '%' && *(src + 1) == var_name) {
                const char *replacement = token;
                while (*replacement && remaining > 0) {
                    *dst++ = *replacement++;
                    remaining--;
                }
                src += 2;
            } else {
                *dst++ = *src++;
                remaining--;
            }
        }
        *dst = '\0';

        /* Execute the expanded command */
        batch_run_nested(expanded_cmd);

        /* Check for goto/exit conditions that end the loop early */
        if (s_goto_pending || s_stop_mode != BATCH_STOP_NONE) {
            return;
        }

        token = strtok_r(NULL, " \t", &save_ptr);
    }
}

/* ========================================================================
 * LIFECYCLE
 * ======================================================================== */

void batch_init(void)
{
    if (s_initialized) {
        return;
    }

    memset(s_shell_env_vars, 0, sizeof(s_shell_env_vars));
    (void)shell_env_set("PATH", "sd:/");

    s_active_batch_frame = NULL;
    s_errorlevel = 0;
    s_goto_label[0] = '\0';
    s_goto_pending = false;
    s_stop_mode = BATCH_STOP_NONE;

    /* Release any setlocal snapshot left over from a previous init. */
    shell_setlocal_unwind_to(0);
    memset(s_setlocal_stack, 0, sizeof(s_setlocal_stack));
    s_setlocal_depth = 0;

    s_initialized = true;
    ESP_LOGI(BATCH_TAG, "Batch module initialized");
}

bool batch_is_initialized(void)
{
    return s_initialized;
}
