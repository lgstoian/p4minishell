/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file cas_simplify.c
 * @brief CAS expression simplification.
 */

#include "cas.h"
#include "p4minishell_config.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* Forward declarations: the entry points above dispatch to these
 * helpers, which are defined further down the file. */
/* ========================================================================
 * Simplification entry point
 * ======================================================================== */
cas_node_t *cas_simplify(cas_expr_t *expr, cas_node_t *node);
static cas_node_t *cas_simplify_add(cas_expr_t *expr, cas_node_t *node);
static cas_node_t *cas_simplify_sub(cas_expr_t *expr, cas_node_t *node);
static cas_node_t *cas_simplify_mul(cas_expr_t *expr, cas_node_t *node);
static cas_node_t *cas_simplify_div(cas_expr_t *expr, cas_node_t *node);
static cas_node_t *cas_simplify_pow(cas_expr_t *expr, cas_node_t *node);
static cas_node_t *cas_simplify_negate(cas_expr_t *expr, cas_node_t *node);
static cas_node_t *cas_simplify_function(cas_expr_t *expr, cas_node_t *node);

/* Node predicates, defined at the end of this file. */
bool cas_node_is_zero(const cas_node_t *node);
bool cas_node_is_one(const cas_node_t *node);
bool cas_node_is_two(const cas_node_t *node);
bool cas_node_is_constant(const cas_node_t *node);
bool cas_node_is_same_variable(const cas_node_t *a, const cas_node_t *b);
bool cas_nodes_equal(const cas_node_t *a, const cas_node_t *b);
cas_node_t *cas_expand_mul(cas_expr_t *expr, cas_node_t *node);
cas_node_t *cas_expand_pow(cas_expr_t *expr, cas_node_t *node);


/* ========================================================================
 * Simplification entry point
 * ======================================================================== */

cas_node_t *cas_simplify(cas_expr_t *expr, cas_node_t *node)
{
    if (!node) return NULL;

    /* First simplify children */
    for (int i = 0; i < node->child_count; i++) {
        node->children[i] = cas_simplify(expr, node->children[i]);
    }

    /* Then apply simplification rules based on node type */
    switch (node->type) {
        case CAS_NODE_ADD:
            return cas_simplify_add(expr, node);
        case CAS_NODE_SUB:
            return cas_simplify_sub(expr, node);
        case CAS_NODE_MUL:
            return cas_simplify_mul(expr, node);
        case CAS_NODE_DIV:
            return cas_simplify_div(expr, node);
        case CAS_NODE_POW:
            return cas_simplify_pow(expr, node);
        case CAS_NODE_NEGATE:
            return cas_simplify_negate(expr, node);
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
        case CAS_NODE_SQRT:
            return cas_simplify_function(expr, node);
        default:
            return node;
    }
}

/* ========================================================================
 * Addition simplification
 * ======================================================================== */

static cas_node_t *cas_simplify_add(cas_expr_t *expr, cas_node_t *node)
{
    if (node->child_count != 2) return node;

    cas_node_t *left = node->children[0];
    cas_node_t *right = node->children[1];

    /* 0 + x = x */
    if (cas_node_is_zero(left)) return right;
    if (cas_node_is_zero(right)) return left;

    /* Combine numeric constants */
    if (cas_node_is_constant(left) && cas_node_is_constant(right)) {
        double lval, rval;
        if (cas_eval_num(expr, left, &lval) && cas_eval_num(expr, right, &rval)) {
            double result = lval + rval;
            return cas_node_new_number(expr, result);
        }
    }

    /* Combine like terms: a*x + b*x = (a+b)*x */
    if (cas_node_is_same_variable(left, right)) {
        return node;
    }

    /* Reorder for canonical form: constants last */
    if (cas_node_is_constant(left) && !cas_node_is_constant(right)) {
        cas_node_t *temp = node->children[0];
        node->children[0] = node->children[1];
        node->children[1] = temp;
    }

    return node;
}

static cas_node_t *cas_simplify_sub(cas_expr_t *expr, cas_node_t *node)
{
    if (node->child_count != 2) return node;

    cas_node_t *left = node->children[0];
    cas_node_t *right = node->children[1];

    /* x - 0 = x */
    if (cas_node_is_zero(right)) return left;

    /* x - x = 0 */
    if (cas_nodes_equal(left, right)) {
        return cas_node_new_number(expr, 0);
    }

    /* Combine constants */
    if (cas_node_is_constant(left) && cas_node_is_constant(right)) {
        double lval, rval;
        if (cas_eval_num(expr, left, &lval) && cas_eval_num(expr, right, &rval)) {
            double result = lval - rval;
            return cas_node_new_number(expr, result);
        }
    }

    return node;
}

