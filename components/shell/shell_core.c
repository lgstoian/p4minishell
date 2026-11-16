/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file shell_core.c
 * @brief Hardware-free shell core: text helpers, statement scanner and state.
 *
 * Everything here is pure C. It is the half of the shell that can run without
 * a display, a panel, USB-Serial-JTAG, the SD card or the board BSP, which is
 * what lets the host unit-test lane (test/host) exercise it natively. The
 * transcript's LVGL label, the UART mirror, the input line and history recall
 * stay in shell.c.
 *
 * One quote/escape scanner backs every surface that must distinguish syntax
 * from data: the argument tokenizer, redirection parsing, pipe splitting and
 * command chaining. Keeping them together means those places can never
 * disagree about what a quote means.
 *
 * The state that lives here is the state that needs no hardware: the recall
 * history store (with its password masking), the abort/busy flags the command
 * worker polls, the search matcher and the help registry. shell.c reads the
 * objects it shares through shell_core_internal.h, so each is defined once.
 *
 * @module components/shell
 * @layer shell
 * @domain shell
 * @provides shell_trim, shell_join_args, shell_split_args, shell_split_chain, quote/escape scanner, group depth, history store, abort/busy/foreground-break flags, header-notification forwarder, help registry
 * @status stable
 * @test test/main/test_shell_quoting.c, test/main/test_shell_abort.c,
 *       test/main/test_history_file.c, test/main/test_history_search.c,
 *       test/main/test_batch_expr.c, test/main/test_svc.c
 * @hard-rule pure C only - no LVGL, display, USB, storage or board BSP in this file
 */

#include "shell.h"
#include "shell_core_internal.h"
#include "p4minishell_config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ========================================================================
 * SHARED STATE
 * ========================================================================
 * Definitions for the objects declared in shell_core_internal.h: the store
 * and the flags are hardware-free data, so they live with the pure code and
 * shell.c reads the same objects by name.
 */

/* Command module operations table, filled in by shell_register_command_ops(). */
shell_command_ops_t shell_core_command_ops;

/* Command history, heap-backed so very long (up to P4_CONFIG_COMMAND_BYTES)
 * commands do not reserve a fixed grid of RAM. Each entry is a strdup'd line;
 * the pointer table and total byte usage are bounded by
 * P4_CONFIG_COMMAND_HISTORY_DEPTH and P4_CONFIG_HISTORY_TOTAL_BYTES. */
char **shell_core_command_history;
size_t shell_core_command_history_count;
static size_t shell_core_command_history_bytes;
static size_t shell_core_command_history_generation;
int shell_core_command_history_cursor = -1;
char shell_core_history_draft[P4_CONFIG_COMMAND_BYTES];

/* Foreground break + worker-busy flags (see shell.h). Written from the LVGL
 * task (USB Ctrl+C, Stop button), read on the command worker; plain bools
 * like the background kill flags. */
static volatile bool s_foreground_abort;
static volatile bool s_command_busy;

bool shell_text_equals_ignore_case(const char *left, const char *right)
{
    if (left == NULL || right == NULL) {
        return false;
    }

    while (*left != '\0' && *right != '\0') {
        if (tolower((unsigned char)*left) != tolower((unsigned char)*right)) {
            return false;
        }
        left++;
        right++;
    }

    return *left == '\0' && *right == '\0';
}

char *shell_trim(char *text)
{
    char *end;

    if (text == NULL) {
        return NULL;
    }

    while (isspace((unsigned char)*text)) {
        text++;
    }

    if (*text == '\0') {
        return text;
    }

    end = text + strlen(text) - 1;
    while (end > text && isspace((unsigned char)*end)) {
        end--;
    }

    *(end + 1) = '\0';
    return text;
}

/* ========================================================================
 * QUOTING AND ESCAPING
 * ========================================================================
 * One scanner backs every surface that must distinguish syntax from data:
 * the argument tokenizer, redirection parsing, pipe splitting, and command
 * chaining. Keeping it here means those five places can never disagree.
 */

/**
 * Advance one position through @p text, maintaining quote state.
 *
 * @param cursor      Current position.
 * @param state       Quote state, updated in place.
 * @param escaped_out Set to true when this position is an escaped literal
 *                    (the caret was consumed and this is the payload). May be
 *                    NULL. Escapes are inert inside single quotes, matching
 *                    the "fully literal" contract.
 * @return Pointer to the next position to examine.
 */
static const char *shell_quote_advance(const char *cursor,
                                       shell_quote_state_t *state,
                                       bool *escaped_out)
{
    if (escaped_out != NULL) {
        *escaped_out = false;
    }

    if (cursor == NULL || *cursor == '\0') {
        return cursor;
    }

    /* A caret escapes the following character everywhere except inside
     * single quotes, where the whole run is taken literally. */
    if (*cursor == P4_CONFIG_ESCAPE_CHAR && *state != SHELL_QUOTE_SINGLE && cursor[1] != '\0') {
        if (escaped_out != NULL) {
            *escaped_out = true;
        }
        return cursor + 2;
    }

    if (*cursor == '"' && *state != SHELL_QUOTE_SINGLE) {
        *state = (*state == SHELL_QUOTE_DOUBLE) ? SHELL_QUOTE_NONE : SHELL_QUOTE_DOUBLE;
        return cursor + 1;
    }

    if (*cursor == '\'' && *state != SHELL_QUOTE_DOUBLE) {
        *state = (*state == SHELL_QUOTE_SINGLE) ? SHELL_QUOTE_NONE : SHELL_QUOTE_SINGLE;
        return cursor + 1;
    }

    return cursor + 1;
}

char *shell_find_unquoted_any(const char *text, const char *targets)
{
    shell_quote_state_t state = SHELL_QUOTE_NONE;
    const char *cursor = text;
    int paren_depth = 0;

    if (text == NULL || targets == NULL) {
        return NULL;
    }

    while (*cursor != '\0') {
        const char *candidate = cursor;
        bool escaped = false;
        const char *next = shell_quote_advance(cursor, &state, &escaped);

        /* A candidate only counts when it is the character itself, outside
         * quotes, and not the payload of a caret escape. */
        if (!escaped && state == SHELL_QUOTE_NONE && next == candidate + 1) {
            /* Balanced parens group operators: separators inside `(...)`
             * belong to the group (chain, pipe, and redirect scanners all
             * share this rule). Unbalanced `(` runs literal to end of
             * line; a stray `)` never drives the depth negative. */
            if (*candidate == '(') {
                paren_depth++;
            } else if (*candidate == ')') {
                if (paren_depth > 0) {
                    paren_depth--;
                }
            } else if (paren_depth == 0 && strchr(targets, *candidate) != NULL &&
                       *candidate != '\0') {
                /* Quote delimiters advance by one too, but they were consumed as
                 * state changes above and are never in the target set in
                 * practice. Guard anyway so a caller cannot search for a quote. */
                if (*candidate != '"' && *candidate != '\'') {
                    return (char *)candidate;
                }
            }
        }

        cursor = next;
    }

    return NULL;
}

