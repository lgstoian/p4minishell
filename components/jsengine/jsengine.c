#include "jsengine.h"
#include "quickjs.h"
#include "p4minishell_config.h"
#include "shell.h"
#include "batch.h"
#include "storage.h"
#include <string.h>
#include <stdlib.h>
#include "esp_timer.h"
#include "esp_heap_caps.h"

static JSRuntime *s_rt = NULL;
static JSContext *s_ctx = NULL;
static uint32_t s_timeout_ms = 5000;
static int64_t s_start_time = 0;
static volatile bool s_interrupt_flag = false;

struct dom_tree *g_dom_tree = NULL;

static void *js_psram_malloc(JSMallocState *s, size_t size)
{
    (void)s;
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
}

static void js_psram_free(JSMallocState *s, void *ptr)
{
    (void)s;
    heap_caps_free(ptr);
}

static void *js_psram_realloc(JSMallocState *s, void *ptr, size_t size)
{
    (void)s;
    return heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM);
}

static size_t js_psram_malloc_usable_size(const void *ptr)
{
    return 0;
}

static int js_interrupt_handler(JSRuntime *rt, void *opaque)
{
    (void)rt;
    (void)opaque;
    if (s_interrupt_flag) {
        return 1;
    }
    if (s_timeout_ms > 0) {
        int64_t elapsed = (esp_timer_get_time() - s_start_time) / 1000;
        if (elapsed > (int64_t)s_timeout_ms) {
            return 1;
        }
    }
    return 0;
}

static void js_error_to_string(JSContext *ctx, char *result, size_t result_size)
{
    JSValue exc = JS_GetException(ctx);
    const char *str = JS_ToCString(ctx, exc);
    if (str) {
        snprintf(result, result_size, "Error: %s", str);
        JS_FreeCString(ctx, str);
    } else {
        snprintf(result, result_size, "Error: unknown");
    }
    JS_FreeValue(ctx, exc);
}

static bool js_eval_internal(const char *code, const char *filename,
                             char *result, size_t result_size)
{
    s_interrupt_flag = false;
    s_start_time = esp_timer_get_time();

    JSValue val = JS_Eval(s_ctx, code, strlen(code), filename, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(val)) {
        js_error_to_string(s_ctx, result, result_size);
        JS_FreeValue(s_ctx, val);
        return false;
    }

    const char *str = JS_ToCString(s_ctx, val);
    if (str) {
        snprintf(result, result_size, "%s", str);
        JS_FreeCString(s_ctx, str);
    } else {
        result[0] = '\0';
    }
    JS_FreeValue(s_ctx, val);
    return true;
}

static char *read_file_to_heap(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > P4_CONFIG_JS_FILE_MAX_BYTES) {
        fclose(f);
        return NULL;
    }

    char *buf = malloc(size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    size_t read = fread(buf, 1, size, f);
    fclose(f);
    buf[read] = '\0';
    return buf;
}

bool jsengine_init(void)
{
    if (s_rt) {
        return true;
    }

    JSMallocFunctions mf = {
        .js_malloc = js_psram_malloc,
        .js_free = js_psram_free,
        .js_realloc = js_psram_realloc,
        .js_malloc_usable_size = js_psram_malloc_usable_size,
    };
    s_rt = JS_NewRuntime2(&mf, NULL);
    if (!s_rt) {
        return false;
    }

    JS_SetRuntimeInfo(s_rt, "p4minishell");
    JS_SetMemoryLimit(s_rt, P4_CONFIG_JS_HEAP_MAX_BYTES);
    JS_SetMaxStackSize(s_rt, P4_CONFIG_JS_STACK_MAX_BYTES);
    JS_SetInterruptHandler(s_rt, js_interrupt_handler, NULL);
    JS_SetGCThreshold(s_rt, P4_CONFIG_JS_HEAP_MAX_BYTES / 2);

    s_ctx = JS_NewContext(s_rt);
    if (!s_ctx) {
        JS_FreeRuntime(s_rt);
        s_rt = NULL;
        return false;
    }

    js_bindings_register(s_ctx);
    js_dom_bindings_register(s_ctx, g_dom_tree);
    return true;
}

void jsengine_deinit(void)
{
    if (s_ctx) {
        JS_FreeContext(s_ctx);
        s_ctx = NULL;
    }
    if (s_rt) {
        JS_FreeRuntime(s_rt);
        s_rt = NULL;
    }
}

bool jsengine_eval(const char *code, char *result, size_t result_size)
{
    if (!s_ctx || !code || !result || result_size == 0) {
        return false;
    }
    return js_eval_internal(code, "<eval>", result, result_size);
}

bool jsengine_run_file(const char *path, char *result, size_t result_size)
{
    if (!s_ctx || !path || !result || result_size == 0) {
        return false;
    }

    char *code = read_file_to_heap(path);
    if (!code) {
        snprintf(result, result_size, "Error: cannot read file %s", path);
        return false;
    }

    bool ok = js_eval_internal(code, path, result, result_size);
    free(code);
    return ok;
}

bool jsengine_call(const char *path, const char *func,
                   const char *args_json, char *result, size_t result_size)
{
    if (!s_ctx || !path || !func || !result || result_size == 0) {
        return false;
    }

    char *code = read_file_to_heap(path);
    if (!code) {
        snprintf(result, result_size, "Error: cannot read file %s", path);
        return false;
    }

    char wrapper[512];
    if (args_json && args_json[0]) {
        snprintf(wrapper, sizeof(wrapper),
                 "(function(){var module={exports:{}};(function(exports,require,module,__filename,__dirname){%s\n})({exports:module.exports},null,module,'%s','');return typeof %s!=='undefined'?%s(%s):null;})()",
                 code, path, func, func, args_json);
    } else {
        snprintf(wrapper, sizeof(wrapper),
                 "(function(){var module={exports:{}};(function(exports,require,module,__filename,__dirname){%s\n})({exports:module.exports},null,module,'%s','');return typeof %s!=='undefined'?%s():null;})()",
                 code, path, func, func);
    }

    bool ok = js_eval_internal(wrapper, path, result, result_size);
    free(code);
    return ok;
}

void jsengine_set_timeout_ms(uint32_t ms)
{
    s_timeout_ms = ms;
}

void jsengine_set_heap_limit(size_t bytes)
{
    if (s_rt) {
        JS_SetMemoryLimit(s_rt, bytes);
    }
}
