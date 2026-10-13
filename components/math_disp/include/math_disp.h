/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file math_disp.h
 * @brief Natural Display math expression layout/rendering/editing engine.
 *
 * This component provides Natural Display (2D math layout) capabilities:
 *  - Expression tree parsing from 1D input (leveraging existing calc parser)
 *  - 2D layout engine: fractions (vertical), radicals, exponents, subscripts
 *  - 2D cursor navigation through expression tree (up/down/left/right)
 *  - ANSI rendering for terminal (with proper box-drawing for fractions/radicals)
 *  - LVGL rendering for display
 *  - Editing operations (insert, delete, replace, move cursor)
 *
 * Evaluation is delegated to the existing calc engine (components/batch/calc.c).
 * This component focuses on 2D layout, rendering, and editing.
 */

#ifndef MATH_DISP_H
#define MATH_DISP_H

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

#ifndef MATH_DISP_MAX_NODES
#define MATH_DISP_MAX_NODES       256
#endif

#ifndef MATH_DISP_MAX_DEPTH
#define MATH_DISP_MAX_DEPTH       32
#endif

#ifndef MATH_DISP_MAX_CHILDREN
#define MATH_DISP_MAX_CHILDREN    8
#endif

#ifndef MATH_DISP_MAX_LAYOUT_W
#define MATH_DISP_MAX_LAYOUT_W    160
#endif

#ifndef MATH_DISP_MAX_LAYOUT_H
#define MATH_DISP_MAX_LAYOUT_H    64
#endif

/* ========================================================================
 * Expression Node Types (Natural Display math constructs)
 * ======================================================================== */

typedef enum {
    /* Leaf nodes */
    MATH_DISP_NODE_NUMBER,       /* Numeric literal */
    MATH_DISP_NODE_VARIABLE,     /* Variable (x, y, etc.) */
    MATH_DISP_NODE_SYMBOL,       /* Constant symbol (pi, e, infinity) */

    /* Unary operators */
    MATH_DISP_NODE_NEGATE,       /* -x */
    MATH_DISP_NODE_SQRT,         /* sqrt(x) */
    MATH_DISP_NODE_CBRT,         /* cbrt(x) */
    MATH_DISP_NODE_ROOT,         /* nth root: root(n, x) - binary */
    MATH_DISP_NODE_SIN,          /* sin(x) */
    MATH_DISP_NODE_COS,          /* cos(x) */
    MATH_DISP_NODE_TAN,          /* tan(x) */
    MATH_DISP_NODE_ASIN,         /* asin(x) */
    MATH_DISP_NODE_ACOS,         /* acos(x) */
    MATH_DISP_NODE_ATAN,         /* atan(x) */
    MATH_DISP_NODE_LOG,          /* log(x) */
    MATH_DISP_NODE_LN,           /* ln(x) */
    MATH_DISP_NODE_EXP,          /* exp(x) */
    MATH_DISP_NODE_ABS,          /* |x| */
    MATH_DISP_NODE_FACTORIAL,    /* x! */
    MATH_DISP_NODE_PAREN,        /* (x) - grouping */

    /* Binary operators with special 2D layout */
    MATH_DISP_NODE_ADD,          /* x + y */
    MATH_DISP_NODE_SUB,          /* x - y */
    MATH_DISP_NODE_MUL,          /* x * y (implicit or explicit) */
    MATH_DISP_NODE_DIV,          /* x / y (inline division) */
    MATH_DISP_NODE_FRAC,         /* Fraction: numerator/denominator (2D stacked) */
    MATH_DISP_NODE_POW,          /* x ^ y (exponent - superscript) */
    MATH_DISP_NODE_SUBSCRIPT,    /* x _ y (subscript) */

    /* N-ary operators with special layout */
    MATH_DISP_NODE_SUM,          /* Sum: sum(i, start, end, expr) */
    MATH_DISP_NODE_PROD,         /* Product: prod(i, start, end, expr) */
    MATH_DISP_NODE_INTEGRAL,     /* Integral: int(var, lower, upper, expr) */
    MATH_DISP_NODE_DERIVATIVE,   /* Derivative: d/dx(expr) */
    MATH_DISP_NODE_LIMIT,        /* Limit: lim(x->a, expr) */

    /* Matrix/Vector */
    MATH_DISP_NODE_MATRIX,       /* Matrix: [[a,b],[c,d]] */
    MATH_DISP_NODE_VECTOR,       /* Vector: [a,b,c] */

    /* Special */
    MATH_DISP_NODE_SEQUENCE,     /* Sequence of expressions */
    MATH_DISP_NODE_TEXT,         /* Plain text annotation */
} math_disp_node_type_t;

/* ========================================================================
 * Node Value
 * ======================================================================== */