char *shell_find_unquoted_char(const char *text, char target)
{
    char targets[2];

    targets[0] = target;
    targets[1] = '\0';

    return shell_find_unquoted_any(text, targets);
}

bool shell_has_unquoted_char(const char *text, char target)
{
    return shell_find_unquoted_char(text, target) != NULL;
}

char *shell_unescape_in_place(char *text)
{
    shell_quote_state_t state = SHELL_QUOTE_NONE;
    char *read;
    char *write;

    if (text == NULL) {
        return NULL;
    }

    read = text;
    write = text;

    while (*read != '\0') {
        /* Caret escape: drop the caret, keep the payload verbatim. */
        if (*read == P4_CONFIG_ESCAPE_CHAR && state != SHELL_QUOTE_SINGLE && read[1] != '\0') {
            *write++ = read[1];
            read += 2;
            continue;
        }

        /* Quote delimiters group text but are not part of the value. */
        if (*read == '"' && state != SHELL_QUOTE_SINGLE) {
            state = (state == SHELL_QUOTE_DOUBLE) ? SHELL_QUOTE_NONE : SHELL_QUOTE_DOUBLE;
            read++;
            continue;
        }

        if (*read == '\'' && state != SHELL_QUOTE_DOUBLE) {
            state = (state == SHELL_QUOTE_SINGLE) ? SHELL_QUOTE_NONE : SHELL_QUOTE_SINGLE;
            read++;
            continue;
        }

        *write++ = *read++;
    }

    *write = '\0';
    return text;
}

/**
 * Strip caret escapes (`^c` -> `c`) from a string in place WITHOUT removing
 * quote delimiters. Used by `echo`, which prints the raw remainder of the
 * command line and must keep `"..."` visible (matching DOS) while still
 * consuming a caret that escapes the following character (`echo a^&b` -> `a&b`).
 *
 * A caret inside single quotes is literal (the same rule the operator scanner
 * uses); a trailing caret with nothing after it is kept.
 */
char *shell_unescape_carets_in_place(char *text)
{
    shell_quote_state_t state = SHELL_QUOTE_NONE;
    char *read;
    char *write;

    if (text == NULL) {
        return NULL;
    }

    read = text;
    write = text;

    while (*read != '\0') {
        if (*read == P4_CONFIG_ESCAPE_CHAR && state != SHELL_QUOTE_SINGLE && read[1] != '\0') {
            *write++ = read[1];
            read += 2;
            continue;
        }
        if (*read == '"' && state != SHELL_QUOTE_SINGLE) {
            state = (state == SHELL_QUOTE_DOUBLE) ? SHELL_QUOTE_NONE : SHELL_QUOTE_DOUBLE;
        } else if (*read == '\'' && state != SHELL_QUOTE_DOUBLE) {
            state = (state == SHELL_QUOTE_SINGLE) ? SHELL_QUOTE_NONE : SHELL_QUOTE_SINGLE;
        }
        *write++ = *read++;
    }

    *write = '\0';
    return text;
}

int shell_split_args(char *text, char **argv, int max_args)
{
    int argc = 0;
    char *p = text;

    if (text == NULL || argv == NULL || max_args <= 0) {
        return 0;
    }

    while (*p != '\0' && argc < max_args) {
        shell_quote_state_t state = SHELL_QUOTE_NONE;
        char *token_start;

        /* Skip leading whitespace between arguments. */
        while (*p != '\0' && isspace((unsigned char)*p)) {
            p++;
        }

        if (*p == '\0') {
            break;
        }

        /* An argument runs until unquoted whitespace. Quote runs and caret
         * escapes are carried along and stripped once the extent is known,
         * so `"a b"`, `'a b'`, and `a^ b` all yield one argument. */
        token_start = p;
        while (*p != '\0') {
            bool escaped = false;
            const char *next = shell_quote_advance(p, &state, &escaped);

            if (!escaped && state == SHELL_QUOTE_NONE && next == p + 1 &&
                isspace((unsigned char)*p)) {
                break;
            }

            p = (char *)next;
        }

        if (*p != '\0') {
            *p = '\0';
            p++;
        }

        /* Remove the quoting and escape markup now that the token is
         * delimited, so handlers receive the literal value. */
        argv[argc] = shell_unescape_in_place(token_start);
        argc++;
    }

    /* NULL-terminate the vector when there is room, matching the standard C
     * argv contract (native apps registered through the applib ABI rely on
     * it; built-ins only ever use argc). */
    if (argc < max_args) {
        argv[argc] = NULL;
    }

    return argc;
}

/**
 * Count the arguments in a command line without mutating it, using the same
 * quote/escape rules as shell_split_args(). Used to detect when a command has
 * more arguments than the dispatcher's argv capacity, so truncation is never
 * silent.
 */
int shell_count_args(const char *text)
{
    int count = 0;
    const char *p = text;

    if (text == NULL) {
        return 0;
    }

    while (*p != '\0') {
        shell_quote_state_t state = SHELL_QUOTE_NONE;

        /* Skip leading whitespace between arguments. */
        while (*p != '\0' && isspace((unsigned char)*p)) {
            p++;
        }
        if (*p == '\0') {
            break;
        }

        count++;

        /* Advance over the token with the same quote/escape rules. */
        while (*p != '\0') {
            bool escaped = false;
            const char *next = shell_quote_advance(p, &state, &escaped);

            if (!escaped && state == SHELL_QUOTE_NONE && next == p + 1 &&
                isspace((unsigned char)*p)) {
                break;
            }
            p = next;
        }
        if (*p != '\0') {
            p++;
        }
    }

    return count;
}

/**
 * Join argv[@p start_index..@p argc) with single spaces into @p output.
 *
 * Moved here from shell.c: it is string surgery, and the batch verbs (set /a,
 * set /p) call it from the host-callable half of the engine. Behaviour is
 * unchanged: a NULL argv/output or a zero size leaves @p output untouched
 * except for its terminator, and truncation stops at the last whole copy.
 */
void shell_join_args(char **argv, int start_index, int argc, char *output, size_t output_size)
{
    int i;
    size_t pos = 0;

    if (argv == NULL || output == NULL || output_size == 0) {
        return;
    }

    output[0] = '\0';

    for (i = start_index; i < argc && pos < output_size - 1; i++) {
        if (i > start_index) {
            output[pos++] = ' ';
            output[pos] = '\0';
        }
        pos += snprintf(output + pos, output_size - pos, "%s", argv[i]);
    }
}

/* ========================================================================
 * COMMAND CHAINING
 * ======================================================================== */

