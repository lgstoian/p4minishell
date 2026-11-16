/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file store_commands.c
 * @brief `store` — remote app-store client over the shared networking stack.
 *
 * The store is a thin client over the SAME primitives the rest of the shell
 * uses: `networking_http_get_stream()` downloads, `json_walk()` parses the
 * index (there is no second JSON parser), and `pkg install` installs the
 * downloaded `.PKG` through the ordinary bundle flow. It adds no runtime, no
 * second dispatcher, and no second package format.
 *
 * The index is a JSON document shaped like ESP-Brookesia's app-store index:
 *
 *     { "apps": [ { "package_name": "demo", "latest_version": "1.0",
 *                   "app_name": {"en": "Demo"}, "bpk_url": "https://.../demo.PKG" } ] }
 *
 * `bpk_url` is downloaded to `PKGS/<id>.PKG` (the `.PKG` archive bundle), so
 * the device-side contract is unchanged. The index URL is the compile-time
 * `P4_CONFIG_STORE_INDEX_URL` (empty disables the verb).
 *
 * ERRORLEVEL: 0 ok / 1 fetch/parse/install failed / 2 usage.
 */

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shell.h"
#include "batch.h"
#include "command.h"
#include "storage.h"
#include "networking.h"
#include "json.h"
#include "ansi_palette.h"
#include "p4minishell_config.h"
#include "esp_heap_caps.h"
#include "p4heap.h"

#define STORE_TAG "store"

/** Index cap (same class as the INI/asset manifests: small by design). */
#define STORE_INDEX_MAX_BYTES 16384

#define STORE_ID_MAX    64
#define STORE_NAME_MAX  96
#define STORE_URL_MAX   256
#define STORE_VER_MAX   32

typedef struct {
    char id[STORE_ID_MAX];
    char version[STORE_VER_MAX];
    char url[STORE_URL_MAX];
    char name[STORE_NAME_MAX];
} store_entry_t;

typedef struct {
    store_entry_t *items;
    int count;
    int cap;
} store_index_t;

/** Parse an `apps.<n>.<field>` path into its array index and field name.
 *  Pure and unit-tested (test/main/test_pkg.c). */
bool store_path_parse(const char *path, int *index_out,
                      const char **field_out, size_t *field_len_out)
{
    const char *p = path;
    int idx = 0;

    if (path == NULL || index_out == NULL || field_out == NULL ||
        field_len_out == NULL) {
        return false;
    }
    if (strncmp(p, "apps.", 5) != 0) {
        return false;
    }
    p += 5;
    if (!isdigit((unsigned char)*p)) {
        return false;
    }
    while (isdigit((unsigned char)*p)) {
        idx = idx * 10 + (*p - '0');
        p++;
    }
    if (*p != '.') {
        return false;
    }
    p++;
    *index_out = idx;
    *field_out = p;
    *field_len_out = strlen(p);
    return true;
}

/** Store one scalar into the right field of the entry at @p i. */
static void store_entry_set(store_index_t *idx, int i, const char *field,
                            size_t flen, const char *value, size_t vlen)
{
    store_entry_t *e;
    char tmp[STORE_NAME_MAX];
    size_t n;

    if (i < 0) {
        return;
    }
    if (i >= idx->cap) {
        int newcap = idx->cap ? idx->cap * 2 : 8;
        store_entry_t *grown;

        if (i >= newcap) {
            newcap = i + 1;
        }
        grown = p4heap_alloc_psram((size_t)newcap * sizeof(*grown));
        if (grown == NULL) {
            return;
        }
        if (idx->items != NULL && idx->count > 0) {
            memcpy(grown, idx->items, (size_t)idx->count * sizeof(*grown));
        }
        heap_caps_free(idx->items);
        idx->items = grown;
        idx->cap = newcap;
    }
    if (i >= idx->count) {
        memset(&idx->items[i], 0, sizeof(idx->items[i]));
        idx->count = i + 1;
    }
    e = &idx->items[i];
    n = (vlen < sizeof(tmp) - 1) ? vlen : sizeof(tmp) - 1;
    memcpy(tmp, value, n);
    tmp[n] = '\0';

    if (flen == 2 && strncmp(field, "id", 2) == 0) {
        snprintf(e->id, sizeof(e->id), "%.63s", tmp);
    } else if (flen == 7 && strncmp(field, "version", 7) == 0) {
        snprintf(e->version, sizeof(e->version), "%.31s", tmp);
    } else if (flen == 7 && strncmp(field, "bpk_url", 7) == 0) {
        snprintf(e->url, sizeof(e->url), "%.255s", tmp);
    } else if ((flen == 11 && strncmp(field, "app_name.en", 11) == 0) ||
               (flen == 6 && strncmp(field, "name.en", 6) == 0)) {
        snprintf(e->name, sizeof(e->name), "%.95s", tmp);
    }
}

