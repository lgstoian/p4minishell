/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file asset_commands.c
 * @brief `crc32` + `asset` verbs: file checksums and app asset manifests.
 *
 * `crc32 <path>` streams a file through the firmware's single CRC-32
 * primitive (shell_crc32_update, shared with `receive`). `asset check|list
 * <app>` verifies/lists `APPS/<APP>.ASSETS`, a text manifest of
 * `path=HEXCRC` lines (comments `#`/`;`, blanks skipped) that batch tools
 * generate after pushing game/app assets. Manifest parsing is pure
 * (asset_parse_line) and unit-tested; verbs are thin file shells around it.
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
#include "ansi_palette.h"
#include "p4minishell_config.h"
#include "esp_heap_caps.h"

/** Manifest cap (same class as INI 16K: manifests are small by design). */
#define ASSET_MANIFEST_MAX_BYTES 16384

/** Streaming chunk for crc32 (SD DMA-friendly 4K). */
#define ASSET_CRC_CHUNK_BYTES 4096

/** Longest manifest line accepted (TEXT_LINE parity). */
#define ASSET_LINE_MAX_BYTES 512

uint32_t asset_crc32_data(const uint8_t *data, size_t len)
{
    if (data == NULL) return 0;
    return ~shell_crc32_update(0xFFFFFFFFu, data, len);
}

/** True for app-manifest-safe names: [A-Za-z0-9_-]+ (no slashes, dots, or
 * spaces — manifests address SD-relative paths, never escapes). */
static bool asset_name_ok(const char *s)
{
    size_t i;

    if (s == NULL || *s == '\0') return false;
    for (i = 0; s[i] != '\0'; i++) {
        char c = s[i];

        if (!isalnum((unsigned char)c) && c != '_' && c != '-' && c != '/' && c != '.') {
            return false;
        }
    }
    return true;
}

bool asset_parse_line(const char *line, char *path_out, size_t path_size,
                      uint32_t *crc_out)
{
    const char *eq;
    const char *hex;
    size_t path_len;
    uint32_t crc = 0;
    int digits = 0;

    if (path_out != NULL && path_size > 0) {
        path_out[0] = '\0';
    }
    if (line == NULL || path_out == NULL || path_size == 0 || crc_out == NULL) {
        return false;
    }
    /* Skip leading whitespace; blanks and comments are "not entries". */
    while (*line == ' ' || *line == '\t' || *line == '\r' || *line == '\n') {
        line++;
    }
    if (*line == '\0' || *line == '#' || *line == ';') {
        return false;
    }
    eq = strchr(line, '=');
    if (eq == NULL || eq == line) {
        return false;
    }
    /* Trim trailing whitespace off the path half. */
    path_len = (size_t)(eq - line);
    while (path_len > 0 &&
           (line[path_len - 1] == ' ' || line[path_len - 1] == '\t')) {
        path_len--;
    }
    if (path_len == 0 || path_len >= path_size) {
        return false;
    }
    memcpy(path_out, line, path_len);
    path_out[path_len] = '\0';
    if (!asset_name_ok(path_out)) {
        path_out[0] = '\0';
        return false;
    }
    /* Refuse escapes and absolute paths even when the charset passes. */
    if (strstr(path_out, "..") != NULL || path_out[0] == '/') {
        path_out[0] = '\0';
        return false;
    }
    /* CRC half: exactly 8 hex digits (trailing whitespace tolerated). */
    hex = eq + 1;
    while (*hex == ' ' || *hex == '\t') {
        hex++;
    }
    for (; *hex != '\0' && *hex != '\r' && *hex != '\n'; hex++) {
        char c = *hex;

        if (c == ' ' || c == '\t') {
            break;
        }
        if (!isxdigit((unsigned char)c) || digits >= 8) {
            path_out[0] = '\0';
            return false;
        }
        crc = (crc << 4) | (uint32_t)(isdigit((unsigned char)c) ? c - '0'
                                      : tolower((unsigned char)c) - 'a' + 10);
        digits++;
    }
    if (digits != 8) {
        path_out[0] = '\0';
        return false;
    }
    while (*hex == ' ' || *hex == '\t') {
        hex++;
    }
    if (*hex != '\0' && *hex != '\r' && *hex != '\n') {
        path_out[0] = '\0';
        return false;
    }
    *crc_out = crc;
    return true;
}

