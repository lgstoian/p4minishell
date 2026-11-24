/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file host_batch_bridge.c
 * @brief Host-only bridge for the real batch interpreter.
 *
 * The firmware installs these services from command.c/storage.c/tui.*;
 * the host lane has no SD card, panel or FreeRTOS worker task. This file
 * supplies only the missing link-time symbols. It lives exclusively in
 * test/host and is never in firmware.
 *
 * Two groups of services live here:
 *
 *   1. Honest "unavailable" stubs (19 of them): no display, no RTC, no
 *      directory expansion, no shell confirmation gate. They report failure
 *      or an inert value; no SD write, display page or console device is
 *      ever simulated.
 *
 *   2. A POSIX-backed SD fixture root, installed explicitly by
 *      host_batch_fs_init() from the host_batch_run probe. It maps the board's
 *      SD mount point onto a real host directory (env P4_HOST_SD_ROOT) and
 *      installs the additive batch_io_ops_t file/stat seam so the REAL
 *      interpreter reads real .bat files. With no root installed every POSIX
 *      service fails honestly; a missing root is never faked as a card.
 *
 * The Unity host lane never calls host_batch_fs_init(), so its configuration
 * is unchanged: s_sd_root is empty, shell_sd_begin() returns ESP_FAIL, and all
 * 19 stubs keep their original "unavailable" behaviour.
 */

/* The batch verbs need the guarded-session prototypes but never touch a
 * FATFS FRESULT. Same guard the production batch.c uses on the host lane. */
#define P4_STORAGE_NO_FATFS_TYPES 1

#include "batch.h"
#include "batch_io.h"
#include "board_identity.h"
#include "clock.h"
#include "command.h"
#include "esp_timer.h"
#include "shell.h"
#include "shell_transcript_internal.h"
#include "storage.h"
#include "tui.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ========================================================================
 * POSIX-BACKED SD FIXTURE ROOT (host_batch_run only)
 * ========================================================================
 * The SD mount prefix is a board fact from board_identity.h, never copied
 * into the interpreter. Only this host-only file knows it is backed by a
 * directory on the host. The root is process-global and boot-lived, like the
 * registration tables it feeds.
 */

#define HOST_SD_ROOT_BYTES 512
#define HOST_INI_MAX_BYTES (64u * 1024u)

static char s_sd_root[HOST_SD_ROOT_BYTES];
static bool s_sd_root_ready = false;

/**
 * Map a firmware VFS path onto the host fixture root.
 *
 * `/sdcard/X` becomes `<root>/X` with the mount prefix from board_identity.h.
 * A path outside the mount is passed through as an ordinary POSIX path so a
 * caller can still open an explicit host file. With no root installed (the
 * Unity lane) every path fails: there is no card and no fixture to pretend.
 */
static bool host_sd_map_path(const char *path, char *out, size_t out_size)
{
    size_t mount_len;
    int written;

    if (!s_sd_root_ready || path == NULL || out == NULL || out_size == 0) {
        return false;
    }
    mount_len = strlen(BOARD_CFG_SD_MOUNT_POINT);
    if (strncmp(path, BOARD_CFG_SD_MOUNT_POINT, mount_len) == 0 &&
        (path[mount_len] == '\0' || path[mount_len] == '/')) {
        written = snprintf(out, out_size, "%s%s", s_sd_root, path + mount_len);
    } else {
        written = snprintf(out, out_size, "%s", path);
    }
    return written >= 0 && (size_t)written < out_size;
}

/** POSIX `fopen` behind the additive batch_io_ops_t file seam. */
static FILE *host_batch_file_open(const char *path, const char *mode)
{
    char host_path[HOST_SD_ROOT_BYTES];

    if (!host_sd_map_path(path, host_path, sizeof(host_path))) {
        return NULL;
    }
    return fopen(host_path, mode != NULL ? mode : "r");
}

/** POSIX `stat` behind the additive batch_io_ops_t discovery seam. */
static esp_err_t host_batch_stat_path(const char *path, struct stat *st)
{
    char host_path[HOST_SD_ROOT_BYTES];

    if (!host_sd_map_path(path, host_path, sizeof(host_path))) {
        return ESP_ERR_NOT_FOUND;
    }
    if (stat(host_path, st) == 0) {
        return ESP_OK;
    }
    return (errno == ENOENT || errno == ENOTDIR) ? ESP_ERR_NOT_FOUND : ESP_FAIL;
}