static bool store_index_visit(const char *path, const char *value,
                              size_t value_len, void *ctx)
{
    store_index_t *idx = ctx;
    int index = 0;
    const char *field = NULL;
    size_t flen = 0;

    if (!store_path_parse(path, &index, &field, &flen)) {
        return true;   /* not an app field: keep walking */
    }
    store_entry_set(idx, index, field, flen, value, value_len);
    return true;
}

/** Fetch and parse the index. @return true with @p idx filled. */
static bool store_fetch_index(store_index_t *idx)
{
    networking_http_result_t result;
    char err[128];
    bool ok = false;

    memset(idx, 0, sizeof(*idx));
    memset(&result, 0, sizeof(result));
    /* The quiet GET: `store list` reports its own rows, no transcript chatter. */
    if (networking_http_get_ex(P4_CONFIG_STORE_INDEX_URL, NULL, 0,
                               NULL, NULL, NULL, &result) != ESP_OK) {
        shell_transcript_appendf_ansi(SH_ERR "store: cannot fetch %s\n" SH_RST,
                                      P4_CONFIG_STORE_INDEX_URL);
        return false;
    }
    if (result.body == NULL || result.body_size == 0) {
        shell_transcript_appendf_ansi(SH_ERR "store: empty index\n" SH_RST);
        networking_http_result_free(&result);
        return false;
    }
    err[0] = '\0';
    if (json_walk((const char *)result.body, result.body_size,
                  store_index_visit, idx, err, sizeof(err))) {
        ok = (idx->count > 0);
        if (!ok) {
            shell_transcript_appendf_ansi(SH_ERR "store: index has no apps[]\n" SH_RST);
        }
    } else {
        shell_transcript_appendf_ansi(SH_ERR "store: bad index JSON (%s)\n" SH_RST, err);
    }
    networking_http_result_free(&result);
    return ok;
}

static void store_index_free(store_index_t *idx)
{
    heap_caps_free(idx->items);
    idx->items = NULL;
    idx->count = 0;
    idx->cap = 0;
}

/** `store url`: print the configured index URL. */
static void store_url(void)
{
    if (P4_CONFIG_STORE_INDEX_URL[0] == '\0') {
        shell_transcript_append_text("store: no index URL configured "
                                     "(set P4_CONFIG_STORE_INDEX_URL)\n");
        batch_set_errorlevel(1);
        return;
    }
    shell_transcript_appendf("store: %s\n", P4_CONFIG_STORE_INDEX_URL);
    batch_set_errorlevel(0);
}

/** `store list`: fetch the index and print id/version/name rows. */
static void store_list(void)
{
    store_index_t idx;
    int i;

    if (P4_CONFIG_STORE_INDEX_URL[0] == '\0') {
        shell_transcript_appendf_ansi(SH_ERR "store: no index URL configured "
                                            "(set P4_CONFIG_STORE_INDEX_URL)\n" SH_RST);
        batch_set_errorlevel(1);
        return;
    }
    if (!store_fetch_index(&idx)) {
        batch_set_errorlevel(1);
        return;
    }
    shell_transcript_appendf("store: %d app(s) at %s\n", idx.count,
                             P4_CONFIG_STORE_INDEX_URL);
    for (i = 0; i < idx.count; i++) {
        const char *name = idx.items[i].name[0] ? idx.items[i].name
                                                : idx.items[i].id;

        shell_transcript_appendf("  %-16s v%-10s %s\n",
                                 idx.items[i].id,
                                 idx.items[i].version[0] ? idx.items[i].version : "-",
                                 name);
    }
    store_index_free(&idx);
    batch_set_errorlevel(0);
}

