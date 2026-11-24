/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file js_canvas_js.c
 * @brief QuickJS Canvas 2D binding (WHATWG HTML Living Standard 4.12.5 subset).
 *
 * The supported surface is deliberately small and documented: colours, alpha,
 * line width, save/restore, the affine transform, rectangles, one-or-more
 * subpaths with fill/stroke, arcs and bitmap text. Calls the core cannot
 * implement (images, patterns, gradients, clipping, pixel access) are present
 * as no-ops or null returns so a real page keeps running instead of throwing
 * a ReferenceError.
 *
 * @module jsengine
 * @layer leaf
 * @domain app
 * @provides CanvasRenderingContext2D bindings over the js_canvas core
 * @status stable
 * @test test/main/test_js_canvas.c
 * @hard-rule ai-context.md#module-layering-rules
 */

#include "js_canvas_js.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "js_canvas.h"

static JSClassID s_canvas_class_id;
static JSClassID s_gradient_class_id;

/** A JS-visible CanvasGradient object. The C gradient is the same type CSS
 *  uses, so there is still one colour/gradient parser and one DOS palette. */
typedef struct {
    js_canvas_gradient_t gradient;
} canvas_gradient_holder_t;

static void canvas_gradient_finalizer(JSRuntime *rt, JSValue val)
{
    canvas_gradient_holder_t *holder;

    (void)rt;
    holder = (canvas_gradient_holder_t *)JS_GetOpaque(val, s_gradient_class_id);
    free(holder);
}

static JSClassDef s_gradient_class = {
    "CanvasGradient",
    .finalizer = canvas_gradient_finalizer,
};

static void canvas_js_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    (void)val;   /* the DOM registry owns the C canvas */
}

static JSClassDef s_canvas_class = {
    "CanvasRenderingContext2D",
    .finalizer = canvas_js_finalizer,
};

static js_canvas_t *canvas_of(JSContext *ctx, JSValueConst this_val)
{
    (void)ctx;
    return (js_canvas_t *)JS_GetOpaque(this_val, s_canvas_class_id);
}

static double arg_number(JSContext *ctx, int argc, JSValueConst *argv, int index)
{
    double value = 0.0;

    if (index < argc) {
        (void)JS_ToFloat64(ctx, &value, argv[index]);
    }
    return value;
}

static void canvas_format_color(char *buf, size_t size, js_canvas_rgba_t color)
{
    if (color.a == 255) {
        snprintf(buf, size, "#%02x%02x%02x", color.r, color.g, color.b);
    } else {
        snprintf(buf, size, "rgba(%u, %u, %u, %.3f)", color.r, color.g, color.b,
                 (double)color.a / 255.0);
    }
}

static JSValue canvas_get_fill_style(JSContext *ctx, JSValueConst this_val)
{
    char buf[40];

    canvas_format_color(buf, sizeof(buf), js_canvas_get_fill(canvas_of(ctx, this_val)));
    return JS_NewString(ctx, buf);
}

static JSValue canvas_set_fill_style(JSContext *ctx, JSValueConst this_val,
                                     JSValueConst val)
{
    js_canvas_t *canvas = canvas_of(ctx, this_val);
    const char *text;
    js_canvas_rgba_t color;

    if (canvas == NULL) {
        return JS_UNDEFINED;
    }
    if (JS_IsObject(val)) {
        canvas_gradient_holder_t *holder =
            (canvas_gradient_holder_t *)JS_GetOpaque(val, s_gradient_class_id);

        if (holder != NULL) {
            js_canvas_set_gradient(canvas, &holder->gradient);
            return JS_UNDEFINED;
        }
    }
    text = JS_ToCString(ctx, val);
    if (text != NULL) {
        js_canvas_gradient_t gradient;

        if (js_canvas_gradient_parse(text, &gradient)) {
            js_canvas_set_gradient(canvas, &gradient);
        } else if (js_canvas_parse_color(text, &color)) {
            js_canvas_clear_gradient(canvas);
            js_canvas_set_fill(canvas, color);
        }
        JS_FreeCString(ctx, text);
    }
    return JS_UNDEFINED;
}

