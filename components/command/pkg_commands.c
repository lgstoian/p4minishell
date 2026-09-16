/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file pkg_commands.c
 * @brief `pkg` — SD application packages.
 *
 * A package is metadata plus a contents manifest:
 *   APPS/<APP>.APPINFO   INI: title=, description=, version=
 *   APPS/<APP>.ASSETS    `path=HEXCRC` lines (shared with `asset`)
 * The install source is a bundle directory:
 *   PKGS/<APP>/<APP>.ASSETS   the same manifest
 *   PKGS/<APP>/<path>         each payload file at its install-relative path
 *   PKGS/<APP>/<APP>.APPINFO  the metadata file
 *
 * Verbs: list, info, verify, check, install, remove. Verification reuses the
 * single CRC/manifest implementation (`asset_parse_line`, `asset_crc_file`,
 * `asset_verify_app`) and copies reuse `shell_fs_copy_file`; removal reuses
 * `storage_trash_delete_file`. Nothing here re-implements those.
 *
 * ERRORLEVEL: 0 ok / 1 some package failed / 2 usage.
 */

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#include "shell.h"
#include "batch.h"
#include "command.h"
#include "storage.h"
#include "ansi_palette.h"
#include "p4minishell_config.h"
#include "esp_heap_caps.h"

#define PKG_APP_NAME_BYTES 32
#define PKG_MAX_ENTRIES    P4_CONFIG_PKG_MAX_ENTRIES

/** `APPS/<app>.APPINFO` (relative) into @p out. */
static void pkg_appinfo_rel(const char *app, char *out, size_t size)
{
    snprintf(out, size, "%s/%s.APPINFO", P4_CONFIG_PKG_APPS_DIR_NAME, app);
}

/** `APPS/<app>.ASSETS` (relative) into @p out. */
static void pkg_manifest_rel(const char *app, char *out, size_t size)
{
    snprintf(out, size, "%s/%s.ASSETS", P4_CONFIG_PKG_APPS_DIR_NAME, app);
}

