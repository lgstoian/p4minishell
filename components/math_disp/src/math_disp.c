/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file math_disp.c
 * @brief Natural Display math expression engine - core implementation.
 */

#include "math_disp.h"
#include "calc.h"
#include "p4minishell_config.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>

/* ========================================================================
 * Pool allocation
 * ======================================================================== */

math_disp_node_t *math_disp_pool_alloc(math_disp_expr_t *expr)
{
    if (expr->pool_used >= expr->max_nodes) {
        return NULL;
    }
    math_disp_node_t *node = &expr->node_pool[expr->pool_used++];
    memset(node, 0, sizeof(math_disp_node_t));
    return node;
}

static void math_disp_pool_reset(math_disp_expr_t *expr)
{
    expr->pool_used = 0;
}

/* ========================================================================
 * Expression lifecycle
 * ======================================================================== */

math_disp_expr_t *math_disp_expr_new(void)
{
    math_disp_expr_t *expr = heap_caps_calloc(1, sizeof(math_disp_expr_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!expr) return NULL;
    
    expr->max_nodes = MATH_DISP_MAX_NODES;
    expr->use_calc_eval = true;
    math_disp_pool_reset(expr);
    
    return expr;
}

void math_disp_expr_free(math_disp_expr_t *expr)
{
    if (!expr) return;
    
    /* Free any allocated strings in node values */
    for (int i = 0; i < expr->pool_used; i++) {
        math_disp_node_t *node = &expr->node_pool[i];
        if (node->value.type == MATH_DISP_VAL_STRING && node->value.v.str) {
            free(node->value.v.str);
        }
    }
    
    free(expr);
}

void math_disp_expr_clear(math_disp_expr_t *expr)
{
    if (!expr) return;
    
    for (int i = 0; i < expr->pool_used; i++) {
        math_disp_node_t *node = &expr->node_pool[i];
        if (node->value.type == MATH_DISP_VAL_STRING && node->value.v.str) {
            free(node->value.v.str);
            node->value.v.str = NULL;
        }
    }
    
    math_disp_pool_reset(expr);
    expr->root = NULL;
    expr->node_count = 0;
    expr->cursor.node = NULL;
    expr->cursor.child_idx = -1;
    expr->cursor.offset = 0;
    expr->selection.active = false;
    expr->layout_valid = false;
    expr->error_msg[0] = '\0';
}

/* ========================================================================
 * Node creation helpers
 * ======================================================================== */

static math_disp_node_t *math_disp_node_new(math_disp_expr_t *expr, 
                                             math_disp_node_type_t type)
{
    math_disp_node_t *node = math_disp_pool_alloc(expr);
    if (!node) return NULL;
    
    node->type = type;
    node->child_count = 0;
    node->parent = NULL;
    node->layout.x = node->layout.y = 0;
    node->layout.w = node->layout.h = 0;
    node->layout.baseline = 0;
    node->cursor.has_cursor = false;
    node->cursor.cursor_child = -1;
    node->cursor.cursor_pos = 0;
    node->flags = 0;
    node->value.type = MATH_DISP_VAL_NONE;
    
    if (expr->node_count < expr->max_nodes) {
        expr->nodes[expr->node_count++] = *node;  /* Copy to linear array for iteration */
    }
    
    return node;
}

math_disp_node_t *math_disp_node_new_number(math_disp_expr_t *expr, double val)
{
    math_disp_node_t *node = math_disp_node_new(expr, MATH_DISP_NODE_NUMBER);
    if (!node) return NULL;
    node->value.type = MATH_DISP_VAL_DOUBLE;
    node->value.v.d = val;
    return node;
}

math_disp_node_t *math_disp_node_new_int(math_disp_expr_t *expr, int64_t val)
{
    math_disp_node_t *node = math_disp_pool_alloc(expr);
    if (!node) return NULL;
    memset(node, 0, sizeof(math_disp_node_t));
    node->type = MATH_DISP_NODE_NUMBER;
    node->value.type = MATH_DISP_VAL_INT64;
    node->value.v.i64 = val;
    return node;
}

math_disp_node_t *math_disp_node_new_variable(math_disp_expr_t *expr, const char *name)
{
    math_disp_node_t *node = math_disp_pool_alloc(expr);
    if (!node) return NULL;
    memset(node, 0, sizeof(math_disp_node_t));
    node->type = MATH_DISP_NODE_VARIABLE;
    node->value.type = MATH_DISP_VAL_STRING;
    node->value.v.str = strdup(name);
    return node;
}

math_disp_node_t *math_disp_node_new_binary(math_disp_expr_t *expr,
                                                    math_disp_node_type_t type,
                                                    math_disp_node_t *left,
                                                    math_disp_node_t *right)
{
    math_disp_node_t *node = math_disp_pool_alloc(expr);
    if (!node) return NULL;
    memset(node, 0, sizeof(math_disp_node_t));
    node->type = type;
    node->child_count = 2;
    node->children[0] = left;
    node->children[1] = right;
    if (left) left->parent = node;
    if (right) right->parent = node;
    return node;
}

math_disp_node_t *math_disp_node_new_unary(math_disp_expr_t *expr,
                                                   math_disp_node_type_t type,
                                                   math_disp_node_t *child)
{
    math_disp_node_t *node = math_disp_pool_alloc(expr);
    if (!node) return NULL;
    memset(node, 0, sizeof(math_disp_node_t));
    node->type = type;
    node->child_count = 1;
    node->children[0] = child;
    if (child) child->parent = node;
    return node;
}

/* ========================================================================
 * Layout grid cell types (forward declare for rendering)
 * ======================================================================== */

/* The layout grid is defined in math_disp.h as math_disp_layout_t */


/* ========================================================================
 * Text serialization and evaluation
 * ========================================================================
 * math_disp_evaluate() is declared in math_disp.h ("delegates to calc
 * engine") and math_disp_to_string() alongside it, but neither had an
 * implementation, so the `math` verb failed to link. The tree is walked once
 * into a calc-compatible expression and handed to the ONE evaluator
 * (calc_evaluate); there is no second parser or evaluator here.
 */

/** Worst-case serialized expression length held on the stack. */
#define MATH_DISP_EVAL_TEXT_BYTES 512

/* Mutual recursion: md_text_children() recurses into the node walker. */
static void math_disp_node_text(const math_disp_node_t *node, char *buf,
                                size_t buf_size, size_t *pos, bool *ok);

/** Append formatted text, always leaving @p buf NUL-terminated. */
static void md_text_append(char *buf, size_t buf_size, size_t *pos,
                           const char *fmt, ...)
{
    va_list ap;
    int n;

    if (*pos >= buf_size) {
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(buf + *pos, buf_size - *pos, fmt, ap);
    va_end(ap);
    if (n > 0) {
        *pos += (size_t)n;
    }
    if (*pos >= buf_size) {
        *pos = buf_size - 1;
        buf[buf_size - 1] = '\0';
    }
}

/** Emit every child as a comma-separated argument list. */
static void md_text_children(const math_disp_node_t *node, char *buf,
                             size_t buf_size, size_t *pos, bool *ok)
{
    uint8_t i;

    for (i = 0; i < node->child_count; i++) {
        if (i > 0) {
            md_text_append(buf, buf_size, pos, ", ");
        }
        math_disp_node_text(node->children[i], buf, buf_size, pos, ok);
    }
}

/** Recursive best-effort serialization. Sets *ok = false for constructs the
 *  numeric evaluator cannot express (matrices, integrals, subscript, ...). */
static void math_disp_node_text(const math_disp_node_t *node, char *buf,
                                size_t buf_size, size_t *pos, bool *ok)
{
    if (node == NULL || !*ok) {
        return;
    }

    switch (node->type) {
    case MATH_DISP_NODE_NUMBER:
        if (node->value.type == MATH_DISP_VAL_RATIONAL) {
            md_text_append(buf, buf_size, pos, "(%lld/%lld)",
                           (long long)node->value.v.rat.num,
                           (long long)node->value.v.rat.den);
        } else if (node->value.type == MATH_DISP_VAL_INT64) {
            md_text_append(buf, buf_size, pos, "%lld",
                           (long long)node->value.v.i64);
        } else {
            md_text_append(buf, buf_size, pos, "%.17g", node->value.v.d);
        }
        break;

    case MATH_DISP_NODE_VARIABLE:
    case MATH_DISP_NODE_SYMBOL:
        md_text_append(buf, buf_size, pos, "%s",
                       node->value.v.str != NULL ? node->value.v.str : "0");
        break;

    case MATH_DISP_NODE_TEXT:
        /* An annotation has no numeric meaning; treat it as unsupported. */
        *ok = false;
        break;

    case MATH_DISP_NODE_NEGATE:
        md_text_append(buf, buf_size, pos, "-(");
        md_text_children(node, buf, buf_size, pos, ok);
        md_text_append(buf, buf_size, pos, ")");
        break;

    case MATH_DISP_NODE_PAREN:
        md_text_append(buf, buf_size, pos, "(");
        md_text_children(node, buf, buf_size, pos, ok);
        md_text_append(buf, buf_size, pos, ")");
        break;

    case MATH_DISP_NODE_SQRT:
    case MATH_DISP_NODE_CBRT:
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
    case MATH_DISP_NODE_FACTORIAL: {
        static const char *const NAMES[] = {
            "sqrt", "cbrt", "sin", "cos", "tan", "asin", "acos",
            "atan", "log", "ln", "exp", "abs",
        };
        switch (node->type) {
        case MATH_DISP_NODE_SQRT:      md_text_append(buf, buf_size, pos, "%s(", NAMES[0]); break;
        case MATH_DISP_NODE_CBRT:      md_text_append(buf, buf_size, pos, "%s(", NAMES[1]); break;
        case MATH_DISP_NODE_SIN:       md_text_append(buf, buf_size, pos, "%s(", NAMES[2]); break;
        case MATH_DISP_NODE_COS:       md_text_append(buf, buf_size, pos, "%s(", NAMES[3]); break;
        case MATH_DISP_NODE_TAN:       md_text_append(buf, buf_size, pos, "%s(", NAMES[4]); break;
        case MATH_DISP_NODE_ASIN:      md_text_append(buf, buf_size, pos, "%s(", NAMES[5]); break;
        case MATH_DISP_NODE_ACOS:      md_text_append(buf, buf_size, pos, "%s(", NAMES[6]); break;
        case MATH_DISP_NODE_ATAN:      md_text_append(buf, buf_size, pos, "%s(", NAMES[7]); break;
        case MATH_DISP_NODE_LOG:       md_text_append(buf, buf_size, pos, "%s(", NAMES[8]); break;
        case MATH_DISP_NODE_LN:        md_text_append(buf, buf_size, pos, "%s(", NAMES[9]); break;
        case MATH_DISP_NODE_EXP:       md_text_append(buf, buf_size, pos, "%s(", NAMES[10]); break;
        case MATH_DISP_NODE_ABS:       md_text_append(buf, buf_size, pos, "%s(", NAMES[11]); break;
        default:                       md_text_append(buf, buf_size, pos, "fac("); break;
        }
        md_text_children(node, buf, buf_size, pos, ok);
        md_text_append(buf, buf_size, pos, ")");
        break;
    }

    case MATH_DISP_NODE_ROOT:
        /* root(index, radicand) -> pow(radicand, 1/index) is not expressible
         * directly; the calc engine's root-free form is used instead. */
        md_text_append(buf, buf_size, pos, "(");
        if (node->child_count >= 2) {
            math_disp_node_text(node->children[1], buf, buf_size, pos, ok);
            md_text_append(buf, buf_size, pos, ")^(1/(");
            math_disp_node_text(node->children[0], buf, buf_size, pos, ok);
            md_text_append(buf, buf_size, pos, "))");
        } else if (node->child_count == 1) {
            static const char *const ONE = "1";
            (void)ONE;
            math_disp_node_text(node->children[0], buf, buf_size, pos, ok);
            md_text_append(buf, buf_size, pos, ")^(1/3)");
        } else {
            *ok = false;
        }
        break;

    case MATH_DISP_NODE_ADD:
    case MATH_DISP_NODE_SUB:
    case MATH_DISP_NODE_MUL:
    case MATH_DISP_NODE_DIV:
    case MATH_DISP_NODE_FRAC:
    case MATH_DISP_NODE_POW: {
        static const char *const OPS[] = { "+", "-", "*", "/", "/", "^" };
        const char *op = "?";

        switch (node->type) {
        case MATH_DISP_NODE_ADD:  op = OPS[0]; break;
        case MATH_DISP_NODE_SUB:  op = OPS[1]; break;
        case MATH_DISP_NODE_MUL:  op = OPS[2]; break;
        case MATH_DISP_NODE_DIV:  op = OPS[3]; break;
        case MATH_DISP_NODE_FRAC: op = OPS[4]; break;
        case MATH_DISP_NODE_POW:  op = OPS[5]; break;
        default: break;
        }
        md_text_append(buf, buf_size, pos, "(");
        if (node->child_count >= 2) {
            math_disp_node_text(node->children[0], buf, buf_size, pos, ok);
            md_text_append(buf, buf_size, pos, "%s", op);
            math_disp_node_text(node->children[1], buf, buf_size, pos, ok);
        } else if (node->child_count == 1) {
            math_disp_node_text(node->children[0], buf, buf_size, pos, ok);
        } else {
            *ok = false;
        }
        md_text_append(buf, buf_size, pos, ")");
        break;
    }

    default:
        /* Subscript, sums, products, integrals, derivatives, limits,
         * matrices, vectors and sequences have no flat numeric form. */
        *ok = false;
        break;
    }
}

char *math_disp_to_string(const math_disp_expr_t *expr, char *buf, size_t buf_size)
{
    size_t pos = 0;
    bool ok = true;

    if (buf == NULL || buf_size == 0) {
        return NULL;
    }
    buf[0] = '\0';
    if (expr != NULL && expr->root != NULL) {
        math_disp_node_text(expr->root, buf, buf_size, &pos, &ok);
    }
    (void)ok;   /* best-effort: callers that need strictness use evaluate(). */
    return buf;
}

bool math_disp_evaluate(const math_disp_expr_t *expr, double *out_result,
                        const char **error_out)
{
    char text[MATH_DISP_EVAL_TEXT_BYTES];
    size_t pos = 0;
    bool ok = true;
    calc_value_t value;
    const char *calc_error = NULL;

    if (expr == NULL || out_result == NULL) {
        return false;
    }
    if (error_out != NULL) {
        *error_out = NULL;
    }

    text[0] = '\0';
    if (expr->root != NULL) {
        math_disp_node_text(expr->root, text, sizeof(text), &pos, &ok);
    }
    if (!ok) {
        if (error_out != NULL) {
            *error_out = "expression contains a construct the evaluator cannot reduce";
        }
        return false;
    }
    if (text[0] == '\0') {
        return false;
    }

    /* The ONE evaluator lives in components/batch/calc.c. */
    if (!calc_evaluate(text, &value, &calc_error)) {
        if (error_out != NULL) {
            *error_out = calc_error;
        }
        return false;
    }
    *out_result = value.num;
    return true;
}
