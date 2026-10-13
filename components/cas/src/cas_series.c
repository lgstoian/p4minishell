/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file cas_series.c
 * @brief CAS series expansion (Taylor/Maclaurin).
 */

#include "cas.h"
#include "p4minishell_config.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* Forward declarations: the entry points above dispatch to these
 * helpers, which are defined further down the file. */
/* ========================================================================
 * Series expansion (Taylor/Maclaurin)
 * ======================================================================== */
cas_node_t *cas_series(cas_expr_t *expr, cas_node_t *node, const char *var, double point, int order);


/* ========================================================================
 * Series expansion (Taylor/Maclaurin)
 * ======================================================================== */

cas_node_t *cas_series(cas_expr_t *expr, cas_node_t *node, const char *var, double point, int order)
{
    if (!node || !var || order <= 0) return NULL;
    
    /* For now, return a placeholder indicating series expansion is not fully implemented */
    cas_node_t *result = cas_node_new_function(expr, CAS_NODE_SERIES, node);
    if (!result) return NULL;
    
    /* Store series parameters as children or in a custom structure */
    /* For now, return the original node with a marker */
    return node;
}

/* ========================================================================
 * Series expansion helper functions
 * ======================================================================== */

cas_node_t *cas_series_term(cas_expr_t *expr, cas_node_t *node, const char *var, double point, int n)
{
    /* Compute the n-th term of the Taylor series: f^(n)(a) * (x-a)^n / n! */
    /* This would require computing the n-th derivative at the expansion point */
    return NULL;
}

double cas_factorial(int n)
{
    if (n <= 1) return 1.0;
    double result = 1.0;
    for (int i = 2; i <= n; i++) {
        result *= i;
    }
    return result;
}

double cas_derivative_at(cas_expr_t *expr, cas_node_t *node, const char *var, double point, int order)
{
    double value = 0.0;
    cas_node_t *sub;

    if (order == 0) {
        /* cas_eval_at() returns a node, not a number, so substitute the point
         * and read the value back through the numeric evaluator. */
        sub = (var != NULL) ? cas_eval_at(expr, node, var, point) : node;
        if (sub == NULL || !cas_eval_num(expr, sub, &value) || !isfinite(value)) {
            return 0.0;
        }
        return value;
    }
    return 0.0;
}