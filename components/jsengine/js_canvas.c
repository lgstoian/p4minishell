/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file js_canvas.c
 * @brief Stripped-down Canvas 2D software rasterizer (see js_canvas.h).
 *
 * @module jsengine
 * @layer leaf
 * @domain app
 * @provides Canvas 2D RGBA rasterizer, path/transform/text state, DOM registry
 * @status stable
 * @test test/main/test_js_canvas.c
 * @hard-rule ai-context.md#module-layering-rules
 */

#include "js_canvas.h"

#include "css_color.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dom.h"
#include "gfx.h"
#include "p4heap.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    float a, b, c, d, e, f;   /* x' = a*x + c*y + e ; y' = b*x + d*y + f */
} canvas_matrix_t;

typedef struct {
    canvas_matrix_t matrix;
    js_canvas_rgba_t fill;
    js_canvas_rgba_t stroke;
    uint8_t alpha;
    float line_width;
    int font_scale;
    int text_align;
    int text_baseline;
} canvas_state_t;

typedef struct {
    float x;
    float y;
    bool move;              /* starts a new subpath */
} canvas_point_t;

struct js_canvas {
    int width;
    int height;
    uint8_t *pixels;        /* width * height * 4, straight RGBA */
    canvas_state_t state;
    canvas_state_t stack[JS_CANVAS_SAVE_DEPTH];
    int stack_depth;
    canvas_point_t path[JS_CANVAS_MAX_PATH];
    int path_count;
    bool path_closed;
};

typedef struct {
    struct dom_node *node;
    js_canvas_t *canvas;
} canvas_slot_t;

static canvas_slot_t s_canvases[JS_CANVAS_MAX_COUNT];

/* ---------------------------------------------------------------------- */
/* State and pixel compositing                                            */
/* ---------------------------------------------------------------------- */

static void canvas_state_default(canvas_state_t *state)
{
    memset(state, 0, sizeof(*state));
    state->matrix.a = 1.0f;
    state->matrix.d = 1.0f;
    state->fill.r = 0;
    state->fill.g = 0;
    state->fill.b = 0;
    state->fill.a = 255;
    state->stroke = state->fill;
    state->alpha = 255;
    state->line_width = 1.0f;
    state->font_scale = 1;
    state->text_align = 0;
    state->text_baseline = 0;
}

static void canvas_transform_xy(const js_canvas_t *canvas, float x, float y,
                                float *out_x, float *out_y)
{
    const canvas_matrix_t *m = &canvas->state.matrix;

    *out_x = m->a * x + m->c * y + m->e;
    *out_y = m->b * x + m->d * y + m->f;
}

/** Source-over with straight alpha, per the compositing model in the spec. */
static void canvas_blend(js_canvas_t *canvas, int x, int y,
                         js_canvas_rgba_t src, uint32_t coverage)
{
    uint8_t *dst;
    uint32_t sa;
    uint32_t da;
    uint32_t oa;

    if (x < 0 || y < 0 || x >= canvas->width || y >= canvas->height) {
        return;
    }
    if (coverage == 0) {
        return;
    }
    sa = (uint32_t)src.a * canvas->state.alpha / 255u;
    sa = sa * coverage / 255u;
    if (sa == 0) {
        return;
    }
    dst = canvas->pixels + ((size_t)y * (size_t)canvas->width + (size_t)x) * 4u;
    da = dst[3];
    oa = sa + da * (255u - sa) / 255u;
    if (oa == 0) {
        return;
    }
    dst[0] = (uint8_t)(((uint32_t)src.r * sa +
                        (uint32_t)dst[0] * da * (255u - sa) / 255u) / oa);
    dst[1] = (uint8_t)(((uint32_t)src.g * sa +
                        (uint32_t)dst[1] * da * (255u - sa) / 255u) / oa);
    dst[2] = (uint8_t)(((uint32_t)src.b * sa +
                        (uint32_t)dst[2] * da * (255u - sa) / 255u) / oa);
    dst[3] = (uint8_t)oa;
}

static void canvas_clear_pixel(js_canvas_t *canvas, int x, int y)
{
    uint8_t *dst;

    if (x < 0 || y < 0 || x >= canvas->width || y >= canvas->height) {
        return;
    }
    dst = canvas->pixels + ((size_t)y * (size_t)canvas->width + (size_t)x) * 4u;
    dst[0] = 0;
    dst[1] = 0;
    dst[2] = 0;
    dst[3] = 0;
}

