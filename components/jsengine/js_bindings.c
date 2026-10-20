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
#include "networking.h"
#include "audio.h"
#include "locale.h"
#include "error_strings.h"
#include "event_bus.h"
#include "display.h"
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>
#include "driver/gpio.h"
#include "esp_system.h"

/* ========================================================================
 * EXISTING BINDINGS (unchanged)
 * ======================================================================== */

static JSValue js_print(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    for (int i = 0; i < argc; i++) {
        const char *str = JS_ToCString(ctx, argv[i]);
        if (str) { shell_transcript_appendf("%s", str); JS_FreeCString(ctx, str); }
    }
    shell_transcript_appendf("\n");
    return JS_UNDEFINED;
}

static JSValue js_html(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_NewString(ctx, "");
    const char *src = JS_ToCString(ctx, argv[0]);
    if (!src) return JS_NewString(ctx, "");
    char *ansi = malloc(P4_CONFIG_HTML_RENDER_MAX_BYTES);
    if (!ansi) { JS_FreeCString(ctx, src); return JS_NewString(ctx, ""); }
    size_t len = html_render_ansi(src, ansi, P4_CONFIG_HTML_RENDER_MAX_BYTES);
    JSValue r = JS_NewStringLen(ctx, ansi, len);
    free(ansi); JS_FreeCString(ctx, src);
    return r;
}

static JSValue js_md(JSContext *ctx, JSValueConst this_val,
                      int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_NewString(ctx, "");
    const char *src = JS_ToCString(ctx, argv[0]);
    if (!src) return JS_NewString(ctx, "");
    char *ansi = malloc(P4_CONFIG_MD_EXPORT_MAX_BYTES);
    if (!ansi) { JS_FreeCString(ctx, src); return JS_NewString(ctx, ""); }
    size_t len = markdown_render_doc(src, ansi, P4_CONFIG_MD_EXPORT_MAX_BYTES);
    JSValue r = JS_NewStringLen(ctx, ansi, len);
    free(ansi); JS_FreeCString(ctx, src);
    return r;
}

static JSValue js_rss_parse(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_NewString(ctx, "Error: rssParse requires XML/JSON string");
    const char *data = JS_ToCString(ctx, argv[0]);
    if (!data) return JS_NewString(ctx, "Error: invalid input");
    rss_feed_t feed;
    memset(&feed, 0, sizeof(feed));
    if (rss_parse(data, strlen(data), &feed) != ESP_OK) {
        JS_FreeCString(ctx, data);
        return JS_NewString(ctx, "Error: RSS parse failed");
    }
    JS_FreeCString(ctx, data);
    char *json = malloc(P4_CONFIG_RSS_ITEM_CAPACITY * 64);
    if (!json) { rss_feed_free(&feed); return JS_NewString(ctx, "Error: out of memory"); }
    int pos = 0;
    pos += snprintf(json + pos, P4_CONFIG_RSS_ITEM_CAPACITY * 64 - pos,
                    "{\"title\":\"%s\",\"link\":\"%s\",\"description\":\"%s\",\"items\":[",
                    feed.title[0] ? feed.title : "", feed.link[0] ? feed.link : "",
                    feed.description[0] ? feed.description : "");
    for (int i = 0; i < feed.item_count && i < 20; i++) {
        rss_item_t *item = &feed.items[i];
        if (i > 0) pos += snprintf(json + pos, P4_CONFIG_RSS_ITEM_CAPACITY * 64 - pos, ",");
        pos += snprintf(json + pos, P4_CONFIG_RSS_ITEM_CAPACITY * 64 - pos,
                        "{\"title\":\"%s\",\"link\":\"%s\",\"summary\":\"%s\",\"author\":\"%s\",\"date\":\"%s\"}",
                        item->title[0] ? item->title : "", item->link[0] ? item->link : "",
                        item->summary[0] ? item->summary : "", item->author[0] ? item->author : "",
                        item->published[0] ? item->published : "");
    }
    pos += snprintf(json + pos, P4_CONFIG_RSS_ITEM_CAPACITY * 64 - pos, "]}");
    JSValue r = JS_NewString(ctx, json);
    free(json); rss_feed_free(&feed);
    return r;
}

static JSValue js_tui_clear(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    tui_clear(); return JS_UNDEFINED;
}

