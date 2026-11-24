#include "quickjs.h"
#include "jsengine.h"
#include "js_canvas.h"
#include "js_canvas_js.h"
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
extern dom_tree_t *g_dom_tree;

static JSClassID s_dom_node_class_id;

/** Document-level listeners live here (one registry, cleared on page load). */
#define JS_DOM_DOC_LISTENERS 8
typedef struct {
    char event[24];
    JSContext *ctx;
    JSValue callback;
    bool active;
} js_doc_listener_t;

static js_doc_listener_t s_doc_listeners[JS_DOM_DOC_LISTENERS];

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
        dom_mark_style_dirty();
        css_propagate_mutation(node);   /* re-resolves inline + cached rules */
        css_apply_inline_text(node, node->style);  /* also works with no cache */
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
        } else if (strcmp(event, "input") == 0 || strcmp(event, "change") == 0 ||
                   strcmp(event, "submit") == 0) {
            /* The cell DOM stores one interaction handler per node; document
             * listeners keep their own registry for the full event set. */
            snprintf(node->onclick, sizeof(node->onclick), "%s", handler);
        }
    }
    if (event != NULL) JS_FreeCString(ctx, event);
    if (handler != NULL) JS_FreeCString(ctx, handler);
    return JS_UNDEFINED;
}

/** canvas.getContext('2d'): the canvas bitmap lives in the js_canvas registry
 *  keyed by this DOM node, so repeated calls return the same backing store. */
static JSValue js_node_get_context(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    dom_node_t *node = js_node_of(ctx, this_val);
    const char *kind;
    js_canvas_t *canvas;
    int width;
    int height;

    if (node == NULL || strcmp(node->tag, "canvas") != 0 || argc < 1) {
        return JS_NULL;
    }
    kind = JS_ToCString(ctx, argv[0]);
    if (kind == NULL) {
        return JS_NULL;
    }
    if (strcmp(kind, "2d") != 0) {
        JS_FreeCString(ctx, kind);
        return JS_NULL;   /* no WebGL on this hardware */
    }
    JS_FreeCString(ctx, kind);

    width = node->canvas_w > 0 ? node->canvas_w : 300;   /* HTML defaults */
    height = node->canvas_h > 0 ? node->canvas_h : 150;
    canvas = js_canvas_for_node(node, width, height);
    return js_canvas_wrap_context(ctx, canvas);
}

static JSValue js_node_get_width(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    dom_node_t *node = js_node_of(ctx, this_val);

    (void)argc;
    (void)argv;

    return JS_NewInt32(ctx, node != NULL && node->canvas_w > 0
                            ? node->canvas_w : 300);
}

static JSValue js_node_set_width(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    dom_node_t *node = js_node_of(ctx, this_val);
    int32_t value = 300;
    js_canvas_t *canvas;

    if (argc >= 1) (void)JS_ToInt32(ctx, &value, argv[0]);
    if (node == NULL) return JS_UNDEFINED;
    if (value < 1) value = 1;
    if (value > JS_CANVAS_MAX_W) value = JS_CANVAS_MAX_W;
    node->canvas_w = value;
    canvas = js_canvas_find_node(node);
    if (canvas != NULL) {
        (void)js_canvas_resize(canvas, node->canvas_w,
                               node->canvas_h > 0 ? node->canvas_h : 150);
    }
    return JS_UNDEFINED;
}

static JSValue js_node_get_height(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    dom_node_t *node = js_node_of(ctx, this_val);

    (void)argc;
    (void)argv;

    return JS_NewInt32(ctx, node != NULL && node->canvas_h > 0
                            ? node->canvas_h : 150);
}

static JSValue js_node_set_height(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    dom_node_t *node = js_node_of(ctx, this_val);
    int32_t value = 150;
    js_canvas_t *canvas;

    if (argc >= 1) (void)JS_ToInt32(ctx, &value, argv[0]);
    if (node == NULL) return JS_UNDEFINED;
    if (value < 1) value = 1;
    if (value > JS_CANVAS_MAX_H) value = JS_CANVAS_MAX_H;
    node->canvas_h = value;
    canvas = js_canvas_find_node(node);
    if (canvas != NULL) {
        (void)js_canvas_resize(canvas, node->canvas_w > 0 ? node->canvas_w : 300,
                               node->canvas_h);
    }
    return JS_UNDEFINED;
}

/** node.dispatchEvent(new Event('click')) and node.click() go through the
 *  ONE dom_dispatch_event hook, so the browser and any batch caller agree. */
