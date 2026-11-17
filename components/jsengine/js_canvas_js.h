/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file js_canvas_js.h
 * @brief QuickJS binding for the Canvas 2D core (private to jsengine).
 */
#ifndef P4MINISHELL_JS_CANVAS_JS_H
#define P4MINISHELL_JS_CANVAS_JS_H

#include "quickjs.h"
#include "js_canvas.h"

/** Wrap @p canvas as the JS object returned by canvas.getContext('2d').
 *  The registry owns the C canvas; the JS object only references it. */
JSValue js_canvas_wrap_context(JSContext *ctx, js_canvas_t *canvas);

#endif /* P4MINISHELL_JS_CANVAS_JS_H */
