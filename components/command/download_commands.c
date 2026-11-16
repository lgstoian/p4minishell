/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file download_commands.c
 * @brief `download` — stream an HTTP resource to SD with progress and resume.
 *
 * Uses the ONE HTTP client (`networking_http_get_stream_ex`) and the ONE
 * guarded SD session (`shell_sd_begin`/`shell_sd_end`); there is no second
 * downloader. `/resume` sends a `Range: bytes=<offset>-` header and appends,
 * so an interrupted transfer can continue instead of restarting.
 *
 * ERRORLEVEL: 0 ok / 1 fetch/write failed / 2 usage.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "shell.h"
#include "batch.h"
#include "command.h"
#include "storage.h"
#include "networking.h"
#include "ansi_palette.h"
#include "p4minishell_config.h"
#include "esp_timer.h"

#define DOWNLOAD_TAG "download"

/** Minimum interval between progress lines (the transcript is slow). */
#define DOWNLOAD_PROGRESS_INTERVAL_MS 200

typedef struct {
    FILE *file;
    uint64_t written;
    uint64_t total;
    uint32_t last_report_ms;
    bool progress;
    bool failed;
} download_ctx_t;

static esp_err_t download_cb(const uint8_t *chunk, size_t len, size_t total,
                             void *ctx)
{
    download_ctx_t *d = (download_ctx_t *)ctx;
    uint32_t now_ms;

    if (d->file == NULL || d->failed) {
        return ESP_FAIL;
    }
    if (len > 0 && fwrite(chunk, 1, len, d->file) != len) {
        d->failed = true;
        return ESP_FAIL;
    }
    d->written += (uint64_t)len;
    if (total > 0) {
        d->total = (uint64_t)total;
    }
    if (!d->progress) {
        return ESP_OK;
    }
    now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (d->last_report_ms == 0 ||
        (now_ms - d->last_report_ms) >= DOWNLOAD_PROGRESS_INTERVAL_MS) {
        d->last_report_ms = now_ms;
        if (d->total > 0) {
            shell_transcript_appendf(DOWNLOAD_TAG ": " SH_NUM "%llu/%llu" SH_RST " bytes\n",
                                     (unsigned long long)d->written,
                                     (unsigned long long)d->total);
        } else {
            shell_transcript_appendf(DOWNLOAD_TAG ": " SH_NUM "%llu" SH_RST " bytes\n",
                                     (unsigned long long)d->written);
        }
    }
    return ESP_OK;
}

