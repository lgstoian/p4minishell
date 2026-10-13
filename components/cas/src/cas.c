/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file cas.c
 * @brief Computer Algebra System - Core implementation.
 */

#include "cas.h"
#include "p4minishell_config.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <ctype.h>

/* Forward declarations: the entry points above dispatch to these
 * helpers, which are defined further down the file. */
static cas_node_t *cas_pool_alloc(cas_expr_t *expr);
static double cas_eval_num_internal(const cas_expr_t *expr, const cas_node_t *node);
static void cas_node_to_string_rec(const cas_expr_t *expr, const cas_node_t *node, bool latex, char *buf, size_t *pos, size_t buf_size);
static int64_t cas_gcd(int64_t a, int64_t b);

/* ========================================================================
 * Pool allocation
 * ======================================================================== */

static cas_node_t *cas_pool_alloc(cas_expr_t *expr)
{
    if (expr->pool_used >= expr->max_nodes) {
        return NULL;
    }
    cas_node_t *node = &expr->node_pool[expr->pool_used++];
    memset(node, 0, sizeof(cas_node_t));
    return node;
}

/* ========================================================================
 * Expression lifecycle
 * ======================================================================== */

cas_expr_t *cas_expr_new(void)
{
    cas_expr_t *expr = calloc(1, sizeof(cas_expr_t));
    if (!expr) return NULL;

    expr->max_nodes = CAS_MAX_EXPR_NODES;
    expr->use_rational = true;
    expr->use_complex = true;
    return expr;
}

void cas_expr_free(cas_expr_t *expr)
{
    if (!expr) return;

    for (int i = 0; i < expr->pool_used; i++) {
        cas_node_t *node = &expr->node_pool[i];
        if (node->value.type == CAS_VAL_STRING && node->value.v.str) {
            free(node->value.v.str);
        }
    }
    free(expr);
}

void cas_expr_clear(cas_expr_t *expr)
{
    if (!expr) return;

    for (int i = 0; i < expr->pool_used; i++) {
        cas_node_t *node = &expr->node_pool[i];
        if (node->value.type == CAS_VAL_STRING && node->value.v.str) {
            free(node->value.v.str);
            node->value.v.str = NULL;
        }
    }

    expr->pool_used = 0;
    expr->node_count = 0;
    expr->root = NULL;
    expr->error_msg[0] = '\0';
}

/* ========================================================================
 * Node creation helpers (public API)
 * ======================================================================== */

cas_node_t *cas_node_new_number(cas_expr_t *expr, double val)
{
    cas_node_t *node = cas_pool_alloc(expr);
    if (!node) return NULL;
    node->type = CAS_NODE_NUMBER;
    node->value.type = CAS_VAL_DOUBLE;
    node->value.v.d = val;
    return node;
}

cas_node_t *cas_node_new_int(cas_expr_t *expr, int64_t val)
{
    cas_node_t *node = cas_pool_alloc(expr);
    if (!node) return NULL;
    node->type = CAS_NODE_NUMBER;
    cas_value_init_int(&node->value, val);
    return node;
}

cas_node_t *cas_node_new_rational(cas_expr_t *expr, int64_t num, int64_t den)
{
    cas_node_t *node = cas_pool_alloc(expr);
    if (!node) return NULL;
    node->type = CAS_NODE_RATIONAL;
    node->value.type = CAS_VAL_RATIONAL;
    node->value.v.rat.num = num;
    node->value.v.rat.den = den;
    return node;
}

cas_node_t *cas_node_new_variable(cas_expr_t *expr, const char *name)
{
    cas_node_t *node = cas_pool_alloc(expr);
    if (!node) return NULL;
    node->type = CAS_NODE_VARIABLE;
    node->value.type = CAS_VAL_STRING;
    node->value.v.str = strdup(name);
    return node;
}

cas_node_t *cas_node_new_binary(cas_expr_t *expr, cas_node_type_t type, cas_node_t *left, cas_node_t *right)
{
    cas_node_t *node = cas_pool_alloc(expr);
    if (!node) return NULL;
    node->type = type;
    node->child_count = 2;
    node->children[0] = left;
    node->children[1] = right;
    if (left) left->parent = node;
    if (right) right->parent = node;
    return node;
}

