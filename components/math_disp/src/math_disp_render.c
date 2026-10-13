/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file math_disp_render.c
 * @brief Natural Display rendering to ANSI (terminal) and LVGL (display).
 */

#include "math_disp.h"
#include "p4minishell_config.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>


/* ========================================================================
 * ANSI rendering helpers
 * ======================================================================== */

#define ANSI_BUF_FLUSH(buf, out, out_size, out_len) \
    do { \
        size_t len = strlen(buf); \
        if (*out_len + len >= out_size) return; \
        memcpy(out + *out_len, buf, len); \
        *out_len += len; \
        buf[0] = '\0'; \
    } while (0)

/* ========================================================================
 * ANSI escape sequences
 * ======================================================================== */

#define ANSI_RESET       "\x1b[0m"
#define ANSI_BOLD        "\x1b[1m"
#define ANSI_DIM         "\x1b[2m"
#define ANSI_UNDERLINE   "\x1b[4m"
#define ANSI_REVERSE     "\x1b[7m"
#define ANSI_FG_BLACK    "\x1b[30m"
#define ANSI_FG_RED      "\x1b[31m"
#define ANSI_FG_GREEN    "\x1b[32m"
#define ANSI_FG_YELLOW   "\x1b[33m"
#define ANSI_FG_BLUE     "\x1b[34m"
#define ANSI_FG_MAGENTA  "\x1b[35m"
#define ANSI_FG_CYAN     "\x1b[36m"
#define ANSI_FG_WHITE    "\x1b[37m"
#define ANSI_FG_BRIGHT_BLACK  "\x1b[90m"
#define ANSI_FG_BRIGHT_RED    "\x1b[91m"
#define ANSI_FG_BRIGHT_GREEN  "\x1b[92m"
#define ANSI_FG_BRIGHT_YELLOW "\x1b[93m"
#define ANSI_FG_BRIGHT_BLUE   "\x1b[94m"
#define ANSI_FG_BRIGHT_MAGENTA "\x1b[95m"
#define ANSI_FG_BRIGHT_CYAN   "\x1b[96m"
#define ANSI_FG_BRIGHT_WHITE  "\x1b[97m"

#define ANSI_BG_BLACK   "\x1b[40m"
#define ANSI_BG_RED     "\x1b[41m"
#define ANSI_BG_GREEN   "\x1b[42m"
#define ANSI_BG_YELLOW  "\x1b[43m"
#define ANSI_BG_BLUE    "\x1b[44m"
#define ANSI_BG_MAGENTA "\x1b[45m"
#define ANSI_BG_CYAN    "\x1b[46m"
#define ANSI_BG_WHITE   "\x1b[47m"

#define ANSI_CURSOR_HOME     "\x1b[H"
#define ANSI_CURSOR_UP(n)    "\x1b[" #n "A"
#define ANSI_CURSOR_DOWN(n)  "\x1b[" #n "B"
#define ANSI_CURSOR_RIGHT(n) "\x1b[" #n "C"
#define ANSI_CURSOR_LEFT(n)  "\x1b[" #n "D"
#define ANSI_CURSOR_POS(y,x) "\x1b[" #y ";" #x "H"

#define ANSI_SAVE_CURSOR   "\x1b[s"
#define ANSI_RESTORE_CURSOR "\x1b[u"
#define ANSI_CLEAR_LINE    "\x1b[2K"
#define ANSI_CLEAR_SCREEN  "\x1b[2J"

/* Unicode box-drawing characters for fractions/radicals */
#define CHAR_HLINE       "\xe2\x94\x80"  /* ─ */
#define CHAR_VLINE       "\xe2\x94\x82"  /* │ */
#define CHAR_UL_CORNER   "\xe2\x94\x8c"  /* ┌ */
#define CHAR_UR_CORNER   "\xe2\x94\x90"  /* ┐ */
#define CHAR_LL_CORNER   "\xe2\x94\x94"  /* └ */
#define CHAR_LR_CORNER   "\xe2\x94\x98"  /* ┘ */
#define CHAR_T_DOWN      "\xe2\x94\xac"  /* ┬ */
#define CHAR_T_UP        "\xe2\x94\xb4"  /* ┴ */
#define CHAR_T_RIGHT     "\xe2\x94\x9c"  /* ├ */
#define CHAR_T_LEFT      "\xe2\x94\xa4"  /* ┤ */
#define CHAR_CROSS       "\xe2\x94\xbc"  /* ┼ */
#define CHAR_SQRT_HOOK   "\xe2\x88\x9a"  /* √ */