/* ---------------------------------------------------------------------- */
/* Polygon scanline fill                                                  */
/* ---------------------------------------------------------------------- */

static void canvas_fill_polygon(js_canvas_t *canvas, const float *xs,
                                const float *ys, int count, bool clear)
{
    float min_y = 0.0f;
    float max_y = 0.0f;

    if (count < 3) {
        return;
    }
    min_y = max_y = ys[0];
    for (int i = 1; i < count; i++) {
        if (ys[i] < min_y) min_y = ys[i];
        if (ys[i] > max_y) max_y = ys[i];
    }
    {
        int y0 = (int)floorf(min_y);
        int y1 = (int)ceilf(max_y);

        if (y0 < 0) y0 = 0;
        if (y1 > canvas->height - 1) y1 = canvas->height - 1;
        for (int y = y0; y <= y1; y++) {
            float scan = (float)y + 0.5f;
            float hits[JS_CANVAS_MAX_PATH];
            int hit_count = 0;

            for (int i = 0; i < count; i++) {
                int j = (i + 1) % count;
                float y_a = ys[i];
                float y_b = ys[j];

                if ((y_a <= scan && y_b > scan) || (y_b <= scan && y_a > scan)) {
                    float t = (scan - y_a) / (y_b - y_a);
                    float x = xs[i] + t * (xs[j] - xs[i]);

                    if (hit_count < JS_CANVAS_MAX_PATH) {
                        hits[hit_count++] = x;
                    }
                }
            }
            for (int a = 1; a < hit_count; a++) {  /* insertion sort */
                float key = hits[a];
                int b = a - 1;

                while (b >= 0 && hits[b] > key) {
                    hits[b + 1] = hits[b];
                    b--;
                }
                hits[b + 1] = key;
            }
            for (int h = 0; h + 1 < hit_count; h += 2) {
                int xa = (int)ceilf(hits[h] - 0.5f);
                int xb = (int)floorf(hits[h + 1] - 0.5f);

                if (xa < 0) xa = 0;
                if (xb > canvas->width - 1) xb = canvas->width - 1;
                for (int x = xa; x <= xb; x++) {
                    if (clear) {
                        canvas_clear_pixel(canvas, x, y);
                    } else {
                        canvas_blend(canvas, x, y, canvas->state.fill, 255);
                    }
                }
            }
        }
    }
}

static void canvas_transform_polygon(const js_canvas_t *canvas,
                                     const float *src_x, const float *src_y,
                                     float *dst_x, float *dst_y, int count)
{
    for (int i = 0; i < count; i++) {
        canvas_transform_xy(canvas, src_x[i], src_y[i], &dst_x[i], &dst_y[i]);
    }
}

/* ---------------------------------------------------------------------- */
/* Line and path helpers                                                  */
/* ---------------------------------------------------------------------- */

static void canvas_dot(js_canvas_t *canvas, int x, int y, js_canvas_rgba_t color,
                       int size)
{
    int half = size / 2;

    for (int dy = -half; dy <= half; dy++) {
        for (int dx = -half; dx <= half; dx++) {
            canvas_blend(canvas, x + dx, y + dy, color, 255);
        }
    }
}

static void canvas_line(js_canvas_t *canvas, float x0, float y0, float x1, float y1,
                        js_canvas_rgba_t color)
{
    int size = (int)lroundf(canvas->state.line_width);

    if (size < 1) size = 1;
    if (size > 16) size = 16;
    {
        int ix0 = (int)lroundf(x0);
        int iy0 = (int)lroundf(y0);
        int ix1 = (int)lroundf(x1);
        int iy1 = (int)lroundf(y1);
        int dx = abs(ix1 - ix0);
        int dy = -abs(iy1 - iy0);
        int sx = ix0 < ix1 ? 1 : -1;
        int sy = iy0 < iy1 ? 1 : -1;
        int err = dx + dy;

        for (;;) {
            canvas_dot(canvas, ix0, iy0, color, size);
            if (ix0 == ix1 && iy0 == iy1) {
                break;
            }
            {
                int e2 = 2 * err;

                if (e2 >= dy) {
                    err += dy;
                    ix0 += sx;
                }
                if (e2 <= dx) {
                    err += dx;
                    iy0 += sy;
                }
            }
        }
    }
}