cas_node_t *cas_node_new_unary(cas_expr_t *expr, cas_node_type_t type, cas_node_t *child)
{
    cas_node_t *node = cas_pool_alloc(expr);
    if (!node) return NULL;
    node->type = type;
    node->child_count = 1;
    node->children[0] = child;
    if (child) child->parent = node;
    return node;
}

cas_node_t *cas_node_new_function(cas_expr_t *expr, cas_node_type_t type, cas_node_t *arg)
{
    cas_node_t *node = cas_pool_alloc(expr);
    if (!node) return NULL;
    node->type = type;
    node->child_count = 1;
    node->children[0] = arg;
    if (arg) arg->parent = node;
    return node;
}

cas_node_t *cas_node_new_text(cas_expr_t *expr, const char *text)
{
    cas_node_t *node = cas_pool_alloc(expr);
    if (!node) return NULL;
    node->type = CAS_NODE_TEXT;
    node->value.type = CAS_VAL_STRING;
    node->value.v.str = strdup(text);
    return node;
}

/* ========================================================================
 * Node copy
 * ======================================================================== */

cas_node_t *cas_node_copy(cas_expr_t *expr, const cas_node_t *src)
{
    if (!src) return NULL;

    cas_node_t *dst = cas_pool_alloc(expr);
    if (!dst) return NULL;

    memcpy(dst, src, sizeof(cas_node_t));
    dst->parent = NULL;
    dst->children[0] = dst->children[1] = NULL;

    if (src->value.type == CAS_VAL_STRING && src->value.v.str) {
        dst->value.v.str = strdup(src->value.v.str);
    }

    for (int i = 0; i < src->child_count; i++) {
        dst->children[i] = cas_node_copy(expr, src->children[i]);
        if (dst->children[i]) dst->children[i]->parent = dst;
    }

    return dst;
}

/* ========================================================================
 * Evaluation
 * ======================================================================== */

static double cas_eval_num_internal(const cas_expr_t *expr, const cas_node_t *node)
{
    (void)expr;

    if (!node) return NAN;

    switch (node->type) {
        case CAS_NODE_NUMBER:
            return node->value.v.d;
        case CAS_NODE_RATIONAL:
            return (double)node->value.v.rat.num / node->value.v.rat.den;
        case CAS_NODE_VARIABLE:
            return NAN;  /* Cannot evaluate unbound variable */
        case CAS_NODE_ADD:
            if (node->child_count >= 2) {
                return cas_eval_num_internal(expr, node->children[0]) +
                       cas_eval_num_internal(expr, node->children[1]);
            }
            break;
        case CAS_NODE_SUB:
            if (node->child_count >= 2) {
                return cas_eval_num_internal(expr, node->children[0]) -
                       cas_eval_num_internal(expr, node->children[1]);
            }
            break;
        case CAS_NODE_MUL:
            if (node->child_count >= 2) {
                return cas_eval_num_internal(expr, node->children[0]) *
                       cas_eval_num_internal(expr, node->children[1]);
            }
            break;
        case CAS_NODE_DIV:
            if (node->child_count >= 2) {
                double denom = cas_eval_num_internal(expr, node->children[1]);
                if (denom != 0) return cas_eval_num_internal(expr, node->children[0]) / denom;
            }
            break;
        case CAS_NODE_POW:
            if (node->child_count >= 2) {
                double base = cas_eval_num_internal(expr, node->children[0]);
                double exp = cas_eval_num_internal(expr, node->children[1]);
                return pow(base, exp);
            }
            break;
        case CAS_NODE_NEGATE:
            if (node->child_count >= 1) {
                return -cas_eval_num_internal(expr, node->children[0]);
            }
            break;
        case CAS_NODE_SIN:
            if (node->child_count >= 1) {
                return sin(cas_eval_num_internal(expr, node->children[0]));
            }
            break;
        case CAS_NODE_COS:
            if (node->child_count >= 1) {
                return cos(cas_eval_num_internal(expr, node->children[0]));
            }
            break;
        case CAS_NODE_TAN:
            if (node->child_count >= 1) {
                return tan(cas_eval_num_internal(expr, node->children[0]));
            }
            break;
        case CAS_NODE_EXP:
            if (node->child_count >= 1) {
                return exp(cas_eval_num_internal(expr, node->children[0]));
            }
            break;
        case CAS_NODE_LOG:
        case CAS_NODE_LN:
            if (node->child_count >= 1) {
                double v = cas_eval_num_internal(expr, node->children[0]);
                if (v > 0) return log(v);
            }
            break;
        case CAS_NODE_SQRT:
            if (node->child_count >= 1) {
                double v = cas_eval_num_internal(expr, node->children[0]);
                if (v >= 0) return sqrt(v);
            }
            break;
        default:
            /* Symbolic-only nodes (relations, assignment, unevaluated
             * integrals/derivatives/limits/roots, ...) have no numeric
             * value here; the exact evaluator handles those. */
            break;
    }
    return NAN;
}

