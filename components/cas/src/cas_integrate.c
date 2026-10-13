/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file cas_integrate.c
 * @brief CAS integration engine.
 */

#include "cas.h"
#include "p4minishell_config.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* Forward declarations: the entry points above dispatch to these
 * helpers, which are defined further down the file. */
/* ========================================================================
 * Integration
 * ======================================================================== */
cas_node_t *cas_integrate(cas_expr_t *expr, cas_node_t *node, const char *var);
cas_node_t *cas_integrate_product(cas_expr_t *expr, cas_node_t *node, const char *var);
cas_node_t *cas_integrate_rational(cas_expr_t *expr, cas_node_t *node, const char *var);


/* ========================================================================
 * Integration
 * ======================================================================== */

cas_node_t *cas_integrate(cas_expr_t *expr, cas_node_t *node, const char *var)
{
    if (!node || !var) return NULL;

    switch (node->type) {
        case CAS_NODE_NUMBER:
        case CAS_NODE_RATIONAL:
            /* ∫ c dx = c*x */
            return cas_node_new_binary(expr, CAS_NODE_MUL,
                cas_node_copy(expr, node),
                cas_node_new_variable(expr, var));

        case CAS_NODE_VARIABLE:
            if (node->value.v.str && strcmp(node->value.v.str, var) == 0) {
                /* ∫ x dx = x^2/2 */
                cas_node_t *x = cas_node_new_variable(expr, var);
                cas_node_t *pow = cas_node_new_binary(expr, CAS_NODE_POW, x, cas_node_new_number(expr, 2));
                return cas_node_new_binary(expr, CAS_NODE_DIV, pow, cas_node_new_number(expr, 2));
            }
            /* ∫ c dx = c*x (for other variables) */
            return cas_node_new_binary(expr, CAS_NODE_MUL,
                cas_node_copy(expr, node),
                cas_node_new_variable(expr, var));

        case CAS_NODE_ADD:
            if (node->child_count >= 2) {
                cas_node_t *left = cas_integrate(expr, node->children[0], var);
                cas_node_t *right = cas_integrate(expr, node->children[1], var);
                return cas_node_new_binary(expr, CAS_NODE_ADD, left, right);
            }
            break;

        case CAS_NODE_SUB:
            if (node->child_count >= 2) {
                cas_node_t *left = cas_integrate(expr, node->children[0], var);
                cas_node_t *right = cas_integrate(expr, node->children[1], var);
                return cas_node_new_binary(expr, CAS_NODE_SUB, left, right);
            }
            break;

        case CAS_NODE_MUL:
            /* Try to find a u-substitution or use integration by parts */
            return cas_integrate_product(expr, node, var);

        case CAS_NODE_DIV:
            return cas_integrate_rational(expr, node, var);

        case CAS_NODE_POW:
            if (node->child_count >= 2) {
                cas_node_t *base = node->children[0];
                cas_node_t *exp = node->children[1];

                /* ∫ x^n dx = x^(n+1)/(n+1) for constant n != -1 */
                if (exp->type == CAS_NODE_NUMBER && base->type == CAS_NODE_VARIABLE &&
                    base->value.v.str && strcmp(base->value.v.str, var) == 0) {
                    double n = exp->value.v.d;
                    if (fabs(n + 1.0) > 1e-12) {
                        double new_exp = n + 1.0;
                        cas_node_t *pow = cas_node_new_binary(expr, CAS_NODE_POW,
                            cas_node_copy(expr, base),
                            cas_node_new_number(expr, n + 1));
                        return cas_node_new_binary(expr, CAS_NODE_DIV, pow, cas_node_new_number(expr, new_exp));
                    }
                    /* ∫ x^-1 dx = ln|x| */
                    if (fabs(n + 1.0) < 1e-12) {
                        cas_node_t *ln = cas_node_new_function(expr, CAS_NODE_LN,
                            cas_node_copy(expr, base));
                        return ln;
                    }
                }
            }
            break;

        case CAS_NODE_SIN:
            if (node->child_count >= 1) {
                cas_node_t *arg = node->children[0];
                if (arg->type == CAS_NODE_VARIABLE && arg->value.v.str &&
                    strcmp(arg->value.v.str, var) == 0) {
                    /* ∫ sin(x) dx = -cos(x) */
                    cas_node_t *neg = cas_node_new_unary(expr, CAS_NODE_NEGATE,
                        cas_node_new_function(expr, CAS_NODE_COS,
                            cas_node_copy(expr, node->children[0])));
                    return neg;
                }
            }
            break;

        case CAS_NODE_COS:
            if (node->child_count >= 1) {
                cas_node_t *arg = node->children[0];
                if (arg->type == CAS_NODE_VARIABLE && arg->value.v.str &&
                    strcmp(arg->value.v.str, var) == 0) {
                    /* ∫ cos(x) dx = sin(x) */
                    return cas_node_new_function(expr, CAS_NODE_SIN, cas_node_copy(expr, node->children[0]));
                }
            }
            break;

        case CAS_NODE_EXP:
            if (node->child_count >= 1) {
                cas_node_t *arg = node->children[0];
                if (arg->type == CAS_NODE_VARIABLE && arg->value.v.str &&
                    strcmp(arg->value.v.str, var) == 0) {
                    /* ∫ e^x dx = e^x */
                    return cas_node_copy(expr, node);
                }
            }
            break;
        default:
            break;
    }

    return NULL;
}

cas_node_t *cas_integrate_definite(cas_expr_t *expr, cas_node_t *node, const char *var, double a, double b)
{
    if (!node || !var) return NULL;

    /* Find antiderivative */
    cas_node_t *antideriv = cas_integrate(expr, node, var);
    if (!antideriv) return NULL;

    /* Evaluate at bounds: F(b) - F(a) */
    cas_node_t *F_b = cas_eval_at(expr, antideriv, var, b);
    cas_node_t *F_a = cas_eval_at(expr, antideriv, var, a);

    if (!F_b || !F_a) return NULL;

    return cas_node_new_binary(expr, CAS_NODE_SUB, F_b, F_a);
}

cas_node_t *cas_integrate_product(cas_expr_t *expr, cas_node_t *node, const char *var)
{
    /* Integration by parts: ∫ u dv = uv - ∫ v du
     * Simple heuristic: pick u as polynomial/log, dv as exponential/trig */
    (void)expr; (void)node; (void)var;
    return NULL;
}

cas_node_t *cas_integrate_rational(cas_expr_t *expr, cas_node_t *node, const char *var)
{
    if (node->child_count != 2) return NULL;

    cas_node_t *num = node->children[0];
    cas_node_t *den = node->children[1];

    /* Only the 1/(x-a) form is handled below; anything else must be refused
     * rather than silently integrated as if the numerator were 1. */
    if (num->type != CAS_NODE_NUMBER || num->value.v.d != 1.0) {
        return NULL;
    }

    /* Check if numerator degree < denominator degree */
    /* If not, perform polynomial division first */

    /* For now, try partial fraction decomposition for simple cases */
    /* ∫ 1/(x-a) dx = ln|x-a| */
    if (den->type == CAS_NODE_SUB) {
        cas_node_t *a = den->children[0];
        cas_node_t *b = den->children[1];

        if (b->type == CAS_NODE_NUMBER && a->type == CAS_NODE_VARIABLE &&
            a->value.v.str && strcmp(a->value.v.str, var) == 0) {
            /* ∫ 1/(x-a) dx = ln|x-a| */
            cas_node_t *ln = cas_node_new_function(expr, CAS_NODE_LN, den);
            return ln;
        }
    }

    return NULL;
}