/** `download <url> <path> [/resume] [/progress]`. */
static void download_run(const char *url, const char *path,
                         bool resume, bool progress)
{
    char rel[P4_CONFIG_SD_PATH_BYTES];
    char resolved[P4_CONFIG_SD_PATH_BYTES];
    char range[64];
    networking_http_header_t headers[1];
    size_t header_count = 0;
    shell_sd_session_t session;
    download_ctx_t ctx;
    esp_err_t error;
    uint64_t offset = 0;
    const char *mode = "wb";

    if (url == NULL || url[0] == '\0' || path == NULL || path[0] == '\0') {
        shell_print_usage("Usage: download <url> <path> [/resume] [/progress]");
        batch_set_errorlevel(2);
        return;
    }
    snprintf(rel, sizeof(rel), "%s", path);
    if (shell_fs_resolve_path(rel, resolved, sizeof(resolved)) != ESP_OK) {
        shell_print_error(DOWNLOAD_TAG ": invalid path %s", path);
        batch_set_errorlevel(2);
        return;
    }

    if (resume) {
        struct stat st;

        if (stat(resolved, &st) == 0 && st.st_size > 0) {
            offset = (uint64_t)st.st_size;
            mode = "ab";
        }
    }
    if (shell_sd_begin(&session) != ESP_OK) {
        shell_print_error(DOWNLOAD_TAG ": SD card not present");
        batch_set_errorlevel(1);
        return;
    }
    /* Create the destination directory (the default target is sd:/DOWNLOAD/). */
    {
        char parent[P4_CONFIG_SD_PATH_BYTES];
        const char *slash = strrchr(resolved, '/');

        if (slash != NULL && slash != resolved) {
            size_t n = (size_t)(slash - resolved);

            if (n < sizeof(parent)) {
                memcpy(parent, resolved, n);
                parent[n] = '\0';
                (void)storage_mkdir_p(parent);
            }
        }
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.file = fopen(resolved, mode);
    if (ctx.file == NULL) {
        shell_sd_end(&session, DOWNLOAD_TAG);
        shell_print_error(DOWNLOAD_TAG ": cannot create %s", resolved);
        batch_set_errorlevel(1);
        return;
    }
    ctx.progress = progress;
    ctx.written = offset;
    shell_sd_end(&session, DOWNLOAD_TAG);

    if (offset > 0) {
        snprintf(range, sizeof(range), "bytes=%llu-", (unsigned long long)offset);
        headers[0].name = "Range";
        headers[0].value = range;
        header_count = 1;
        shell_transcript_appendf(DOWNLOAD_TAG ": resuming at " SH_NUM "%llu" SH_RST " bytes\n",
                                 (unsigned long long)offset);
    }

    error = networking_http_get_stream_ex(url, headers, header_count,
                                          download_cb, &ctx);
    if (fclose(ctx.file) != 0 && error == ESP_OK) {
        error = ESP_FAIL;
    }
    if (error != ESP_OK || ctx.failed) {
        /* Remove a partial file so it is never mistaken for a complete one. */
        (void)unlink(resolved);
        shell_transcript_appendf_ansi(SH_ERR DOWNLOAD_TAG ": download failed (%s)\n" SH_RST,
                                      esp_err_to_name(error));
        batch_set_errorlevel(1);
        return;
    }
    shell_transcript_appendf(DOWNLOAD_TAG ": saved " SH_VAL "%s" SH_RST
                             " (" SH_NUM "%llu" SH_RST " bytes)\n",
                             resolved, (unsigned long long)ctx.written);
    batch_set_errorlevel(0);
}

void shell_command_download(int argc, char **argv)
{
    const char *url = NULL;
    const char *path = NULL;
    bool resume = false;
    bool progress = false;
    int i;

    for (i = 1; i < argc; i++) {
        const char *tok = argv[i];

        if (tok[0] == '/') {
            if (shell_text_equals_ignore_case(tok, "/resume")) {
                resume = true;
            } else if (shell_text_equals_ignore_case(tok, "/progress")) {
                progress = true;
            } else {
                shell_print_usage("Usage: download <url> <path> [/resume] [/progress]");
                batch_set_errorlevel(2);
                return;
            }
            continue;
        }
        if (url == NULL) {
            url = tok;
        } else if (path == NULL) {
            path = tok;
        } else {
            shell_print_usage("Usage: download <url> <path> [/resume] [/progress]");
            batch_set_errorlevel(2);
            return;
        }
    }
    if (url == NULL) {
        shell_print_usage("Usage: download <url> <path> [/resume] [/progress]");
        batch_set_errorlevel(2);
        return;
    }
    if (path == NULL) {
        /* Default into the public download directory (D2). */
        static char default_path[P4_CONFIG_SD_PATH_BYTES];
        const char *slash;
        char name[96];
        size_t k;

        /* Last path segment of the URL, sanitised to a safe filename. */
        slash = strrchr(url, '/');
        snprintf(name, sizeof(name), "%s", slash != NULL ? slash + 1 : "download.bin");
        for (k = 0; name[k] != '\0'; k++) {
            char c = name[k];
            if (!isalnum((unsigned char)c) && c != '.' && c != '_' && c != '-') {
                name[k] = '_';
            }
        }
        snprintf(default_path, sizeof(default_path), "%s/%s",
                 P4_CONFIG_DOWNLOAD_DIR_NAME, name);
        path = default_path;
    }
    download_run(url, path, resume, progress);
}
