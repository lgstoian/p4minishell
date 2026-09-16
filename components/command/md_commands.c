/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file md_commands.c
 * @brief Markdown verbs (`markdown <file> | -e <text> | on | off`).
 *
 * Document-mode rendering for files and inline text; per-line auto-render
 * for echo/type lives in batch.c/storage.c behind markdown_get_auto().
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shell.h"
#include "batch.h"
#include "markdown.h"
#include "storage.h"
#include "ansi_palette.h"
#include "command.h"
#include "p4minishell_config.h"
#include "esp_err.h"

#define SHELL_COMMAND_BYTES             P4_CONFIG_COMMAND_BYTES

/* File render budgets (heap/PSRAM; transcript truncates to 16 KB anyway). */
#define MD_FILE_MAX_BYTES   (64 * 1024)
#define MD_OUT_MAX_BYTES    (128 * 1024)

static void shell_command_markdown_usage(void)
{
    shell_transcript_appendf_ansi("Usage: markdown <file> | markdown -e <text> | markdown on | off\n");
    shell_transcript_appendf_ansi("  <file>  - render a Markdown file (fences, tables, lists)\n");
    shell_transcript_appendf_ansi("  -e      - render inline text\n");
    shell_transcript_appendf_ansi("  on|off  - auto-render echo/type output (default on; echo /raw bypasses)\n");
}

void shell_command_markdown(int argc, char **argv)
{
    if (argc < 2) {
        shell_command_markdown_usage();
        batch_set_errorlevel(2);
        return;
    }
    if (shell_text_equals_ignore_case(argv[1], "on")) {
        markdown_set_auto(true);
        shell_transcript_appendf_ansi(SH_OK "markdown auto-render on" SH_RST "\n");
        batch_set_errorlevel(0);
        return;
    }
    if (shell_text_equals_ignore_case(argv[1], "off")) {
        markdown_set_auto(false);
        shell_transcript_appendf_ansi(SH_OK "markdown auto-render off" SH_RST "\n");
        batch_set_errorlevel(0);
        return;
    }
    if (shell_text_equals_ignore_case(argv[1], "-e")) {
        char *joined = NULL;
        char *rendered = NULL;

        if (argc < 3) {
            shell_transcript_appendf_ansi(SH_ERR "markdown: usage: markdown -e <text>\n" SH_RST);
            batch_set_errorlevel(2);
            return;
        }
        joined = malloc(SHELL_COMMAND_BYTES);
        rendered = malloc(SHELL_COMMAND_BYTES * 2);
        if (joined == NULL || rendered == NULL) {
            shell_transcript_appendf_ansi(SH_ERR "markdown: out of memory\n" SH_RST);
            batch_set_errorlevel(1);
            free(joined);
            free(rendered);
            return;
        }
        shell_join_args(argv, 2, argc, joined, SHELL_COMMAND_BYTES);
        markdown_render_doc(joined, rendered, SHELL_COMMAND_BYTES * 2);
        shell_transcript_append_ansi(rendered);
        batch_set_errorlevel(0);
        free(joined);
        free(rendered);
        return;
    }
    /* Otherwise: a file path. */
    {
        char resolved[P4_CONFIG_SD_PATH_BYTES];
        FILE *file = NULL;
        char *input = NULL;
        char *rendered = NULL;
        size_t got = 0;

        if (shell_fs_resolve_path(argv[1], resolved, sizeof(resolved)) != ESP_OK) {
            shell_print_error("markdown: invalid path %s", argv[1]);
            batch_set_errorlevel(2);
            return;
        }
        file = fopen(resolved, "rb");
        if (file == NULL) {
            shell_print_error("markdown: cannot open %s", argv[1]);
            batch_set_errorlevel(1);
            return;
        }
        input = malloc(MD_FILE_MAX_BYTES + 1);
        rendered = malloc(MD_OUT_MAX_BYTES + 1);
        if (input == NULL || rendered == NULL) {
            shell_transcript_appendf_ansi(SH_ERR "markdown: out of memory\n" SH_RST);
            batch_set_errorlevel(1);
            free(input);
            free(rendered);
            fclose(file);
            return;
        }
        got = fread(input, 1, MD_FILE_MAX_BYTES, file);
        fclose(file);
        input[got] = '\0';
        markdown_render_doc(input, rendered, MD_OUT_MAX_BYTES + 1);
        shell_transcript_append_ansi(rendered);
        batch_set_errorlevel(0);
        free(input);
        free(rendered);
        return;
    }
}
