/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file epub_commands.c
 * @brief `epub` verb family implementation.
 *
 * EPUB e-book reader commands:
 *   epub open <file>       — open an EPUB file
 *   epub close             — close current EPUB
 *   epub info              — show metadata (title, author, etc.)
 *   epub toc               — show table of contents
 *   epub read              — render current chapter
 *   epub next              — next chapter in spine
 *   epub prev              — previous chapter in spine
 *   epub goto <n>          — go to chapter n (spine index)
 *   epub toc <n>           — go to TOC entry n
 *   epub extract <n> <file> — extract resource (image, CSS) by manifest index
 */

#include "command.h"
#include "epub.h"
#include "shell.h"
#include "storage.h"
#include "strutil.h"
#include "p4minishell_config.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* Global EPUB handle for current session */
static epub_handle_t *s_epub_handle = NULL;

/* Forward declarations */
static esp_err_t epub_cmd_open(int argc, char *argv[]);
static esp_err_t epub_cmd_close(int argc, char *argv[]);
static esp_err_t epub_cmd_info(int argc, char *argv[]);
static esp_err_t epub_cmd_toc(int argc, char *argv[]);
static esp_err_t epub_cmd_read(int argc, char *argv[]);
static esp_err_t epub_cmd_next(int argc, char *argv[]);
static esp_err_t epub_cmd_prev(int argc, char *argv[]);
static esp_err_t epub_cmd_goto(int argc, char *argv[]);
static esp_err_t epub_cmd_toc_goto(int argc, char *argv[]);
static esp_err_t epub_cmd_extract(int argc, char *argv[]);

/* ---- epub open ---- */
static esp_err_t epub_cmd_open(int argc, char *argv[])
{
    if (argc < 2) {
        shell_print_error("epub: open <file>");
        return 2;
    }

    if (s_epub_handle) {
        epub_close(s_epub_handle);
        s_epub_handle = NULL;
    }

    char path[P4_CONFIG_SD_PATH_BYTES];
    if (shell_fs_resolve_path(argv[1], path, sizeof(path)) != ESP_OK) {
        return 2;
    }

    esp_err_t err = epub_open(path, &s_epub_handle);
    if (err != ESP_OK) {
        shell_print_error("epub: cannot open %s (@y%s@R)", argv[1], esp_err_to_name(err));
        return 1;
    }

    epub_metadata_t meta;
    epub_get_metadata(s_epub_handle, &meta);

    shell_print_muted("Opened: %s", argv[1]);
    if (meta.title[0]) shell_print_muted("Title: %s", meta.title);
    if (meta.author[0]) shell_print_muted("Author: %s", meta.author);
    if (meta.language[0]) shell_print_muted("Language: %s", meta.language);

    epub_chapter_t ch[16];
    int count;
    epub_get_spine(s_epub_handle, ch, 16, &count);
    shell_print_muted("Chapters: %d", count);

    return 0;
}

/* ---- epub close ---- */
static esp_err_t epub_cmd_close(int argc, char *argv[])
{
    (void)argc; (void)argv;

    if (!s_epub_handle) {
        shell_print_error("epub: no book open");
        return 1;
    }

    epub_close(s_epub_handle);
    s_epub_handle = NULL;
    shell_print_muted("epub: closed");
    return 0;
}

/* ---- epub info ---- */
static esp_err_t epub_cmd_info(int argc, char *argv[])
{
    (void)argc; (void)argv;

    if (!s_epub_handle) {
        shell_print_error("epub: no book open");
        return 1;
    }

    epub_metadata_t meta;
    epub_get_metadata(s_epub_handle, &meta);

    shell_print_muted("Title: %s", meta.title[0] ? meta.title : "(none)");
    shell_print_muted("Author: %s", meta.author[0] ? meta.author : "(none)");
    shell_print_muted("Language: %s", meta.language[0] ? meta.language : "(none)");
    shell_print_muted("Identifier: %s", meta.identifier[0] ? meta.identifier : "(none)");
    shell_print_muted("Publisher: %s", meta.publisher[0] ? meta.publisher : "(none)");
    shell_print_muted("Date: %s", meta.date[0] ? meta.date : "(none)");
    if (meta.description[0]) shell_print_muted("Description: %s", meta.description);

    return 0;
}