static JSValue js_node_dispatch_event(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    dom_node_t *node = js_node_of(ctx, this_val);

    if (node == NULL || argc < 1 || !JS_IsObject(argv[0])) return JS_FALSE;
    /* Read the type into a bounded C buffer (JS strings are transient). */
    {
        JSValue prop = JS_GetPropertyStr(ctx, argv[0], "type");
        const char *text = JS_ToCString(ctx, prop);

        if (text != NULL) {
            char event[32];

            snprintf(event, sizeof(event), "%s", text);
            dom_dispatch_event(node, event);
            JS_FreeCString(ctx, text);
        }
        JS_FreeValue(ctx, prop);
    }
    return JS_TRUE;
}

static JSValue js_node_click(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    dom_node_t *node = js_node_of(ctx, this_val);

    (void)argc;
    (void)argv;
    if (node == NULL) return JS_FALSE;
    dom_dispatch_event(node, "click");
    return JS_TRUE;
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
    JS_SetPropertyStr(ctx, obj, "dispatchEvent",
                      JS_NewCFunction(ctx, js_node_dispatch_event, "dispatchEvent", 1));
    JS_SetPropertyStr(ctx, obj, "click",
                      JS_NewCFunction(ctx, js_node_click, "click", 0));
    JS_SetPropertyStr(ctx, obj, "getContext",
                      JS_NewCFunction(ctx, js_node_get_context, "getContext", 1));

    if (node != NULL && strcmp(node->tag, "canvas") == 0) {
        atom = JS_NewAtom(ctx, "width");
        JS_DefinePropertyGetSet(ctx, obj, atom,
                                JS_NewCFunction(ctx, js_node_get_width, "width", 0),
                                JS_NewCFunction(ctx, js_node_set_width, "width", 1),
                                JS_PROP_CONFIGURABLE);
        JS_FreeAtom(ctx, atom);
        atom = JS_NewAtom(ctx, "height");
        JS_DefinePropertyGetSet(ctx, obj, atom,
                                JS_NewCFunction(ctx, js_node_get_height, "height", 0),
                                JS_NewCFunction(ctx, js_node_set_height, "height", 1),
                                JS_PROP_CONFIGURABLE);
        JS_FreeAtom(ctx, atom);
    }

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
    (void)this_val;
    shell_transcript_appendf("[console] ");
    for (int i = 0; i < argc; i++) {
        const char *text = JS_ToCString(ctx, argv[i]);

        if (text != NULL) {
            shell_transcript_appendf("%s%s", i > 0 ? " " : "", text);
            JS_FreeCString(ctx, text);
        }
    }
    shell_transcript_appendf("\n");
    return JS_UNDEFINED;
}

/* ---- console levels: diagnostics go to the transcript, never the page --- */

static JSValue js_noop_event(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    return JS_UNDEFINED;
}

static JSValue js_console_level(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv, const char *prefix)
{
    (void)this_val;
    shell_transcript_appendf("%s", prefix);
    for (int i = 0; i < argc; i++) {
        const char *text = JS_ToCString(ctx, argv[i]);

        if (text != NULL) {
            shell_transcript_appendf("%s%s", i > 0 ? " " : "", text);
            JS_FreeCString(ctx, text);
        }
    }
    shell_transcript_appendf("\n");
    return JS_UNDEFINED;
}

static JSValue js_console_warn(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    return js_console_level(ctx, this_val, argc, argv, "[warn] ");
}

static JSValue js_console_error(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    return js_console_level(ctx, this_val, argc, argv, "[error] ");
}

/* ---- Event dispatch: document + node listeners on the one runtime ------ */

static JSValue js_event_object(JSContext *ctx, const char *type,
                               const char *target_id)
{
    JSValue object = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, object, "type", JS_NewString(ctx, type != NULL ? type : ""));
    {
        JSValue target = JS_NewObject(ctx);

        JS_SetPropertyStr(ctx, target, "id", JS_NewString(ctx, target_id != NULL ? target_id : ""));
        JS_SetPropertyStr(ctx, object, "target", target);
    }
    JS_SetPropertyStr(ctx, object, "preventDefault",
                      JS_NewCFunction(ctx, js_noop_event, "preventDefault", 0));
    JS_SetPropertyStr(ctx, object, "stopPropagation",
                      JS_NewCFunction(ctx, js_noop_event, "stopPropagation", 0));
    return object;
}

static void js_doc_dispatch(JSContext *ctx, const char *event,
                            dom_node_t *node)
{
    for (int i = 0; i < JS_DOM_DOC_LISTENERS; i++) {
        js_doc_listener_t *listener = &s_doc_listeners[i];
        JSValue evt;
        JSValue result;

        if (!listener->active || listener->ctx != ctx ||
            strcmp(listener->event, event) != 0) {
            continue;
        }
        evt = js_event_object(ctx, event, node != NULL ? node->id : "");
        result = JS_Call(ctx, listener->callback, JS_UNDEFINED, 1, &evt);
        JS_FreeValue(ctx, evt);
        if (JS_IsException(result)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
        }
        JS_FreeValue(ctx, result);
    }
}

