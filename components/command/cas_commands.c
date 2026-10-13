/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file cas_commands.c
 * @brief `cas` verb family implementation.
 */

#include "command.h"
#include "calc.h"
#include "cas.h"
#include "shell.h"
#include "strutil.h"
#include "p4minishell_config.h"
#include <string.h>
#include <stdlib.h>

/* Command-line buffer size, per the component convention. */
#define SHELL_COMMAND_BYTES P4_CONFIG_COMMAND_BYTES


/* Global CAS handle */
static cas_expr_t *s_cas_handle = NULL;
static esp_err_t cas_cmd_expand(int argc, char *argv[]);
static esp_err_t cas_cmd_factor(int argc, char *argv[]);
static esp_err_t cas_cmd_diff(int argc, char *argv[]);
static esp_err_t cas_cmd_integrate(int argc, char *argv[]);
static esp_err_t cas_cmd_solve(int argc, char *argv[]);
static esp_err_t cas_cmd_series(int argc, char *argv[]);
static esp_err_t cas_cmd_substitute(int argc, char *argv[]);
static esp_err_t cas_cmd_evaluate(int argc, char *argv[]);
static esp_err_t cas_cmd_latex(int argc, char *argv[]);
static esp_err_t cas_cmd_open(int argc, char *argv[]);
static esp_err_t cas_cmd_close(int argc, char *argv[]);

/* Global CAS handle */

/* Forward declarations */
static esp_err_t cas_cmd_simplify(int argc, char *argv[]);
static esp_err_t cas_cmd_expand(int argc, char *argv[]);
static esp_err_t cas_cmd_factor(int argc, char *argv[]);
static esp_err_t cas_cmd_diff(int argc, char *argv[]);
static esp_err_t cas_cmd_integrate(int argc, char *argv[]);
static esp_err_t cas_cmd_solve(int argc, char *argv[]);
static esp_err_t cas_cmd_series(int argc, char *argv[]);
static esp_err_t cas_cmd_substitute(int argc, char *argv[]);
static esp_err_t cas_cmd_evaluate(int argc, char *argv[]);
static esp_err_t cas_cmd_latex(int argc, char *argv[]);
static esp_err_t cas_cmd_open(int argc, char *argv[]);
static esp_err_t cas_cmd_close(int argc, char *argv[]);

/* ---- cas simplify ---- */
static esp_err_t cas_cmd_simplify(int argc, char *argv[])
{
    if (argc < 2) {
        shell_print_error("cas: simplify <expression>");
        return 2;
    }
    
    if (!s_cas_handle) {
        shell_print_error("cas: no expression open");
        return 1;
    }
    
    char *text = malloc(SHELL_COMMAND_BYTES);
    if (!text) return 1;
    
    char *p = text;
    for (int i = 1; i < argc; i++) {
        if (i > 1) *p++ = ' ';
        size_t len = strlen(argv[i]);
        memcpy(p, argv[i], len);
        p += len;
    }
    *p = '\0';
    
    int err = cas_parse(s_cas_handle, text, NULL);
    free(text);
    
    if (err != 0) {
        shell_print_error("cas: parse failed: %s", s_cas_handle->error_msg);
        return 1;
    }
    
    cas_node_t *root = cas_simplify(s_cas_handle, s_cas_handle->root);
    if (!root) {
        shell_print_error("cas: simplify failed: %s", s_cas_handle->error_msg);
        return 1;
    }
    
    char *result = cas_to_string(s_cas_handle, root, false);
    if (!result) return 1;
    
    shell_transcript_appendf_ansi("%s", result);
    free(result);
    return 0;
}