/* ---- epub toc ---- */
static esp_err_t epub_cmd_toc(int argc, char *argv[])
{
    (void)argc; (void)argv;

    if (!s_epub_handle) {
        shell_print_error("epub: no book open");
        return 1;
    }

    epub_toc_entry_t toc[64];
    int count;
    epub_get_toc(s_epub_handle, toc, 64, &count);

    if (count == 0) {
        shell_print_muted("epub: no table of contents");
        return 0;
    }

    for (int i = 0; i < count; i++) {
        shell_print_muted("%3d %*s%s @ %s",
                           i, toc[i].depth * 2, "",
                           toc[i].label[0] ? toc[i].label : "(untitled)",
                           toc[i].href[0] ? toc[i].href : "(no href)");
    }

    return 0;
}

/* ---- epub read ---- */
static esp_err_t epub_cmd_read(int argc, char *argv[])
{
    (void)argc; (void)argv;

    if (!s_epub_handle) {
        shell_print_error("epub: no book open");
        return 1;
    }

    epub_nav_t nav;
    epub_get_nav(s_epub_handle, &nav);
    if (!nav.current) {
        shell_print_error("epub: no chapter selected (use 'epub goto <n>')");
        return 1;
    }

    char *buf = malloc(P4_CONFIG_HTML_RENDER_MAX_BYTES);
    if (!buf) {
        shell_print_error("epub: out of memory");
        return 1;
    }

    size_t len;
    esp_err_t err = epub_render_chapter(s_epub_handle, buf, P4_CONFIG_HTML_RENDER_MAX_BYTES, &len);
    if (err != ESP_OK) {
        free(buf);
        shell_print_error("epub: render failed (@y%s@R)", esp_err_to_name(err));
        return 1;
    }

    shell_transcript_appendf_ansi("%s", buf);
    free(buf);
    return 0;
}

/* ---- epub next ---- */
static esp_err_t epub_cmd_next(int argc, char *argv[])
{
    (void)argc; (void)argv;

    if (!s_epub_handle) {
        shell_print_error("epub: no book open");
        return 1;
    }

    esp_err_t err = epub_next_chapter(s_epub_handle);
    if (err != ESP_OK) {
        shell_print_error("epub: no next chapter");
        return 1;
    }

    epub_nav_t nav;
    epub_get_nav(s_epub_handle, &nav);
    shell_print_muted("Chapter %d/%d: %s", nav.current_index + 1, nav.total_chapters, nav.current->title[0] ? nav.current->title : "(untitled)");

    return 0;
}

/* ---- epub prev ---- */
static esp_err_t epub_cmd_prev(int argc, char *argv[])
{
    (void)argc; (void)argv;

    if (!s_epub_handle) {
        shell_print_error("epub: no book open");
        return 1;
    }

    esp_err_t err = epub_prev_chapter(s_epub_handle);
    if (err != ESP_OK) {
        shell_print_error("epub: no previous chapter");
        return 1;
    }

    epub_nav_t nav;
    epub_get_nav(s_epub_handle, &nav);
    shell_print_muted("Chapter %d/%d: %s", nav.current_index + 1, nav.total_chapters, nav.current->title[0] ? nav.current->title : "(untitled)");

    return 0;
}

