/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file host_tui_render.c
 * @brief Host-only scene driver for tools/host_tui_gate.py.
 *
 * Compiled with plain gcc against the SAME production sources the firmware
 * uses: components/tui/tui_frame.h (the one cell renderer and dirty-rect
 * diff), components/ansi/ansi.c (the one DOS palette and image ramp) and
 * components/markdown/css_color.c (the one CSS colour parser). It implements
 * no second renderer, sampler or colour parser: it only feeds scenes into the
 * public tui_frame_* API and prints a `tui dump /a`-style report.
 *
 * Never built into firmware. Scene files live next to this source.
 */

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tui_frame.h"
#include "ansi.h"
#include "css_color.h"

/* The two wrappers tui.c defines for the firmware. On the host they forward
 * to the SAME ansi_cga_* functions, so no palette is duplicated. */
uint32_t tui_dos_color_rgb(uint8_t index)
{
    return index > 15 ? 0 : ansi_cga_color(index);
}

uint8_t tui_rgb_to_dos(uint32_t rgb)
{
    return ansi_cga_nearest(rgb);
}

#define HOST_COLS 80
#define HOST_ROWS 25
#define LINE_MAX 1024

static tui_cell_t s_shown[HOST_ROWS][HOST_COLS];
static tui_cell_t s_next[HOST_ROWS][HOST_COLS];
static tui_frame_t s_frame;
static char s_scene[64] = "none";
static int s_cols = HOST_COLS;
static int s_rows = HOST_ROWS;

static char *skip_ws(char *p)
{
    while (*p != '\0' && isspace((unsigned char)*p)) p++;
    return p;
}

static char *next_token(char **cursor)
{
    char *start = skip_ws(*cursor);
    char *p;

    if (*start == '\0') {
        *cursor = start;
        return NULL;
    }
    p = start;
    while (*p != '\0' && !isspace((unsigned char)*p)) p++;
    if (*p != '\0') *p++ = '\0';
    *cursor = p;
    return start;
}

/** Parse a colour token using the ONE CSS parser + ONE DOS palette. */
static bool scene_color(const char *token, uint8_t *dos_out, uint32_t *rgb_out,
                        bool *rgb_mode)
{
    char buf[128];
    css_rgba_t css;

    if (token == NULL) return false;
    *rgb_mode = false;
    if (strncmp(token, "dos:", 4) == 0) {
        int v = atoi(token + 4);

        if (v < 0 || v > 16) return false;
        *dos_out = (uint8_t)v;
        return true;
    }
    if (strncmp(token, "idx:", 4) == 0) {
        int v = atoi(token + 4);

        if (v < 0 || v > 255) return false;
        *rgb_out = tui_256_rgb((uint8_t)v);
        *rgb_mode = true;
        return true;
    }
    if (strncmp(token, "rgb:", 4) == 0) {
        snprintf(buf, sizeof(buf), "%s", token + 4);
    } else if (strncmp(token, "css:", 4) == 0) {
        snprintf(buf, sizeof(buf), "%s", token + 4);
    } else {
        snprintf(buf, sizeof(buf), "%s", token);
    }
    if (css_color_parse(buf, &css)) {
        *rgb_out = ((uint32_t)css.r << 16) | ((uint32_t)css.g << 8) |
                   (uint32_t)css.b;
        *rgb_mode = true;
        return true;
    }
    return false;
}

