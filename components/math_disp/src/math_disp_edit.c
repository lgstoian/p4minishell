/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file math_disp_edit.c
 * @brief Natural Display math expression editing operations.
 */

#include "math_disp.h"
#include "p4minishell_config.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>


/* ========================================================================
 * Cursor movement (2D navigation through expression tree)
 * ======================================================================== */

static math_disp_node_t *find_leftmost_leaf(math_disp_node_t *node)
{
    if (!node) return NULL;
    while (node->child_count > 0) {
        node = node->children[0];
    }
    return node;
}

static math_disp_node_t *find_rightmost_leaf(math_disp_node_t *node)
{
    if (!node) return NULL;
    while (node->child_count > 0) {
        node = node->children[node->child_count - 1];
    }
    return node;
}

static math_disp_node_t *find_next_leaf(math_disp_node_t *node)
{
    if (!node) return NULL;
    
    math_disp_node_t *parent = node->parent;
    while (parent) {
        for (int i = 0; i < parent->child_count; i++) {
            if (parent->children[i] == node) {
                if (i + 1 < parent->child_count) {
                    return find_leftmost_leaf(parent->children[i + 1]);
                }
                node = parent;
                parent = parent->parent;
                break;
            }
        }
    }
    return NULL;
}

static math_disp_node_t *find_prev_leaf(math_disp_node_t *node)
{
    if (!node) return NULL;
    
    math_disp_node_t *parent = node->parent;
    while (parent) {
        for (int i = 0; i < parent->child_count; i++) {
            if (parent->children[i] == node) {
                if (i > 0) {
                    return find_rightmost_leaf(parent->children[i - 1]);
                }
                node = parent;
                parent = parent->parent;
                break;
            }
        }
    }
    return NULL;
}

static math_disp_node_t *find_up_leaf(math_disp_node_t *node)
{
    if (!node) return NULL;
    
    math_disp_node_t *parent = node->parent;
    if (!parent) return NULL;
    
    int idx = -1;
    for (int i = 0; i < parent->child_count; i++) {
        if (parent->children[i] == node) {
            idx = i;
            break;
        }
    }
    
    if (idx <= 0) {
        return find_up_leaf(parent);
    }
    
    return find_rightmost_leaf(parent->children[idx - 1]);
}

static math_disp_node_t *find_down_leaf(math_disp_node_t *node)
{
    if (!node) return NULL;
    
    if (node->child_count > 0) {
        return find_leftmost_leaf(node->children[0]);
    }
    
    math_disp_node_t *parent = node->parent;
    while (parent) {
        for (int i = 0; i < parent->child_count; i++) {
            if (parent->children[i] == node) {
                if (i + 1 < parent->child_count) {
                    return find_leftmost_leaf(parent->children[i + 1]);
                }
                node = parent;
                parent = parent->parent;
                break;
            }
        }
    }
    return NULL;
}

/* ========================================================================
 * Cursor movement
 * ======================================================================== */

void math_disp_cursor_home(math_disp_expr_t *expr)
{
    if (!expr || !expr->root) return;
    expr->cursor.node = find_leftmost_leaf(expr->root);
    expr->cursor.child_idx = -1;
    expr->cursor.offset = 0;
}

void math_disp_cursor_end(math_disp_expr_t *expr)
{
    if (!expr || !expr->root) return;
    expr->cursor.node = find_rightmost_leaf(expr->root);
    expr->cursor.child_idx = -1;
    expr->cursor.offset = 0;
}

void math_disp_cursor_left(math_disp_expr_t *expr)
{
    if (!expr || !expr->cursor.node) return;
    
    math_disp_node_t *prev = find_prev_leaf(expr->cursor.node);
    if (prev) {
        expr->cursor.node = prev;
        expr->cursor.child_idx = -1;
        expr->cursor.offset = 0;
    }
}

void math_disp_cursor_right(math_disp_expr_t *expr)
{
    if (!expr || !expr->cursor.node) return;
    
    math_disp_node_t *next = find_next_leaf(expr->cursor.node);
    if (next) {
        expr->cursor.node = next;
        expr->cursor.child_idx = -1;
        expr->cursor.offset = 0;
    }
}