static JSValue js_tui_print(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 3) return JS_UNDEFINED;
    int32_t x, y;
    JS_ToInt32(ctx, &x, argv[0]);
    JS_ToInt32(ctx, &y, argv[1]);
    const char *text = JS_ToCString(ctx, argv[2]);
    if (text) { tui_print_at(x, y, text, 16, 16); JS_FreeCString(ctx, text); }
    return JS_UNDEFINED;
}

static JSValue js_tui_box(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 4) return JS_UNDEFINED;
    int32_t x, y, w, h;
    JS_ToInt32(ctx, &x, argv[0]); JS_ToInt32(ctx, &y, argv[1]);
    JS_ToInt32(ctx, &w, argv[2]); JS_ToInt32(ctx, &h, argv[3]);
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
    if (argc < 4) return JS_UNDEFINED;
    int32_t x1, y1, x2, y2;
    JS_ToInt32(ctx, &x1, argv[0]); JS_ToInt32(ctx, &y1, argv[1]);
    JS_ToInt32(ctx, &x2, argv[2]); JS_ToInt32(ctx, &y2, argv[3]);
    const char *style = (argc >= 5) ? JS_ToCString(ctx, argv[4]) : "single";
    tui_draw_line(x1, y1, x2, y2, style ? style : "single", 16, 16);
    if (style) JS_FreeCString(ctx, style);
    return JS_UNDEFINED;
}

static JSValue js_tui_flush(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    tui_flush(); return JS_UNDEFINED;
}

static JSValue js_modal_alert(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    (void)ctx; (void)this_val;
    if (argc < 2) return JS_UNDEFINED;
    const char *title = JS_ToCString(ctx, argv[0]);
    const char *msg = JS_ToCString(ctx, argv[1]);
    if (title && msg) modal_dialog_run(title, msg, "OK", NULL, 0);
    if (title) JS_FreeCString(ctx, title);
    if (msg) JS_FreeCString(ctx, msg);
    return JS_UNDEFINED;
}

static JSValue js_modal_prompt(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2) return JS_UNDEFINED;
    const char *prompt = JS_ToCString(ctx, argv[0]);
    const char *def = JS_ToCString(ctx, argv[1]);
    char result[256] = {0};
    if (prompt && def) modal_ask_run(prompt, def[0] ? def : "", false, 0, result, sizeof(result));
    if (prompt) JS_FreeCString(ctx, prompt);
    if (def) JS_FreeCString(ctx, def);
    return JS_NewString(ctx, result);
}

static JSValue js_modal_list(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2) return JS_NewInt32(ctx, -1);
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
    while (token && count < 32) { items[count++] = token; token = strtok_r(NULL, "|", &saveptr); }
    int selected = (count > 0) ? modal_list_run(title, (const char **)items, count, 0) : -1;
    JS_FreeCString(ctx, title);
    JS_FreeCString(ctx, items_str);
    return JS_NewInt32(ctx, selected);
}

static JSValue js_batch_get(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_NewString(ctx, "");
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_NewString(ctx, "");
    const char *val = shell_env_get(name);
    JS_FreeCString(ctx, name);
    return JS_NewString(ctx, val ? val : "");
}

static JSValue js_batch_set(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2) return JS_UNDEFINED;
    const char *name = JS_ToCString(ctx, argv[0]);
    const char *val = JS_ToCString(ctx, argv[1]);
    if (name && val) shell_env_set(name, val);
    if (name) JS_FreeCString(ctx, name);
    if (val) JS_FreeCString(ctx, val);
    return JS_UNDEFINED;
}

static JSValue js_delay(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv)
{
    (void)ctx; (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    int32_t ms;
    JS_ToInt32(ctx, &ms, argv[0]);
    if (ms > 0 && ms <= 10000) vTaskDelay(pdMS_TO_TICKS(ms));
    return JS_UNDEFINED;
}

/* ========================================================================
 * NEW BINDINGS: File I/O
 * ======================================================================== */

static JSValue js_fs_read(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_NewString(ctx, "");
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_NewString(ctx, "");
    char resolved[256];
    if (shell_sd_resolve_path(path, resolved, sizeof(resolved)) != ESP_OK) {
        JS_FreeCString(ctx, path); return JS_NewString(ctx, "");
    }
    JS_FreeCString(ctx, path);
    FILE *f = fopen(resolved, "rb");
    if (!f) return JS_NewString(ctx, "");
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > P4_CONFIG_JS_FS_MAX_BYTES) { fclose(f); return JS_NewString(ctx, ""); }
    char *buf = malloc(size + 1);
    if (!buf) { fclose(f); return JS_NewString(ctx, ""); }
    size_t nread = fread(buf, 1, size, f);
    fclose(f);
    buf[nread] = '\0';
    JSValue r = JS_NewStringLen(ctx, buf, nread);
    free(buf); return r;
}

