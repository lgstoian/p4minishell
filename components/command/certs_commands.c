/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file certs_commands.c
 * @brief `certs` verb: TLS trust store management.
 *
 *   certs [info]              Show trust store status (PEM/DER counts, buffer)
 *   certs list                List certificate files in sd:/CERTS
 *   certs add <file>          Import a PEM or DER CA certificate into sd:/CERTS
 *   certs remove <name>       Remove a certificate file from sd:/CERTS
 *   certs rebuild             Rebuild the combined PEM bundle (BUNDLE.PEM)
 *   certs clear               Clear the loaded trust store (reboot reloads)
 *   certs reload              Re-scan sd:/CERTS and re-load the global CA store
 *
 * The certificate store lives in sd:/CERTS/.  PEM and DER files are accepted.
 * On first boot the directory is created automatically.  `certs rebuild`
 * concatenates every .pem file into sd:/CERTS/BUNDLE.PEM for quick loading.
 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shell.h"
#include "batch.h"
#include "storage.h"
#include "command.h"
#include "certs.h"
#include "ansi_palette.h"
#include "p4minishell_config.h"
#include "bsp/esp-bsp.h"
#include "esp_err.h"
#include "esp_log.h"

#define CERTS_CMD_TAG "certs_cmd"

/* ---- helpers ---- */

static void certs_print_usage(void)
{
    shell_print_usage("Usage: certs [info] | list | add <file> | remove <name> | rebuild | clear | reload");
}

/* ---- subcommands ---- */

/** `certs info` — trust store status. */
static void certs_cmd_info(void)
{
    certs_store_info_t info;

    certs_get_store_info(&info);

    shell_transcript_appendf_ansi(SH_HEAD "TLS Trust Store" SH_RST "\n");
    shell_transcript_appendf_ansi("  " SH_LBL "SD available:" SH_RST " %s\n",
                                  info.sd_available ? SH_OK "yes" SH_RST : SH_MUTE "no" SH_RST);
    shell_transcript_appendf_ansi("  " SH_LBL "global CA store:" SH_RST " %s\n",
                                  info.global_store_active ? SH_OK "active" SH_RST : SH_MUTE "inactive" SH_RST);
    shell_transcript_appendf_ansi("  " SH_LBL "PEM files:" SH_RST " " SH_NUM "%d" SH_RST "\n",
                                  info.pem_files);
    shell_transcript_appendf_ansi("  " SH_LBL "PEM buffer:" SH_RST " " SH_NUM "%d bytes" SH_RST "\n",
                                  (int)info.pem_bytes);
    shell_transcript_appendf_ansi("  " SH_LBL "DER files:" SH_RST " " SH_NUM "%d" SH_RST "\n",
                                  info.der_files);
    if (info.pem_files == 0 && info.der_files == 0) {
        shell_transcript_appendf_ansi(SH_MUTE "  No certificates installed. Use `certs add <file>` to import CAs.\n" SH_RST);
    }
}

/** `certs list` — list certificate files in sd:/CERTS. */
static void certs_cmd_list(void)
{
    DIR *dir;
    struct dirent *entry;
    shell_sd_session_t session;
    esp_err_t error;
    int count = 0;

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        shell_transcript_appendf_ansi(SH_ERR "certs:" SH_RST " SD not available\n");
        return;
    }

    dir = opendir("/sdcard/" P4_CONFIG_TLS_CERTS_DIR);
    if (dir == NULL) {
        shell_transcript_appendf_ansi(SH_MUTE "certs:" SH_RST " sd:/" P4_CONFIG_TLS_CERTS_DIR " directory not found\n");
        shell_sd_end(&session, "certs list");
        return;
    }

    shell_transcript_appendf_ansi(SH_HEAD "sd:/" P4_CONFIG_TLS_CERTS_DIR "/:" SH_RST "\n");
    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;

        if (entry->d_type == DT_DIR) {
            continue;
        }
        if (shell_path_has_extension(name, ".pem") || shell_path_has_extension(name, ".der")) {
            shell_transcript_appendf_ansi("  " SH_VAL "%s" SH_RST "\n", name);
            count++;
        }
    }
    closedir(dir);

    if (count == 0) {
        shell_transcript_appendf_ansi(SH_MUTE "  (no certificates)\n" SH_RST);
    } else {
        shell_transcript_appendf_ansi(SH_MUTE "  %d certificate file(s)\n" SH_RST, count);
    }
    shell_sd_end(&session, "certs list");
}

