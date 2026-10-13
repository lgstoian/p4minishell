/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file math_disp_parse.c
 * @brief Natural Display math expression parser.
 *
 * Parses 1D input string into 2D expression tree.
 * Integrates with existing calc parser for evaluation.
 */

#include "math_disp.h"
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
    math_disp_expr_t *expr;
    int error_pos;
    char error_msg[128];
    math_disp_node_t *last_node;
} math_disp_parser_t;

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
    TOK_SUBSCRIPT,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_COMMA,
    TOK_SQRT,
    TOK_FRAC,
    TOK_FUNCTION,
    TOK_VARIABLE_NAME,
    TOK_EOF_MARKER,
} math_token_type_t;

typedef struct {
    math_token_type_t type;
    union {
        double num;
        char *str;
        int64_t i64;
    } value;
    int start_pos;
    int end_pos;
} math_token_t;

/* ========================================================================
 * Lexer
 * ======================================================================== */

static void parser_error(math_disp_parser_t *p, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->error_msg, sizeof(p->error_msg), fmt, ap);
    va_end(ap);
    p->error_pos = p->pos;
}

static void skip_whitespace(math_disp_parser_t *p)
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

static math_token_t lexer_next(math_disp_parser_t *p)
{
    math_token_t tok = {0};
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
        if (strcasecmp(tok.value.str, "sqrt") == 0) tok.type = TOK_SQRT;
        else if (strcasecmp(tok.value.str, "frac") == 0) tok.type = TOK_FRAC;
        else if (strcasecmp(tok.value.str, "sin") == 0 ||
                 strcasecmp(tok.value.str, "cos") == 0 ||
                 strcasecmp(tok.value.str, "tan") == 0 ||
                 strcasecmp(tok.value.str, "asin") == 0 ||
                 strcasecmp(tok.value.str, "acos") == 0 ||
                 strcasecmp(tok.value.str, "atan") == 0 ||
                 strcasecmp(tok.value.str, "log") == 0 ||
                 strcasecmp(tok.value.str, "ln") == 0 ||
                 strcasecmp(tok.value.str, "exp") == 0 ||
                 strcasecmp(tok.value.str, "abs") == 0 ||
                 strcasecmp(tok.value.str, "sin") == 0 ||
                 strcasecmp(tok.value.str, "cos") == 0 ||
                 strcasecmp(tok.value.str, "tan") == 0) {
            tok.type = TOK_FUNCTION;
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
        case '_': tok.type = TOK_SUBSCRIPT; p->pos++; break;
        case '(': tok.type = TOK_LPAREN; p->pos++; break;
        case ')': tok.type = TOK_RPAREN; p->pos++; break;
        case ',': tok.type = TOK_COMMA; p->pos++; break;
        default:
            p->pos++;
            tok.type = TOK_EOF_MARKER;
            break;
    }
    
    tok.end_pos = p->pos;
    return tok;
}

/* ========================================================================
 * Recursive descent parser
 * ======================================================================== */

typedef struct {
    math_disp_parser_t *parser;
    math_token_t current;
    math_disp_expr_t *expr;
} parser_state_t;

static void advance(parser_state_t *ps)
{
    ps->current = lexer_next(ps->parser);
}

static math_disp_node_t *parse_expression(parser_state_t *ps);
static math_disp_node_t *parse_additive(parser_state_t *ps);
static math_disp_node_t *parse_multiplicative(parser_state_t *ps);
static math_disp_node_t *parse_power(parser_state_t *ps);
static math_disp_node_t *parse_unary(parser_state_t *ps);
static math_disp_node_t *parse_primary(parser_state_t *ps);
static math_disp_node_t *parse_fraction(parser_state_t *ps);
static math_disp_node_t *parse_sqrt(parser_state_t *ps);
static math_disp_node_t *parse_function(parser_state_t *ps);

static math_disp_node_t *parse_expression(parser_state_t *ps)
{
    return parse_additive(ps);
}

static math_disp_node_t *parse_additive(parser_state_t *ps)
{
    math_disp_node_t *left = parse_multiplicative(ps);
    if (!left) return NULL;
    
    while (ps->current.type == TOK_PLUS || ps->current.type == TOK_MINUS) {
        int op = ps->current.type;
        advance(ps);
        math_disp_node_t *right = parse_multiplicative(ps);
        if (!right) {
            parser_error(ps->parser, "Expected expression after '%s'",
                         ps->current.type == TOK_PLUS ? "+" : "-");
            return NULL;
        }
        left = math_disp_node_new_binary(ps->parser->expr,
            op == TOK_PLUS ? MATH_DISP_NODE_ADD : MATH_DISP_NODE_SUB,
            left, right);
        if (!left) return NULL;
    }
    return left;
}

