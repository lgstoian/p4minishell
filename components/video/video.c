/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file video.c
 * @brief Firmware frame-sequence playback on the command worker's frame loop.
 *
 * Builds an ordered, bounded plan of SD image paths, then presents one frame
 * at a time through imagefile_decode_fit() (the ONE decoder) and the command
 * layer's registered present callback (gfx canvas or the ONE TUI cell sampler).
 * There is no playback task, no second playlist and no scheduler: the loop
 * runs synchronously on the caller, exactly like a batch `for` body, and the
 * cooperative stop flag is checked between frames.
 */

#include "video.h"

#include "applib_time.h"
#include "esp_heap_caps.h"
#include "ff.h"
#include "imagefile.h"
#include "p4heap.h"
#include "p4minishell_config.h"
#include "shell.h"
#include "storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#ifndef P4_CONFIG_VIDEO_GFX_MAX_W
#define P4_CONFIG_VIDEO_GFX_MAX_W 320
#endif
#ifndef P4_CONFIG_VIDEO_GFX_MAX_H
#define P4_CONFIG_VIDEO_GFX_MAX_H 240
#endif
#ifndef P4_CONFIG_VIDEO_TUI_COLS
#define P4_CONFIG_VIDEO_TUI_COLS 78
#endif
#ifndef P4_CONFIG_VIDEO_TUI_ROWS
#define P4_CONFIG_VIDEO_TUI_ROWS 22
#endif

#define VIDEO_TAG "video"

static video_cursor_t s_video_cursor;
static bool s_video_initialized;
static video_present_cb_t s_video_present;
static void *s_video_present_ctx;

void video_set_present_callback(video_present_cb_t callback, void *ctx)
{
    s_video_present = callback;
    s_video_present_ctx = ctx;
}

void video_stop(void)
{
    if (s_video_initialized) {
        video_cursor_stop(&s_video_cursor);
    }
}

void video_status(video_cursor_t *out)
{
    if (out != NULL) {
        if (s_video_initialized) {
            *out = s_video_cursor;
        } else {
            memset(out, 0, sizeof(*out));
            out->index = -1;
        }
    }
}

/** Add every supported image directly inside @p dir_vfs (no recursion). */
static void video_collect_dir(const char *dir_vfs, char **frames, int *count,
                              int capacity)
{
    char fatfs[P4_CONFIG_SD_PATH_BYTES];
    FF_DIR dir;
    FILINFO info;
    FRESULT result;

    if (shell_sd_vfs_to_fatfs_path(dir_vfs, fatfs, sizeof(fatfs)) != ESP_OK ||
        f_opendir(&dir, fatfs) != FR_OK) {
        return;
    }
    for (;;) {
        char path[VIDEO_PATH_MAX];
        int written;

        result = f_readdir(&dir, &info);
        if (result != FR_OK || info.fname[0] == '\0') {
            break;
        }
        if ((info.fattrib & AM_DIR) != 0) {
            continue;
        }
        if (!video_frame_supported(info.fname)) {
            continue;
        }
        written = snprintf(path, sizeof(path), "%s/%s", dir_vfs, info.fname);
        if (written < 0 || (size_t)written >= sizeof(path)) {
            continue;
        }
        {
            char *copy = strdup(path);

            if (copy == NULL || !video_frame_add(frames, count, capacity, copy)) {
                free(copy);
                break;
            }
        }
    }
    f_closedir(&dir);
}

/** Add each supported path line of a plain-text manifest. */
static void video_collect_manifest(const char *manifest_vfs, char **frames,
                                   int *count, int capacity)
{
    FILE *file = fopen(manifest_vfs, "r");
    char line[VIDEO_PATH_MAX + 16];
    char resolved[VIDEO_PATH_MAX];

    if (file == NULL) {
        return;
    }
    while (fgets(line, sizeof(line), file) != NULL) {
        char path[VIDEO_PATH_MAX];

        if (!video_manifest_line(line, path, sizeof(path))) {
            continue;
        }
        if (path[0] == '/' || strchr(path, ':') != NULL) {
            snprintf(resolved, sizeof(resolved), "%s", path);
        } else {
            const char *slash = strrchr(manifest_vfs, '/');
            if (slash == NULL) {
                snprintf(resolved, sizeof(resolved), "%s", path);
            } else {
                int n = snprintf(resolved, sizeof(resolved), "%.*s/%s",
                                 (int)(slash - manifest_vfs), manifest_vfs, path);
                if (n < 0 || (size_t)n >= sizeof(resolved)) {
                    continue;
                }
            }
        }
        {
            char *copy = strdup(resolved);

            if (copy == NULL || !video_frame_add(frames, count, capacity, copy)) {
                free(copy);
                break;
            }
        }
    }
    fclose(file);
}

