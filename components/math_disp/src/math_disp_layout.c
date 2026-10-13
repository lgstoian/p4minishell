/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file math_disp_layout.c
 * @brief Natural Display 2D layout engine for math expressions.
 *
 * Computes 2D layout (x, y, width, height, baseline) for each node in the
 * expression tree. Handles special 2D constructs:
 *  - Fractions: numerator above denominator with horizontal bar
 *  - Radicals: sqrt hook + horizontal bar over radicand
 *  - Exponents: superscript positioning
 *  - Subscripts: subscript positioning
 *  - Roots: index + radical
 *  - Functions: function name + parentheses
 *  - Matrices: grid layout
 *  - Operators: spacing around binary operators
 */

#include "math_disp.h"
#include "p4minishell_config.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

/* Layout walker: defined below, but the per-shape layout helpers
 * above all recurse through it. */
void math_disp_layout_node(math_disp_expr_t *expr, math_disp_node_t *node);


/* ========================================================================
 * Layout helper functions
 * ======================================================================== */

static void layout_reset_node(math_disp_node_t *node)
{
    node->layout.x = node->layout.y = 0;
    node->layout.w = node->layout.h = 0;
    node->layout.baseline = 0;
}

static int max_int(int a, int b) { return a > b ? a : b; }

/* ========================================================================
 * Leaf node layout
 * ======================================================================== */

static void layout_number(math_disp_expr_t *expr, math_disp_node_t *node)
{
    layout_reset_node(node);
    if (node->value.type == MATH_DISP_VAL_INT64) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%lld", node->value.v.i64);
        node->layout.w = strlen(buf);
        node->layout.h = 1;
        node->layout.baseline = 0;
    } else if (node->value.type == MATH_DISP_VAL_DOUBLE) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.*g", 10, node->value.v.d);
        node->layout.w = strlen(buf);
        node->layout.h = 1;
        node->layout.baseline = 0;
    } else if (node->value.type == MATH_DISP_VAL_RATIONAL) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%lld/%lld", 
                 node->value.v.rat.num, node->value.v.rat.den);
        node->layout.w = strlen(buf);
        node->layout.h = 1;
        node->layout.baseline = 0;
    } else if (node->value.type == MATH_DISP_VAL_STRING) {
        node->layout.w = node->value.v.str ? strlen(node->value.v.str) : 0;
        node->layout.h = 1;
        node->layout.baseline = 0;
    }
}

static void layout_variable(math_disp_expr_t *expr, math_disp_node_t *node)
{
    layout_reset_node(node);
    if (node->value.type == MATH_DISP_VAL_STRING && node->value.v.str) {
        node->layout.w = strlen(node->value.v.str);
        node->layout.h = 1;
        node->layout.baseline = 0;
    } else {
        node->layout.w = 1;
        node->layout.h = 1;
        node->layout.baseline = 0;
    }
}

static void layout_symbol(math_disp_expr_t *expr, math_disp_node_t *node)
{
    layout_reset_node(node);
    if (node->value.type == MATH_DISP_VAL_STRING && node->value.v.str) {
        node->layout.w = strlen(node->value.v.str);
        node->layout.h = 1;
        node->layout.baseline = 0;
    } else {
        node->layout.w = 2;  /* pi, e, etc. */
        node->layout.h = 1;
        node->layout.baseline = 0;
    }
}

/* ========================================================================
 * Binary operator layout (infix with spacing)
 * ======================================================================== */