static JSValue canvas_get_stroke_style(JSContext *ctx, JSValueConst this_val)
{
    char buf[40];

    canvas_format_color(buf, sizeof(buf), js_canvas_get_stroke(canvas_of(ctx, this_val)));
    return JS_NewString(ctx, buf);
}

static JSValue canvas_set_stroke_style(JSContext *ctx, JSValueConst this_val,
                                       JSValueConst val)
{
    js_canvas_t *canvas = canvas_of(ctx, this_val);
    const char *text;
    js_canvas_rgba_t color;

    if (canvas == NULL) {
        return JS_UNDEFINED;
    }
    text = JS_ToCString(ctx, val);
    if (text != NULL) {
        if (js_canvas_parse_color(text, &color)) {
            js_canvas_set_stroke(canvas, color);
        }
        JS_FreeCString(ctx, text);
    }
    return JS_UNDEFINED;
}

static JSValue canvas_get_global_alpha(JSContext *ctx, JSValueConst this_val)
{
    return JS_NewFloat64(ctx, (double)js_canvas_get_global_alpha(canvas_of(ctx, this_val)) / 255.0);
}

static JSValue canvas_set_global_alpha(JSContext *ctx, JSValueConst this_val,
                                       JSValueConst val)
{
    js_canvas_t *canvas = canvas_of(ctx, this_val);
    double alpha = 1.0;

    if (canvas == NULL) {
        return JS_UNDEFINED;
    }
    (void)JS_ToFloat64(ctx, &alpha, val);
    if (alpha < 0.0) alpha = 0.0;
    if (alpha > 1.0) alpha = 1.0;
    js_canvas_set_global_alpha(canvas, (uint8_t)(alpha * 255.0 + 0.5));
    return JS_UNDEFINED;
}

static JSValue canvas_get_line_width(JSContext *ctx, JSValueConst this_val)
{
    return JS_NewFloat64(ctx, (double)js_canvas_get_line_width(canvas_of(ctx, this_val)));
}

static JSValue canvas_set_line_width(JSContext *ctx, JSValueConst this_val,
                                     JSValueConst val)
{
    js_canvas_t *canvas = canvas_of(ctx, this_val);

    if (canvas != NULL) {
        js_canvas_set_line_width(canvas, (float)arg_number(ctx, 1, &val, 0));
    }
    return JS_UNDEFINED;
}

static JSValue canvas_get_font(JSContext *ctx, JSValueConst this_val)
{
    char buf[40];

    snprintf(buf, sizeof(buf), "%dpx sans-serif",
             js_canvas_get_font_scale(canvas_of(ctx, this_val)) * 8);
    return JS_NewString(ctx, buf);
}

static JSValue canvas_set_font(JSContext *ctx, JSValueConst this_val,
                               JSValueConst val)
{
    js_canvas_t *canvas = canvas_of(ctx, this_val);
    const char *text;
    int px = 10;

    if (canvas == NULL) {
        return JS_UNDEFINED;
    }
    text = JS_ToCString(ctx, val);
    if (text != NULL) {
        char *end = NULL;
        long value = strtol(text, &end, 10);

        if (end != text) {
            px = (int)value;
        }
        JS_FreeCString(ctx, text);
    }
    /* The bitmap face is 8 px; pick the integer scale nearest the requested
     * size (10 px -> 1, 16 px -> 2, ...). */
    {
        int scale = (px + 4) / 8;

        if (scale < 1) scale = 1;
        js_canvas_set_font_scale(canvas, scale);
    }
    return JS_UNDEFINED;
}

static JSValue canvas_get_text_align(JSContext *ctx, JSValueConst this_val)
{
    static const char *const names[] = { "left", "center", "right" };
    int align = js_canvas_get_text_align(canvas_of(ctx, this_val));

    if (align < 0 || align > 2) align = 0;
    return JS_NewString(ctx, names[align]);
}