static JSValue js_fs_write(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2) return JS_NewBool(ctx, false);
    const char *path = JS_ToCString(ctx, argv[0]);
    const char *data = JS_ToCString(ctx, argv[1]);
    if (!path || !data) { if (path) JS_FreeCString(ctx, path); if (data) JS_FreeCString(ctx, data); return JS_NewBool(ctx, false); }
    char resolved[256];
    if (shell_sd_resolve_path(path, resolved, sizeof(resolved)) != ESP_OK) {
        JS_FreeCString(ctx, path); JS_FreeCString(ctx, data); return JS_NewBool(ctx, false);
    }
    JS_FreeCString(ctx, path);
    FILE *f = fopen(resolved, "wb");
    if (!f) { JS_FreeCString(ctx, data); return JS_NewBool(ctx, false); }
    size_t len = strlen(data);
    size_t nwrote = fwrite(data, 1, len, f);
    fclose(f); JS_FreeCString(ctx, data);
    return JS_NewBool(ctx, nwrote == len);
}

static JSValue js_fs_exists(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_NewBool(ctx, false);
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_NewBool(ctx, false);
    char resolved[256];
    if (shell_sd_resolve_path(path, resolved, sizeof(resolved)) != ESP_OK) {
        JS_FreeCString(ctx, path); return JS_NewBool(ctx, false);
    }
    JS_FreeCString(ctx, path);
    struct stat st;
    return JS_NewBool(ctx, stat(resolved, &st) == 0);
}

static JSValue js_fs_remove(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_NewBool(ctx, false);
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_NewBool(ctx, false);
    char resolved[256];
    if (shell_sd_resolve_path(path, resolved, sizeof(resolved)) != ESP_OK) {
        JS_FreeCString(ctx, path); return JS_NewBool(ctx, false);
    }
    JS_FreeCString(ctx, path);
    return JS_NewBool(ctx, remove(resolved) == 0);
}

/* ========================================================================
 * NEW BINDINGS: System
 * ======================================================================== */

static JSValue js_sys_info(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t total_heap = heap_caps_get_total_size(MALLOC_CAP_8BIT);
    uint32_t uptime_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    uint32_t task_count = uxTaskGetNumberOfTasks();
    char json[512];
    snprintf(json, sizeof(json),
             "{\"heap_free\":%u,\"heap_total\":%u,\"uptime_ms\":%lu,\"tasks\":%u,"
             "\"wifi\":%s,\"brightness\":%d,\"volume\":%d}",
             (unsigned)free_heap, (unsigned)total_heap, (unsigned long)uptime_ms,
             (unsigned)task_count, networking_wifi_is_connected() ? "true" : "false",
             display_get_brightness(), audio_get_volume());
    return JS_NewString(ctx, json);
}

static JSValue js_sys_reboot(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    esp_restart();
    return JS_UNDEFINED;
}

/* ========================================================================
 * NEW BINDINGS: HTTP / GPIO / Audio / Locale / Errors / Event Bus
 * ======================================================================== */

static JSValue js_http_get(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_NewString(ctx, "Error: http.get requires URL");
    const char *url = JS_ToCString(ctx, argv[0]);
    if (!url) return JS_NewString(ctx, "Error: invalid URL");
    networking_http_result_t result;
    memset(&result, 0, sizeof(result));
    if (networking_http_get(url, &result) != ESP_OK) {
        JS_FreeCString(ctx, url); return JS_NewString(ctx, "Error: HTTP request failed");
    }
    JS_FreeCString(ctx, url);
    size_t copy_len = result.body_size < P4_CONFIG_JS_HTTP_MAX_BYTES ? result.body_size : P4_CONFIG_JS_HTTP_MAX_BYTES;
    char *buf = malloc(copy_len + 1);
    if (!buf) { networking_http_result_free(&result); return JS_NewString(ctx, "Error: out of memory"); }
    memcpy(buf, result.body, copy_len); buf[copy_len] = '\0';
    networking_http_result_free(&result);
    JSValue r = JS_NewStringLen(ctx, buf, copy_len);
    free(buf); return r;
}