/* Fallback ASCII for non-UTF8 terminals */
static const char *char_hline = CHAR_HLINE;
static const char *char_vline = CHAR_VLINE;
static const char *char_sqrt = CHAR_SQRT_HOOK;

/* ========================================================================
 * Render buffer
 * ======================================================================== */

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
} render_buf_t;

static void rb_init(render_buf_t *rb, char *out, size_t out_size)
{
    rb->buf = (char *)malloc(4096);
    rb->cap = 4096;
    rb->len = 0;
    if (rb->buf) rb->buf[0] = '\0';
}

static void rb_free(render_buf_t *rb)
{
    free(rb->buf);
}

static void rb_append(render_buf_t *rb, const char *s)
{
    size_t len = strlen(s);
    if (rb->len + len + 1 >= rb->cap) {
        rb->cap *= 2;
        rb->buf = realloc(rb->buf, rb->cap);
    }
    memcpy(rb->buf + rb->len, s, len);
    rb->len += len;
    rb->buf[rb->len] = '\0';
}

static void rb_appendf(render_buf_t *rb, const char *fmt, ...)
{
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (len > 0) {
        rb_append(rb, tmp);
    }
}

static void rb_flush(render_buf_t *rb, char *out, size_t out_size, size_t *out_len)
{
    if (rb->len == 0) return;
    if (*out_len + rb->len >= out_size) return;
    memcpy(out + *out_len, rb->buf, rb->len);
    *out_len += rb->len;
    rb->len = 0;
    rb->buf[0] = '\0';
}

/* ========================================================================
 * Cursor positioning in ANSI
 * ======================================================================== */

static void rb_cursor_move(render_buf_t *rb, int row, int col)
{
    rb_appendf(rb, "\x1b[%d;%dH", row + 1, col + 1);
}

/* ========================================================================
 * Cell rendering helpers
 * ======================================================================== */

static void rb_hline(render_buf_t *rb, int row, int col, int width)
{
    rb_cursor_move(rb, row, col);
    for (int i = 0; i < width; i++) {
        rb_append(rb, char_hline);
    }
}

/* ========================================================================
 * Render node to ANSI
 * ======================================================================== */

