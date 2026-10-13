/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file cas.h
 * @brief Computer Algebra System (CAS) public API.
 *
 * Minimal symbolic math engine providing:
 *  - Expression parsing and pretty printing
 *  - Simplification (algebraic, trigonometric)
 *  - Differentiation
 *  - Integration (indefinite/definite)
 *  - Equation solving
 *  - Series expansion
 *  - Expression evaluation (numeric)
 *
 * This is a minimal implementation focused on the core needs of the P4MiniShell
 * calculator. It uses a custom expression tree representation and does not
 * depend on external CAS libraries.
 */

#ifndef CAS_H
#define CAS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Configuration
 * ======================================================================== */

#ifndef CAS_MAX_EXPR_NODES
#define CAS_MAX_EXPR_NODES       2048
#endif

#ifndef CAS_MAX_EXPR_DEPTH
#define CAS_MAX_EXPR_DEPTH       64
#endif

#ifndef CAS_MAX_STR_LEN
#define CAS_MAX_STR_LEN          256
#endif

/* ========================================================================
 * Expression Node Types
 * ======================================================================== */

typedef enum {
    /* Leaf nodes */
    CAS_NODE_NUMBER,       /* Numeric constant */
    CAS_NODE_VARIABLE,     /* Variable (x, y, etc.) */
    CAS_NODE_SYMBOL,       /* Special symbol (pi, e, infinity) */
    CAS_NODE_RATIONAL,     /* Exact rational number */

    /* Unary operators */
    CAS_NODE_NEGATE,       /* -x */
    CAS_NODE_SQRT,         /* sqrt(x) */
    CAS_NODE_SIN,          /* sin(x) */
    CAS_NODE_COS,          /* cos(x) */
    CAS_NODE_TAN,          /* tan(x) */
    CAS_NODE_ASIN,         /* asin(x) */
    CAS_NODE_ACOS,         /* acos(x) */
    CAS_NODE_ATAN,         /* atan(x) */
    CAS_NODE_SINH,         /* sinh(x) */
    CAS_NODE_COSH,         /* cosh(x) */
    CAS_NODE_TANH,         /* tanh(x) */
    CAS_NODE_LOG,          /* log(x) */
    CAS_NODE_LN,           /* ln(x) */
    CAS_NODE_EXP,          /* exp(x) */
    CAS_NODE_ABS,          /* |x| */
    CAS_NODE_FACTORIAL,    /* x! */

    /* Binary operators */
    CAS_NODE_ADD,          /* x + y */
    CAS_NODE_SUB,          /* x - y */
    CAS_NODE_MUL,          /* x * y */
    CAS_NODE_DIV,          /* x / y */
    CAS_NODE_POW,          /* x ^ y */

    /* Special constructs */
    CAS_NODE_FRAC,         /* Fraction: numerator/denominator */
    CAS_NODE_ROOT,         /* nth root */
    CAS_NODE_DERIVATIVE,   /* d/dx */
    CAS_NODE_INTEGRAL,     /* Integral */
    CAS_NODE_LIMIT,        /* Limit */
    CAS_NODE_SUM,          /* Summation */
    CAS_NODE_PRODUCT,      /* Product */
    CAS_NODE_SERIES,       /* Series expansion */
    CAS_NODE_TEXT,         /* Text/string literal */

    /* Relations (for solving) */
    CAS_NODE_EQ,           /* x = y */
    CAS_NODE_NEQ,          /* x != y */
    CAS_NODE_LT,           /* x < y */
    CAS_NODE_LE,           /* x <= y */
    CAS_NODE_GT,           /* x > y */
    CAS_NODE_GE,           /* x >= y */

    /* Special */
    CAS_NODE_SEQUENCE,     /* Sequence of expressions */
    CAS_NODE_ASSIGNMENT,   /* Variable assignment */
} cas_node_type_t;

/* ========================================================================
 * Expression Node Value
 * ======================================================================== */

typedef enum {
    CAS_VAL_NONE,
    CAS_VAL_INT64,
    CAS_VAL_DOUBLE,
    CAS_VAL_RATIONAL,      /* Exact rational: num/den */
    CAS_VAL_COMPLEX,       /* Complex: re + im*i */
    CAS_VAL_STRING,        /* Variable name or symbol name */
} cas_value_type_t;

typedef struct {
    cas_value_type_t type;
    union {
        int64_t i64;
        double d;
        struct { int64_t num; int64_t den; } rat;
        struct { double re, im; } complex;
        char *str;
    } v;
} cas_value_t;

/* ========================================================================
 * Expression Node
 * ======================================================================== */

typedef struct cas_node_s cas_node_t;

struct cas_node_s {
    cas_node_type_t type;
    cas_node_t *parent;
    cas_node_t *children[8];
    uint8_t child_count;

    /* For leaf nodes */
    cas_value_t value;

    /* Layout info (optional, for pretty printing) */
    struct {
        int16_t x, y;
        uint16_t w, h;
        uint16_t baseline;
    } layout;

    /* Cursor state (for interactive editing) */
    struct {
        bool has_cursor;
        int cursor_child;
        int cursor_pos;
    } cursor;

    /* Flags */
    uint8_t flags;
#define CAS_NODE_FLAG_SIMPLIFIED    (1 << 0)
#define CAS_NODE_FLAG_EVALUATED     (1 << 1)
#define CAS_NODE_FLAG_ERROR         (1 << 2)
};

/* ========================================================================
 * Expression Handle
 * ======================================================================== */

typedef struct cas_expr_s cas_expr_t;

struct cas_expr_s {
    cas_node_t *root;
    cas_node_t nodes[2048];
    int node_count;
    int max_nodes;

    /* Pool allocator */
    cas_node_t node_pool[2048];
    int pool_used;