/** Read a resolved/relative file into a heap buffer (cap manifest size). */
static bool pkg_read_file(const char *path, char **text_out, size_t *len_out)
{
    char resolved[P4_CONFIG_SD_PATH_BYTES];
    shell_sd_session_t session;
    FILE *file;
    char *text;
    size_t got;

    if (text_out == NULL) return false;
    *text_out = NULL;
    if (len_out != NULL) *len_out = 0;
    if (shell_fs_resolve_path(path, resolved, sizeof(resolved)) != ESP_OK) {
        return false;
    }
    if (shell_sd_begin(&session) != ESP_OK) {
        return false;
    }
    file = fopen(resolved, "rb");
    if (file == NULL) {
        shell_sd_end(&session, "pkg");
        return false;
    }
    text = heap_caps_malloc(P4_CONFIG_PKG_MANIFEST_BYTES + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (text == NULL) {
        text = malloc(P4_CONFIG_PKG_MANIFEST_BYTES + 1);
    }
    if (text == NULL) {
        fclose(file);
        shell_sd_end(&session, "pkg");
        return false;
    }
    got = fread(text, 1, P4_CONFIG_PKG_MANIFEST_BYTES, file);
    fclose(file);
    shell_sd_end(&session, "pkg");
    text[got] = '\0';
    *text_out = text;
    if (len_out != NULL) *len_out = got;
    return true;
}

/** Read one APPINFO key (`title`/`description`/`version`). Empty on miss. */
static void pkg_meta(const char *app, const char *key, char *out, size_t size)
{
    char rel[P4_CONFIG_SD_PATH_BYTES];

    if (out == NULL || size == 0) return;
    out[0] = '\0';
    pkg_appinfo_rel(app, rel, sizeof(rel));
    (void)storage_ini_file_get(rel, key, out, size);
}

/** True for a `NAME.APPINFO` filename; writes the uppercased NAME to @p out.
 * Pure (no I/O) so it is unit-testable. */
bool pkg_app_name_from_appinfo(const char *filename, char *out, size_t size)
{
    size_t len;
    int i;

    if (out != NULL && size > 0) out[0] = '\0';
    if (filename == NULL || out == NULL || size == 0) return false;
    len = strlen(filename);
    if (len < 9) return false; /* "X.APPINFO" is 9 */
    if (strcasecmp(filename + len - 8, ".APPINFO") != 0) return false;
    len -= 8;
    if (len >= size) return false;
    for (i = 0; i < (int)len; i++) {
        out[i] = (char)toupper((unsigned char)filename[i]);
    }
    out[len] = '\0';
    return asset_app_ok(out);
}

/** Collect installed app names by scanning the APPINFO files in APPS/. */
static int pkg_collect_apps(char names[][PKG_APP_NAME_BYTES], int max)
{
    char dir_rel[P4_CONFIG_SD_PATH_BYTES];
    char dir_resolved[P4_CONFIG_SD_PATH_BYTES];
    shell_sd_session_t session;
    DIR *dir;
    struct dirent *entry;
    int count = 0;

    snprintf(dir_rel, sizeof(dir_rel), "%s", P4_CONFIG_PKG_APPS_DIR_NAME);
    if (shell_fs_resolve_path(dir_rel, dir_resolved, sizeof(dir_resolved)) != ESP_OK) {
        return 0;
    }
    if (shell_sd_begin(&session) != ESP_OK) {
        return 0;
    }
    dir = opendir(dir_resolved);
    if (dir == NULL) {
        shell_sd_end(&session, "pkg");
        return 0;
    }
    while (count < max && (entry = readdir(dir)) != NULL) {
        char name[PKG_APP_NAME_BYTES];

        if (!pkg_app_name_from_appinfo(entry->d_name, name, sizeof(name))) {
            continue;
        }
        snprintf(names[count], PKG_APP_NAME_BYTES, "%s", name);
        count++;
    }
    closedir(dir);
    shell_sd_end(&session, "pkg");
    return count;
}

/** Public: installed app names (APPS APPINFO files) for completion. */
int pkg_list_installed(char names[][COMMAND_APP_NAME_BYTES], int max)
{
    return pkg_collect_apps(names, max);
}

/** Public: available bundle names (PKGS/<APP>/ dirs) for completion. */
int pkg_list_available(char names[][COMMAND_APP_NAME_BYTES], int max)
{
    char dir_rel[P4_CONFIG_SD_PATH_BYTES];
    char dir_resolved[P4_CONFIG_SD_PATH_BYTES];
    shell_sd_session_t session;
    DIR *dir;
    struct dirent *entry;
    int count = 0;

    snprintf(dir_rel, sizeof(dir_rel), "%s", P4_CONFIG_PKG_BUNDLE_DIR_NAME);
    if (shell_fs_resolve_path(dir_rel, dir_resolved, sizeof(dir_resolved)) != ESP_OK) {
        return 0;
    }
    if (shell_sd_begin(&session) != ESP_OK) {
        return 0;
    }
    dir = opendir(dir_resolved);
    if (dir == NULL) {
        shell_sd_end(&session, "pkg");
        return 0;
    }
    while (count < max && (entry = readdir(dir)) != NULL) {
        char full[P4_CONFIG_SD_PATH_BYTES + 258];
        struct stat st;
        size_t len;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        snprintf(full, sizeof(full), "%s/%s", dir_resolved, entry->d_name);
        if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) {
            continue;
        }
        len = strlen(entry->d_name);
        if (len == 0 || len >= COMMAND_APP_NAME_BYTES) {
            continue;
        }
        memcpy(names[count], entry->d_name, len);
        names[count][len] = '\0';
        count++;
    }
    closedir(dir);
    shell_sd_end(&session, "pkg");
    return count;
}