static void layout_binary_op(math_disp_expr_t *expr, math_disp_node_t *node)
{
    if (node->child_count < 2) {
        node->layout.w = node->layout.h = 1;
        node->layout.baseline = 0;
        return;
    }
    
    math_disp_node_t *left = node->children[0];
    math_disp_node_t *right = node->children[1];
    
    layout_reset_node(node);
    
    /* Recursively layout children first */
    math_disp_layout_node(expr, left);
    math_disp_layout_node(expr, right);
    
    /* Operator symbol width */
    int op_w = 1;  /* +, -, *, /, ^, _ */
    if (node->type == MATH_DISP_NODE_ADD || node->type == MATH_DISP_NODE_SUB) {
        op_w = 1;  /* + - */
    } else if (node->type == MATH_DISP_NODE_MUL) {
        op_w = 1;  /* * */
    } else if (node->type == MATH_DISP_NODE_DIV) {
        op_w = 1;  /* / */
    } else if (node->type == MATH_DISP_NODE_POW) {
        op_w = 0;  /* no explicit symbol, handled by positioning */
    } else if (node->type == MATH_DISP_NODE_SUBSCRIPT) {
        op_w = 0;  /* handled by positioning */
    }
    
    int spacing = 1;  /* space around binary operators */
    
    node->layout.w = left->layout.w + spacing + op_w + spacing + right->layout.w;
    node->layout.h = max_int(left->layout.h, right->layout.h);
    node->layout.baseline = max_int(left->layout.baseline, right->layout.baseline);
}

static void layout_frac(math_disp_expr_t *expr, math_disp_node_t *node)
{
    if (node->child_count < 2) {
        node->layout.w = node->layout.h = 1;
        node->layout.baseline = 0;
        return;
    }
    
    math_disp_node_t *num = node->children[0];   /* numerator */
    math_disp_node_t *den = node->children[1];   /* denominator */
    
    layout_reset_node(node);
    
    math_disp_layout_node(expr, num);
    math_disp_layout_node(expr, den);
    
    int bar_w = max_int(num->layout.w, den->layout.w);
    
    int total_w = bar_w + 2;  /* padding on sides of fraction bar */
    int total_h = num->layout.h + 1 + den->layout.h;  /* num + bar + den */
    
    node->layout.w = total_w;
    node->layout.h = total_h;
    node->layout.baseline = num->layout.h;  /* baseline at fraction bar */
}

static void layout_sqrt(math_disp_expr_t *expr, math_disp_node_t *node)
{
    if (node->child_count < 1) {
        node->layout.w = node->layout.h = 1;
        return;
    }
    
    math_disp_node_t *radicand = node->children[0];
    math_disp_layout_node(expr, radicand);
    
    int hook_w = 2;  /* sqrt hook width */
    int bar_w = radicand->layout.w + 2;  /* padding on radicand */
    
    node->layout.w = hook_w + bar_w;
    node->layout.h = radicand->layout.h + 1;  /* +1 for radical top bar */
    node->layout.baseline = radicand->layout.baseline + 1;
}

static void layout_root(math_disp_expr_t *expr, math_disp_node_t *node)
{
    if (node->child_count < 2) {
        layout_sqrt(expr, node);
        return;
    }
    
    math_disp_node_t *index = node->children[0];   /* root index */
    math_disp_node_t *radicand = node->children[1];
    
    math_disp_layout_node(expr, index);
    math_disp_layout_node(expr, radicand);
    
    int hook_w = 2;
    int index_w = index->layout.w;
    int bar_w = radicand->layout.w + 2;
    int index_h = index->layout.h;
    
    node->layout.w = hook_w + max_int(index_w, 0) + bar_w;
    node->layout.h = max_int(index_h + 1, radicand->layout.h + 1);
    node->layout.baseline = max_int(index_h, 1) + radicand->layout.baseline;
}

static void layout_pow(math_disp_expr_t *expr, math_disp_node_t *node)
{
    if (node->child_count < 2) {
        math_disp_layout_node(expr, node->children[0]);
        node->layout = node->children[0]->layout;
        return;
    }
    
    math_disp_node_t *base = node->children[0];
    math_disp_node_t *exp = node->children[1];
    
    math_disp_layout_node(expr, base);
    math_disp_layout_node(expr, exp);
    
    int exp_w = exp->layout.w;
    int exp_h = exp->layout.h;
    
    node->layout.w = base->layout.w + exp_w + 1;
    node->layout.h = base->layout.h + exp_h;
    node->layout.baseline = base->layout.baseline;
}