/* ---- cas expand ---- */
static esp_err_t cas_cmd_expand(int argc, char *argv[])
{
    if (argc < 2) {
        shell_print_error("cas: expand <expression>");
        return 2;
    }
    
    if (!s_cas_handle) {
        shell_print_error("cas: no expression open");
        return 1;
    }
    
    char *text = malloc(SHELL_COMMAND_BYTES);
    if (!text) return 1;
    
    char *p = text;
    for (int i = 1; i < argc; i++) {
        if (i > 1) *p++ = ' ';
        size_t len = strlen(argv[i]);
        memcpy(p, argv[i], len);
        p += len;
    }
    *p = '\0';
    
    int err = cas_parse(s_cas_handle, text, NULL);
    free(text);
    
    if (err != 0) {
        shell_print_error("cas: parse failed: %s", s_cas_handle->error_msg);
        return 1;
    }
    
    cas_node_t *expanded = cas_expand(s_cas_handle, s_cas_handle->root);
    if (!expanded) {
        shell_print_error("cas: expand failed: %s", s_cas_handle->error_msg);
        return 1;
    }
    
    char *result = cas_to_string(s_cas_handle, expanded, false);
    if (!result) return 1;
    
    shell_transcript_appendf_ansi("%s", result);
    free(result);
    return 0;
}

/* ---- cas factor ---- */
static esp_err_t cas_cmd_factor(int argc, char *argv[])
{
    if (argc < 2) {
        shell_print_error("cas: factor <expression>");
        return 2;
    }
    
    if (!s_cas_handle) {
        shell_print_error("cas: no expression open");
        return 1;
    }
    
    char *text = malloc(SHELL_COMMAND_BYTES);
    if (!text) return 1;
    
    char *p = text;
    for (int i = 1; i < argc; i++) {
        if (i > 1) *p++ = ' ';
        size_t len = strlen(argv[i]);
        memcpy(p, argv[i], len);
        p += len;
    }
    *p = '\0';
    
    int err = cas_parse(s_cas_handle, text, NULL);
    free(text);
    
    if (err != 0) {
        shell_print_error("cas: parse failed: %s", s_cas_handle->error_msg);
        return 1;
    }
    
    cas_node_t *factored = cas_factor(s_cas_handle, s_cas_handle->root);
    if (!factored) {
        shell_print_error("cas: factor failed: %s", s_cas_handle->error_msg);
        return 1;
    }
    
    char *result = cas_to_string(s_cas_handle, factored, false);
    if (!result) return 1;
    
    shell_transcript_appendf_ansi("%s", result);
    free(result);
    return 0;
}

/* ---- cas diff ---- */
static esp_err_t cas_cmd_diff(int argc, char *argv[])
{
    if (argc < 3) {
        shell_print_error("cas: diff <expression> <variable>");
        return 2;
    }
    
    if (!s_cas_handle) {
        shell_print_error("cas: no expression open");
        return 1;
    }
    
    const char *var = argv[2];
    cas_node_t *diff = cas_diff(s_cas_handle, s_cas_handle->root, var);
    if (!diff) {
        shell_print_error("cas: diff failed: %s", s_cas_handle->error_msg);
        return 1;
    }
    
    char *result = cas_to_string(s_cas_handle, diff, false);
    if (!result) return 1;
    
    shell_transcript_appendf_ansi("%s", result);
    free(result);
    return 0;
}

/* ---- cas integrate ---- */
static esp_err_t cas_cmd_integrate(int argc, char *argv[])
{
    if (argc < 3) {
        shell_print_error("cas: integrate <expression> <variable> [a b]");
        return 2;
    }
    
    if (!s_cas_handle) {
        shell_print_error("cas: no expression open");
        return 1;
    }
    
    const char *var = argv[2];
    
    if (argc >= 5 && shell_text_equals_ignore_case(argv[3], "from")) {
        /* Definite integral */
        double a = strtod(argv[4], NULL);
        double b = strtod(argv[5], NULL);
        
        cas_node_t *integral = cas_integrate_definite(s_cas_handle, s_cas_handle->root, var, a, b);
        if (!integral) {
            shell_print_error("cas: integrate failed: %s", s_cas_handle->error_msg);
            return 1;
        }
        
        char *result = cas_to_string(s_cas_handle, integral, false);
        if (!result) return 1;
        
        shell_transcript_appendf_ansi("%s", result);
        free(result);
        return 0;
    }
    
    cas_node_t *integral = cas_integrate(s_cas_handle, s_cas_handle->root, var);
    if (!integral) {
        shell_print_error("cas: integrate failed: %s", s_cas_handle->error_msg);
        return 1;
    }
    
    char *result = cas_to_string(s_cas_handle, integral, false);
    if (!result) return 1;
    
    shell_transcript_appendf_ansi("%s", result);
    free(result);
    return 0;
}