/** `pkg list`: every installed package with title/version + manifest entries. */
static void pkg_list(void)
{
    char names[PKG_MAX_ENTRIES][PKG_APP_NAME_BYTES];
    int count = pkg_collect_apps(names, PKG_MAX_ENTRIES);
    int i;

    if (count == 0) {
        shell_transcript_append_text("pkg: no packages installed (APPS/*.APPINFO)\n");
        batch_set_errorlevel(1);
        return;
    }
    for (i = 0; i < count; i++) {
        char title[64];
        char version[32];
        char rel[P4_CONFIG_SD_PATH_BYTES];
        char *text = NULL;
        size_t len = 0;
        int files = 0;

        pkg_meta(names[i], "title", title, sizeof(title));
        pkg_meta(names[i], "version", version, sizeof(version));
        pkg_manifest_rel(names[i], rel, sizeof(rel));
        if (pkg_read_file(rel, &text, &len)) {
            char *save = NULL;
            char *ln;

            for (ln = strtok_r(text, "\n", &save); ln != NULL; ln = strtok_r(NULL, "\n", &save)) {
                char p[P4_CONFIG_SD_PATH_BYTES];
                uint32_t crc = 0;

                if (asset_parse_line(ln, p, sizeof(p), &crc)) files++;
            }
            heap_caps_free(text);
        }
        shell_transcript_appendf("pkg: %-10s %-24s v%-10s %d file(s)\n",
                                 names[i], title[0] ? title : "(untitled)",
                                 version[0] ? version : "-", files);
    }
    batch_set_errorlevel(0);
}

/** `pkg info <app>`: metadata + per-file manifest status. */
static void pkg_info(const char *app)
{
    char title[64];
    char description[160];
    char version[32];
    char rel[P4_CONFIG_SD_PATH_BYTES];
    char *text = NULL;
    size_t len = 0;
    int present = 0;
    int missing = 0;
    char *save = NULL;
    char *ln;

    pkg_meta(app, "title", title, sizeof(title));
    pkg_meta(app, "description", description, sizeof(description));
    pkg_meta(app, "version", version, sizeof(version));
    shell_transcript_appendf("pkg: %s\n", app);
    shell_transcript_appendf("  title:       %s\n", title[0] ? title : "(none)");
    shell_transcript_appendf("  version:     %s\n", version[0] ? version : "(none)");
    if (description[0]) {
        shell_transcript_appendf("  description: %s\n", description);
    }

    pkg_manifest_rel(app, rel, sizeof(rel));
    if (!pkg_read_file(rel, &text, &len)) {
        shell_transcript_appendf("  manifest:    %s (missing)\n", rel);
        batch_set_errorlevel(1);
        return;
    }
    shell_transcript_appendf("  files (%s):\n", rel);
    for (ln = strtok_r(text, "\n", &save); ln != NULL; ln = strtok_r(NULL, "\n", &save)) {
        char path[P4_CONFIG_SD_PATH_BYTES];
        char entry[P4_CONFIG_SD_PATH_BYTES];
        uint32_t expect = 0;
        uint32_t actual = 0;

        if (!asset_parse_line(ln, path, sizeof(path), &expect)) {
            continue;
        }
        if (shell_fs_resolve_path(path, entry, sizeof(entry)) != ESP_OK ||
            !asset_crc_file(entry, &actual)) {
            shell_transcript_appendf("    MISSING  %s\n", path);
            missing++;
        } else if (actual != expect) {
            shell_transcript_appendf("    BAD      %s (want %08X got %08X)\n",
                                     path, (unsigned)expect, (unsigned)actual);
            missing++;
        } else {
            shell_transcript_appendf("    ok       %s %08X\n", path, (unsigned)actual);
            present++;
        }
    }
    heap_caps_free(text);
    shell_transcript_appendf("  %d ok, %d problem(s)\n", present, missing);
    batch_set_errorlevel(missing == 0 ? 0 : 1);
}

