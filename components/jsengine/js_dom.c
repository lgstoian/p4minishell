#include "quickjs.h"
#include "jsengine.h"
#include "p4minishell_config.h"
#include "dom.h"
#include "networking.h"
#include "shell.h"
#include <string.h>
#include <stdlib.h>

static JSValue js_document_getElementById(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_NULL;
    const char *id = JS_ToCString(ctx, argv[0]);
    if (!id) return JS_NULL;
    extern dom_tree_t *g_dom_tree;
    dom_node_t *node = g_dom_tree ? dom_find_by_id(g_dom_tree, id) : NULL;
    JS_FreeCString(ctx, id);
    if (!node) return JS_NULL;
    return JS_NewObject(ctx);
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

static JSValue js_document_title(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    extern dom_tree_t *g_dom_tree;
    const char *title = (g_dom_tree && g_dom_tree->title[0]) ? g_dom_tree->title : "";
    return JS_NewString(ctx, title);
}

void js_dom_bindings_register(JSContext *ctx, struct dom_tree *tree)
{
    (void)tree;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue document = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, document, "getElementById",
                      JS_NewCFunction(ctx, js_document_getElementById, "getElementById", 1));
    JS_SetPropertyStr(ctx, document, "title",
                      JS_NewCFunction2(ctx, js_document_title, "title", 0, JS_CFUNC_getter, 0));
    JS_SetPropertyStr(ctx, global, "document", document);
    JS_SetPropertyStr(ctx, global, "fetch",
                      JS_NewCFunction(ctx, js_fetch, "fetch", 1));
    JS_FreeValue(ctx, global);
}
