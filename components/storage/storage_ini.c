/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file storage_ini.c
 * @brief INI-style persistent-state and temp-file services.
 *
 * DOS-like apps kept state in environment variables, temporary files, and
 * simple `KEY=VALUE` config files. This module provides that surface once,
 * for every consumer:
 *
 *   - The pure INI line editors (`storage_ini_get_value` / `storage_ini_upsert`
 *     / `storage_ini_remove`) edit a `KEY=VALUE` text buffer, skipping
 *     `;`/`#`/`REM` comments and blank lines. This is the same logic the
 *     `config` command used for CONFIG.SYS (moved here so batch and applib
 *     share it instead of duplicating it).
 *   - The file-level operations (`storage_ini_file_*`) read, update, and
 *     iterate an INI file on the SD card through guarded sessions, writing
 *     atomically (temp file + rename).
 *   - `storage_temp_*` manages temporary files in the SD temp directory
 *     (`sd:/<P4_CONFIG_TEMP_DIR_NAME>`), so every temp file lives on the SD
 *     card.
 *
 * All paths are resolved relative to the shell current directory and all SD
 * access goes through the guarded session, matching the firmware rules.
 */

#include "storage.h"
#include "shell.h"
#include "p4minishell_config.h"
#include "bsp/esp-bsp.h"
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SHELL_SD_PATH_BYTES     P4_CONFIG_SD_PATH_BYTES
#define INI_MAX_BYTES           P4_CONFIG_INI_MAX_BYTES
#define INI_TAG                 "ini"
#define INI_KEY_BYTES           P4_CONFIG_ENV_NAME_BYTES
#define INI_VALUE_BYTES         P4_CONFIG_ENV_VALUE_BYTES

/* ========================================================================
 * PURE INI LINE EDITORS
 * ======================================================================== */

int storage_ini_get_value(const char *text, const char *key, char *out, size_t out_size)
{
    const char *cursor;

    if (text == NULL || key == NULL) {
        return -1;
    }

    cursor = text;
    while (*cursor != '\0') {
        const char *line = cursor;
        const char *nl = strchr(cursor, '\n');
        const char *end = nl != NULL ? nl : cursor + strlen(cursor);
        const char *q = line;
        const char *eq;

        while (q < end && isspace((unsigned char)*q)) {
            q++;
        }
        if (q < end && (*q == ';' || *q == '#')) {
            cursor = nl != NULL ? nl + 1 : end;
            continue;
        }
        if ((size_t)(end - q) >= 3 && strncasecmp(q, "REM", 3) == 0 &&
            (q + 3 == end || isspace((unsigned char)q[3]))) {
            cursor = nl != NULL ? nl + 1 : end;
            continue;
        }
        if (q == end) {
            cursor = nl != NULL ? nl + 1 : end;
            continue;
        }

        eq = memchr(q, '=', (size_t)(end - q));
        if (eq != NULL) {
            size_t key_len = (size_t)(eq - q);
            const char *v = eq + 1;
            size_t value_len = (size_t)(end - v);

            while (key_len > 0 && isspace((unsigned char)q[key_len - 1])) {
                key_len--;
            }
            if (key_len == strlen(key) && strncasecmp(q, key, key_len) == 0) {
                while (value_len > 0 && isspace((unsigned char)v[value_len - 1])) {
                    value_len--;
                }
                while (value_len > 0 && isspace((unsigned char)*v)) {
                    v++;
                    value_len--;
                }
                if (out != NULL && out_size > 0) {
                    size_t copy = value_len < out_size - 1 ? value_len : out_size - 1;

                    memcpy(out, v, copy);
                    out[copy] = '\0';
                }
                return (int)value_len;
            }
        }

        cursor = nl != NULL ? nl + 1 : end;
    }

    return -1;
}

/** Append a single "KEY=VALUE\n" line, returning true on success. */
static bool storage_ini_append(char *text, size_t cap,
                               const char *key, const char *value)
{
    size_t text_len;
    size_t key_len;
    size_t value_len;

    if (text == NULL || key == NULL || value == NULL) {
        return false;
    }
    text_len = strlen(text);
    key_len = strlen(key);
    value_len = strlen(value);

    if (text_len > 0 && text[text_len - 1] != '\n') {
        if (text_len + 1 + key_len + 1 + value_len + 1 + 1 > cap) {
            return false;
        }
        text[text_len++] = '\n';
    }
    if (text_len + key_len + 1 + value_len + 1 + 1 > cap) {
        return false;
    }
    memcpy(text + text_len, key, key_len);
    text_len += key_len;
    text[text_len++] = '=';
    memcpy(text + text_len, value, value_len);
    text_len += value_len;
    text[text_len++] = '\n';
    text[text_len] = '\0';
    return true;
}