/* ------------------------------------------------------------------------- */
/* Download                                                                  */
/* ------------------------------------------------------------------------- */

typedef struct {
    FILE *file;
    uint64_t written;
    bool failed;
} store_dl_t;

static esp_err_t store_dl_cb(const uint8_t *chunk, size_t len, size_t total,
                             void *ctx)
{
    store_dl_t *dl = ctx;

    (void)total;
    if (dl->file == NULL || dl->failed) {
        return ESP_FAIL;
    }
    if (len > 0 && fwrite(chunk, 1, len, dl->file) != len) {
        dl->failed = true;
        return ESP_FAIL;
    }
    dl->written += len;
    return ESP_OK;
}

/** Stream @p url to `PKGS/<app>.PKG` through a guarded SD session. */
static bool store_download(const char *url, const char *app)
{
    char rel[P4_CONFIG_SD_PATH_BYTES];
    char dest[P4_CONFIG_SD_PATH_BYTES];
    shell_sd_session_t session;
    store_dl_t dl;
    esp_err_t err;

    snprintf(rel, sizeof(rel), "%s/%s%s",
             P4_CONFIG_PKG_BUNDLE_DIR_NAME, app, P4_CONFIG_PKG_ARCHIVE_EXT);
    if (shell_fs_resolve_path(rel, dest, sizeof(dest)) != ESP_OK) {
        return false;
    }
    if (shell_sd_begin(&session) != ESP_OK) {
        return false;
    }
    dl.file = fopen(dest, "wb");
    if (dl.file == NULL) {
        shell_sd_end(&session, STORE_TAG);
        return false;
    }
    dl.written = 0;
    dl.failed = false;
    err = networking_http_get_stream(url, store_dl_cb, &dl);
    fclose(dl.file);
    shell_sd_end(&session, STORE_TAG);
    if (err != ESP_OK || dl.failed || dl.written == 0) {
        (void)unlink(dest);
        return false;
    }
    return true;
}

/** `store install <id>`: download the entry's archive, then `pkg install`. */
static void store_install(const char *app)
{
    store_index_t idx;
    int i;
    const char *url = NULL;

    if (!asset_app_ok(app)) {
        shell_transcript_appendf_ansi(SH_ERR "store: bad app name '%s'\n" SH_RST, app);
        batch_set_errorlevel(2);
        return;
    }
    if (!store_fetch_index(&idx)) {
        batch_set_errorlevel(1);
        return;
    }
    for (i = 0; i < idx.count; i++) {
        if (strcmp(idx.items[i].id, app) == 0) {
            url = idx.items[i].url;
            break;
        }
    }
    if (url == NULL || url[0] == '\0') {
        shell_transcript_appendf_ansi(SH_ERR "store: %s has no bpk_url in the index\n" SH_RST, app);
        store_index_free(&idx);
        batch_set_errorlevel(1);
        return;
    }
    shell_transcript_appendf("store: downloading %s\n", url);
    if (!store_download(url, app)) {
        shell_transcript_appendf_ansi(SH_ERR "store: download failed\n" SH_RST);
        store_index_free(&idx);
        batch_set_errorlevel(1);
        return;
    }
    store_index_free(&idx);
    /* Install through the ordinary bundle flow (auto-unpacks the .PKG). */
    pkg_install_app(app, (P4_CONFIG_PKG_REQUIRE_SIGN != 0), false);
}

void shell_command_store(int argc, char **argv)
{
    if (argc == 2 && shell_text_equals_ignore_case(argv[1], "url")) {
        store_url();
        return;
    }
    if (argc == 2 && shell_text_equals_ignore_case(argv[1], "list")) {
        store_list();
        return;
    }
    if (argc == 3 && shell_text_equals_ignore_case(argv[1], "install")) {
        store_install(argv[2]);
        return;
    }
    shell_print_usage("Usage: store url | list | install <id>");
    batch_set_errorlevel(2);
}
