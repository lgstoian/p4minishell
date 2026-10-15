#include "quickjs.h"
#include "jsengine.h"
#include "p4minishell_config.h"
#include "shell.h"
#include "batch.h"
#include "html.h"
#include "markdown.h"
#include "rss.h"
#include "tui.h"
#include "modal.h"
#include "modal_surf.h"
#include "storage.h"
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

static JSValue js_print(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) {
        return JS_UNDEFINED;
    }

    for (int i = 0; i < argc; i++) {
        const char *str = JS_ToCString(ctx, argv[i]);
        if (str) {
            shell_transcript_appendf("%s", str);
            JS_FreeCString(ctx, str);
        }
    }
    shell_transcript_appendf("\n");
    return JS_UNDEFINED;
}

static JSValue js_html(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) {
        return JS_NewString(ctx, "");
    }

    const char *src = JS_ToCString(ctx, argv[0]);
    if (!src) {
        return JS_NewString(ctx, "");
    }

    char *ansi = malloc(P4_CONFIG_HTML_RENDER_MAX_BYTES);
    if (!ansi) {
        JS_FreeCString(ctx, src);
        return JS_NewString(ctx, "");
    }

    size_t out_len = html_render_ansi(src, ansi, P4_CONFIG_HTML_RENDER_MAX_BYTES);
    JSValue result = JS_NewStringLen(ctx, ansi, out_len);
    free(ansi);
    JS_FreeCString(ctx, src);
    return result;
}

static JSValue js_md(JSContext *ctx, JSValueConst this_val,
                      int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) {
        return JS_NewString(ctx, "");
    }

    const char *src = JS_ToCString(ctx, argv[0]);
    if (!src) {
        return JS_NewString(ctx, "");
    }

    char *ansi = malloc(P4_CONFIG_MD_EXPORT_MAX_BYTES);
    if (!ansi) {
        JS_FreeCString(ctx, src);
        return JS_NewString(ctx, "");
    }

    size_t out_len = markdown_render_doc(src, ansi, P4_CONFIG_MD_EXPORT_MAX_BYTES);
    JSValue result = JS_NewStringLen(ctx, ansi, out_len);
    free(ansi);
    JS_FreeCString(ctx, src);
    return result;
}

static JSValue js_rss_parse(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) {
        return JS_NewString(ctx, "Error: rssParse requires XML/JSON string");
    }

    const char *data = JS_ToCString(ctx, argv[0]);
    if (!data) {
        return JS_NewString(ctx, "Error: invalid input");
    }

    rss_feed_t feed;
    memset(&feed, 0, sizeof(feed));
    esp_err_t err = rss_parse(data, strlen(data), &feed);
    JS_FreeCString(ctx, data);

    if (err != ESP_OK) {
        return JS_NewString(ctx, "Error: RSS parse failed");
    }

    char *json = malloc(P4_CONFIG_RSS_ITEM_CAPACITY * 64);
    if (!json) {
        rss_feed_free(&feed);
        return JS_NewString(ctx, "Error: out of memory");
    }

    int pos = 0;
    pos += snprintf(json + pos, P4_CONFIG_RSS_ITEM_CAPACITY * 64 - pos,
                    "{\"title\":\"%s\",\"link\":\"%s\",\"description\":\"%s\",\"items\":[",
                    feed.title[0] ? feed.title : "",
                    feed.link[0] ? feed.link : "",
                    feed.description[0] ? feed.description : "");

    for (int i = 0; i < feed.item_count && i < 20; i++) {
        rss_item_t *item = &feed.items[i];
        if (i > 0) {
            pos += snprintf(json + pos, P4_CONFIG_RSS_ITEM_CAPACITY * 64 - pos, ",");
        }
        pos += snprintf(json + pos, P4_CONFIG_RSS_ITEM_CAPACITY * 64 - pos,
                        "{\"title\":\"%s\",\"link\":\"%s\",\"summary\":\"%s\",\"author\":\"%s\",\"date\":\"%s\"}",
                        item->title[0] ? item->title : "",
                        item->link[0] ? item->link : "",
                        item->summary[0] ? item->summary : "",
                        item->author[0] ? item->author : "",
                        item->published[0] ? item->published : "");
    }

    pos += snprintf(json + pos, P4_CONFIG_RSS_ITEM_CAPACITY * 64 - pos, "]}");
    JSValue result = JS_NewString(ctx, json);
    free(json);
    rss_feed_free(&feed);
    return result;
}