/** `pkg check`: verify every installed package. */
static void pkg_check(void)
{
    char names[PKG_MAX_ENTRIES][PKG_APP_NAME_BYTES];
    int count = pkg_collect_apps(names, PKG_MAX_ENTRIES);
    int i;
    int failed = 0;

    if (count == 0) {
        shell_transcript_append_text("pkg: no packages installed\n");
        batch_set_errorlevel(1);
        return;
    }
    for (i = 0; i < count; i++) {
        shell_transcript_appendf("pkg: %s\n", names[i]);
        if (asset_verify_app("  pkg", names[i], false) != 0) {
            failed++;
        }
    }
    shell_transcript_appendf("pkg: %d/%d package(s) ok\n", count - failed, count);
    batch_set_errorlevel(failed == 0 ? 0 : 1);
}

/** `pkg install <app>`: copy a PKGS/<app> bundle into place, CRC-verified. */
static void pkg_install(const char *app)
{
    char bmanifest_rel[P4_CONFIG_SD_PATH_BYTES];
    char *text = NULL;
    size_t len = 0;
    char (*paths)[P4_CONFIG_SD_PATH_BYTES] = NULL;
    uint32_t *crcs = NULL;
    int n = 0;
    int i;
    int failed = 0;
    char *save = NULL;
    char *ln;

    snprintf(bmanifest_rel, sizeof(bmanifest_rel), "%s/%s/%s.ASSETS",
             P4_CONFIG_PKG_BUNDLE_DIR_NAME, app, app);
    if (!pkg_read_file(bmanifest_rel, &text, &len)) {
        shell_transcript_appendf_ansi(SH_ERR "pkg: no bundle %s (push the app first)\n" SH_RST,
                                      bmanifest_rel);
        batch_set_errorlevel(1);
        return;
    }
    paths = heap_caps_malloc((size_t)PKG_MAX_ENTRIES * sizeof(*paths), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (paths == NULL) paths = malloc((size_t)PKG_MAX_ENTRIES * sizeof(*paths));
    crcs = malloc((size_t)PKG_MAX_ENTRIES * sizeof(*crcs));
    if (paths == NULL || crcs == NULL) {
        free(paths);
        free(crcs);
        heap_caps_free(text);
        shell_transcript_appendf_ansi(SH_ERR "pkg: out of memory\n" SH_RST);
        batch_set_errorlevel(1);
        return;
    }

    for (ln = strtok_r(text, "\n", &save); ln != NULL && n < PKG_MAX_ENTRIES; ln = strtok_r(NULL, "\n", &save)) {
        uint32_t crc = 0;
        if (asset_parse_line(ln, paths[n], sizeof(paths[n]), &crc)) {
            crcs[n] = crc;
            n++;
        }
    }
    if (n == 0) {
        shell_transcript_appendf_ansi(SH_ERR "pkg: bundle manifest %s is empty\n" SH_RST, bmanifest_rel);
        free(paths);
        free(crcs);
        heap_caps_free(text);
        batch_set_errorlevel(1);
        return;
    }

    /* Pass 1: verify every source payload before touching installed files. */
    for (i = 0; i < n; i++) {
        char src_rel[P4_CONFIG_SD_PATH_BYTES];
        char src[P4_CONFIG_SD_PATH_BYTES];
        uint32_t actual = 0;

        snprintf(src_rel, sizeof(src_rel), "%s/%s/%s", P4_CONFIG_PKG_BUNDLE_DIR_NAME, app, paths[i]);
        if (shell_fs_resolve_path(src_rel, src, sizeof(src)) != ESP_OK ||
            !asset_crc_file(src, &actual)) {
            shell_transcript_appendf_ansi(SH_ERR "pkg: bundle missing %s\n" SH_RST, src_rel);
            failed++;
        } else if (actual != crcs[i]) {
            shell_transcript_appendf_ansi(SH_ERR "pkg: bundle %s corrupt (want %08X got %08X)\n" SH_RST,
                                          src_rel, (unsigned)crcs[i], (unsigned)actual);
            failed++;
        }
    }
    if (failed != 0) {
        shell_transcript_appendf_ansi(SH_ERR "pkg: install aborted (%d bad file(s))\n" SH_RST, failed);
        free(paths);
        free(crcs);
        heap_caps_free(text);
        batch_set_errorlevel(1);
        return;
    }

    /* Pass 2: copy payloads, then the metadata + manifest. */
    for (i = 0; i < n; i++) {
        char src_rel[P4_CONFIG_SD_PATH_BYTES];
        char src[P4_CONFIG_SD_PATH_BYTES];
        char dst[P4_CONFIG_SD_PATH_BYTES];
        esp_err_t error;

        snprintf(src_rel, sizeof(src_rel), "%s/%s/%s", P4_CONFIG_PKG_BUNDLE_DIR_NAME, app, paths[i]);
        if (shell_fs_resolve_path(src_rel, src, sizeof(src)) != ESP_OK ||
            shell_fs_resolve_path(paths[i], dst, sizeof(dst)) != ESP_OK) {
            shell_transcript_appendf_ansi(SH_ERR "pkg: bad path %s\n" SH_RST, paths[i]);
            failed++;
            continue;
        }
        error = shell_fs_copy_file(src, dst);
        if (error != ESP_OK) {
            shell_transcript_appendf_ansi(SH_ERR "pkg: copy %s failed (%s)\n" SH_RST,
                                          paths[i], esp_err_to_name(error));
            failed++;
            continue;
        }
        shell_transcript_appendf("pkg: installed %s\n", paths[i]);
    }
    if (failed == 0) {
        /* Metadata + manifest from the bundle into APPS/. */
        char bundle_appinfo[P4_CONFIG_SD_PATH_BYTES];
        char bundle_manifest[P4_CONFIG_SD_PATH_BYTES];
        char dst_rel[P4_CONFIG_SD_PATH_BYTES];
        char src[P4_CONFIG_SD_PATH_BYTES];
        char dst[P4_CONFIG_SD_PATH_BYTES];

        snprintf(bundle_appinfo, sizeof(bundle_appinfo), "%s/%s/%s.APPINFO",
                 P4_CONFIG_PKG_BUNDLE_DIR_NAME, app, app);
        snprintf(bundle_manifest, sizeof(bundle_manifest), "%s/%s/%s.ASSETS",
                 P4_CONFIG_PKG_BUNDLE_DIR_NAME, app, app);

        if (shell_fs_resolve_path(bundle_appinfo, src, sizeof(src)) == ESP_OK) {
            pkg_appinfo_rel(app, dst_rel, sizeof(dst_rel));
            if (shell_fs_resolve_path(dst_rel, dst, sizeof(dst)) != ESP_OK ||
                shell_fs_copy_file(src, dst) != ESP_OK) {
                shell_transcript_appendf_ansi(SH_ERR "pkg: metadata copy failed\n" SH_RST);
                failed++;
            }
        }
        if (shell_fs_resolve_path(bundle_manifest, src, sizeof(src)) == ESP_OK) {
            pkg_manifest_rel(app, dst_rel, sizeof(dst_rel));
            if (shell_fs_resolve_path(dst_rel, dst, sizeof(dst)) != ESP_OK ||
                shell_fs_copy_file(src, dst) != ESP_OK) {
                shell_transcript_appendf_ansi(SH_ERR "pkg: manifest copy failed\n" SH_RST);
                failed++;
            }
        }
    }

    free(paths);
    free(crcs);
    heap_caps_free(text);
    if (failed == 0) {
        shell_transcript_appendf("pkg: %s installed\n", app);
        batch_set_errorlevel(0);
    } else {
        shell_transcript_appendf_ansi(SH_ERR "pkg: %s install had %d error(s)\n" SH_RST, app, failed);
        batch_set_errorlevel(1);
    }
}

/** `pkg remove <app>`: trash every installed file + metadata. */
static void pkg_remove(const char *app)
{
    char rel[P4_CONFIG_SD_PATH_BYTES];
    char *text = NULL;
    size_t len = 0;
    int removed = 0;
    char *save = NULL;
    char *ln;

    pkg_manifest_rel(app, rel, sizeof(rel));
    if (!pkg_read_file(rel, &text, &len)) {
        shell_transcript_appendf_ansi(SH_ERR "pkg: %s is not installed (no %s)\n" SH_RST, app, rel);
        batch_set_errorlevel(1);
        return;
    }
    for (ln = strtok_r(text, "\n", &save); ln != NULL; ln = strtok_r(NULL, "\n", &save)) {
        char path[P4_CONFIG_SD_PATH_BYTES];
        char resolved[P4_CONFIG_SD_PATH_BYTES];
        uint32_t crc = 0;

        if (!asset_parse_line(ln, path, sizeof(path), &crc)) {
            continue;
        }
        if (shell_fs_resolve_path(path, resolved, sizeof(resolved)) == ESP_OK &&
            storage_trash_delete_file(resolved, false) == ESP_OK) {
            shell_transcript_appendf("pkg: removed %s\n", path);
            removed++;
        }
    }
    heap_caps_free(text);

    /* Metadata + manifest themselves (the manifest may already list APPINFO;
     * removing twice is harmless). */
    {
        char appinfo[P4_CONFIG_SD_PATH_BYTES];
        char resolved[P4_CONFIG_SD_PATH_BYTES];

        pkg_appinfo_rel(app, appinfo, sizeof(appinfo));
        if (shell_fs_resolve_path(appinfo, resolved, sizeof(resolved)) == ESP_OK) {
            (void)storage_trash_delete_file(resolved, false);
        }
        pkg_manifest_rel(app, rel, sizeof(rel));
        if (shell_fs_resolve_path(rel, resolved, sizeof(resolved)) == ESP_OK) {
            (void)storage_trash_delete_file(resolved, false);
        }
    }
    shell_transcript_appendf("pkg: %s uninstalled (%d file(s))\n", app, removed);
    batch_set_errorlevel(0);
}

void shell_command_pkg(int argc, char **argv)
{
    if (argc < 2) {
        shell_print_usage("Usage: pkg list | info <app> | verify <app> | check | install <app> | remove <app>");
        batch_set_errorlevel(2);
        return;
    }

    if (shell_text_equals_ignore_case(argv[1], "list")) {
        pkg_list();
        return;
    }
    if (shell_text_equals_ignore_case(argv[1], "check")) {
        pkg_check();
        return;
    }
    if (argc == 3 && shell_text_equals_ignore_case(argv[1], "info")) {
        if (!asset_app_ok(argv[2])) {
            shell_transcript_appendf_ansi(SH_ERR "pkg: bad app name '%s'\n" SH_RST, argv[2]);
            batch_set_errorlevel(2);
            return;
        }
        pkg_info(argv[2]);
        return;
    }
    if (argc == 3 && shell_text_equals_ignore_case(argv[1], "verify")) {
        if (!asset_app_ok(argv[2])) {
            shell_transcript_appendf_ansi(SH_ERR "pkg: bad app name '%s'\n" SH_RST, argv[2]);
            batch_set_errorlevel(2);
            return;
        }
        (void)asset_verify_app("pkg", argv[2], false);
        return;
    }
    if (argc == 3 && shell_text_equals_ignore_case(argv[1], "install")) {
        if (!asset_app_ok(argv[2])) {
            shell_transcript_appendf_ansi(SH_ERR "pkg: bad app name '%s'\n" SH_RST, argv[2]);
            batch_set_errorlevel(2);
            return;
        }
        pkg_install(argv[2]);
        return;
    }
    if (argc == 3 && shell_text_equals_ignore_case(argv[1], "remove")) {
        if (!asset_app_ok(argv[2])) {
            shell_transcript_appendf_ansi(SH_ERR "pkg: bad app name '%s'\n" SH_RST, argv[2]);
            batch_set_errorlevel(2);
            return;
        }
        pkg_remove(argv[2]);
        return;
    }

    shell_print_usage("Usage: pkg list | info <app> | verify <app> | check | install <app> | remove <app>");
    batch_set_errorlevel(2);
}