void math_disp_cursor_up(math_disp_expr_t *expr)
{
    if (!expr || !expr->cursor.node) return;
    
    math_disp_node_t *up = find_up_leaf(expr->cursor.node);
    if (up) {
        expr->cursor.node = up;
        expr->cursor.child_idx = -1;
        expr->cursor.offset = 0;
    }
}

void math_disp_cursor_down(math_disp_expr_t *expr)
{
    if (!expr || !expr->cursor.node) return;
    
    math_disp_node_t *down = find_down_leaf(expr->cursor.node);
    if (down) {
        expr->cursor.node = down;
        expr->cursor.child_idx = -1;
        expr->cursor.offset = 0;
    }
}

void math_disp_cursor_word_left(math_disp_expr_t *expr)
{
    if (!expr || !expr->cursor.node) return;
    
    math_disp_node_t *prev = find_prev_leaf(expr->cursor.node);
    while (prev && (prev->type == MATH_DISP_NODE_ADD || prev->type == MATH_DISP_NODE_SUB ||
                    prev->type == MATH_DISP_NODE_MUL || prev->type == MATH_DISP_NODE_DIV ||
                    prev->type == MATH_DISP_NODE_POW)) {
        prev = find_prev_leaf(prev);
    }
    if (prev) {
        expr->cursor.node = prev;
        expr->cursor.child_idx = -1;
        expr->cursor.offset = 0;
    }
}

void math_disp_cursor_word_right(math_disp_expr_t *expr)
{
    if (!expr || !expr->cursor.node) return;
    
    math_disp_node_t *next = find_next_leaf(expr->cursor.node);
    while (next && (next->type == MATH_DISP_NODE_ADD || next->type == MATH_DISP_NODE_SUB ||
                    next->type == MATH_DISP_NODE_MUL || next->type == MATH_DISP_NODE_DIV ||
                    next->type == MATH_DISP_NODE_POW)) {
        next = find_next_leaf(next);
    }
    if (next) {
        expr->cursor.node = next;
        expr->cursor.child_idx = -1;
        expr->cursor.offset = 0;
    }
}

/* ========================================================================
 * Selection
 * ======================================================================== */

void math_disp_selection_clear(math_disp_expr_t *expr)
{
    if (expr) expr->selection.active = false;
}

void math_disp_selection_start(math_disp_expr_t *expr)
{
    if (!expr || !expr->cursor.node) return;
    expr->selection.start_node = expr->cursor.node;
    expr->selection.start_child = expr->cursor.child_idx;
    expr->selection.start_offset = expr->cursor.offset;
    expr->selection.active = true;
}

void math_disp_selection_end(math_disp_expr_t *expr)
{
    if (!expr || !expr->cursor.node) return;
    if (!expr->selection.active) return;
    
    expr->selection.end_node = expr->cursor.node;
    expr->selection.end_child = expr->cursor.child_idx;
    expr->selection.end_offset = expr->cursor.offset;
    expr->selection.active = true;
}

bool math_disp_selection_active(const math_disp_expr_t *expr)
{
    return expr && expr->selection.active;
}

/* ========================================================================
 * Basic editing
 * ======================================================================== */

int math_disp_insert_char(math_disp_expr_t *expr, char ch)
{
    if (!expr || !expr->cursor.node) return -1;
    
    math_disp_node_t *node = expr->cursor.node;
    
    if (node->type != MATH_DISP_NODE_NUMBER && 
        node->type != MATH_DISP_NODE_VARIABLE &&
        node->type != MATH_DISP_NODE_TEXT) {
        return -1;
    }
    
    if (node->value.type == MATH_DISP_VAL_STRING && node->value.v.str) {
        int len = strlen(node->value.v.str);
        if (expr->cursor.offset < 0) expr->cursor.offset = len;
        if (expr->cursor.offset > len) expr->cursor.offset = len;
        
        char *new_str = realloc(node->value.v.str, len + 2);
        if (!new_str) return -1;
        
        memmove(new_str + expr->cursor.offset + 1, 
                new_str + expr->cursor.offset, len - expr->cursor.offset + 1);
        new_str[expr->cursor.offset] = ch;
        node->value.v.str = new_str;
        expr->cursor.offset++;
        math_disp_invalidate_layout(expr);
        return 0;
    }
    
    return -1;
}