/* ---- epub goto ---- */
static esp_err_t epub_cmd_goto(int argc, char *argv[])
{
    if (!s_epub_handle) {
        shell_print_error("epub: no book open");
        return 1;
    }

    if (argc < 2) {
        shell_print_error("epub: goto <chapter_index>");
        return 2;
    }

    int index = atoi(argv[1]);
    if (index < 0) {
        shell_print_error("epub: invalid chapter index");
        return 2;
    }

    esp_err_t err = epub_set_chapter(s_epub_handle, index);
    if (err != ESP_OK) {
        shell_print_error("epub: invalid chapter index (%d max)", epub_get_spine_count(s_epub_handle) - 1);
        return 1;
    }

    epub_nav_t nav;
    epub_get_nav(s_epub_handle, &nav);
    shell_print_muted("Chapter %d/%d: %s", nav.current_index + 1, nav.total_chapters, nav.current->title[0] ? nav.current->title : "(untitled)");

    return 0;
}

/* ---- epub toc <n> ---- */
static esp_err_t epub_cmd_toc_goto(int argc, char *argv[])
{
    if (!s_epub_handle) {
        shell_print_error("epub: no book open");
        return 1;
    }

    if (argc < 2) {
        shell_print_error("epub: toc <index>");
        return 2;
    }

    int index = atoi(argv[1]);
    esp_err_t err = epub_goto_toc_entry(s_epub_handle, index);
    if (err != ESP_OK) {
        shell_print_error("epub: invalid TOC index");
        return 1;
    }

    epub_nav_t nav;
    epub_get_nav(s_epub_handle, &nav);
    shell_print_muted("Chapter %d/%d: %s", nav.current_index + 1, nav.total_chapters, nav.current->title[0] ? nav.current->title : "(untitled)");

    return 0;
}

/* ---- epub extract ---- */
static esp_err_t epub_cmd_extract(int argc, char *argv[])
{
    if (!s_epub_handle) {
        shell_print_error("epub: no book open");
        return 1;
    }

    if (argc < 3) {
        shell_print_error("epub: extract <manifest_index> <dest_file>");
        return 2;
    }

    int index = atoi(argv[1]);
    if (index < 0) {
        shell_print_error("epub: invalid manifest index");
        return 2;
    }

    /* TODO: implement manifest index lookup */
    shell_print_error("epub: extract not fully implemented");
    return 1;
}

/* ---- Command table ---- */
static const shell_command_entry_t s_epub_commands[] = {
    { "open",   epub_cmd_open,   "epub open <file> — open an EPUB file" },
    { "close",  epub_cmd_close,  "epub close — close current EPUB" },
    { "info",   epub_cmd_info,   "epub info — show book metadata" },
    { "toc",    epub_cmd_toc,    "epub toc — show table of contents" },
    { "read",   epub_cmd_read,   "epub read — render current chapter" },
    { "next",   epub_cmd_next,   "epub next — next chapter" },
    { "prev",   epub_cmd_prev,   "epub prev — previous chapter" },
    { "goto",   epub_cmd_goto,   "epub goto <n> — go to chapter n" },
    { "toc",    epub_cmd_toc_goto, "epub toc <n> — go to TOC entry n" },
    { "extract", epub_cmd_extract, "epub extract <n> <file> — extract resource" },
};

const shell_command_entry_t *epub_commands_get_table(size_t *out_count)
{
    if (out_count) *out_count = sizeof(s_epub_commands) / sizeof(s_epub_commands[0]);
    return s_epub_commands;
}

int shell_command_epub(int argc, char **argv)
{
    if (argc < 2) {
        shell_print_muted("epub open <file> | close | info | toc | read | next | prev | goto <n> | toc <n> | extract <n> <file>");
        return 2;
    }

    const shell_command_entry_t *table = epub_commands_get_table(NULL);
    size_t count = 0;
    epub_commands_get_table(&count);

    for (size_t i = 0; i < count; i++) {
        if (strcasecmp(argv[1], table[i].name) == 0) {
            int sub_argc = argc - 1;
            char **sub_argv = &argv[1];
            return table[i].handler(sub_argc, sub_argv);
        }
    }

    shell_print_error("epub: unknown sub-command '%s'", argv[1]);
    shell_print_muted("epub open <file> | close | info | toc | read | next | prev | goto <n> | toc <n> | extract <n> <file>");
    return 2;
}