/* ---- cas solve ---- */
static esp_err_t cas_cmd_solve(int argc, char *argv[])
{
    if (argc < 3) {
        shell_print_error("cas: solve <expression> <variable>");
        return 2;
    }
    
    if (!s_cas_handle) {
        shell_print_error("cas: no expression open");
        return 1;
    }
    
    const char *var = argv[2];
    cas_node_t *solution = cas_solve(s_cas_handle, s_cas_handle->root, var);
    if (!solution) {
        shell_print_error("cas: solve failed: %s", s_cas_handle->error_msg);
        return 1;
    }
    
    char *result = cas_to_string(s_cas_handle, solution, false);
    if (!result) return 1;
    
    shell_transcript_appendf_ansi("%s", result);
    free(result);
    return 0;
}

/* ---- cas series ---- */
static esp_err_t cas_cmd_series(int argc, char *argv[])
{
    if (argc < 4) {
        shell_print_error("cas: series <expression> <variable> <point> [order]");
        return 2;
    }
    
    if (!s_cas_handle) {
        shell_print_error("cas: no expression open");
        return 1;
    }
    
    const char *var = argv[2];
    double point = strtod(argv[3], NULL);
    int order = (argc >= 5) ? atoi(argv[4]) : 5;
    
    cas_node_t *series = cas_series(s_cas_handle, s_cas_handle->root, var, point, order);
    if (!series) {
        shell_print_error("cas: series failed: %s", s_cas_handle->error_msg);
        return 1;
    }
    
    char *result = cas_to_string(s_cas_handle, series, false);
    if (!result) return 1;
    
    shell_transcript_appendf_ansi("%s", result);
    free(result);
    return 0;
}

/* ---- cas substitute ---- */
static esp_err_t cas_cmd_substitute(int argc, char *argv[])
{
    if (argc < 4) {
        shell_print_error("cas: substitute <expression> <var> <value>");
        return 2;
    }
    
    if (!s_cas_handle) {
        shell_print_error("cas: no expression open");
        return 1;
    }
    
    const char *var = argv[2];
    double val = strtod(argv[3], NULL);
    
    cas_node_t *substituted = cas_eval_at(s_cas_handle, s_cas_handle->root, var, val);
    if (!substituted) {
        shell_print_error("cas: substitute failed: %s", s_cas_handle->error_msg);
        return 1;
    }
    
    char *result = cas_to_string(s_cas_handle, substituted, false);
    if (!result) return 1;
    
    shell_transcript_appendf_ansi("%s", result);
    free(result);
    return 0;
}

/* ---- cas evaluate ---- */
static esp_err_t cas_cmd_evaluate(int argc, char *argv[])
{
    if (argc < 2) {
        shell_print_error("cas: evaluate <expression>");
        return 2;
    }
    
    if (!s_cas_handle) {
        shell_print_error("cas: no expression open");
        return 1;
    }
    
    double result;
    if (!cas_eval_num(s_cas_handle, s_cas_handle->root, &result)) {
        shell_print_error("cas: evaluation failed: %s", s_cas_handle->error_msg);
        return 1;
    }
    
    char buf[64];
    calc_format_number(result, buf, sizeof(buf));
    shell_transcript_appendf_ansi("%s", buf);
    return 0;
}

/* ---- cas latex ---- */
static esp_err_t cas_cmd_latex(int argc, char *argv[])
{
    if (argc < 2) {
        shell_print_error("cas: latex <expression>");
        return 2;
    }
    
    if (!s_cas_handle) {
        shell_print_error("cas: no expression open");
        return 1;
    }
    
    char *result = cas_to_string(s_cas_handle, s_cas_handle->root, true);
    if (!result) return 1;
    
    shell_transcript_appendf_ansi("%s", result);
    free(result);
    return 0;
}