bool storage_ini_remove(char *text, size_t cap, const char *key)
{
    size_t key_len;
    bool removed = false;

    (void)cap;
    if (text == NULL || key == NULL) {
        return false;
    }
    key_len = strlen(key);

    while (*text != '\0') {
        const char *line = text;
        const char *nl = strchr(text, '\n');
        const char *end = nl != NULL ? nl : text + strlen(text);
        const char *span_end = nl != NULL ? nl + 1 : end;
        const char *q = line;
        const char *eq;

        while (q < end && isspace((unsigned char)*q)) {
            q++;
        }
        if (q < end && (*q == ';' || *q == '#')) {
            text = (char *)span_end;
            continue;
        }
        if ((size_t)(end - q) >= 3 && strncasecmp(q, "REM", 3) == 0 &&
            (q + 3 == end || isspace((unsigned char)q[3]))) {
            text = (char *)span_end;
            continue;
        }
        eq = memchr(q, '=', (size_t)(end - q));
        if (eq != NULL) {
            size_t kw_len = (size_t)(eq - q);

            while (kw_len > 0 && isspace((unsigned char)q[kw_len - 1])) {
                kw_len--;
            }
            if (kw_len == key_len && strncasecmp(q, key, kw_len) == 0) {
                size_t removed_len = (size_t)(span_end - text);

                memmove(text, text + removed_len, strlen(text + removed_len) + 1);
                removed = true;
                continue;
            }
        }
        text = (char *)span_end;
    }

    return removed;
}

bool storage_ini_upsert(char *text, size_t cap, const char *key, const char *value)
{
    if (text == NULL || key == NULL || value == NULL) {
        return false;
    }
    (void)storage_ini_remove(text, cap, key);
    return storage_ini_append(text, cap, key, value);
}

/* ========================================================================
 * FILE-LEVEL INI OPERATIONS (guarded sessions, atomic writes)
 * ======================================================================== */

