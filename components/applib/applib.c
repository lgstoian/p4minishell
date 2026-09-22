/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file applib.c
 * @brief Native-app runtime library implementation.
 *
 * The four service groups defined in `applib.h`:
 *   1. Console output through the shell transcript (the redirection layer).
 *   2. A single memory-allocation policy plus debug-log error reporting.
 *   3. Time / timer / sleep / system-info helpers over clock and FreeRTOS.
 *   4. Wi-Fi state accessors routed through a registered ops table.
 *
 * Layering: this component depends only on `shell` and `clock` (plus the
 * FreeRTOS / heap / esp_timer IDF components). It never includes
 * `networking.h`; the Wi-Fi accessors call through `applib_net_ops_t`, which
 * `command_init()` registers with `networking` wrappers.
 */

#include "applib.h"
#include "shell.h"
#include "clock.h"
#include "storage.h"
#include "db.h"
#include "ansi.h"
#include "p4minishell_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "p4heap.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define APPLIB_PSRAM_THRESHOLD P4_CONFIG_APPLIB_PSRAM_THRESHOLD_BYTES
#define APPLIB_LINE_BYTES      P4_CONFIG_COMMAND_BYTES

/* ========================================================================
 * 1. CONSOLE OUTPUT
 * ======================================================================== */

/**
 * Format into a command-sized heap buffer and append it to the transcript.
 * The buffer is command-sized because app lines may be long; it is transient
 * and freed immediately, so it never sits on the app's task stack.
 */
static int app_vformat_append(bool ansi, const char *format, va_list args)
{
    char *buffer;
    int written;

    if (format == NULL) {
        return 0;
    }

    buffer = malloc(APPLIB_LINE_BYTES);
    if (buffer == NULL) {
        return -1;
    }

    if (ansi) {
        written = ansi_vformat(buffer, APPLIB_LINE_BYTES, format, args);
    } else {
        written = vsnprintf(buffer, APPLIB_LINE_BYTES, format, args);
    }
    if (written < 0) {
        free(buffer);
        return -1;
    }

    if (ansi) {
        shell_transcript_append_ansi(buffer);
    } else {
        shell_transcript_append_text(buffer);
    }

    free(buffer);
    return written;
}

int app_vprintf(const char *format, va_list args)
{
    return app_vformat_append(false, format, args);
}

int app_printf(const char *format, ...)
{
    va_list args;
    int written;

    va_start(args, format);
    written = app_vformat_append(false, format, args);
    va_end(args);
    return written;
}

int app_vprintf_ansi(const char *format, va_list args)
{
    return app_vformat_append(true, format, args);
}

int app_printf_ansi(const char *format, ...)
{
    va_list args;
    int written;

    va_start(args, format);
    written = app_vformat_append(true, format, args);
    va_end(args);
    return written;
}

int app_print_styled(const char *sgr_codes, const char *format, ...)
{
    char *text = NULL;
    char *styled = NULL;
    va_list args;
    int written = -1;

    if (sgr_codes == NULL || format == NULL) {
        return 0;
    }
    text = malloc(APPLIB_LINE_BYTES);
    styled = malloc(APPLIB_LINE_BYTES + 32);
    if (text == NULL || styled == NULL) {
        free(text);
        free(styled);
        return -1;
    }

    va_start(args, format);
    vsnprintf(text, APPLIB_LINE_BYTES, format, args);
    va_end(args);

    written = snprintf(styled, APPLIB_LINE_BYTES + 32, "\x1b[%sm%s\x1b[0m", sgr_codes, text);
    shell_transcript_append_ansi(styled);
    free(text);
    free(styled);
    return written;
}

/**
 * Semantic helpers format the caller's text first and pass it to the shell
 * helper as a `%s` argument, so a literal `%` or `@` in app data stays data.
 */

/** Format @p format into a command-sized heap buffer. Returns NULL on OOM. */
static char *app_format_text(const char *format, va_list args)
{
    char *buffer;

    if (format == NULL) {
        return NULL;
    }
    buffer = malloc(APPLIB_LINE_BYTES);
    if (buffer == NULL) {
        return NULL;
    }
    vsnprintf(buffer, APPLIB_LINE_BYTES, format, args);
    return buffer;
}

void app_print_heading(const char *format, ...)
{
    va_list args;
    char *buffer;

    va_start(args, format);
    buffer = app_format_text(format, args);
    va_end(args);
    if (buffer != NULL) {
        shell_print_heading("%s", buffer);
        free(buffer);
    }
}