static math_disp_node_t *parse_multiplicative(parser_state_t *ps)
{
    math_disp_node_t *left = parse_power(ps);
    if (!left) return NULL;
    
    while (ps->current.type == TOK_MUL || ps->current.type == TOK_DIV) {
        int op = ps->current.type;
        advance(ps);
        math_disp_node_t *right = parse_power(ps);
        if (!right) {
            parser_error(ps->parser, "Expected expression after '%s'",
                         ps->current.type == TOK_MUL ? "*" : "/");
            return NULL;
        }
        left = math_disp_node_new_binary(ps->parser->expr,
            op == TOK_MUL ? MATH_DISP_NODE_MUL : MATH_DISP_NODE_DIV,
            left, right);
        if (!left) return NULL;
    }
    return left;
}

static math_disp_node_t *parse_power(parser_state_t *ps)
{
    math_disp_node_t *left = parse_unary(ps);
    if (!left) return NULL;
    
    if (ps->current.type == TOK_POW) {
        advance(ps);
        math_disp_node_t *right = parse_power(ps);  /* Right associative */
        if (!right) return NULL;
        left = math_disp_node_new_binary(ps->parser->expr, MATH_DISP_NODE_POW, left, right);
    } else if (ps->current.type == TOK_SUBSCRIPT) {
        advance(ps);
        math_disp_node_t *right = parse_unary(ps);
        if (!right) return NULL;
        left = math_disp_node_new_binary(ps->parser->expr, MATH_DISP_NODE_SUBSCRIPT, left, right);
    }
    return left;
}

static math_disp_node_t *parse_unary(parser_state_t *ps)
{
    if (ps->current.type == TOK_MINUS) {
        advance(ps);
        math_disp_node_t *child = parse_unary(ps);
        if (!child) return NULL;
        return math_disp_node_new_unary(ps->parser->expr, MATH_DISP_NODE_NEGATE, child);
    }
    
    if (ps->current.type == TOK_PLUS) {
        advance(ps);
        return parse_unary(ps);
    }
    
    return parse_primary(ps);
}

static math_disp_node_t *parse_primary(parser_state_t *ps)
{
    switch (ps->current.type) {
        case TOK_NUMBER: {
            math_disp_node_t *node = math_disp_node_new_number(ps->parser->expr, ps->current.value.num);
            advance(ps);
            return node;
        }
        
        case TOK_VARIABLE_NAME: {
            math_disp_node_t *node = math_disp_node_new_variable(ps->parser->expr, ps->current.value.str);
            free(ps->current.value.str);
            advance(ps);
            return node;
        }
        
        case TOK_STRING: {
            math_disp_node_t *node = math_disp_pool_alloc(ps->parser->expr);
            if (!node) return NULL;
            memset(node, 0, sizeof(math_disp_node_t));
            node->type = MATH_DISP_NODE_TEXT;
            node->value.type = MATH_DISP_VAL_STRING;
            node->value.v.str = ps->current.value.str;
            advance(ps);
            return node;
        }
        
        case TOK_FUNCTION:
            return parse_function(ps);
        
        case TOK_LPAREN: {
            advance(ps);
            math_disp_node_t *expr = parse_expression(ps);
            if (ps->current.type != TOK_RPAREN) {
                parser_error(ps->parser, "Expected ')'");
                return NULL;
            }
            advance(ps);
            /* Wrap in paren node. `expr` already holds the parsed operand;
             * indexing the linear `nodes` array by pool_used was both the
             * wrong array and a value where a pointer is required. */
            math_disp_node_t *paren = math_disp_node_new_unary(ps->parser->expr, MATH_DISP_NODE_PAREN, expr);
            return paren;
        }
        
        case TOK_SQRT: {
            return parse_sqrt(ps);
        }
        
        case TOK_FRAC:
            return parse_fraction(ps);
        
        default:
            parser_error(ps->parser, "Unexpected token");
            return NULL;
    }
}

static math_disp_node_t *parse_fraction(parser_state_t *ps)
{
    if (ps->current.type != TOK_FRAC) return NULL;
    advance(ps);
    
    /* Expect { numerator } { denominator } or ( numerator ) / ( denominator ) */
    if (ps->current.type == TOK_LPAREN) {
        advance(ps);
        math_disp_node_t *num = parse_expression(ps);
        if (ps->current.type != TOK_RPAREN) return NULL;
        advance(ps);
        
        if (ps->current.type != TOK_DIV) {
            math_disp_node_t *den = parse_expression(ps);
            if (!den) return NULL;
            
            /* `num` already holds the parsed numerator; the previous
             * nodes[pool_used-2] read the wrong array and yielded a value
             * instead of a pointer. */
            math_disp_node_t *frac = math_disp_node_new_binary(ps->parser->expr, MATH_DISP_NODE_FRAC,
                num, den);
            return frac;
        }
    }
    
    /* Try inline: expr / expr */
    math_disp_node_t *left = parse_expression(ps);
    if (ps->current.type == TOK_DIV) {
        advance(ps);
        math_disp_node_t *right = parse_expression(ps);
        if (!right) return NULL;
        return math_disp_node_new_binary(ps->parser->expr, MATH_DISP_NODE_FRAC, left, right);
    }
    
    return left;
}

