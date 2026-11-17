/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file js_canvas.h
 * @brief Stripped-down HTML Canvas 2D core (WHATWG HTML Living Standard 4.12.5).
 *
 * The firmware has no GPU and the browser is a cell grid, so this is a small,
 * deterministic software rasterizer rather than a full Canvas implementation.
 * It keeps the spec's model where it matters: an RGBA backing store, the 2D
 * affine transform, source-over alpha compositing, save/restore, subpaths,
 * solid fill/stroke colours, rectangles, lines, arcs and 8x8 bitmap text.
 *
 * Deliberately not implemented (documented in documentation.md): images,
 * patterns, gradients, shadows, clipping paths, line joins/caps beyond square
 * ends, anti-aliasing, and sub-pixel text metrics. A page that uses them still
 * renders; those calls are no-ops, never crashes.
 *
 * The core is pure C: no QuickJS, no DOM, no LVGL, no USB. The JS bindings
 * live in js_canvas_js.c and the browser converts the RGBA snapshot to cells
 * through the same pixel-to-cell mapping images use.
 */
#ifndef P4MINISHELL_JS_CANVAS_H
#define P4MINISHELL_JS_CANVAS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Documented caps: a canvas is a PSRAM allocation and the browser is 80x25. */
#define JS_CANVAS_MAX_W       320
#define JS_CANVAS_MAX_H       200
#define JS_CANVAS_MAX_COUNT   2
#define JS_CANVAS_MAX_PATH    512
#define JS_CANVAS_SAVE_DEPTH  8

#include "css_color.h"

/** Canvas colours are the project RGBA type: there is exactly one CSS colour
 *  parser (components/markdown/css_color.c), shared by <canvas>, CSS and the
 *  TUI. This alias keeps the canvas API spelling stable. */
typedef css_rgba_t js_canvas_rgba_t;

typedef struct js_canvas js_canvas_t;

/** Create a canvas; width/height are clamped to the documented caps.
 *  @return NULL on allocation failure. */
js_canvas_t *js_canvas_new(int width, int height);
void js_canvas_free(js_canvas_t *canvas);

/** Resize and reset the bitmap and drawing state (HTML canvas width/height
 *  setters reset the context, per spec). Returns false on OOM (the old bitmap
 *  is kept). */
bool js_canvas_resize(js_canvas_t *canvas, int width, int height);

int js_canvas_width(const js_canvas_t *canvas);
int js_canvas_height(const js_canvas_t *canvas);
const uint8_t *js_canvas_pixels(const js_canvas_t *canvas);

/* Drawing state. Colours are straight (non-premultiplied) RGBA; alpha is the
 * canvas globalAlpha in 0..255. */
void js_canvas_set_fill(js_canvas_t *canvas, js_canvas_rgba_t color);
js_canvas_rgba_t js_canvas_get_fill(const js_canvas_t *canvas);
void js_canvas_set_stroke(js_canvas_t *canvas, js_canvas_rgba_t color);
js_canvas_rgba_t js_canvas_get_stroke(const js_canvas_t *canvas);
uint8_t js_canvas_get_global_alpha(const js_canvas_t *canvas);
float js_canvas_get_line_width(const js_canvas_t *canvas);
int js_canvas_get_font_scale(const js_canvas_t *canvas);
int js_canvas_get_text_align(const js_canvas_t *canvas);
int js_canvas_get_text_baseline(const js_canvas_t *canvas);
void js_canvas_set_global_alpha(js_canvas_t *canvas, uint8_t alpha);
void js_canvas_set_line_width(js_canvas_t *canvas, float width);
void js_canvas_set_font_scale(js_canvas_t *canvas, int scale);
void js_canvas_set_text_align(js_canvas_t *canvas, int align);      /* 0 left, 1 centre, 2 right */
void js_canvas_set_text_baseline(js_canvas_t *canvas, int baseline);/* 0 alphabetic, 1 top, 2 middle, 3 bottom */

void js_canvas_save(js_canvas_t *canvas);
void js_canvas_restore(js_canvas_t *canvas);

void js_canvas_translate(js_canvas_t *canvas, float tx, float ty);
void js_canvas_scale(js_canvas_t *canvas, float sx, float sy);
void js_canvas_rotate(js_canvas_t *canvas, float radians);
void js_canvas_set_transform(js_canvas_t *canvas, float a, float b, float c,
                             float d, float e, float f);
void js_canvas_reset_transform(js_canvas_t *canvas);

void js_canvas_clear(js_canvas_t *canvas);
void js_canvas_fill_rect(js_canvas_t *canvas, float x, float y, float w, float h);
void js_canvas_clear_rect(js_canvas_t *canvas, float x, float y, float w, float h);
void js_canvas_stroke_rect(js_canvas_t *canvas, float x, float y, float w, float h);

void js_canvas_begin_path(js_canvas_t *canvas);
void js_canvas_move_to(js_canvas_t *canvas, float x, float y);
void js_canvas_line_to(js_canvas_t *canvas, float x, float y);
void js_canvas_close_path(js_canvas_t *canvas);
void js_canvas_rect(js_canvas_t *canvas, float x, float y, float w, float h);
void js_canvas_arc(js_canvas_t *canvas, float cx, float cy, float radius,
                   float start, float end, bool counterclockwise);
void js_canvas_fill(js_canvas_t *canvas);
void js_canvas_stroke(js_canvas_t *canvas);

/** Draw UTF-8 text with the public-domain 8x8 CP437 font, scaled by the
 *  current font scale. Returns the advance width in pixels (before the
 *  transform). */
int js_canvas_fill_text(js_canvas_t *canvas, const char *text, float x, float y,
                        bool stroke);
int js_canvas_measure_text(const js_canvas_t *canvas, const char *text);

/** Downsample the RGBA bitmap into a max_w * max_h straight-RGBA buffer.
 *  Transparent pixels keep alpha 0. Returns false on bad args/OOM. */
bool js_canvas_snapshot_rgba(const js_canvas_t *canvas, int max_w, int max_h,
                             uint8_t *out_rgba, int *out_w, int *out_h);

/** Parse a CSS colour string (CSS Color 3 subset): #rgb, #rrggbb, #rrggbbaa,
 *  rgb()/rgba() with 0-255 or percentage components, `transparent`, and the
 *  basic named colours. Returns false for an unsupported value (the caller
 *  keeps the previous colour, matching the spec's "ignore invalid value"). */
bool js_canvas_parse_color(const char *text, js_canvas_rgba_t *out);

/** Canvas registry, keyed by DOM node (a page re-render reuses the same node
 *  pointers only until the tree is freed, so clear it on page load). */
struct dom_node;
js_canvas_t *js_canvas_for_node(struct dom_node *node, int width, int height);
js_canvas_t *js_canvas_find_node(struct dom_node *node);
void js_canvas_forget_node(struct dom_node *node);
void js_canvas_clear_registry(void);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_JS_CANVAS_H */