void app_print_ok(const char *format, ...)
{
    va_list args;
    char *buffer;

    va_start(args, format);
    buffer = app_format_text(format, args);
    va_end(args);
    if (buffer != NULL) {
        shell_print_ok("%s", buffer);
        free(buffer);
    }
}

void app_print_error(const char *format, ...)
{
    va_list args;
    char *buffer;

    va_start(args, format);
    buffer = app_format_text(format, args);
    va_end(args);
    if (buffer != NULL) {
        shell_print_error("%s", buffer);
        free(buffer);
    }
}

void app_print_warning(const char *format, ...)
{
    va_list args;
    char *buffer;

    va_start(args, format);
    buffer = app_format_text(format, args);
    va_end(args);
    if (buffer != NULL) {
        shell_print_warning("%s", buffer);
        free(buffer);
    }
}

void app_print_muted(const char *format, ...)
{
    va_list args;
    char *buffer;

    va_start(args, format);
    buffer = app_format_text(format, args);
    va_end(args);
    if (buffer != NULL) {
        shell_print_muted("%s", buffer);
        free(buffer);
    }
}

void app_print_usage(const char *format, ...)
{
    va_list args;
    char *buffer;

    va_start(args, format);
    buffer = app_format_text(format, args);
    va_end(args);
    if (buffer != NULL) {
        shell_print_usage("%s", buffer);
        free(buffer);
    }
}

void app_print_field(const char *label, const char *format, ...)
{
    va_list args;
    char *buffer;

    va_start(args, format);
    buffer = app_format_text(format, args);
    va_end(args);
    if (buffer != NULL) {
        shell_print_field(label, "%s", buffer);
        free(buffer);
    }
}

/* ========================================================================
 * 2. MEMORY ALLOCATION POLICY + ERROR REPORTING
 * ======================================================================== */

void *app_alloc(size_t size)
{
    if (size == 0) {
        return NULL;
    }
    if (size >= APPLIB_PSRAM_THRESHOLD) {
        void *block = p4heap_alloc_psram(size);

        if (block != NULL) {
            return block;
        }
        /* PSRAM absent or exhausted: fall back to the internal heap. */
    }
    return malloc(size);
}

void *app_calloc(size_t count, size_t size)
{
    void *block;

    if (count != 0 && size > (size_t)-1 / count) {
        return NULL;
    }
    block = app_alloc(count * size);
    if (block != NULL) {
        memset(block, 0, count * size);
    }
    return block;
}

void *app_realloc(void *ptr, size_t size)
{
    size_t old_size;
    size_t copy_size;
    void *block;

    if (ptr == NULL) {
        return app_alloc(size);
    }
    if (size == 0) {
        app_free(ptr);
        return NULL;
    }
    block = app_alloc(size);
    if (block == NULL) {
        return NULL;
    }
    /* Copy only the bytes that exist in both blocks. */
    old_size = heap_caps_get_allocated_size(ptr);
    copy_size = (old_size < size) ? old_size : size;
    memcpy(block, ptr, copy_size);
    app_free(ptr);
    return block;
}

char *app_strdup(const char *s)
{
    size_t len;
    char *copy;

    if (s == NULL) {
        return NULL;
    }
    len = strlen(s);
    copy = app_alloc(len + 1);
    if (copy != NULL) {
        memcpy(copy, s, len + 1);
    }
    return copy;
}

char *app_strndup(const char *s, size_t n)
{
    size_t len;
    char *copy;

    if (s == NULL) {
        return NULL;
    }
    len = strlen(s);
    if (len > n) {
        len = n;
    }
    copy = app_alloc(len + 1);
    if (copy != NULL) {
        memcpy(copy, s, len);
        copy[len] = '\0';
    }
    return copy;
}

void app_free(void *ptr)
{
    if (ptr != NULL) {
        heap_caps_free(ptr);
    }
}

void app_report_error(const char *tag, int error, const char *format, ...)
{
    va_list args;
    char *buffer;

    va_start(args, format);
    buffer = app_format_text(format, args);
    va_end(args);
    if (buffer != NULL) {
        shell_record_errorf(tag, error, "%s", buffer);
        free(buffer);
    }
}

void app_report_warning(const char *tag, const char *format, ...)
{
    va_list args;
    char *buffer;

    va_start(args, format);
    buffer = app_format_text(format, args);
    va_end(args);
    if (buffer != NULL) {
        shell_record_warningf(tag, "%s", buffer);
        free(buffer);
    }
}