static void layout_subscript(math_disp_expr_t *expr, math_disp_node_t *node)
{
    if (node->child_count < 2) {
        math_disp_layout_node(expr, node->children[0]);
        node->layout = node->children[0]->layout;
        return;
    }
    
    math_disp_node_t *base = node->children[0];
    math_disp_node_t *sub = node->children[1];
    
    math_disp_layout_node(expr, base);
    math_disp_layout_node(expr, sub);
    
    int sub_w = sub->layout.w;
    int sub_h = sub->layout.h;
    
    node->layout.w = base->layout.w + sub_w + 1;
    node->layout.h = base->layout.h + sub_h;
    node->layout.baseline = base->layout.baseline;
}

static void layout_function(math_disp_expr_t *expr, math_disp_node_t *node)
{
    if (node->child_count < 1) {
        node->layout.w = node->layout.h = 1;
        node->layout.baseline = 0;
        return;
    }
    
    math_disp_node_t *arg = node->children[0];
    math_disp_layout_node(expr, arg);
    
    /* Function name width */
    int fn_w = 0;
    switch (node->type) {
        case MATH_DISP_NODE_SIN: fn_w = 3; break;
        case MATH_DISP_NODE_COS: fn_w = 3; break;
        case MATH_DISP_NODE_TAN: fn_w = 3; break;
        case MATH_DISP_NODE_ASIN: fn_w = 4; break;
        case MATH_DISP_NODE_ACOS: fn_w = 4; break;
        case MATH_DISP_NODE_ATAN: fn_w = 4; break;
        case MATH_DISP_NODE_LOG: fn_w = 3; break;
        case MATH_DISP_NODE_LN: fn_w = 2; break;
        case MATH_DISP_NODE_EXP: fn_w = 3; break;
        case MATH_DISP_NODE_ABS: fn_w = 3; break;
        default: fn_w = 1;
    }
    
    int paren_w = 2;  /* ( ) */
    
    node->layout.w = fn_w + paren_w + arg->layout.w;
    node->layout.h = max_int(arg->layout.h, 1);
    node->layout.baseline = arg->layout.baseline;
}

static void layout_negate(math_disp_expr_t *expr, math_disp_node_t *node)
{
    if (node->child_count < 1) return;
    math_disp_layout_node(expr, node->children[0]);
    math_disp_node_t *child = node->children[0];
    node->layout = child->layout;
    node->layout.w += 1;  /* minus sign */
}

static void layout_paren(math_disp_expr_t *expr, math_disp_node_t *node)
{
    if (node->child_count < 1) {
        node->layout.w = 2;  /* () */
        node->layout.h = 1;
        node->layout.baseline = 0;
        return;
    }
    
    math_disp_layout_node(expr, node->children[0]);
    math_disp_node_t *child = node->children[0];
    node->layout = child->layout;
    node->layout.w += 2;  /* ( ) */
}

static void layout_matrix(math_disp_expr_t *expr, math_disp_node_t *node)
{
    /* Simplified: treat as [ ... ] */
    node->layout.w = 10;
    node->layout.h = 3;
    node->layout.baseline = 1;
}

static void layout_sequence(math_disp_expr_t *expr, math_disp_node_t *node)
{
    int total_w = 0;
    int max_h = 1;
    int max_baseline = 0;
    
    for (int i = 0; i < node->child_count; i++) {
        math_disp_layout_node(expr, node->children[i]);
        math_disp_node_t *child = node->children[i];
        total_w += child->layout.w + 1;  /* spacing */
        max_h = max_int(max_h, child->layout.h);
        max_baseline = max_int(max_baseline, child->layout.baseline);
    }
    
    node->layout.w = total_w;
    node->layout.h = max_h;
    node->layout.baseline = max_baseline;
}

static void layout_text(math_disp_expr_t *expr, math_disp_node_t *node)
{
    layout_reset_node(node);
    if (node->value.type == MATH_DISP_VAL_STRING && node->value.v.str) {
        node->layout.w = strlen(node->value.v.str);
    } else {
        node->layout.w = 1;
    }
    node->layout.h = 1;
    node->layout.baseline = 0;
}

/* ========================================================================
 * Main layout dispatcher
 * ======================================================================== */

