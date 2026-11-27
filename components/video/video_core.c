/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file video_core.c
 * @brief Pure video plan / option / cursor core (no SD, decoder or display).
 *
 * Kept free of ESP-IDF drivers so the same translation unit is compiled into
 * the firmware, the Unity suite and the host gate. Everything that needs a
 * file, a pixel or a clock lives in video.c or the command layer.
 */

#include "video.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void video_options_default(video_options_t *out)
{
    if (out == NULL) {
        return;
    }
    out->fps = VIDEO_DEFAULT_FPS;
    out->loop = false;
    out->tui = false;
    out->bare = false;
    out->frame_budget = P4_CONFIG_VIDEO_FRAME_BUDGET;
}

bool video_parse_fps(const char *text, int *fps_out)
{
    int value = 0;
    const char *p = text;

    if (p == NULL || *p == '\0' || fps_out == NULL) {
        return false;
    }
    for (; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        value = value * 10 + (*p - '0');
        if (value > VIDEO_MAX_FPS) {
            return false;
        }
    }
    if (value < 1) {
        return false;
    }
    *fps_out = value;
    return true;
}

bool video_options_parse(const char *const *args, int argc,
                         video_options_t *options_out,
                         char *source_out, size_t source_size)
{
    int i;
    bool have_source = false;

    if (args == NULL || options_out == NULL || source_out == NULL ||
        source_size == 0 || argc < 1) {
        return false;
    }
    video_options_default(options_out);
    source_out[0] = '\0';

    for (i = 0; i < argc; i++) {
        const char *arg = args[i];

        if (arg == NULL || arg[0] == '\0') {
            return false;
        }
        if (arg[0] == '/') {
            if (strcasecmp(arg, "/loop") == 0) {
                options_out->loop = true;
            } else if (strcasecmp(arg, "/tui") == 0) {
                options_out->tui = true;
            } else if (strcasecmp(arg, "/b") == 0) {
                options_out->bare = true;
            } else if (strncasecmp(arg, "/fps:", 5) == 0 &&
                       !video_parse_fps(arg + 5, &options_out->fps)) {
                return false;
            } else if (strncasecmp(arg, "/fps:", 5) != 0) {
                return false;
            }
            continue;
        }
        if (have_source || strlen(arg) >= source_size) {
            return false;
        }
        snprintf(source_out, source_size, "%s", arg);
        have_source = true;
    }
    return have_source;
}

video_frame_kind_t video_frame_kind(const char *path)
{
    const char *dot;
    size_t len;

    if (path == NULL) {
        return VIDEO_FRAME_UNKNOWN;
    }
    dot = strrchr(path, '.');
    if (dot == NULL) {
        return VIDEO_FRAME_UNKNOWN;
    }
    len = strlen(dot);
    if (strcasecmp(dot, ".bmp") == 0 && len == 4) {
        return VIDEO_FRAME_BMP;
    }
    if ((strcasecmp(dot, ".png") == 0 || strcasecmp(dot, ".jpg") == 0 ||
         strcasecmp(dot, ".jpeg") == 0) && (len == 4 || len == 5)) {
        return (len == 4 && strcasecmp(dot, ".png") == 0)
                   ? VIDEO_FRAME_PNG : VIDEO_FRAME_JPEG;
    }
    return VIDEO_FRAME_UNKNOWN;
}

bool video_frame_supported(const char *path)
{
    return video_frame_kind(path) != VIDEO_FRAME_UNKNOWN;
}

bool video_frame_add(char **frames, int *count, int capacity, const char *path)
{
    int i;

    if (frames == NULL || count == NULL || path == NULL || *count < 0 ||
        *count >= capacity || !video_frame_supported(path) ||
        strlen(path) >= VIDEO_PATH_MAX) {
        return false;
    }
    for (i = 0; i < *count; i++) {
        if (frames[i] != NULL && strcmp(frames[i], path) == 0) {
            /* Not re-stored: the caller keeps ownership of its copy. */
            return false;
        }
    }
    frames[*count] = (char *)path;
    (*count)++;
    return true;
}

