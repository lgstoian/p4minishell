/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file shell_core.c
 * @brief Hardware-free shell core: text helpers and the statement scanner.
 *
 * Everything here is pure C. It is the half of the shell that can run without
 * a display, a panel, USB-Serial-JTAG, the SD card or the board BSP, which is
 * what lets the host unit-test lane (test/host) exercise it natively. The
 * transcript's LVGL label, the UART mirror and the input line stay in
 * shell.c.
 *
 * One quote/escape scanner backs every surface that must distinguish syntax
 * from data: the argument tokenizer, redirection parsing, pipe splitting and
 * command chaining. Keeping them together means those places can never
 * disagree about what a quote means.
 *
 * @module components/shell
 * @layer shell
 * @domain shell
 * @provides shell_trim, shell_split_args, shell_split_chain, quote/escape scanner, group depth
 * @status active
 * @test test/main/test_shell_quoting.c
 * @hard-rule pure C only - no LVGL, display, USB, storage or board BSP in this file
 */

#include "shell.h"
#include "p4minishell_config.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

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