void app_report_info(const char *tag, const char *format, ...)
{
    va_list args;
    char *buffer;

    va_start(args, format);
    buffer = app_format_text(format, args);
    va_end(args);
    if (buffer != NULL) {
        shell_record_infof(tag, "%s", buffer);
        free(buffer);
    }
}

/* ========================================================================
 * 3. TIME, TIMERS, SLEEP, SYSTEM INFORMATION
 * ======================================================================== */

time_t app_time(void)
{
    return time(NULL);
}

struct tm app_time_local(void)
{
    return time_get_local();
}

struct tm app_time_utc(void)
{
    return time_get_utc();
}

uint32_t app_uptime_sec(void)
{
    return time_get_uptime_sec();
}

int64_t app_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

void app_delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

bool app_time_synced(void)
{
    return time_is_synchronized();
}

void app_uptime_formatted(char *buf, size_t buflen)
{
    if (buf != NULL && buflen > 0) {
        time_get_uptime_formatted(buf, buflen);
    }
}

void app_sysinfo(char *buf, size_t buflen)
{
    if (buf == NULL || buflen == 0) {
        return;
    }
    snprintf(buf, buflen,
             "heap_free=%" PRIu32 " internal_free=%" PRIu32
             " psram_free=%" PRIu32 " uptime=%" PRIu32 "s clock=%s wifi=%s",
             (uint32_t)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
             (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (uint32_t)app_uptime_sec(),
             app_time_synced() ? "synced" : "unsynced",
             app_wifi_state_string());
}

/* ========================================================================
 * 4. NETWORKING HELPERS (state through the registered ops table)
 * ======================================================================== */

static applib_net_ops_t s_net_ops;

void applib_register_net_ops(const applib_net_ops_t *ops)
{
    if (ops != NULL) {
        s_net_ops = *ops;
    } else {
        s_net_ops.wifi_is_connected = NULL;
        s_net_ops.wifi_get_rssi = NULL;
        s_net_ops.wifi_state_string = NULL;
    }
}

bool app_wifi_is_connected(void)
{
    return (s_net_ops.wifi_is_connected != NULL) ? s_net_ops.wifi_is_connected() : false;
}

int app_wifi_get_rssi(void)
{
    int rssi = 0;

    if (s_net_ops.wifi_get_rssi != NULL) {
        (void)s_net_ops.wifi_get_rssi(&rssi);
    }
    return rssi;
}

const char *app_wifi_state_string(void)
{
    if (s_net_ops.wifi_state_string == NULL) {
        return "n/a";
    }
    return s_net_ops.wifi_state_string();
}

/* ========================================================================
 * 5. INPUT WITH TIMEOUT (bounded keypress / line reads for apps)
 * ======================================================================== */

bool app_wait_key(uint32_t timeout_ms, char *key_out)
{
    bool got = false;

    /* A headless board (no UART console, no USB keyboard) cannot deliver a
     * key; report failure instead of blocking the app. */
    if (!shell_key_input_available()) {
        return false;
    }

    shell_key_wait_begin();
    {
        char key = '\0';

        got = shell_wait_for_key(timeout_ms, &key);
        if (got && key_out != NULL) {
            *key_out = key;
        }
    }
    shell_key_wait_end();
    return got;
}

bool app_read_line(char *buf, size_t size, uint32_t timeout_ms)
{
    if (buf == NULL || size == 0) {
        return false;
    }
    if (!shell_key_input_available()) {
        return false;
    }
    return shell_read_line(buf, size, timeout_ms);
}

bool app_read_password(char *buf, size_t size, uint32_t timeout_ms)
{
    if (buf == NULL || size == 0) {
        return false;
    }
    if (!shell_key_input_available()) {
        return false;
    }
    return shell_read_line_hidden(buf, size, timeout_ms);
}

int app_menu(const char *title, const char **items, int count, uint32_t timeout_ms)
{
    char input[128];
    int choice;

    if (items == NULL || count <= 0) {
        return 0;
    }
    if (title != NULL && title[0] != '\0') {
        app_print_heading("%s", title);
    }
    for (int i = 0; i < count; i++) {
        app_printf("  %d. %s\n", i + 1, items[i] != NULL ? items[i] : "");
    }
    app_printf("Enter choice (1-%d): ", count);

    if (!app_read_line(input, sizeof(input), timeout_ms)) {
        return 0;
    }
    choice = atoi(input);
    if (choice < 1 || choice > count) {
        return 0;
    }
    return choice;
}

/* ========================================================================
 * 6. PERSISTENT STATE (INI files + temp files, all on the SD card)
 * ========================================================================
 * Thin app-facing wrappers over the shared storage_ini.c mechanics; the
 * batch `ini` / `temp` commands use the same storage functions, so nothing
 * is duplicated between the batch and applib surfaces.
 */

bool app_ini_get(const char *path, const char *key, char *value, size_t value_size)
{
    if (path == NULL || key == NULL || value == NULL || value_size == 0) {
        return false;
    }
    return storage_ini_file_get(path, key, value, value_size) == ESP_OK;
}

bool app_ini_set(const char *path, const char *key, const char *value)
{
    if (path == NULL || key == NULL || value == NULL) {
        return false;
    }
    return storage_ini_file_set(path, key, value) == ESP_OK;
}

bool app_ini_delete(const char *path, const char *key)
{
    if (path == NULL || key == NULL) {
        return false;
    }
    return storage_ini_file_delete(path, key) == ESP_OK;
}

bool app_temp_path(char *buf, size_t size, const char *ext)
{
    if (buf == NULL || size == 0) {
        return false;
    }
    return storage_temp_path(buf, size, ext) == ESP_OK;
}

bool app_temp_cleanup(void)
{
    return storage_temp_cleanup() == ESP_OK;
}

/* ========================================================================
 * 6b. DATABASE (Palm-OS-style SD record store)
 * ========================================================================
 * Thin app-facing wrappers over components/db (db.h). All database data lives
 * on the SD card under sd:/DBS/<name>.DB/. These mirror the `db` shell command
 * and share the same guarded-SD / atomic-write core.
 */

bool app_db_create(const char *name, const char *creator, const char *type,
                   uint32_t version)
{
    return db_create(name, creator, type, version) == ESP_OK;
}

bool app_db_info(const char *name, db_info_t *out)
{
    if (out == NULL) {
        return false;
    }
    return db_info(name, out) == ESP_OK;
}

bool app_db_drop(const char *name)
{
    return db_drop(name) == ESP_OK;
}

bool app_db_category_set(const char *name, uint8_t cat, const char *label)
{
    return db_category_set(name, cat, label) == ESP_OK;
}

bool app_db_category_get(const char *name, uint8_t cat, char *label, size_t size)
{
    if (label == NULL || size == 0) {
        return false;
    }
    return db_category_get(name, cat, label, size) == ESP_OK;
}

bool app_db_add(const char *name, uint8_t cat, const char *key, bool secret,
                const void *data, size_t len, uint32_t *out_id)
{
    return db_add(name, cat, key, secret, data, len, out_id) == ESP_OK;
}

bool app_db_get(const char *name, uint32_t id, void *buf, size_t *inout_len,
                bool reveal_secret, db_record_info_t *info)
{
    if (inout_len == NULL) {
        return false;
    }
    return db_get(name, id, buf, inout_len, reveal_secret, info) == ESP_OK;
}

bool app_db_set(const char *name, uint32_t id, uint8_t cat, const char *key,
                bool secret, const void *data, size_t len)
{
    return db_set(name, id, cat, key, secret, data, len) == ESP_OK;
}

bool app_db_del(const char *name, uint32_t id, bool permanent)
{
    return db_del(name, id, permanent) == ESP_OK;
}

bool app_db_purge(const char *name)
{
    return db_purge(name) == ESP_OK;
}

bool app_db_count(const char *name, uint8_t cat_filter, int *out_count)
{
    if (out_count == NULL) {
        return false;
    }
    return db_count(name, cat_filter, out_count) == ESP_OK;
}

bool app_db_find(const char *name, uint8_t cat_filter, const char *key_filter,
                 const char *text_filter, bool ignore_case, bool reveal_secret,
                 app_db_find_cb cb, void *ctx, int *out_count)
{
    if (out_count == NULL) {
        return false;
    }
    return db_find(name, cat_filter, key_filter, text_filter, ignore_case,
                   reveal_secret, (db_find_cb_t)cb, ctx, out_count) == ESP_OK;
}

/* ========================================================================
 * 7. APP MODE (save/restore screen + optional full-screen surface)
 * ========================================================================
 * Wraps the shell-core app-mode primitives (shared with the batch `appmode`
 * command), so an app can take over the transcript and restore it on exit.
 */

static char *s_appmode_saved = NULL;

bool app_mode_enter(bool full_screen)
{
    shell_screen_discard(s_appmode_saved);
    s_appmode_saved = shell_screen_save();
    shell_app_mode_enter(full_screen);
    return true;
}

bool app_mode_exit(void)
{
    shell_app_mode_exit();
    shell_screen_restore(s_appmode_saved);
    shell_screen_discard(s_appmode_saved);
    s_appmode_saved = NULL;
    return true;
}

/* ========================================================================
 * 8. NOTIFICATIONS
 * ========================================================================
 * Routes native-app notifications through the same header path as the batch
 * `notify` command.
 */

void app_notify(const char *text)
{
    if (text == NULL) {
        text = "";
    }
    shell_header_notify(text, P4_CONFIG_HEADER_NOTIFY_TIMEOUT_MS);
}

/* ========================================================================
 * 9. PROCESS ENVIRONMENT (env table + cwd, through the registered ops table)
 * ========================================================================
 * The env table is owned by components/batch and cwd by components/storage;
 * applib reads both through applib_env_ops_t registered by command_init, so
 * this leaf component never includes batch.h.
 */

static applib_env_ops_t s_env_ops;

void applib_register_env_ops(const applib_env_ops_t *ops)
{
    if (ops != NULL) {
        s_env_ops = *ops;
    } else {
        s_env_ops.get_env = NULL;
        s_env_ops.set_env = NULL;
        s_env_ops.get_cwd = NULL;
    }
}

const char *app_env_get(const char *name)
{
    if (name == NULL || s_env_ops.get_env == NULL) {
        return NULL;
    }
    return s_env_ops.get_env(name);
}

bool app_env_set(const char *name, const char *value)
{
    if (name == NULL || s_env_ops.set_env == NULL) {
        return false;
    }
    return s_env_ops.set_env(name, value) == ESP_OK;
}

const char *app_get_cwd(void)
{
    if (s_env_ops.get_cwd == NULL) {
        return NULL;
    }
    return s_env_ops.get_cwd();
}

/* ========================================================================
 * 10. NATIVE-APP REGISTRY (the app ABI)
 * ========================================================================
 * A registered app becomes a shell command: typing its name dispatches to
 * its app_main_t entry with argc/argv, and the return value becomes
 * ERRORLEVEL. The registry lives here (the SDK); the shell dispatcher reaches
 * it through app_dispatch().
 */

typedef struct {
    bool used;
    char name[P4_CONFIG_APP_NAME_BYTES];
    char description[P4_CONFIG_APP_DESC_BYTES];
    app_main_t entry;
} applib_app_entry_t;

static applib_app_entry_t s_apps[P4_CONFIG_APP_MAX];

bool app_register(const char *name, const char *description, app_main_t entry)
{
    if (name == NULL || name[0] == '\0' || entry == NULL) {
        return false;
    }
    for (int i = 0; i < P4_CONFIG_APP_MAX; i++) {
        if (!s_apps[i].used) {
            snprintf(s_apps[i].name, sizeof(s_apps[i].name), "%s", name);
            snprintf(s_apps[i].description, sizeof(s_apps[i].description),
                     description != NULL ? description : "");
            s_apps[i].entry = entry;
            s_apps[i].used = true;
            return true;
        }
    }
    return false; /* table full */
}

bool app_find(const char *name)
{
    if (name == NULL) {
        return false;
    }
    for (int i = 0; i < P4_CONFIG_APP_MAX; i++) {
        if (s_apps[i].used && strcasecmp(name, s_apps[i].name) == 0) {
            return true;
        }
    }
    return false;
}

bool app_dispatch(int argc, char **argv, int *errorlevel_out)
{
    if (argc < 1 || argv == NULL || argv[0] == NULL) {
        return false;
    }
    for (int i = 0; i < P4_CONFIG_APP_MAX; i++) {
        if (s_apps[i].used && strcasecmp(argv[0], s_apps[i].name) == 0) {
            int result = s_apps[i].entry(argc, argv);
            if (errorlevel_out != NULL) {
                *errorlevel_out = result;
            }
            return true;
        }
    }
    return false;
}

bool app_get(int index, char *name_out, size_t name_size,
             char *desc_out, size_t desc_size)
{
    if (index < 0 || index >= P4_CONFIG_APP_MAX || !s_apps[index].used) {
        return false;
    }
    if (name_out != NULL && name_size > 0) {
        snprintf(name_out, name_size, "%s", s_apps[index].name);
    }
    if (desc_out != NULL && desc_size > 0) {
        snprintf(desc_out, desc_size, "%s", s_apps[index].description);
    }
    return true;
}