/** Resolve a user path against the shell current directory. */
static esp_err_t storage_ini_resolve(const char *path, char *resolved, size_t resolved_size)
{
    if (path == NULL || path[0] == '\0' || resolved == NULL || resolved_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return shell_fs_resolve_path(path, resolved, resolved_size);
}

/** Read a text file into a heap buffer (INI_MAX_BYTES+1, NUL-terminated). */
static char *storage_ini_read_file(const char *resolved, size_t *out_len)
{
    shell_sd_session_t session;
    FILE *file = NULL;
    char *buffer = NULL;

    if (shell_sd_begin(&session) != ESP_OK) {
        return NULL;
    }
    file = fopen(resolved, "r");
    if (file == NULL) {
        shell_sd_end(&session, INI_TAG);
        return NULL;
    }
    buffer = malloc(INI_MAX_BYTES + 1);
    if (buffer == NULL) {
        fclose(file);
        shell_sd_end(&session, INI_TAG);
        return NULL;
    }
    {
        size_t got = fread(buffer, 1, INI_MAX_BYTES, file);

        buffer[got] = '\0';
        if (out_len != NULL) {
            *out_len = got;
        }
    }
    fclose(file);
    shell_sd_end(&session, INI_TAG);
    return buffer;
}

esp_err_t storage_write_text_file(const char *path, const char *text)
{
    char resolved[SHELL_SD_PATH_BYTES];
    char tmp[SHELL_SD_PATH_BYTES + 16];
    shell_sd_session_t session;
    FILE *file = NULL;
    size_t text_len;
    esp_err_t error;

    if (text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    error = storage_ini_resolve(path, resolved, sizeof(resolved));
    if (error != ESP_OK) {
        return error;
    }
    text_len = strlen(text);

    if (shell_sd_begin(&session) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp", resolved);

    /* Precheck capacity before opening, so an overwrite cannot destroy the
     * existing file and then fail for lack of room. */
    {
        uint64_t needed = (uint64_t)text_len + 512;
        uint64_t reclaim = storage_get_file_size(resolved);

        if (!storage_check_free_space(needed, reclaim, INI_TAG)) {
            shell_sd_end(&session, INI_TAG);
            return ESP_ERR_NO_MEM;
        }
    }

    file = fopen(tmp, "w");
    if (file == NULL) {
        shell_sd_end(&session, INI_TAG);
        return ESP_FAIL;
    }
    {
        bool wrote_ok = (fwrite(text, 1, text_len, file) == text_len);
        int flush_rc = fflush(file);
        int close_rc = fclose(file);
        file = NULL;
        if (!wrote_ok || flush_rc != 0 || close_rc != 0) {
            remove(tmp);
            shell_sd_end(&session, INI_TAG);
            return ESP_FAIL;
        }
    }

    /* FATFS f_rename refuses to overwrite an existing target. */
    if (rename(tmp, resolved) != 0) {
        remove(resolved);
        if (rename(tmp, resolved) != 0) {
            remove(tmp);
            shell_sd_end(&session, INI_TAG);
            return ESP_FAIL;
        }
    }

    shell_sd_end(&session, INI_TAG);
    return ESP_OK;
}

esp_err_t storage_ini_file_get(const char *path, const char *key,
                               char *value, size_t value_size)
{
    char resolved[SHELL_SD_PATH_BYTES];
    char *text;
    int n;
    esp_err_t error;

    error = storage_ini_resolve(path, resolved, sizeof(resolved));
    if (error != ESP_OK) {
        return error;
    }
    text = storage_ini_read_file(resolved, NULL);
    if (text == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    n = storage_ini_get_value(text, key, value, value_size);
    free(text);
    return n >= 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

/** Ensure a directory exists on the SD card, creating missing parents. */
esp_err_t storage_mkdir_p(const char *vfs_dir)
{
    shell_sd_session_t session;
    char *buf;
    char *p;
    struct stat st;
    esp_err_t error;

    if (vfs_dir == NULL || vfs_dir[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (shell_sd_begin(&session) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    if (stat(vfs_dir, &st) == 0 && S_ISDIR(st.st_mode)) {
        shell_sd_end(&session, "mkdir");
        return ESP_OK;
    }

    /* Heap (not a 320-byte stack local): callers include the recursive batch
     * path, where a path-sized frame is multiplied by the nesting depth. */
    buf = malloc(P4_CONFIG_SD_PATH_BYTES);
    if (buf == NULL) {
        shell_sd_end(&session, "mkdir");
        return ESP_ERR_NO_MEM;
    }
    snprintf(buf, P4_CONFIG_SD_PATH_BYTES, "%s", vfs_dir);
    for (p = buf + 1; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            if (stat(buf, &st) != 0) {
                (void)mkdir(buf, 0775);
            }
            *p = '/';
        }
    }
    if (stat(buf, &st) != 0) {
        (void)mkdir(buf, 0775);
    }
    error = (stat(buf, &st) == 0 && S_ISDIR(st.st_mode)) ? ESP_OK : ESP_FAIL;
    free(buf);
    shell_sd_end(&session, "mkdir");
    return error;
}

esp_err_t storage_ini_file_set(const char *path, const char *key, const char *value)
{
    char resolved[SHELL_SD_PATH_BYTES];
    char *text;
    bool changed;
    esp_err_t error;

    if (path == NULL || key == NULL || value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    error = storage_ini_resolve(path, resolved, sizeof(resolved));
    if (error != ESP_OK) {
        return error;
    }

    /* Ensure the parent directory exists so `appconfig`-style namespaced
     * config files (e.g. sd:/APPS/APP.INI) can be created on demand. */
    {
        char *slash = strrchr(resolved, '/');

        if (slash != NULL && slash != resolved) {
            *slash = '\0';
            (void)storage_mkdir_p(resolved);
            *slash = '/';
        }
    }

    text = storage_ini_read_file(resolved, NULL);
    if (text == NULL) {
        /* Missing file: create it holding just this key. */
        text = malloc(INI_MAX_BYTES + 1);
        if (text == NULL) {
            return ESP_ERR_NO_MEM;
        }
        text[0] = '\0';
    }
    changed = storage_ini_upsert(text, INI_MAX_BYTES + 1, key, value);
    if (!changed) {
        free(text);
        return ESP_ERR_INVALID_SIZE;
    }
    error = storage_write_text_file(resolved, text);
    free(text);
    return error;
}

esp_err_t storage_ini_file_delete(const char *path, const char *key)
{
    char resolved[SHELL_SD_PATH_BYTES];
    char *text;
    bool changed;
    esp_err_t error;

    error = storage_ini_resolve(path, resolved, sizeof(resolved));
    if (error != ESP_OK) {
        return error;
    }
    text = storage_ini_read_file(resolved, NULL);
    if (text == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    changed = storage_ini_remove(text, INI_MAX_BYTES + 1, key);
    if (!changed) {
        free(text);
        return ESP_ERR_NOT_FOUND;
    }
    error = storage_write_text_file(resolved, text);
    free(text);
    return error;
}

esp_err_t storage_ini_file_foreach(const char *path,
                                   bool (*cb)(const char *key, const char *value, void *ctx),
                                   void *ctx)
{
    char resolved[SHELL_SD_PATH_BYTES];
    char *text;
    const char *cursor;
    bool keep = true;
    esp_err_t error;

    if (cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    error = storage_ini_resolve(path, resolved, sizeof(resolved));
    if (error != ESP_OK) {
        return error;
    }
    text = storage_ini_read_file(resolved, NULL);
    if (text == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    cursor = text;
    while (keep && *cursor != '\0') {
        const char *nl = strchr(cursor, '\n');
        const char *end = nl != NULL ? nl : cursor + strlen(cursor);
        const char *q = cursor;
        const char *eq;

        while (q < end && isspace((unsigned char)*q)) {
            q++;
        }
        if (q < end && (*q == ';' || *q == '#')) {
            cursor = nl != NULL ? nl + 1 : end;
            continue;
        }
        if ((size_t)(end - q) >= 3 && strncasecmp(q, "REM", 3) == 0 &&
            (q + 3 == end || isspace((unsigned char)q[3]))) {
            cursor = nl != NULL ? nl + 1 : end;
            continue;
        }
        if (q == end) {
            cursor = nl != NULL ? nl + 1 : end;
            continue;
        }

        eq = memchr(q, '=', (size_t)(end - q));
        if (eq != NULL) {
            char key[INI_KEY_BYTES];
            char value[INI_VALUE_BYTES];
            size_t key_len = (size_t)(eq - q);
            const char *v = eq + 1;
            size_t value_len = (size_t)(end - v);

            while (key_len > 0 && isspace((unsigned char)q[key_len - 1])) {
                key_len--;
            }
            while (value_len > 0 && isspace((unsigned char)v[value_len - 1])) {
                value_len--;
            }
            while (value_len > 0 && isspace((unsigned char)*v)) {
                v++;
                value_len--;
            }

            if (key_len >= sizeof(key) || value_len >= sizeof(value)) {
                cursor = nl != NULL ? nl + 1 : end;
                continue;
            }
            memcpy(key, q, key_len);
            key[key_len] = '\0';
            memcpy(value, v, value_len);
            value[value_len] = '\0';

            keep = cb(key, value, ctx);
        }
        cursor = nl != NULL ? nl + 1 : end;
    }

    free(text);
    return keep ? ESP_OK : ESP_OK;
}

/* ========================================================================
 * TEMP FILES (all on the SD card, under the temp directory)
 * ======================================================================== */

static void storage_temp_dir_path(char *buf, size_t size)
{
    snprintf(buf, size, "%s/%s", BSP_SD_MOUNT_POINT, P4_CONFIG_TEMP_DIR_NAME);
}

/** Ensure the temp directory exists on the SD card. */
static esp_err_t storage_temp_ensure_dir(void)
{
    char dir[SHELL_SD_PATH_BYTES];
    struct stat st;

    storage_temp_dir_path(dir, sizeof(dir));
    if (stat(dir, &st) == 0 && S_ISDIR(st.st_mode)) {
        return ESP_OK;
    }
    if (mkdir(dir, 0755) == 0) {
        return ESP_OK;
    }
    return (stat(dir, &st) == 0 && S_ISDIR(st.st_mode)) ? ESP_OK : ESP_FAIL;
}

esp_err_t storage_temp_path(char *buf, size_t size, const char *ext)
{
    char dir[SHELL_SD_PATH_BYTES];
    struct stat st;
    static unsigned counter = 0;

    if (buf == NULL || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (storage_temp_ensure_dir() != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    storage_temp_dir_path(dir, sizeof(dir));
    if (ext == NULL || ext[0] == '\0') {
        ext = "tmp";
    }

    for (int attempt = 0; attempt < 100; attempt++) {
        int n = snprintf(buf, size, "%s/_app%u.%s", dir, counter++, ext);

        if (n < 0 || (size_t)n >= size) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (stat(buf, &st) != 0) {
            /* Free name: create it so the path is reserved for the caller. */
            FILE *f = fopen(buf, "w");

            if (f == NULL) {
                return ESP_FAIL;
            }
            fclose(f);
            return ESP_OK;
        }
    }
    return ESP_FAIL;
}

esp_err_t storage_temp_cleanup(void)
{
    char dir[SHELL_SD_PATH_BYTES];
    DIR *d;
    struct dirent *e;

    if (storage_temp_ensure_dir() != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    storage_temp_dir_path(dir, sizeof(dir));
    d = opendir(dir);
    if (d == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    while ((e = readdir(d)) != NULL) {
        char full[SHELL_SD_PATH_BYTES + P4_CONFIG_LFN_BYTES];
        int n;

        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        n = snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
        if (n < 0 || (size_t)n >= sizeof(full)) {
            continue;
        }
        (void)unlink(full);
    }
    closedir(d);
    return ESP_OK;
}