static JSValue js_tui_clear(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    tui_clear();
    return JS_UNDEFINED;
}

static JSValue js_tui_print(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 3) {
        return JS_UNDEFINED;
    }

    int32_t x, y;
    JS_ToInt32(ctx, &x, argv[0]);
    JS_ToInt32(ctx, &y, argv[1]);
    const char *text = JS_ToCString(ctx, argv[2]);
    if (text) {
        tui_print_at(x, y, text, 16, 16);
        JS_FreeCString(ctx, text);
    }
    return JS_UNDEFINED;
}

static JSValue js_tui_box(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 4) {
        return JS_UNDEFINED;
    }

    int32_t x, y, w, h;
    JS_ToInt32(ctx, &x, argv[0]);
    JS_ToInt32(ctx, &y, argv[1]);
    JS_ToInt32(ctx, &w, argv[2]);
    JS_ToInt32(ctx, &h, argv[3]);
    const char *style = (argc >= 5) ? JS_ToCString(ctx, argv[4]) : "single";
    const char *title = (argc >= 6) ? JS_ToCString(ctx, argv[5]) : NULL;

    tui_draw_box(x, y, w, h, style ? style : "single", 16, 16, title);

    if (style) JS_FreeCString(ctx, style);
    if (title) JS_FreeCString(ctx, title);
    return JS_UNDEFINED;
}

static JSValue js_tui_line(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 4) {
        return JS_UNDEFINED;
    }

    int32_t x1, y1, x2, y2;
    JS_ToInt32(ctx, &x1, argv[0]);
    JS_ToInt32(ctx, &y1, argv[1]);
    JS_ToInt32(ctx, &x2, argv[2]);
    JS_ToInt32(ctx, &y2, argv[3]);
    const char *style = (argc >= 5) ? JS_ToCString(ctx, argv[4]) : "single";

    tui_draw_line(x1, y1, x2, y2, style ? style : "single", 16, 16);

    if (style) JS_FreeCString(ctx, style);
    return JS_UNDEFINED;
}

static JSValue js_tui_flush(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    tui_flush();
    return JS_UNDEFINED;
}

static JSValue js_modal_alert(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    (void)ctx; (void)this_val;
    if (argc < 2) {
        return JS_UNDEFINED;
    }

    const char *title = JS_ToCString(ctx, argv[0]);
    const char *msg = JS_ToCString(ctx, argv[1]);
    if (title && msg) {
        modal_dialog_run(title, msg, "OK", NULL, 0);
    }
    if (title) JS_FreeCString(ctx, title);
    if (msg) JS_FreeCString(ctx, msg);
    return JS_UNDEFINED;
}

static JSValue js_modal_prompt(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2) {
        return JS_UNDEFINED;
    }

    const char *prompt = JS_ToCString(ctx, argv[0]);
    const char *def = JS_ToCString(ctx, argv[1]);
    char result[256] = {0};

    if (prompt && def) {
        modal_ask_run(prompt, def[0] ? def : "", false, 0, result, sizeof(result));
    }

    if (prompt) JS_FreeCString(ctx, prompt);
    if (def) JS_FreeCString(ctx, def);

    return JS_NewString(ctx, result);
}