int math_disp_insert_text(math_disp_expr_t *expr, const char *text)
{
    if (!expr || !expr->cursor.node || !text) return -1;
    
    for (int i = 0; text[i]; i++) {
        if (math_disp_insert_char(expr, text[i]) != 0) {
            return -1;
        }
    }
    return 0;
}

int math_disp_backspace(math_disp_expr_t *expr)
{
    if (!expr || !expr->cursor.node) return -1;
    
    math_disp_node_t *node = expr->cursor.node;
    
    if (node->value.type == MATH_DISP_VAL_STRING && node->value.v.str) {
        int len = strlen(node->value.v.str);
        if (expr->cursor.offset > 0) {
            memmove(node->value.v.str + expr->cursor.offset - 1,
                    node->value.v.str + expr->cursor.offset,
                    len - expr->cursor.offset + 1);
            expr->cursor.offset--;
            math_disp_invalidate_layout(expr);
            return 0;
        }
    }
    
    return -1;
}

int math_disp_delete(math_disp_expr_t *expr)
{
    if (!expr || !expr->cursor.node) return -1;
    
    math_disp_node_t *node = expr->cursor.node;
    
    if (node->value.type == MATH_DISP_VAL_STRING && node->value.v.str) {
        int len = strlen(node->value.v.str);
        if (expr->cursor.offset < len) {
            memmove(node->value.v.str + expr->cursor.offset,
                    node->value.v.str + expr->cursor.offset + 1,
                    len - expr->cursor.offset);
            math_disp_invalidate_layout(expr);
            return 0;
        }
    }
    
    return -1;
}

int math_disp_delete_selection(math_disp_expr_t *expr)
{
    if (!expr || !expr->selection.active) return -1;
    
    math_disp_selection_clear(expr);
    return 0;
}

/* ========================================================================
 * Math-specific insertions
 * ======================================================================== */

int math_disp_insert_fraction(math_disp_expr_t *expr)
{
    if (!expr || !expr->cursor.node) return -1;
    
    math_disp_node_t *cursor_node = expr->cursor.node;
    
    math_disp_node_t *frac = math_disp_node_new_binary(expr, MATH_DISP_NODE_FRAC,
                                                       cursor_node, NULL);
    if (!frac) return -1;
    
    math_disp_node_t *den = math_disp_node_new_number(expr, 1);
    if (!den) return -1;
    frac->children[1] = den;
    den->parent = frac;
    
    math_disp_node_t *parent = cursor_node->parent;
    if (parent) {
        for (int i = 0; i < parent->child_count; i++) {
            if (parent->children[i] == cursor_node) {
                parent->children[i] = frac;
                break;
            }
        }
    } else {
        expr->root = frac;
    }
    frac->parent = parent;
    cursor_node->parent = frac;
    
    expr->cursor.node = den;
    expr->cursor.child_idx = -1;
    expr->cursor.offset = 0;
    
    math_disp_invalidate_layout(expr);
    return 0;
}

int math_disp_insert_sqrt(math_disp_expr_t *expr)
{
    if (!expr || !expr->cursor.node) return -1;
    
    math_disp_node_t *cursor_node = expr->cursor.node;
    
    math_disp_node_t *sqrt = math_disp_node_new_unary(expr, MATH_DISP_NODE_SQRT, cursor_node);
    if (!sqrt) return -1;
    
    math_disp_node_t *parent = cursor_node->parent;
    if (parent) {
        for (int i = 0; i < parent->child_count; i++) {
            if (parent->children[i] == cursor_node) {
                parent->children[i] = sqrt;
                break;
            }
        }
    } else {
        expr->root = sqrt;
    }
    sqrt->parent = parent;
    cursor_node->parent = sqrt;
    
    expr->cursor.node = cursor_node;
    expr->cursor.child_idx = -1;
    expr->cursor.offset = 0;
    
    math_disp_invalidate_layout(expr);
    return 0;
}