typedef enum {
    MATH_DISP_VAL_NONE,
    MATH_DISP_VAL_INT64,
    MATH_DISP_VAL_DOUBLE,
    MATH_DISP_VAL_RATIONAL,
    MATH_DISP_VAL_STRING,
} math_disp_value_type_t;

typedef struct {
    math_disp_value_type_t type;
    union {
        int64_t i64;
        double d;
        struct { int64_t num; int64_t den; } rat;
        char *str;
    } v;
} math_disp_value_t;

/* ========================================================================
 * Expression Node
 * ======================================================================== */

typedef struct math_disp_node_s math_disp_node_t;

struct math_disp_node_s {
    math_disp_node_type_t type;
    math_disp_node_t *parent;
    math_disp_node_t *children[8];
    uint8_t child_count;
    
    /* For leaf nodes */
    struct {
        int type;
        union {
            int64_t i64;
            double d;
            struct { int64_t num; int64_t den; } rat;
            char *str;
        } v;
    } value;
    
    /* Layout info (filled by layout engine) */
    struct {
        int16_t x, y;
        uint16_t w, h;
        uint16_t baseline;
    } layout;
    
    /* Cursor state within this node */
    struct {
        bool has_cursor;
        int cursor_child;
        int cursor_pos;
    } cursor;
    
    uint8_t flags;
#define MATH_DISP_NODE_FLAG_IMPLICIT_MUL   (1 << 0)
#define MATH_DISP_NODE_FLAG_SELECTED       (1 << 1)
#define MATH_DISP_NODE_FLAG_ERROR          (1 << 2)
};

/* ========================================================================
 * Expression Handle
 * ======================================================================== */

typedef struct math_disp_expr_s math_disp_expr_t;

struct math_disp_expr_s {
    math_disp_node_t *root;
    math_disp_node_t nodes[256];
    int node_count;
    int max_nodes;
    
    /* Cursor position */
    struct {
        struct math_disp_node_s *node;
        int child_idx;
        int offset;
    } cursor;
    
    /* Selection range */
    struct {
        struct math_disp_node_s *start_node;
        int start_child;
        int start_offset;
        struct math_disp_node_s *end_node;
        int end_child;
        int end_offset;
        bool active;
    } selection;
    
    /* Pool allocator */
    math_disp_node_t node_pool[256];
    int pool_used;
    
    /* Layout cache */
    bool layout_valid;
    
    /* Error info */
    char error_msg[128];
    
    /* Calc evaluator integration */
    bool use_calc_eval;
};

/* ========================================================================
 * Layout Grid (2D cell grid for rendering)
 * ======================================================================== */

typedef enum {
    MATH_DISP_CELL_EMPTY,
    MATH_DISP_CELL_CHAR,
    MATH_DISP_CELL_HLINE,
    MATH_DISP_CELL_VLINE,
    MATH_DISP_CELL_UL_CORNER,
    MATH_DISP_CELL_UR_CORNER,
    MATH_DISP_CELL_LL_CORNER,
    MATH_DISP_CELL_LR_CORNER,
    MATH_DISP_CELL_SQRT_HOOK,
} math_disp_cell_type_t;

typedef struct {
    int type;
    char ch;
    uint8_t attr;
} math_disp_cell_t;

typedef struct {
    struct {
        int type;
        char ch;
        uint8_t attr;
    } cells[64][160];
    int width;
    int height;
    int cursor_x, cursor_y;
} math_disp_layout_t;

/* NOTE: the recursive-descent parser state (math_disp_parser_t) is private to
 * math_disp_parse.c. It used to be declared here as well, which gave the same
 * typedef two different layouts (the parser's copy carries `last_node`); the
 * copy in the header has been removed so there is exactly one definition. */

/* ========================================================================
 * Public API
 * ======================================================================== */

/* --- Expression lifecycle --- */

typedef struct math_disp_expr_s math_disp_expr_t;
typedef struct math_disp_node_s math_disp_node_t;

math_disp_expr_t *math_disp_expr_new(void);
/* Node builders shared with math_disp_edit.c (single implementation in
 * math_disp.c; the editor must not grow its own constructors). */
/** Allocate one node from the expression's fixed pool. */
math_disp_node_t *math_disp_pool_alloc(math_disp_expr_t *expr);

math_disp_node_t *math_disp_node_new_number(math_disp_expr_t *expr, double val);
math_disp_node_t *math_disp_node_new_int(math_disp_expr_t *expr, int64_t val);
math_disp_node_t *math_disp_node_new_variable(math_disp_expr_t *expr, const char *name);
math_disp_node_t *math_disp_node_new_binary(math_disp_expr_t *expr,
                                            math_disp_node_type_t op,
                                            math_disp_node_t *lhs,
                                            math_disp_node_t *rhs);