int shell_split_chain(char *text,
                      shell_chain_segment_t *segments,
                      int max_segments,
                      bool *truncated_out)
{
    int count = 0;
    char *segment_start;
    char *cursor;
    shell_chain_op_t pending_op = SHELL_CHAIN_FIRST;

    if (truncated_out != NULL) {
        *truncated_out = false;
    }

    if (text == NULL || segments == NULL || max_segments <= 0) {
        return 0;
    }

    /* `if`, `for`, and `while` consume the rest of the line as their
     * command body (cmd.exe semantics: `if cond a & b` runs a and b only
     * when cond is true). Splitting here would turn a `&` inside a guarded
     * body into an unconditional chain link that runs even when the
     * condition is false, so a guarded `... & goto label` would jump
     * regardless of the test. Each body re-enters this splitter on its own
     * (no longer starting with the guard word), so nested separators still
     * chain inside the body. */
    {
        char first_word[8];
        size_t word_len = 0;
        const char *word = text;

        while (*word == ' ' || *word == '\t') {
            word++;
        }
        while (word_len + 1 < sizeof(first_word) && *word != '\0' &&
               *word != ' ' && *word != '\t' && *word != '(') {
            first_word[word_len++] = (char)tolower((unsigned char)*word);
            word++;
        }
        first_word[word_len] = '\0';
        if (strcmp(first_word, "if") == 0 || strcmp(first_word, "for") == 0 ||
            strcmp(first_word, "while") == 0) {
            segments[0].command = shell_trim(text);
            segments[0].op = SHELL_CHAIN_FIRST;
            return 1;
        }
    }

    segment_start = text;
    cursor = text;

    while (true) {
        char *op_pos = shell_find_unquoted_any(cursor, "&|");
        shell_chain_op_t next_op;
        size_t op_len;

        if (op_pos == NULL) {
            break;
        }

        /* A single '|' is the pipe operator, not a chain separator; leave it
         * for the pipeline splitter. '||' is chaining. */
        if (op_pos[0] == '|') {
            if (op_pos[1] != '|') {
                cursor = op_pos + 1;
                continue;
            }
            next_op = SHELL_CHAIN_ON_FAILURE;
            op_len = 2;
        } else {
            if (op_pos[1] == '&') {
                next_op = SHELL_CHAIN_ON_SUCCESS;
                op_len = 2;
            } else {
                next_op = SHELL_CHAIN_ALWAYS;
                op_len = 1;
            }
        }

        if (count >= max_segments) {
            if (truncated_out != NULL) {
                *truncated_out = true;
            }
            return count;
        }

        /* Close the current segment at the operator and record it. */
        {
            char *tail = op_pos + op_len;

            *op_pos = '\0';
            segments[count].command = shell_trim(segment_start);
            segments[count].op = pending_op;
            count++;

            pending_op = next_op;
            segment_start = tail;
            cursor = tail;
        }
    }

    if (count >= max_segments) {
        if (truncated_out != NULL) {
            *truncated_out = true;
        }
        return count;
    }

    {
        char *final_command = shell_trim(segment_start);
        /* Do not emit a trailing empty segment (e.g. after a dangling
         * separator, or for an all-whitespace line). */
        if (final_command[0] != '\0') {
            segments[count].command = final_command;
            segments[count].op = pending_op;
            count++;
        }
    }

    return count;
}

const char *shell_group_match(const char *text)
{
    shell_quote_state_t state = SHELL_QUOTE_NONE;
    const char *cursor;
    int depth = 0;

    if (text == NULL || text[0] != '(') {
        return NULL;
    }
    cursor = text;
    while (*cursor != '\0') {
        const char *candidate = cursor;
        bool escaped = false;
        const char *next = shell_quote_advance(cursor, &state, &escaped);

        if (!escaped && state == SHELL_QUOTE_NONE && next == candidate + 1) {
            if (*candidate == '(') {
                depth++;
            } else if (*candidate == ')') {
                depth--;
                if (depth == 0) {
                    return next;
                }
            }
        }
        cursor = next;
    }
    return NULL;
}

int shell_group_depth(const char *text)
{
    shell_quote_state_t state = SHELL_QUOTE_NONE;
    const char *cursor;
    int depth = 0;
    int peak = 0;

    if (text == NULL) {
        return 0;
    }
    cursor = text;
    while (*cursor != '\0') {
        const char *candidate = cursor;
        bool escaped = false;
        const char *next = shell_quote_advance(cursor, &state, &escaped);

        if (!escaped && state == SHELL_QUOTE_NONE && next == candidate + 1) {
            if (*candidate == '(') {
                depth++;
                if (depth > peak) {
                    peak = depth;
                }
            } else if (*candidate == ')') {
                if (depth > 0) {
                    depth--;
                }
            }
        }
        cursor = next;
    }
    return peak;
}

bool shell_parse_percentage_arg(const char *text, int *percentage_out)
{
    char *endptr;
    long value;

    if (text == NULL || percentage_out == NULL) {
        return false;
    }

    value = strtol(text, &endptr, 10);
    if (endptr == text || *endptr != '\0' || value < 0 || value > 100) {
        return false;
    }

    *percentage_out = (int)value;
    return true;
}

/* ========================================================================
 * COMMAND HISTORY
 * ========================================================================
 * The recall buffer is state, not UI: storing, masking and persisting a
 * command needs no display. Only recall itself (painting the input line)
 * stays in shell.c.
 */

/**
 * Detect `wifi connect <ssid> <password>` and `crypt ... /p:<password>`,
 * which must never reach the transcript or the recall buffer in clear text.
 */
static bool shell_command_is_sensitive(const char *command)
{
    char *buffer = malloc(P4_CONFIG_COMMAND_BYTES);
    char *argv[8];
    int argc;
    int i;
    bool sensitive = false;

    if (buffer == NULL) {
        return false;
    }

    snprintf(buffer, P4_CONFIG_COMMAND_BYTES, "%s", command != NULL ? command : "");
    argc = shell_split_args(buffer, argv, 8);
    if (argc >= 4 && strcmp(argv[0], "wifi") == 0 && strcmp(argv[1], "connect") == 0) {
        sensitive = true;
    }
    /* crypt and pim both take inline /p:<password> tokens. */
    if (argc >= 2 && (strcmp(argv[0], "crypt") == 0 || strcmp(argv[0], "pim") == 0)) {
        for (i = 1; i < argc; i++) {
            if (strncasecmp(argv[i], "/p:", 3) == 0) {
                sensitive = true;
                break;
            }
        }
    }
    free(buffer);
    return sensitive;
}

/** Overwrite the value of a `/p:<password>` token with asterisks (in place,
 *  length-preserving so history byte accounting is unaffected). */
static void shell_mask_inline_password(char *command)
{
    char *at = command;

    while (*at != '\0') {
        if ((at[0] == '/' || at[0] == '-') &&
            (at[1] == 'p' || at[1] == 'P') && at[2] == ':') {
            char *value = at + 3;
            while (*value != '\0' && *value != ' ' && *value != '\t') {
                *value++ = '*';
            }
            at = value;
        } else {
            at++;
        }
    }
}

bool shell_command_should_store_history(const char *command)
{
    /* OTA confirmation replies (YES/NO) are not shell commands. */
    if (shell_core_command_ops.c6ota_is_pending != NULL && shell_core_command_ops.c6ota_is_pending()) {
        return false;
    }

    return !shell_command_is_sensitive(command);
}