static void canvas_stroke_path(js_canvas_t *canvas)
{
    int sub_start = -1;

    for (int i = 0; i < canvas->path_count; i++) {
        if (canvas->path[i].move) {
            sub_start = i;
            continue;
        }
        if (sub_start < 0) {
            continue;
        }
        {
            float x0, y0, x1, y1;

            canvas_transform_xy(canvas, canvas->path[i - 1].x,
                                canvas->path[i - 1].y, &x0, &y0);
            canvas_transform_xy(canvas, canvas->path[i].x, canvas->path[i].y,
                                &x1, &y1);
            canvas_line(canvas, x0, y0, x1, y1, canvas->state.stroke);
        }
    }
    if (canvas->path_closed && sub_start >= 0 && canvas->path_count > sub_start + 1) {
        float x0, y0, x1, y1;
        int last = canvas->path_count - 1;

        canvas_transform_xy(canvas, canvas->path[last].x, canvas->path[last].y,
                            &x0, &y0);
        canvas_transform_xy(canvas, canvas->path[sub_start].x,
                            canvas->path[sub_start].y, &x1, &y1);
        canvas_line(canvas, x0, y0, x1, y1, canvas->state.stroke);
    }
}

/* ---------------------------------------------------------------------- */
/* Public lifecycle and state                                             */
/* ---------------------------------------------------------------------- */

js_canvas_t *js_canvas_new(int width, int height)
{
    js_canvas_t *canvas;

    if (width < 1 || height < 1) {
        return NULL;
    }
    if (width > JS_CANVAS_MAX_W) width = JS_CANVAS_MAX_W;
    if (height > JS_CANVAS_MAX_H) height = JS_CANVAS_MAX_H;

    canvas = calloc(1, sizeof(*canvas));
    if (canvas == NULL) {
        return NULL;
    }
    canvas->pixels = p4heap_alloc_psram((size_t)width * (size_t)height * 4u);
    if (canvas->pixels == NULL) {
        free(canvas);
        return NULL;
    }
    canvas->width = width;
    canvas->height = height;
    canvas_state_default(&canvas->state);
    js_canvas_clear(canvas);
    return canvas;
}

void js_canvas_free(js_canvas_t *canvas)
{
    if (canvas == NULL) {
        return;
    }
    p4heap_free(canvas->pixels);
    free(canvas);
}

bool js_canvas_resize(js_canvas_t *canvas, int width, int height)
{
    uint8_t *pixels;
    int old_w;
    int old_h;

    if (canvas == NULL || width < 1 || height < 1) {
        return false;
    }
    if (width > JS_CANVAS_MAX_W) width = JS_CANVAS_MAX_W;
    if (height > JS_CANVAS_MAX_H) height = JS_CANVAS_MAX_H;
    pixels = p4heap_alloc_psram((size_t)width * (size_t)height * 4u);
    if (pixels == NULL) {
        return false;
    }
    old_w = canvas->width;
    old_h = canvas->height;
    p4heap_free(canvas->pixels);
    canvas->pixels = pixels;
    canvas->width = width;
    canvas->height = height;
    (void)old_w;
    (void)old_h;
    /* Per the HTML spec, setting width/height resets the bitmap and the
     * drawing state (the current path is discarded too). */
    canvas_state_default(&canvas->state);
    canvas->stack_depth = 0;
    canvas->path_count = 0;
    canvas->path_closed = false;
    memset(canvas->pixels, 0, (size_t)width * (size_t)height * 4u);
    return true;
}

int js_canvas_width(const js_canvas_t *canvas)
{
    return canvas != NULL ? canvas->width : 0;
}

int js_canvas_height(const js_canvas_t *canvas)
{
    return canvas != NULL ? canvas->height : 0;
}

const uint8_t *js_canvas_pixels(const js_canvas_t *canvas)
{
    return canvas != NULL ? canvas->pixels : NULL;
}

void js_canvas_set_fill(js_canvas_t *canvas, js_canvas_rgba_t color)
{
    if (canvas != NULL) canvas->state.fill = color;
}

void js_canvas_set_stroke(js_canvas_t *canvas, js_canvas_rgba_t color)
{
    if (canvas != NULL) canvas->state.stroke = color;
}

