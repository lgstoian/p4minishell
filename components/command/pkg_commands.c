/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file pkg_commands.c
 * @brief `pkg` — SD application packages.
 *
 * A package is metadata plus a contents manifest:
 *   APPS/<APP>.APPINFO   INI: title=, description=, version=, plus type=
 *                        (batch when absent, hybrid, or native) and for
 *                        hybrid/native apps abi=/arch=/entry= (see ABI.md)
 *   APPS/<APP>.ASSETS    `path=HEXCRC` lines + optional `SIGN=` line
 * The install source is a bundle directory:
 *   PKGS/<APP>/<APP>.ASSETS   the same manifest
 *   PKGS/<APP>/<path>         each payload file at its install-relative path
 *   PKGS/<APP>/<APP>.APPINFO  the metadata file
 *
 * Verbs: list, info, verify, check, install [/signed], remove, key. Verification
 * reuses the single CRC/manifest implementation (`asset_parse_line`,
 * `asset_crc_file`, `asset_verify_app`); copies reuse `shell_fs_copy_file`;
 * removal reuses `storage_trash_delete_file`. Nothing here re-implements those.
 *
 * This file also owns the ONE manifest trust core (`pkg_sign_*`: canonical
 * bytes, ECDSA P-256 + SHA-256 verify, the `p4sign` NVS key store) and the
 * `pkg key` verb; the contract is frozen in ABI.md. The pure helpers are
 * unit-tested (test/main/test_pkg.c).
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
#include "storage_commands.h"
#include "ansi_palette.h"
#include "p4minishell_config.h"
#include "esp_heap_caps.h"
#include "p4heap.h"
#include "nvs_flash.h"
#include "mbedtls/sha256.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/ecp.h"

#if !defined(MBEDTLS_ECDSA_C) || !defined(MBEDTLS_ECP_C) || !defined(MBEDTLS_SHA256_C)
#error "pkg signatures need MBEDTLS_ECDSA_C + MBEDTLS_ECP_C + MBEDTLS_SHA256_C"
#endif

#define PKG_APP_NAME_BYTES 32
#define PKG_MAX_ENTRIES    P4_CONFIG_PKG_MAX_ENTRIES

/** Manifest trust: raw P-256 X||Y (64), raw r||s (64), SHA-256 (32). */
#define PKG_SIGN_PUB_BYTES  64
#define PKG_SIGN_SIG_BYTES  64
#define PKG_SIGN_HASH_BYTES 32
#define PKG_SIGN_HEXSIG_LEN (PKG_SIGN_SIG_BYTES * 2)
#define PKG_SIGN_NVS_NS     "p4sign"
#define PKG_SIGN_NVS_KEY    "pub"

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
    text = p4heap_alloc_psram(P4_CONFIG_PKG_MANIFEST_BYTES + 1);
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

/** Read one APPINFO key (`title`/`description`/`version`/`type`/`abi`). Empty on miss. */
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
        {
            /* Native bundles are stored only; hybrid bundles run a `.BAT`
             * shim into a linked entry (see ABI.md). Flag both. */
            char ptype[16];
            const char *tag = "";

            ptype[0] = '\0';
            pkg_meta(names[i], "type", ptype, sizeof(ptype));
            if (shell_text_equals_ignore_case(ptype, "native")) {
                tag = " [native]";
            } else if (shell_text_equals_ignore_case(ptype, "hybrid")) {
                tag = " [hybrid]";
            }
            shell_transcript_appendf("pkg: %-10s %-24s v%-10s %d file(s)%s\n",
                                     names[i], title[0] ? title : "(untitled)",
                                     version[0] ? version : "-", files, tag);
        }
    }
    batch_set_errorlevel(0);
}