static JSValue canvas_set_text_align(JSContext *ctx, JSValueConst this_val,
                                     JSValueConst val)
{
    js_canvas_t *canvas = canvas_of(ctx, this_val);
    const char *text;
    int align = 0;

    if (canvas == NULL) {
        return JS_UNDEFINED;
    }
    text = JS_ToCString(ctx, val);
    if (text != NULL) {
        if (strcmp(text, "center") == 0) align = 1;
        else if (strcmp(text, "right") == 0 || strcmp(text, "end") == 0) align = 2;
        js_canvas_set_text_align(canvas, align);
        JS_FreeCString(ctx, text);
    }
    return JS_UNDEFINED;
}

static JSValue canvas_get_text_baseline(JSContext *ctx, JSValueConst this_val)
{
    static const char *const names[] = { "alphabetic", "top", "middle", "bottom" };
    int baseline = js_canvas_get_text_baseline(canvas_of(ctx, this_val));

    if (baseline < 0 || baseline > 3) baseline = 0;
    return JS_NewString(ctx, names[baseline]);
}

static JSValue canvas_set_text_baseline(JSContext *ctx, JSValueConst this_val,
                                        JSValueConst val)
{
    js_canvas_t *canvas = canvas_of(ctx, this_val);
    const char *text;
    int baseline = 0;

    if (canvas == NULL) {
        return JS_UNDEFINED;
    }
    text = JS_ToCString(ctx, val);
    if (text != NULL) {
        if (strcmp(text, "top") == 0 || strcmp(text, "hanging") == 0) baseline = 1;
        else if (strcmp(text, "middle") == 0) baseline = 2;
        else if (strcmp(text, "bottom") == 0 || strcmp(text, "ideographic") == 0) baseline = 3;
        js_canvas_set_text_baseline(canvas, baseline);
        JS_FreeCString(ctx, text);
    }
    return JS_UNDEFINED;
}

static JSValue canvas_save(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv)
{
    (void)ctx; (void)argc; (void)argv;
    js_canvas_save(canvas_of(ctx, this_val));
    return JS_UNDEFINED;
}

static JSValue canvas_restore(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv)
{
    (void)ctx; (void)argc; (void)argv;
    js_canvas_restore(canvas_of(ctx, this_val));
    return JS_UNDEFINED;
}

static JSValue canvas_translate(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv)
{
    js_canvas_translate(canvas_of(ctx, this_val),
                        (float)arg_number(ctx, argc, argv, 0),
                        (float)arg_number(ctx, argc, argv, 1));
    return JS_UNDEFINED;
}

static JSValue canvas_scale(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv)
{
    js_canvas_scale(canvas_of(ctx, this_val),
                    (float)arg_number(ctx, argc, argv, 0),
                    (float)arg_number(ctx, argc, argv, 1));
    return JS_UNDEFINED;
}

static JSValue canvas_rotate(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv)
{
    js_canvas_rotate(canvas_of(ctx, this_val),
                     (float)arg_number(ctx, argc, argv, 0));
    return JS_UNDEFINED;
}

static JSValue canvas_set_transform(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    js_canvas_set_transform(canvas_of(ctx, this_val),
                            (float)arg_number(ctx, argc, argv, 0),
                            (float)arg_number(ctx, argc, argv, 1),
                            (float)arg_number(ctx, argc, argv, 2),
                            (float)arg_number(ctx, argc, argv, 3),
                            (float)arg_number(ctx, argc, argv, 4),
                            (float)arg_number(ctx, argc, argv, 5));
    return JS_UNDEFINED;
}

static JSValue canvas_reset_transform(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    js_canvas_reset_transform(canvas_of(ctx, this_val));
    return JS_UNDEFINED;
}

static JSValue canvas_fill_rect(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv)
{
    js_canvas_fill_rect(canvas_of(ctx, this_val),
                        (float)arg_number(ctx, argc, argv, 0),
                        (float)arg_number(ctx, argc, argv, 1),
                        (float)arg_number(ctx, argc, argv, 2),
                        (float)arg_number(ctx, argc, argv, 3));
    return JS_UNDEFINED;
}