/** Reload the store after add/remove and report the result in one line. */
static void certs_cmd_reload_report(const char *verb)
{
    certs_store_info_t info;
    esp_err_t error = certs_reload();

    if (error == ESP_OK) {
        certs_get_store_info(&info);
        shell_transcript_appendf_ansi(SH_OK "certs:" SH_RST " %s applied, store reloaded ("
                                      SH_NUM "%d" SH_RST " PEM + " SH_NUM "%d" SH_RST " DER, "
                                      SH_NUM "%d bytes" SH_RST ")\n",
                                      verb, info.pem_files, info.der_files,
                                      (int)info.pem_bytes);
    } else if (error == ESP_ERR_NOT_FOUND) {
        shell_transcript_appendf_ansi(SH_WARN "certs:" SH_RST " %s applied, no certificates remain\n",
                                      verb);
    } else {
        shell_transcript_appendf_ansi(SH_ERR "certs:" SH_RST " %s applied, reload failed (%s)\n",
                                      verb, esp_err_to_name(error));
    }
}

/** `certs add <file>` — import a PEM or DER file into sd:/CERTS. */
static void certs_cmd_add(const char *src_path)
{
    shell_sd_session_t session;
    esp_err_t error;
    const char *basename;
    char dest_path[128];
    FILE *fp_src;
    FILE *fp_dst;

    /* Resolve and validate the source path. */
    if (src_path == NULL || src_path[0] == '\0') {
        shell_print_error("certs: provide a source file path (PEM or DER)");
        return;
    }

    /* Extract just the filename from the path. */
    basename = strrchr(src_path, '/');
    basename = basename ? basename + 1 : src_path;

    if (!shell_path_has_extension(basename, ".pem") && !shell_path_has_extension(basename, ".der")) {
        shell_print_error("certs: file must have .pem or .der extension");
        return;
    }

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        shell_print_error("certs: SD not available");
        return;
    }

    fp_src = fopen(src_path, "rb");
    if (fp_src == NULL) {
        /* Try resolving against the SD root. */
        {
            char resolved[128];

            snprintf(resolved, sizeof(resolved), "/sdcard/%s", src_path);
            fp_src = fopen(resolved, "rb");
        }
        if (fp_src == NULL) {
            shell_print_error("certs: cannot open source file %s", src_path);
            shell_sd_end(&session, "certs add src");
            return;
        }
    }

    snprintf(dest_path, sizeof(dest_path), "/sdcard/%s/%s",
             P4_CONFIG_TLS_CERTS_DIR, basename);
    fp_dst = fopen(dest_path, "wb");
    if (fp_dst == NULL) {
        fclose(fp_src);
        shell_print_error("certs: cannot create destination %s", dest_path);
        shell_sd_end(&session, "certs add dst");
        return;
    }

    /* Copy the file, then reload while the session is closed (the reload
     * opens its own sessions). */
    {
        uint8_t buf[4096];
        size_t nread;
        size_t total = 0;
        bool copy_ok = true;

        while ((nread = fread(buf, 1, sizeof(buf), fp_src)) > 0) {
            if (fwrite(buf, 1, nread, fp_dst) != nread) {
                copy_ok = false;
                break;
            }
            total += nread;
        }
        fclose(fp_src);
        fclose(fp_dst);

        if (!copy_ok) {
            shell_print_error("certs: failed while copying %s", basename);
            shell_sd_end(&session, "certs add");
            return;
        }
        shell_transcript_appendf_ansi(SH_OK "certs:" SH_RST " imported " SH_VAL "%s" SH_RST
                                      " (" SH_NUM "%d bytes" SH_RST ") into sd:/" P4_CONFIG_TLS_CERTS_DIR "\n",
                                      basename, (int)total);
    }

    shell_sd_end(&session, "certs add");
    certs_cmd_reload_report("import");
}