bool cas_eval_num(const cas_expr_t *expr, const cas_node_t *node, double *out)
{
    if (!node || !out) return false;

    double val = cas_eval_num_internal(expr, node);
    if (!isfinite(val)) return false;
    *out = val;
    return true;
}

cas_node_t *cas_eval_at(cas_expr_t *expr, cas_node_t *node, const char *var, double val)
{
    if (!node) return NULL;

    cas_node_t *copy = cas_node_copy(expr, node);
    if (!copy) return NULL;

    /* Substitute variable with value */
    cas_node_substitute(copy, var, val);

    /* Evaluate numerically */
    double result;
    if (!cas_eval_num(expr, copy, &result)) return NULL;
    if (!isfinite(result)) return NULL;

    return cas_node_new_number(expr, result);
}

void cas_node_substitute(cas_node_t *node, const char *var, double val)
{
    if (!node) return;

    if (node->type == CAS_NODE_VARIABLE && node->value.v.str &&
        strcmp(node->value.v.str, var) == 0) {
        node->type = CAS_NODE_NUMBER;
        node->value.type = CAS_VAL_DOUBLE;
        node->value.v.d = val;
        if (node->value.v.str) {
            free(node->value.v.str);
            node->value.v.str = NULL;
        }
        return;
    }

    for (int i = 0; i < node->child_count; i++) {
        cas_node_substitute(node->children[i], var, val);
    }
}

/* ========================================================================
 * Serialization
 * ======================================================================== */