void math_disp_layout_node(math_disp_expr_t *expr, math_disp_node_t *node)
{
    if (!node) return;
    
    /* Already laid out? */
    if (node->layout.w > 0 || node->layout.h > 0) {
        return;
    }
    
    switch (node->type) {
        case MATH_DISP_NODE_NUMBER:
            layout_number(NULL, node);
            break;
        case MATH_DISP_NODE_VARIABLE:
            layout_variable(NULL, node);
            break;
        case MATH_DISP_NODE_SYMBOL:
            layout_symbol(NULL, node);
            break;
        case MATH_DISP_NODE_ADD:
        case MATH_DISP_NODE_SUB:
        case MATH_DISP_NODE_MUL:
        case MATH_DISP_NODE_DIV:
            layout_binary_op(NULL, node);
            break;
        case MATH_DISP_NODE_FRAC:
            layout_frac(NULL, node);
            break;
        case MATH_DISP_NODE_SQRT:
            layout_sqrt(NULL, node);
            break;
        case MATH_DISP_NODE_CBRT:
        case MATH_DISP_NODE_ROOT:
            layout_root(NULL, node);
            break;
        case MATH_DISP_NODE_POW:
            layout_pow(NULL, node);
            break;
        case MATH_DISP_NODE_SUBSCRIPT:
            layout_subscript(NULL, node);
            break;
        case MATH_DISP_NODE_SIN:
        case MATH_DISP_NODE_COS:
        case MATH_DISP_NODE_TAN:
        case MATH_DISP_NODE_ASIN:
        case MATH_DISP_NODE_ACOS:
        case MATH_DISP_NODE_ATAN:
        case MATH_DISP_NODE_LOG:
        case MATH_DISP_NODE_LN:
        case MATH_DISP_NODE_EXP:
        case MATH_DISP_NODE_ABS:
        case MATH_DISP_NODE_FACTORIAL:
            layout_function(NULL, node);
            break;
        case MATH_DISP_NODE_NEGATE:
            layout_negate(NULL, node);
            break;
        case MATH_DISP_NODE_PAREN:
            layout_paren(NULL, node);
            break;
        case MATH_DISP_NODE_MATRIX:
        case MATH_DISP_NODE_VECTOR:
            layout_matrix(NULL, node);
            break;
        case MATH_DISP_NODE_SEQUENCE:
            layout_sequence(NULL, node);
            break;
        case MATH_DISP_NODE_TEXT:
            layout_text(NULL, node);
            break;
        default:
            node->layout.w = node->layout.h = 1;
            node->layout.baseline = 0;
            break;
    }
}

/* ========================================================================
 * Main layout entry point
 * ======================================================================== */

int math_disp_layout(math_disp_expr_t *expr)
{
    if (!expr || !expr->root) return -1;
    
    if (expr->layout_valid && expr->root->layout.w > 0) {
        return 0;  /* Already laid out */
    }
    
    math_disp_layout_node(NULL, expr->root);
    expr->layout_valid = true;
    
    return 0;
}

/* Get layout grid */
const math_disp_layout_t *math_disp_get_layout(const math_disp_expr_t *expr)
{
    static math_disp_layout_t layout;
    
    if (!expr || !expr->root || !expr->layout_valid) {
        layout.width = layout.height = 0;
        return &layout;
    }
    
    /* Build layout grid from node positions */
    /* This is a simplified version - full implementation would rasterize nodes */
    math_disp_layout_t *layout_ptr = (math_disp_layout_t *)malloc(sizeof(math_disp_layout_t));
    if (!layout_ptr) return NULL;
    
    memset(layout_ptr, 0, sizeof(math_disp_layout_t));
    layout_ptr->width = expr->root->layout.w;
    layout_ptr->height = expr->root->layout.h;
    layout_ptr->cursor_x = expr->cursor.offset;
    layout_ptr->cursor_y = 0;
    
    return (const math_disp_layout_t *)layout_ptr;
}

/* ========================================================================
 * Layout invalidation
 * ======================================================================== */

void math_disp_invalidate_layout(math_disp_expr_t *expr)
{
    if (expr) expr->layout_valid = false;
}