/* The other optional members are installed with honest POSIX-backed versions
 * defined below, so a .bat verb that reaches them on the host runs against a
 * real file (or reports a real error) rather than an invented value. */
static const batch_io_ops_t s_host_io_ops = {
    .sd_begin = shell_sd_begin,
    .sd_end = shell_sd_end,
    .resolve_input_source = storage_resolve_input_source,
    .read_line = shell_read_line,
    .read_line_hidden = shell_read_line_hidden,
    .ini_file_set = storage_ini_file_set,
    .ini_file_delete = storage_ini_file_delete,
    .ini_file_foreach = storage_ini_file_foreach,
    .file_open = host_batch_file_open,
    .stat_path = host_batch_stat_path,
};

bool host_batch_fs_init(const char *root)
{
    struct stat st;
    /* realpath()'s fortified form requires a full PATH_MAX output buffer;
     * keep it off the FreeRTOS main-task stack. */
    static char canonical[PATH_MAX];

    s_sd_root[0] = '\0';
    s_sd_root_ready = false;

    if (root != NULL && root[0] != '\0') {
        if (realpath(root, canonical) != NULL) {
            snprintf(s_sd_root, sizeof(s_sd_root), "%.*s",
                     (int)sizeof(s_sd_root) - 1, canonical);
        } else {
            snprintf(s_sd_root, sizeof(s_sd_root), "%.*s",
                     (int)sizeof(s_sd_root) - 1, root);
        }
        /* A trailing slash would produce `//NAME` after the mount mapping;
         * realpath already removes it, but tolerate a caller-provided path. */
        {
            size_t len = strlen(s_sd_root);

            while (len > 1 && s_sd_root[len - 1] == '/') {
                s_sd_root[--len] = '\0';
            }
        }
        s_sd_root_ready = (stat(s_sd_root, &st) == 0 && S_ISDIR(st.st_mode));
    }

    /* Always install the table: batch_stat_path() falls back to the firmware's
     * shell_sd_stat_path() when stat_path is NULL, and that symbol does not
     * exist on the host lane. A missing root still makes both members fail. */
    batch_register_io_ops(&s_host_io_ops);
    return s_sd_root_ready;
}

/* ========================================================================
 * shell.c view / app-mode services (no display on the host)
 * ======================================================================== */

void shell_transcript_defer_begin(void)
{
    /* No LVGL label repaint to defer on the host. */
}

void shell_transcript_defer_end(void)
{
    /* Pairs with shell_transcript_defer_begin(); no repaint to flush. */
}

void shell_screen_restore(const char *saved)
{
    (void)saved;
    /* No transcript/screen surface to restore. */
}

void shell_screen_discard(char *saved)
{
    (void)saved;
    /* No screen copy was ever produced by the host lane. */
}

void shell_app_mode_exit(void)
{
    /* No shell input widgets / full-screen surface to restore. */
}

void shell_set_batch_active(bool active)
{
    (void)active;
    /* The host has no interactive confirmation gate to drive from this flag. */
}

/* ========================================================================
 * storage.c services
 * ======================================================================== */

esp_err_t shell_sd_begin(shell_sd_session_t *session)
{
    (void)session;
    /* Honest: the "card" is mounted only when a real fixture root exists. */
    return s_sd_root_ready ? ESP_OK : ESP_FAIL;
}

void shell_sd_end(shell_sd_session_t *session, const char *operation)
{
    (void)session;
    (void)operation;
    /* No guarded session state to release: host_sd_begin is a root check. */
}

esp_err_t storage_expand_wildcard(const char *pattern, char ***tokens_out, int *count_out)
{
    (void)pattern;
    /* No directory to list: return an empty result, never fake a match. */
    if (tokens_out != NULL) *tokens_out = NULL;
    if (count_out != NULL) *count_out = 0;
    return ESP_FAIL;
}

esp_err_t storage_expand_dirs(const char *pattern, char ***tokens_out, int *count_out)
{
    (void)pattern;
    if (tokens_out != NULL) *tokens_out = NULL;
    if (count_out != NULL) *count_out = 0;
    return ESP_FAIL;
}

