#include "quickjs.h"
#include "jsengine.h"
#include "p4minishell_config.h"
#include "dom.h"
#include "css.h"
#include "networking.h"
#include "shell.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* The browser binds the page's DOM here for the duration of <script>
 * execution (jsengine_set_dom_tree). There is no second DOM registry. */
static JSClassID s_dom_node_class_id;

static void js_dom_node_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    (void)val;
}

static JSClassDef s_dom_node_class = {
    "DOMNode",
    .finalizer = js_dom_node_finalizer,
};

static dom_node_t *js_node_of(JSContext *ctx, JSValueConst val)
{
    (void)ctx;
    return (dom_node_t *)JS_GetOpaque(val, s_dom_node_class_id);
}

static void node_clear_children(dom_node_t *node)
{
    while (node->first_child != NULL) {
        dom_node_t *ch = node->first_child;
        dom_node_remove_child(node, ch);
        dom_node_free(ch);
    }
}

static void node_set_text(dom_node_t *node, const char *text)
{
    dom_node_t *t;

    node_clear_children(node);
    t = dom_node_new(DOM_NODE_TEXT);
    if (t != NULL) {
        snprintf(t->text, sizeof(t->text), "%s", text);
        dom_node_append_child(node, t);
    }
}

static void node_set_html(dom_node_t *node, const char *html)
{
    dom_tree_t *t;
    dom_node_t *src;

    node_clear_children(node);
    t = dom_parse(html);
    if (t == NULL) return;
    src = (t->body != NULL) ? t->body : t->root;
    while (src->first_child != NULL) {
        dom_node_t *ch = src->first_child;
        dom_node_remove_child(src, ch);
        dom_node_append_child(node, ch);
    }
    dom_free(t);
}

static void node_text_append(const dom_node_t *node, char *out, size_t out_size)
{
    size_t pos = strlen(out);

    for (const dom_node_t *ch = node->first_child; ch != NULL; ch = ch->next_sibling) {
        if (ch->type == DOM_NODE_TEXT) {
            size_t len = strlen(ch->text);
            if (pos + len + 1 >= out_size) len = out_size - pos - 1;
            memcpy(out + pos, ch->text, len);
            pos += len;
            out[pos] = '\0';
        } else {
            node_text_append(ch, out, out_size);
            pos = strlen(out);
        }
    }
}

static JSValue js_node_get_text(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    dom_node_t *node = js_node_of(ctx, this_val);
    char buf[2048];

    (void)argc;
    (void)argv;
    if (node == NULL) return JS_NewString(ctx, "");
    buf[0] = '\0';
    node_text_append(node, buf, sizeof(buf));
    return JS_NewString(ctx, buf);
}

static JSValue js_node_set_text(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    dom_node_t *node = js_node_of(ctx, this_val);
    const char *text;

    if (node == NULL || argc < 1) return JS_UNDEFINED;
    text = JS_ToCString(ctx, argv[0]);
    if (text != NULL) {
        node_set_text(node, text);
        JS_FreeCString(ctx, text);
    }
    return JS_UNDEFINED;
}

static JSValue js_node_set_html(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    dom_node_t *node = js_node_of(ctx, this_val);
    const char *text;

    if (node == NULL || argc < 1) return JS_UNDEFINED;
    text = JS_ToCString(ctx, argv[0]);
    if (text != NULL) {
        node_set_html(node, text);
        JS_FreeCString(ctx, text);
    }
    return JS_UNDEFINED;
}

static JSValue js_node_get_style(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    dom_node_t *node = js_node_of(ctx, this_val);

    (void)argc;
    (void)argv;
    return JS_NewString(ctx, node != NULL ? node->style : "");
}

static JSValue js_node_set_style(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    dom_node_t *node = js_node_of(ctx, this_val);
    const char *text;

    if (node == NULL || argc < 1) return JS_UNDEFINED;
    text = JS_ToCString(ctx, argv[0]);
    if (text != NULL) {
        snprintf(node->style, sizeof(node->style), "%s", text);
        css_apply_inline_text(node, text);
        JS_FreeCString(ctx, text);
    }
    return JS_UNDEFINED;
}

