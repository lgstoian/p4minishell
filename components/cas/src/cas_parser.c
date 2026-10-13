/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file cas_parser.c
 * @brief CAS expression parser - recursive descent parser for math expressions.
 */

#include "cas.h"
#include "p4minishell_config.h"
#include "calc.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>
#include <stdarg.h>



/* ========================================================================
 * Parser context
 * ======================================================================== */

typedef struct {
    const char *input;
    int pos;
    int len;
    cas_expr_t *expr;
    int error_pos;
    char error_msg[128];
} cas_parser_t;

/* ========================================================================
 * Token types
 * ======================================================================== */

typedef enum {
    TOK_EOF,
    TOK_NUMBER,
    TOK_VARIABLE,
    TOK_SYMBOL,
    TOK_STRING,
    TOK_PLUS,
    TOK_MINUS,
    TOK_MUL,
    TOK_DIV,
    TOK_POW,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_COMMA,
    TOK_FUNCTION,
    TOK_VARIABLE_NAME,
    TOK_SQRT,
    TOK_FRAC,
} cas_token_type_t;

typedef struct {
    cas_token_type_t type;
    union {
        double num;
        char *str;
        int64_t i64;
    } value;
    int start_pos;
    int end_pos;
} cas_token_t;

/* ========================================================================
 * Lexer
 * ======================================================================== */

static void parser_error(cas_parser_t *p, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->error_msg, sizeof(p->error_msg), fmt, ap);
    va_end(ap);
    p->error_pos = p->pos;
}

static void skip_whitespace(cas_parser_t *p)
{
    while (p->pos < p->len && isspace((unsigned char)p->input[p->pos])) {
        p->pos++;
    }
}

static bool is_ident_start(char c)
{
    return isalpha((unsigned char)c) || c == '_';
}

static bool is_ident_char(char c)
{
    return isalnum((unsigned char)c) || c == '_';
}