/** A small deterministic RGB565 sprite; sampled by the ONE sampler. */
static void scene_sprite(int col, int row, int w, int h, bool checker)
{
    uint16_t px[64];
    gfx_surface_t img;

    for (int i = 0; i < 64; i++) {
        int sx = i % 8;
        int sy = i / 8;
        int r, g, b;

        if (checker) {
            bool white = ((sx / 2 + sy / 2) & 1) != 0;

            r = g = b = white ? 255 : 0;
            if (!white && (sx & 1)) r = 255;
        } else {
            r = g = b = sx * 255 / 7;
        }
        px[i] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
    img.px = px;
    img.w = 8;
    img.h = 8;
    tui_frame_draw_sprite(&s_frame, col, row, w, h, &img);
}

/** Print the normalised text row (same trimming rule as `tui dump`). */
static void dump_text_row(int row)
{
    char line[HOST_COLS * 4 + 1];
    size_t pos = 0;

    for (int c = 1; c <= s_cols; c++) {
        const tui_cell_t *cell =
            &s_frame.working[(row - 1) * s_cols + (c - 1)];
        const char *u = cell->utf8[0] ? cell->utf8 : " ";
        size_t len = strlen(u);

        if (pos + len + 1 >= sizeof(line)) break;
        memcpy(line + pos, u, len);
        pos += len;
    }
    while (pos > 0 && line[pos - 1] == ' ') pos--;
    line[pos] = '\0';
    printf("%03d|%s\n", row, line);
}

static void scene_rebind(void)
{
    memset(s_shown, 0, sizeof(s_shown));
    memset(s_next, 0, sizeof(s_next));
    tui_frame_bind(&s_frame, s_cols, s_rows, s_shown[0], s_next[0]);
    tui_frame_clear(&s_frame, 16, 16);
    /* Start from a clean presented frame so the diff measures only the scene. */
    tui_frame_commit(&s_frame);
}

static void scene_begin(const char *name)
{
    snprintf(s_scene, sizeof(s_scene), "%s", name);
    s_cols = HOST_COLS;
    s_rows = HOST_ROWS;
    scene_rebind();
    printf("tui.scene.begin name=%s cols=%d rows=%d\n", s_scene, s_cols, s_rows);
}

static char *next_token_lines(char **cursor);

static void scene_render(void)
{
    tui_dirty_t d = tui_frame_diff(&s_frame);
    char runs[HOST_COLS * 24 + 1];

    printf("tui.scene.dump name=%s\n", s_scene);
    for (int r = 1; r <= s_rows; r++) dump_text_row(r);
    tui_frame_render_runs(&s_frame, 1, s_rows, runs, sizeof(runs));
    {
        char *cursor = runs;
        char *line;

        while ((line = next_token_lines(&cursor)) != NULL) {
            printf("%s\n", line);
        }
    }
    printf("tui.scene.dirty any=%s rect=%d,%d-%d,%d\n",
           d.any ? "yes" : "no", d.min_col, d.min_row, d.max_col, d.max_row);
    tui_frame_commit(&s_frame);
    d = tui_frame_diff(&s_frame);
    printf("tui.scene.clean any=%s\n", d.any ? "yes" : "no");
    printf("tui.scene.end name=%s\n", s_scene);
}

/* next_token_lines(): split a newline-separated run buffer in place. */
static char *next_token_lines(char **cursor)
{
    char *start = *cursor;
    char *p;

    if (start == NULL || *start == '\0') return NULL;
    p = strchr(start, '\n');
    if (p != NULL) {
        *p = '\0';
        *cursor = p + 1;
    } else {
        *cursor = start + strlen(start);
    }
    return start;
}

int main(int argc, char **argv)
{
    FILE *fh;
    char line[LINE_MAX];

    if (argc < 2) {
        fprintf(stderr, "usage: host_tui_render <scene-file>...\n");
        return 2;
    }
    for (int a = 1; a < argc; a++) {
        fh = fopen(argv[a], "r");
        if (fh == NULL) {
            fprintf(stderr, "cannot open scene %s\n", argv[a]);
            return 2;
        }
        while (fgets(line, sizeof(line), fh) != NULL) {
            char *cursor = line;
            char *cmd;

            line[strcspn(line, "\r\n")] = '\0';
            if ((cmd = next_token(&cursor)) == NULL) continue;
            if (*cmd == '#' || *cmd == ';') continue;
            if (strcmp(cmd, "scene") == 0) {
                char *name = next_token(&cursor);

                if (name != NULL) scene_begin(name);
            } else if (strcmp(cmd, "cols") == 0) {
                int v = atoi(next_token(&cursor));

                if (v >= 1 && v <= HOST_COLS) {
                    s_cols = v;
                    scene_rebind();
                }
            } else if (strcmp(cmd, "rows") == 0) {
                int v = atoi(next_token(&cursor));

                if (v >= 1 && v <= HOST_ROWS) {
                    s_rows = v;
                    scene_rebind();
                }
            } else if (strcmp(cmd, "set") == 0) {
                char *cs = next_token(&cursor);
                char *rs = next_token(&cursor);
                char *color = next_token(&cursor);
                char *text = skip_ws(cursor);
                uint8_t dos = 16;
                uint32_t rgb = 0;
                bool rgb_mode = false;

                if (cs == NULL || rs == NULL || color == NULL || text == NULL) continue;
                if (scene_color(color, &dos, &rgb, &rgb_mode)) {
                    if (rgb_mode) {
                        tui_frame_set_rgb(&s_frame, atoi(cs), atoi(rs), text,
                                          rgb, 0, false, 0);
                    } else {
                        tui_frame_set(&s_frame, atoi(cs), atoi(rs), text,
                                      dos, 16, 0);
                    }
                }
            } else if (strcmp(cmd, "attr") == 0) {
                char *cs = next_token(&cursor);
                char *rs = next_token(&cursor);
                char *as = next_token(&cursor);
                tui_cell_t *cell;

                if (cs == NULL || rs == NULL || as == NULL) continue;
                cell = tui_frame_cell(&s_frame, atoi(cs), atoi(rs));
                if (cell != NULL) cell->attr = (uint8_t)atoi(as);
            } else if (strcmp(cmd, "sprite") == 0) {
                char *cs = next_token(&cursor);
                char *rs = next_token(&cursor);
                char *ws = next_token(&cursor);
                char *hs = next_token(&cursor);
                char *kind = next_token(&cursor);

                if (cs == NULL || rs == NULL || ws == NULL || hs == NULL) continue;
                scene_sprite(atoi(cs), atoi(rs), atoi(ws), atoi(hs),
                             kind == NULL || strcmp(kind, "ramp") != 0);
            } else if (strcmp(cmd, "render") == 0) {
                scene_render();
            }
        }
        fclose(fh);
    }
    return 0;
}