/** `pkg info <app>`: metadata + per-file manifest status. */
static void pkg_info(const char *app)
{
    char title[64];
    char description[160];
    char version[32];
    char ptype[16];
    char abi[32];
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
    ptype[0] = '\0';
    abi[0] = '\0';
    pkg_meta(app, "type", ptype, sizeof(ptype));
    pkg_meta(app, "abi", abi, sizeof(abi));
    shell_transcript_appendf("pkg: %s\n", app);
    shell_transcript_appendf("  title:       %s\n", title[0] ? title : "(none)");
    shell_transcript_appendf("  version:     %s\n", version[0] ? version : "(none)");
    shell_transcript_appendf("  type:        %s\n", ptype[0] ? ptype : "batch");
    if (abi[0]) {
        shell_transcript_appendf("  abi:         %s\n", abi);
    }
    if (description[0]) {
        shell_transcript_appendf("  description: %s\n", description);
    }
    {
        /* Trust line over the installed manifest (no file reads beyond it). */
        char *manifest = NULL;
        size_t manifest_len = 0;
        char manifest_rel[P4_CONFIG_SD_PATH_BYTES];

        pkg_manifest_rel(app, manifest_rel, sizeof(manifest_rel));
        if (pkg_read_file(manifest_rel, &manifest, &manifest_len)) {
            char fingerprint[PKG_SIGN_HASH_BYTES * 2 + 1];
            pkg_sign_status_t sign;

            fingerprint[0] = '\0';
            sign = pkg_sign_check_manifest(manifest, fingerprint);
            if (sign == PKG_SIGN_OK) {
                shell_transcript_appendf("  signed:      yes (%.16s...)\n", fingerprint);
            } else if (sign == PKG_SIGN_NONE) {
                shell_transcript_appendf("  signed:      no\n");
            } else if (sign == PKG_SIGN_NOKEY) {
                shell_transcript_appendf("  signed:      yes, UNVERIFIED (no trusted key)\n");
            } else {
                shell_transcript_appendf("  signed:      BAD (manifest untrusted)\n");
            }
            heap_caps_free(manifest);
        }
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

/** `pkg install <app> [/signed]`: copy a PKGS/<app> bundle into place,
 *  CRC-verified, with the manifest signature enforced when present (or when
 *  `/signed` / `P4_CONFIG_PKG_REQUIRE_SIGN` demands it). */
static void pkg_install(const char *app, bool require_sign)
{
    char bmanifest_rel[P4_CONFIG_SD_PATH_BYTES];
    char bappinfo_rel[P4_CONFIG_SD_PATH_BYTES];
    char *text = NULL;
    size_t len = 0;
    char (*paths)[P4_CONFIG_SD_PATH_BYTES] = NULL;
    uint32_t *crcs = NULL;
    int n = 0;
    int i;
    int failed = 0;
    char *save = NULL;
    char *ln;
    bool native = false;
    bool hybrid = false;
    char sign_fp[PKG_SIGN_HASH_BYTES * 2 + 1];
    pkg_sign_status_t sign = PKG_SIGN_NONE;

    snprintf(bmanifest_rel, sizeof(bmanifest_rel), "%s/%s/%s.ASSETS",
             P4_CONFIG_PKG_BUNDLE_DIR_NAME, app, app);
    if (!pkg_read_file(bmanifest_rel, &text, &len)) {
        shell_transcript_appendf_ansi(SH_ERR "pkg: no bundle %s (push the app first)\n" SH_RST,
                                      bmanifest_rel);
        batch_set_errorlevel(1);
        return;
    }
    /* Trust verdict while newlines still separate the lines (the entry loop
     * below tokenizes @p text in place). */
    sign_fp[0] = '\0';
    sign = pkg_sign_check_manifest(text, sign_fp);
    paths = p4heap_alloc_psram((size_t)PKG_MAX_ENTRIES * sizeof(*paths));
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

    /* Validate the bundle type before touching installed files. Unknown
     * types fail loudly; native bundles install store-only, hybrid bundles
     * install a `.BAT` shim that calls a linked-in entry (see ABI.md). */
    snprintf(bappinfo_rel, sizeof(bappinfo_rel), "%s/%s/%s.APPINFO",
             P4_CONFIG_PKG_BUNDLE_DIR_NAME, app, app);
    {
        char btype[16];
        char bentry[64];

        btype[0] = '\0';
        bentry[0] = '\0';
        (void)storage_ini_file_get(bappinfo_rel, "type", btype, sizeof(btype));
        if (btype[0] != '\0' &&
            !shell_text_equals_ignore_case(btype, "batch") &&
            !shell_text_equals_ignore_case(btype, "native") &&
            !shell_text_equals_ignore_case(btype, "hybrid")) {
            shell_transcript_appendf_ansi(SH_ERR "pkg: bundle %s has unknown type=%s (want batch|native|hybrid)\n" SH_RST,
                                          app, btype);
            free(paths);
            free(crcs);
            heap_caps_free(text);
            batch_set_errorlevel(1);
            return;
        }
        native = shell_text_equals_ignore_case(btype, "native");
        hybrid = shell_text_equals_ignore_case(btype, "hybrid");
        if (hybrid) {
            (void)storage_ini_file_get(bappinfo_rel, "entry", bentry, sizeof(bentry));
            if (bentry[0] == '\0') {
                shell_transcript_appendf_ansi(SH_ERR "pkg: hybrid bundle %s needs entry=<linked-name>\n" SH_RST,
                                              app);
                free(paths);
                free(crcs);
                heap_caps_free(text);
                batch_set_errorlevel(1);
                return;
            }
        }
    }

    /* Trust gate: a BAD signature always aborts before anything is copied. An
     * unsigned bundle, or a signed one with no trusted key, aborts only when
     * `/signed` or the policy demands it. */
    if (sign == PKG_SIGN_BAD) {
        shell_transcript_appendf_ansi(SH_ERR "pkg: bundle %s has a BAD signature (refusing)\n" SH_RST, app);
        free(paths);
        free(crcs);
        heap_caps_free(text);
        batch_set_errorlevel(1);
        return;
    }
    if (sign == PKG_SIGN_OK) {
        shell_transcript_appendf("pkg: bundle %s signed OK %.8s\n", app, sign_fp);
    } else if (sign == PKG_SIGN_NOKEY) {
        if (require_sign) {
            shell_transcript_appendf_ansi(SH_ERR "pkg: bundle %s is signed but no trusted key is installed\n" SH_RST, app);
            free(paths);
            free(crcs);
            heap_caps_free(text);
            batch_set_errorlevel(1);
            return;
        }
        shell_transcript_appendf_ansi(SH_WARN "pkg: warning: bundle %s is signed but no trusted key is installed (CRC-only)\n" SH_RST, app);
    } else if (sign == PKG_SIGN_ERROR) {
        shell_transcript_appendf_ansi(SH_ERR "pkg: bundle %s signature unreadable (refusing)\n" SH_RST, app);
        free(paths);
        free(crcs);
        heap_caps_free(text);
        batch_set_errorlevel(1);
        return;
    } else if (require_sign) {
        shell_transcript_appendf_ansi(SH_ERR "pkg: bundle %s is unsigned (policy refuses)\n" SH_RST, app);
        free(paths);
        free(crcs);
        heap_caps_free(text);
        batch_set_errorlevel(1);
        return;
    } else {
        shell_transcript_appendf("pkg: bundle %s unsigned (CRC-only)\n", app);
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
        char bundle_manifest[P4_CONFIG_SD_PATH_BYTES];
        char dst_rel[P4_CONFIG_SD_PATH_BYTES];
        char src[P4_CONFIG_SD_PATH_BYTES];
        char dst[P4_CONFIG_SD_PATH_BYTES];

        snprintf(bundle_manifest, sizeof(bundle_manifest), "%s/%s/%s.ASSETS",
                 P4_CONFIG_PKG_BUNDLE_DIR_NAME, app, app);
        if (shell_fs_resolve_path(bappinfo_rel, src, sizeof(src)) == ESP_OK) {
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
        if (native || hybrid) {
            /* The blob runs only through linked-in entries (hybrid) or not
             * at all (native store-only): abi=/arch= gate what the firmware
             * will execute (see ABI.md). */
            char babi[32];
            char bentry[64];

            babi[0] = '\0';
            bentry[0] = '\0';
            (void)storage_ini_file_get(bappinfo_rel, "abi", babi, sizeof(babi));
            if (babi[0] != '\0' && strcmp(babi, P4_CONFIG_NATIVE_ABI) != 0) {
                shell_transcript_appendf_ansi(SH_WARN "pkg: warning: %s abi=%s, firmware expects %s\n" SH_RST,
                                              app, babi, P4_CONFIG_NATIVE_ABI);
            }
            if (hybrid) {
                (void)storage_ini_file_get(bappinfo_rel, "entry", bentry, sizeof(bentry));
                shell_transcript_appendf("pkg: %s is hybrid (launch %s runs %s.BAT into %s)\n",
                                         app, app, app,
                                         bentry[0] ? bentry : "(no entry)");
            } else {
                shell_transcript_appendf_ansi(SH_MUTE "pkg: %s is a native package (stored only; runs only if linked into the firmware)\n" SH_RST,
                                              app);
            }
        }
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

/* ========================================================================
 * Manifest signatures (ECDSA P-256 + SHA-256, the `sign_*` family)
 * ========================================================================
 * A signed `.ASSETS` manifest carries one `SIGN=<128 hex>` line (raw r||s,
 * made with `tools/pkg_sign.py`). Verification hashes the canonical bytes —
 * every line except blanks, `#`/`;` comments, and `SIGN` lines, each with
 * one trailing `\n` — and checks the signature against the trusted P-256
 * public key (raw X||Y) in the `p4sign` NVS namespace. The private key never
 * touches the device. All helpers below are pure except the NVS key store,
 * and the pure ones are unit-tested (see test/main/test_pkg.c).
 */

/** True when @p line is a manifest signature line (skipped by verifiers). */
bool pkg_is_sign_line(const char *line)
{
    if (line == NULL) {
        return false;
    }
    while (*line == ' ' || *line == '\t' || *line == '\r' || *line == '\n') {
        line++;
    }
    return strncasecmp(line, "SIGN=", 5) == 0;
}

/** Parse a `SIGN=<128 hex>` line into @p sig_out (64 raw bytes). */
bool pkg_sign_parse(const char *line, uint8_t *sig_out)
{
    const char *hex;
    int digits = 0;

    if (sig_out != NULL) {
        memset(sig_out, 0, PKG_SIGN_SIG_BYTES);
    }
    if (line == NULL || sig_out == NULL) {
        return false;
    }
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    if (strncasecmp(line, "SIGN=", 5) != 0) {
        return false;
    }
    hex = line + 5;
    while (*hex == ' ' || *hex == '\t') {
        hex++;
    }
    for (; *hex != '\0' && *hex != '\r' && *hex != '\n'; hex++) {
        unsigned v;

        if (*hex == ' ' || *hex == '\t') {
            break;
        }
        if (!isxdigit((unsigned char)*hex) || digits >= PKG_SIGN_HEXSIG_LEN) {
            memset(sig_out, 0, PKG_SIGN_SIG_BYTES);
            return false;
        }
        v = (unsigned)(isdigit((unsigned char)*hex) ? *hex - '0'
                                                    : tolower((unsigned char)*hex) - 'a' + 10);
        if ((digits % 2) == 0) {
            sig_out[digits / 2] = (uint8_t)(v << 4);
        } else {
            sig_out[digits / 2] |= (uint8_t)v;
        }
        digits++;
    }
    if (digits != PKG_SIGN_HEXSIG_LEN) {
        memset(sig_out, 0, PKG_SIGN_SIG_BYTES);
        return false;
    }
    while (*hex == ' ' || *hex == '\t') {
        hex++;
    }
    if (*hex != '\0' && *hex != '\r' && *hex != '\n') {
        memset(sig_out, 0, PKG_SIGN_SIG_BYTES);
        return false;
    }
    return true;
}

/** Rebuild the canonical signed bytes (see above). @return 0 ok, 1 overflow. */
int pkg_sign_canonical(const char *text, char *out, size_t out_size, size_t *len_out)
{
    const char *cursor;
    size_t used = 0;

    if (len_out != NULL) {
        *len_out = 0;
    }
    if (out == NULL || out_size == 0) {
        return 1;
    }
    out[0] = '\0';
    if (text == NULL) {
        return 0;
    }
    cursor = text;
    while (*cursor != '\0') {
        const char *nl = strchr(cursor, '\n');
        const char *end = (nl != NULL) ? nl : cursor + strlen(cursor);
        const char *trim = cursor;
        const char *content_end;
        size_t len = (size_t)(end - cursor);

        /* One trailing CR (CRLF files) is not signed. */
        if (len > 0 && cursor[len - 1] == '\r') {
            len--;
        }
        content_end = cursor + len;
        while (trim < content_end && (*trim == ' ' || *trim == '\t')) {
            trim++;
        }
        if (trim < content_end && *trim != '#' && *trim != ';' && !pkg_is_sign_line(trim)) {
            if (used + len + 1 >= out_size) {
                return 1;
            }
            memcpy(out + used, cursor, len);
            used += len;
            out[used++] = '\n';
        }
        cursor = (nl != NULL) ? nl + 1 : end;
    }
    out[used] = '\0';
    if (len_out != NULL) {
        *len_out = used;
    }
    return 0;
}

/** SHA-256 @p msg into @p hash_out (32 bytes). @return 0 ok. */
int pkg_sign_hash(const uint8_t *msg, size_t msg_len, uint8_t *hash_out)
{
    if (msg == NULL || hash_out == NULL) {
        return 1;
    }
    /* msg_len 0 with a NULL msg is rejected above; an empty manifest hashes
     * as the empty string (mbedtls accepts len 0 with any pointer). */
    return mbedtls_sha256(msg, msg_len, hash_out, 0) == 0 ? 0 : 1;
}

/** Verify raw r||s @p sig over SHA-256(@p msg) with raw X||Y @p pub.
 *  @return 0 valid, 1 anything else (bad args, off-curve key, bad math). */
int pkg_sign_verify(const uint8_t *msg, size_t msg_len,
                    const uint8_t *sig, const uint8_t *pub)
{
    mbedtls_ecp_group grp;
    mbedtls_ecp_point q;
    mbedtls_mpi r;
    mbedtls_mpi s;
    uint8_t hash[PKG_SIGN_HASH_BYTES];
    uint8_t point[1 + PKG_SIGN_PUB_BYTES];
    int rc = 1;

    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&q);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);
    do {
        if (msg == NULL || sig == NULL || pub == NULL) {
            break;
        }
        if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) != 0) {
            break;
        }
        /* Raw X||Y storage gains its uncompressed marker here (the NVS blob
         * stays 64 bytes; only the verify path widens it). */
        point[0] = 0x04;
        memcpy(point + 1, pub, PKG_SIGN_PUB_BYTES);
        if (mbedtls_ecp_point_read_binary(&grp, &q, point, sizeof(point)) != 0) {
            break;
        }
        if (mbedtls_ecp_check_pubkey(&grp, &q) != 0) {
            break;
        }
        if (mbedtls_mpi_read_binary(&r, sig, 32) != 0 ||
            mbedtls_mpi_read_binary(&s, sig + 32, 32) != 0) {
            break;
        }
        if (pkg_sign_hash(msg, msg_len, hash) != 0) {
            break;
        }
        if (mbedtls_ecdsa_verify(&grp, hash, sizeof(hash), &q, &r, &s) != 0) {
            break;
        }
        rc = 0;
    } while (0);
    memset(point, 0, sizeof(point));
    memset(hash, 0, sizeof(hash));
    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    mbedtls_ecp_point_free(&q);
    mbedtls_ecp_group_free(&grp);
    return rc;
}

/** Hex fingerprint (SHA-256, 64 chars + NUL) of a raw public key. */
void pkg_sign_fingerprint(const uint8_t *pub, char *hex_out)
{
    static const char digits[] = "0123456789abcdef";
    uint8_t hash[PKG_SIGN_HASH_BYTES];

    if (hex_out != NULL) {
        hex_out[0] = '\0';
    }
    if (pub == NULL || hex_out == NULL) {
        return;
    }
    if (pkg_sign_hash(pub, PKG_SIGN_PUB_BYTES, hash) != 0) {
        return;
    }
    for (int i = 0; i < PKG_SIGN_HASH_BYTES; i++) {
        hex_out[i * 2] = digits[(hash[i] >> 4) & 0xF];
        hex_out[i * 2 + 1] = digits[hash[i] & 0xF];
    }
    hex_out[PKG_SIGN_HASH_BYTES * 2] = '\0';
    memset(hash, 0, sizeof(hash));
}

/** Ensure the NVS backend answers (the worker task owns an internal stack,
 *  so touching flash here is legal; the lwIP thread may never). */
static bool pkg_sign_nvs_ready(void)
{
    esp_err_t error = nvs_flash_init();

    if (error == ESP_ERR_NVS_NO_FREE_PAGES || error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        error = nvs_flash_erase();
        if (error == ESP_OK) {
            error = nvs_flash_init();
        }
    }
    return error == ESP_OK;
}

/** Load the trusted raw key. @return 0 ok, 1 none stored, 2 NVS/corrupt. */
int pkg_sign_pubkey_load(uint8_t *pub_out)
{
    nvs_handle_t handle;
    size_t len = PKG_SIGN_PUB_BYTES;

    if (pub_out != NULL) {
        memset(pub_out, 0, PKG_SIGN_PUB_BYTES);
    }
    if (pub_out == NULL || !pkg_sign_nvs_ready()) {
        return 2;
    }
    if (nvs_open(PKG_SIGN_NVS_NS, NVS_READONLY, &handle) != ESP_OK) {
        return 1;
    }
    if (nvs_get_blob(handle, PKG_SIGN_NVS_KEY, pub_out, &len) != ESP_OK ||
        len != PKG_SIGN_PUB_BYTES) {
        nvs_close(handle);
        if (len != PKG_SIGN_PUB_BYTES) {
            memset(pub_out, 0, PKG_SIGN_PUB_BYTES);
        }
        return (len != PKG_SIGN_PUB_BYTES && len != 0) ? 2 : 1;
    }
    nvs_close(handle);
    return 0;
}

/** Store the trusted raw key (overwrites). @return 0 ok, 1 failure. */
int pkg_sign_pubkey_save(const uint8_t *pub)
{
    nvs_handle_t handle;

    if (pub == NULL || !pkg_sign_nvs_ready()) {
        return 1;
    }
    if (nvs_open(PKG_SIGN_NVS_NS, NVS_READWRITE, &handle) != ESP_OK) {
        return 1;
    }
    if (nvs_set_blob(handle, PKG_SIGN_NVS_KEY, pub, PKG_SIGN_PUB_BYTES) != ESP_OK ||
        nvs_commit(handle) != ESP_OK) {
        nvs_close(handle);
        return 1;
    }
    nvs_close(handle);
    return 0;
}

/** Forget the trusted key. @return 0 ok (or already absent), 1 failure. */
int pkg_sign_pubkey_clear(void)
{
    nvs_handle_t handle;
    esp_err_t error;

    if (!pkg_sign_nvs_ready()) {
        return 1;
    }
    if (nvs_open(PKG_SIGN_NVS_NS, NVS_READWRITE, &handle) != ESP_OK) {
        return 1;
    }
    error = nvs_erase_key(handle, PKG_SIGN_NVS_KEY);
    if (error != ESP_OK && error != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return 1;
    }
    error = nvs_commit(handle);
    nvs_close(handle);
    return error == ESP_OK ? 0 : 1;
}

/**
 * Collect the manifest's (single) SIGN line: 1 with @p sig_out filled,
 * 0 when unsigned, -1 when malformed or doubled. Pure, shared by both
 * verdict paths so "is it signed?" never depends on key state.
 */
static int pkg_sign_find(const char *text, uint8_t *sig_out)
{
    const char *cursor;
    bool have_sign = false;

    if (text == NULL || sig_out == NULL) {
        return -1;
    }
    memset(sig_out, 0, PKG_SIGN_SIG_BYTES);
    cursor = text;
    while (*cursor != '\0') {
        const char *nl = strchr(cursor, '\n');
        size_t len = (nl != NULL) ? (size_t)(nl - cursor) : strlen(cursor);
        char line[P4_CONFIG_PKG_LINE_BYTES];

        if (len >= sizeof(line)) {
            memset(sig_out, 0, PKG_SIGN_SIG_BYTES);
            return -1;
        }
        memcpy(line, cursor, len);
        line[len] = '\0';
        if (pkg_is_sign_line(line)) {
            if (have_sign) {
                memset(sig_out, 0, PKG_SIGN_SIG_BYTES);
                return -1;
            }
            if (!pkg_sign_parse(line, sig_out)) {
                return -1;
            }
            have_sign = true;
        }
        cursor = (nl != NULL) ? nl + 1 : cursor + len;
    }
    return have_sign ? 1 : 0;
}

/** Manifest trust verdict over in-memory manifest @p text against an explicit
 *  raw key (pure except the heap scratch, unit-tested). */
pkg_sign_status_t pkg_sign_check_with_key(const char *text, const uint8_t *pub,
                                          char *fingerprint_out)
{
    char *canon = NULL;
    size_t canon_len = 0;
    uint8_t sig[PKG_SIGN_SIG_BYTES];
    int found;

    if (fingerprint_out != NULL) {
        fingerprint_out[0] = '\0';
    }
    if (text == NULL || pub == NULL) {
        return PKG_SIGN_ERROR;
    }
    found = pkg_sign_find(text, sig);
    if (found < 0) {
        return PKG_SIGN_BAD;
    }
    if (found == 0) {
        return PKG_SIGN_NONE;
    }
    canon = p4heap_alloc_psram(P4_CONFIG_PKG_MANIFEST_BYTES + 1);
    if (canon == NULL) {
        memset(sig, 0, sizeof(sig));
        return PKG_SIGN_ERROR;
    }
    if (pkg_sign_canonical(text, canon, P4_CONFIG_PKG_MANIFEST_BYTES + 1, &canon_len) != 0) {
        heap_caps_free(canon);
        memset(sig, 0, sizeof(sig));
        return PKG_SIGN_ERROR;
    }
    {
        int rc = pkg_sign_verify((const uint8_t *)canon, canon_len, sig, pub);
        pkg_sign_fingerprint(pub, fingerprint_out);
        heap_caps_free(canon);
        memset(sig, 0, sizeof(sig));
        return rc == 0 ? PKG_SIGN_OK : PKG_SIGN_BAD;
    }
}

/** Manifest trust verdict over in-memory manifest @p text against the NVS
 *  trust store. Unsigned text reports NONE without touching NVS. */
pkg_sign_status_t pkg_sign_check_manifest(const char *text, char *fingerprint_out)
{
    uint8_t pub[PKG_SIGN_PUB_BYTES];
    uint8_t probe[PKG_SIGN_SIG_BYTES];
    int found;
    int key;
    pkg_sign_status_t verdict;

    if (fingerprint_out != NULL) {
        fingerprint_out[0] = '\0';
    }
    if (text == NULL) {
        return PKG_SIGN_ERROR;
    }
    found = pkg_sign_find(text, probe);
    memset(probe, 0, sizeof(probe));
    if (found < 0) {
        return PKG_SIGN_BAD;
    }
    if (found == 0) {
        return PKG_SIGN_NONE;
    }
    key = pkg_sign_pubkey_load(pub);
    if (key != 0) {
        memset(pub, 0, sizeof(pub));
        return (key == 1) ? PKG_SIGN_NOKEY : PKG_SIGN_ERROR;
    }
    verdict = pkg_sign_check_with_key(text, pub, fingerprint_out);
    memset(pub, 0, sizeof(pub));
    return verdict;
}

/** `pkg key show|install <file>|clear`: the ECDSA trust store. */
static void pkg_key_command(int argc, char **argv)
{
    if (argc == 3 && shell_text_equals_ignore_case(argv[2], "show")) {
        uint8_t pub[PKG_SIGN_PUB_BYTES];
        char fingerprint[PKG_SIGN_HASH_BYTES * 2 + 1];
        int rc = pkg_sign_pubkey_load(pub);

        if (rc != 0) {
            shell_transcript_append_text("pkg: no trusted key installed\n");
            batch_set_errorlevel(1);
            return;
        }
        pkg_sign_fingerprint(pub, fingerprint);
        shell_transcript_appendf("pkg: trusted key %.16s...\n", fingerprint);
        memset(pub, 0, sizeof(pub));
        batch_set_errorlevel(0);
        return;
    }
    if (argc == 4 && shell_text_equals_ignore_case(argv[2], "install")) {
        char resolved[P4_CONFIG_SD_PATH_BYTES];
        shell_sd_session_t session;
        FILE *file = NULL;
        uint8_t pub[PKG_SIGN_PUB_BYTES];
        size_t got = 0;

        if (shell_fs_resolve_path(argv[3], resolved, sizeof(resolved)) != ESP_OK) {
            shell_print_error("pkg: invalid path %s", argv[3]);
            batch_set_errorlevel(2);
            return;
        }
        if (shell_sd_begin(&session) != ESP_OK) {
            shell_print_error("pkg: SD card not present");
            batch_set_errorlevel(1);
            return;
        }
        file = fopen(resolved, "rb");
        if (file == NULL) {
            shell_sd_end(&session, "pkg key");
            shell_print_error("pkg: cannot open %s", argv[3]);
            batch_set_errorlevel(1);
            return;
        }
        got = fread(pub, 1, sizeof(pub), file);
        {
            int tail = fgetc(file);
            fclose(file);
            shell_sd_end(&session, "pkg key");
            if (got != sizeof(pub) || tail != EOF) {
                shell_print_error("pkg: key file must hold exactly %d raw bytes (X||Y)",
                                  PKG_SIGN_PUB_BYTES);
                memset(pub, 0, sizeof(pub));
                batch_set_errorlevel(1);
                return;
            }
        }
        /* Reject off-curve garbage before it becomes trust. */
        {
            mbedtls_ecp_group grp;
            mbedtls_ecp_point q;
            uint8_t point[1 + PKG_SIGN_PUB_BYTES];
            bool ok = false;

            mbedtls_ecp_group_init(&grp);
            mbedtls_ecp_point_init(&q);
            do {
                if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) != 0) {
                    break;
                }
                point[0] = 0x04;
                memcpy(point + 1, pub, PKG_SIGN_PUB_BYTES);
                if (mbedtls_ecp_point_read_binary(&grp, &q, point, sizeof(point)) != 0) {
                    break;
                }
                ok = (mbedtls_ecp_check_pubkey(&grp, &q) == 0);
            } while (0);
            memset(point, 0, sizeof(point));
            mbedtls_ecp_point_free(&q);
            mbedtls_ecp_group_free(&grp);
            if (!ok) {
                shell_print_error("pkg: key is not a valid P-256 point");
                memset(pub, 0, sizeof(pub));
                batch_set_errorlevel(1);
                return;
            }
        }
        if (pkg_sign_pubkey_save(pub) != 0) {
            shell_print_error("pkg: cannot store the key");
            memset(pub, 0, sizeof(pub));
            batch_set_errorlevel(1);
            return;
        }
        {
            char fingerprint[PKG_SIGN_HASH_BYTES * 2 + 1];
            pkg_sign_fingerprint(pub, fingerprint);
            shell_transcript_appendf("pkg: trusted key %.16s...\n", fingerprint);
        }
        memset(pub, 0, sizeof(pub));
        batch_set_errorlevel(0);
        return;
    }
    if (argc == 3 && shell_text_equals_ignore_case(argv[2], "clear")) {
        if (!shell_confirm_destructive("pkg key clear",
                                       "The firmware will stop verifying signed packages.",
                                       "Install a key again with pkg key install <file>.")) {
            batch_set_errorlevel(1);
            return;
        }
        if (pkg_sign_pubkey_clear() != 0) {
            shell_print_error("pkg: cannot clear the key");
            batch_set_errorlevel(1);
            return;
        }
        shell_transcript_append_text("pkg: trusted key cleared\n");
        batch_set_errorlevel(0);
        return;
    }
    shell_print_usage("Usage: pkg key show | pkg key install <file> | pkg key clear");
    batch_set_errorlevel(2);
}