static JSValue js_modal_list(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2) {
        return JS_NewInt32(ctx, -1);
    }

    const char *title = JS_ToCString(ctx, argv[0]);
    const char *items_str = JS_ToCString(ctx, argv[1]);

    if (!title || !items_str) {
        if (title) JS_FreeCString(ctx, title);
        if (items_str) JS_FreeCString(ctx, items_str);
        return JS_NewInt32(ctx, -1);
    }

    char items_buf[1024];
    strncpy(items_buf, items_str, sizeof(items_buf) - 1);
    items_buf[sizeof(items_buf) - 1] = '\0';

    char *items[32];
    int count = 0;
    char *saveptr = NULL;
    char *token = strtok_r(items_buf, "|", &saveptr);
    while (token && count < 32) {
        items[count++] = token;
        token = strtok_r(NULL, "|", &saveptr);
    }

    int selected = (count > 0) ? modal_list_run(title, (const char **)items, count, 0) : -1;

    JS_FreeCString(ctx, title);
    JS_FreeCString(ctx, items_str);
    return JS_NewInt32(ctx, selected);
}

static JSValue js_batch_get(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) {
        return JS_NewString(ctx, "");
    }

    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) {
        return JS_NewString(ctx, "");
    }

    const char *val = shell_env_get(name);
    JS_FreeCString(ctx, name);
    return JS_NewString(ctx, val ? val : "");
}

static JSValue js_batch_set(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2) {
        return JS_UNDEFINED;
    }

    const char *name = JS_ToCString(ctx, argv[0]);
    const char *val = JS_ToCString(ctx, argv[1]);
    if (name && val) {
        shell_env_set(name, val);
    }
    if (name) JS_FreeCString(ctx, name);
    if (val) JS_FreeCString(ctx, val);
    return JS_UNDEFINED;
}

static JSValue js_delay(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv)
{
    (void)ctx; (void)this_val;
    if (argc < 1) {
        return JS_UNDEFINED;
    }

    int32_t ms;
    JS_ToInt32(ctx, &ms, argv[0]);
    if (ms > 0 && ms <= 10000) {
        vTaskDelay(pdMS_TO_TICKS(ms));
    }
    return JS_UNDEFINED;
}

void js_bindings_register(JSContext *ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);

    JS_SetPropertyStr(ctx, global, "print",
                      JS_NewCFunction(ctx, js_print, "print", 1));
    JS_SetPropertyStr(ctx, global, "html",
                      JS_NewCFunction(ctx, js_html, "html", 1));
    JS_SetPropertyStr(ctx, global, "md",
                      JS_NewCFunction(ctx, js_md, "md", 1));
    JS_SetPropertyStr(ctx, global, "rssParse",
                      JS_NewCFunction(ctx, js_rss_parse, "rssParse", 1));

    JSValue tui = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, tui, "clear",
                      JS_NewCFunction(ctx, js_tui_clear, "clear", 0));
    JS_SetPropertyStr(ctx, tui, "print",
                      JS_NewCFunction(ctx, js_tui_print, "print", 3));
    JS_SetPropertyStr(ctx, tui, "box",
                      JS_NewCFunction(ctx, js_tui_box, "box", 4));
    JS_SetPropertyStr(ctx, tui, "line",
                      JS_NewCFunction(ctx, js_tui_line, "line", 4));
    JS_SetPropertyStr(ctx, tui, "flush",
                      JS_NewCFunction(ctx, js_tui_flush, "flush", 0));
    JS_SetPropertyStr(ctx, global, "tui", tui);

    JSValue modal = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, modal, "alert",
                      JS_NewCFunction(ctx, js_modal_alert, "alert", 2));
    JS_SetPropertyStr(ctx, modal, "prompt",
                      JS_NewCFunction(ctx, js_modal_prompt, "prompt", 2));
    JS_SetPropertyStr(ctx, modal, "list",
                      JS_NewCFunction(ctx, js_modal_list, "list", 2));
    JS_SetPropertyStr(ctx, global, "modal", modal);

    JSValue batch = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, batch, "get",
                      JS_NewCFunction(ctx, js_batch_get, "get", 1));
    JS_SetPropertyStr(ctx, batch, "set",
                      JS_NewCFunction(ctx, js_batch_set, "set", 2));
    JS_SetPropertyStr(ctx, global, "batch", batch);

    JS_SetPropertyStr(ctx, global, "delay",
                      JS_NewCFunction(ctx, js_delay, "delay", 1));

    JS_FreeValue(ctx, global);
}
