/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file cas_diff.c
 * @brief CAS differentiation engine.
 */

#include "cas.h"
#include "p4minishell_config.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* Forward declarations: the entry points above dispatch to these
 * helpers, which are defined further down the file. */
/* ========================================================================
 * Differentiation
 * ======================================================================== */
cas_node_t *cas_diff(cas_expr_t *expr, cas_node_t *node, const char *var);


/* ========================================================================
 * Differentiation
 * ======================================================================== */

cas_node_t *cas_diff(cas_expr_t *expr, cas_node_t *node, const char *var)
{
    if (!node || !var) return NULL;
    
    switch (node->type) {
        case CAS_NODE_NUMBER:
        case CAS_NODE_RATIONAL:
            return cas_node_new_number(expr, 0);
        
        case CAS_NODE_VARIABLE:
            if (node->value.v.str && strcmp(node->value.v.str, var) == 0) {
                return cas_node_new_number(expr, 1);
            }
            return cas_node_new_number(expr, 0);
        
        case CAS_NODE_ADD:
            if (node->child_count >= 2) {
                cas_node_t *left = cas_diff(expr, node->children[0], var);
                cas_node_t *right = cas_diff(expr, node->children[1], var);
                return cas_node_new_binary(expr, CAS_NODE_ADD, left, right);
            }
            break;
            
        case CAS_NODE_SUB:
            if (node->child_count >= 2) {
                cas_node_t *left = cas_diff(expr, node->children[0], var);
                cas_node_t *right = cas_diff(expr, node->children[1], var);
                return cas_node_new_binary(expr, CAS_NODE_SUB, left, right);
            }
            break;
            
        case CAS_NODE_MUL:
            if (node->child_count >= 2) {
                /* Product rule: (f*g)' = f'*g + f*g' */
                cas_node_t *left = node->children[0];
                cas_node_t *right = node->children[1];
                
                cas_node_t *left_diff = cas_diff(expr, left, var);
                cas_node_t *right_diff = cas_diff(expr, right, var);
                
                cas_node_t *term1 = cas_node_new_binary(expr, CAS_NODE_MUL, left_diff, right);
                cas_node_t *term2 = cas_node_new_binary(expr, CAS_NODE_MUL, left, right_diff);
                
                return cas_node_new_binary(expr, CAS_NODE_ADD, term1, term2);
            }
            break;
            
        case CAS_NODE_DIV:
            if (node->child_count >= 2) {
                /* Quotient rule: (u/v)' = (u'*v - u*v') / v^2 */
                cas_node_t *u = node->children[0];
                cas_node_t *v = node->children[1];
                
                cas_node_t *u_diff = cas_diff(expr, u, var);
                cas_node_t *v_diff = cas_diff(expr, v, var);
                
                cas_node_t *num1 = cas_node_new_binary(expr, CAS_NODE_MUL, u_diff, v);
                cas_node_t *num2 = cas_node_new_binary(expr, CAS_NODE_MUL, u, v_diff);
                cas_node_t *num = cas_node_new_binary(expr, CAS_NODE_SUB, num1, num2);
                
                cas_node_t *v_sq = cas_node_new_binary(expr, CAS_NODE_POW, v, cas_node_new_number(expr, 2));
                
                return cas_node_new_binary(expr, CAS_NODE_DIV, num, v_sq);
            }
            break;
            
        case CAS_NODE_POW:
            if (node->child_count >= 2) {
                cas_node_t *base = node->children[0];
                cas_node_t *exp = node->children[1];
                
                /* x^n -> n*x^(n-1) for constant exponent.
                 * The derivative is n*x^(n-1)*x' (chain rule); the previous
                 * code built this expression and then returned only x'. */
                if (exp->type == CAS_NODE_NUMBER) {
                    double n = exp->value.v.d;
                    if (n == 0) return cas_node_new_number(expr, 0);

                    cas_node_t *pow = cas_node_new_binary(expr, CAS_NODE_MUL,
                        cas_node_new_number(expr, n),
                        cas_node_new_binary(expr, CAS_NODE_POW,
                            base,
                            cas_node_new_number(expr, n - 1)));

                    return cas_node_new_binary(expr, CAS_NODE_MUL,
                                               pow, cas_diff(expr, base, var));
                }
                
                /* General case: u^v = u^v * (v' * ln(u) + v * u'/u) */
                cas_node_t *u = node->children[0];
                cas_node_t *v = node->children[1];
                
                cas_node_t *u_diff = cas_diff(expr, u, var);
                cas_node_t *v_diff = cas_diff(expr, v, var);
                
                cas_node_t *ln_u = cas_node_new_function(expr, CAS_NODE_LN, u);
                cas_node_t *v_ln_u = cas_node_new_binary(expr, CAS_NODE_MUL, v_diff, ln_u);
                
                cas_node_t *u_diff_div_u = cas_node_new_binary(expr, CAS_NODE_DIV,
                    u_diff, u);
                cas_node_t *v_u_diff = cas_node_new_binary(expr, CAS_NODE_MUL, v, u_diff_div_u);
                
                cas_node_t *sum = cas_node_new_binary(expr, CAS_NODE_ADD, v_ln_u, v_u_diff);
                cas_node_t *base_pow = cas_node_new_binary(expr, CAS_NODE_POW, u, v);
                cas_node_t *result = cas_node_new_binary(expr, CAS_NODE_MUL, base_pow, sum);
                
                return result;
            }
            break;
            
        case CAS_NODE_SIN:
            if (node->child_count >= 1) {
                cas_node_t *arg = node->children[0];
                cas_node_t *arg_diff = cas_diff(expr, arg, var);
                cas_node_t *cos = cas_node_new_function(expr, CAS_NODE_COS, arg);
                return cas_node_new_binary(expr, CAS_NODE_MUL, arg_diff, cos);
            }
            break;
            
        case CAS_NODE_COS:
            if (node->child_count >= 1) {
                cas_node_t *arg = node->children[0];
                cas_node_t *arg_diff = cas_diff(expr, arg, var);
                cas_node_t *sin = cas_node_new_function(expr, CAS_NODE_SIN, arg);
                cas_node_t *neg = cas_node_new_unary(expr, CAS_NODE_NEGATE, sin);
                return cas_node_new_binary(expr, CAS_NODE_MUL, arg_diff, neg);
            }
            break;
            
        case CAS_NODE_TAN:
            if (node->child_count >= 1) {
                cas_node_t *arg = node->children[0];
                cas_node_t *arg_diff = cas_diff(expr, arg, var);
                cas_node_t *cos = cas_node_new_function(expr, CAS_NODE_COS, arg);
                cas_node_t *cos_sq = cas_node_new_binary(expr, CAS_NODE_POW, cos, cas_node_new_number(expr, 2));
                cas_node_t *sec_sq = cas_node_new_binary(expr, CAS_NODE_DIV, 
                    cas_node_new_number(expr, 1), cos_sq);
                return cas_node_new_binary(expr, CAS_NODE_MUL, arg_diff, sec_sq);
            }
            break;
            
        case CAS_NODE_ASIN:
            if (node->child_count >= 1) {
                cas_node_t *arg = node->children[0];
                cas_node_t *arg_diff = cas_diff(expr, arg, var);
                cas_node_t *one = cas_node_new_number(expr, 1);
                cas_node_t *arg_sq = cas_node_new_binary(expr, CAS_NODE_POW, arg, cas_node_new_number(expr, 2));
                cas_node_t *one_minus = cas_node_new_binary(expr, CAS_NODE_SUB, one, arg_sq);
                cas_node_t *sqrt = cas_node_new_function(expr, CAS_NODE_SQRT, one_minus);
                return cas_node_new_binary(expr, CAS_NODE_DIV, arg_diff, sqrt);
            }
            break;
            
        case CAS_NODE_ACOS:
            if (node->child_count >= 1) {
                cas_node_t *arg = node->children[0];
                cas_node_t *arg_diff = cas_diff(expr, arg, var);
                cas_node_t *one = cas_node_new_number(expr, 1);
                cas_node_t *arg_sq = cas_node_new_binary(expr, CAS_NODE_POW, arg, cas_node_new_number(expr, 2));
                cas_node_t *one_minus = cas_node_new_binary(expr, CAS_NODE_SUB, one, arg_sq);
                cas_node_t *sqrt = cas_node_new_function(expr, CAS_NODE_SQRT, one_minus);
                cas_node_t *neg = cas_node_new_unary(expr, CAS_NODE_NEGATE, arg_diff);
                return cas_node_new_binary(expr, CAS_NODE_DIV, neg, sqrt);
            }
            break;
            
        case CAS_NODE_ATAN:
            if (node->child_count >= 1) {
                cas_node_t *arg = node->children[0];
                cas_node_t *arg_diff = cas_diff(expr, arg, var);
                cas_node_t *one = cas_node_new_number(expr, 1);
                cas_node_t *arg_sq = cas_node_new_binary(expr, CAS_NODE_POW, arg, cas_node_new_number(expr, 2));
                cas_node_t *one_plus = cas_node_new_binary(expr, CAS_NODE_ADD, one, arg_sq);
                return cas_node_new_binary(expr, CAS_NODE_DIV, arg_diff, one_plus);
            }
            break;
            
        case CAS_NODE_SINH:
            if (node->child_count >= 1) {
                cas_node_t *arg = node->children[0];
                cas_node_t *arg_diff = cas_diff(expr, arg, var);
                cas_node_t *cosh = cas_node_new_function(expr, CAS_NODE_COSH, arg);
                return cas_node_new_binary(expr, CAS_NODE_MUL, arg_diff, cosh);
            }
            break;
            
        case CAS_NODE_COSH:
            if (node->child_count >= 1) {
                cas_node_t *arg = node->children[0];
                cas_node_t *arg_diff = cas_diff(expr, arg, var);
                cas_node_t *sinh = cas_node_new_function(expr, CAS_NODE_SINH, arg);
                return cas_node_new_binary(expr, CAS_NODE_MUL, arg_diff, sinh);
            }
            break;
            
        case CAS_NODE_TANH:
            if (node->child_count >= 1) {
                cas_node_t *arg = node->children[0];
                cas_node_t *arg_diff = cas_diff(expr, arg, var);
                cas_node_t *cosh = cas_node_new_function(expr, CAS_NODE_COSH, arg);
                cas_node_t *cosh_sq = cas_node_new_binary(expr, CAS_NODE_POW, cosh, cas_node_new_number(expr, 2));
                cas_node_t *one = cas_node_new_number(expr, 1);
                cas_node_t *sech_sq = cas_node_new_binary(expr, CAS_NODE_SUB, one, cosh_sq);
                return cas_node_new_binary(expr, CAS_NODE_MUL, arg_diff, sech_sq);
            }
            break;
            
        case CAS_NODE_LOG:
        case CAS_NODE_LN:
            if (node->child_count >= 1) {
                cas_node_t *arg = node->children[0];
                cas_node_t *arg_diff = cas_diff(expr, arg, var);
                return cas_node_new_binary(expr, CAS_NODE_DIV, arg_diff, arg);
            }
            break;
            
        case CAS_NODE_EXP:
            if (node->child_count >= 1) {
                cas_node_t *arg = node->children[0];
                cas_node_t *arg_diff = cas_diff(expr, arg, var);
                return cas_node_new_binary(expr, CAS_NODE_MUL, arg_diff, node);
            }
            break;
            
        case CAS_NODE_ABS:
            if (node->child_count >= 1) {
                cas_node_t *arg = node->children[0];
                cas_node_t *arg_diff = cas_diff(expr, arg, var);
                cas_node_t *sign = cas_node_new_function(expr, CAS_NODE_ABS, node->children[0]);
                return cas_node_new_binary(expr, CAS_NODE_MUL, arg_diff, sign);
            }
            break;
            
        case CAS_NODE_SQRT:
            if (node->child_count >= 1) {
                cas_node_t *arg = node->children[0];
                cas_node_t *arg_diff = cas_diff(expr, arg, var);
                cas_node_t *two = cas_node_new_number(expr, 2);
                cas_node_t *sqrt = cas_node_new_function(expr, CAS_NODE_SQRT, arg);
                cas_node_t *den = cas_node_new_binary(expr, CAS_NODE_MUL, two, sqrt);
                return cas_node_new_binary(expr, CAS_NODE_DIV, arg_diff, den);
            }
            break;
        default:
            break;
    }
    
    return cas_node_new_number(expr, 0);
}

cas_node_t *cas_diff_partial(cas_expr_t *expr, cas_node_t *node, const char *var, int order)
{
    cas_node_t *result = node;
    for (int i = 0; i < order; i++) {
        cas_node_t *next = cas_diff(expr, result, var);
        if (!next) return NULL;
        result = next;
    }
    return result;
}