void shell_command_pkg(int argc, char **argv)
{
    if (argc < 2) {
        shell_print_usage("Usage: pkg list | info <app> | verify <app> | check | install <app> [/signed] | remove <app> | key show|install|clear");
        batch_set_errorlevel(2);
        return;
    }

    if (argc >= 3 && shell_text_equals_ignore_case(argv[1], "key")) {
        pkg_key_command(argc, argv);
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
    if ((argc == 3 || argc == 4) && shell_text_equals_ignore_case(argv[1], "install")) {
        bool require_sign = (P4_CONFIG_PKG_REQUIRE_SIGN != 0);

        if (!asset_app_ok(argv[2])) {
            shell_transcript_appendf_ansi(SH_ERR "pkg: bad app name '%s'\n" SH_RST, argv[2]);
            batch_set_errorlevel(2);
            return;
        }
        if (argc == 4) {
            if (!shell_text_equals_ignore_case(argv[3], "/signed")) {
                shell_print_usage("Usage: pkg list | info <app> | verify <app> | check | install <app> [/signed] | remove <app> | key show|install|clear");
                batch_set_errorlevel(2);
                return;
            }
            require_sign = true;
        }
        pkg_install(argv[2], require_sign);
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

    shell_print_usage("Usage: pkg list | info <app> | verify <app> | check | install <app> [/signed] | remove <app> | key show|install|clear");
    batch_set_errorlevel(2);
}