static JSValue canvas_clear_rect(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv)
{
    js_canvas_clear_rect(canvas_of(ctx, this_val),
                         (float)arg_number(ctx, argc, argv, 0),
                         (float)arg_number(ctx, argc, argv, 1),
                         (float)arg_number(ctx, argc, argv, 2),
                         (float)arg_number(ctx, argc, argv, 3));
    return JS_UNDEFINED;
}

static JSValue canvas_stroke_rect(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv)
{
    js_canvas_stroke_rect(canvas_of(ctx, this_val),
                          (float)arg_number(ctx, argc, argv, 0),
                          (float)arg_number(ctx, argc, argv, 1),
                          (float)arg_number(ctx, argc, argv, 2),
                          (float)arg_number(ctx, argc, argv, 3));
    return JS_UNDEFINED;
}

static JSValue canvas_begin_path(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv)
{
    (void)argc; (void)argv;
    js_canvas_begin_path(canvas_of(ctx, this_val));
    return JS_UNDEFINED;
}

static JSValue canvas_move_to(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv)
{
    js_canvas_move_to(canvas_of(ctx, this_val),
                      (float)arg_number(ctx, argc, argv, 0),
                      (float)arg_number(ctx, argc, argv, 1));
    return JS_UNDEFINED;
}

static JSValue canvas_line_to(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv)
{
    js_canvas_line_to(canvas_of(ctx, this_val),
                      (float)arg_number(ctx, argc, argv, 0),
                      (float)arg_number(ctx, argc, argv, 1));
    return JS_UNDEFINED;
}

static JSValue canvas_close_path(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv)
{
    (void)argc; (void)argv;
    js_canvas_close_path(canvas_of(ctx, this_val));
    return JS_UNDEFINED;
}

static JSValue canvas_rect(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv)
{
    js_canvas_rect(canvas_of(ctx, this_val),
                   (float)arg_number(ctx, argc, argv, 0),
                   (float)arg_number(ctx, argc, argv, 1),
                   (float)arg_number(ctx, argc, argv, 2),
                   (float)arg_number(ctx, argc, argv, 3));
    return JS_UNDEFINED;
}

static JSValue canvas_arc(JSContext *ctx, JSValueConst this_val, int argc,
                          JSValueConst *argv)
{
    js_canvas_arc(canvas_of(ctx, this_val),
                  (float)arg_number(ctx, argc, argv, 0),
                  (float)arg_number(ctx, argc, argv, 1),
                  (float)arg_number(ctx, argc, argv, 2),
                  (float)arg_number(ctx, argc, argv, 3),
                  (float)arg_number(ctx, argc, argv, 4),
                  argc > 5 && JS_ToBool(ctx, argv[5]));
    return JS_UNDEFINED;
}

static JSValue canvas_fill(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv)
{
    (void)argc; (void)argv;
    js_canvas_fill(canvas_of(ctx, this_val));
    return JS_UNDEFINED;
}

static JSValue canvas_stroke(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv)
{
    (void)argc; (void)argv;
    js_canvas_stroke(canvas_of(ctx, this_val));
    return JS_UNDEFINED;
}

static JSValue canvas_fill_text(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv)
{
    js_canvas_t *canvas = canvas_of(ctx, this_val);
    const char *text;
    int width = 0;

    if (canvas == NULL || argc < 1) {
        return JS_UNDEFINED;
    }
    text = JS_ToCString(ctx, argv[0]);
    if (text != NULL) {
        width = js_canvas_fill_text(canvas, text,
                                    (float)arg_number(ctx, argc, argv, 1),
                                    (float)arg_number(ctx, argc, argv, 2),
                                    false);
        JS_FreeCString(ctx, text);
    }
    (void)width;
    return JS_UNDEFINED;
}

static JSValue canvas_stroke_text(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv)
{
    js_canvas_t *canvas = canvas_of(ctx, this_val);
    const char *text;

    if (canvas == NULL || argc < 1) {
        return JS_UNDEFINED;
    }
    text = JS_ToCString(ctx, argv[0]);
    if (text != NULL) {
        (void)js_canvas_fill_text(canvas, text,
                                  (float)arg_number(ctx, argc, argv, 1),
                                  (float)arg_number(ctx, argc, argv, 2), true);
        JS_FreeCString(ctx, text);
    }
    return JS_UNDEFINED;
}

