/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file math_disp_commands.c
 * @brief `math` verb family - Natural Display math editor.
 */

#include "command.h"
#include "calc.h"
#include "math_disp.h"
#include "shell.h"
#include "strutil.h"
#include "p4minishell_config.h"
#include <string.h>
#include <stdlib.h>

/* Command-line buffer size, per the component convention. */
#define SHELL_COMMAND_BYTES P4_CONFIG_COMMAND_BYTES

/* Global math display handle */
static math_disp_expr_t *s_math_disp_handle = NULL;

/* Forward declarations */
static int math_cmd_new(int argc, char *argv[]);
static int math_cmd_open(int argc, char *argv[]);
static int math_cmd_close(int argc, char *argv[]);
static int math_cmd_edit(int argc, char *argv[]);
static int math_cmd_render(int argc, char *argv[]);
static int math_cmd_eval(int argc, char *argv[]);
static int math_cmd_cursor(int argc, char *argv[]);
static int math_cmd_insert(int argc, char *argv[]);

/* ---- math new ---- */
static int math_cmd_new(int argc, char *argv[])
{
    if (s_math_disp_handle) {
        math_disp_expr_free(s_math_disp_handle);
    }
    s_math_disp_handle = math_disp_expr_new();
    if (!s_math_disp_handle) {
        shell_print_error("math: out of memory");
        return 1;
    }
    shell_print_muted("math: new expression created");
    return 0;
}

/* ---- math open <expr> ---- */
static int math_cmd_open(int argc, char *argv[])
{
    if (argc < 2) {
        shell_print_error("math: open <expression>");
        return 2;
    }
    
    if (s_math_disp_handle) {
        math_disp_expr_free(s_math_disp_handle);
    }
    
    s_math_disp_handle = math_disp_expr_new();
    if (!s_math_disp_handle) {
        shell_print_error("math: out of memory");
        return 1;
    }
    
    /* Join remaining args as expression */
    size_t total_len = 0;
    for (int i = 1; i < argc; i++) {
        total_len += strlen(argv[i]) + 1;
    }
    
    char *expr_text = malloc(total_len + 1);
    if (!expr_text) {
        math_disp_expr_free(s_math_disp_handle);
        s_math_disp_handle = NULL;
        return 1;
    }
    
    char *p = expr_text;
    for (int i = 1; i < argc; i++) {
        if (i > 1) *p++ = ' ';
        strcpy(p, argv[i]);
        p += strlen(argv[i]);
    }
    *p = '\0';
    
    int err = math_disp_parse_with_calc(s_math_disp_handle, expr_text);
    free(expr_text);
    
    if (err != 0) {
        shell_print_error("math: parse failed: %s", s_math_disp_handle->error_msg);
        math_disp_expr_free(s_math_disp_handle);
        s_math_disp_handle = NULL;
        return 1;
    }
    
    shell_print_muted("math: opened expression");
    return 0;
}

/* ---- math close ---- */
static int math_cmd_close(int argc, char *argv[])
{
    if (!s_math_disp_handle) {
        shell_print_error("math: no expression open");
        return 1;
    }
    math_disp_expr_free(s_math_disp_handle);
    s_math_disp_handle = NULL;
    shell_print_muted("math: closed");
    return 0;
}

/* ---- math edit ---- */
static int math_cmd_edit(int argc, char *argv[])
{
    if (!s_math_disp_handle) {
        shell_print_error("math: no expression open");
        return 1;
    }
    
    if (argc < 2) {
        shell_print_muted("math edit: left|right|up|down|home|end|word_left|word_right");
        return 0;
    }
    
    const char *cmd = argv[1];
    
    if (strcasecmp(cmd, "left") == 0) math_disp_cursor_left(s_math_disp_handle);
    else if (strcasecmp(cmd, "right") == 0) math_disp_cursor_right(s_math_disp_handle);
    else if (strcasecmp(cmd, "up") == 0) math_disp_cursor_up(s_math_disp_handle);
    else if (strcasecmp(cmd, "down") == 0) math_disp_cursor_down(s_math_disp_handle);
    else if (strcasecmp(cmd, "home") == 0) math_disp_cursor_home(s_math_disp_handle);
    else if (strcasecmp(cmd, "end") == 0) math_disp_cursor_end(s_math_disp_handle);
    else if (strcasecmp(cmd, "word_left") == 0) math_disp_cursor_word_left(s_math_disp_handle);
    else if (strcasecmp(cmd, "word_right") == 0) math_disp_cursor_word_right(s_math_disp_handle);
    else {
        shell_print_error("math edit: unknown direction");
        return 2;
    }
    return 0;
}

/* ---- math render ---- */
static int math_cmd_render(int argc, char *argv[])
{
    if (!s_math_disp_handle) {
        shell_print_error("math: no expression open");
        return 1;
    }
    
    math_disp_layout(s_math_disp_handle);
    
    char *buf = malloc(8192);
    if (!buf) return 1;
    
    size_t len;
    math_disp_render_ansi(s_math_disp_handle, buf, 8192, &len);
    shell_transcript_appendf_ansi("%s", buf);
    free(buf);
    return 0;
}

/* ---- math eval ---- */
static int math_cmd_eval(int argc, char *argv[])
{
    if (!s_math_disp_handle) {
        shell_print_error("math: no expression open");
        return 1;
    }
    
    double result;
    const char *error;
    if (!math_disp_evaluate(s_math_disp_handle, &result, &error)) {
        shell_print_error("math: eval failed: %s", error ? error : "unknown");
        return 1;
    }
    
    char buf[64];
    calc_format_number(result, buf, sizeof(buf));
    shell_print_muted("= %s", buf);
    return 0;
}