static JSValue js_gpio_read(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_NewInt32(ctx, -1);
    int32_t pin;
    JS_ToInt32(ctx, &pin, argv[0]);
    if (pin < 0 || pin >= GPIO_NUM_MAX) return JS_NewInt32(ctx, -1);
    return JS_NewInt32(ctx, (int)gpio_get_level((gpio_num_t)pin));
}

static JSValue js_gpio_write(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2) return JS_NewBool(ctx, false);
    int32_t pin, val;
    JS_ToInt32(ctx, &pin, argv[0]);
    JS_ToInt32(ctx, &val, argv[1]);
    if (pin < 0 || pin >= GPIO_NUM_MAX) return JS_NewBool(ctx, false);
    gpio_set_level((gpio_num_t)pin, (uint32_t)(val ? 1 : 0));
    return JS_NewBool(ctx, true);
}

static JSValue js_audio_play(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_NewBool(ctx, false);
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_NewBool(ctx, false);
    char resolved[256];
    if (shell_sd_resolve_path(path, resolved, sizeof(resolved)) != ESP_OK) {
        JS_FreeCString(ctx, path); return JS_NewBool(ctx, false);
    }
    JS_FreeCString(ctx, path);
    return JS_NewBool(ctx, audio_play_wav(resolved));
}

static JSValue js_audio_stop(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    audio_stop(); return JS_UNDEFINED;
}

static JSValue js_audio_volume(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_NewInt32(ctx, audio_get_volume());
    int32_t level;
    JS_ToInt32(ctx, &level, argv[0]);
    if (level < 0) level = 0;
    if (level > 100) level = 100;
    audio_set_volume((int)level);
    return JS_NewInt32(ctx, audio_get_volume());
}

static JSValue js_locale_set(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    int32_t id;
    JS_ToInt32(ctx, &id, argv[0]);
    if (id >= 0 && id < (int)LOCALE_COUNT) locale_set((locale_id_t)id);
    return JS_UNDEFINED;
}

static JSValue js_locale_get(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return JS_NewInt32(ctx, (int)locale_get());
}

static JSValue js_locale_name(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    (void)this_val;
    int32_t id = (int32_t)locale_get();
    if (argc >= 1) JS_ToInt32(ctx, &id, argv[0]);
    return JS_NewString(ctx, locale_name((locale_id_t)id));
}

static JSValue js_errors_code(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return JS_NewInt32(ctx, batch_get_errorcode());
}

static JSValue js_errors_message(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    (void)this_val;
    int32_t code = (int32_t)batch_get_errorcode();
    if (argc >= 1) JS_ToInt32(ctx, &code, argv[0]);
    return JS_NewString(ctx, error_code_string((int)code));
}

static JSValue js_event_publish(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2) return JS_NewBool(ctx, false);
    const char *topic = JS_ToCString(ctx, argv[0]);
    if (!topic) return JS_NewBool(ctx, false);
    const char *data = JS_ToCString(ctx, argv[1]);
    if (!data) { JS_FreeCString(ctx, topic); return JS_NewBool(ctx, false); }
    bool ok = event_bus_publish(topic, data, strlen(data));
    JS_FreeCString(ctx, topic); JS_FreeCString(ctx, data);
    return JS_NewBool(ctx, ok);
}

static JSValue js_event_subscribers(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return JS_NewInt32(ctx, event_bus_subscriber_count());
}

/* ========================================================================
 * REGISTRATION
 * ======================================================================== */