static JSValue canvas_measure_text(JSContext *ctx, JSValueConst this_val, int argc,
                                   JSValueConst *argv)
{
    js_canvas_t *canvas = canvas_of(ctx, this_val);
    JSValue result = JS_NewObject(ctx);
    const char *text;
    int width = 0;

    if (argc >= 1) {
        text = JS_ToCString(ctx, argv[0]);
        if (text != NULL) {
            width = js_canvas_measure_text(canvas, text);
            JS_FreeCString(ctx, text);
        }
    }
    JS_SetPropertyStr(ctx, result, "width", JS_NewInt32(ctx, width));
    return result;
}

/* ---- Additive Canvas 2D subset: gradients, rounded rects, shadows ------- */

static JSValue canvas_gradient_add_stop(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    canvas_gradient_holder_t *holder =
        (canvas_gradient_holder_t *)JS_GetOpaque(this_val, s_gradient_class_id);
    const char *text;
    js_canvas_rgba_t color;
    double position = 0.0;

    if (holder == NULL || argc < 2 || holder->gradient.count >= CSS_GRADIENT_MAX_STOPS) {
        return JS_UNDEFINED;
    }
    (void)JS_ToFloat64(ctx, &position, argv[0]);
    if (position < 0.0) position = 0.0;
    if (position > 1.0) position = 1.0;
    text = JS_ToCString(ctx, argv[1]);
    if (text == NULL) return JS_UNDEFINED;
    if (js_canvas_parse_color(text, &color)) {
        int index = holder->gradient.count++;

        holder->gradient.stops[index] = color;
        holder->gradient.positions[index] = (uint8_t)(position * 255.0 + 0.5);
        /* Keep positions monotonic so sampling stays deterministic. */
        for (int i = 1; i < holder->gradient.count; i++) {
            if (holder->gradient.positions[i] < holder->gradient.positions[i - 1]) {
                holder->gradient.positions[i] = holder->gradient.positions[i - 1];
            }
        }
    }
    JS_FreeCString(ctx, text);
    return JS_UNDEFINED;
}

static JSValue canvas_create_linear_gradient(JSContext *ctx, JSValueConst this_val,
                                             int argc, JSValueConst *argv)
{
    canvas_gradient_holder_t *holder;
    JSValue object;

    (void)this_val;
    (void)argc;
    (void)argv;
    if (s_gradient_class_id == 0) {
        JS_NewClassID(&s_gradient_class_id);
    }
    if (!JS_IsRegisteredClass(JS_GetRuntime(ctx), s_gradient_class_id)) {
        JS_NewClass(JS_GetRuntime(ctx), s_gradient_class_id, &s_gradient_class);
    }
    holder = calloc(1, sizeof(*holder));
    if (holder == NULL) return JS_NULL;
    holder->gradient.angle = (uint16_t)90;  /* horizontal by default */
    object = JS_NewObjectClass(ctx, s_gradient_class_id);
    if (JS_IsException(object)) {
        free(holder);
        return JS_EXCEPTION;
    }
    JS_SetOpaque(object, holder);
    JS_SetPropertyStr(ctx, object, "addColorStop",
                      JS_NewCFunction(ctx, canvas_gradient_add_stop,
                                      "addColorStop", 2));
    return object;
}

static JSValue canvas_round_rect(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    js_canvas_t *canvas = canvas_of(ctx, this_val);

    if (canvas == NULL || argc < 4) return JS_UNDEFINED;
    js_canvas_fill_rounded_rect(canvas, arg_number(ctx, argc, argv, 0),
                                arg_number(ctx, argc, argv, 1),
                                arg_number(ctx, argc, argv, 2),
                                arg_number(ctx, argc, argv, 3),
                                arg_number(ctx, argc, argv, 4));
    return JS_UNDEFINED;
}