static void render_node_ansi(math_disp_expr_t *expr, render_buf_t *rb, 
                             math_disp_node_t *node, int base_row, int base_col)
{
    if (!node) return;
    
    int row = base_row + node->layout.y;
    int col = base_col + node->layout.x;
    
    switch (node->type) {
        case MATH_DISP_NODE_NUMBER:
            if (node->value.type == MATH_DISP_VAL_INT64) {
                rb_cursor_move(rb, row, col);
                rb_appendf(rb, "%lld", node->value.v.i64);
            } else if (node->value.type == MATH_DISP_VAL_DOUBLE) {
                char buf[32];
                snprintf(buf, sizeof(buf), "%.*g", 10, node->value.v.d);
                rb_cursor_move(rb, row, col);
                rb_append(rb, buf);
            }
            break;
            
        case MATH_DISP_NODE_VARIABLE:
        case MATH_DISP_NODE_SYMBOL:
            if (node->value.type == MATH_DISP_VAL_STRING && node->value.v.str) {
                rb_cursor_move(rb, row, col);
                rb_append(rb, node->value.v.str);
            }
            break;
            
        case MATH_DISP_NODE_ADD:
        case MATH_DISP_NODE_SUB: {
            math_disp_node_t *left = node->children[0];
            if (left) render_node_ansi(NULL, rb, left, base_row, base_col);
            
            int op_col = base_col + node->children[0]->layout.w + 1;
            rb_cursor_move(rb, base_row + node->children[0]->layout.baseline, op_col);
            rb_append(rb, node->type == MATH_DISP_NODE_ADD ? "+" : "-");
            
            int right_col = op_col + 2;
            if (node->children[1]) render_node_ansi(NULL, rb, node->children[1], base_row, right_col);
            break;
        }
            
        case MATH_DISP_NODE_FRAC: {
            int bar_row = base_row + node->children[0]->layout.h;
            int bar_col = base_col + 1;
            int bar_w = (node->children[0]->layout.w > node->children[1]->layout.w)
                        ? node->children[0]->layout.w : node->children[1]->layout.w;
            
            /* Render numerator (centered) */
            int num_col = base_col + 1 + (node->layout.w - node->children[0]->layout.w) / 2;
            if (node->children[0]) render_node_ansi(NULL, rb, node->children[0], base_row, num_col);
            
            /* Fraction bar: spans the wider of numerator/denominator. */
            rb_hline(rb, bar_row, bar_col, bar_w);
            
            /* Render denominator (centered) */
            int den_col = base_col + 1 + (node->layout.w - node->children[1]->layout.w) / 2;
            if (node->children[1]) render_node_ansi(NULL, rb, node->children[1], bar_row + 1, den_col);
            break;
        }
            
        case MATH_DISP_NODE_SQRT: {
            math_disp_node_t *rad = node->children[0];
            if (!rad) break;
            
            int hook_col = base_col;
            int bar_w = rad->layout.w + 2;
            int rad_row = base_row + 1;
            
            /* Sqrt hook */
            rb_cursor_move(rb, base_row, hook_col);
            rb_append(rb, char_sqrt);
            rb_cursor_move(rb, base_row + 1, hook_col);
            rb_append(rb, char_vline);
            
            /* Top bar */
            for (int i = 0; i < rad->layout.w + 2; i++) {
                rb_cursor_move(rb, base_row, base_col + 2 + i);
                rb_append(rb, char_hline);
            }
            
            /* Radicand */
            if (rad) render_node_ansi(NULL, rb, rad, rad_row, base_col + 2);
            
            /* Bottom bar */
            for (int i = 0; i < bar_w; i++) {
                rb_cursor_move(rb, rad_row + rad->layout.h, base_col + 2 + i);
                rb_append(rb, char_hline);
            }
            break;
        }
            
        default:
            break;
    }
}

/* ========================================================================
 * Main ANSI render entry point
 * ======================================================================== */

void math_disp_render_ansi(const math_disp_expr_t *expr, char *out, 
                           size_t out_size, size_t *out_len)
{
    if (!expr || !out || out_size == 0 || !out_len) return;
    
    *out_len = 0;
    if (!expr || !expr->root) return;
    
    render_buf_t rb;
    rb_init(&rb, out, out_size);
    
    /* Clear screen and home cursor */
    rb_append(&rb, ANSI_CLEAR_SCREEN);
    rb_append(&rb, ANSI_CURSOR_HOME);
    
    /* Render root node */
    render_node_ansi((math_disp_expr_t *)expr, &rb, expr->root, 0, 0);
    
    /* Position cursor at end */
    const math_disp_layout_t *layout = math_disp_get_layout(expr);
    if (layout) {
        rb_cursor_move(&rb, layout->height + 1, 0);
    }
    
    rb_flush(&rb, out, out_size, out_len);
    rb_free(&rb);
}

/* Render just the cursor position */
void math_disp_render_ansi_cursor(const math_disp_expr_t *expr, char *out, size_t out_size)
{
    if (!expr || !out) return;
    
    size_t len = 0;
    render_buf_t rb;
    rb_init(&rb, out, out_size);
    
    const math_disp_layout_t *layout = math_disp_get_layout(expr);
    if (layout) {
        rb_cursor_move(&rb, layout->cursor_y, layout->cursor_x);
    }
    
    rb_flush(&rb, out, out_size, &len);
    rb_free(&rb);
}

/* LVGL rendering - stub for now */
#ifdef CONFIG_LVGL
void math_disp_render_lvgl(const math_disp_expr_t *expr, void *canvas,
                           int x, int y, int max_w, int max_h)
{
    /* TODO: Implement LVGL canvas rendering */
}
#endif