js_canvas_rgba_t js_canvas_get_fill(const js_canvas_t *canvas)
{
    return canvas != NULL ? canvas->state.fill : (js_canvas_rgba_t){ 0, 0, 0, 255 };
}

js_canvas_rgba_t js_canvas_get_stroke(const js_canvas_t *canvas)
{
    return canvas != NULL ? canvas->state.stroke : (js_canvas_rgba_t){ 0, 0, 0, 255 };
}

uint8_t js_canvas_get_global_alpha(const js_canvas_t *canvas)
{
    return canvas != NULL ? canvas->state.alpha : 255;
}

float js_canvas_get_line_width(const js_canvas_t *canvas)
{
    return canvas != NULL ? canvas->state.line_width : 1.0f;
}

int js_canvas_get_font_scale(const js_canvas_t *canvas)
{
    return canvas != NULL ? canvas->state.font_scale : 1;
}

int js_canvas_get_text_align(const js_canvas_t *canvas)
{
    return canvas != NULL ? canvas->state.text_align : 0;
}

int js_canvas_get_text_baseline(const js_canvas_t *canvas)
{
    return canvas != NULL ? canvas->state.text_baseline : 0;
}

void js_canvas_set_global_alpha(js_canvas_t *canvas, uint8_t alpha)
{
    if (canvas != NULL) canvas->state.alpha = alpha;
}

void js_canvas_set_line_width(js_canvas_t *canvas, float width)
{
    if (canvas == NULL) return;
    if (width < 1.0f) width = 1.0f;
    if (width > 16.0f) width = 16.0f;
    canvas->state.line_width = width;
}

void js_canvas_set_font_scale(js_canvas_t *canvas, int scale)
{
    if (canvas == NULL) return;
    if (scale < 1) scale = 1;
    if (scale > 4) scale = 4;
    canvas->state.font_scale = scale;
}

void js_canvas_set_text_align(js_canvas_t *canvas, int align)
{
    if (canvas != NULL) canvas->state.text_align = align;
}

void js_canvas_set_text_baseline(js_canvas_t *canvas, int baseline)
{
    if (canvas != NULL) canvas->state.text_baseline = baseline;
}

void js_canvas_save(js_canvas_t *canvas)
{
    if (canvas == NULL || canvas->stack_depth >= JS_CANVAS_SAVE_DEPTH) {
        return;
    }
    canvas->stack[canvas->stack_depth++] = canvas->state;
}

void js_canvas_restore(js_canvas_t *canvas)
{
    if (canvas == NULL || canvas->stack_depth <= 0) {
        return;
    }
    canvas->state = canvas->stack[--canvas->stack_depth];
}

void js_canvas_translate(js_canvas_t *canvas, float tx, float ty)
{
    canvas_matrix_t *m;

    if (canvas == NULL) return;
    m = &canvas->state.matrix;
    m->e += m->a * tx + m->c * ty;
    m->f += m->b * tx + m->d * ty;
}

void js_canvas_scale(js_canvas_t *canvas, float sx, float sy)
{
    canvas_matrix_t *m;

    if (canvas == NULL) return;
    m = &canvas->state.matrix;
    m->a *= sx;
    m->b *= sx;
    m->c *= sy;
    m->d *= sy;
}

void js_canvas_rotate(js_canvas_t *canvas, float radians)
{
    canvas_matrix_t *m;
    float cs = cosf(radians);
    float sn = sinf(radians);
    float a, b, c, d;

    if (canvas == NULL) return;
    m = &canvas->state.matrix;
    a = m->a * cs + m->c * sn;
    b = m->b * cs + m->d * sn;
    c = m->c * cs - m->a * sn;
    d = m->d * cs - m->b * sn;
    m->a = a;
    m->b = b;
    m->c = c;
    m->d = d;
}

void js_canvas_set_transform(js_canvas_t *canvas, float a, float b, float c,
                             float d, float e, float f)
{
    if (canvas == NULL) return;
    canvas->state.matrix.a = a;
    canvas->state.matrix.b = b;
    canvas->state.matrix.c = c;
    canvas->state.matrix.d = d;
    canvas->state.matrix.e = e;
    canvas->state.matrix.f = f;
}

void js_canvas_reset_transform(js_canvas_t *canvas)
{
    js_canvas_set_transform(canvas, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);
}

