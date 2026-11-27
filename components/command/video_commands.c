/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file video_commands.c
 * @module command
 * @layer command
 * @domain media
 * @provides `video play|status|stop` over the one video core
 * @status experimental
 * @test test/main/test_video.c, tools/host_video_gate.py
 * @hard-rule ai-context.md#module-layering-rules
 * @brief Batch-facing facade for frame-sequence playback.
 *
 * This file is deliberately thin: it parses argv, registers the ONE present
 * callback (gfx canvas or the ONE TUI cell sampler through tui_draw_sprite),
 * and maps the core result onto ERRORLEVEL 0/1/2. No playlist, decoder,
 * scheduler or timing loop is duplicated here.
 */
#include "command.h"

#include "ansi_palette.h"
#include "batch.h"
#include "gfx.h"
#include "shell.h"
#include "tui.h"
#include "tui_frame.h"
#include "video.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

/** Present one decoded frame through the existing surfaces only. */
static esp_err_t video_command_present(const gfx_surface_t *frame, bool tui,
                                       void *ctx)
{
    char command[80];
    gfx_surface_t *canvas;

    (void)ctx;
    if (frame == NULL || frame->px == NULL || frame->w < 1 || frame->h < 1) {
        return ESP_ERR_INVALID_ARG;
    }
    if (tui) {
        int cols;
        int rows;
        int w;
        int h;

        if (!tui_is_active() && !tui_init()) {
            shell_print_error("video: cannot open the TUI surface");
            return ESP_FAIL;
        }
        cols = tui_get_cols();
        rows = tui_get_rows();
        w = (frame->w < cols) ? frame->w : cols;
        h = (frame->h < rows) ? frame->h : rows;
        if (w < 1 || h < 1) {
            return ESP_ERR_INVALID_STATE;
        }
        tui_draw_sprite((cols - w) / 2 + 1, (rows - h) / 2 + 1, w, h, frame);
        tui_flush();
        return ESP_OK;
    }

    if (!gfx_canvas_is_open()) {
        snprintf(command, sizeof(command), "gfx init %d %d", frame->w, frame->h);
        if (!shell_execute_command_core(command)) {
            shell_print_error("video: cannot open the gfx canvas");
            return ESP_FAIL;
        }
    }
    canvas = gfx_canvas_surface();
    if (canvas == NULL || canvas->px == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    gfx_surface_blit_scaled(canvas, frame, 0, 0, canvas->w, canvas->h,
                            false, 0);
    if (!shell_execute_command_core("gfx show")) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

int shell_command_video(int argc, char **argv)
{
    if (argc < 2) {
        shell_print_usage("Usage: video play <dir|manifest> [/fps:N] [/loop] [/tui] [/b] | video status [/b] | video stop");
        return 2;
    }

    if (shell_text_equals_ignore_case(argv[1], "play")) {
        video_options_t options;
        char source[VIDEO_PATH_MAX];
        esp_err_t err;

        if (!video_options_parse((const char *const *)(argv + 2), argc - 2,
                                 &options, source, sizeof(source))) {
            shell_print_usage("Usage: video play <dir|manifest> [/fps:N] [/loop] [/tui] [/b]");
            return 2;
        }
        video_set_present_callback(video_command_present, NULL);
        err = video_play(source, &options);
        return (err == ESP_OK) ? 0 : 1;
    }

    if (shell_text_equals_ignore_case(argv[1], "status")) {
        video_cursor_t cursor;
        char line[160];

        if (argc > 3 || (argc == 3 &&
                         !shell_text_equals_ignore_case(argv[2], "/b"))) {
            shell_print_usage("Usage: video status [/b]");
            return 2;
        }
        video_status(&cursor);
        video_cursor_format(&cursor, line, sizeof(line));
        if (argc == 3) {
            shell_transcript_appendf("%s\n", line);
        } else {
            shell_transcript_appendf_ansi(SH_LBL "video:" SH_RST " %s\n", line);
        }
        return 0;
    }

    if (shell_text_equals_ignore_case(argv[1], "stop")) {
        if (argc != 2) {
            shell_print_usage("Usage: video stop");
            return 2;
        }
        video_stop();
        shell_transcript_appendf_ansi(SH_LBL "video:" SH_RST " stop requested\n");
        return 0;
    }

    shell_print_usage("Usage: video play <dir|manifest> [/fps:N] [/loop] [/tui] [/b] | video status [/b] | video stop");
    return 2;
}