int math_disp_insert_power(math_disp_expr_t *expr)
{
    if (!expr || !expr->cursor.node) return -1;
    
    math_disp_node_t *cursor_node = expr->cursor.node;
    
    math_disp_node_t *pow = math_disp_node_new_binary(expr, MATH_DISP_NODE_POW,
                                                      cursor_node, NULL);
    if (!pow) return -1;
    
    math_disp_node_t *exp = math_disp_node_new_number(expr, 2);
    if (!exp) return -1;
    pow->children[1] = exp;
    exp->parent = pow;
    
    math_disp_node_t *parent = cursor_node->parent;
    if (parent) {
        for (int i = 0; i < parent->child_count; i++) {
            if (parent->children[i] == cursor_node) {
                parent->children[i] = pow;
                break;
            }
        }
    } else {
        expr->root = pow;
    }
    pow->parent = parent;
    cursor_node->parent = pow;
    
    expr->cursor.node = exp;
    expr->cursor.child_idx = -1;
    expr->cursor.offset = 0;
    
    math_disp_invalidate_layout(expr);
    return 0;
}

int math_disp_insert_subscript(math_disp_expr_t *expr)
{
    if (!expr || !expr->cursor.node) return -1;
    
    math_disp_node_t *cursor_node = expr->cursor.node;
    
    math_disp_node_t *sub = math_disp_node_new_binary(expr, MATH_DISP_NODE_SUBSCRIPT,
                                                      cursor_node, NULL);
    if (!sub) return -1;
    
    math_disp_node_t *sub_val = math_disp_node_new_number(expr, 1);
    if (!sub_val) return -1;
    sub->children[1] = sub_val;
    sub_val->parent = sub;
    
    math_disp_node_t *parent = cursor_node->parent;
    if (parent) {
        for (int i = 0; i < parent->child_count; i++) {
            if (parent->children[i] == cursor_node) {
                parent->children[i] = sub;
                break;
            }
        }
    } else {
        expr->root = sub;
    }
    sub->parent = parent;
    cursor_node->parent = sub;
    
    expr->cursor.node = sub_val;
    expr->cursor.child_idx = -1;
    expr->cursor.offset = 0;
    
    math_disp_invalidate_layout(expr);
    return 0;
}

int math_disp_insert_parens(math_disp_expr_t *expr)
{
    if (!expr || !expr->cursor.node) return -1;
    
    math_disp_node_t *cursor_node = expr->cursor.node;
    
    math_disp_node_t *paren = math_disp_node_new_unary(expr, MATH_DISP_NODE_PAREN, cursor_node);
    if (!paren) return -1;
    
    math_disp_node_t *parent = cursor_node->parent;
    if (parent) {
        for (int i = 0; i < parent->child_count; i++) {
            if (parent->children[i] == cursor_node) {
                parent->children[i] = paren;
                break;
            }
        }
    } else {
        expr->root = paren;
    }
    paren->parent = parent;
    cursor_node->parent = paren;
    
    math_disp_invalidate_layout(expr);
    return 0;
}

int math_disp_insert_function(math_disp_expr_t *expr, const char *name)
{
    if (!expr || !expr->cursor.node || !name) return -1;
    
    math_disp_node_type_t type = MATH_DISP_NODE_TEXT;
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
    else return -1;
    
    math_disp_node_t *cursor_node = expr->cursor.node;
    
    math_disp_node_t *func = math_disp_node_new_unary(expr, type, cursor_node);
    if (!func) return -1;
    
    math_disp_node_t *parent = cursor_node->parent;
    if (parent) {
        for (int i = 0; i < parent->child_count; i++) {
            if (parent->children[i] == cursor_node) {
                parent->children[i] = func;
                break;
            }
        }
    } else {
        expr->root = func;
    }
    func->parent = parent;
    cursor_node->parent = func;
    
    math_disp_invalidate_layout(expr);
    return 0;
}