static cas_token_t lexer_next(cas_parser_t *p)
{
    cas_token_t tok = {0};
    skip_whitespace(p);
    tok.start_pos = p->pos;
    
    if (p->pos >= p->len) {
        tok.type = TOK_EOF;
        return tok;
    }
    
    char c = p->input[p->pos];
    
    /* Numbers */
    if (isdigit((unsigned char)c) || (c == '.' && p->pos + 1 < p->len && isdigit((unsigned char)p->input[p->pos + 1]))) {
        tok.type = TOK_NUMBER;
        char *end;
        tok.value.num = strtod(p->input + p->pos, &end);
        tok.end_pos = p->pos + (end - (p->input + p->pos));
        p->pos = tok.end_pos;
        return tok;
    }
    
    /* Hex numbers */
    if (c == '&' && p->pos + 1 < p->len && (p->input[p->pos + 1] == 'H' || p->input[p->pos + 1] == 'h')) {
        p->pos += 2;
        char *end;
        tok.value.i64 = strtoll(p->input + p->pos, &end, 16);
        tok.type = TOK_NUMBER;
        tok.value.num = (double)tok.value.i64;
        tok.end_pos = p->pos + (end - (p->input + p->pos));
        p->pos = tok.end_pos;
        return tok;
    }
    
    if (c == '0' && p->pos + 1 < p->len && (p->input[p->pos + 1] == 'x' || p->input[p->pos + 1] == 'X')) {
        p->pos += 2;
        char *end;
        tok.value.i64 = strtoll(p->input + p->pos, &end, 16);
        tok.type = TOK_NUMBER;
        tok.value.num = (double)tok.value.i64;
        tok.end_pos = p->pos + (end - (p->input + p->pos));
        p->pos = tok.end_pos;
        return tok;
    }
    
    /* Strings */
    if (c == '"' || c == '\'') {
        char quote = c;
        p->pos++;
        int start = p->pos;
        while (p->pos < p->len && p->input[p->pos] != quote) {
            if (p->input[p->pos] == '\\' && p->pos + 1 < p->len) {
                p->pos += 2;
            } else {
                p->pos++;
            }
        }
        int len = p->pos - start;
        tok.value.str = malloc(len + 1);
        memcpy(tok.value.str, p->input + start, len);
        tok.value.str[len] = '\0';
        tok.type = TOK_STRING;
        if (p->pos < p->len) p->pos++;  /* skip closing quote */
        tok.end_pos = p->pos;
        return tok;
    }
    
    /* Identifiers / functions */
    if (is_ident_start(c)) {
        int start = p->pos;
        while (p->pos < p->len && is_ident_char(p->input[p->pos])) {
            p->pos++;
        }
        int len = p->pos - start;
        tok.value.str = malloc(len + 1);
        memcpy(tok.value.str, p->input + start, len);
        tok.value.str[len] = '\0';
        
        /* Check for keywords */
        if (strcasecmp(tok.value.str, "sin") == 0 || 
            strcasecmp(tok.value.str, "cos") == 0 ||
            strcasecmp(tok.value.str, "tan") == 0 ||
            strcasecmp(tok.value.str, "asin") == 0 ||
            strcasecmp(tok.value.str, "acos") == 0 ||
            strcasecmp(tok.value.str, "atan") == 0 ||
            strcasecmp(tok.value.str, "sinh") == 0 ||
            strcasecmp(tok.value.str, "cosh") == 0 ||
            strcasecmp(tok.value.str, "tanh") == 0 ||
            strcasecmp(tok.value.str, "log") == 0 ||
            strcasecmp(tok.value.str, "ln") == 0 ||
            strcasecmp(tok.value.str, "exp") == 0 ||
            strcasecmp(tok.value.str, "abs") == 0 ||
            strcasecmp(tok.value.str, "sqrt") == 0) {
            tok.type = TOK_FUNCTION;
        } else if (strcasecmp(tok.value.str, "pi") == 0 || 
                   strcasecmp(tok.value.str, "e") == 0) {
            tok.type = TOK_SYMBOL;
        } else {
            tok.type = TOK_VARIABLE_NAME;
        }
        tok.end_pos = p->pos;
        return tok;
    }
    
    /* Operators and punctuation */
    switch (c) {
        case '+': tok.type = TOK_PLUS; p->pos++; break;
        case '-': tok.type = TOK_MINUS; p->pos++; break;
        case '*': tok.type = TOK_MUL; p->pos++; break;
        case '/': tok.type = TOK_DIV; p->pos++; break;
        case '^': tok.type = TOK_POW; p->pos++; break;
        case '(': tok.type = TOK_LPAREN; p->pos++; break;
        case ')': tok.type = TOK_RPAREN; p->pos++; break;
        case ',': tok.type = TOK_COMMA; p->pos++; break;
        default:
            p->pos++;
            tok.type = TOK_EOF;
            break;
    }
    
    tok.end_pos = p->pos;
    return tok;
}

/* ========================================================================
 * Parser state
 * ======================================================================== */

typedef struct {
    cas_parser_t *parser;
    cas_token_t current;
    cas_expr_t *expr;
} parser_state_t;


static void advance(parser_state_t *ps)
{
    ps->current = lexer_next(ps->parser);
}

/* Forward declarations */
static cas_node_t *parse_expression(parser_state_t *ps);
static cas_node_t *parse_additive(parser_state_t *ps);
static cas_node_t *parse_multiplicative(parser_state_t *ps);
static cas_node_t *parse_power(parser_state_t *ps);
static cas_node_t *parse_unary(parser_state_t *ps);
static cas_node_t *parse_primary(parser_state_t *ps);
static cas_node_t *parse_function_call(parser_state_t *ps, const char *name);
static cas_node_t *parse_fraction(parser_state_t *ps);

static cas_node_t *parse_expression(parser_state_t *ps)
{
    return parse_additive(ps);
}

cas_node_t *parse_additive(parser_state_t *ps)
{
    cas_node_t *left = parse_multiplicative(ps);
    if (!left) return NULL;
    
    while (ps->current.type == TOK_PLUS || ps->current.type == TOK_MINUS) {
        int op = ps->current.type;
        advance(ps);
        cas_node_t *right = parse_multiplicative(ps);
        if (!right) return NULL;
        
        left = cas_node_new_binary(ps->parser->expr,
            op == TOK_PLUS ? CAS_NODE_ADD : CAS_NODE_SUB,
            left, right);
        if (!left) return NULL;
    }
    return left;
}

