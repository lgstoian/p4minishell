/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file cas_solve.c
 * @brief CAS equation solving engine.
 */

#include "cas.h"
#include "p4minishell_config.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* Forward declarations: the entry points above dispatch to these
 * helpers, which are defined further down the file. */
/* ========================================================================
 * Equation solving
 * ======================================================================== */
cas_node_t *cas_solve(cas_expr_t *expr, cas_node_t *node, const char *var);
cas_node_t *cas_equation_to_zero(cas_expr_t *expr, cas_node_t *node);
bool cas_extract_polynomial(cas_node_t *node, const char *var, double *coeffs, int *count);


/* ========================================================================
 * Equation solving
 * ======================================================================== */

cas_node_t *cas_solve(cas_expr_t *expr, cas_node_t *node, const char *var)
{
    if (!node || !var) return NULL;

    /* Normalize equation to f(x) = 0 form */
    cas_node_t *f = cas_equation_to_zero(expr, node);
    if (!f) return NULL;

    /* Try polynomial solving first */
    cas_node_t *poly_solution = cas_solve_polynomial(expr, f, var);
    if (poly_solution) return poly_solution;

    /* Try linear solving */
    cas_node_t *linear_solution = cas_solve_linear(expr, f, var);
    if (linear_solution) return linear_solution;

    /* Fall back to numerical solving */
    return cas_solve_nonlinear(expr, f, var, 0);
}

cas_node_t *cas_equation_to_zero(cas_expr_t *expr, cas_node_t *eq)
{
    if (eq->type == CAS_NODE_EQ) {
        /* Convert a = b to a - b = 0 */
        cas_node_t *left = eq->children[0];
        cas_node_t *right = eq->children[1];
        return cas_node_new_binary(expr, CAS_NODE_SUB, left, right);
    }

    /* Assume already in f(x) = 0 form */
    return eq;
}

cas_node_t *cas_solve_polynomial(cas_expr_t *expr, cas_node_t *f, const char *var)
{
    /* Extract polynomial degree and coefficients. Initialised so a solver
     * failure path can never read indeterminate values. */
    int degree = 0;
    double coeffs[10] = {0};  /* Max degree 9 for now */

    if (!cas_extract_polynomial(f, var, coeffs, &degree)) {
        return NULL;
    }

    if (degree == 1) {
        /* Linear: a*x + b = 0 -> x = -b/a */
        if (fabs(coeffs[1]) > 1e-12) {
            double sol = -coeffs[0] / coeffs[1];
            return cas_node_new_number(expr, sol);
        }
    } else if (degree == 2) {
        /* Quadratic: ax^2 + bx + c = 0 */
        double a = coeffs[2];
        double b = coeffs[1];
        double c = coeffs[0];

        if (fabs(a) < 1e-12) return NULL;

        double disc = b*b - 4*a*c;
        if (disc < 0) return NULL;  /* No real roots */

        double sqrt_disc = sqrt(disc);
        double x1 = (-b + sqrt_disc) / (2*a);
        double x2 = (-b - sqrt_disc) / (2*a);

        /* Return as sequence of solutions */
        cas_node_t *sol1 = cas_node_new_number(expr, x1);
        cas_node_t *sol2 = cas_node_new_number(expr, x2);

        return cas_node_new_binary(expr, CAS_NODE_SEQUENCE, sol1, sol2);
    }

    return NULL;
}

bool cas_extract_polynomial(cas_node_t *f, const char *var, double *coeffs, int *degree)
{
    /* For now, only handle simple cases */
    (void)f; (void)var; (void)coeffs; (void)degree;
    return false;
}

cas_node_t *cas_solve_linear(cas_expr_t *expr, cas_node_t *f, const char *var)
{
    /* Solve a*x + b = 0 for x */
    /* This is a simplified version - real implementation would extract coefficients */
    (void)expr; (void)f; (void)var;
    return NULL;
}

cas_node_t *cas_solve_nonlinear(cas_expr_t *expr, cas_node_t *f, const char *var, double guess)
{
    /* Use Newton's method for numerical solving */
    double x = guess;
    double fx, fpx;

    for (int iter = 0; iter < 50; iter++) {
        cas_node_t *fx_node = cas_eval_at(expr, f, var, x);
        if (!fx_node) return NULL;

        double fx_val;
        if (!cas_eval_num(expr, fx_node, &fx_val)) return NULL;
        fx = fx_val;

        if (fabs(fx) < 1e-12) {
            return cas_node_new_number(expr, x);
        }

        /* Numerical derivative */
        double h = 1e-6;
        cas_node_t *fx_plus = cas_eval_at(expr, f, var, x + h);
        cas_node_t *fx_minus = cas_eval_at(expr, f, var, x - h);
        if (!fx_plus || !fx_minus) return NULL;

        double fx_plus_val, fx_minus_val;
        if (!cas_eval_num(expr, fx_plus, &fx_plus_val)) return NULL;
        if (!cas_eval_num(expr, fx_minus, &fx_minus_val)) return NULL;

        fpx = (fx_plus_val - fx_minus_val) / (2*h);

        if (fabs(fpx) < 1e-12) return NULL;

        x = x - fx / fpx;

        if (fabs(fx) < 1e-12) {
            return cas_node_new_number(expr, x);
        }
    }

    return NULL;
}

cas_node_t *cas_solve_system(cas_expr_t *expr, cas_node_t **equations, int count, const char **vars, int var_count)
{
    /* System of equations solver - placeholder */
    (void)expr; (void)equations; (void)count; (void)vars; (void)var_count;
    return NULL;
}