static JSValue js_node_set_attr(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    dom_node_t *node = js_node_of(ctx, this_val);
    const char *name;
    const char *value;

    if (node == NULL || argc < 2) return JS_UNDEFINED;
    name = JS_ToCString(ctx, argv[0]);
    value = JS_ToCString(ctx, argv[1]);
    if (name != NULL && value != NULL) {
        dom_set_attribute(node, name, value);
    }
    if (name != NULL) JS_FreeCString(ctx, name);
    if (value != NULL) JS_FreeCString(ctx, value);
    return JS_UNDEFINED;
}

static JSValue js_node_get_attr(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    dom_node_t *node = js_node_of(ctx, this_val);
    const char *name;
    const char *value = "";

    if (node == NULL || argc < 1) return JS_NewString(ctx, "");
    name = JS_ToCString(ctx, argv[0]);
    if (name != NULL) {
        value = dom_get_attribute(node, name);
        JS_FreeCString(ctx, name);
    }
    return JS_NewString(ctx, value != NULL ? value : "");
}

/* addEventListener('click'|'load', fn): QuickJS's JS_ToCString on a function
 * yields its source, which the browser evaluates on dispatch. */
static JSValue js_node_add_listener(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    dom_node_t *node = js_node_of(ctx, this_val);
    const char *event;
    const char *handler;

    if (node == NULL || argc < 2) return JS_UNDEFINED;
    event = JS_ToCString(ctx, argv[0]);
    handler = JS_ToCString(ctx, argv[1]);
    if (event != NULL && handler != NULL) {
        if (strcmp(event, "click") == 0) {
            snprintf(node->onclick, sizeof(node->onclick), "%s", handler);
        } else if (strcmp(event, "load") == 0) {
            snprintf(node->onload, sizeof(node->onload), "%s", handler);
        }
    }
    if (event != NULL) JS_FreeCString(ctx, event);
    if (handler != NULL) JS_FreeCString(ctx, handler);
    return JS_UNDEFINED;
}

static JSValue js_dom_node_obj(JSContext *ctx, dom_node_t *node)
{
    JSValue obj = JS_NewObjectClass(ctx, (int)s_dom_node_class_id);
    JSAtom atom;

    if (JS_IsException(obj)) return obj;
    JS_SetOpaque(obj, node);
    JS_SetPropertyStr(ctx, obj, "setAttribute",
                      JS_NewCFunction(ctx, js_node_set_attr, "setAttribute", 2));
    JS_SetPropertyStr(ctx, obj, "getAttribute",
                      JS_NewCFunction(ctx, js_node_get_attr, "getAttribute", 1));
    JS_SetPropertyStr(ctx, obj, "addEventListener",
                      JS_NewCFunction(ctx, js_node_add_listener, "addEventListener", 2));

    atom = JS_NewAtom(ctx, "textContent");
    JS_DefinePropertyGetSet(ctx, obj, atom,
                            JS_NewCFunction(ctx, js_node_get_text, "textContent", 0),
                            JS_NewCFunction(ctx, js_node_set_text, "textContent", 1),
                            JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, atom);

    atom = JS_NewAtom(ctx, "innerHTML");
    JS_DefinePropertyGetSet(ctx, obj, atom,
                            JS_NewCFunction(ctx, js_node_get_text, "innerHTML", 0),
                            JS_NewCFunction(ctx, js_node_set_html, "innerHTML", 1),
                            JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, atom);

    atom = JS_NewAtom(ctx, "style");
    JS_DefinePropertyGetSet(ctx, obj, atom,
                            JS_NewCFunction(ctx, js_node_get_style, "style", 0),
                            JS_NewCFunction(ctx, js_node_set_style, "style", 1),
                            JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, atom);
    return obj;
}

static JSValue js_document_getElementById(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    extern dom_tree_t *g_dom_tree;
    const char *id;
    dom_node_t *node;

    (void)this_val;
    if (argc < 1) return JS_NULL;
    id = JS_ToCString(ctx, argv[0]);
    if (id == NULL) return JS_NULL;
    node = g_dom_tree ? dom_find_by_id(g_dom_tree, id) : NULL;
    JS_FreeCString(ctx, id);
    if (node == NULL) return JS_NULL;
    return js_dom_node_obj(ctx, node);
}

