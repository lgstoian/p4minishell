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

#include <stdbool.h>
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
    shell_transcript_appendf_ansi("Usage: markdown <file> | markdown -e <text> | markdown on | off | markdown export <src> <out> [text|html|print]\n");
    shell_transcript_appendf_ansi("  <file>  - render a Markdown file (fences, tables, lists)\n");
    shell_transcript_appendf_ansi("  -e      - render inline text\n");
    shell_transcript_appendf_ansi("  on|off  - auto-render echo/type output (default on; echo /raw bypasses)\n");
    shell_transcript_appendf_ansi("  export  - write <src> to <out> as plain text, HTML, or a paginated print layout (atomic)\n");
}

/** Basename of @p path without one trailing extension (export title). */
static void md_export_title(const char *path, char *out, size_t out_size)
{
    const char *base = path;
    const char *slash = strrchr(path, '/');
    const char *bslash = strrchr(path, '\\');
    char *dot;
    size_t n;

    if (bslash != NULL && (slash == NULL || bslash > slash)) {
        base = bslash + 1;
    } else if (slash != NULL) {
        base = slash + 1;
    }
    n = strlen(base);
    if (n >= out_size) {
        n = out_size - 1;
    }
    memcpy(out, base, n);
    out[n] = '\0';
    dot = strrchr(out, '.');
    if (dot != NULL && dot != out) {
        *dot = '\0';
    }
    if (out[0] == '\0') {
        n = strlen(base);
        if (n >= out_size) {
            n = out_size - 1;
        }
        memcpy(out, base, n);
        out[n] = '\0';
    }
}

/** Read up to @p cap bytes; sets *oversize when the file exceeds @p cap so a
 *  caller can refuse rather than silently truncate. */
