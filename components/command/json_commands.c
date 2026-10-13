/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file json_commands.c
 * @brief JSON verbs (`json validate|pretty <file>`).
 *
 * The parser itself now lives in components/json so that library components
 * (the RSS reader for JSON Feed) share one implementation instead of keeping
 * a private copy. This file is only the command surface: resolve the path,
 * read it within the viewer budget, and call the shared reader.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shell.h"
#include "batch.h"
#include "storage.h"
#include "ansi_palette.h"
#include "command.h"
#include "p4minishell_config.h"
#include "json.h"
#include "esp_err.h"

#define SHELL_COMMAND_BYTES             P4_CONFIG_COMMAND_BYTES

/* Viewer-sized budget shared with the md verb. */
#define JSON_FILE_MAX_BYTES   (64 * 1024)

static void shell_command_json_usage(void)
{
    shell_transcript_appendf_ansi("Usage: json validate <file> | json pretty <file>\n");
    shell_transcript_appendf_ansi("  validate  - check structure, report line/col on error\n");
    shell_transcript_appendf_ansi("  pretty    - validate and print 2-space indented JSON\n");
}

void shell_command_json(int argc, char **argv)
{
    bool pretty;
    char resolved[P4_CONFIG_SD_PATH_BYTES];
    FILE *file = NULL;
    char *input = NULL;
    char *output = NULL;
    size_t got = 0;
    char err[128];

    if (argc != 3) {
        shell_command_json_usage();
        batch_set_errorlevel(2);
        return;
    }
    if (shell_text_equals_ignore_case(argv[1], "validate")) {
        pretty = false;
    } else if (shell_text_equals_ignore_case(argv[1], "pretty")) {
        pretty = true;
    } else {
        shell_command_json_usage();
        batch_set_errorlevel(2);
        return;
    }

    if (shell_fs_resolve_path(argv[2], resolved, sizeof(resolved)) != ESP_OK) {
        shell_print_error("json: invalid path %s", argv[2]);
        batch_set_errorlevel(2);
        return;
    }
    file = fopen(resolved, "rb");
    if (file == NULL) {
        shell_print_error("json: cannot open %s", argv[2]);
        batch_set_errorlevel(1);
        return;
    }
    /* Heap/PSRAM budgets (same class as the md verb). */
    input = malloc(JSON_FILE_MAX_BYTES + 1);
    output = malloc(JSON_FILE_MAX_BYTES * 2 + 1);
    if (input == NULL || output == NULL) {
        shell_transcript_appendf_ansi(SH_ERR "json: out of memory\n" SH_RST);
        batch_set_errorlevel(1);
        free(input);
        free(output);
        fclose(file);
        return;
    }
    got = fread(input, 1, JSON_FILE_MAX_BYTES, file);
    fclose(file);
    input[got] = '\0';

    if (pretty) {
        size_t n = json_pretty_text(input, got, output,
                                    JSON_FILE_MAX_BYTES * 2 + 1,
                                    err, sizeof(err));
        if (n == 0) {
            shell_transcript_appendf_ansi(SH_ERR "json: invalid: %s\n" SH_RST, err);
            batch_set_errorlevel(1);
        } else {
            shell_transcript_appendf("%s", output);
            batch_set_errorlevel(0);
        }
    } else if (json_validate_text(input, got, err, sizeof(err))) {
        shell_transcript_appendf_ansi(SH_OK "json: valid (%u bytes)\n" SH_RST,
                                      (unsigned)got);
        batch_set_errorlevel(0);
    } else {
        shell_transcript_appendf_ansi(SH_ERR "json: invalid: %s\n" SH_RST, err);
        batch_set_errorlevel(1);
    }
    free(input);
    free(output);
}