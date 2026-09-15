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
        shell_sd_session_t session;
        FILE *file = NULL;
        long size = 0;
        uint8_t *buf = NULL;
        size_t got = 0;
        gfx_bmp_info_t info;

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
        if (shell_sd_begin(&session) != ESP_OK) {
            shell_print_error("image: SD card not present");
            batch_set_errorlevel(1);
            return;
        }
        file = fopen(resolved, "rb");
        if (file == NULL) {
            shell_print_error("image: cannot open %s", resolved);
            shell_sd_end(&session, "image info");
            batch_set_errorlevel(1);
            return;
        }
        if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0) {
            shell_print_error("image: cannot size %s", resolved);
            fclose(file);
            shell_sd_end(&session, "image info");
            batch_set_errorlevel(1);
            return;
        }
        if (size < 54 || (uint64_t)size > P4_CONFIG_IMAGE_MAX_BYTES) {
            shell_print_error("image: bad size %ld (max %u bytes)", size,
                              (unsigned)P4_CONFIG_IMAGE_MAX_BYTES);
            fclose(file);
            shell_sd_end(&session, "image info");
            batch_set_errorlevel(1);
            return;
        }
        buf = heap_caps_malloc((size_t)size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (buf == NULL) buf = malloc((size_t)size);
        if (buf == NULL) {
            shell_print_error("image: out of memory");
            fclose(file);
            shell_sd_end(&session, "image info");
            batch_set_errorlevel(1);
            return;
        }
        got = fread(buf, 1, (size_t)size, file);
        fclose(file);
        shell_sd_end(&session, "image info");
        if (got != (size_t)size) {
            shell_print_error("image: short read %s", resolved);
            heap_caps_free(buf);
            batch_set_errorlevel(1);
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
                                 (unsigned)size);
        batch_set_errorlevel(0);
        return;
    }

    image_usage();
    batch_set_errorlevel(2);
}