/* ---- math cursor ---- */
static int math_cmd_cursor(int argc, char *argv[])
{
    if (!s_math_disp_handle) {
        shell_print_error("math: no expression open");
        return 1;
    }
    
    if (argc < 2) {
        shell_print_muted("cursor at node: %p", s_math_disp_handle->cursor.node);
        return 0;
    }
    
    const char *cmd = argv[1];
    
    if (strcasecmp(cmd, "home") == 0) math_disp_cursor_home(s_math_disp_handle);
    else if (strcasecmp(cmd, "end") == 0) math_disp_cursor_end(s_math_disp_handle);
    else if (strcasecmp(cmd, "left") == 0) math_disp_cursor_left(s_math_disp_handle);
    else if (strcasecmp(cmd, "right") == 0) math_disp_cursor_right(s_math_disp_handle);
    else if (strcasecmp(cmd, "up") == 0) math_disp_cursor_up(s_math_disp_handle);
    else if (strcasecmp(cmd, "down") == 0) math_disp_cursor_down(s_math_disp_handle);
    else if (strcasecmp(cmd, "home") == 0) math_disp_cursor_home(s_math_disp_handle);
    else if (strcasecmp(cmd, "end") == 0) math_disp_cursor_end(s_math_disp_handle);
    else if (strcasecmp(cmd, "word_left") == 0) math_disp_cursor_word_left(s_math_disp_handle);
    else if (strcasecmp(cmd, "word_right") == 0) math_disp_cursor_word_right(s_math_disp_handle);
    else {
        shell_print_error("math cursor: unknown command");
        return 2;
    }
    return 0;
}

/* ---- math insert ---- */
static int math_cmd_insert(int argc, char *argv[])
{
    if (!s_math_disp_handle) {
        shell_print_error("math: no expression open");
        return 1;
    }
    
    if (argc < 2) {
        shell_print_error("math insert: frac|sqrt|power|sub|paren|func <name>|text <text>");
        return 2;
    }
    
    const char *cmd = argv[1];
    
    if (strcasecmp(cmd, "frac") == 0) {
        return math_disp_insert_fraction(s_math_disp_handle);
    } else if (strcasecmp(cmd, "sqrt") == 0) {
        return math_disp_insert_sqrt(s_math_disp_handle);
    } else if (strcasecmp(cmd, "power") == 0) {
        return math_disp_insert_power(s_math_disp_handle);
    } else if (strcasecmp(cmd, "sub") == 0) {
        return math_disp_insert_subscript(s_math_disp_handle);
    } else if (strcasecmp(cmd, "paren") == 0) {
        return math_disp_insert_parens(s_math_disp_handle);
    } else if (strcasecmp(cmd, "func") == 0) {
        if (argc < 3) {
            shell_print_error("math insert func <name>");
            return 2;
        }
        return math_disp_insert_function(s_math_disp_handle, argv[2]);
    } else if (strcasecmp(cmd, "text") == 0) {
        if (argc < 3) {
            shell_print_error("math insert text <text>");
            return 2;
        }
        size_t len = 0;
        for (int i = 2; i < argc; i++) len += strlen(argv[i]) + 1;
        char *text = malloc(len + 1);
        if (!text) return 1;
        char *p = text;
        for (int i = 2; i < argc; i++) {
            if (i > 2) *p++ = ' ';
            strcpy(p, argv[i]);
            p += strlen(argv[i]);
        }
        *p = '\0';
        int err = math_disp_insert_text(s_math_disp_handle, text);
        free(text);
        return err;
    } else {
        shell_print_error("math insert: unknown command");
        return 2;
    }
    return 0;
}

/* ---- Command table ---- */

static const shell_command_entry_t s_math_commands[] = {
    { "new",      math_cmd_new,      "math new — create new expression" },
    { "open",     math_cmd_open,     "math open <expression> — parse and open expression" },
    { "close",    math_cmd_close,    "math close — close current expression" },
    { "edit",     math_cmd_edit,     "math edit left|right|up|down|home|end|word_left|word_right" },
    { "render",   math_cmd_render,   "math render — render current expression to ANSI" },
    { "eval",     math_cmd_eval,     "math eval — evaluate current expression" },
    { "cursor",   math_cmd_cursor,   "math cursor home|end|left|right|up|down|word_left|word_right" },
    { "insert",   math_cmd_insert,   "math insert frac|sqrt|power|sub|paren|func <name>|text <text>" },
};

const shell_command_entry_t *math_disp_commands_get_table(size_t *out_count)
{
    if (out_count) *out_count = sizeof(s_math_commands) / sizeof(s_math_commands[0]);
    return s_math_commands;
}

int shell_command_math(int argc, char **argv)
{
    if (argc < 2) {
        shell_print_muted("math new | open <expr> | close | edit <dir> | render | eval | cursor <cmd> | insert <type> [args]");
        return 2;
    }
    
    const shell_command_entry_t *table = math_disp_commands_get_table(NULL);
    size_t count = 0;
    math_disp_commands_get_table(&count);
    
    for (size_t i = 0; i < count; i++) {
        if (strcasecmp(argv[1], table[i].name) == 0) {
            int sub_argc = argc - 1;
            char **sub_argv = &argv[1];
            return table[i].handler(sub_argc, sub_argv);
        }
    }
    
    shell_print_error("math: unknown sub-command '%s'", argv[1]);
    shell_print_muted("math new | open <expr> | close | edit <dir> | render | eval | cursor <cmd> | insert <type> [args]");
    return 2;
}