esp_err_t storage_expand_recursive(const char *root, const char *name_pattern,
                                   char ***tokens_out, int *count_out)
{
    (void)root;
    (void)name_pattern;
    if (tokens_out != NULL) *tokens_out = NULL;
    if (count_out != NULL) *count_out = 0;
    return ESP_FAIL;
}

void storage_free_wildcard_expansion(char **tokens, int count)
{
    (void)tokens;
    (void)count;
    /* Expansions never allocate on the host, so there is nothing to free. */
}

esp_err_t storage_resolve_input_source(const char *argument, char *output, size_t output_size)
{
    struct stat st;
    char host_path[HOST_SD_ROOT_BYTES];

    if (output == NULL || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    output[0] = '\0';
    /* No redirection slot on the host; a caller passing an explicit path still
     * resolves it against the fixture root and gets a real host path that the
     * verb's plain fopen() can open. */
    if (!host_sd_map_path(argument != NULL ? argument : "", host_path, sizeof(host_path))) {
        return ESP_ERR_NOT_FOUND;
    }
    if (stat(host_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return ESP_ERR_NOT_FOUND;
    }
    snprintf(output, output_size, "%.*s", (int)output_size - 1, host_path);
    return ESP_OK;
}

esp_err_t storage_write_text_file(const char *path, const char *text)
{
    char host_path[HOST_SD_ROOT_BYTES];
    FILE *fh;
    size_t len;
    size_t written;

    if (text == NULL || !host_sd_map_path(path, host_path, sizeof(host_path))) {
        return ESP_FAIL;
    }
    fh = fopen(host_path, "wb");
    if (fh == NULL) {
        return ESP_FAIL;
    }
    len = strlen(text);
    written = (len > 0) ? fwrite(text, 1, len, fh) : 0;
    if (fclose(fh) != 0 || written != len) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

/** Read a bounded whole INI file; NULL on a missing root/file or OOM. */
static char *host_ini_read(const char *path, const char *operation)
{
    char host_path[HOST_SD_ROOT_BYTES];
    FILE *fh;
    long size;
    char *buf;
    size_t got;

    (void)operation;
    if (!host_sd_map_path(path, host_path, sizeof(host_path))) {
        return NULL;
    }
    fh = fopen(host_path, "rb");
    if (fh == NULL) {
        return NULL;
    }
    if (fseek(fh, 0, SEEK_END) != 0) {
        fclose(fh);
        return NULL;
    }
    size = ftell(fh);
    if (size < 0 || (unsigned long)size >= HOST_INI_MAX_BYTES ||
        fseek(fh, 0, SEEK_SET) != 0) {
        fclose(fh);
        return NULL;
    }
    buf = malloc((size_t)size + 1u);
    if (buf == NULL) {
        fclose(fh);
        return NULL;
    }
    got = fread(buf, 1, (size_t)size, fh);
    fclose(fh);
    buf[got] = '\0';
    return buf;
}

/** Rewrite an INI file from a bounded in-memory image. */
static esp_err_t host_ini_write(const char *path, const char *text)
{
    return storage_write_text_file(path, text);
}

esp_err_t storage_ini_file_set(const char *path, const char *key, const char *value)
{
    char image[HOST_INI_MAX_BYTES];
    char *source;
    char *cursor;
    size_t used = 0;
    bool replaced = false;

    if (key == NULL || key[0] == '\0' || value == NULL || path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    source = host_ini_read(path, "ini set");
    cursor = source;
    while (cursor != NULL && *cursor != '\0' && used < sizeof(image) - 1u) {
        char *line = cursor;
        char *end = strpbrk(cursor, "\r\n");
        char *equals;
        char name[P4_CONFIG_ENV_NAME_BYTES];

        if (end != NULL) {
            *end = '\0';
            cursor = end + 1;
            while (*cursor == '\r' || *cursor == '\n') cursor++;
        } else {
            cursor = NULL;
        }
        equals = strchr(line, '=');
        if (equals != NULL) {
            size_t name_len = (size_t)(equals - line);
            if (name_len >= sizeof(name)) name_len = sizeof(name) - 1u;
            memcpy(name, line, name_len);
            name[name_len] = '\0';
            if (shell_text_equals_ignore_case(name, key)) {
                int n = snprintf(image + used, sizeof(image) - used,
                                 "%s=%s\n", key, value);
                if (n < 0 || (size_t)n >= sizeof(image) - used) break;
                used += (size_t)n;
                replaced = true;
                continue;
            }
        }
        {
            int n = snprintf(image + used, sizeof(image) - used, "%s\n", line);
            if (n < 0 || (size_t)n >= sizeof(image) - used) break;
            used += (size_t)n;
        }
    }
    free(source);
    if (!replaced) {
        int n = snprintf(image + used, sizeof(image) - used, "%s=%s\n", key, value);

        if (n < 0 || (size_t)n >= sizeof(image) - used) {
            return ESP_FAIL;
        }
        used += (size_t)n;
    }
    image[used] = '\0';
    return host_ini_write(path, image);
}

esp_err_t storage_ini_file_delete(const char *path, const char *key)
{
    char image[HOST_INI_MAX_BYTES];
    char *source;
    char *cursor;
    size_t used = 0;
    bool removed = false;

    if (key == NULL || key[0] == '\0' || path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    source = host_ini_read(path, "ini del");
    if (source == NULL) {
        return ESP_OK;  /* Nothing to delete is not a write failure. */
    }
    cursor = source;
    while (*cursor != '\0' && used < sizeof(image) - 1u) {
        char *line = cursor;
        char *end = strpbrk(cursor, "\r\n");
        char *equals;
        bool match = false;

        if (end != NULL) {
            *end = '\0';
            cursor = end + 1;
            while (*cursor == '\r' || *cursor == '\n') cursor++;
        } else {
            cursor = line + strlen(line);
        }
        equals = strchr(line, '=');
        if (equals != NULL) {
            char name[P4_CONFIG_ENV_NAME_BYTES];
            size_t name_len = (size_t)(equals - line);

            if (name_len >= sizeof(name)) name_len = sizeof(name) - 1u;
            memcpy(name, line, name_len);
            name[name_len] = '\0';
            match = shell_text_equals_ignore_case(name, key);
        }
        if (match) {
            removed = true;
            continue;
        }
        {
            int n = snprintf(image + used, sizeof(image) - used, "%s\n", line);

            if (n < 0 || (size_t)n >= sizeof(image) - used) break;
            used += (size_t)n;
        }
    }
    free(source);
    image[used] = '\0';
    if (!removed) {
        return ESP_OK;
    }
    return host_ini_write(path, image);
}

esp_err_t storage_ini_file_foreach(const char *path,
                                   bool (*cb)(const char *key, const char *value, void *ctx),
                                   void *ctx)
{
    char *source;
    char *cursor;
    esp_err_t result = ESP_OK;

    if (cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    source = host_ini_read(path, "ini list");
    if (source == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    cursor = source;
    while (*cursor != '\0') {
        char *line = cursor;
        char *end = strpbrk(cursor, "\r\n");
        char *equals;

        if (end != NULL) {
            *end = '\0';
            cursor = end + 1;
            while (*cursor == '\r' || *cursor == '\n') cursor++;
        } else {
            cursor = line + strlen(line);
        }
        equals = strchr(line, '=');
        if (equals != NULL && equals > line) {
            *equals = '\0';
            if (!cb(line, equals + 1, ctx)) {
                result = ESP_OK;
                break;
            }
        }
    }
    free(source);
    return result;
}

/** Build a real, unique temp path under the fixture root (never invented). */
static esp_err_t host_temp_path(char *buf, size_t size, const char *stem, const char *ext)
{
    static unsigned counter = 0;
    char host_path[HOST_SD_ROOT_BYTES];
    int n;

    if (buf == NULL || size == 0 || !s_sd_root_ready) {
        return ESP_FAIL;
    }
    n = snprintf(host_path, sizeof(host_path), "%s/P4_HOST_%s_%lu_%u%s",
                 s_sd_root, stem != NULL ? stem : "TMP",
                 (unsigned long)getpid(), ++counter,
                 (ext != NULL && ext[0] != '\0') ? ext : ".tmp");
    if (n < 0 || (size_t)n >= sizeof(host_path)) {
        return ESP_FAIL;
    }
    snprintf(buf, size, "%.*s", (int)size - 1, host_path);
    return ESP_OK;
}

esp_err_t storage_temp_path(char *buf, size_t size, const char *ext)
{
    return host_temp_path(buf, size, "TMP", ext);
}

esp_err_t storage_temp_path_stem(char *buf, size_t size, const char *stem, const char *ext)
{
    return host_temp_path(buf, size, stem, ext);
}

/** Interactive line source: POSIX stdin. No line is invented on EOF. */
static bool host_read_line(char *output, size_t output_size, uint32_t timeout_ms)
{
    (void)timeout_ms;  /* stdin has no host-side timeout; EOF ends the read. */
    if (output == NULL || output_size == 0) {
        return false;
    }
    output[0] = '\0';
    if (fgets(output, (int)output_size, stdin) == NULL) {
        return false;
    }
    {
        size_t len = strlen(output);

        while (len > 0 && (output[len - 1] == '\n' || output[len - 1] == '\r')) {
            output[--len] = '\0';
        }
    }
    return true;
}

bool shell_read_line(char *output, size_t output_size, uint32_t timeout_ms)
{
    return host_read_line(output, output_size, timeout_ms);
}

bool shell_read_line_hidden(char *output, size_t output_size, uint32_t timeout_ms)
{
    /* No terminal echo to suppress in a piped host process. */
    return host_read_line(output, output_size, timeout_ms);
}

/* ========================================================================
 * Remaining batch.c link-time services (honest, no board behind them)
 * ========================================================================
 * The real interpreter calls these from verbs the host router can reach.
 * None of them invents a value: without a fixture root there is no file, no
 * key source and no screen to save. They are never part of firmware.
 */

esp_err_t shell_sd_stat_path(const char *path, struct stat *st)
{
    char host_path[HOST_SD_ROOT_BYTES];

    if (path == NULL || st == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!host_sd_map_path(path, host_path, sizeof(host_path))) {
        return ESP_ERR_NOT_FOUND;
    }
    if (stat(host_path, st) == 0) {
        return ESP_OK;
    }
    return (errno == ENOENT || errno == ENOTDIR) ? ESP_ERR_NOT_FOUND : ESP_FAIL;
}

void shell_transcript_reset(void)
{
    /* The host lane has no LVGL view to clear; the shared model is the whole
     * transcript. shell.c's view-side reset is not linked here. */
    shell_transcript_model_clear();
}

esp_err_t storage_ini_file_get(const char *path, const char *key,
                               char *value, size_t value_size)
{
    char *source;
    char *cursor;
    esp_err_t result = ESP_ERR_NOT_FOUND;

    if (key == NULL || value == NULL || value_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    value[0] = '\0';
    source = host_ini_read(path, "ini get");
    if (source == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    cursor = source;
    while (*cursor != '\0') {
        char *line = cursor;
        char *end = strpbrk(cursor, "\r\n");
        char *equals;

        if (end != NULL) {
            *end = '\0';
            cursor = end + 1;
            while (*cursor == '\r' || *cursor == '\n') cursor++;
        } else {
            cursor = line + strlen(line);
        }
        equals = strchr(line, '=');
        if (equals != NULL && equals > line) {
            char name[P4_CONFIG_ENV_NAME_BYTES];
            size_t name_len = (size_t)(equals - line);

            if (name_len >= sizeof(name)) name_len = sizeof(name) - 1u;
            memcpy(name, line, name_len);
            name[name_len] = '\0';
            if (shell_text_equals_ignore_case(name, key)) {
                snprintf(value, value_size, "%.*s", (int)value_size - 1,
                         equals + 1);
                result = ESP_OK;
                break;
            }
        }
    }
    free(source);
    return result;
}

bool storage_ini_upsert(char *text, size_t cap, const char *key, const char *value)
{
    char image[HOST_INI_MAX_BYTES];
    char *cursor;
    size_t used = 0;
    bool replaced = false;

    if (text == NULL || cap == 0 || key == NULL || value == NULL || cap > sizeof(image)) {
        return false;
    }
    snprintf(image, sizeof(image), "%.*s", (int)cap - 1, text);
    cursor = image;
    while (*cursor != '\0' && used < sizeof(image) - 1u) {
        char *line = cursor;
        char *end = strpbrk(cursor, "\r\n");
        char *equals;

        if (end != NULL) {
            *end = '\0';
            cursor = end + 1;
            while (*cursor == '\r' || *cursor == '\n') cursor++;
        } else {
            cursor = line + strlen(line);
        }
        equals = strchr(line, '=');
        if (equals != NULL && shell_text_equals_ignore_case(line, key)) {
            /* Names are compared after the caller normalised the key; the pure
             * firmware helper does the same case-folded replacement. */
            int n = snprintf(text + used, cap - used, "%s=%s\n", key, value);

            if (n < 0 || (size_t)n >= cap - used) return false;
            used += (size_t)n;
            replaced = true;
            continue;
        }
        {
            int n = snprintf(text + used, cap - used, "%s\n", line);

            if (n < 0 || (size_t)n >= cap - used) return false;
            used += (size_t)n;
        }
    }
    if (!replaced) {
        int n = snprintf(text + used, cap - used, "%s=%s\n", key, value);

        if (n < 0 || (size_t)n >= cap - used) return false;
        used += (size_t)n;
    }
    text[used] = '\0';
    return true;
}

uint64_t storage_get_file_size(const char *resolved_path)
{
    char host_path[HOST_SD_ROOT_BYTES];
    struct stat st;

    if (!host_sd_map_path(resolved_path, host_path, sizeof(host_path)) ||
        stat(host_path, &st) != 0) {
        return 0;
    }
    return (uint64_t)st.st_size;
}

bool storage_check_free_space(uint64_t needed_bytes, uint64_t reclaim_bytes,
                              const char *operation)
{
    (void)needed_bytes;
    (void)reclaim_bytes;
    (void)operation;
    /* The host lane cannot promise SD free space; refuse rather than claim a
     * save succeeded. A real fixture write reports its own POSIX error. */
    return false;
}

bool shell_key_input_available(void)
{
    /* No USB keyboard/panel key source on the host lane. */
    return false;
}

void shell_key_wait_begin(void)
{
    /* No key source to arm. */
}

void shell_key_wait_end(void)
{
    /* Pairs with shell_key_wait_begin(). */
}

bool shell_wait_for_key(uint32_t timeout_ms, char *key_out)
{
    (void)timeout_ms;
    if (key_out != NULL) key_out[0] = '\0';
    return false;
}

/* One redirection slot, shared by the real verbs (storage.c owns it on the
 * board). On the host it is a plain RAM path, never a fabricated file. */
static char s_input_redirect[P4_CONFIG_SD_PATH_BYTES];

void storage_set_input_redirect(const char *resolved_path)
{
    snprintf(s_input_redirect, sizeof(s_input_redirect), "%s",
             resolved_path != NULL ? resolved_path : "");
}

const char *storage_get_input_redirect(void)
{
    return s_input_redirect;
}

void storage_clear_input_redirect(void)
{
    s_input_redirect[0] = '\0';
}

esp_err_t storage_temp_cleanup(void)
{
    /* host_temp_path() only publishes paths; it never creates a temp file
     * behind the caller's back, so there is nothing to reclaim. */
    return ESP_OK;
}

char *shell_screen_save(void)
{
    /* No transcript/screen surface to capture on the host. */
    return NULL;
}

void shell_app_mode_enter(bool full_screen)
{
    (void)full_screen;
    /* No app widgets or full-screen surface to switch. */
}

/* ========================================================================
 * tui.c / clock.c services (no panel, no RTC on the host)
 * ======================================================================== */

bool tui_is_active(void)
{
    return false;
}

void tui_deinit(void)
{
    /* No TUI surface to leave. */
}

void tui_new_app_session(void)
{
    /* No app surface to clear between launches. */
}

void tui_app_grid(int *cols_out, int *rows_out, int *cell_w_out, int *cell_h_out)
{
    /* Report the configured headless grid; no panel geometry is available. */
    if (cols_out != NULL) *cols_out = 0;
    if (rows_out != NULL) *rows_out = 0;
    if (cell_w_out != NULL) *cell_w_out = 0;
    if (cell_h_out != NULL) *cell_h_out = 0;
}

int64_t esp_timer_get_time(void)
{
    /* POSIX monotonic clock in microseconds: the same time base the firmware
     * timer API exposes, with no board RTC or esp_timer task needed. */
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + (int64_t)ts.tv_nsec / 1000LL;
}

struct tm time_get_local(void)
{
    /* No RTC/SNTP on the host: return the zero time (epoch), which formats
     * as a well-formed but deliberately unset date and time. */
    struct tm unset;

    memset(&unset, 0, sizeof(unset));
    return unset;
}