/** Stream a resolved file through CRC-32. @return true with @p crc_out set. */
bool asset_crc_file(const char *resolved, uint32_t *crc_out)
{
    shell_sd_session_t session;
    FILE *file = NULL;
    uint8_t *chunk = NULL;
    size_t got;
    uint32_t crc = 0xFFFFFFFFu;
    bool ok = false;

    if (shell_sd_begin(&session) != ESP_OK) {
        return false;
    }
    file = fopen(resolved, "rb");
    if (file == NULL) {
        shell_sd_end(&session, "crc32");
        return false;
    }
    chunk = heap_caps_malloc(ASSET_CRC_CHUNK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (chunk == NULL) {
        chunk = malloc(ASSET_CRC_CHUNK_BYTES);
    }
    if (chunk == NULL) {
        fclose(file);
        shell_sd_end(&session, "crc32");
        return false;
    }
    ok = true;
    while ((got = fread(chunk, 1, ASSET_CRC_CHUNK_BYTES, file)) > 0) {
        crc = shell_crc32_update(crc, chunk, got);
    }
    if (ferror(file)) {
        ok = false;
    }
    heap_caps_free(chunk);
    fclose(file);
    shell_sd_end(&session, "crc32");
    if (ok && crc_out != NULL) {
        *crc_out = ~crc;
    }
    return ok;
}

/** App names for `asset`/`pkg`: [A-Za-z0-9_-]+ (validated before path use). */
bool asset_app_ok(const char *app)
{
    size_t i;

    if (app == NULL || *app == '\0') return false;
    for (i = 0; app[i] != '\0'; i++) {
        char c = app[i];

        if (!isalnum((unsigned char)c) && c != '_' && c != '-') {
            return false;
        }
    }
    return true;
}

void shell_command_crc32(int argc, char **argv)
{
    char resolved[P4_CONFIG_SD_PATH_BYTES];
    uint32_t crc;

    if (argc != 2) {
        shell_print_usage("Usage: crc32 <path>");
        batch_set_errorlevel(2);
        return;
    }
    if (shell_fs_resolve_path(argv[1], resolved, sizeof(resolved)) != ESP_OK) {
        shell_transcript_appendf_ansi(SH_ERR "crc32: invalid path %s\n" SH_RST, argv[1]);
        batch_set_errorlevel(2);
        return;
    }
    if (!asset_crc_file(resolved, &crc)) {
        shell_transcript_appendf_ansi(SH_ERR "crc32: cannot read %s\n" SH_RST, resolved);
        batch_set_errorlevel(1);
        return;
    }
    shell_transcript_appendf("crc32: %s %08X\n", resolved, (unsigned)crc);
    batch_set_errorlevel(0);
}

void shell_command_asset(int argc, char **argv)
{
    bool list_only;

    if (argc != 3 ||
        (!shell_text_equals_ignore_case(argv[1], "check") &&
         !shell_text_equals_ignore_case(argv[1], "list"))) {
        shell_print_usage("Usage: asset check|list <app>");
        batch_set_errorlevel(2);
        return;
    }
    list_only = shell_text_equals_ignore_case(argv[1], "list");
    (void)asset_verify_app("asset", argv[2], list_only);
}

/**
 * Verify (or list) an app's `APPS/<app>.ASSETS` manifest. Shared by the `asset`
 * verb and `pkg verify` so there is exactly one CRC-checking implementation.
 * Messages are prefixed with @p tag. Sets and returns ERRORLEVEL:
 * 0 ok / 1 missing|mismatch|empty / 2 usage|no SD.
 */
int asset_verify_app(const char *tag, const char *app, bool list_only)
{
    char manifest_arg[P4_CONFIG_SD_PATH_BYTES];
    char resolved[P4_CONFIG_SD_PATH_BYTES];
    shell_sd_session_t session;
    FILE *file = NULL;
    char *text = NULL;
    size_t got = 0;
    int ok = 0;
    int failed = 0;
    char *save = NULL;
    char *ln;
    const char *pfx = (tag != NULL) ? tag : "asset";

    if (!asset_app_ok(app)) {
        shell_transcript_appendf_ansi(SH_ERR "%s: bad app name '%s'\n" SH_RST, pfx, app);
        batch_set_errorlevel(2);
        return 2;
    }
    snprintf(manifest_arg, sizeof(manifest_arg), "%s/%s.ASSETS", P4_CONFIG_PKG_APPS_DIR_NAME, app);
    if (shell_fs_resolve_path(manifest_arg, resolved, sizeof(resolved)) != ESP_OK) {
        shell_transcript_appendf_ansi(SH_ERR "%s: invalid manifest %s\n" SH_RST, pfx, manifest_arg);
        batch_set_errorlevel(2);
        return 2;
    }
    if (shell_sd_begin(&session) != ESP_OK) {
        shell_transcript_appendf_ansi(SH_ERR "%s: SD card not present\n" SH_RST, pfx);
        batch_set_errorlevel(1);
        return 1;
    }
    file = fopen(resolved, "rb");
    if (file == NULL) {
        shell_transcript_appendf_ansi(SH_ERR "%s: no manifest %s (install or push one first)\n" SH_RST, pfx, resolved);
        shell_sd_end(&session, pfx);
        batch_set_errorlevel(1);
        return 1;
    }
    text = heap_caps_malloc(ASSET_MANIFEST_MAX_BYTES + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (text == NULL) {
        text = malloc(ASSET_MANIFEST_MAX_BYTES + 1);
    }
    if (text == NULL) {
        shell_transcript_appendf_ansi(SH_ERR "%s: out of memory\n" SH_RST, pfx);
        fclose(file);
        shell_sd_end(&session, pfx);
        batch_set_errorlevel(1);
        return 1;
    }
    got = fread(text, 1, ASSET_MANIFEST_MAX_BYTES, file);
    fclose(file);
    shell_sd_end(&session, pfx);
    text[got] = '\0';

    /* Split in place; overlong lines fail the entry loudly. */
    for (ln = strtok_r(text, "\n", &save); ln != NULL; ln = strtok_r(NULL, "\n", &save)) {
        char path[P4_CONFIG_SD_PATH_BYTES];
        uint32_t expect = 0;
        const char *trim = ln;

        while (*trim == ' ' || *trim == '\t' || *trim == '\r') {
            trim++;
        }
        if (*trim == '\0' || *trim == '#' || *trim == ';') {
            continue;
        }
        if (strlen(ln) > ASSET_LINE_MAX_BYTES) {
            shell_transcript_appendf_ansi(SH_ERR "%s: overlong line (%u max)\n" SH_RST,
                                          pfx, (unsigned)ASSET_LINE_MAX_BYTES);
            failed++;
            continue;
        }
        if (!asset_parse_line(ln, path, sizeof(path), &expect)) {
            shell_transcript_appendf_ansi(SH_ERR "%s: malformed line: %s\n" SH_RST, pfx, ln);
            failed++;
            continue;
        }
        if (list_only) {
            shell_transcript_appendf("%s: %s %08X\n", pfx, path, (unsigned)expect);
            ok++;
            continue;
        }
        {
            char entry[P4_CONFIG_SD_PATH_BYTES];
            uint32_t actual = 0;

            if (shell_fs_resolve_path(path, entry, sizeof(entry)) != ESP_OK ||
                !asset_crc_file(entry, &actual)) {
                shell_transcript_appendf_ansi(SH_ERR "%s: MISSING %s\n" SH_RST, pfx, path);
                failed++;
                continue;
            }
            if (actual != expect) {
                shell_transcript_appendf_ansi(SH_ERR "%s: MISMATCH %s (want %08X got %08X)\n" SH_RST,
                                              pfx, path, (unsigned)expect, (unsigned)actual);
                failed++;
                continue;
            }
            ok++;
        }
    }
    heap_caps_free(text);
    if (list_only) {
        batch_set_errorlevel(0);
        return 0;
    }
    if (failed == 0 && ok == 0) {
        shell_transcript_appendf("%s: manifest empty (nothing to check)\n", pfx);
        batch_set_errorlevel(1);
        return 1;
    }
    shell_transcript_appendf("%s: %s %d/%d ok\n", pfx, failed == 0 ? "OK" : "FAIL",
                             ok, ok + failed);
    batch_set_errorlevel(failed == 0 ? 0 : 1);
    return failed == 0 ? 0 : 1;
}