void js_dom_clear_page_state(void)
{
    for (int i = 0; i < JS_DOM_DOC_LISTENERS; i++) {
        if (s_doc_listeners[i].active) {
            JS_FreeValue(s_doc_listeners[i].ctx, s_doc_listeners[i].callback);
        }
        memset(&s_doc_listeners[i], 0, sizeof(s_doc_listeners[i]));
    }
}

/** The one DOM event hook, registered by js_dom_bindings_register(). */
static void js_dom_event_hook(void *ctx, dom_node_t *node, const char *event,
                              const char *handler)
{
    JSContext *js = (JSContext *)ctx;
    char result[128];

    if (js == NULL || event == NULL) return;
    if (handler != NULL && handler[0] != '\0') {
        (void)jsengine_eval_event(handler, event, node != NULL ? node->id : "",
                                  result, sizeof(result));
    }
    js_doc_dispatch(js, event, node);
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

static JSValue js_document_add_listener(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    const char *event;

    (void)this_val;
    if (argc < 2 || !JS_IsFunction(ctx, argv[1])) return JS_UNDEFINED;
    event = JS_ToCString(ctx, argv[0]);
    if (event == NULL) return JS_UNDEFINED;
    for (int i = 0; i < JS_DOM_DOC_LISTENERS; i++) {
        js_doc_listener_t *listener = &s_doc_listeners[i];

        if (listener->active && listener->ctx == ctx &&
            strcmp(listener->event, event) == 0) {
            JS_FreeValue(ctx, listener->callback);
            listener->callback = JS_DupValue(ctx, argv[1]);
            JS_FreeCString(ctx, event);
            return JS_UNDEFINED;
        }
        if (!listener->active) {
            snprintf(listener->event, sizeof(listener->event), "%s", event);
            listener->ctx = ctx;
            listener->callback = JS_DupValue(ctx, argv[1]);
            listener->active = true;
            break;
        }
    }
    JS_FreeCString(ctx, event);
    return JS_UNDEFINED;
}

static JSValue js_document_dispatch_event(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1 || !JS_IsObject(argv[0])) return JS_FALSE;
    {
        JSValue prop = JS_GetPropertyStr(ctx, argv[0], "type");
        const char *text = JS_ToCString(ctx, prop);

        if (text != NULL) {
            char event[32];

            snprintf(event, sizeof(event), "%s", text);
            js_doc_dispatch(ctx, event, g_dom_tree != NULL ? g_dom_tree->body : NULL);
            JS_FreeCString(ctx, text);
        }
        JS_FreeValue(ctx, prop);
    }
    return JS_TRUE;
}

static JSValue js_make_event(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    char type[32] = { 0 };

    (void)this_val;
    if (argc >= 1) {
        const char *text = JS_ToCString(ctx, argv[0]);

        if (text != NULL) {
            snprintf(type, sizeof(type), "%s", text);
            JS_FreeCString(ctx, text);
        }
    }
    return js_event_object(ctx, type, "");
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
    JS_SetPropertyStr(ctx, document, "addEventListener",
                      JS_NewCFunction(ctx, js_document_add_listener, "addEventListener", 2));
    JS_SetPropertyStr(ctx, document, "dispatchEvent",
                      JS_NewCFunction(ctx, js_document_dispatch_event, "dispatchEvent", 1));
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
        JS_SetPropertyStr(ctx, console, "warn",
                          JS_NewCFunction(ctx, js_console_warn, "warn", 1));
        JS_SetPropertyStr(ctx, console, "error",
                          JS_NewCFunction(ctx, js_console_error, "error", 1));
        JS_SetPropertyStr(ctx, console, "info",
                          JS_NewCFunction(ctx, js_console_log, "info", 1));
        JS_SetPropertyStr(ctx, global, "console", console);
    }
    JS_SetPropertyStr(ctx, global, "Event",
                      JS_NewCFunction(ctx, js_make_event, "Event", 1));
    JS_SetPropertyStr(ctx, global, "fetch",
                      JS_NewCFunction(ctx, js_fetch, "fetch", 1));
    /* The DOM dispatches through this one hook; QuickJS stays private to
     * jsengine/js_dom, and no other component learns about JS values. */
    dom_set_event_hook(js_dom_event_hook, ctx);
    js_dom_clear_page_state();
    JS_FreeValue(ctx, global);
}