void shell_format_command_for_transcript(const char *command, char *output, size_t output_size)
{
    char *buffer = malloc(P4_CONFIG_COMMAND_BYTES);
    char *argv[4];
    int argc;

    if (output == NULL || output_size == 0) {
        free(buffer);
        return;
    }
    if (buffer == NULL) {
        snprintf(output, output_size, "%s", command != NULL ? command : "");
        return;
    }

    snprintf(buffer, P4_CONFIG_COMMAND_BYTES, "%s", command != NULL ? command : "");
    argc = shell_split_args(buffer, argv, 4);

    if (argc >= 4 && strcmp(argv[0], "wifi") == 0 && strcmp(argv[1], "connect") == 0) {
        snprintf(output, output_size, "wifi connect %s ********", argv[2]);
        free(buffer);
        return;
    }

    /* crypt and pim both take inline /p:<password> tokens: mask the value so
     * it never reaches the transcript or history in the clear. */
    if (argc >= 2 && (strcmp(argv[0], "crypt") == 0 || strcmp(argv[0], "pim") == 0)) {
        snprintf(output, output_size, "%s", command != NULL ? command : "");
        shell_mask_inline_password(output);
        free(buffer);
        return;
    }

    snprintf(output, output_size, "%s", command != NULL ? command : "");
    free(buffer);
}

/** Drop the oldest history entry and slide the rest down (pointer array). */
static void shell_history_drop_oldest(void)
{
    size_t index;

    if (shell_core_command_history_count == 0) {
        return;
    }
    shell_core_command_history_bytes -= strlen(shell_core_command_history[0]) + 1;
    free(shell_core_command_history[0]);
    for (index = 1; index < shell_core_command_history_count; index++) {
        shell_core_command_history[index - 1] = shell_core_command_history[index];
    }
    shell_core_command_history_count--;
}

void shell_store_command_history(const char *command)
{
    char *masked = NULL;
    const char *entry;
    size_t entry_bytes;

    if (command == NULL || command[0] == '\0') {
        return;
    }

    /* Defence in depth: callers are expected to filter sensitive commands
     * via shell_command_should_store_history(), but mask here as well so no
     * code path can ever persist a Wi-Fi password in the recall buffer. */
    if (shell_command_is_sensitive(command)) {
        masked = malloc(P4_CONFIG_COMMAND_BYTES);
        if (masked == NULL) {
            return;
        }
        shell_format_command_for_transcript(command, masked, P4_CONFIG_COMMAND_BYTES);
        entry = masked;
    } else {
        entry = command;
    }

    /* Skip consecutive duplicates so repeated Enter presses do not flood the
     * recall buffer. */
    if (shell_core_command_history_count > 0 &&
        strcmp(shell_core_command_history[shell_core_command_history_count - 1], entry) == 0) {
        free(masked);
        return;
    }

    /* Make sure the pointer table exists. */
    if (shell_core_command_history == NULL) {
        shell_core_command_history = calloc(P4_CONFIG_COMMAND_HISTORY_DEPTH, sizeof(char *));
        if (shell_core_command_history == NULL) {
            free(masked);
            return;
        }
    }

    /* Make room for the new entry: depth cap first, then the byte cap. */
    while (shell_core_command_history_count >= P4_CONFIG_COMMAND_HISTORY_DEPTH) {
        shell_history_drop_oldest();
    }
    entry_bytes = strlen(entry) + 1;
    while (shell_core_command_history_count > 0 &&
           shell_core_command_history_bytes + entry_bytes > P4_CONFIG_HISTORY_TOTAL_BYTES) {
        shell_history_drop_oldest();
    }

    shell_core_command_history[shell_core_command_history_count] = strdup(entry);
    if (shell_core_command_history[shell_core_command_history_count] != NULL) {
        shell_core_command_history_bytes += entry_bytes;
        shell_core_command_history_count++;
        shell_core_command_history_generation++;
    }

    free(masked);
}

size_t shell_history_get_count(void)
{
    return shell_core_command_history_count;
}

const char *shell_history_get(size_t index)
{
    if (index >= shell_core_command_history_count || shell_core_command_history == NULL) {
        return NULL;
    }
    return shell_core_command_history[index];
}

size_t shell_history_generation(void)
{
    return shell_core_command_history_generation;
}

void shell_history_clear(void)
{
    size_t index;

    for (index = 0; index < shell_core_command_history_count; index++) {
        free(shell_core_command_history[index]);
    }
    free(shell_core_command_history);
    shell_core_command_history = NULL;
    shell_core_command_history_count = 0;
    shell_core_command_history_bytes = 0;
    shell_core_command_history_cursor = -1;
    shell_core_history_draft[0] = '\0';
}

bool shell_history_save_lines(FILE *fp)
{
    size_t index;

    if (fp == NULL) {
        return false;
    }
    for (index = 0; index < shell_history_get_count(); index++) {
        const char *line = shell_history_get(index);

        if (line != NULL && fprintf(fp, "%s\n", line) < 0) {
            return false;
        }
    }
    return true;
}

size_t shell_history_load_lines(FILE *fp)
{
    // Command-sized and heap-allocated: `history /load` can run from a
    // nested batch line, so a stack buffer would eat into the worker stack.
    char *line = malloc(P4_CONFIG_COMMAND_BYTES);
    size_t loaded = 0;

    if (fp == NULL || line == NULL) {
        free(line);
        return 0;
    }
    while (fgets(line, P4_CONFIG_COMMAND_BYTES, fp) != NULL) {
        /* Keep the allocation pointer for free(); trim only the view of it. */
        const char *trimmed = shell_trim(line);
        if (trimmed[0] == '\0') {
            continue;
        }
        shell_store_command_history(trimmed);
        loaded++;
    }
    free(line);
    return loaded;
}

/* ========================================================================
 * ABORT AND BUSY FLAGS
 * ========================================================================
 * Written from the LVGL task, polled by the command worker and the batch
 * engine. No lock: a bool store is atomic on both targets and a lost
 * request is re-observed on the next poll.
 */

void shell_request_abort(void)
{
    s_foreground_abort = true;
}

bool shell_abort_requested(void)
{
    return s_foreground_abort;
}

void shell_clear_abort(void)
{
    s_foreground_abort = false;
}

void shell_set_command_busy(bool busy)
{
    s_command_busy = busy;
}

bool shell_is_command_busy(void)
{
    return s_command_busy;
}

/* Foreground break flag (see shell.h). Written from the LVGL task (USB
 * Ctrl+C, Stop button), read on the command worker; a plain bool like the
 * abort/busy flags above, and hardware-free by the same reasoning. The batch
 * sync tables poll it, which is why it lives here rather than in shell.c. */
static volatile bool s_foreground_break_seen;

void shell_mark_foreground_break(void)
{
    s_foreground_break_seen = true;
}

bool shell_foreground_break_pending(void)
{
    return s_foreground_break_seen;
}

void shell_clear_foreground_break(void)
{
    s_foreground_break_seen = false;
}

/* ========================================================================
 * HEADER NOTIFICATIONS (forwarder + optional sink)
 * ========================================================================
 * shell.h declares shell_header_notify_level()/shell_header_notify() for every
 * notifier in the firmware (alarm, boot, storage, the batch service
 * supervisor). The header widget they feed is LVGL-side, so it stays in
 * header.c and shell.c installs it here from shell_init(). With no sink
 * installed (the host unit-test lane, or before shell_init()) a notification
 * is simply not displayed - there is no fake widget and nothing claims it
 * was shown.
 */