static void cas_node_to_string_rec(const cas_expr_t *expr, const cas_node_t *node, bool latex, char *buf, size_t *pos, size_t buf_size)
{
    if (!node || *pos >= buf_size - 1) return;

    switch (node->type) {
        case CAS_NODE_NUMBER: {
            int n = snprintf(buf + *pos, buf_size - *pos, "%g", node->value.v.d);
            if (n > 0) *pos += n;
            break;
        }
        case CAS_NODE_RATIONAL: {
            int n = snprintf(buf + *pos, buf_size - *pos, "%lld/%lld",
                            node->value.v.rat.num, node->value.v.rat.den);
            if (n > 0) *pos += n;
            break;
        }
        case CAS_NODE_VARIABLE: {
            if (node->value.v.str) {
                int n = snprintf(buf + *pos, buf_size - *pos, "%s", node->value.v.str);
                if (n > 0) *pos += n;
            }
            break;
        }
        case CAS_NODE_TEXT: {
            if (node->value.v.str) {
                if (latex) {
                    int n = snprintf(buf + *pos, buf_size - *pos, "\\text{%s}", node->value.v.str);
                    if (n > 0) *pos += n;
                } else {
                    int n = snprintf(buf + *pos, buf_size - *pos, "\"%s\"", node->value.v.str);
                    if (n > 0) *pos += n;
                }
            }
            break;
        }
        case CAS_NODE_NEGATE: {
            if (latex) {
                int n = snprintf(buf + *pos, buf_size - *pos, "-");
                if (n > 0) *pos += n;
            } else {
                int n = snprintf(buf + *pos, buf_size - *pos, "-");
                if (n > 0) *pos += n;
            }
            if (node->child_count >= 1) {
                cas_node_to_string_rec(expr, node->children[0], latex, buf, pos, buf_size);
            }
            break;
        }
        case CAS_NODE_ADD:
        case CAS_NODE_SUB:
        case CAS_NODE_MUL:
        case CAS_NODE_DIV:
        case CAS_NODE_POW: {
            const char *op = "";
            bool need_parens = true;
            switch (node->type) {
                case CAS_NODE_ADD: op = latex ? " + " : " + "; break;
                case CAS_NODE_SUB: op = latex ? " - " : " - "; break;
                case CAS_NODE_MUL: op = latex ? " \\cdot " : " * "; break;
                case CAS_NODE_DIV: op = latex ? " / " : " / "; break;
                case CAS_NODE_POW: op = latex ? "^{" : "^"; need_parens = false; break;
                default:
                    break;   /* only the arithmetic operators reach here */
            }

            if (need_parens && node->child_count >= 1) {
                int n = snprintf(buf + *pos, buf_size - *pos, "(");
                if (n > 0) *pos += n;
            }

            if (node->child_count >= 1) {
                cas_node_to_string_rec(expr, node->children[0], latex, buf, pos, buf_size);
            }

            if (node->type == CAS_NODE_POW) {
                int n = snprintf(buf + *pos, buf_size - *pos, "%s", op);
                if (n > 0) *pos += n;
                if (node->child_count >= 2) {
                    cas_node_to_string_rec(expr, node->children[1], latex, buf, pos, buf_size);
                }
                if (latex) {
                    int n = snprintf(buf + *pos, buf_size - *pos, "}");
                    if (n > 0) *pos += n;
                }
            } else {
                int n = snprintf(buf + *pos, buf_size - *pos, "%s", op);
                if (n > 0) *pos += n;
                if (node->child_count >= 2) {
                    cas_node_to_string_rec(expr, node->children[1], latex, buf, pos, buf_size);
                }
            }

            if (need_parens && node->child_count >= 1) {
                int n = snprintf(buf + *pos, buf_size - *pos, ")");
                if (n > 0) *pos += n;
            }
            break;
        }
        case CAS_NODE_SIN:
        case CAS_NODE_COS:
        case CAS_NODE_TAN:
        case CAS_NODE_ASIN:
        case CAS_NODE_ACOS:
        case CAS_NODE_ATAN:
        case CAS_NODE_SINH:
        case CAS_NODE_COSH:
        case CAS_NODE_TANH:
        case CAS_NODE_LOG:
        case CAS_NODE_LN:
        case CAS_NODE_EXP:
        case CAS_NODE_ABS:
        case CAS_NODE_SQRT: {
            const char *func = "";
            switch (node->type) {
                case CAS_NODE_SIN: func = latex ? "\\sin" : "sin"; break;
                case CAS_NODE_COS: func = latex ? "\\cos" : "cos"; break;
                case CAS_NODE_TAN: func = latex ? "\\tan" : "tan"; break;
                case CAS_NODE_ASIN: func = latex ? "\\arcsin" : "asin"; break;
                case CAS_NODE_ACOS: func = latex ? "\\arccos" : "acos"; break;
                case CAS_NODE_ATAN: func = latex ? "\\arctan" : "atan"; break;
                case CAS_NODE_SINH: func = latex ? "\\sinh" : "sinh"; break;
                case CAS_NODE_COSH: func = latex ? "\\cosh" : "cosh"; break;
                case CAS_NODE_TANH: func = latex ? "\\tanh" : "tanh"; break;
                case CAS_NODE_LOG: func = latex ? "\\log" : "log"; break;
                case CAS_NODE_LN: func = latex ? "\\ln" : "ln"; break;
                case CAS_NODE_EXP: func = latex ? "\\exp" : "exp"; break;
                case CAS_NODE_ABS: func = latex ? "|" : "abs"; break;
                case CAS_NODE_SQRT: func = latex ? "\\sqrt" : "sqrt"; break;
                default:
                    break;
            }

            int n = snprintf(buf + *pos, buf_size - *pos, "%s", func);
            if (n > 0) *pos += n;

            if (node->child_count >= 1) {
                if (latex && node->type == CAS_NODE_SQRT) {
                    n = snprintf(buf + *pos, buf_size - *pos, "{");
                    if (n > 0) *pos += n;
                } else {
                    n = snprintf(buf + *pos, buf_size - *pos, "(");
                    if (n > 0) *pos += n;
                }
                cas_node_to_string_rec(expr, node->children[0], latex, buf, pos, buf_size);
                if (latex && node->type == CAS_NODE_SQRT) {
                    n = snprintf(buf + *pos, buf_size - *pos, "}");
                    if (n > 0) *pos += n;
                } else if (latex && node->type == CAS_NODE_ABS) {
                    n = snprintf(buf + *pos, buf_size - *pos, "|");
                    if (n > 0) *pos += n;
                } else {
                    n = snprintf(buf + *pos, buf_size - *pos, ")");
                    if (n > 0) *pos += n;
                }
            }
            break;
        }
        default: {
            int n = snprintf(buf + *pos, buf_size - *pos, "?");
            if (n > 0) *pos += n;
            break;
        }
    }
}