math_disp_node_t *math_disp_node_new_unary(math_disp_expr_t *expr,
                                           math_disp_node_type_t op,
                                           math_disp_node_t *operand);
void math_disp_expr_free(math_disp_expr_t *expr);
void math_disp_expr_clear(math_disp_expr_t *expr);

/* --- Parsing (from 1D input string) --- */

int math_disp_parse(math_disp_expr_t *expr, const char *input);
int math_disp_parse_at(math_disp_expr_t *expr, const char *input, 
                        math_disp_node_t **out_node);

/* Parse using existing calc engine for evaluation capability */
int math_disp_parse_with_calc(math_disp_expr_t *expr, const char *input);

/* --- Cursor/Selection --- */

void math_disp_cursor_home(math_disp_expr_t *expr);
void math_disp_cursor_end(math_disp_expr_t *expr);
void math_disp_cursor_left(math_disp_expr_t *expr);
void math_disp_cursor_right(math_disp_expr_t *expr);
void math_disp_cursor_up(math_disp_expr_t *expr);
void math_disp_cursor_down(math_disp_expr_t *expr);
void math_disp_cursor_word_left(math_disp_expr_t *expr);
void math_disp_cursor_word_right(math_disp_expr_t *expr);

void math_disp_selection_clear(math_disp_expr_t *expr);
void math_disp_selection_start(math_disp_expr_t *expr);
void math_disp_selection_end(math_disp_expr_t *expr);
bool math_disp_selection_active(const math_disp_expr_t *expr);

/* --- Editing --- */

int math_disp_insert_char(math_disp_expr_t *expr, char ch);
int math_disp_insert_text(math_disp_expr_t *expr, const char *text);
int math_disp_backspace(math_disp_expr_t *expr);
int math_disp_delete(math_disp_expr_t *expr);
int math_disp_delete_selection(math_disp_expr_t *expr);

/* Math-specific insertions */
int math_disp_insert_fraction(math_disp_expr_t *expr);
int math_disp_insert_sqrt(math_disp_expr_t *expr);
int math_disp_insert_power(math_disp_expr_t *expr);
int math_disp_insert_subscript(math_disp_expr_t *expr);
int math_disp_insert_parens(math_disp_expr_t *expr);
int math_disp_insert_function(math_disp_expr_t *expr, const char *name);

/* --- Layout --- */

int math_disp_layout(math_disp_expr_t *expr);
void math_disp_invalidate_layout(math_disp_expr_t *expr);
const math_disp_layout_t *math_disp_get_layout(const math_disp_expr_t *expr);

/* --- Rendering (ANSI) --- */

void math_disp_render_ansi(const math_disp_expr_t *expr, char *out, 
                           size_t out_size, size_t *out_len);
void math_disp_render_ansi_cursor(const math_disp_expr_t *expr, 
                                   char *out, size_t out_size);

/* --- Rendering (LVGL) --- */
#ifdef CONFIG_LVGL
void math_disp_render_lvgl(const math_disp_expr_t *expr, void *canvas,
                           int x, int y, int max_w, int max_h);
#endif

/* --- Evaluation (delegates to calc engine) --- */

bool math_disp_evaluate(const math_disp_expr_t *expr, double *out_result,
                        const char **error_out);

/* --- Serialization --- */

char *math_disp_to_string(const math_disp_expr_t *expr, 
                          char *buf, size_t buf_size);
int math_disp_from_string(math_disp_expr_t *expr, const char *str);

/* --- Utilities --- */

const char *math_disp_node_type_str(int type);
bool math_disp_node_is_leaf(const math_disp_node_t *node);
bool math_disp_node_is_binary(const math_disp_node_t *node);
bool math_disp_node_is_unary(const math_disp_node_t *node);
int math_disp_node_child_count(const math_disp_node_t *node);

math_disp_node_t *math_disp_find_node_at(math_disp_expr_t *expr, 
                                         int x, int y);
math_disp_node_t *math_disp_get_node_at_cursor(const math_disp_expr_t *expr);

/* Value handling */
void math_disp_value_init_int(void *v, int64_t val);
void math_disp_value_init_double(void *v, double val);
void math_disp_value_init_rational(void *v, int64_t num, int64_t den);
void math_disp_value_init_string(void *v, const char *str);
void math_disp_value_free(void *v);
char *math_disp_value_to_string(const void *v, char *buf, size_t buf_size);

#ifdef __cplusplus
}
#endif

/* NOTE: math_disp_layout_t is defined once, with the other value types above.
 * A second copy used to follow this forward declaration; it conflicted with
 * the first and has been removed so there is exactly one definition. */

#endif /* MATH_DISP_H */