    /* Error info */
    char error_msg[256];

    /* Configuration */
    bool use_rational;       /* Use exact rational arithmetic */
    bool use_complex;        /* Enable complex numbers */
};

/* ========================================================================
 * Public API
 * ======================================================================== */

/* --- Expression lifecycle --- */

cas_expr_t *cas_expr_new(void);
void cas_expr_free(cas_expr_t *expr);
void cas_expr_clear(cas_expr_t *expr);

/* --- Parsing --- */

int cas_parse(cas_expr_t *expr, const char *input, char **error_out);
int cas_parse_at(cas_expr_t *expr, const char *input, cas_node_t **out_node);

/* Parse using existing calc engine for evaluation capability */
int cas_parse_with_calc(cas_expr_t *expr, const char *input);

/* --- Simplification --- */

cas_node_t *cas_simplify(cas_expr_t *expr, cas_node_t *node);
cas_node_t *cas_simplify_algebraic(cas_expr_t *expr, cas_node_t *node);
cas_node_t *cas_simplify_trig(cas_expr_t *expr, cas_node_t *node);
cas_node_t *cas_simplify_rational(cas_expr_t *expr, cas_node_t *node);
cas_node_t *cas_expand(cas_expr_t *expr, cas_node_t *node);
cas_node_t *cas_factor(cas_expr_t *expr, cas_node_t *node);
cas_node_t *cas_collect(cas_expr_t *expr, cas_node_t *node, const char *var);

/* --- Differentiation --- */

cas_node_t *cas_diff(cas_expr_t *expr, cas_node_t *node, const char *var);
cas_node_t *cas_diff_partial(cas_expr_t *expr, cas_node_t *node, const char *var, int order);

/* --- Integration --- */

cas_node_t *cas_integrate(cas_expr_t *expr, cas_node_t *node, const char *var);
cas_node_t *cas_integrate_definite(cas_expr_t *expr, cas_node_t *node, const char *var, double a, double b);

/* --- Equation Solving --- */

cas_node_t *cas_solve(cas_expr_t *expr, cas_node_t *node, const char *var);
cas_node_t *cas_solve_polynomial(cas_expr_t *expr, cas_node_t *node, const char *var);
cas_node_t *cas_solve_linear(cas_expr_t *expr, cas_node_t *node, const char *var);
cas_node_t *cas_solve_nonlinear(cas_expr_t *expr, cas_node_t *node, const char *var, double guess);

/* --- Series Expansion --- */

cas_node_t *cas_series(cas_expr_t *expr, cas_node_t *node, const char *var, double point, int order);

/* --- Evaluation --- */

bool cas_eval_num(const cas_expr_t *expr, const cas_node_t *node, double *out);
cas_node_t *cas_eval_at(cas_expr_t *expr, cas_node_t *node, const char *var, double val);
void cas_node_substitute(cas_node_t *node, const char *var, double val);

/* --- Serialization --- */

char *cas_to_string(const cas_expr_t *expr, cas_node_t *node, bool latex);
int cas_from_string(cas_expr_t *expr, const char *str);

/* --- Utilities --- */

const char *cas_node_type_str(int type);
bool cas_node_is_leaf(const cas_node_t *node);
bool cas_node_is_binary(const cas_node_t *node);
bool cas_node_is_unary(const cas_node_t *node);
int cas_node_child_count(const cas_node_t *node);

cas_node_t *cas_find_node_at(cas_expr_t *expr, int x, int y);
cas_node_t *cas_get_node_at_cursor(const cas_expr_t *expr);

/* Value handling */
void cas_value_init_int(cas_value_t *v, int64_t val);
void cas_value_init_double(cas_value_t *v, double val);
void cas_value_init_rational(cas_value_t *v, int64_t num, int64_t den);
void cas_value_init_complex(cas_value_t *v, double re, double im);
void cas_value_init_string(cas_value_t *v, const char *str);
void cas_value_free(cas_value_t *v);
char *cas_value_to_string(const cas_value_t *v, char *buf, size_t buf_size);

/* Rational arithmetic */
void cas_rational_reduce(int64_t *num, int64_t *den);
void cas_rational_add(int64_t n1, int64_t d1, int64_t n2, int64_t d2, int64_t *rn, int64_t *rd);
void cas_rational_sub(int64_t n1, int64_t d1, int64_t n2, int64_t d2, int64_t *rn, int64_t *rd);
void cas_rational_mul(int64_t n1, int64_t d1, int64_t n2, int64_t d2, int64_t *rn, int64_t *rd);
void cas_rational_div(int64_t n1, int64_t d1, int64_t n2, int64_t d2, int64_t *rn, int64_t *rd);

/* --- Node construction helpers (public) --- */

cas_node_t *cas_node_new_number(cas_expr_t *expr, double val);

/** Create an exact integer literal node (CAS_VAL_INT64). */
cas_node_t *cas_node_new_int(cas_expr_t *expr, int64_t val);
cas_node_t *cas_node_new_rational(cas_expr_t *expr, int64_t num, int64_t den);
cas_node_t *cas_node_new_variable(cas_expr_t *expr, const char *name);
cas_node_t *cas_node_new_text(cas_expr_t *expr, const char *text);
cas_node_t *cas_node_new_binary(cas_expr_t *expr, cas_node_type_t type, cas_node_t *left, cas_node_t *right);
cas_node_t *cas_node_new_unary(cas_expr_t *expr, cas_node_type_t type, cas_node_t *child);
cas_node_t *cas_node_new_function(cas_expr_t *expr, cas_node_type_t type, cas_node_t *arg);
cas_node_t *cas_node_copy(cas_expr_t *expr, const cas_node_t *src);

#ifdef __cplusplus
}
#endif

#endif /* CAS_H */