static JSValue js_document_body(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    extern dom_tree_t *g_dom_tree;

    (void)this_val;
    (void)argc;
    (void)argv;
    if (g_dom_tree == NULL || g_dom_tree->body == NULL) return JS_NULL;
    return js_dom_node_obj(ctx, g_dom_tree->body);
}

static JSValue js_document_write(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    extern dom_tree_t *g_dom_tree;
    const char *text;

    (void)this_val;
    if (argc < 1 || g_dom_tree == NULL || g_dom_tree->body == NULL) {
        return JS_UNDEFINED;
    }
    text = JS_ToCString(ctx, argv[0]);
    if (text != NULL) {
        dom_node_t *t = dom_node_new(DOM_NODE_TEXT);
        if (t != NULL) {
            snprintf(t->text, sizeof(t->text), "%s", text);
            dom_node_append_child(g_dom_tree->body, t);
        }
        JS_FreeCString(ctx, text);
    }
    return JS_UNDEFINED;
}

static JSValue js_document_title(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    extern dom_tree_t *g_dom_tree;

    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewString(ctx, (g_dom_tree != NULL && g_dom_tree->title[0])
                                 ? g_dom_tree->title : "");
}

static JSValue js_document_set_title(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    extern dom_tree_t *g_dom_tree;
    const char *text;

    (void)this_val;
    if (argc < 1 || g_dom_tree == NULL) return JS_UNDEFINED;
    text = JS_ToCString(ctx, argv[0]);
    if (text != NULL) {
        snprintf(g_dom_tree->title, sizeof(g_dom_tree->title), "%s", text);
        JS_FreeCString(ctx, text);
    }
    return JS_UNDEFINED;
}

static JSValue js_console_log(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    /* The browser owns a TUI surface; console output would corrupt the page.
     * Diagnostics go through the shell debug log, not the transcript. */
    return JS_UNDEFINED;
}

static JSValue js_fetch(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_NewString(ctx, "");
    const char *url = JS_ToCString(ctx, argv[0]);
    if (!url) return JS_NewString(ctx, "");
    networking_http_result_t result;
    memset(&result, 0, sizeof(result));
    esp_err_t err = networking_http_get(url, &result);
    JS_FreeCString(ctx, url);
    if (err != ESP_OK) {
        return JS_NewString(ctx, "");
    }
    JSValue body = JS_NewStringLen(ctx, (const char *)result.body, result.body_size);
    free(result.body);
    return body;
}

void js_dom_bindings_register(JSContext *ctx, struct dom_tree *tree)
{
    JSValue global;
    JSValue document;
    JSAtom atom;

    (void)tree;
    if (s_dom_node_class_id == 0) {
        JS_NewClassID(&s_dom_node_class_id);
    }
    if (!JS_IsRegisteredClass(JS_GetRuntime(ctx), s_dom_node_class_id)) {
        JS_NewClass(JS_GetRuntime(ctx), s_dom_node_class_id, &s_dom_node_class);
    }

    global = JS_GetGlobalObject(ctx);
    document = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, document, "getElementById",
                      JS_NewCFunction(ctx, js_document_getElementById, "getElementById", 1));
    JS_SetPropertyStr(ctx, document, "write",
                      JS_NewCFunction(ctx, js_document_write, "write", 1));
    atom = JS_NewAtom(ctx, "body");
    JS_DefinePropertyGetSet(ctx, document, atom,
                            JS_NewCFunction(ctx, js_document_body, "body", 0),
                            JS_UNDEFINED, JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, atom);
    atom = JS_NewAtom(ctx, "title");
    JS_DefinePropertyGetSet(ctx, document, atom,
                            JS_NewCFunction(ctx, js_document_title, "title", 0),
                            JS_NewCFunction(ctx, js_document_set_title, "title", 1),
                            JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, atom);
    JS_SetPropertyStr(ctx, global, "document", document);

    {
        JSValue console = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, console, "log",
                          JS_NewCFunction(ctx, js_console_log, "log", 1));
        JS_SetPropertyStr(ctx, global, "console", console);
    }
    JS_SetPropertyStr(ctx, global, "fetch",
                      JS_NewCFunction(ctx, js_fetch, "fetch", 1));
    JS_FreeValue(ctx, global);
}