cas_node_t *parse_multiplicative(parser_state_t *ps)
{
    cas_node_t *left = parse_power(ps);
    if (!left) return NULL;
    
    while (ps->current.type == TOK_MUL || ps->current.type == TOK_DIV) {
        int op = ps->current.type;
        advance(ps);
        cas_node_t *right = parse_power(ps);
        if (!right) return NULL;
        
        left = cas_node_new_binary(ps->parser->expr,
            op == TOK_MUL ? CAS_NODE_MUL : CAS_NODE_DIV,
            left, right);
        if (!left) return NULL;
    }
    return left;
}

cas_node_t *parse_power(parser_state_t *ps)
{
    cas_node_t *left = parse_unary(ps);
    if (!left) return NULL;
    
    if (ps->current.type == TOK_POW) {
        advance(ps);
        cas_node_t *right = parse_power(ps);  /* Right associative */
        if (!right) return NULL;
        left = cas_node_new_binary(ps->parser->expr, CAS_NODE_POW, left, right);
    }
    return left;
}

cas_node_t *parse_unary(parser_state_t *ps)
{
    if (ps->current.type == TOK_MINUS) {
        advance(ps);
        cas_node_t *child = parse_unary(ps);
        if (!child) return NULL;
        return cas_node_new_unary(ps->parser->expr, CAS_NODE_NEGATE, child);
    }
    
    if (ps->current.type == TOK_PLUS) {
        advance(ps);
        return parse_unary(ps);
    }
    
    return parse_primary(ps);
}

cas_node_t *parse_primary(parser_state_t *ps)
{
    switch (ps->current.type) {
        case TOK_NUMBER: {
            cas_node_t *node = cas_node_new_number(ps->parser->expr, ps->current.value.num);
            advance(ps);
            return node;
        }
        
        case TOK_VARIABLE_NAME: {
            cas_node_t *node = cas_node_new_variable(ps->parser->expr, ps->current.value.str);
            free(ps->current.value.str);
            advance(ps);
            return node;
        }
        
        case TOK_SYMBOL: {
            cas_node_t *node = cas_node_new_variable(ps->parser->expr, ps->current.value.str);
            free(ps->current.value.str);
            advance(ps);
            return node;
        }
        
        case TOK_STRING: {
            cas_node_t *node = cas_node_new_text(ps->parser->expr, ps->current.value.str);
            free(ps->current.value.str);
            advance(ps);
            return node;
        }
        
        case TOK_FUNCTION:
            return parse_function_call(ps, ps->current.value.str);
        
        case TOK_LPAREN: {
            advance(ps);
            cas_node_t *expr = parse_expression(ps);
            if (ps->current.type != TOK_RPAREN) {
                parser_error(ps->parser, "Expected ')'");
                return NULL;
            }
            advance(ps);
            return expr;
        }
        
        case TOK_SQRT: {
            return parse_function_call(ps, "sqrt");
        }
        
        case TOK_FRAC:
            return parse_fraction(ps);
        
        default:
            return NULL;
    }
}