static cas_node_t *cas_simplify_mul(cas_expr_t *expr, cas_node_t *node)
{
    if (node->child_count != 2) return node;

    cas_node_t *left = node->children[0];
    cas_node_t *right = node->children[1];

    /* 0 * x = 0 */
    if (cas_node_is_zero(left) || cas_node_is_zero(right)) {
        return cas_node_new_number(expr, 0);
    }

    /* 1 * x = x */
    if (cas_node_is_one(left)) return right;
    if (cas_node_is_one(right)) return left;

    /* Combine constants */
    if (cas_node_is_constant(left) && cas_node_is_constant(right)) {
        double lval, rval;
        if (cas_eval_num(expr, left, &lval) && cas_eval_num(expr, right, &rval)) {
            double result = lval * rval;
            return cas_node_new_number(expr, result);
        }
    }

    /* x * x = x^2 */
    if (cas_nodes_equal(left, right)) {
        cas_node_t *base = left;
        cas_node_t *exp = cas_node_new_number(expr, 2);
        return cas_node_new_binary(expr, CAS_NODE_POW, base, exp);
    }

    /* Reorder for canonical form: constants first */
    if (!cas_node_is_constant(left) && cas_node_is_constant(right)) {
        cas_node_t *temp = node->children[0];
        node->children[0] = node->children[1];
        node->children[1] = temp;
    }

    return node;
}

static cas_node_t *cas_simplify_div(cas_expr_t *expr, cas_node_t *node)
{
    if (node->child_count != 2) return node;

    cas_node_t *left = node->children[0];
    cas_node_t *right = node->children[1];

    /* x / 1 = x */
    if (cas_node_is_one(right)) return left;

    /* x / x = 1 */
    if (cas_nodes_equal(left, right)) {
        return cas_node_new_number(expr, 1);
    }

    /* 0 / x = 0 */
    if (cas_node_is_zero(left)) return cas_node_new_number(expr, 0);

    /* Combine constants */
    if (cas_node_is_constant(left) && cas_node_is_constant(right)) {
        double lval, rval;
        if (cas_eval_num(expr, left, &lval) && cas_eval_num(expr, right, &rval)) {
            if (rval != 0) {
                double result = lval / rval;
                return cas_node_new_number(expr, result);
            }
        }
    }

    return node;
}

static cas_node_t *cas_simplify_pow(cas_expr_t *expr, cas_node_t *node)
{
    if (node->child_count != 2) return node;

    cas_node_t *base = node->children[0];
    cas_node_t *exp = node->children[1];

    /* x^0 = 1 */
    if (cas_node_is_zero(exp)) {
        return cas_node_new_number(expr, 1);
    }

    /* x^1 = x */
    if (cas_node_is_one(exp)) {
        return base;
    }

    /* 0^x = 0 (for x > 0) */
    if (cas_node_is_zero(base)) {
        return cas_node_new_number(expr, 0);
    }

    /* 1^x = 1 */
    if (cas_node_is_one(base)) {
        return cas_node_new_number(expr, 1);
    }

    /* (x^a)^b = x^(a*b) */
    if (base->type == CAS_NODE_POW) {
        cas_node_t *new_exp = cas_node_new_binary(expr, CAS_NODE_MUL, base->children[1], exp);
        return cas_node_new_binary(expr, CAS_NODE_POW, base->children[0], new_exp);
    }

    return node;
}

static cas_node_t *cas_simplify_negate(cas_expr_t *expr, cas_node_t *node)
{
    if (node->child_count != 1) return node;

    cas_node_t *child = node->children[0];

    /* -(-x) = x */
    if (child->type == CAS_NODE_NEGATE) {
        return child->children[0];
    }

    /* Combine with constant */
    if (cas_node_is_constant(child)) {
        double cval;
        if (cas_eval_num(expr, child, &cval)) {
            double result = -cval;
            return cas_node_new_number(expr, result);
        }
    }

    return node;
}

static cas_node_t *cas_simplify_function(cas_expr_t *expr, cas_node_t *node)
{
    if (node->child_count != 1) return node;

    cas_node_t *arg = node->children[0];

    /* Simplify argument first */
    node->children[0] = cas_simplify(expr, arg);
    arg = node->children[0];

    /* Trigonometric identities */
    if (node->type == CAS_NODE_SIN && cas_node_is_zero(arg)) {
        return cas_node_new_number(expr, 0);
    }
    if (node->type == CAS_NODE_COS && cas_node_is_zero(arg)) {
        return cas_node_new_number(expr, 1);
    }
    if (node->type == CAS_NODE_TAN && cas_node_is_zero(arg)) {
        return cas_node_new_number(expr, 0);
    }
    if (node->type == CAS_NODE_ASIN && cas_node_is_zero(arg)) {
        return cas_node_new_number(expr, 0);
    }
    if (node->type == CAS_NODE_ACOS && cas_node_is_one(arg)) {
        return cas_node_new_number(expr, 0);
    }
    if (node->type == CAS_NODE_ATAN && cas_node_is_zero(arg)) {
        return cas_node_new_number(expr, 0);
    }

    /* log(1) = 0, log(e) = 1 */
    if (node->type == CAS_NODE_LOG && cas_node_is_one(arg)) {
        return cas_node_new_number(expr, 0);
    }
    if (node->type == CAS_NODE_LN && cas_node_is_one(arg)) {
        return cas_node_new_number(expr, 0);
    }

    /* exp(0) = 1, exp(1) = e */
    if (node->type == CAS_NODE_EXP && cas_node_is_zero(arg)) {
        return cas_node_new_number(expr, 1);
    }

    /* sqrt(x^2) = |x| - we'll just simplify to x for now */
    if (node->type == CAS_NODE_SQRT && arg->type == CAS_NODE_POW) {
        if (arg->children[1] && cas_node_is_two(arg->children[1])) {
            return arg->children[0];
        }
    }

    return node;
}