/** `certs remove <name>` — remove a certificate from sd:/CERTS. */
static void certs_cmd_remove(const char *name)
{
    shell_sd_session_t session;
    esp_err_t error;
    char path[128];

    if (name == NULL || name[0] == '\0') {
        shell_print_error("certs: provide a filename to remove");
        return;
    }

    /* Refuse to remove the combined bundle directly (rebuild regenerates it). */
    {
        const char *bundle = P4_CONFIG_TLS_CERTS_COMBINED_PEM;
        const char *slash = strrchr(bundle, '/');
        const char *base = (slash != NULL) ? slash + 1 : bundle;

        if (certs_name_equals_ci(name, base)) {
            shell_print_error("certs: refusing to remove the combined bundle (rebuild regenerates it)");
            return;
        }
    }

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        shell_print_error("certs: SD not available");
        return;
    }

    snprintf(path, sizeof(path), "/sdcard/%s/%s", P4_CONFIG_TLS_CERTS_DIR, name);
    if (remove(path) == 0) {
        shell_transcript_appendf_ansi(SH_OK "certs:" SH_RST " removed " SH_VAL "%s" SH_RST "\n", name);
        shell_sd_end(&session, "certs remove");
        certs_cmd_reload_report("removal");
    } else {
        shell_transcript_appendf_ansi(SH_ERR "certs:" SH_RST " cannot remove " SH_VAL "%s" SH_RST " (not found?)\n", name);
        shell_sd_end(&session, "certs remove");
    }
}

/** `certs rebuild` — concatenate all .pem files into BUNDLE.PEM. */
static void certs_cmd_rebuild(void)
{
    esp_err_t error = certs_rebuild_bundle();

    if (error == ESP_OK) {
        shell_transcript_appendf_ansi(SH_OK "certs:" SH_RST " bundle rebuilt\n");
    } else if (error == ESP_ERR_NOT_FOUND) {
        shell_transcript_appendf_ansi(SH_WARN "certs:" SH_RST " no .pem files to bundle\n");
    } else {
        shell_transcript_appendf_ansi(SH_ERR "certs:" SH_RST " rebuild failed (%s)\n",
                                      esp_err_to_name(error));
    }
}

/** `certs clear` — free the loaded CA store from memory. */
static void certs_cmd_clear(void)
{
    certs_clear();
    shell_transcript_appendf_ansi(SH_OK "certs:" SH_RST " global CA store cleared\n");
}

/** `certs reload` — re-scan sd:/CERTS and re-load the global CA store. */
static void certs_cmd_reload(void)
{
    certs_cmd_reload_report("reload");
}

/* ---- dispatcher ---- */

void shell_command_certs(int argc, char **argv)
{
    if (argc <= 1 || shell_text_equals_ignore_case(argv[1], "info")) {
        certs_cmd_info();
        return;
    }

    if (shell_text_equals_ignore_case(argv[1], "list")) {
        certs_cmd_list();
        return;
    }

    if (shell_text_equals_ignore_case(argv[1], "add") && argc >= 3) {
        certs_cmd_add(argv[2]);
        return;
    }

    if (shell_text_equals_ignore_case(argv[1], "remove") && argc >= 3) {
        certs_cmd_remove(argv[2]);
        return;
    }

    if (shell_text_equals_ignore_case(argv[1], "rebuild")) {
        certs_cmd_rebuild();
        return;
    }

    if (shell_text_equals_ignore_case(argv[1], "clear")) {
        certs_cmd_clear();
        return;
    }

    if (shell_text_equals_ignore_case(argv[1], "reload")) {
        certs_cmd_reload();
        return;
    }

    certs_print_usage();
}