static void (*s_header_notify)(header_notify_level_t level, const char *text,
                              uint32_t timeout_ms) = NULL;

void shell_core_set_header_notify(void (*notify)(header_notify_level_t level,
                                                 const char *text,
                                                 uint32_t timeout_ms))
{
    s_header_notify = notify;
}

void shell_header_notify_level(const char *text, uint32_t timeout_ms,
                               header_notify_level_t level)
{
    if (s_header_notify != NULL) {
        s_header_notify(level, text, timeout_ms);
    }
}

void shell_header_notify(const char *text, uint32_t timeout_ms)
{
    shell_header_notify_level(text, timeout_ms, HEADER_NOTIFY_INFO);
}

/* ========================================================================
 * HISTORY SEARCH MATCHER
 * ========================================================================
 * Shared by the Ctrl+R reverse search (shell.c) and the batch-testable
 * `history /search` verb, so both agree on what "matches".
 */

/** Case-insensitive substring test (newlib lacks a portable strcasestr). */
bool shell_history_search_matches(const char *entry, const char *query)
{
    size_t nl;

    if (entry == NULL || query == NULL) {
        return false;
    }
    nl = strlen(query);
    if (nl == 0) {
        return true;
    }
    for (; *entry != '\0'; entry++) {
        if (strncasecmp(entry, query, nl) == 0) {
            return true;
        }
    }
    return false;
}


/* ========================================================================
 * HELP REGISTRY
 * ========================================================================
 * The offline command reference is data, not UI: `help`, `help <cmd>` and
 * the completion providers all read this one table.
 */

#define SHELL_CORE_HELP_ENTRY_COUNT \
    ((int)(sizeof(shell_core_help_entries) / sizeof(shell_core_help_entries[0])))

/* One-line entry in the offline command reference (`help /all`, `help <cmd>`).
 * The entry type itself is declared in shell_core_internal.h, because shell.c
 * formats a single entry. */