cas_node_t *parse_function_call(parser_state_t *ps, const char *name)
{
    cas_node_type_t type = CAS_NODE_TEXT;
    
    if (strcasecmp(name, "sin") == 0) type = CAS_NODE_SIN;
    else if (strcasecmp(name, "cos") == 0) type = CAS_NODE_COS;
    else if (strcasecmp(name, "tan") == 0) type = CAS_NODE_TAN;
    else if (strcasecmp(name, "asin") == 0) type = CAS_NODE_ASIN;
    else if (strcasecmp(name, "acos") == 0) type = CAS_NODE_ACOS;
    else if (strcasecmp(name, "atan") == 0) type = CAS_NODE_ATAN;
    else if (strcasecmp(name, "sinh") == 0) type = CAS_NODE_SINH;
    else if (strcasecmp(name, "cosh") == 0) type = CAS_NODE_COSH;
    else if (strcasecmp(name, "tanh") == 0) type = CAS_NODE_TANH;
    else if (strcasecmp(name, "log") == 0) type = CAS_NODE_LOG;
    else if (strcasecmp(name, "ln") == 0) type = CAS_NODE_LN;
    else if (strcasecmp(name, "exp") == 0) type = CAS_NODE_EXP;
    else if (strcasecmp(name, "abs") == 0) type = CAS_NODE_ABS;
    else if (strcasecmp(name, "sqrt") == 0) type = CAS_NODE_SQRT;
    /* cbrt is the cube root. The node enum has no dedicated cube-root case;
     * it maps onto the nth-root node rather than inventing a parallel type
     * that every switch would then have to handle. */
    else if (strcasecmp(name, "cbrt") == 0) type = CAS_NODE_ROOT;
    else type = CAS_NODE_TEXT;
    
    advance(ps);
    
    if (ps->current.type != TOK_LPAREN) {
        free(ps->current.value.str);
        return NULL;
    }
    advance(ps);
    
    cas_node_t *arg = parse_expression(ps);
    if (!arg) {
        free(ps->current.value.str);
        return NULL;
    }
    
    if (ps->current.type != TOK_RPAREN) {
        free(ps->current.value.str);
        return NULL;
    }
    advance(ps);
    
    free(ps->current.value.str);
    return cas_node_new_unary(ps->parser->expr, type, arg);
}

cas_node_t *parse_fraction(parser_state_t *ps)
{
    if (ps->current.type != TOK_FRAC) return NULL;
    advance(ps);
    
    if (ps->current.type != TOK_LPAREN) return NULL;
    advance(ps);
    
    cas_node_t *num = parse_expression(ps);
    if (!num) return NULL;
    
    if (ps->current.type != TOK_COMMA) return NULL;
    advance(ps);
    
    cas_node_t *den = parse_expression(ps);
    if (!den) return NULL;
    
    if (ps->current.type != TOK_RPAREN) return NULL;
    advance(ps);
    
    return cas_node_new_binary(ps->parser->expr, CAS_NODE_FRAC, num, den);
}

/* ========================================================================
 * Public parse entry points
 * ======================================================================== */

int cas_parse(cas_expr_t *expr, const char *input, char **error_out)
{
    if (!expr || !input) return -1;
    
    cas_expr_clear(expr);
    
    cas_parser_t parser = {0};
    parser.input = input;
    parser.len = strlen(input);
    parser.pos = 0;
    parser.expr = expr;
    parser.error_msg[0] = '\0';
    
    cas_parser_t *p = &parser;
    cas_token_t tok = lexer_next(p);
    
    parser_state_t ps = {0};
    ps.parser = p;
    ps.current = tok;
    ps.expr = expr;
    
    cas_node_t *root = parse_expression(&ps);
    
    if (parser.error_msg[0]) {
        expr->error_msg[0] = '\0';
        strlcpy(expr->error_msg, parser.error_msg, sizeof(expr->error_msg));
        return -1;
    }
    
    if (ps.current.type != TOK_EOF) {
        if (error_out) *error_out = "Trailing characters";
        return -1;
    }
    
    expr->root = root;
    if (root) {
        /* The interactive cursor lives on cas_node_t (see its `cursor`
         * member), not on the expression; reset the root's cursor here. */
        root->cursor.has_cursor = false;
        root->cursor.cursor_child = -1;
        root->cursor.cursor_pos = 0;
    }

    return 0;
}

int cas_parse_at(cas_expr_t *expr, const char *input, cas_node_t **out_node)
{
    return cas_parse(expr, input, NULL);
}

/* Parse using calc engine for evaluation capability */
int cas_parse_with_calc(cas_expr_t *expr, const char *input)
{
    if (!expr || !input) return -1;
    
    /* First try to evaluate with calc engine */
    calc_value_t calc_result;
    const char *calc_error = NULL;
    
    if (calc_evaluate(input, &calc_result, &calc_error)) {
        /* Successfully parsed by calc - now parse for display */
        return cas_parse(expr, input, NULL);
    }
    
    /* Fall back to display parser */
    return cas_parse(expr, input, NULL);
}