/* ---------------------------------------------------------------------- */
/* Rectangles, paths, text                                                */
/* ---------------------------------------------------------------------- */

void js_canvas_clear(js_canvas_t *canvas)
{
    if (canvas != NULL && canvas->pixels != NULL) {
        memset(canvas->pixels, 0,
               (size_t)canvas->width * (size_t)canvas->height * 4u);
    }
}

void js_canvas_fill_rect(js_canvas_t *canvas, float x, float y, float w, float h)
{
    float sx[4], sy[4], dx[4], dy[4];

    if (canvas == NULL || w == 0.0f || h == 0.0f) return;
    if (w < 0.0f) { x += w; w = -w; }
    if (h < 0.0f) { y += h; h = -h; }
    sx[0] = x;     sy[0] = y;
    sx[1] = x + w; sy[1] = y;
    sx[2] = x + w; sy[2] = y + h;
    sx[3] = x;     sy[3] = y + h;
    canvas_transform_polygon(canvas, sx, sy, dx, dy, 4);
    canvas_fill_polygon(canvas, dx, dy, 4, false);
}

void js_canvas_clear_rect(js_canvas_t *canvas, float x, float y, float w, float h)
{
    float sx[4], sy[4], dx[4], dy[4];

    if (canvas == NULL || w == 0.0f || h == 0.0f) return;
    if (w < 0.0f) { x += w; w = -w; }
    if (h < 0.0f) { y += h; h = -h; }
    sx[0] = x;     sy[0] = y;
    sx[1] = x + w; sy[1] = y;
    sx[2] = x + w; sy[2] = y + h;
    sx[3] = x;     sy[3] = y + h;
    canvas_transform_polygon(canvas, sx, sy, dx, dy, 4);
    canvas_fill_polygon(canvas, dx, dy, 4, true);
}

void js_canvas_stroke_rect(js_canvas_t *canvas, float x, float y, float w, float h)
{
    float x0, y0, x1, y1;

    if (canvas == NULL) return;
    canvas_transform_xy(canvas, x, y, &x0, &y0);
    canvas_transform_xy(canvas, x + w, y + h, &x1, &y1);
    canvas_line(canvas, x0, y0, x1, y0, canvas->state.stroke);
    canvas_line(canvas, x1, y0, x1, y1, canvas->state.stroke);
    canvas_line(canvas, x1, y1, x0, y1, canvas->state.stroke);
    canvas_line(canvas, x0, y1, x0, y0, canvas->state.stroke);
}

void js_canvas_begin_path(js_canvas_t *canvas)
{
    if (canvas == NULL) return;
    canvas->path_count = 0;
    canvas->path_closed = false;
}

static void canvas_path_push(js_canvas_t *canvas, float x, float y, bool move)
{
    if (canvas == NULL || canvas->path_count >= JS_CANVAS_MAX_PATH) {
        return;
    }
    canvas->path[canvas->path_count].x = x;
    canvas->path[canvas->path_count].y = y;
    canvas->path[canvas->path_count].move = move;
    canvas->path_count++;
}

void js_canvas_move_to(js_canvas_t *canvas, float x, float y)
{
    canvas_path_push(canvas, x, y, true);
}

void js_canvas_line_to(js_canvas_t *canvas, float x, float y)
{
    if (canvas == NULL) return;
    if (canvas->path_count == 0) {
        canvas_path_push(canvas, x, y, true);
    } else {
        canvas_path_push(canvas, x, y, false);
    }
}

void js_canvas_close_path(js_canvas_t *canvas)
{
    if (canvas != NULL) {
        canvas->path_closed = true;
    }
}

void js_canvas_rect(js_canvas_t *canvas, float x, float y, float w, float h)
{
    js_canvas_move_to(canvas, x, y);
    js_canvas_line_to(canvas, x + w, y);
    js_canvas_line_to(canvas, x + w, y + h);
    js_canvas_line_to(canvas, x, y + h);
    js_canvas_close_path(canvas);
}