/** qsort comparator: case-insensitive path order, bytewise tie-break. */
static int video_frame_compare(const void *a, const void *b)
{
    const char *left = *(const char *const *)a;
    const char *right = *(const char *const *)b;
    int folded;

    if (left == NULL || right == NULL) {
        return (left == right) ? 0 : (left == NULL ? -1 : 1);
    }
    folded = strcasecmp(left, right);
    return (folded != 0) ? folded : strcmp(left, right);
}

void video_frames_sort(char **frames, int count)
{
    if (frames == NULL || count < 2) {
        return;
    }
    qsort(frames, (size_t)count, sizeof(frames[0]), video_frame_compare);
}

uint32_t video_frame_interval_ms(int fps)
{
    if (fps < 1) {
        fps = VIDEO_DEFAULT_FPS;
    }
    if (fps > VIDEO_MAX_FPS) {
        fps = VIDEO_MAX_FPS;
    }
    return (uint32_t)(1000 / fps);
}

bool video_budget_ok(const video_cursor_t *cursor)
{
    if (cursor == NULL || !cursor->playing || cursor->stop_requested ||
        cursor->count <= 0) {
        return false;
    }
    return cursor->frames_played < P4_CONFIG_VIDEO_FRAME_BUDGET;
}

void video_cursor_begin(video_cursor_t *cursor, int count, int fps)
{
    if (cursor == NULL) {
        return;
    }
    memset(cursor, 0, sizeof(*cursor));
    cursor->playing = (count > 0);
    cursor->index = (count > 0) ? 0 : -1;
    cursor->count = (count > 0) ? count : 0;
    cursor->fps = (fps >= 1 && fps <= VIDEO_MAX_FPS) ? fps : VIDEO_DEFAULT_FPS;
}

bool video_cursor_next(video_cursor_t *cursor, bool loop)
{
    if (!video_budget_ok(cursor)) {
        if (cursor != NULL) {
            cursor->playing = false;
        }
        return false;
    }
    cursor->frames_played++;
    if (cursor->index + 1 < cursor->count) {
        cursor->index++;
        return true;
    }
    if (loop) {
        cursor->index = 0;
        cursor->loops_done++;
        return true;
    }
    cursor->playing = false;
    return false;
}

void video_cursor_stop(video_cursor_t *cursor)
{
    if (cursor == NULL) {
        return;
    }
    cursor->stop_requested = true;
    cursor->playing = false;
}

void video_cursor_format(const video_cursor_t *cursor, char *out, size_t size)
{
    if (out == NULL || size == 0) {
        return;
    }
    if (cursor == NULL || !cursor->playing) {
        snprintf(out, size, "state=idle index=-1 frames=0 fps=0 played=0 loops=0");
        return;
    }
    snprintf(out, size,
             "state=%s index=%d frames=%d fps=%d played=%d loops=%d",
             cursor->stop_requested ? "stopping" : "playing",
             cursor->index, cursor->count, cursor->fps,
             cursor->frames_played, cursor->loops_done);
}

bool video_manifest_line(const char *line, char *path_out, size_t path_size)
{
    const char *p;
    const char *end;
    size_t len;

    if (line == NULL || path_out == NULL || path_size == 0) {
        return false;
    }
    p = line;
    while (*p != '\0' && isspace((unsigned char)*p)) {
        p++;
    }
    if (*p == '\0' || *p == '#' || *p == ';') {
        return false;
    }
    end = p + strlen(p);
    while (end > p && isspace((unsigned char)end[-1])) {
        end--;
    }
    len = (size_t)(end - p);
    if (len == 0 || len >= path_size || len >= VIDEO_PATH_MAX) {
        return false;
    }
    memcpy(path_out, p, len);
    path_out[len] = '\0';
    return video_frame_supported(path_out);
}