static math_disp_node_t *parse_sqrt(parser_state_t *ps)
{
    if (ps->current.type != TOK_SQRT) return NULL;
    advance(ps);
    
    math_disp_node_t *radicand = parse_primary(ps);
    if (!radicand) return NULL;
    
    return math_disp_node_new_unary(ps->parser->expr, MATH_DISP_NODE_SQRT, radicand);
}

static math_disp_node_t *parse_function(parser_state_t *ps)
{
    math_disp_node_type_t type = MATH_DISP_NODE_TEXT;
    const char *name = ps->current.value.str;
    
    if (strcasecmp(name, "sin") == 0) type = MATH_DISP_NODE_SIN;
    else if (strcasecmp(name, "cos") == 0) type = MATH_DISP_NODE_COS;
    else if (strcasecmp(name, "tan") == 0) type = MATH_DISP_NODE_TAN;
    else if (strcasecmp(name, "asin") == 0) type = MATH_DISP_NODE_ASIN;
    else if (strcasecmp(name, "acos") == 0) type = MATH_DISP_NODE_ACOS;
    else if (strcasecmp(name, "atan") == 0) type = MATH_DISP_NODE_ATAN;
    else if (strcasecmp(name, "log") == 0) type = MATH_DISP_NODE_LOG;
    else if (strcasecmp(name, "ln") == 0) type = MATH_DISP_NODE_LN;
    else if (strcasecmp(name, "exp") == 0) type = MATH_DISP_NODE_EXP;
    else if (strcasecmp(name, "abs") == 0) type = MATH_DISP_NODE_ABS;
    else if (strcasecmp(name, "sqrt") == 0) type = MATH_DISP_NODE_SQRT;
    else if (strcasecmp(name, "cbrt") == 0) type = MATH_DISP_NODE_CBRT;
    else if (strcasecmp(name, "ln") == 0) type = MATH_DISP_NODE_LN;
    else if (strcasecmp(name, "log") == 0) type = MATH_DISP_NODE_LOG;
    else if (strcasecmp(name, "sin") == 0) type = MATH_DISP_NODE_SIN;
    else if (strcasecmp(name, "cos") == 0) type = MATH_DISP_NODE_COS;
    else if (strcasecmp(name, "tan") == 0) type = MATH_DISP_NODE_TAN;
    else type = MATH_DISP_NODE_TEXT;
    
    advance(ps);
    
    if (ps->current.type != TOK_LPAREN) {
        free(ps->current.value.str);
        return NULL;
    }
    advance(ps);
    
    math_disp_node_t *arg = parse_expression(ps);
    if (ps->current.type != TOK_RPAREN) {
        if (arg) math_disp_pool_alloc(ps->parser->expr); /* dummy */
        return NULL;
    }
    advance(ps);
    
    free(ps->current.value.str);
    return math_disp_node_new_unary(ps->parser->expr, type, arg);
}

/* ========================================================================
 * Public parse entry points
 * ======================================================================== */

int math_disp_parse(math_disp_expr_t *expr, const char *input)
{
    if (!expr || !input) return -1;
    
    math_disp_expr_clear(expr);
    
    math_disp_parser_t parser = {0};
    parser.input = input;
    parser.len = strlen(input);
    parser.pos = 0;
    parser.expr = expr;
    parser.error_msg[0] = '\0';
    
    math_disp_parser_t *p = &parser;
    math_token_t tok = lexer_next(p);
    
    math_disp_parser_t *p_ptr = p;
    parser_state_t ps = {0};
    ps.parser = p_ptr;
    ps.current = tok;
    ps.expr = expr;
    
    math_disp_node_t *root = parse_expression(&ps);
    
    if (ps.parser->error_msg[0]) {
        expr->error_msg[0] = '\0';
        strlcpy(expr->error_msg, ps.parser->error_msg, sizeof(expr->error_msg));
        return -1;
    }
    
    expr->root = root;
    if (root) {
        expr->cursor.node = root;
        expr->cursor.child_idx = -1;
        expr->cursor.offset = 0;
    }
    
    math_disp_invalidate_layout(ps.parser->expr);
    return 0;
}

int math_disp_parse_at(math_disp_expr_t *expr, const char *input, math_disp_node_t **out_node)
{
    return math_disp_parse(expr, input);
}

/* Parse using calc engine for evaluation */
int math_disp_parse_with_calc(math_disp_expr_t *expr, const char *input)
{
    if (!expr || !input) return -1;
    
    /* First try to evaluate with calc engine */
    calc_value_t calc_result;
    const char *calc_error = NULL;
    
    if (calc_evaluate(input, &calc_result, &calc_error)) {
        /* Successfully parsed by calc - now parse for display */
        return math_disp_parse(expr, input);
    }
    
    /* Fall back to display parser */
    return math_disp_parse(expr, input);
}