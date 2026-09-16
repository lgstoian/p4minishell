/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file header_commands.c
 * @brief `header` verb: layout mode, visibility, and status.
 *
 * The header component owns rendering and the responsive layout
 * (components/header/). This file owns the shell surface:
 *
 *   header [status]                     mode, height, visibility, font step
 *   header mode [auto|full|compact] [/save]
 *   header show|hide | on|off           visibility (rebuilds the layout)
 *
 * `/save` persists the mode to sd:/APPS/SHELL.INI (`header_mode=<name>`),
 * restored at the first SD mount next boot alongside the font/theme choice.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "shell.h"
#include "batch.h"
#include "header.h"
#include "display.h"
#include "storage.h"
#include "command.h"
#include "config_cmd.h"
#include "ansi_palette.h"
#include "p4minishell_config.h"
#include "bsp/esp-bsp.h"
#include "esp_err.h"

#define HEADER_INI_KEY "header_mode"

static void header_shell_ini_path(char *out, size_t out_size)
{
    snprintf(out, out_size, "%s/APPS/SHELL.INI", BSP_SD_MOUNT_POINT);
}

static bool header_save_mode(void)
{
    return config_persist_set("HEADER_MODE", header_mode_name(header_get_mode()));
}

/** Best-effort boot restore of the saved header mode. Silent without SD.
 * CONFIG.SYS is the store; SHELL.INI is read only as a legacy fallback. */
void header_restore_saved(void)
{
    char path[P4_CONFIG_SD_PATH_BYTES];
    char value[16];
    header_mode_t mode;

    header_shell_ini_path(path, sizeof(path));
    if (config_get_saved(HEADER_INI_KEY, value, sizeof(value)) < 0 &&
        storage_ini_file_get(path, HEADER_INI_KEY, value, sizeof(value)) != ESP_OK) {
        return;
    }
    if (header_mode_parse(value, &mode)) {
        header_set_mode(mode);
    }
}

static void header_usage(void)
{
    shell_transcript_appendf_ansi(
        "Usage: header [status] | header mode [auto|full|compact] [/save] | header show|hide\n");
}

void shell_command_header(int argc, char **argv)
{
    if (argc < 2 || shell_text_equals_ignore_case(argv[1], "status")) {
        header_metrics_t m;

        header_get_metrics(&m);
        shell_transcript_appendf("header.mode=%s\n", header_mode_name(header_get_mode()));
        shell_transcript_appendf("header.height=%d\n", header_get_height());
        shell_transcript_appendf("header.visible=%s\n",
                                 header_get_visible() ? "ON" : "OFF");
        shell_transcript_appendf("header.dynamic_font=%s\n",
                                 P4_CONFIG_HEADER_DYNAMIC_FONT ? "on" : "off");
        shell_transcript_appendf("header.status_style=%s\n",
                                 m.glyph_style ? "glyph" : "words");
        shell_transcript_appendf("header.levels=status:%d sys:%d font:%d center:%s\n",
                                 m.status_level, m.sys_level, m.font_step,
                                 m.show_center ? "on" : "off");
        shell_transcript_appendf("header.widths=screen:%d want:%d/%d/%d actual:%d/%d/%d\n",
                                 m.screen_w, m.left_w, m.center_w, m.right_w,
                                 m.actual_left, m.actual_center, m.actual_right);
        batch_set_errorlevel(0);
        return;
    }

    if (shell_text_equals_ignore_case(argv[1], "mode")) {
        header_mode_t mode;
        bool save = false;
        int i;

        for (i = 3; i < argc; i++) {
            if (shell_text_equals_ignore_case(argv[i], "/save")) {
                save = true;
            } else {
                shell_transcript_appendf_ansi(SH_ERR "header mode: unknown option '%s'\n" SH_RST, argv[i]);
                batch_set_errorlevel(2);
                return;
            }
        }
        if (argc < 3) {
            shell_transcript_appendf("header.mode=%s\n", header_mode_name(header_get_mode()));
            batch_set_errorlevel(0);
            return;
        }
        if (!header_mode_parse(argv[2], &mode)) {
            shell_transcript_appendf_ansi(SH_ERR "header mode: unknown mode '%s' (auto|full|compact)\n" SH_RST, argv[2]);
            batch_set_errorlevel(2);
            return;
        }
        header_set_mode(mode);
        if (save && !header_save_mode()) {
            shell_transcript_appendf_ansi(SH_ERR "header mode: applied, but could not save SHELL.INI\n" SH_RST);
            batch_set_errorlevel(1);
            return;
        }
        shell_transcript_appendf("header.mode=%s%s\n", header_mode_name(mode),
                                 save ? " (saved)" : "");
        batch_set_errorlevel(0);
        return;
    }

    if (shell_text_equals_ignore_case(argv[1], "show") ||
        shell_text_equals_ignore_case(argv[1], "on") ||
        shell_text_equals_ignore_case(argv[1], "hide") ||
        shell_text_equals_ignore_case(argv[1], "off")) {
        bool visible = shell_text_equals_ignore_case(argv[1], "show") ||
                       shell_text_equals_ignore_case(argv[1], "on");

        header_set_visible(visible);
        display_schedule_ui_rebuild();
        shell_transcript_appendf("header.visible=%s\n", visible ? "ON" : "OFF");
        batch_set_errorlevel(0);
        return;
    }

    header_usage();
    batch_set_errorlevel(2);
}