static JSValue canvas_get_shadow_color(JSContext *ctx, JSValueConst this_val)
{
    char buf[40];

    canvas_format_color(buf, sizeof(buf), js_canvas_get_shadow_color(canvas_of(ctx, this_val)));
    return JS_NewString(ctx, buf);
}

static JSValue canvas_set_shadow_color(JSContext *ctx, JSValueConst this_val,
                                       JSValueConst val)
{
    js_canvas_t *canvas = canvas_of(ctx, this_val);
    const char *text;
    js_canvas_rgba_t color;
    int dx = 0;
    int dy = 0;
    int blur = 0;

    if (canvas == NULL) return JS_UNDEFINED;
    text = JS_ToCString(ctx, val);
    if (text == NULL) return JS_UNDEFINED;
    if (js_canvas_parse_color(text, &color)) {
        dx = js_canvas_get_shadow_offset_x(canvas);
        dy = js_canvas_get_shadow_offset_y(canvas);
        blur = js_canvas_get_shadow_blur(canvas);
        js_canvas_set_shadow(canvas, color, (float)dx, (float)dy, (float)blur);
    }
    JS_FreeCString(ctx, text);
    return JS_UNDEFINED;
}

static JSValue canvas_get_shadow_offset_x(JSContext *ctx, JSValueConst this_val)
{
    return JS_NewInt32(ctx, js_canvas_get_shadow_offset_x(canvas_of(ctx, this_val)));
}

static JSValue canvas_set_shadow_offset_x(JSContext *ctx, JSValueConst this_val,
                                          JSValueConst val)
{
    js_canvas_t *canvas = canvas_of(ctx, this_val);

    if (canvas != NULL) {
        js_canvas_set_shadow_offset(canvas, (float)arg_number(ctx, 1, &val, 0),
                                    (float)js_canvas_get_shadow_offset_y(canvas));
    }
    return JS_UNDEFINED;
}

static JSValue canvas_get_shadow_offset_y(JSContext *ctx, JSValueConst this_val)
{
    return JS_NewInt32(ctx, js_canvas_get_shadow_offset_y(canvas_of(ctx, this_val)));
}

static JSValue canvas_set_shadow_offset_y(JSContext *ctx, JSValueConst this_val,
                                          JSValueConst val)
{
    js_canvas_t *canvas = canvas_of(ctx, this_val);

    if (canvas != NULL) {
        js_canvas_set_shadow_offset(canvas,
                                    (float)js_canvas_get_shadow_offset_x(canvas),
                                    (float)arg_number(ctx, 1, &val, 0));
    }
    return JS_UNDEFINED;
}

static JSValue canvas_get_shadow_blur(JSContext *ctx, JSValueConst this_val)
{
    return JS_NewInt32(ctx, js_canvas_get_shadow_blur(canvas_of(ctx, this_val)));
}

static JSValue canvas_set_shadow_blur(JSContext *ctx, JSValueConst this_val,
                                      JSValueConst val)
{
    js_canvas_t *canvas = canvas_of(ctx, this_val);

    if (canvas != NULL) js_canvas_set_shadow_blur(canvas, (float)arg_number(ctx, 1, &val, 0));
    return JS_UNDEFINED;
}

/* Unsupported Canvas 2D features: present so a page keeps running. */
static JSValue canvas_noop(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv)
{
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    return JS_UNDEFINED;
}

static JSValue canvas_null(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv)
{
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    return JS_NULL;
}

static JSValue canvas_false(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv)
{
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    return JS_FALSE;
}