void js_bindings_register(JSContext *ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);

    JS_SetPropertyStr(ctx, global, "print", JS_NewCFunction(ctx, js_print, "print", 1));
    JS_SetPropertyStr(ctx, global, "html", JS_NewCFunction(ctx, js_html, "html", 1));
    JS_SetPropertyStr(ctx, global, "md", JS_NewCFunction(ctx, js_md, "md", 1));
    JS_SetPropertyStr(ctx, global, "rssParse", JS_NewCFunction(ctx, js_rss_parse, "rssParse", 1));

    JSValue tui = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, tui, "clear", JS_NewCFunction(ctx, js_tui_clear, "clear", 0));
    JS_SetPropertyStr(ctx, tui, "print", JS_NewCFunction(ctx, js_tui_print, "print", 3));
    JS_SetPropertyStr(ctx, tui, "box", JS_NewCFunction(ctx, js_tui_box, "box", 4));
    JS_SetPropertyStr(ctx, tui, "line", JS_NewCFunction(ctx, js_tui_line, "line", 4));
    JS_SetPropertyStr(ctx, tui, "flush", JS_NewCFunction(ctx, js_tui_flush, "flush", 0));
    JS_SetPropertyStr(ctx, global, "tui", tui);

    JSValue modal = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, modal, "alert", JS_NewCFunction(ctx, js_modal_alert, "alert", 2));
    JS_SetPropertyStr(ctx, modal, "prompt", JS_NewCFunction(ctx, js_modal_prompt, "prompt", 2));
    JS_SetPropertyStr(ctx, modal, "list", JS_NewCFunction(ctx, js_modal_list, "list", 2));
    JS_SetPropertyStr(ctx, global, "modal", modal);

    JSValue batch = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, batch, "get", JS_NewCFunction(ctx, js_batch_get, "get", 1));
    JS_SetPropertyStr(ctx, batch, "set", JS_NewCFunction(ctx, js_batch_set, "set", 2));
    JS_SetPropertyStr(ctx, global, "batch", batch);

    JS_SetPropertyStr(ctx, global, "delay", JS_NewCFunction(ctx, js_delay, "delay", 1));

    JSValue fs = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, fs, "read", JS_NewCFunction(ctx, js_fs_read, "read", 1));
    JS_SetPropertyStr(ctx, fs, "write", JS_NewCFunction(ctx, js_fs_write, "write", 2));
    JS_SetPropertyStr(ctx, fs, "exists", JS_NewCFunction(ctx, js_fs_exists, "exists", 1));
    JS_SetPropertyStr(ctx, fs, "remove", JS_NewCFunction(ctx, js_fs_remove, "remove", 1));
    JS_SetPropertyStr(ctx, global, "fs", fs);

    JSValue sys = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, sys, "info", JS_NewCFunction(ctx, js_sys_info, "info", 0));
    JS_SetPropertyStr(ctx, sys, "reboot", JS_NewCFunction(ctx, js_sys_reboot, "reboot", 0));
    JS_SetPropertyStr(ctx, global, "sys", sys);

    JSValue http = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, http, "get", JS_NewCFunction(ctx, js_http_get, "get", 1));
    JS_SetPropertyStr(ctx, global, "http", http);

    JSValue gpio = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, gpio, "read", JS_NewCFunction(ctx, js_gpio_read, "read", 1));
    JS_SetPropertyStr(ctx, gpio, "write", JS_NewCFunction(ctx, js_gpio_write, "write", 2));
    JS_SetPropertyStr(ctx, global, "gpio", gpio);

    JSValue audio = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, audio, "play", JS_NewCFunction(ctx, js_audio_play, "play", 1));
    JS_SetPropertyStr(ctx, audio, "stop", JS_NewCFunction(ctx, js_audio_stop, "stop", 0));
    JS_SetPropertyStr(ctx, audio, "volume", JS_NewCFunction(ctx, js_audio_volume, "volume", 1));
    JS_SetPropertyStr(ctx, global, "audio", audio);

    JSValue locale = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, locale, "set", JS_NewCFunction(ctx, js_locale_set, "set", 1));
    JS_SetPropertyStr(ctx, locale, "get", JS_NewCFunction(ctx, js_locale_get, "get", 0));
    JS_SetPropertyStr(ctx, locale, "name", JS_NewCFunction(ctx, js_locale_name, "name", 1));
    JS_SetPropertyStr(ctx, global, "locale", locale);

    JSValue errors = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, errors, "code", JS_NewCFunction(ctx, js_errors_code, "code", 0));
    JS_SetPropertyStr(ctx, errors, "message", JS_NewCFunction(ctx, js_errors_message, "message", 1));
    JS_SetPropertyStr(ctx, global, "errors", errors);

    JSValue event = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, event, "publish", JS_NewCFunction(ctx, js_event_publish, "publish", 2));
    JS_SetPropertyStr(ctx, event, "subscribers", JS_NewCFunction(ctx, js_event_subscribers, "subscribers", 0));
    JS_SetPropertyStr(ctx, global, "event", event);

    JS_FreeValue(ctx, global);
}
