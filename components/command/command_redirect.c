/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file command_redirect.c
 * @brief Pure DOS redirection parser shared by the firmware and the host lane.
 *
 * This was split out of command.c so the hardware-free host unit lane can
 * exercise the REAL parser (the one the firmware uses) instead of a stub.
 * It has no filesystem, display, USB or network dependency: the only helper
 * it calls is the shared quote/escape scanner in shell_core.c.
 */

#include "command.h"
#include "shell.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>

/* Token bound for the redirection parser below. */
#define SHELL_REDIRECT_TOKEN_MAX 4

/** Remove quoting and escape markup from a redirection target. */
static char *shell_redirect_unquote(char *target)
{
    return shell_unescape_in_place(target);
}

/** Find the next redirection operator that is neither quoted nor escaped. */
static char *shell_redirect_find_operator(char *cursor)
{
    return shell_find_unquoted_any(cursor, "><");
}

/** One redirection operator located during the scanning pass. */
typedef struct {
    char *position;
    char *target;
    bool is_output;
    bool is_append;
    bool has_handle;
} shell_redirect_token_t;

bool shell_parse_redirection(char *command,
                             char **command_part,
                             char **redirect_target,
                             bool *append_mode,
                             char **input_source,
                             bool *missing_target_out)
{
    shell_redirect_token_t tokens[SHELL_REDIRECT_TOKEN_MAX];
    size_t token_count = 0;
    char *cursor;
    size_t index;

    if (command_part == NULL || redirect_target == NULL ||
        append_mode == NULL || input_source == NULL) {
        return false;
    }

    *command_part = command;
    *redirect_target = NULL;
    *append_mode = false;
    *input_source = NULL;
    if (missing_target_out != NULL) {
        *missing_target_out = false;
    }

    if (command == NULL) {
        return false;
    }

    cursor = shell_redirect_find_operator(command);
    while (cursor != NULL && token_count < SHELL_REDIRECT_TOKEN_MAX) {
        shell_redirect_token_t *token = &tokens[token_count++];

        token->position = cursor;
        token->is_output = (*cursor == '>');
        token->is_append = token->is_output && (cursor[1] == '>');
        token->target = cursor + (token->is_append ? 2 : 1);
        token->has_handle = (cursor > command &&
                             (cursor[-1] == '0' || cursor[-1] == '1' || cursor[-1] == '2'));

        cursor = shell_redirect_find_operator(token->target);
    }

    if (token_count == 0) {
        *command_part = shell_trim(command);
        return false;
    }

    for (index = 0; index < token_count; index++) {
        tokens[index].position[0] = '\0';
        if (tokens[index].is_append) {
            tokens[index].position[1] = '\0';
        }
        if (tokens[index].has_handle) {
            tokens[index].position[-1] = '\0';
        }
    }

    for (index = 0; index < token_count; index++) {
        char *target = shell_redirect_unquote(shell_trim(tokens[index].target));

        if (target[0] == '\0' && missing_target_out != NULL) {
            *missing_target_out = true;
        }
        if (tokens[index].is_output) {
            *redirect_target = target;
            *append_mode = tokens[index].is_append;
        } else {
            *input_source = target;
        }
    }

    *command_part = shell_trim(command);
    return true;
}

shell_redirect_target_kind_t shell_redirect_target_kind(const char *target)
{
    const char *dot;
    size_t head_len;

    if (target == NULL || target[0] == '\0') {
        return SHELL_REDIRECT_FILE;
    }
    dot = strchr(target, '.');
    head_len = (dot != NULL) ? (size_t)(dot - target) : strlen(target);
    if (head_len == 3 && strncasecmp(target, "NUL", 3) == 0) {
        return SHELL_REDIRECT_NUL;
    }
    if (head_len == 3 && strncasecmp(target, "CON", 3) == 0) {
        return SHELL_REDIRECT_CON;
    }
    return SHELL_REDIRECT_FILE;
}