char *cas_to_string(const cas_expr_t *expr, cas_node_t *node, bool latex)
{
    if (!expr || !node) return NULL;

    char *buf = malloc(1024);
    if (!buf) return NULL;

    size_t pos = 0;
    cas_node_to_string_rec(expr, node, latex, buf, &pos, 1024);
    buf[pos] = '\0';

    return buf;
}

int cas_from_string(cas_expr_t *expr, const char *str)
{
    (void)expr; (void)str;
    return -1;  /* Not implemented */
}

/* ========================================================================
 * Utilities
 * ======================================================================== */

const char *cas_node_type_str(int type)
{
    switch (type) {
        case CAS_NODE_NUMBER: return "number";
        case CAS_NODE_VARIABLE: return "variable";
        case CAS_NODE_SYMBOL: return "symbol";
        case CAS_NODE_RATIONAL: return "rational";
        case CAS_NODE_NEGATE: return "negate";
        case CAS_NODE_SQRT: return "sqrt";
        case CAS_NODE_SIN: return "sin";
        case CAS_NODE_COS: return "cos";
        case CAS_NODE_TAN: return "tan";
        case CAS_NODE_ASIN: return "asin";
        case CAS_NODE_ACOS: return "acos";
        case CAS_NODE_ATAN: return "atan";
        case CAS_NODE_SINH: return "sinh";
        case CAS_NODE_COSH: return "cosh";
        case CAS_NODE_TANH: return "tanh";
        case CAS_NODE_LOG: return "log";
        case CAS_NODE_LN: return "ln";
        case CAS_NODE_EXP: return "exp";
        case CAS_NODE_ABS: return "abs";
        case CAS_NODE_FACTORIAL: return "factorial";
        case CAS_NODE_ADD: return "add";
        case CAS_NODE_SUB: return "sub";
        case CAS_NODE_MUL: return "mul";
        case CAS_NODE_DIV: return "div";
        case CAS_NODE_POW: return "pow";
        case CAS_NODE_FRAC: return "frac";
        case CAS_NODE_ROOT: return "root";
        case CAS_NODE_DERIVATIVE: return "derivative";
        case CAS_NODE_INTEGRAL: return "integral";
        case CAS_NODE_LIMIT: return "limit";
        case CAS_NODE_SUM: return "sum";
        case CAS_NODE_PRODUCT: return "product";
        case CAS_NODE_SERIES: return "series";
        case CAS_NODE_EQ: return "eq";
        case CAS_NODE_NEQ: return "neq";
        case CAS_NODE_LT: return "lt";
        case CAS_NODE_LE: return "le";
        case CAS_NODE_GT: return "gt";
        case CAS_NODE_GE: return "ge";
        case CAS_NODE_SEQUENCE: return "sequence";
        case CAS_NODE_TEXT: return "text";
        case CAS_NODE_ASSIGNMENT: return "assignment";
        default: return "unknown";
    }
}

bool cas_node_is_leaf(const cas_node_t *node)
{
    if (!node) return false;
    return node->child_count == 0;
}

bool cas_node_is_binary(const cas_node_t *node)
{
    if (!node) return false;
    return node->child_count == 2;
}

bool cas_node_is_unary(const cas_node_t *node)
{
    if (!node) return false;
    return node->child_count == 1;
}

int cas_node_child_count(const cas_node_t *node)
{
    if (!node) return 0;
    return node->child_count;
}