/** Read one frame into a PSRAM buffer; caller frees with heap_caps_free(). */
static uint8_t *video_read_frame(const char *path, size_t *size_out)
{
    FILE *file;
    long length;
    uint8_t *buffer;

    file = fopen(path, "rb");
    if (file == NULL) {
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    length = ftell(file);
    if (length <= 0 || (uint64_t)length > P4_CONFIG_IMAGE_MAX_BYTES) {
        fclose(file);
        return NULL;
    }
    rewind(file);
    buffer = (uint8_t *)p4heap_alloc_psram((size_t)length);
    if (buffer == NULL || fread(buffer, 1, (size_t)length, file) != (size_t)length) {
        heap_caps_free(buffer);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *size_out = (size_t)length;
    return buffer;
}

esp_err_t video_play(const char *source, const video_options_t *options)
{
    shell_sd_session_t session;
    char resolved[VIDEO_PATH_MAX];
    char **frames = NULL;
    int count = 0;
    int capacity = P4_CONFIG_VIDEO_MAX_FRAMES;
    int i;
    esp_err_t result = ESP_OK;
    struct stat st;
    uint32_t interval;

    if (source == NULL || source[0] == '\0' || options == NULL ||
        options->fps < 1 || options->fps > VIDEO_MAX_FPS ||
        options->frame_budget < 1) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_video_initialized && s_video_cursor.playing) {
        return ESP_ERR_INVALID_STATE;
    }
    if (shell_sd_begin(&session) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    if (shell_fs_resolve_path(source, resolved, sizeof(resolved)) != ESP_OK ||
        resolved[0] == '\0') {
        shell_sd_end(&session, VIDEO_TAG);
        return ESP_ERR_NOT_FOUND;
    }
    frames = (char **)calloc((size_t)capacity, sizeof(frames[0]));
    if (frames == NULL) {
        shell_sd_end(&session, VIDEO_TAG);
        return ESP_ERR_NO_MEM;
    }

    if (stat(resolved, &st) == 0 && S_ISDIR(st.st_mode)) {
        video_collect_dir(resolved, frames, &count, capacity);
    } else {
        video_collect_manifest(resolved, frames, &count, capacity);
    }
    video_frames_sort(frames, count);
    if (count == 0) {
        result = ESP_ERR_NOT_FOUND;
        if (!options->bare) {
            shell_transcript_appendf("video: no supported frames in %s\n", resolved);
        }
        goto cleanup;
    }

    s_video_initialized = true;
    video_cursor_begin(&s_video_cursor, count, options->fps);
    interval = video_frame_interval_ms(options->fps);
    if (!options->bare) {
        shell_transcript_appendf("video: %d frame(s), %d fps, %s%s\n", count,
                                 options->fps, options->loop ? "loop " : "",
                                 options->tui ? "tui" : "gfx");
    }

    for (i = 0; i < options->frame_budget; i++) {
        uint8_t *bytes;
        size_t length = 0;
        gfx_surface_t surface;
        int fit_w = options->tui ? P4_CONFIG_VIDEO_TUI_COLS
                                 : P4_CONFIG_VIDEO_GFX_MAX_W;
        int fit_h = options->tui ? P4_CONFIG_VIDEO_TUI_ROWS
                                 : P4_CONFIG_VIDEO_GFX_MAX_H;

        if (!video_budget_ok(&s_video_cursor)) {
            break;
        }
        memset(&surface, 0, sizeof(surface));
        bytes = video_read_frame(frames[s_video_cursor.index], &length);
        if (bytes == NULL) {
            if (!options->bare) {
                shell_transcript_appendf("video: cannot read frame %s\n",
                                         frames[s_video_cursor.index]);
            }
            result = ESP_FAIL;
            break;
        }
        if (!imagefile_decode_fit(bytes, length, fit_w, fit_h, &surface)) {
            heap_caps_free(bytes);
            if (!options->bare) {
                shell_transcript_appendf("video: decode failed for %s\n",
                                         frames[s_video_cursor.index]);
            }
            result = ESP_FAIL;
            break;
        }
        heap_caps_free(bytes);
        if (s_video_present != NULL) {
            result = s_video_present(&surface, options->tui, s_video_present_ctx);
        }
        gfx_surface_free(&surface);
        if (result != ESP_OK) {
            break;
        }
        if (!options->bare) {
            shell_transcript_appendf("video: frame %d/%d %s\n",
                                     s_video_cursor.index + 1, count,
                                     frames[s_video_cursor.index]);
        }
        if (!video_cursor_next(&s_video_cursor, options->loop)) {
            result = ESP_OK;
            break;
        }
        if (s_video_cursor.stop_requested) {
            result = ESP_OK;
            break;
        }
        app_delay_ms(interval);
    }
    s_video_cursor.playing = false;

    {
        char status[160];

        video_cursor_format(&s_video_cursor, status, sizeof(status));
        shell_transcript_appendf("video: %s\n", status);
    }

cleanup:
    for (i = 0; i < count; i++) {
        free(frames[i]);
    }
    free(frames);
    shell_sd_end(&session, VIDEO_TAG);
    return result;
}