const shell_help_entry_t shell_core_help_entries[] = {
    { "help",     "help [cmd | /all] - this summary; help /all lists every command; help <cmd> shows one" },
    { "about",    "about - project identity, build date/time, Git hash, license, third-party summary" },
    { "version",  "version | ver - banner, version, build date/time, Git hash, IDF, board, heap, uptime" },
    { "sysinfo",  "sysinfo - board, display, storage, heap, FreeRTOS tasks, uptime, Wi-Fi, OTA state" },
    { "mem",      "mem - free heap, total, minimum, internal, task count, PSRAM state" },
    { "debug",    "debug [save [file] [txt|csv|json] | mem <addr> [len] | peek <addr> | poke <addr> <hex..> | disk <lba> [count]] - log, RAM/sector hexdumps" },
    { "clear",    "clear | cls - clear transcript history and redraw the prompt" },
    { "reboot",   "reboot - restart the board" },
    { "launch",   "launch | launch <name> [args] | launch /list - discover and run script apps .bat/.cmd (PATH + sd:/APPS)" },
    { "apps",     "apps - list the registered native apps (applib ABI table)" },
    { "ps",       "ps | tasks | top [/b] [/O:key] - FreeRTOS task list (name, state, prio, core, stack, CPU%)" },
    { "tasks",    "tasks - alias of ps (FreeRTOS task list)" },
    { "top",      "top [/b] [/O:key] - task list sorted by CPU with a live summary header" },
    { "brightness", "brightness <0-100> - set display backlight" },
    { "rotate",   "rotate <0|90|180|270> - rotate the display (touch coordinates follow)" },
    { "battery",  "battery - read battery voltage/percent from the ADC divider" },
    { "power",    "power [status] | power idle [seconds|off] - display power state and idle display-off timeout" },
    { "sleep",    "sleep - enter light sleep (timer/GPIO wake); display-only idle is power/display off" },
    { "deepsleep", "deepsleep - enter ESP deep sleep (wake on configured source)" },
    { "shutdown", "shutdown (poweroff) - flush, LEDs off, and cut board power (deep sleep when no latch)" },
    { "pwm",      "pwm <pin> <freq> <duty%> - LEDC PWM tone on a pin" },
    { "freq",     "freq <pin> <hz> - square wave generator on a pin" },
    { "tone",     "tone <freq> [duration_ms] - play a tone through the speaker" },
    { "wavplay",  "wavplay <file.wav> - play a WAV file from SD" },
    { "audio",    "audio status | audio stop - background playback state" },
    { "mic",      "mic level [ms] | mic hear <freq> [ms] | mic audit [freq] | mic selftest - mic level, tone detect, speaker audit + hiss probe" },
    { "adc",      "adc <pin> - one-shot ADC read on a pin" },
    { "i2c",      "i2c scan | peek <addr> <reg> | poke <addr> <reg> <val> - I2C bus tools" },
    { "spi",      "spi status | spi loopback <sclk> <mosi> <miso> | spi peek|poke <sclk> <mosi> <miso> <cs> <reg> [value] | spi release - SPI master (SPI3, GPIO-matrix pins)" },
    { "rgb",      "rgb status | rgb <#RRGGBB|r g b|effect> | rgb auto <on|off> | rgb <1|2> r g b - status LED(s)" },
    { "imu",      "imu [read] | imu status | imu rotate <on|off> - BMI270 accel/gyro + tilt auto-rotate" },
    { "gpio",     "gpio list | status | read <pin> | set <pin> <0|1> - digital IO" },
    { "volume",   "volume <0-100> - set speaker volume" },
    { "display",  "display info | resolution | refresh | power <on|sleep|off>" },
    { "keyboard", "keyboard show | hide | toggle | status - on-screen keyboard" },
    { "windows",  "windows info - window layout state" },
    { "ui",       "ui tap|longpress|swipe|press|move|release|key|target|targets|hit|state - synthetic touch automation" },
    { "dialog",   "dialog [/t:secs] \"title\" \"message\" [button1] [button2] - message box (ERRORLEVEL 0/1/255)" },
    { "list",     "list [/t:secs] [/v:NAME] \"title\" item... - scrollable selector (ERRORLEVEL = 0-based index)" },
    { "ask",      "ask [/t:secs] [/v:NAME] [/p] \"prompt\" [default] - text input to ASK_RESULT (ERRORLEVEL 0/1)" },
    { "screen",   "screen run <file.frm> | screen flow <file.flow> | screen info <file> - declarative screen (dialog|list|ask|form|menu|browse|view|hexview|image) or multi-screen flow, results to env" },
    { "browse",   "browse [/t:secs] [/v:NAME] [path] - fullscreen file picker (ERRORLEVEL 0/1)" },
    { "view",     "view [/t:secs] [--raw] <file> - text viewer pager (.md renders in the reading font, .bmp/.dib open the image viewer) (ERRORLEVEL 0/1)" },
    { "open",     "open [/t:secs] [--raw] <file> - open by type: scripts in editor, md rendered, images viewed, rest as text (never executes)" },
    { "json",     "json validate|pretty <file> - check structure or print 2-space indented JSON (ERRORLEVEL 0/1)" },
    { "js",       "js eval <code> | js run <file.js> | js call <file.js> <fn> [args] - JavaScript engine (ERRORLEVEL 0/1/2)" },
    { "browser",  "browser <url> | browser back | browser forward - basic web browser (ERRORLEVEL 0/1/2)" },
    { "csv",      "csv rows|cols|cell|get|set|eval <file> [row col] [/b] [/v:NAME] - CSV grid (R1C1 refs, ranges, =EXPR via calc) (ERRORLEVEL 0/1/2)" },
    { "export",   "export <db NAME|alarms> <csv|json|txt|vcf|ics> <file> - portable store interchange (ERRORLEVEL 0/1/2)" },
    { "import",   "import db <name> <csv|json|vcf> <file> | import alarms <csv|json|ics> <file> - store interchange in, fresh ids (ERRORLEVEL 0/1/2)" },
    { "pim",      "pim get db <name> | pim get alarms | pim put db <name> <size> [/crc] | pim put alarms <size> [/crc] - serial PIM sync, merge by uid (ERRORLEVEL 0/1/2)" },
    { "sync",     "sync [status] - P4Sync handshake: sync.* capability snapshot for USB desktop sync (ERRORLEVEL 0/2)" },
    { "mcp",      "mcp status | mcp connect <url> [token] | mcp tools | mcp call <tool> [json] - MCP JSON-RPC client (ERRORLEVEL 0/1/2)" },
    { "net",      "net status|broker|connect|sub|pub|msg|onmsg|outbox - persistent MQTT service with SD outbox (ERRORLEVEL 0/1/2)" },
    { "archive",  "archive create|extract|list|verify ... - USTAR backups with CRC manifest (ERRORLEVEL 0/1/2)" },
    { "backup",   "backup <file> [paths...] - archive DBS + alarms by default (ERRORLEVEL 0/1/2)" },
    { "crypt",    "crypt lock|unlock <src> <dst> [/p:pass|/ask] - password file encryption (ERRORLEVEL 0/1/2)" },
    { "hexview",  "hexview [/t:secs] <file> - 16-byte hex dump pager (ERRORLEVEL 0/1)" },
    { "image",    "image info <file.bmp> | image show [/t:secs] <file.bmp> - BMP metadata / fit-to-screen viewer (ERRORLEVEL 0/1/2)" },
    { "draw",     "draw <box|line|fill|text|bar|table|list|image|clear|window|save|restore|cursor|hold|alt-screen|close|refresh|fullscreen> - TUI drawing (foreground only)" },
    { "anchor",   "anchor <label> <command> [continue_line] - named transcript anchor region" },
    { "link",     "link <label> <command...> | link list | link open <n> - tap-confirm hyperlink (underline)" },
    { "complete", "complete <partial line> - list Tab-completion matches as runnable links" },
    { "tui",      "tui status|stats|clear|fullscreen|refresh|selftest - TUI control (selftest runs headless palette/coverage/metrics checks)" },
    { "wig",     "wig /panel|text|gauge|button|list|clear|render - declarative widget layer (foreground only)" },
    { "pane",     "pane /add|split|focus|cycle|hide|close|list - pane stack management (foreground only)" },
    { "hud",      "hud /add|refresh|flash|del - HUD status cells (foreground only)" },
    { "key",      "key /bind|unbind|inject|mode|list - key bindings and synthetic input" },
    { "tap",      "tap /on|off|clear|list - touch hotspot binding (foreground only)" },
    { "event",    "event /wait|type|field|dispatch|post|clear|count - script-visible event multiplexer" },
    { "trace",    "trace /on|off|level|status - on-screen diagnostics" },
    { "color",    "color [fg] [bg] - DOS COLOR parity (hex digits)" },
    { "locate",   "locate <row> <col> - DOS LOCATE parity (1-based, 80x25)" },
    { "config",   "config [KEY=VALUE | save | reset [key] | factory] - persistent settings (CONFIG.SYS)" },
    { "prompt",   "prompt [template] - set the command prompt template" },
    { "title",    "title [text] - show or set the session title (empty clears)" },
    { "cd",       "cd | chdir [path] - show or change the working directory" },
    { "dir",      "dir [path] [/W] [/P] [/S] [/B] [/L] [/A:attrs] [/O:order] - list directory" },
    { "copy",     "copy <src> <dst> - copy a file (dir target keeps basename)" },
    { "cursor",   "cursor [block|bar] [blink <ms 0..2000|off>] - input-line cursor style (session-only)" },
    { "move",     "move <src> <dst> - move a file (cwd-relative destination)" },
    { "del",      "del [/s] [/p|/f|/permanent] <path> - delete file(s); aliases erase" },
    { "ren",      "ren | rename <src> <dst> - rename a file" },
    { "md",       "md | mkdir <path> - create a directory" },
    { "rd",       "rd | rmdir <path> - remove a directory" },
    { "type",     "type <path> - print a file to the transcript" },
    { "write",    "write <path> <text> - write (overwrite) a file" },
    { "append",   "append <path> <text> - append text to a file" },
    { "touch",    "touch <path> - create/update a file timestamp" },
    { "attrib",   "attrib [+-RHSA] <path> - show/set file attributes" },
    { "label",    "label [name] - show/set the volume label" },
    { "vol",      "vol - show the volume label and serial number" },
    { "xcopy",    "xcopy <src> <dst> [/S] [/E] [/I] [/Y] - recursive copy" },
    { "chkdsk",   "chkdsk | scandisk [path] [/F] - read-only filesystem check" },
    { "format",   "format [/FS:FAT|FAT32] [/A:size] [/V:label] [/Q] - format the SD volume (interactive)" },
    { "find",     "find <text> [file] [/I] [/N] [/C] [/V] - text search, or recursive file discovery (/NAME: et al)" },
    { "findstr",  "findstr [switches] <search> [file...] - literal or regex-lite text search (/R /C /I /N /V /X /E /B /L /S /M /F /G)" },
    { "more",     "more [file] - paginated output (keypress- or timer-bounded)" },
    { "tree",     "tree [path] [/F] [/A] - directory tree" },
    { "fc",       "fc <f1> <f2> - compare two files" },
    { "font",     "font info | font coverage | font list | font set <terminal|ui|reading> <name> [/save] | font size <terminal|ui|reading> <px> [/save] - roles, coverage, live TTF switching + sizes" },
    { "theme",    "theme list | show [name] | set <name> [/save] - switch the UI color theme (persists in SHELL.INI)" },
    { "comp",     "comp <f1> <f2> - compare two files byte-by-byte" },
    { "sort",     "sort [file] [/R] [/I] [/U] - sort lines (pipes: sort < f | sort)" },
    { "clip",     "clip [text | copy [N] | file <path> | read <file> | paste <dest>] - clipboard" },
    { "history",  "history [ /save [file] | /load [file] | /search <text> | /clear ] - command recall buffer" },
    { "edit",     "edit <file> | edit db <name> <id> | edit alarm <id> [/focus] [/template <name>] - modal text editor (byte-preserving document model)" },
    { "spell",    "spell learn <word> | spell forget <word> | spell list user - user dictionary overlay" },
    { "recover",  "recover [list] | recover restore <path> | recover discard <path> | recover clear - editor crash-file recovery" },
    { "trash",    "trash - show trash | trash <path> - move to trash | trash empty - empty it" },
    { "undelete", "undelete <path> - restore a file from trash" },
    { "receive",  "receive <path> <size> [/crc] - host-to-device binary transfer (USB serial)" },
    { "send",     "send <path> [offset] [count] | send /diag - device-to-host binary transfer" },
    { "screenshot", "screenshot | scr | capture [file.bmp] - capture the screen to a BMP" },
    { "sd",       "sd info | ls [path] | stat <path> | cat <path> [bytes] | mount | eject" },
    { "sdeject",  "sdeject - safe SD unmount before card removal" },
    { "disk",     "disk list | detail | clean | create partition primary [size=N] | delete partition N | active N|none | type N <hex> | mbr [save|restore] | verify | geometry | image save|restore | format" },
    { "set",      "set [NAME=VALUE] | set /a NAME=<expr> | set /p NAME=<prompt> - environment variables" },
    { "calc",     "calc [NAME=] <expr> | calc /deg | /rad | /angle | /hex | /fin | /date - float calculator (BASIC/financial/date funcs, PI, RAN#, &H hex)" },
    { "path",     "path [dirs] - show/set the executable search path" },
    { "echo",     "echo <text> | echo on|off - print text or toggle command echo" },
    { "call",     "call <file.bat> [args] - run a batch file from another" },
    { "if",       "if [not] errorlevel|exist|\"a\"==\"b\" <cmd> - conditional execution" },
    { "goto",     "goto :label - jump to a label in a batch file" },
    { "gosub",    "gosub :label [args] | <file>::<routine> - call a labelled subroutine" },
    { "return",   "return [code] - return from a gosub/call :label (ends the file at top level)" },
    { "on",       "on <expr> goto|gosub <label>[,<label>...] - computed dispatch" },
    { "switch",   "switch <value> <match>:<label> [...] [/d:<label>] - string-match dispatch to a label" },
    { "shift",    "shift - shift batch arguments" },
    { "pause",    "pause [message] - wait for a key (30 s timeout)" },
    { "choice",   "choice [/C:keys] [/N] [/T:c,secs] [/S] [text] - interactive selection" },
    { "delay",    "delay <ms> - pure deterministic wait (melodies/demos), clamped to P4_CONFIG_DELAY_MAX_MS" },
    { "gfx",      "gfx init|close|status|stats|clear|pixel|line|rect|circle|hline|vline|triangle|ellipse|polygon|fill|text|show|image|load|blit|blitmany|free|slots|save - RGB565 canvas + toolkit (max 320x240); image <file.bmp> blits a scaled BMP; load ingests 24/32-bit BMP sprites (max 64x64); blitmany stamps one slot at many X Y pairs; stats reports frame pacing" },
    { "plot",     "plot tui|window|auto|axes|func|polar|para|data|bar|table|line|point|clear|status - world-coordinate graphs + charts on the gfx canvas or TUI (uses calc)" },
    { "crc32",    "crc32 <path> - print a file's CRC-32 checksum (ERRORLEVEL 0/1)" },
    { "asset",    "asset check|list <app> - verify/list an app's APPS/<APP>.ASSETS manifest (ERRORLEVEL 0/1)" },
    { "pkg",      "pkg list|info <app>|verify <app>|check|install <app>|remove <app> - SD app packages (APPS/<APP>.APPINFO + .ASSETS, bundles under PKGS/)" },
    { "header",   "header [status] | mode [auto|full|compact] [/save] | show|hide - responsive status-bar layout" },
    { "start",    "start [/wait] [/t:secs] <command> [args] | start /b - run as a background job; bare start lists the pool (/b: machine rows bgN|state|exit|prio|cpu|stack|ovl|himem|line); /wait joins, /t: auto-kills" },
    { "taskkill", "taskkill <job> - cooperatively stop a background job (name like bg0, or slot number)" },
    { "kill",     "kill <job> - alias of taskkill (Unix name, DOS behavior)" },
    { "suspend",  "suspend <job> - freeze a background pool job (pool slots only; taskkill wakes it to stop)" },
    { "resume",   "resume <job> - thaw a suspended background pool job" },
    { "nice",     "nice <job> <1..3|+n|-n> - set a pool job priority within P4_CONFIG_TMGR_PRIO bounds (renice is an alias)" },
    { "renice",   "renice <job> <1..3|+n|-n> - alias of nice" },
    { "taskmgr",  "taskmgr - open the task manager app (same as launch TASKMGR)" },
    { "assoc",    "assoc [.ext[=app]] - show/set/clear file associations (bare lists; .ext= clears)" },
    { "ftype",    "ftype [app] - show declared (APPINFO handles=) and assigned handlers" },
    { "startup",  "startup [list] | add <app> | remove <app> - one-shot apps run at boot (sd:/STARTUP.INI)" },
    { "service",  "service list | install <name> <line> [/restart] [/noauto] | remove|start|stop <name> - resident services (supervised, boot-persistent)" },
    { "lock",     "lock <name> [/t:secs] - take a named batch mutex (auto-released at job exit)" },
    { "unlock",   "unlock <name> - release a named batch mutex" },
    { "signal",   "signal <name> - set a named batch event (sticky until taken)" },
    { "wait",     "wait <name> [/t:secs] - wait for a named batch event (auto-clears on take)" },
    { "himem",    "himem [info] | alloc <bytes>[K|M] [/v:VAR] | free|lock|unlock <h> | fill|load|save|crc <h> ... - XMS-style extended memory over PSRAM" },
    { "uptime",   "uptime - seconds since boot (also in version)" },
    { "df",       "df - free/total space for every mounted volume (SD + USB sticks)" },
    { "env",      "env | printenv - alias of bare set (list environment variables)" },
    { "hostname", "hostname [name] - show or set the device hostname (persisted in CONFIG.SYS)" },
    { "for",      "for %v in (set) do <cmd> | for /f \"delims= tokens=\\n\" %%v in (file) do <cmd> | for /L %%v in (start,step,end) do <cmd> | for /A %%k in (PREFIX) do <cmd> | for /D %%v in (set) do <cmd> | for /R [path] %%v in (set) do <cmd> - loops" },
    { "while",    "while <expr> do <cmd> - re-run while a set /a expression is nonzero (ERRORLEVEL 0/1/2)" },
    { "setlocal", "setlocal [enabledelayedexpansion|disabledelayedexpansion] - begin a local environment scope (!VAR! when enabled)" },
    { "endlocal", "endlocal - end a local environment scope" },
    { "exit",     "exit [/b] [code] - leave a batch file or the shell" },
    { "proc",     "proc [/args | /name | /depth | /errorlevel | /echo | /stdin | /labels | /goto] - introspect the batch process stack" },
    { "breadcrumb", "breadcrumb [/b] - print the current location trail (cwd > batch stack > bg slot > modal)" },
    { "ini",      "ini <list|get|set|del|load|save> <file> [key] [value] - persistent state in KEY=VALUE files on the SD card" },
    { "appconfig", "appconfig <app> [path|list|get|set|del] [key] [value] - per-app settings (sd:/APPS/<APP>.INI)" },
    { "temp",     "temp [new [ext] | clean] - SD-backed temporary files" },
    { "ansi",     "ansi <sgr-codes> [text...] - emit ANSI-styled text (reverse, bold, color) into the transcript" },
    { "menu",     "menu <item> [item...] - numbered menu; ERRORLEVEL = chosen index (0 = cancel)" },
    { "mode",     "mode /push NAME [/type T] [/map M] | mode /pop | mode /switch NAME | mode /top | mode /list | mode /count | mode /drop TYPE | mode /timeout MS - mode stack" },
    { "form",     "form [/t:secs] \"title\" \"Label=type[:arg]:VAR\" ... - multi-field modal (text|password|check|select|range), values to env (ERRORLEVEL 0/255)" },
    { "markdown", "markdown <file> | markdown -e <text> | markdown on | off | markdown export <src> <out> [text|html|print] - render/export Markdown (echo/type auto-render; /raw bypasses)" },
    { "notify",   "notify [/t:secs] <text> | notify - - header notification (clear with -)" },
    { "appmode",  "appmode on [/full] [/clear] | appmode off | appmode status - enter/exit app mode (save/restore screen)" },
    { "rem",      "rem <text> - batch comment" },
    { "alias",    "alias [name[=value]] | alias /save [/load] [file] - DOSKEY-style macros" },
    { "unalias",  "unalias <name> - remove a macro alias" },
    { "bind",     "bind [F1..F12|^A..^Z <line>] | bind unbind <key> | bind /save [/load] [file] | bind /clear - key bindings (^C reserved)" },
    { "mbind",    "mbind /map NAME CHORD LABEL | mbind /unbind NAME CHORD | mbind /leader KEY | mbind /list NAME - modal keymap chords" },
    { "mcmd",     "mcmd /def NAME LABEL [/doc D] | mcmd /list | mcmd /run NAME | mcmd /undef NAME - named commands" },
    { "mdialog",  "mdialog /confirm TEXT [/var V] | mdialog /input TEXT [/default D] [/var V] | mdialog /result V - nested dialogs" },
    { "guard",    "guard /on enter|leave TYPE LABEL | guard /off enter|leave TYPE | guard /list - transition guards" },
    { "scope",    "scope /save MODE VAR [VAR ...] | scope /restore MODE | scope /list MODE | scope /clear MODE - mode-scoped state" },
    { "palette",  "palette /open | palette /close | palette /source NAME - command palette" },
    { "mmacro",   "mmacro /record NAME | mmacro /stop | mmacro /replay NAME [N] | mmacro /list - event macro record/replay" },
    { "pending",  "pending /get | pending /clear | pending /timeout MS - chord buffer" },
    { "mstatus",  "mstatus /set TYPE FORMAT | mstatus /render - per-mode statusline" },
    { "repeat",   "repeat /last [N] | repeat /set N | repeat /clear - repeat count" },
    { "cancel",   "cancel [/to NAME] [/all] [/code N] - unwind modal state" },
    { "buffer",   "buffer /open NAME | buffer /close NAME | buffer /list | buffer /mode NAME MODE - modal text buffer" },
    { "macro",    "macro record [file] | macro stop | macro play <file> | macro status - capture/replay command macros" },
    { "date",     "date [MM-DD-YYYY] - show/set the date" },
    { "db",       "db <create|list|info|drop|open|close|categories|add|get|set|del|purge|count|find|export|import> [...] - Palm-OS-style SD record store" },
    { "alarm",    "alarm <add|list|del|enable|disable|snooze|status|purge> [...] - SD-persisted alarms (daily/weekly/monthly/yearly, header notify / beep / LED / optional run)" },
    { "cal",      "cal [today|week|next|YYYY-MM] - calendar grid + events over the alarm store" },
    { "gfind",    "gfind <text> [/b] [/i] [/count] [/db:name] [/cat:N] [/field:k=v] [/files [/root:path] [/ext:.txt,.md] [/hidden] [/filesonly]] - Palm-style global find over db + alarms + (opt-in) text files" },
    { "time",     "time [HH:MM[:SS]] - show/set the time" },
    { "timer",    "timer|stopwatch start|stop|lap|status [name] [/b] [/v:NAME] - named stopwatch runs" },
    { "timezone", "timezone - show/set the timezone" },
    { "sntp",     "sntp | ntpsync [server] - sync the clock via SNTP" },
    { "locale",   "locale [US|UK|DE|FR|list] - show/set locale for date/time/number formatting" },
    { "rtc",      "rtc [anchor] - RTC backup status (source, anchor age, ext chip)" },
    { "wifi",     "wifi status | scan [/b] | diag | connect [ssid pass] | disconnect | setup [on|off]" },
    { "bluetooth", "bluetooth | bt status | scan [limit] | advertise <on [name]|off> | connect <addr> | disconnect | mouse <on|off|cursor <on|off>|click <on|off>>" },
    { "usb",      "usb status | ls [path] | keyboard <on|off> | mouse <on|off|cursor <on|off>|click <on|off>> | userial <status|open|close|send|recv|term>" },
    { "httpd",    "httpd status | start | stop - HTTP file server on port 80" },
    { "certs",    "certs [info|list|add <file>|remove <name>|rebuild|clear|reload] - TLS trust store" },
    { "netstat",  "netstat - active TCP/UDP connections and listeners" },
    { "ipconfig", "ipconfig [/all] - IP configuration" },
    { "ping",     "ping <host-or-ip> [count] - ICMP echo" },
    { "dns",      "dns | nslookup <hostname> - resolve a hostname" },
    { "httpget",  "httpget | wget <url> [localfile] - download over HTTP(S)" },
    { "tcpterm",  "tcpterm <host> <port> [/t:secs] [text...] - one-shot TCP request/response (ERRORLEVEL 0/1/2)" },
    { "c6ota",    "c6ota <sd:/path | http[s]://url | default> - OTA the ESP32-C6 firmware" },
    { "camera",   "camera init | snap <filename> - camera stack (stub: not wired)" },
};

#define SHELL_CORE_HELP_ENTRY_COUNT \
    ((int)(sizeof(shell_core_help_entries) / sizeof(shell_core_help_entries[0])))

size_t shell_help_entry_count(void)
{
    return (size_t)SHELL_CORE_HELP_ENTRY_COUNT;
}

bool shell_help_entry_get(size_t index, const char **name, const char **usage)
{
    if (index >= (size_t)SHELL_CORE_HELP_ENTRY_COUNT) {
        return false;
    }
    if (name != NULL) {
        *name = shell_core_help_entries[index].name;
    }
    if (usage != NULL) {
        *usage = shell_core_help_entries[index].summary;
    }
    return true;
}