/* ========================================================================
 * Expansion
 * ======================================================================== */

cas_node_t *cas_expand(cas_expr_t *expr, cas_node_t *node)
{
    if (!node) return NULL;

    /* Expand children first */
    for (int i = 0; i < node->child_count; i++) {
        node->children[i] = cas_expand(expr, node->children[i]);
    }

    switch (node->type) {
        case CAS_NODE_MUL:
            return cas_expand_mul(expr, node);
        case CAS_NODE_POW:
            return cas_expand_pow(expr, node);
        default:
            return node;
    }
}

cas_node_t *cas_expand_mul(cas_expr_t *expr, cas_node_t *node)
{
    if (node->child_count != 2) return node;

    cas_node_t *left = node->children[0];
    cas_node_t *right = node->children[1];

    /* Distribute over addition: (a+b)*c = a*c + b*c */
    if (left->type == CAS_NODE_ADD) {
        cas_node_t *left_mul = cas_node_new_binary(expr, CAS_NODE_MUL, left->children[0], right);
        cas_node_t *right_mul = cas_node_new_binary(expr, CAS_NODE_MUL, left->children[1], right);
        return cas_node_new_binary(expr, CAS_NODE_ADD, left_mul, right_mul);
    }

    if (right->type == CAS_NODE_ADD) {
        cas_node_t *left_mul = cas_node_new_binary(expr, CAS_NODE_MUL, left, right->children[0]);
        cas_node_t *right_mul = cas_node_new_binary(expr, CAS_NODE_MUL, left, right->children[1]);
        return cas_node_new_binary(expr, CAS_NODE_ADD, left_mul, right_mul);
    }

    return node;
}

cas_node_t *cas_expand_pow(cas_expr_t *expr, cas_node_t *node)
{
    (void)expr;
    if (node->child_count != 2) return node;

    /* (a+b)^n - binomial expansion would be complex, leave as-is for now */
    return node;
}

cas_node_t *cas_factor(cas_expr_t *expr, cas_node_t *node)
{
    if (!node) return NULL;

    /* For now, just simplify */
    return cas_simplify(expr, node);
}

cas_node_t *cas_collect(cas_expr_t *expr, cas_node_t *node, const char *var)
{
    if (!node) return node;

    /* Collect terms with respect to variable */
    (void)var;
    return cas_simplify(expr, node);
}

/* ========================================================================
 * Helper functions
 * ======================================================================== */

bool cas_node_is_zero(const cas_node_t *node)
{
    if (!node) return false;
    if (node->type == CAS_NODE_NUMBER && node->value.type == CAS_VAL_DOUBLE) {
        return fabs(node->value.v.d) < 1e-12;
    }
    if (node->type == CAS_NODE_RATIONAL) {
        return node->value.v.rat.num == 0;
    }
    return false;
}

bool cas_node_is_one(const cas_node_t *node)
{
    if (!node) return false;
    if (node->type == CAS_NODE_NUMBER && node->value.type == CAS_VAL_DOUBLE) {
        return fabs(node->value.v.d - 1.0) < 1e-12;
    }
    if (node->type == CAS_NODE_RATIONAL) {
        return node->value.v.rat.num == node->value.v.rat.den && node->value.v.rat.den != 0;
    }
    return false;
}

bool cas_node_is_two(const cas_node_t *node)
{
    if (!node) return false;
    if (node->type == CAS_NODE_NUMBER && node->value.type == CAS_VAL_DOUBLE) {
        return fabs(node->value.v.d - 2.0) < 1e-12;
    }
    return false;
}

bool cas_node_is_constant(const cas_node_t *node)
{
    if (!node) return false;
    return node->type == CAS_NODE_NUMBER || node->type == CAS_NODE_RATIONAL;
}

bool cas_node_is_same_variable(const cas_node_t *a, const cas_node_t *b)
{
    if (!a || !b) return false;
    if (a->type != CAS_NODE_VARIABLE || b->type != CAS_NODE_VARIABLE) return false;
    if (!a->value.v.str || !b->value.v.str) return false;
    return strcmp(a->value.v.str, b->value.v.str) == 0;
}

bool cas_nodes_equal(const cas_node_t *a, const cas_node_t *b)
{
    if (!a || !b) return false;
    if (a->type != b->type) return false;
    if (a->child_count != b->child_count) return false;
    if (a->type == CAS_NODE_VARIABLE && a->value.v.str && b->value.v.str) {
        return strcmp(a->value.v.str, b->value.v.str) == 0;
    }
    for (int i = 0; i < a->child_count; i++) {
        if (!cas_nodes_equal(a->children[i], b->children[i])) return false;
    }
    return true;
}