/* ---- cas open ---- */
static esp_err_t cas_cmd_open(int argc, char *argv[])
{
    if (argc < 2) {
        shell_print_error("cas: open <expression>");
        return 2;
    }
    
    if (s_cas_handle) {
        cas_expr_free(s_cas_handle);
    }
    s_cas_handle = cas_expr_new();
    if (!s_cas_handle) {
        shell_print_error("cas: out of memory");
        return 1;
    }
    
    char *text = malloc(SHELL_COMMAND_BYTES);
    if (!text) return 1;
    
    char *p = text;
    for (int i = 1; i < argc; i++) {
        if (i > 1) *p++ = ' ';
        size_t len = strlen(argv[i]);
        memcpy(p, argv[i], len);
        p += len;
    }
    *p = '\0';
    
    int err = cas_parse(s_cas_handle, text, NULL);
    free(text);
    
    if (err != 0) {
        shell_print_error("cas: parse failed: %s", s_cas_handle->error_msg);
        cas_expr_free(s_cas_handle);
        s_cas_handle = NULL;
        return 1;
    }
    
    shell_print_muted("cas: expression opened");
    return 0;
}

/* ---- cas close ---- */
static esp_err_t cas_cmd_close(int argc, char *argv[])
{
    if (!s_cas_handle) {
        shell_print_error("cas: no expression open");
        return 1;
    }
    cas_expr_free(s_cas_handle);
    s_cas_handle = NULL;
    shell_print_muted("cas: closed");
    return 0;
}

/* ---- Command table ---- */

static const shell_command_entry_t s_cas_commands[] = {
    { "open",     cas_cmd_open,     "cas open <expression> — parse and open expression" },
    { "close",    cas_cmd_close,    "cas close — close current expression" },
    { "simplify", cas_cmd_simplify, "cas simplify — simplify current expression" },
    { "expand",   cas_cmd_expand,   "cas expand — expand current expression" },
    { "factor",   cas_cmd_factor,   "cas factor — factor current expression" },
    { "diff",     cas_cmd_diff,     "cas diff <variable> — differentiate" },
    { "integrate", cas_cmd_integrate, "cas integrate <variable> [from <a> to <b>] — integrate" },
    { "solve",    cas_cmd_solve,    "cas solve <variable> — solve for variable" },
    { "series",   cas_cmd_series,   "cas series <point> [order] — Taylor series" },
    { "substitute", cas_cmd_substitute, "cas substitute <var> <value> — substitute value" },
    { "evaluate", cas_cmd_evaluate, "cas evaluate — evaluate current expression" },
    { "latex",    cas_cmd_latex,    "cas latex — output LaTeX" },
    { "close",    cas_cmd_close,    "cas close — close current expression" },
};

const shell_command_entry_t *cas_commands_get_table(size_t *out_count)
{
    if (out_count) *out_count = sizeof(s_cas_commands) / sizeof(s_cas_commands[0]);
    return s_cas_commands;
}

int shell_command_cas(int argc, char **argv)
{
    if (argc < 2) {
        shell_print_muted("cas open <expr> | close | simplify | expand | factor | diff | integrate | solve | series | substitute | evaluate | latex");
        return 2;
    }
    
    const shell_command_entry_t *table = cas_commands_get_table(NULL);
    size_t count = 0;
    cas_commands_get_table(&count);
    
    for (size_t i = 0; i < count; i++) {
        if (strcasecmp(argv[1], table[i].name) == 0) {
            int sub_argc = argc - 1;
            char **sub_argv = &argv[1];
            return table[i].handler(sub_argc, sub_argv);
        }
    }
    
    shell_print_error("cas: unknown sub-command '%s'", argv[1]);
    shell_print_muted("cas open <expr> | close | simplify | expand | factor | diff | integrate | solve | series | substitute | evaluate | latex");
    return 2;
}