static size_t md_read_capped(FILE *file, char *buf, size_t cap, bool *oversize)
{
    size_t got = fread(buf, 1, cap, file);

    *oversize = false;
    if (got == cap && fgetc(file) != EOF) {
        *oversize = true;
    }
    return got;
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
    if (shell_text_equals_ignore_case(argv[1], "export")) {
        const char *format = "text";
        int format_count = 0;
        char src_resolved[P4_CONFIG_SD_PATH_BYTES];
        char out_resolved[P4_CONFIG_SD_PATH_BYTES];
        char title[128];
        FILE *file = NULL;
        char *input = NULL;
        char *rendered = NULL;
        char *plain = NULL;
        const char *payload = NULL;
        bool need_plain;
        bool oversize = false;
        bool truncated = false;
        size_t got = 0;
        size_t need = 0;
        int i;

        if (argc < 4) {
            shell_transcript_appendf_ansi(SH_ERR "markdown export: usage: markdown export <src.md> <out> [text|html|print]\n" SH_RST);
            batch_set_errorlevel(2);
            return;
        }
        for (i = 4; i < argc; i++) {
            if (shell_text_equals_ignore_case(argv[i], "text") ||
                shell_text_equals_ignore_case(argv[i], "html") ||
                shell_text_equals_ignore_case(argv[i], "print")) {
                format = argv[i];
                format_count++;
            } else {
                shell_transcript_appendf_ansi(SH_ERR "markdown export: unknown format '%s' (text|html|print)\n" SH_RST, argv[i]);
                batch_set_errorlevel(2);
                return;
            }
        }
        if (format_count > 1) {
            shell_transcript_appendf_ansi(SH_ERR "markdown export: pick one format (text|html|print)\n" SH_RST);
            batch_set_errorlevel(2);
            return;
        }
        if (shell_fs_resolve_path(argv[2], src_resolved, sizeof(src_resolved)) != ESP_OK) {
            shell_print_error("markdown export: invalid source %s", argv[2]);
            batch_set_errorlevel(2);
            return;
        }
        if (shell_fs_resolve_path(argv[3], out_resolved, sizeof(out_resolved)) != ESP_OK) {
            shell_print_error("markdown export: invalid destination %s", argv[3]);
            batch_set_errorlevel(2);
            return;
        }
        file = fopen(src_resolved, "rb");
        if (file == NULL) {
            shell_print_error("markdown export: cannot open %s", argv[2]);
            batch_set_errorlevel(1);
            return;
        }
        need_plain = !shell_text_equals_ignore_case(format, "html");
        input = malloc(MD_FILE_MAX_BYTES + 1);
        rendered = malloc(P4_CONFIG_MD_EXPORT_MAX_BYTES + 1);
        plain = need_plain ? malloc(P4_CONFIG_MD_EXPORT_MAX_BYTES + 1) : NULL;
        if (input == NULL || rendered == NULL || (need_plain && plain == NULL)) {
            shell_transcript_appendf_ansi(SH_ERR "markdown export: out of memory\n" SH_RST);
            batch_set_errorlevel(1);
            free(input);
            free(rendered);
            free(plain);
            fclose(file);
            return;
        }
        got = md_read_capped(file, input, MD_FILE_MAX_BYTES, &oversize);
        fclose(file);
        input[got] = '\0';
        if (oversize) {
            shell_print_error("markdown export: %s is larger than %u KB - not exported",
                              argv[2], (unsigned)(MD_FILE_MAX_BYTES / 1024));
            batch_set_errorlevel(1);
            free(input);
            free(rendered);
            free(plain);
            return;
        }
        md_export_title(argv[2], title, sizeof(title));

        if (shell_text_equals_ignore_case(format, "html")) {
            need = markdown_render_html_page(input, title, rendered,
                                             P4_CONFIG_MD_EXPORT_MAX_BYTES + 1);
            truncated = (need >= P4_CONFIG_MD_EXPORT_MAX_BYTES + 1);
            payload = rendered;
        } else if (shell_text_equals_ignore_case(format, "print")) {
            /* Print-to-file: plain text laid out in fixed pages. */
            need = markdown_render_doc(input, rendered,
                                       P4_CONFIG_MD_EXPORT_MAX_BYTES + 1);
            truncated = (need >= P4_CONFIG_MD_EXPORT_MAX_BYTES + 1);
            if (!truncated) {
                markdown_strip_ansi(rendered, plain,
                                    P4_CONFIG_MD_EXPORT_MAX_BYTES + 1);
                need = markdown_render_print(plain, title, P4_CONFIG_PRINT_COLUMNS,
                                             P4_CONFIG_PRINT_ROWS, rendered,
                                             P4_CONFIG_MD_EXPORT_MAX_BYTES + 1);
                truncated = (need >= P4_CONFIG_MD_EXPORT_MAX_BYTES + 1);
            }
            payload = rendered;
        } else {
            /* Plain text: render then strip SGR (the viewer's own bridge). */
            need = markdown_render_doc(input, rendered,
                                       P4_CONFIG_MD_EXPORT_MAX_BYTES + 1);
            truncated = (need >= P4_CONFIG_MD_EXPORT_MAX_BYTES + 1);
            if (!truncated) {
                markdown_strip_ansi(rendered, plain,
                                    P4_CONFIG_MD_EXPORT_MAX_BYTES + 1);
            }
            payload = plain;
        }
        if (truncated) {
            shell_print_error("markdown export: %s expands past the %u KB export cap - not written",
                              argv[2], (unsigned)(P4_CONFIG_MD_EXPORT_MAX_BYTES / 1024));
            batch_set_errorlevel(1);
        } else if (storage_write_text_file(out_resolved, payload) != ESP_OK) {
            shell_print_error("markdown export: write failed for %s", argv[3]);
            batch_set_errorlevel(1);
        } else {
            shell_transcript_appendf_ansi(SH_LBL "markdown:" SH_RST " exported "
                                          SH_PATH "%s" SH_RST " -> " SH_PATH "%s" SH_RST
                                          " (%s)\n",
                                          argv[2], argv[3], format);
            batch_set_errorlevel(0);
        }
        free(input);
        free(rendered);
        free(plain);
        return;
    }
    /* Otherwise: a file path. */
    {
        char resolved[P4_CONFIG_SD_PATH_BYTES];
        FILE *file = NULL;
        char *input = NULL;
        char *rendered = NULL;
        bool oversize = false;
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
        got = md_read_capped(file, input, MD_FILE_MAX_BYTES, &oversize);
        fclose(file);
        input[got] = '\0';
        if (oversize) {
            shell_print_warning("markdown: %s is larger than %u KB; showing the first %u KB",
                                argv[1], (unsigned)(MD_FILE_MAX_BYTES / 1024),
                                (unsigned)(MD_FILE_MAX_BYTES / 1024));
        }
        markdown_render_doc(input, rendered, MD_OUT_MAX_BYTES + 1);
        shell_transcript_append_ansi(rendered);
        batch_set_errorlevel(0);
        free(input);
        free(rendered);
        return;
    }
}