void js_canvas_arc(js_canvas_t *canvas, float cx, float cy, float radius,
                   float start, float end, bool counterclockwise)
{
    int steps;
    float delta;

    if (canvas == NULL || radius <= 0.0f) return;
    if (counterclockwise && end > start) {
        end -= 2.0f * (float)M_PI;
    } else if (!counterclockwise && end < start) {
        end += 2.0f * (float)M_PI;
    }
    delta = fabsf(end - start);
    steps = (int)(delta / (float)M_PI * 24.0f) + 2;
    if (steps > 180) steps = 180;

    if (canvas->path_count > 0 && !canvas->path[canvas->path_count - 1].move) {
        js_canvas_line_to(canvas, cx + cosf(start) * radius,
                          cy + sinf(start) * radius);
    } else {
        js_canvas_move_to(canvas, cx + cosf(start) * radius,
                          cy + sinf(start) * radius);
    }
    for (int i = 1; i <= steps; i++) {
        float angle = start + (end - start) * (float)i / (float)steps;

        js_canvas_line_to(canvas, cx + cosf(angle) * radius,
                          cy + sinf(angle) * radius);
    }
}

void js_canvas_fill(js_canvas_t *canvas)
{
    int start = -1;

    if (canvas == NULL) return;
    for (int i = 0; i <= canvas->path_count; i++) {
        bool boundary = (i == canvas->path_count) ||
                        (i < canvas->path_count && canvas->path[i].move);

        if (boundary) {
            if (start >= 0 && i - start >= 3) {
                float xs[JS_CANVAS_MAX_PATH];
                float ys[JS_CANVAS_MAX_PATH];
                float dx[JS_CANVAS_MAX_PATH];
                float dy[JS_CANVAS_MAX_PATH];
                int count = i - start;

                for (int j = 0; j < count; j++) {
                    xs[j] = canvas->path[start + j].x;
                    ys[j] = canvas->path[start + j].y;
                }
                canvas_transform_polygon(canvas, xs, ys, dx, dy, count);
                canvas_fill_polygon(canvas, dx, dy, count, false);
            }
            start = i;
        }
    }
}

void js_canvas_stroke(js_canvas_t *canvas)
{
    canvas_stroke_path(canvas);
}

int js_canvas_measure_text(const js_canvas_t *canvas, const char *text)
{
    int scale = canvas != NULL ? canvas->state.font_scale : 1;

    return (int)strlen(text) * 8 * scale;
}

int js_canvas_fill_text(js_canvas_t *canvas, const char *text, float x, float y,
                        bool stroke)
{
    int scale;
    int advance;
    float origin_x;
    float origin_y;
    int cursor = 0;

    if (canvas == NULL || text == NULL) {
        return 0;
    }
    scale = canvas->state.font_scale;
    advance = js_canvas_measure_text(canvas, text);
    origin_x = x;
    origin_y = y;
    if (canvas->state.text_align == 1) {
        origin_x -= (float)advance / 2.0f;
    } else if (canvas->state.text_align == 2) {
        origin_x -= (float)advance;
    }
    if (canvas->state.text_baseline == 1) {
        /* top */
    } else if (canvas->state.text_baseline == 2) {
        origin_y -= 4.0f * (float)scale;
    } else if (canvas->state.text_baseline == 3) {
        origin_y -= 8.0f * (float)scale;
    } else {
        origin_y -= 8.0f * (float)scale;
    }

    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
        unsigned char ch = *p;
        const uint8_t *glyph;

        if (ch < 0x20 || ch > 0x7E) {
            ch = '?';
        }
        glyph = gfx_font8x8[ch];
        for (int row = 0; row < GFX_FONT_H; row++) {
            uint8_t bits = glyph[row];

            for (int col = 0; col < 8; col++) {
                if ((bits & (uint8_t)(0x80u >> col)) == 0) {
                    continue;
                }
                for (int sy = 0; sy < scale; sy++) {
                    for (int sx = 0; sx < scale; sx++) {
                        float gx = origin_x + (float)(cursor + col * scale + sx);
                        float gy = origin_y + (float)(row * scale + sy);
                        float dx, dy;

                        canvas_transform_xy(canvas, gx, gy, &dx, &dy);
                        canvas_blend(canvas, (int)lroundf(dx), (int)lroundf(dy),
                                     stroke ? canvas->state.stroke
                                            : canvas->state.fill, 255);
                    }
                }
            }
        }
        cursor += 8 * scale;
    }
    return advance;
}

