/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file image_commands.c
 * @brief `image` verb: BMP metadata and viewing for batch apps.
 *
 *   image info <file.bmp>            path, WxH, bit depth, orientation, bytes
 *   image show [/t:secs] <file.bmp>  the shared image viewer (same as `view`)
 *
 * All BMP decoding lives once in components/gfx; the viewer is the shared
 * modal surface (components/modal). This file is a thin command shell so
 * batch files get a scriptable, ERRORLEVEL-setting image entry point without
 * duplicating decode, scaling, or display logic. `view`/`open` route `.bmp`
 * here through the filetype registry.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shell.h"
#include "batch.h"
#include "command.h"
#include "storage.h"
#include "gfx.h"
#include "modal_surf.h"
#include "ansi_palette.h"
#include "p4minishell_config.h"
#include "esp_heap_caps.h"

int command_load_file_psram(const char *path_arg, const char *verb,
                            uint32_t max_bytes, uint8_t **out_buf,
                            size_t *out_size)
{
    char resolved[P4_CONFIG_SD_PATH_BYTES];
    shell_sd_session_t session;
    FILE *file;
    long size;
    uint8_t *buf;
    size_t got;

    if (out_buf == NULL || out_size == NULL || path_arg == NULL ||
        shell_fs_resolve_path(path_arg, resolved, sizeof(resolved)) != ESP_OK) {
        shell_print_error("%s: invalid path %s", verb, path_arg ? path_arg : "(null)");
        return 2;
    }
    *out_buf = NULL;
    *out_size = 0;
    if (shell_sd_begin(&session) != ESP_OK) {
        shell_print_error("%s: SD card not present", verb);
        return 1;
    }
    file = fopen(resolved, "rb");
    if (file == NULL) {
        shell_print_error("%s: cannot open %s", verb, resolved);
        shell_sd_end(&session, verb);
        return 1;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        shell_print_error("%s: cannot size %s", verb, resolved);
        fclose(file);
        shell_sd_end(&session, verb);
        return 1;
    }
    if (size < 54 || (uint64_t)size > max_bytes) {
        shell_print_error("%s: bad size %ld (max %u bytes)", verb, size,
                          (unsigned)max_bytes);
        fclose(file);
        shell_sd_end(&session, verb);
        return 1;
    }
    buf = heap_caps_malloc((size_t)size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        buf = malloc((size_t)size);
    }
    if (buf == NULL) {
        shell_print_error("%s: out of memory", verb);
        fclose(file);
        shell_sd_end(&session, verb);
        return 1;
    }
    got = fread(buf, 1, (size_t)size, file);
    fclose(file);
    shell_sd_end(&session, verb);
    if (got != (size_t)size) {
        shell_print_error("%s: short read %s", verb, resolved);
        heap_caps_free(buf);
        return 1;
    }
    *out_buf = buf;
    *out_size = (size_t)size;
    return 0;
}

static void image_usage(void)
{
    shell_print_usage("Usage: image info <file.bmp> | image show [/t:secs] <file.bmp>");
}

void shell_command_image(int argc, char **argv)
{
    const char *sub = (argc >= 2) ? argv[1] : NULL;

    if (sub == NULL) {
        image_usage();
        batch_set_errorlevel(2);
        return;
    }

    if (shell_text_equals_ignore_case(sub, "show") || shell_text_equals_ignore_case(sub, "view")) {
        const char *path = NULL;
        uint32_t timeout_ms = 0;
        char resolved[P4_CONFIG_SD_PATH_BYTES];

        for (int i = 2; i < argc; i++) {
            if (modal_parse_timeout_arg(argv[i], &timeout_ms)) {
                continue;
            } else if (path == NULL) {
                path = argv[i];
            }
        }
        if (path == NULL) {
            image_usage();
            batch_set_errorlevel(2);
            return;
        }
        if (shell_fs_resolve_path(path, resolved, sizeof(resolved)) != ESP_OK) {
            shell_print_error("image: invalid path %s", path);
            batch_set_errorlevel(2);
            return;
        }
        if (modal_image_run("Image", resolved, timeout_ms, P4_CONFIG_IMAGE_VIEWER_FIT != 0) != 0) {
            shell_print_error("image: cannot open %s", path);
            batch_set_errorlevel(1);
            return;
        }
        batch_set_errorlevel(0);
        return;
    }

    if (shell_text_equals_ignore_case(sub, "info")) {
        char resolved[P4_CONFIG_SD_PATH_BYTES];
        uint8_t *buf = NULL;
        size_t got = 0;
        gfx_bmp_info_t info;
        int rc;

        if (argc != 3) {
            image_usage();
            batch_set_errorlevel(2);
            return;
        }
        if (shell_fs_resolve_path(argv[2], resolved, sizeof(resolved)) != ESP_OK) {
            shell_print_error("image: invalid path %s", argv[2]);
            batch_set_errorlevel(2);
            return;
        }
        rc = command_load_file_psram(argv[2], "image info",
                                     P4_CONFIG_IMAGE_MAX_BYTES, &buf, &got);
        if (rc != 0) {
            batch_set_errorlevel(rc);
            return;
        }
        if (!gfx_bmp_parse_header_ex(buf, got, &info, GFX_IMAGE_MAX_W, GFX_IMAGE_MAX_H)) {
            shell_transcript_appendf_ansi(SH_ERR "image: not a 24/32-bit BI_RGB BMP (%s)\n" SH_RST,
                                          resolved);
            heap_caps_free(buf);
            batch_set_errorlevel(1);
            return;
        }
        heap_caps_free(buf);
        /* Machine-parsable single line (batch `for /f`-friendly). */
        shell_transcript_appendf("image: %s %dx%d %ubpp %s %u bytes\n",
                                 resolved, info.w, info.h, (unsigned)info.bpp,
                                 info.top_down ? "top-down" : "bottom-up",
                                 (unsigned)got);
        batch_set_errorlevel(0);
        return;
    }

    image_usage();
    batch_set_errorlevel(2);
}