cas_node_t *cas_find_node_at(cas_expr_t *expr, int x, int y)
{
    (void)expr; (void)x; (void)y;
    return NULL;  /* Not implemented */
}

cas_node_t *cas_get_node_at_cursor(const cas_expr_t *expr)
{
    if (!expr) return NULL;
    return expr->root;  /* Simplified - return root */
}

/* Value handling */
void cas_value_init_int(cas_value_t *v, int64_t val)
{
    v->type = CAS_VAL_INT64;
    v->v.i64 = val;
}

void cas_value_init_double(cas_value_t *v, double val)
{
    v->type = CAS_VAL_DOUBLE;
    v->v.d = val;
}

void cas_value_init_rational(cas_value_t *v, int64_t num, int64_t den)
{
    v->type = CAS_VAL_RATIONAL;
    v->v.rat.num = num;
    v->v.rat.den = den;
}

void cas_value_init_complex(cas_value_t *v, double re, double im)
{
    v->type = CAS_VAL_COMPLEX;
    v->v.complex.re = re;
    v->v.complex.im = im;
}

void cas_value_init_string(cas_value_t *v, const char *str)
{
    v->type = CAS_VAL_STRING;
    v->v.str = strdup(str);
}

void cas_value_free(cas_value_t *v)
{
    if (!v) return;
    if (v->type == CAS_VAL_STRING && v->v.str) {
        free(v->v.str);
        v->v.str = NULL;
    }
    v->type = CAS_VAL_NONE;
}

char *cas_value_to_string(const cas_value_t *v, char *buf, size_t buf_size)
{
    if (!v || !buf || buf_size == 0) return NULL;

    switch (v->type) {
        case CAS_VAL_INT64:
            snprintf(buf, buf_size, "%lld", v->v.i64);
            break;
        case CAS_VAL_DOUBLE:
            snprintf(buf, buf_size, "%g", v->v.d);
            break;
        case CAS_VAL_RATIONAL:
            snprintf(buf, buf_size, "%lld/%lld", v->v.rat.num, v->v.rat.den);
            break;
        case CAS_VAL_COMPLEX:
            snprintf(buf, buf_size, "%g+%gi", v->v.complex.re, v->v.complex.im);
            break;
        case CAS_VAL_STRING:
            if (v->v.str) {
                snprintf(buf, buf_size, "%s", v->v.str);
            } else {
                buf[0] = '\0';
            }
            break;
        default:
            buf[0] = '\0';
            break;
    }
    return buf;
}

/* Rational arithmetic */
static int64_t cas_gcd(int64_t a, int64_t b)
{
    a = a < 0 ? -a : a;
    b = b < 0 ? -b : b;
    while (b != 0) {
        int64_t t = b;
        b = a % b;
        a = t;
    }
    return a;
}

void cas_rational_reduce(int64_t *num, int64_t *den)
{
    if (!num || !den || *den == 0) return;
    int64_t g = cas_gcd(*num, *den);
    if (g > 1) {
        *num /= g;
        *den /= g;
    }
    if (*den < 0) {
        *num = -*num;
        *den = -*den;
    }
}

void cas_rational_add(int64_t n1, int64_t d1, int64_t n2, int64_t d2, int64_t *rn, int64_t *rd)
{
    *rn = n1 * d2 + n2 * d1;
    *rd = d1 * d2;
    cas_rational_reduce(rn, rd);
}

void cas_rational_sub(int64_t n1, int64_t d1, int64_t n2, int64_t d2, int64_t *rn, int64_t *rd)
{
    *rn = n1 * d2 - n2 * d1;
    *rd = d1 * d2;
    cas_rational_reduce(rn, rd);
}

void cas_rational_mul(int64_t n1, int64_t d1, int64_t n2, int64_t d2, int64_t *rn, int64_t *rd)
{
    *rn = n1 * n2;
    *rd = d1 * d2;
    cas_rational_reduce(rn, rd);
}

void cas_rational_div(int64_t n1, int64_t d1, int64_t n2, int64_t d2, int64_t *rn, int64_t *rd)
{
    if (n2 == 0) {
        *rn = 0;
        *rd = 0;
        return;
    }
    *rn = n1 * d2;
    *rd = d1 * n2;
    cas_rational_reduce(rn, rd);
}