bool js_canvas_parse_color(const char *text, js_canvas_rgba_t *out)
{
    /* One parser for the whole firmware: CSS, canvas and the TUI all agree on
     * what a colour string means (see components/markdown/css_color.h). */
    css_rgba_t color;

    if (!css_color_parse(text, &color)) {
        return false;
    }
    out->r = color.r;
    out->g = color.g;
    out->b = color.b;
    out->a = color.a;
    return true;
}

/* ---------------------------------------------------------------------- */
/* Snapshot and DOM registry                                              */
/* ---------------------------------------------------------------------- */

bool js_canvas_snapshot_rgba(const js_canvas_t *canvas, int max_w, int max_h,
                             uint8_t *out_rgba, int *out_w, int *out_h)
{
    int w;
    int h;

    if (canvas == NULL || out_rgba == NULL || max_w < 1 || max_h < 1) {
        return false;
    }
    w = max_w < canvas->width ? max_w : canvas->width;
    h = max_h < canvas->height ? max_h : canvas->height;

    for (int y = 0; y < h; y++) {
        int sy0 = y * canvas->height / h;
        int sy1 = (y + 1) * canvas->height / h;

        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int x = 0; x < w; x++) {
            int sx0 = x * canvas->width / w;
            int sx1 = (x + 1) * canvas->width / w;
            uint32_t r = 0;
            uint32_t g = 0;
            uint32_t b = 0;
            uint32_t a = 0;
            uint32_t n = 0;

            if (sx1 <= sx0) sx1 = sx0 + 1;
            for (int sy = sy0; sy < sy1 && sy < canvas->height; sy++) {
                for (int sx = sx0; sx < sx1 && sx < canvas->width; sx++) {
                    const uint8_t *px = canvas->pixels +
                        ((size_t)sy * (size_t)canvas->width + (size_t)sx) * 4u;

                    r += px[0];
                    g += px[1];
                    b += px[2];
                    a += px[3];
                    n++;
                }
            }
            if (n == 0) n = 1;
            out_rgba[((size_t)y * (size_t)w + (size_t)x) * 4u + 0] =
                (uint8_t)(r / n);
            out_rgba[((size_t)y * (size_t)w + (size_t)x) * 4u + 1] =
                (uint8_t)(g / n);
            out_rgba[((size_t)y * (size_t)w + (size_t)x) * 4u + 2] =
                (uint8_t)(b / n);
            out_rgba[((size_t)y * (size_t)w + (size_t)x) * 4u + 3] =
                (uint8_t)(a / n);
        }
    }
    if (out_w != NULL) *out_w = w;
    if (out_h != NULL) *out_h = h;
    return true;
}

js_canvas_t *js_canvas_find_node(struct dom_node *node)
{
    for (int i = 0; i < JS_CANVAS_MAX_COUNT; i++) {
        if (s_canvases[i].node == node && s_canvases[i].canvas != NULL) {
            return s_canvases[i].canvas;
        }
    }
    return NULL;
}

js_canvas_t *js_canvas_for_node(struct dom_node *node, int width, int height)
{
    js_canvas_t *canvas = js_canvas_find_node(node);

    if (canvas != NULL) {
        return canvas;
    }
    if (node == NULL) {
        return NULL;
    }
    for (int i = 0; i < JS_CANVAS_MAX_COUNT; i++) {
        if (s_canvases[i].node == node) {
            s_canvases[i].canvas = js_canvas_new(width, height);
            return s_canvases[i].canvas;
        }
    }
    for (int i = 0; i < JS_CANVAS_MAX_COUNT; i++) {
        if (s_canvases[i].node == NULL) {
            s_canvases[i].node = node;
            s_canvases[i].canvas = js_canvas_new(width, height);
            return s_canvases[i].canvas;
        }
    }
    return NULL;   /* registry full: documented cap */
}

void js_canvas_forget_node(struct dom_node *node)
{
    for (int i = 0; i < JS_CANVAS_MAX_COUNT; i++) {
        if (s_canvases[i].node == node) {
            js_canvas_free(s_canvases[i].canvas);
            s_canvases[i].canvas = NULL;
            s_canvases[i].node = NULL;
        }
    }
}

void js_canvas_clear_registry(void)
{
    for (int i = 0; i < JS_CANVAS_MAX_COUNT; i++) {
        js_canvas_free(s_canvases[i].canvas);
        s_canvases[i].canvas = NULL;
        s_canvases[i].node = NULL;
    }
}