static const JSCFunctionListEntry s_canvas_funcs[] = {
    JS_CGETSET_DEF("fillStyle", canvas_get_fill_style, canvas_set_fill_style),
    JS_CGETSET_DEF("strokeStyle", canvas_get_stroke_style, canvas_set_stroke_style),
    JS_CGETSET_DEF("globalAlpha", canvas_get_global_alpha, canvas_set_global_alpha),
    JS_CGETSET_DEF("lineWidth", canvas_get_line_width, canvas_set_line_width),
    JS_CGETSET_DEF("font", canvas_get_font, canvas_set_font),
    JS_CGETSET_DEF("textAlign", canvas_get_text_align, canvas_set_text_align),
    JS_CGETSET_DEF("textBaseline", canvas_get_text_baseline, canvas_set_text_baseline),
    JS_CGETSET_DEF("shadowColor", canvas_get_shadow_color, canvas_set_shadow_color),
    JS_CGETSET_DEF("shadowOffsetX", canvas_get_shadow_offset_x, canvas_set_shadow_offset_x),
    JS_CGETSET_DEF("shadowOffsetY", canvas_get_shadow_offset_y, canvas_set_shadow_offset_y),
    JS_CGETSET_DEF("shadowBlur", canvas_get_shadow_blur, canvas_set_shadow_blur),
    JS_CFUNC_DEF("save", 0, canvas_save),
    JS_CFUNC_DEF("restore", 0, canvas_restore),
    JS_CFUNC_DEF("translate", 2, canvas_translate),
    JS_CFUNC_DEF("scale", 2, canvas_scale),
    JS_CFUNC_DEF("rotate", 1, canvas_rotate),
    JS_CFUNC_DEF("setTransform", 6, canvas_set_transform),
    JS_CFUNC_DEF("resetTransform", 0, canvas_reset_transform),
    JS_CFUNC_DEF("fillRect", 4, canvas_fill_rect),
    JS_CFUNC_DEF("clearRect", 4, canvas_clear_rect),
    JS_CFUNC_DEF("strokeRect", 4, canvas_stroke_rect),
    JS_CFUNC_DEF("roundRect", 4, canvas_round_rect),
    JS_CFUNC_DEF("beginPath", 0, canvas_begin_path),
    JS_CFUNC_DEF("moveTo", 2, canvas_move_to),
    JS_CFUNC_DEF("lineTo", 2, canvas_line_to),
    JS_CFUNC_DEF("closePath", 0, canvas_close_path),
    JS_CFUNC_DEF("rect", 4, canvas_rect),
    JS_CFUNC_DEF("arc", 6, canvas_arc),
    JS_CFUNC_DEF("fill", 0, canvas_fill),
    JS_CFUNC_DEF("stroke", 0, canvas_stroke),
    JS_CFUNC_DEF("fillText", 3, canvas_fill_text),
    JS_CFUNC_DEF("strokeText", 3, canvas_stroke_text),
    JS_CFUNC_DEF("measureText", 1, canvas_measure_text),
    JS_CFUNC_DEF("createLinearGradient", 4, canvas_create_linear_gradient),
    /* Documented no-ops (no images/patterns/radial gradients/clipping/pixels). */
    JS_CFUNC_DEF("drawImage", 3, canvas_noop),
    JS_CFUNC_DEF("putImageData", 3, canvas_noop),
    JS_CFUNC_DEF("clip", 0, canvas_noop),
    JS_CFUNC_DEF("setLineDash", 1, canvas_noop),
    JS_CFUNC_DEF("getLineDash", 0, canvas_noop),
    JS_CFUNC_DEF("isPointInPath", 2, canvas_false),
    JS_CFUNC_DEF("createRadialGradient", 6, canvas_null),
    JS_CFUNC_DEF("createPattern", 2, canvas_null),
    JS_CFUNC_DEF("getImageData", 4, canvas_null),
};

JSValue js_canvas_wrap_context(JSContext *ctx, js_canvas_t *canvas)
{
    JSValue object;

    if (canvas == NULL) {
        return JS_NULL;
    }
    if (s_canvas_class_id == 0) {
        JS_NewClassID(&s_canvas_class_id);
    }
    if (!JS_IsRegisteredClass(JS_GetRuntime(ctx), s_canvas_class_id)) {
        JS_NewClass(JS_GetRuntime(ctx), s_canvas_class_id, &s_canvas_class);
    }
    object = JS_NewObjectClass(ctx, s_canvas_class_id);
    if (JS_IsException(object)) {
        return object;
    }
    JS_SetOpaque(object, canvas);
    JS_SetPropertyFunctionList(ctx, object, s_canvas_funcs,
                               sizeof(s_canvas_funcs) / sizeof(s_canvas_funcs[0]));
    return object;
}
